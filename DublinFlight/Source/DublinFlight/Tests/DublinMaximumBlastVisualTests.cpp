#include "City/DublinCityWorld.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "DublinFlightPawn.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImagePixelData.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "ImageWriteTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Weapons/DublinWeaponModel.h"

namespace DublinMaximumBlastVisual
{
	const TCHAR* SourceId = TEXT("osm/way/233804861");
	const double ShotTimes[] = {0.3, 1.0, 3.0, 6.0, 12.0, 16.0};
	const FIntPoint Resolution(1280, 720);
	bool bActive = false;

	void Point(FJsonObject& Json, const TCHAR* Name, const FVector& Value)
	{
		Json.SetArrayField(Name, {MakeShared<FJsonValueNumber>(Value.X),
			MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)});
	}

	struct FWrite
	{
		TFuture<bool> Result;
		TSharedPtr<FJsonObject> Frame;
		FString Path;
	};

	class FCapture final : public IAutomationLatentCommand
	{
	public:
		explicit FCapture(FAutomationTestBase& InTest) : Test(InTest)
		{
			Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
				TEXT("Screenshots"), TEXT("DemolitionDetail-20260918"),
				TEXT("maximum-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			Manifest->SetStringField(TEXT("test"), TEXT("DublinFlight.Destruction.PIE.MaximumBlastVisual"));
			Manifest->SetStringField(TEXT("source_id"), SourceId);
			Manifest->SetStringField(TEXT("route"), TEXT("Fixed source roof collision -> Weapon.DispatchImpact -> City deferred committed-damage outbox. No direct Effects.EmitImpact; not a key/projectile test."));
			Manifest->SetBoolField(TEXT("visual_inspection_required"), true);
			Manifest->SetBoolField(TEXT("performance_claim"), false);
			Manifest->SetBoolField(TEXT("fresh_process_cache_claim"), false);
			Manifest->SetStringField(TEXT("capture_policy"), TEXT("Exact owned 1280x720 PIE viewport after rendering: native RHI pixel readback on game thread, PNG writing asynchronous. Canvas HUD included, no Slate/window capture or global screenshot request. Selection does not depend on OS focus. Use actual readback world timestamps, not requested slots."));
			Manifest->SetStringField(TEXT("scheduled_time_origin"), TEXT("First game-thread observation of the committed city FX request, not initial damage acceptance."));
			Manifest->SetStringField(TEXT("particle_count_evidence"), TEXT("No GPU particle readback captured; active component and fixed allocations are not live particle counts."));
			Manifest->SetStringField(TEXT("art_policy"), TEXT("Mixed/legacy library is baseline only. Rerun after detailed city bake; a passing fixture does not certify visual quality."));
			Manifest->SetStringField(TEXT("aftermath_policy"), TEXT("16-second frame must have no expensive maximum transient. An independently budgeted sparse smoke tail may remain."));
		}

		virtual ~FCapture() override
		{
			Cleanup();
			bActive = false;
		}

		virtual bool Update() override
		{
			if (StartedWall == 0) { StartedWall = FPlatformTime::Seconds(); }
			PollWrites();
			if (bFinishing) { return Finalize(); }
			if (FPlatformTime::Seconds() - StartedWall > 85) { return Fail(TEXT("85-second capture watchdog; reserving five seconds for teardown.")); }
			if (bFailed) { return Finish(); }
			if (!bPrepared) { return Bootstrap(); }
			if (!World.IsValid() || World->bIsTearingDown || !City.IsValid() || !Player.IsValid()
				|| !Camera.IsValid() || !Pawn.IsValid() || !Pawn->Weapons) { return Fail(TEXT("Owned PIE/camera ended during capture.")); }
			if (City->bImpactQueueBlocked) { return Fail(TEXT("Native city queue blocked: ") + City->LastDestructionError); }
			PollReadback();
			if (bFailed) { return Finish(); }
			const double Now = World->GetTimeSeconds();
			if (!bDispatched)
			{
				if (Now - PreparedWorld < 1.0 || GFrameCounter < PreparedFrame + 3 || !HasSize()) { return false; }
				if (!bPrimed)
				{
					if (!Pending.IsValid() && GFrameCounter > LastServiceFrame) { RequestShot(-1); }
					return bFailed ? Finish() : false;
				}
				if (Pending.IsValid() || !Writes.IsEmpty() || GFrameCounter <= LastServiceFrame) { return false; }
				if (City->AcceptedImpactCount != 0 || Pawn->Weapons->AcceptedImpacts != 0
					|| City->DeferredImpactEffectsRequests != 0) { return Fail(TEXT("Fresh-world single-impact prerequisite was contaminated.")); }
				ImpactWorld = Now;
				bDispatched = Pawn->Weapons->DispatchImpact(Impact);
				if (!bDispatched) { return Fail(TEXT("Actual maximum weapon dispatch rejected: ") + Pawn->Weapons->LastFailure); }
				Manifest->SetNumberField(TEXT("accepted_impact_world_seconds"), ImpactWorld);
			}
			ObservePhysicsAndEffects();
			if (FXRequestWorld < 0 && City->DeferredImpactEffectsRequests == 1)
			{
				FXRequestWorld = Now;
				Manifest->SetNumberField(TEXT("fx_request_first_observed_world_seconds"), FXRequestWorld);
				Manifest->SetNumberField(TEXT("accepted_to_fx_request_observed_seconds"), FXRequestWorld - ImpactWorld);
			}
			if (City->AcceptedImpactCount != 1 || Pawn->Weapons->AcceptedBombImpacts != 1
				|| City->DeferredImpactEffectsRequests > 1 || Pawn->Weapons->EffectsRequests > 1)
			{
				return Fail(TEXT("Exactly-one accepted maximum impact/outbox accounting was violated."));
			}
			if (FXRequestWorld >= 0 && NextShot < UE_ARRAY_COUNT(ShotTimes) && Now - FXRequestWorld >= ShotTimes[NextShot]
				&& !Pending.IsValid() && Writes.Num() < 2 && GFrameCounter > LastServiceFrame)
			{
				RequestShot(NextShot++);
			}
			if (bFailed) { return Finish(); }
			if (NextShot == UE_ARRAY_COUNT(ShotTimes) && !Pending.IsValid() && Writes.IsEmpty())
			{
				Test.TestEqual(TEXT("Six actual post-impact images written"), WrittenShots, static_cast<int32>(UE_ARRAY_COUNT(ShotTimes)));
				Test.TestEqual(TEXT("City committed effects request exactly once"), City->DeferredImpactEffectsRequests, 1);
				Test.TestEqual(TEXT("Weapon effects accounting exactly once"), Pawn->Weapons->EffectsRequests, 1);
				Test.TestTrue(TEXT("Accepted and presented event identities match"),
					City->LastAcceptedImpactEventId != 0 && City->LastPresentedImpactEventId == City->LastAcceptedImpactEventId);
				Test.TestTrue(TEXT("Actual maximum component with confirmed structural sources observed"), bMaximumSeen && MaxDamageSamples > 0);
				Test.TestTrue(TEXT("Confirmed source array limit respected"), MaxDamageSamples <= DublinImpactFX::MaxDamageSamples);
				Test.TestTrue(TEXT("Target has initialized collision-enabled Chaos physics"), bPhysicsCollisionSeen);
				Test.TestTrue(TEXT("Target fractured leaves really move and separate"), MovedLeaves.Num() >= 2 && MaxTravel > 25 && MaxSeparation > 25);
				Test.TestTrue(TEXT("Original roof collision really changed"), bRoofCollisionChanged);
				Test.TestTrue(TEXT("Multiple buildings physically activated"), ObservedCollections.Num() >= 2);
				Test.TestFalse(TEXT("Expensive maximum component absent in aftermath"), bMaximumActive);
				return Finish();
			}
			return false;
		}

	private:
		FAutomationTestBase& Test;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ADublinCityWorld> City;
		TWeakObjectPtr<ADublinFlightPawn> Pawn;
		TWeakObjectPtr<APlayerController> Player;
		TWeakObjectPtr<ACameraActor> Camera;
		TWeakObjectPtr<AActor> PreviousViewTarget;
		TWeakObjectPtr<UGameViewportClient> Client;
		TWeakObjectPtr<UEditorEngine> ScopedEditor;
		FSceneViewport* ModifiedViewport = nullptr;
		FIntPoint OriginalSize = FIntPoint::ZeroValue;
		bool bOriginalFixed = false, bOriginalThrottle = false, bPawnTick = false, bScoped = false;
		bool bPrepared = false, bPrimed = false, bDispatched = false, bFailed = false;
		bool bFinishing = false, bCleaned = false, bCaptured = false;
		bool bMaximumSeen = false, bMaximumActive = false, bPhysicsCollisionSeen = false, bRoofCollisionChanged = false;
		double StartedWall = 0, PreparedWorld = 0, ImpactWorld = 0, FinishWall = 0, PendingWall = 0;
		double FXRequestWorld = -1;
		double MaxTravel = 0, MaxSeparation = 0;
		uint64 PreparedFrame = 0, RequestedFrame = 0, LastServiceFrame = 0;
		int32 NextShot = 0, WrittenShots = 0, MaxDamageSamples = 0;
		FDelegateHandle RenderedHandle, ThrottleHandle;
		FDublinImpact Impact;
		FDublinFractureRecord Record;
		TSet<int32> MovedLeaves;
		TSet<FString> ObservedCollections;
		FString Directory, PendingPath;
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pending;
		TArray<TSharedPtr<FJsonValue>> Frames;
		TArray<FWrite> Writes;
		IImageWriteQueue* ImageQueue = nullptr;

		bool Bootstrap()
		{
			UWorld* Candidate = GEditor ? GEditor->PlayWorld : nullptr;
			if (!Candidate || !Candidate->HasBegunPlay()) { return false; }
			World = Candidate;
			int32 PIEWorlds = 0;
			if (GEngine)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.World() && Context.World()->WorldType == EWorldType::PIE
						&& !Context.World()->bIsTearingDown) { ++PIEWorlds; }
				}
			}
			if (PIEWorlds != 1) { return Fail(TEXT("Run the visual fixture alone in single-player PIE.")); }
			if (Candidate->IsPaused() || UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) != TEXT("/Game/Maps/Dublin"))
			{
				return Fail(TEXT("Requires an unpaused fresh Dublin PIE world."));
			}
			for (TActorIterator<ADublinCityWorld> It(Candidate); It; ++It)
			{
				if (City.IsValid() && City.Get() != *It) { return Fail(TEXT("Multiple city actors are unsupported.")); }
				City = *It;
			}
			Player = Candidate->GetFirstPlayerController();
			Pawn = Player.IsValid() ? Cast<ADublinFlightPawn>(Player->GetPawn()) : nullptr;
			Client = Candidate->GetGameViewport();
			if (!City.IsValid() || !City->bCityReady || !City->bDestructionReady || !City->bAllBuildingsFractureReady
				|| !Pawn.IsValid() || !Pawn->bSpawnCaptured || !Pawn->Weapons || !Player->PlayerCameraManager
				|| !Client.IsValid() || !Client->GetGameViewport()) { return false; }
			if (City->bAllowPartialBakePreview || !City->GetSourceData() || !City->FractureLibrary
				|| City->AcceptedImpactCount || City->FracturedBuildingCount || City->QueuedImpactCount)
			{
				return Fail(TEXT("Requires a fresh fully ready city, with partial preview disabled."));
			}
			if (City->FractureLibrary->GetOutermost()->IsDirty()) { return Fail(TEXT("Save the fracture library before capturing baseline/final art.")); }
			if (!City->GetActorTransform().Equals(FTransform::Identity, 0.01)) { return Fail(TEXT("Source coordinates require identity city transform.")); }
			const FDublinCityBuilding* Building = City->GetSourceData()->Buildings.FindByPredicate(
				[](const FDublinCityBuilding& B) { return B.Id == SourceId; });
			const FDublinFractureRecord* Found = City->FractureLibrary->Find(SourceId);
			if (!Building || !Found || !Found->bReady) { return Fail(TEXT("Fixed typical source/ready fracture record is missing.")); }
			Record = *Found;
			FBox Bounds(ForceInit);
			for (const FVector& V : Building->Mesh.VerticesCm) { Bounds += V + Building->PivotCm; }
			FVector Roof = FVector::ZeroVector;
			double BestArea = 0;
			for (int32 T = 0; T < Building->MaterialIds.Num(); ++T)
			{
				if (Building->MaterialIds[T] != 0 || !Building->Mesh.Triangles.IsValidIndex(T * 3 + 2)) { continue; }
				const int32 A = Building->Mesh.Triangles[T * 3], B = Building->Mesh.Triangles[T * 3 + 1], C = Building->Mesh.Triangles[T * 3 + 2];
				if (!Building->Mesh.VerticesCm.IsValidIndex(A) || !Building->Mesh.VerticesCm.IsValidIndex(B) || !Building->Mesh.VerticesCm.IsValidIndex(C)) { continue; }
				const FVector VA = Building->Mesh.VerticesCm[A], VB = Building->Mesh.VerticesCm[B], VC = Building->Mesh.VerticesCm[C];
				const double Area = FMath::Abs(FVector::CrossProduct(VB - VA, VC - VA).Z);
				if (Area > BestArea) { BestArea = Area; Roof = (VA + VB + VC) / 3 + Building->PivotCm; }
			}
			FHitResult Hit;
			FCollisionQueryParams Query(SCENE_QUERY_STAT(MaximumBlastVisualRoof), true);
			Query.AddIgnoredActor(Pawn.Get());
			if (!Bounds.IsValid || BestArea <= 0 || !Candidate->LineTraceSingleByChannel(Hit,
				Roof + FVector(0, 0, 200), Roof - FVector(0, 0, 200), ECC_Visibility, Query)
				|| Hit.GetActor() != City.Get() || Hit.ImpactNormal.Z < 0.5 || FVector::Dist(Hit.ImpactPoint, Roof) > 2)
			{
				return Fail(TEXT("Fixed source roof collision prerequisite failed; no relocated impact fallback."));
			}
			Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
			Impact.PositionCm = Hit.ImpactPoint;
			Impact.Normal = Hit.ImpactNormal;
			Impact.Seed = 20260918;
			if (Impact.RadiusCm != DublinImpactFX::MaximumApprovedRadiusCm) { return Fail(TEXT("Maximum game radius is not 120m.")); }
			IImageWriteQueueModule* Module = FModuleManager::LoadModulePtr<IImageWriteQueueModule>(TEXT("ImageWriteQueue"));
			if (!Module || !IFileManager::Get().MakeDirectory(*Directory, true)) { return Fail(TEXT("Async writer/output directory unavailable.")); }
			ImageQueue = &Module->GetWriteQueue();
			ScopedEditor = GEditor;
			UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
			bOriginalThrottle = Settings->bThrottleCPUWhenNotForeground != 0;
			Settings->bThrottleCPUWhenNotForeground = false;
			UEditorEngine::FShouldDisableCPUThrottling Delegate = UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([] { return true; });
			ThrottleHandle = Delegate.GetHandle();
			ScopedEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
			bScoped = true;
			PreviousViewTarget = Player->GetViewTarget();
			bPawnTick = Pawn->IsActorTickEnabled();
			Pawn->Weapons->SuppressInput();
			Pawn->Weapons->BombYieldTonsTNT = 1000.0f;
			Pawn->SetActorTickEnabled(false);
			Player->SetIgnoreMoveInput(true);
			Player->SetIgnoreLookInput(true);
			Camera = Candidate->SpawnActor<ACameraActor>();
			if (!Camera.IsValid()) { return Fail(TEXT("Observer camera spawn failed.")); }
			const FVector CameraPosition(Roof.X - 22000, Roof.Y - 18000, Bounds.Min.Z + 18000);
			Camera->SetActorLocationAndRotation(CameraPosition, (Roof + FVector(0, 0, 2500) - CameraPosition).Rotation());
			Camera->GetCameraComponent()->SetFieldOfView(75);
			Camera->GetCameraComponent()->SetAspectRatio(16.0f / 9.0f);
			Camera->GetCameraComponent()->SetConstraintAspectRatio(true);
			Player->SetViewTarget(Camera.Get());
			Player->PlayerCameraManager->UpdateCamera(0);
			ModifiedViewport = Client->GetGameViewport();
			OriginalSize = ModifiedViewport->GetSizeXY();
			bOriginalFixed = ModifiedViewport->HasFixedSize();
			ModifiedViewport->SetFixedViewportSize(Resolution.X, Resolution.Y);
			PreparedWorld = Candidate->GetTimeSeconds();
			PreparedFrame = GFrameCounter;
			bPrepared = true;
			Point(*Manifest, TEXT("impact_cm"), Impact.PositionCm);
			Point(*Manifest, TEXT("camera_cm"), CameraPosition);
			Manifest->SetNumberField(TEXT("observer_height_above_source_base_cm"), 18000);
			Manifest->SetNumberField(TEXT("camera_horizontal_fov_degrees"), 75);
			Manifest->SetNumberField(TEXT("approved_radius_cm"), Impact.RadiusCm);
			Manifest->SetNumberField(TEXT("yield_game_label"), 1000);
			Manifest->SetStringField(TEXT("source_digest"), Record.SourceDigest);
			Manifest->SetStringField(TEXT("collection"), Record.Collection.ToSoftObjectPath().ToString());
			Manifest->SetNumberField(TEXT("target_recipe"), static_cast<uint8>(Record.Recipe));
			int32 Legacy = 0, Detailed = 0;
			for (const FDublinCityBuilding& B : City->GetSourceData()->Buildings)
			{
				FBox Box(ForceInit);
				for (const FVector& V : B.Mesh.VerticesCm) { Box += V + B.PivotCm; }
				if (!DublinDestruction::SphereTouchesBox(Impact.PositionCm, Impact.RadiusCm, Box)) { continue; }
				const FDublinFractureRecord* R = City->FractureLibrary->Find(B.Id);
				if (R && DublinFractureBake::IsDetailedRecipe(R->Recipe)) { ++Detailed; } else { ++Legacy; }
			}
			Manifest->SetNumberField(TEXT("covered_detailed_records"), Detailed);
			Manifest->SetNumberField(TEXT("covered_legacy_or_missing_records"), Legacy);
			Manifest->SetBoolField(TEXT("detailed_art_evaluation_eligible"), Legacy == 0 && Detailed > 0);
			return false;
		}

		bool HasSize() const
		{
			const FSceneViewport* Viewport = Client.IsValid() ? Client->GetGameViewport() : nullptr;
			return Viewport && Viewport == ModifiedViewport && Client->Viewport == Viewport
				&& Client->GetWorld() == World.Get() && Viewport->GetSizeXY() == Resolution
				&& Viewport->GetRenderTargetTextureSizeXY() == Resolution;
		}

		void ObservePhysicsAndEffects()
		{
			FHitResult Hit;
			FCollisionQueryParams Query(SCENE_QUERY_STAT(MaximumBlastVisualChangedRoof), true);
			Query.AddIgnoredActor(Pawn.Get());
			const bool bHit = World->LineTraceSingleByChannel(Hit, Impact.PositionCm + FVector(0, 0, 200),
				Impact.PositionCm - FVector(0, 0, 200), ECC_Visibility, Query);
			bRoofCollisionChanged |= !bHit || Hit.GetActor() != City.Get()
				|| FVector::Dist(Hit.ImpactPoint, Impact.PositionCm) > 25;
			bMaximumActive = false;
			for (TObjectIterator<UNiagaraComponent> It; It; ++It)
			{
				if (It->GetWorld() != World.Get() || !It->IsActive() || !It->GetAsset()
					|| It->GetAsset()->GetPathName() != DublinImpactFX::MaximumSystemPath) { continue; }
				bMaximumActive = bMaximumSeen = true;
				bool bValid = false;
				const int32 Samples = It->GetVariableInt(TEXT("User.DamageSampleCount"), bValid);
				if (bValid) { MaxDamageSamples = FMath::Max(MaxDamageSamples, Samples); }
			}
			TInlineComponentArray<UGeometryCollectionComponent*> Components;
			City->GetComponents(Components);
			for (UGeometryCollectionComponent* Component : Components)
			{
				if (!Component || !Component->IsRegistered() || !Component->GetRestCollection()) { continue; }
				if (Component->GetPhysicsProxy() && Component->GetPhysicsProxy()->IsInitializedOnPhysicsThread())
				{
					ObservedCollections.Add(Component->GetRestCollection()->GetPathName());
				}
				if (Component->GetRestCollection() != Record.Collection.Get()) { continue; }
				bPhysicsCollisionSeen |= Component->IsPhysicsStateCreated() && Component->GetPhysicsProxy()
					&& Component->GetPhysicsProxy()->IsInitializedOnPhysicsThread()
					&& Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
				const TArray<FTransform> Rest = Component->GetInitialLocalRestTransforms(), Current = Component->GetCurrentTransforms();
				FVector FirstRest = FVector::ZeroVector, FirstCurrent = FVector::ZeroVector;
				bool bFirst = true;
				for (int32 Leaf : Record.LeafTransforms)
				{
					if (!Rest.IsValidIndex(Leaf) || !Current.IsValidIndex(Leaf) || Current[Leaf].ContainsNaN()) { continue; }
					const FVector R = Component->GetComponentTransform().TransformPosition(Rest[Leaf].GetLocation());
					const FVector C = Component->GetComponentTransform().TransformPosition(Current[Leaf].GetLocation());
					const double Travel = FVector::Dist(R, C);
					MaxTravel = FMath::Max(MaxTravel, Travel);
					if (Travel > 25) { MovedLeaves.Add(Leaf); }
					if (bFirst) { FirstRest = R; FirstCurrent = C; bFirst = false; }
					else { MaxSeparation = FMath::Max(MaxSeparation, FMath::Abs(FVector::Dist(C, FirstCurrent) - FVector::Dist(R, FirstRest))); }
				}
			}
		}

		void RequestShot(int32 Slot)
		{
			if (!HasSize() || Player->GetViewTarget() != Camera.Get() || Pending.IsValid() || Writes.Num() >= 2
				|| FScreenshotRequest::IsScreenshotRequested() || UGameViewportClient::OnScreenshotCaptured().IsBound())
			{
				bFailed = true; Test.AddError(TEXT("Capture viewport/camera or exclusive screenshot ownership unavailable.")); return;
			}
			if (Slot < 0)
			{
				TArray<TSharedPtr<FJsonValue>> Footprint;
				for (int32 I = 0; I < 8; ++I)
				{
					const double Angle = I * PI / 4;
					FVector2D Screen;
					const FVector Edge = Impact.PositionCm + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Impact.RadiusCm;
					if (!Player->ProjectWorldLocationToScreen(Edge, Screen, true)
						|| Screen.X < 0 || Screen.X > Resolution.X || Screen.Y < 0 || Screen.Y > Resolution.Y)
					{
						bFailed = true; Test.AddError(TEXT("Fixed oblique camera does not frame the full approved 120m radius.")); return;
					}
					Footprint.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueNumber>(Screen.X), MakeShared<FJsonValueNumber>(Screen.Y)}));
				}
				Manifest->SetArrayField(TEXT("projected_radius_edge_pixels"), Footprint);
			}
			Pending = MakeShared<FJsonObject>();
			Pending->SetNumberField(TEXT("slot"), Slot);
			Pending->SetNumberField(TEXT("scheduled_seconds"), Slot < 0 ? -1 : ShotTimes[Slot]);
			Pending->SetNumberField(TEXT("requested_world_seconds"), World->GetTimeSeconds());
			Pending->SetNumberField(TEXT("requested_engine_frame"), static_cast<double>(GFrameCounter));
			Pending->SetNumberField(TEXT("active_collections"), City->ActiveFractureCollections);
			Pending->SetNumberField(TEXT("fractured_buildings"), City->FracturedBuildingCount);
			Pending->SetNumberField(TEXT("queued_core_hits"), City->QueuedCoreBuildingHitCount);
			Pending->SetNumberField(TEXT("max_leaf_travel_cm"), MaxTravel);
			Pending->SetBoolField(TEXT("maximum_component_active"), bMaximumActive);
			PendingPath = FPaths::Combine(Directory, Slot < 0 ? TEXT("priming-intact.png")
				: FString::Printf(TEXT("maximum-%02d-%.1fs.png"), Slot, ShotTimes[Slot]));
			Pending->SetStringField(TEXT("file"), FPaths::GetCleanFilename(PendingPath));
			Frames.Add(MakeShared<FJsonValueObject>(Pending));
			bCaptured = false;
			PendingWall = FPlatformTime::Seconds();
			RequestedFrame = GFrameCounter;
			// The engine broadcasts after the viewport canvas flush, before any client can consume a global screenshot request.
			RenderedHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FCapture::ViewportRendered);
		}

		void ViewportRendered(FViewport* RenderedViewport)
		{
			check(IsInGameThread());
			if (!Pending.IsValid() || bCaptured || bCleaned || bFailed || RenderedViewport != ModifiedViewport) { return; }
			if (!World.IsValid() || World->bIsTearingDown || !HasSize() || !Player.IsValid() || !Camera.IsValid()
				|| Player->GetViewTarget() != Camera.Get() || FScreenshotRequest::IsScreenshotRequested()
				|| UGameViewportClient::OnScreenshotCaptured().IsBound())
			{
				bFailed = true; Test.AddError(TEXT("Owned rendered viewport/camera or exclusive readback ownership changed.")); return;
			}
			const FIntPoint Size = RenderedViewport->GetRenderTargetTextureSizeXY();
			const int32 Width = Size.X, Height = Size.Y;
			TArray<FColor> Bitmap;
			if (!GetViewportScreenShot(RenderedViewport, Bitmap, FIntRect(FIntPoint::ZeroValue, Resolution))
				|| FIntPoint(Width, Height) != Resolution || Bitmap.Num() != Width * Height || Writes.Num() >= 2)
			{
				bFailed = true; Test.AddError(TEXT("Owned viewport RHI readback/dimensions/copy bound failed.")); return;
			}
			// Match the engine's SDR screenshot path: retain RGB and make the PNG opaque.
			for (FColor& Pixel : Bitmap) { Pixel.A = 255; }
			bCaptured = true;
			Pending->SetBoolField(TEXT("owned_viewport_render_callback"), true);
			Pending->SetStringField(TEXT("readback_world"), World->GetPathName());
			Pending->SetStringField(TEXT("readback_viewport_client"), Client->GetPathName());
			Pending->SetNumberField(TEXT("captured_world_seconds"), World->GetTimeSeconds());
			Pending->SetNumberField(TEXT("actual_seconds_after_impact"), bDispatched ? World->GetTimeSeconds() - ImpactWorld : -1);
			Pending->SetNumberField(TEXT("actual_seconds_after_fx_request_observed"), FXRequestWorld >= 0 ? World->GetTimeSeconds() - FXRequestWorld : -1);
			Pending->SetNumberField(TEXT("captured_engine_frame"), static_cast<double>(GFrameCounter));
			Pending->SetNumberField(TEXT("pixel_crc32"), FCrc::MemCrc32(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor)));
			Pending->SetNumberField(TEXT("width"), Width);
			Pending->SetNumberField(TEXT("height"), Height);
			TArray64<FColor> Pixels;
			Pixels.Append(Bitmap.GetData(), Bitmap.Num());
			TUniquePtr<FImageWriteTask> Task = MakeUnique<FImageWriteTask>();
			Task->Filename = PendingPath;
			Task->Format = ImageFormatFromDesired(EDesiredImageFormat::PNG);
			Task->bOverwriteFile = false;
			Task->PixelData = MakeUnique<TImagePixelData<FColor>>(Resolution, MoveTemp(Pixels));
			TFuture<bool> Result = ImageQueue->Enqueue(MoveTemp(Task), false);
			if (!Result.IsValid()) { bFailed = true; Test.AddError(TEXT("Nonblocking image write enqueue failed.")); return; }
			FWrite& Write = Writes.AddDefaulted_GetRef();
			Write.Result = MoveTemp(Result);
			Write.Frame = Pending;
			Write.Path = PendingPath;
		}

		void PollReadback()
		{
			if (!Pending.IsValid()) { return; }
			if (bCaptured && GFrameCounter > RequestedFrame)
			{
				UGameViewportClient::OnViewportRendered().Remove(RenderedHandle);
				RenderedHandle.Reset();
				LastServiceFrame = GFrameCounter;
				if (Pending->GetIntegerField(TEXT("slot")) < 0) { bPrimed = true; }
				Pending.Reset();
			}
			else if (FPlatformTime::Seconds() - PendingWall > 5)
			{
				bFailed = true; Test.AddError(TEXT("Owned PIE viewport did not deliver rendered pixels within five wall seconds."));
			}
		}

		void PollWrites()
		{
			for (int32 I = Writes.Num() - 1; I >= 0; --I)
			{
				FWrite& Write = Writes[I];
				if (!Write.Result.IsReady()) { continue; }
				const bool bWritten = Write.Result.Get() && IFileManager::Get().FileSize(*Write.Path) > 0;
				Write.Frame->SetBoolField(TEXT("file_written"), bWritten);
				if (!bWritten) { bFailed = true; Test.AddError(TEXT("PNG write failed: ") + Write.Path); }
				else if (Write.Frame->GetIntegerField(TEXT("slot")) >= 0) { ++WrittenShots; }
				Writes.RemoveAt(I);
			}
		}

		bool Fail(const FString& Reason)
		{
			bFailed = true;
			Test.AddError(Reason);
			Manifest->SetStringField(TEXT("failure"), Reason);
			return Finish();
		}

		void Cleanup()
		{
			if (bCleaned) { return; }
			bCleaned = true;
			UGameViewportClient::OnViewportRendered().Remove(RenderedHandle);
			RenderedHandle.Reset();
			if (Client.IsValid() && Client->GetGameViewport() == ModifiedViewport && ModifiedViewport)
			{
				ModifiedViewport->SetFixedViewportSize(bOriginalFixed ? OriginalSize.X : 0, bOriginalFixed ? OriginalSize.Y : 0);
			}
			if (bScoped)
			{
				GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = bOriginalThrottle;
				if (ScopedEditor.IsValid())
				{
					ScopedEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
						[this](const UEditorEngine::FShouldDisableCPUThrottling& D) { return D.GetHandle() == ThrottleHandle; });
				}
				if (Player.IsValid())
				{
					if (PreviousViewTarget.IsValid()) { Player->SetViewTarget(PreviousViewTarget.Get()); }
					Player->SetIgnoreMoveInput(false);
					Player->SetIgnoreLookInput(false);
				}
				if (Pawn.IsValid()) { Pawn->SetActorTickEnabled(bPawnTick); }
			}
			if (Camera.IsValid()) { Camera->Destroy(); }
			if (GEditor && World.IsValid() && GEditor->PlayWorld == World.Get()) { GEditor->RequestEndPlayMap(); }
		}

		bool Finish()
		{
			if (!bFinishing)
			{
				bFinishing = true;
				FinishWall = FPlatformTime::Seconds();
				Manifest->SetBoolField(TEXT("maximum_component_observed"), bMaximumSeen);
				Manifest->SetNumberField(TEXT("maximum_confirmed_damage_samples"), MaxDamageSamples);
				Manifest->SetBoolField(TEXT("target_physics_collision_initialized"), bPhysicsCollisionSeen);
				Manifest->SetBoolField(TEXT("original_roof_collision_changed"), bRoofCollisionChanged);
				Manifest->SetNumberField(TEXT("moved_target_leaves"), MovedLeaves.Num());
				Manifest->SetNumberField(TEXT("max_target_travel_cm"), MaxTravel);
				Manifest->SetNumberField(TEXT("max_target_separation_change_cm"), MaxSeparation);
				Manifest->SetNumberField(TEXT("physically_observed_collections"), ObservedCollections.Num());
				if (City.IsValid()) { Manifest->SetNumberField(TEXT("city_deferred_effects_requests"), City->DeferredImpactEffectsRequests); }
				if (Pawn.IsValid() && Pawn->Weapons) { Manifest->SetNumberField(TEXT("weapon_effects_requests"), Pawn->Weapons->EffectsRequests); }
				Cleanup();
			}
			return Finalize();
		}

		bool Finalize()
		{
			const bool bDeadline = FPlatformTime::Seconds() - FinishWall >= 5 || FPlatformTime::Seconds() - StartedWall >= 90;
			const bool bEnded = !GEditor || !World.IsValid() || GEditor->PlayWorld != World.Get();
			if (!bDeadline && (!bEnded || !Writes.IsEmpty())) { return false; }
			if (!bEnded || !Writes.IsEmpty()) { bFailed = true; Test.AddError(TEXT("Bounded PIE/async writer teardown incomplete.")); }
			Manifest->SetBoolField(TEXT("capture_completed"), !bFailed && WrittenShots == UE_ARRAY_COUNT(ShotTimes));
			Manifest->SetNumberField(TEXT("written_post_impact_images"), WrittenShots);
			Manifest->SetNumberField(TEXT("wall_seconds"), FPlatformTime::Seconds() - StartedWall);
			Manifest->SetArrayField(TEXT("frames"), Frames);
			FString Json;
			const FString Path = FPaths::Combine(Directory, TEXT("manifest.json"));
			if (!FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Json))
				|| !IFileManager::Get().MakeDirectory(*Directory, true)
				|| !FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Test.AddError(TEXT("Could not save visual evidence manifest: ") + Path);
			}
			Test.AddInfo(TEXT("Maximum rendered evidence; inspect visually, not a performance claim: ") + Path);
			bActive = false;
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumBlastVisualPIETest,
	"DublinFlight.Destruction.PIE.MaximumBlastVisual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinMaximumBlastVisualPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (DublinMaximumBlastVisual::bActive || !EditorWorld || GEditor->PlayWorld
		|| EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin") || EditorWorld->GetPackage()->IsDirty()
		|| FScreenshotRequest::IsScreenshotRequested() || UGameViewportClient::OnScreenshotCaptured().IsBound())
	{
		AddError(TEXT("Save Dublin, end existing PIE/screenshots, and run this destructive visual fixture alone."));
		return false;
	}
	DublinMaximumBlastVisual::bActive = true;
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinMaximumBlastVisual::FCapture(*this));
	return true;
}

#endif
