#include "DublinFlightBenchmark.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "City/DublinCityWorld.h"
#include "DublinFlightPawn.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "InputKeyEventArgs.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "ProceduralMeshComponent.h"
#include "RHIGlobals.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Widgets/SWindow.h"

namespace DublinFlight::Performance
{
	namespace
	{
		constexpr double OrbitRadius = FFlightTuning::StartSpeedCmPerSecond
			/ (FFlightTuning::YawRateDegreesPerSecond * PI / 180.0);

		TSharedPtr<FJsonValue> Point(const FVector& P)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y), MakeShared<FJsonValueNumber>(P.Z)});
		}

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
			return Text;
		}

		bool Ready(const ADublinCityWorld* City)
		{
			return City && City->bCityReady && City->bDestructionReady && City->bAllBuildingsFractureReady
				&& !City->bAllowPartialBakePreview && City->BuildingCount == 1044
				&& City->FractureReadyBuildingCount == 1044 && City->GetSourceData() && City->FractureLibrary;
		}

		TSharedRef<FJsonObject> ReadWeapons(const UDublinWeaponComponent& W)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("bombYieldTonsTNT"), W.BombYieldTonsTNT);
			Out->SetNumberField(TEXT("cannonRoundsPerSecond"), W.CannonRoundsPerSecond);
			Out->SetNumberField(TEXT("bombCooldownSeconds"), W.BombCooldownSeconds);
			Out->SetNumberField(TEXT("maximumActiveProjectiles"), W.MaximumActiveProjectiles);
			Out->SetNumberField(TEXT("bombRadiusAtOneCm"), W.BombRadiusAtOneCm);
			Out->SetNumberField(TEXT("bombDepthAtOneCm"), W.BombDepthAtOneCm);
			Out->SetNumberField(TEXT("bombStrengthAtOne"), W.BombStrengthAtOne);
			Out->SetNumberField(TEXT("bombYieldExponent"), W.BombYieldExponent);
			Out->SetNumberField(TEXT("bombMaximumRadiusCm"), W.BombMaximumRadiusCm);
			Out->SetNumberField(TEXT("bombMaximumDepthCm"), W.BombMaximumDepthCm);
			Out->SetNumberField(TEXT("bombMaximumStrength"), W.BombMaximumStrength);
			return Out;
		}
	}

	bool ParseBenchmarkArguments(const TArray<FString>& Args, EBenchmark& Kind, FOptions& Options, FString& Error)
	{
		if (Args.IsEmpty() || Args.Num() > 3)
		{
			Error = TEXT("Usage: DublinFlight.Benchmark.Start normal|light|maximal [durationSeconds=30] [warmupSeconds=5]");
			return false;
		}
		const FString Name = Args[0].ToLower();
		if (Name == TEXT("normal")) { Kind = EBenchmark::Normal; }
		else if (Name == TEXT("light")) { Kind = EBenchmark::Light; }
		else if (Name == TEXT("maximal")) { Kind = EBenchmark::Maximal; }
		else { Error = TEXT("Benchmark must be normal, light, or maximal."); return false; }
		TArray<FString> ProfileArgs = Args;
		ProfileArgs[0] = Name;
		if (!ParseStartArguments(ProfileArgs, Options, Error)) { return false; }
		Options.ScenarioTargetMinimumFPS = Kind == EBenchmark::Maximal ? 30 : 60;
		return true;
	}

	bool LightBurstActive(double Elapsed, double Duration)
	{
		return Elapsed >= 0 && Elapsed < Duration && FMath::Fmod(Elapsed, 5.0) < 2.0;
	}

	int32 PlannedCannonShots(double Duration)
	{
		int32 Count = 0;
		for (double Burst = 0; Burst < Duration; Burst += 5)
		{
			for (int32 Shot = 0; Shot < 14; ++Shot)
			{
				Count += Burst + Shot / 7.0 < Duration ? 1 : 0;
			}
		}
		return Count;
	}

	bool BenchmarkPassed(bool Completed, bool Valid, bool Admitted, bool FrameTargetMet)
	{
		return Completed && Valid && Admitted && FrameTargetMet;
	}

	int32 FBenchmarkSlots::Take(double Elapsed, double Duration, double Period)
	{
		if (!FMath::IsFinite(Elapsed) || !FMath::IsFinite(Duration) || !FMath::IsFinite(Period)
			|| Elapsed < 0 || Elapsed >= Duration || Period <= 0 || Elapsed < Next * Period) { return INDEX_NONE; }
		if (LastAdmission >= 0 && Elapsed - LastAdmission < Period) { return INDEX_NONE; }
		const int32 Latest = FMath::FloorToInt(Elapsed / Period);
		Skipped += FMath::Max(0, Latest - Next);
		Next = Latest + 1;
		LastAdmission = Elapsed;
		return Latest;
	}

	FDublinRuntimeBenchmark::FDublinRuntimeBenchmark(EBenchmark InKind, const FOptions& InOptions)
		: Kind(InKind), Options(InOptions)
	{
		DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddRaw(this, &FDublinRuntimeBenchmark::OnFocusTransition);
		ActivateHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddRaw(this, &FDublinRuntimeBenchmark::OnFocusTransition);
	}

	FDublinRuntimeBenchmark::~FDublinRuntimeBenchmark()
	{
		Finish(TEXT("runnerDestroyed"));
		FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);
		FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(ActivateHandle);
	}

	void FDublinRuntimeBenchmark::OnFocusTransition()
	{
		if (bMeasured && !bFinished) { Invalidate(TEXT("applicationFocusTransitionDuringMeasurement")); }
	}

	void FDublinRuntimeBenchmark::Invalidate(const FString& Reason)
	{
		Failures.AddUnique(Reason);
	}

	bool FDublinRuntimeBenchmark::Prepare(UWorld& World, FString& Error)
	{
		ActiveWorld = &World;
		for (TActorIterator<ADublinCityWorld> It(&World); It; ++It)
		{
			if (City.IsValid()) { Error = TEXT("notReady: more than one city actor"); return false; }
			City = *It;
		}
		if (!Ready(City.Get()))
		{
			Error = TEXT("notReady: requires all 1044 saved fracture buildings, full city READY, partial preview OFF; no development fallback");
			return false;
		}
		if (!City->GetActorTransform().Equals(FTransform::Identity, 0.01)
			|| City->GetSourceData()->ExtentMeters != 768 || City->AcceptedImpactCount != 0
			|| City->QueuedImpactCount != 0 || City->FracturedBuildingCount != 0)
		{
			Error = TEXT("notReady: requires fresh authored 768m city at identity; restart map between benchmarks");
			return false;
		}
		APlayerController* Player = World.GetFirstPlayerController();
		ADublinFlightPawn* Original = Player ? Cast<ADublinFlightPawn>(Player->GetPawn()) : nullptr;
		if (!Player || !Player->IsLocalController() || !Player->PlayerInput || !Original || !Original->Weapons
			|| Original->Weapons->GetActiveProjectileCount() != 0 || !World.GetSubsystem<UDublinImpactEffectsSubsystem>())
		{
			Error = TEXT("notReady: requires local DublinFlightPawn, player input, empty projectile set and impact effects subsystem");
			return false;
		}

		double Altitude = 8000;
		for (const FDublinCityBuilding& Building : City->GetSourceData()->Buildings)
		{
			FBox Bounds(ForceInit);
			for (const FVector& V : Building.Mesh.VerticesCm) { Bounds += V + Building.PivotCm; }
			if (Bounds.ComputeSquaredDistanceToPoint(FVector(0, 0, Bounds.GetCenter().Z)) < FMath::Square(OrbitRadius + 2500))
			{
				Altitude = FMath::Max(Altitude, Bounds.Max.Z + 3000);
			}
		}
		RouteStart = FVector(0, -OrbitRadius, Altitude);
		if (Kind == EBenchmark::Maximal && !SelectStress(World, Error)) { return false; }
		PreviousPawn = Original;
		bPreviousHidden = Original->IsHidden();
		bPreviousCollision = Original->GetActorEnableCollision();
		bPreviousTick = Original->IsActorTickEnabled();
		Player->FlushPressedKeys();
		Original->Weapons->SuppressInput();
		Original->SetActorHiddenInGame(true);
		Original->SetActorEnableCollision(false);
		Original->SetActorTickEnabled(false);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Plane = World.SpawnActor<ADublinFlightPawn>(Original->GetClass(), RouteStart, FRotator::ZeroRotator, Params);
		if (!Plane.IsValid() || Plane->IsGodMode() || Plane->bResetBlocked)
		{
			Error = TEXT("notReady: safe benchmark flight spawn failed or is obstructed");
			return false;
		}
		Player->Possess(Plane.Get());
		Player->SetViewTarget(Plane.Get());
		if (Kind == EBenchmark::Maximal)
		{
			Plane->ToggleGodMode();
			Observer = World.SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FVector(0, -48000, 75000),
				FRotator(-57.38, 90, 0), Params);
			if (!Observer.IsValid()) { Error = TEXT("notReady: stress observer camera spawn failed"); return false; }
			Observer->GetCameraComponent()->SetFieldOfView(90);
			Player->SetViewTarget(Observer.Get());
		}
		else { Key(EKeys::E, true); }
		LastPosition = Plane->GetActorLocation();
		WeaponSettings = ReadWeapons(*Plane->Weapons);
		WeaponSettingsKey = JsonText(WeaponSettings.ToSharedRef());
		if (Plane->Weapons->BombYieldTonsTNT != 1 || Plane->Weapons->CannonRoundsPerSecond != 7)
		{
			Error = TEXT("notReady: benchmark requires native 1 TNT bomb and 7Hz cannon defaults; tuning was not changed");
			return false;
		}
		return true;
	}

	void FDublinRuntimeBenchmark::Key(const FKey& InKey, bool Pressed)
	{
		if (ActiveWorld.IsValid())
		{
			if (APlayerController* Player = ActiveWorld->GetFirstPlayerController())
			{
				Player->InputKey(FInputKeyEventArgs::CreateSimulated(InKey, Pressed ? IE_Pressed : IE_Released, Pressed ? 1.0f : 0.0f));
			}
		}
	}

	bool FDublinRuntimeBenchmark::SelectStress(UWorld& World, FString& Error)
	{
		const FDublinCityData& Source = *City->GetSourceData();
		TArray<FBox> Bounds;
		TArray<int32> Sorted;
		for (int32 I = 0; I < Source.Buildings.Num(); ++I)
		{
			FBox Box(ForceInit);
			for (const FVector& V : Source.Buildings[I].Mesh.VerticesCm) { Box += V + Source.Buildings[I].PivotCm; }
			Bounds.Add(Box);
			Sorted.Add(I);
		}
		Sorted.Sort([&Source](int32 A, int32 B) { return Source.Buildings[A].Id < Source.Buildings[B].Id; });
		const auto Safe = [](const FDublinImpact& Impact)
		{
			FString Why;
			return DublinDestruction::ValidateImpact(Impact, Why)
				&& FMath::Abs(Impact.PositionCm.X) + Impact.RadiusCm < 38400
				&& FMath::Abs(Impact.PositionCm.Y) + Impact.RadiusCm < 38400;
		};
		FDublinImpact Water = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1, DublinWeapons::FBombCurve());
		Water.bWater = true;
		Water.Seed = 58017;
		bool bWaterFound = false;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBenchmarkWater), false, World.GetFirstPlayerController()->GetPawn());
		for (int32 Y = -32000; Y <= 32000 && !bWaterFound; Y += 800)
		{
			for (int32 X = -32000; X <= 32000 && !bWaterFound; X += 800)
			{
				if (Bounds.ContainsByPredicate([X, Y](const FBox& B)
					{ return B.ComputeSquaredDistanceToPoint(FVector(X, Y, B.GetCenter().Z)) <= FMath::Square(1400.0); }))
				{ continue; }
				for (const FVector Direction : { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) })
				{
					bool bClear = true;
					for (int32 Step = -2; Step <= 8 && bClear; ++Step)
					{
						for (int32 Offset = -2; Offset <= 2; ++Offset)
						{
							FVector P = FVector(X, Y, 0) + Direction * (Step * 400) + FVector(-Direction.Y, Direction.X, 0) * (Offset * 400);
							float Z;
							FHitResult Hit;
							if (!DublinDestruction::SourceWaterZ(Source.Water, P, Z)) { bClear = false; break; }
							P.Z = Z;
							if (Bounds.ContainsByPredicate([&P](const FBox& B)
								{ return B.ComputeSquaredDistanceToPoint(FVector(P.X, P.Y, B.GetCenter().Z)) <= FMath::Square(200.0); })
								|| World.LineTraceSingleByChannel(Hit, P + FVector(0, 0, 10000), P + FVector(0, 0, 20), ECC_Visibility, Query))
							{ bClear = false; break; }
						}
					}
					float Z;
					if (bClear && DublinDestruction::SourceWaterZ(Source.Water, FVector(X, Y, 0), Z))
					{
						Water.PositionCm = FVector(X, Y, Z);
						bWaterFound = Safe(Water);
						if (bWaterFound) { break; }
					}
				}
			}
		}
		if (!bWaterFound) { Error = TEXT("notReady: no bridge-free safe stress water corridor"); return false; }
		TArray<FVector> Centers;
		for (int32 Index : Sorted)
		{
			const FBox& Box = Bounds[Index];
			const FDublinFractureRecord* Record = City->FractureLibrary->Find(Source.Buildings[Index].Id);
			if (Box.GetSize().Z < 500 || Box.GetSize().Z > 10000 || !Record || !Record->bReady
				|| Record->PieceCount < 2 || Record->PieceCount > 128 || Record->RootTransform == INDEX_NONE
				|| Record->LeafTransforms.Num() != Record->PieceCount || Record->Anchors.IsEmpty()
				|| Record->Collection.IsNull() || Record->SourceDigest != DublinDestruction::BuildingDigest(Source.Buildings[Index]))
			{ continue; }
			FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, DublinWeapons::FBombCurve());
			Impact.PositionCm = FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z + 100);
			Impact.Seed = 58100 + Centers.Num();
			if (!Safe(Impact) || Centers.ContainsByPredicate([&Impact](const FVector& P)
				{ return FVector::DistXY(P, Impact.PositionCm) < 5000; })) { continue; }
			Centers.Add(Impact.PositionCm);
			SelectedIds.Add(Source.Buildings[Index].Id);
			Sequence.Add(Impact);
			SequenceIds.Add(Source.Buildings[Index].Id);
			if (Centers.Num() % 3 == 0) { Sequence.Add(Water); SequenceIds.Add(TEXT("water_corridor")); }
			if (Centers.Num() == 24) { break; }
		}
		if (Centers.Num() < 8) { Error = TEXT("notReady: fewer than eight separated bounded stress neighborhoods"); return false; }
		return true;
	}

	void FDublinRuntimeBenchmark::ObserveEnvironment(UWorld& World, const TSharedRef<FJsonObject>& Settings)
	{
		UGameViewportClient* Client = World.GetGameViewport();
		FViewport* Viewport = Client ? Client->Viewport : nullptr;
		if (!Viewport || GUsingNullRHI || IsRunningDedicatedServer()
			|| World.GetNetMode() == NM_DedicatedServer || IsRunningCommandlet())
		{ Invalidate(TEXT("noActualRenderedViewportOrHeadless")); }
		if (Viewport)
		{
			if (Viewport->GetSizeXY() != FIntPoint(1920, 1080)) { Invalidate(TEXT("clientResolutionNot1920x1080")); }
			if (!Viewport->HasFocus() || !Viewport->IsForegroundWindow()) { Invalidate(TEXT("viewportNotForegroundOrFocusTransition")); }
			if (!Viewport->HasMouseCapture()) { Invalidate(TEXT("viewportMouseNotCaptured")); }
			const TSharedPtr<SWindow> Window = Client->GetWindow();
			if (!Window.IsValid() || !Window->GetNativeWindow().IsValid() || Window->GetNativeWindow()->IsMinimized())
			{ Invalidate(TEXT("windowUnavailableOrMinimized")); }
		}
		if (World.IsPaused() || !FMath::IsNearlyEqual(World.GetWorldSettings()->GetEffectiveTimeDilation(), 1.0f)
			|| (Plane.IsValid() && !FMath::IsNearlyEqual(Plane->CustomTimeDilation, 1.0f)))
		{ Invalidate(TEXT("pausedOrTimeDilated")); }
		if (FApp::UseFixedTimeStep() || (GEngine && GEngine->bUseFixedFrameRate))
		{ Invalidate(TEXT("syntheticOrFixedSimulationTime")); }
		if (Client && (!Client->EngineShowFlags.Rendering || !Client->EngineShowFlags.StaticMeshes))
		{ Invalidate(TEXT("renderingOrCityMeshesDisabled")); }
		const IConsoleVariable* DynamicResolution = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicRes.OperationMode"));
		if (DynamicResolution && DynamicResolution->GetInt() != 0) { Invalidate(TEXT("dynamicInternalResolutionNotVerifiable")); }
		if (!Ready(City.Get())) { Invalidate(TEXT("full1044ReadinessLostOrPartialPreview")); }
		if (!Plane.IsValid() || !World.GetFirstPlayerController() || World.GetFirstPlayerController()->GetPawn() != Plane.Get())
		{ Invalidate(TEXT("benchmarkPawnOrPossessionLost")); }
		if (JsonText(Settings) != BaselineSettings) { Invalidate(TEXT("environmentFocusQualityResolutionOrPacingChanged")); }
		if (Plane.IsValid())
		{
			if (Plane->bResetBlocked || (Kind != EBenchmark::Maximal && Plane->IsGodMode())
				|| !Plane->AircraftAirframe || !Plane->AircraftAirframe->IsVisible() || Plane->IsHidden())
			{ Invalidate(TEXT("flightOrAircraftRenderingUnavailable")); }
			if (World.GetFirstPlayerController() && World.GetFirstPlayerController()->GetViewTarget()
				!= (Kind == EBenchmark::Maximal ? static_cast<AActor*>(Observer.Get()) : static_cast<AActor*>(Plane.Get())))
			{ Invalidate(TEXT("benchmarkCameraChanged")); }
		}
	}

	void FDublinRuntimeBenchmark::Sample(UWorld& World)
	{
		if (!City.IsValid()) { Invalidate(TEXT("cityLost")); return; }
		MaxCollections = FMath::Max(MaxCollections, City->ActiveFractureCollections);
		MaxPieces = FMath::Max(MaxPieces, City->ActiveFracturePieces);
		MaxQueued = FMath::Max(MaxQueued, City->QueuedImpactCount);
		MaxQueuedBuildings = FMath::Max(MaxQueuedBuildings, City->QueuedBuildingHitCount);
		Fractured = City->FracturedBuildingCount;
		Moved = FMath::Max(Moved, City->MovedFragmentCount);
		CraterNodes = City->CraterChangedNodeCount;
		CityImpacts = City->AcceptedImpactCount;
		MaxWater = FMath::Max(MaxWater, City->WaterMaxDisplacementCm);
		int32 FX = 0;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			if (It->GetWorld() != &World || !It->IsActive() || !It->GetAsset()) { continue; }
			const FString Path = It->GetAsset()->GetPathName();
			FX += Path.StartsWith(TEXT("/Game/FX/NS_WaterImpact.")) || Path.StartsWith(TEXT("/Game/FX/NS_BombExplosion."))
				|| Path.StartsWith(TEXT("/Game/FX/NS_CannonImpact.")) ? 1 : 0;
		}
		MaxFX = FMath::Max(MaxFX, FX);
		if (City->bImpactQueueBlocked || MaxQueued > DublinDestruction::MaxQueuedImpacts
			|| MaxCollections > DublinDestruction::MaxActiveCollections || MaxPieces > DublinDestruction::MaxActivePieces
			|| MaxFX > DublinImpactFX::MaxSystems) { Invalidate(TEXT("nativeQueuePhysicsOrFXBudgetFailure")); }
		if (Plane.IsValid() && Plane->Weapons)
		{
			const UDublinWeaponComponent& W = *Plane->Weapons;
			CannonShots = W.CannonShots;
			BombShots = W.BombsDropped;
			BombImpacts = W.AcceptedBombImpacts;
			CannonImpacts = W.AcceptedCannonImpacts;
			ImpactCount = W.AcceptedImpacts;
			RejectedImpactCount = W.RejectedImpacts;
			if (Kind != EBenchmark::Maximal) { EffectsRequests = W.EffectsRequests; }
			MaxProjectiles = FMath::Max(MaxProjectiles, W.GetActiveProjectileCount());
			EndProjectiles = W.GetActiveProjectileCount();
			EndWeaponSettings = ReadWeapons(W);
			if (JsonText(EndWeaponSettings.ToSharedRef()) != WeaponSettingsKey) { Invalidate(TEXT("weaponSettingsChanged")); }
			if (Kind != EBenchmark::Maximal && bMeasured)
			{
				const FVector P = Plane->GetActorLocation();
				DistanceCm += FVector::Distance(LastPosition, P);
				if (LastElapsed > 0 && FVector::Distance(LastPosition, P) > 8000)
				{ Invalidate(TEXT("flightDiscontinuityOrReset")); }
				LastPosition = P;
				if (FMath::Abs(FVector2D(P.X, P.Y).Size() - OrbitRadius) > 2000 || FMath::Abs(P.Z - RouteStart.Z) > 200)
				{ Invalidate(TEXT("deterministicFlightRouteDeparted")); }
				if (RouteSamples.Num() <= FMath::FloorToInt(LastElapsed))
				{
					const TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
					Sample->SetNumberField(TEXT("elapsedWallSeconds"), LastElapsed);
					Sample->SetField(TEXT("positionCm"), Point(P));
					Sample->SetNumberField(TEXT("yawDegrees"), Plane->GetActorRotation().Yaw);
					RouteSamples.Add(MakeShared<FJsonValueObject>(Sample));
				}
			}
		}
	}

	void FDublinRuntimeBenchmark::Boundary(UWorld& World, double WallSeconds, const FCaptureWindow& Window,
		bool Ending, const TSharedRef<FJsonObject>& Settings)
	{
		if (bFinished) { return; }
		if (!bMeasured && Window.IsMeasurementBoundary() && !Ending)
		{
			bMeasured = true;
			MeasurementStart = WallSeconds;
			StartSettings = Settings;
			BaselineSettings = JsonText(Settings);
			if (Plane.IsValid()) { LastPosition = Plane->GetActorLocation(); }
			if (City.IsValid() && City->AcceptedImpactCount != 0) { Invalidate(TEXT("destructionDuringWarmup")); }
			if (Plane.IsValid() && Plane->Weapons && (Plane->Weapons->CannonShots || Plane->Weapons->BombsDropped))
			{ Invalidate(TEXT("weaponLaunchDuringWarmup")); }
		}
		if (!bMeasured) { return; }
		LastElapsed = WallSeconds - MeasurementStart;
		EndSettings = Settings;
		ObserveEnvironment(World, Settings);
		Sample(World);
		if (Ending || LastElapsed >= Options.DurationSeconds) { return; }
		if (Kind == EBenchmark::Light)
		{
			const bool Held = LightBurstActive(LastElapsed, Options.DurationSeconds);
			if (Held != bCannonHeld)
			{
				Key(EKeys::LeftMouseButton, Held);
				bCannonHeld = Held;
				++InputRequests;
				const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
				Event->SetStringField(TEXT("operation"), Held ? TEXT("nativeCannonHoldStart") : TEXT("nativeCannonHoldStop"));
				Event->SetNumberField(TEXT("actualSeconds"), LastElapsed);
				Event->SetNumberField(TEXT("cumulativeNativeShots"), CannonShots);
				if (Held)
				{
					const int32 BurstIndex = FMath::FloorToInt(LastElapsed / 5);
					BackpressureSkipped += FMath::Max(0, BurstIndex - LastBurstIndex - 1) * 14;
					LastBurstIndex = BurstIndex;
					BurstStartShots = CannonShots;
					++HeldBursts;
				}
				else { Event->SetNumberField(TEXT("burstNativeShots"), CannonShots - BurstStartShots); }
				Events.Add(MakeShared<FJsonValueObject>(Event));
			}
			for (const double Due : { 5.0, 20.0 })
			{
				bool& Sent = Due == 5.0 ? bBomb5 : bBomb20;
				if (Due < Options.DurationSeconds && LastElapsed >= Due && !Sent)
				{
					Sent = true;
					++Attempted;
					++InputRequests;
					if (LastElapsed - Due > 0.5) { ++BackpressureSkipped; Invalidate(TEXT("missedBombSchedule")); continue; }
					Key(EKeys::B, true);
					Key(EKeys::B, false);
					const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
					Event->SetStringField(TEXT("operation"), TEXT("nativeBombKeyPress"));
					Event->SetNumberField(TEXT("plannedSeconds"), Due);
					Event->SetNumberField(TEXT("actualSeconds"), LastElapsed);
					Events.Add(MakeShared<FJsonValueObject>(Event));
				}
			}
		}
		else if (Kind == EBenchmark::Maximal && City.IsValid())
		{
			const int32 Slot = Slots.Take(LastElapsed, Options.DurationSeconds, 0.5);
			if (Slot == INDEX_NONE) { return; }
			const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
			Event->SetNumberField(TEXT("slot"), Slot);
			Event->SetNumberField(TEXT("plannedSeconds"), Slot * 0.5);
			Event->SetNumberField(TEXT("actualSeconds"), LastElapsed);
			FDublinImpact Impact = Sequence[Slot % Sequence.Num()];
			Event->SetStringField(TEXT("selectedId"), SequenceIds[Slot % SequenceIds.Num()]);
			Impact.Seed += Slot;
			Event->SetField(TEXT("positionCm"), Point(Impact.PositionCm));
			Event->SetNumberField(TEXT("yieldTonsTNT"), Impact.YieldTonsTNT);
			Event->SetBoolField(TEXT("water"), Impact.bWater);
			APlayerController* Player = World.GetFirstPlayerController();
			FVector2D Pixel;
			const bool bProjected = Player && Player->ProjectWorldLocationToScreen(Impact.PositionCm, Pixel)
				&& Pixel.X >= 0 && Pixel.X < 1920 && Pixel.Y >= 0 && Pixel.Y < 1080;
			bool bVisible = bProjected && Player->PlayerCameraManager;
			if (bVisible)
			{
				FHitResult Hit;
				FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBenchmarkVisibility), false, Plane.Get());
				bVisible = !World.LineTraceSingleByChannel(Hit, Player->PlayerCameraManager->GetCameraLocation(),
					Impact.PositionCm, ECC_Visibility, Query) || FVector::Distance(Hit.ImpactPoint, Impact.PositionCm) <= Impact.RadiusCm;
			}
			Event->SetBoolField(TEXT("damageFootprintInViewAndUnoccluded"), bVisible);
			if (!bVisible) { Invalidate(TEXT("stressDamageNotVisible")); }
			if (City->QueuedImpactCount >= DublinDestruction::MaxQueuedImpacts / 4)
			{
				++BackpressureSkipped;
				Event->SetStringField(TEXT("result"), TEXT("skippedBackpressure"));
			}
			else
			{
				++Attempted;
				UDublinImpactEffectsSubsystem* FX = World.GetSubsystem<UDublinImpactEffectsSubsystem>();
				if (FX && City->ApplyImpact(Impact))
				{
					FX->EmitImpact(Impact);
					++Accepted;
					++EffectsRequests;
					Event->SetStringField(TEXT("result"), TEXT("accepted"));
				}
				else { ++Rejected; Event->SetStringField(TEXT("result"), TEXT("rejected")); }
			}
			Events.Add(MakeShared<FJsonValueObject>(Event));
			Sample(World);
		}
	}

	void FDublinRuntimeBenchmark::Finish(const TCHAR* Reason)
	{
		if (bFinished) { return; }
		bFinished = true;
		Completion = Reason;
		if (Completion != TEXT("durationReached")) { Invalidate(Completion == TEXT("manualStop") ? TEXT("invalidManualStop") : Completion); }
		if (!WorkloadAdmitted()) { Invalidate(TEXT("plannedWorkloadNotAdmittedOrObserved")); }
		if (!ActiveWorld.IsValid() || ActiveWorld->bIsTearingDown) { return; }
		if (!PreviousPawn.IsValid() && !Plane.IsValid() && !Observer.IsValid()) { return; }
		Key(EKeys::E, false);
		Key(EKeys::LeftMouseButton, false);
		if (Plane.IsValid() && Plane->Weapons) { Plane->Weapons->SuppressInput(); }
		if (APlayerController* Player = ActiveWorld->GetFirstPlayerController())
		{
			Player->FlushPressedKeys();
			if (PreviousPawn.IsValid())
			{
				PreviousPawn->SetActorHiddenInGame(bPreviousHidden);
				PreviousPawn->SetActorEnableCollision(bPreviousCollision);
				PreviousPawn->SetActorTickEnabled(bPreviousTick);
				Player->Possess(PreviousPawn.Get());
				Player->SetViewTarget(PreviousPawn.Get());
			}
		}
		// Cleanup happens after the final sampled boundary; no measured projectile or frame is removed.
		if (Plane.IsValid()) { Plane->Destroy(); }
		if (Observer.IsValid()) { Observer->Destroy(); }
	}

	bool FDublinRuntimeBenchmark::WorkloadAdmitted() const
	{
		if (!bMeasured || BackpressureSkipped || Slots.Skipped || Rejected || RejectedImpactCount) { return false; }
		if (Kind == EBenchmark::Maximal)
		{
			return Accepted == FMath::CeilToInt(Options.DurationSeconds * 2.0) && CityImpacts == Accepted
				&& Fractured > 0 && Moved > 0 && MaxFX > 0;
		}
		if (DistanceCm < FFlightTuning::StartSpeedCmPerSecond * Options.DurationSeconds * 0.9) { return false; }
		if (Kind == EBenchmark::Normal) { return CannonShots == 0 && BombShots == 0 && ImpactCount == 0 && CityImpacts == 0; }
		const int32 Bombs = (Options.DurationSeconds > 5 ? 1 : 0) + (Options.DurationSeconds > 20 ? 1 : 0);
		return CannonShots == PlannedCannonShots(Options.DurationSeconds) && BombShots == Bombs
			&& (Bombs == 0 || ImpactCount > 0) && EffectsRequests == ImpactCount && CityImpacts == ImpactCount;
	}

	TSharedRef<FJsonObject> FDublinRuntimeBenchmark::Report() const
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("scenario"), Options.Label);
		Out->SetStringField(TEXT("workload"), Kind == EBenchmark::Maximal ? TEXT("downstreamDestructionStress_notWeaponFlight") : TEXT("nativePawnFlight"));
		Out->SetStringField(TEXT("completionReason"), Completion);
		Out->SetBoolField(TEXT("benchmarkValid"), IsValid());
		Out->SetBoolField(TEXT("workloadAdmitted"), WorkloadAdmitted());
		Out->SetBoolField(TEXT("measurementStarted"), bMeasured);
		Out->SetNumberField(TEXT("measuredWallSeconds"), LastElapsed);
		Out->SetNumberField(TEXT("measurementStartSharedWallSeconds"), MeasurementStart);
		if (WeaponSettings) { Out->SetObjectField(TEXT("weaponSettingsAtStart"), WeaponSettings.ToSharedRef()); }
		if (EndWeaponSettings) { Out->SetObjectField(TEXT("weaponSettingsAtEnd"), EndWeaponSettings.ToSharedRef()); }
		Out->SetStringField(TEXT("schedule"), TEXT("Shared OnBeginFrame wall clock; no destructive warmup; 7Hz native cannon held first 2s of every 5s, bombs at 5/20s; maximal 0.5s absolute slots AND minimum 0.5s admission spacing, one per boundary, missed slots not replayed. Scheduling drift/under-admission fails rather than reducing the workload target."));
		Out->SetStringField(TEXT("flightRoute"), TEXT("New native pawn at (0,-radius,altitude), yaw0; hold E through native input, 40m/s and 25deg/s yaw, level clockwise orbit. No pose writes during measurement; original pawn restored after end."));
		Out->SetField(TEXT("routeStartCm"), Point(RouteStart));
		Out->SetNumberField(TEXT("routeRadiusCm"), OrbitRadius);
		Out->SetNumberField(TEXT("flightDistanceCm"), DistanceCm);
		Out->SetArrayField(TEXT("routeSamples"), RouteSamples);
		TArray<TSharedPtr<FJsonValue>> Errors, Ids, Planned;
		for (const FString& Failure : Failures) { Errors.Add(MakeShared<FJsonValueString>(Failure)); }
		for (const FString& Id : SelectedIds) { Ids.Add(MakeShared<FJsonValueString>(Id)); }
		for (int32 Index = 0; Index < Sequence.Num(); ++Index)
		{
			const FDublinImpact& Impact = Sequence[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("selectedId"), SequenceIds[Index]);
			Item->SetField(TEXT("positionCm"), Point(Impact.PositionCm));
			Item->SetNumberField(TEXT("yieldTonsTNT"), Impact.YieldTonsTNT);
			Item->SetNumberField(TEXT("radiusCm"), Impact.RadiusCm);
			Item->SetNumberField(TEXT("seed"), Impact.Seed);
			Item->SetBoolField(TEXT("water"), Impact.bWater);
			Planned.Add(MakeShared<FJsonValueObject>(Item));
		}
		Out->SetArrayField(TEXT("invalidReasons"), Errors);
		Out->SetArrayField(TEXT("selectedBuildingIds"), Ids);
		Out->SetArrayField(TEXT("stressSequence"), Planned);
		Out->SetArrayField(TEXT("events"), Events);
		Out->SetNumberField(TEXT("plannedOperations"), Kind == EBenchmark::Maximal ? FMath::CeilToInt(Options.DurationSeconds * 2)
			: Kind == EBenchmark::Light ? PlannedCannonShots(Options.DurationSeconds) + (Options.DurationSeconds > 5) + (Options.DurationSeconds > 20) : 0);
		Out->SetNumberField(TEXT("attemptedOperations"), Kind == EBenchmark::Light ? CannonShots + Attempted : Attempted);
		Out->SetNumberField(TEXT("acceptedOperations"), Kind == EBenchmark::Light ? CannonShots + BombShots : Accepted);
		Out->SetNumberField(TEXT("rejectedOperations"), Kind == EBenchmark::Light ? FMath::Max(0, Attempted - BombShots - BackpressureSkipped) : Rejected);
		Out->SetNumberField(TEXT("skippedOperations"), BackpressureSkipped + Slots.Skipped);
		Out->SetNumberField(TEXT("cannonShots"), CannonShots);
		Out->SetNumberField(TEXT("bombsDropped"), BombShots);
		Out->SetNumberField(TEXT("acceptedBombImpacts"), BombImpacts);
		Out->SetNumberField(TEXT("acceptedCannonImpacts"), CannonImpacts);
		Out->SetNumberField(TEXT("activeProjectilesAtCaptureEnd"), EndProjectiles);
		Out->SetNumberField(TEXT("cannonHoldBursts"), HeldBursts);
		Out->SetNumberField(TEXT("weaponInputRequests"), InputRequests);
		Out->SetNumberField(TEXT("unadmittedCannonRounds"), Kind == EBenchmark::Light ? FMath::Max(0, PlannedCannonShots(Options.DurationSeconds) - CannonShots) : 0);
		Out->SetStringField(TEXT("lightAdmissionMeaning"), TEXT("acceptedOperations counts real projectile launches, not hits. Cannon native failed Launch invocations are not exposed by public API; attemptedOperations is a lower bound (successful cannon launches plus bomb input attempts). Unadmitted expected cannon rounds are explicitly reported and invalidate the run."));
		Out->SetNumberField(TEXT("weaponAcceptedImpacts"), ImpactCount);
		Out->SetNumberField(TEXT("cityAcceptedImpacts"), CityImpacts);
		Out->SetNumberField(TEXT("weaponRejectedImpacts"), RejectedImpactCount);
		Out->SetNumberField(TEXT("effectsRequests"), EffectsRequests);
		Out->SetNumberField(TEXT("maxAwakeCollections"), MaxCollections);
		Out->SetNumberField(TEXT("maxAwakePieces"), MaxPieces);
		Out->SetNumberField(TEXT("maxQueuedImpacts"), MaxQueued);
		Out->SetNumberField(TEXT("maxQueuedBuildingHits"), MaxQueuedBuildings);
		Out->SetNumberField(TEXT("maxActiveImpactFX"), MaxFX);
		Out->SetNumberField(TEXT("maxActiveProjectiles"), MaxProjectiles);
		Out->SetNumberField(TEXT("fracturedBuildings"), Fractured);
		Out->SetNumberField(TEXT("maxMovedFragments"), Moved);
		Out->SetNumberField(TEXT("craterChangedNodes"), CraterNodes);
		Out->SetNumberField(TEXT("maxWaterDisplacementCm"), MaxWater);
		Out->SetStringField(TEXT("nativeCaps"), TEXT("24 awake collections;1536 awake pieces;64 queued impacts;16 FX;maximal gate queue<16; no budget changed"));
		Out->SetStringField(TEXT("sampling"), TEXT("Environment and maxima observed at every shared engine boundary, plus immediately after direct admissions. Not an intra-frame peak or display-present measurement."));
		Out->SetStringField(TEXT("cleanup"), TEXT("Inputs released and benchmark pawn/projectiles removed only after capture end; native accepted city queues/physics/FX not cleared."));
		if (StartSettings) { Out->SetObjectField(TEXT("measurementStartSettings"), StartSettings.ToSharedRef()); }
		if (EndSettings) { Out->SetObjectField(TEXT("measurementEndSettings"), EndSettings.ToSharedRef()); }
		return Out;
	}
}
