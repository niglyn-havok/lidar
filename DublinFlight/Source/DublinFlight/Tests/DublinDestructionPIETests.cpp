#include "City/DublinCityWorld.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "DublinFlightPawn.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "SceneView.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Weapons/DublinWeaponModel.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinDestructionVerification, Log, All);

namespace DublinDestruction::PIETests
{
constexpr double BootstrapTimeout = 60.0;
constexpr double ScenarioWorldSeconds = 30.0;
constexpr double ScenarioWallSeconds = 45.0;

bool IsDublinPIE(const UWorld* World)
{
	return World && World->WorldType == EWorldType::PIE && !World->bIsTearingDown
		&& UWorld::RemovePIEPrefix(World->GetPackage()->GetName()) == TEXT("/Game/Maps/Dublin");
}

ADublinCityWorld* FindCity(UWorld& World)
{
	ADublinCityWorld* Found = nullptr;
	for (TActorIterator<ADublinCityWorld> It(&World); It; ++It)
	{
		if (Found) { return nullptr; }
		Found = *It;
	}
	return Found;
}

TSharedRef<FJsonObject> WorldDiagnostics(UWorld* World, ADublinCityWorld* City)
{
	APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Possessed = Player ? Player->GetPawn() : nullptr;
	ADublinFlightPawn* Plane = Cast<ADublinFlightPawn>(Possessed);
	UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
	FViewport* Viewport = Client ? Client->Viewport : nullptr;
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("engine_present"), GEngine != nullptr);
	Out->SetNumberField(TEXT("engine_frame_counter"), static_cast<double>(GFrameCounter));
	Out->SetNumberField(TEXT("engine_world_context_count"), GEngine ? GEngine->GetWorldContexts().Num() : 0);
	Out->SetBoolField(TEXT("editor_play_world_present"), GEditor && GEditor->PlayWorld != nullptr);
	Out->SetBoolField(TEXT("world_present"), World != nullptr);
	Out->SetStringField(TEXT("world_name"), World ? World->GetPathName() : TEXT("<none>"));
	Out->SetNumberField(TEXT("world_type"), World ? static_cast<int32>(World->WorldType) : -1);
	Out->SetBoolField(TEXT("actual_Dublin_PIE"), IsDublinPIE(World));
	Out->SetBoolField(TEXT("world_tearing_down"), World && World->bIsTearingDown);
	Out->SetBoolField(TEXT("world_begun_play"), World && World->HasBegunPlay());
	Out->SetBoolField(TEXT("world_paused"), World && World->IsPaused());
	Out->SetNumberField(TEXT("world_seconds"), World ? World->GetTimeSeconds() : 0);
	Out->SetNumberField(TEXT("world_delta_seconds"), World ? World->GetDeltaSeconds() : 0);
	Out->SetBoolField(TEXT("player_controller_present"), Player != nullptr);
	Out->SetBoolField(TEXT("local_player_controller"), Player && Player->IsLocalController());
	Out->SetBoolField(TEXT("player_input_present"), Player && Player->PlayerInput != nullptr);
	Out->SetBoolField(TEXT("possessed_pawn_present"), Possessed != nullptr);
	Out->SetStringField(TEXT("possessed_pawn_class"), Possessed ? Possessed->GetClass()->GetPathName() : TEXT("<none>"));
	Out->SetBoolField(TEXT("dublin_pawn_present"), Plane != nullptr);
	Out->SetBoolField(TEXT("spawn_captured"), Plane && Plane->bSpawnCaptured);
	Out->SetBoolField(TEXT("weapons_component_present"), Plane && Plane->Weapons != nullptr);
	Out->SetBoolField(TEXT("unique_city_present"), City != nullptr);
	Out->SetBoolField(TEXT("city_ready"), City && City->bCityReady);
	Out->SetBoolField(TEXT("source_data_present"), City && City->GetSourceData() != nullptr);
	Out->SetBoolField(TEXT("fracture_library_present"), City && City->FractureLibrary != nullptr);
	Out->SetBoolField(TEXT("destruction_ready"), City && City->bDestructionReady);
	Out->SetBoolField(TEXT("all_buildings_ready"), City && City->bAllBuildingsFractureReady);
	Out->SetBoolField(TEXT("partial_preview"), City && City->bAllowPartialBakePreview);
	Out->SetNumberField(TEXT("fracture_ready_buildings"), City ? City->FractureReadyBuildingCount : 0);
	Out->SetNumberField(TEXT("source_buildings"), City ? City->BuildingCount : 0);
	Out->SetStringField(TEXT("city_build_error"), City ? City->LastBuildError : TEXT("<no unique city>"));
	Out->SetStringField(TEXT("destruction_error"), City ? City->LastDestructionError : TEXT("<no unique city>"));
	Out->SetBoolField(TEXT("viewport_client_present"), Client != nullptr);
	Out->SetBoolField(TEXT("viewport_present"), Viewport != nullptr);
	Out->SetBoolField(TEXT("viewport_has_positive_size"), Viewport && Viewport->GetSizeXY().X > 0 && Viewport->GetSizeXY().Y > 0);
	// Read-only diagnostics: neither focus nor mouse capture is a physical-test prerequisite.
	Out->SetBoolField(TEXT("viewport_has_OS_focus"), Viewport && Viewport->HasFocus());
	Out->SetBoolField(TEXT("viewport_has_mouse_capture"), Viewport && Viewport->HasMouseCapture());
	Out->SetBoolField(TEXT("viewport_focus_required"), false);
	return Out;
}

FString DiagnosticText(const TSharedRef<FJsonObject>& State)
{
	FString Text;
	FJsonSerializer::Serialize(State, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	return Text;
}

TSharedRef<FJsonObject> Counters(const ADublinCityWorld& City)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("accepted_impacts"), City.AcceptedImpactCount);
	Out->SetNumberField(TEXT("queued_impacts"), City.QueuedImpactCount);
	Out->SetNumberField(TEXT("queued_building_hits"), City.QueuedBuildingHitCount);
	Out->SetNumberField(TEXT("active_collections"), City.ActiveFractureCollections);
	Out->SetNumberField(TEXT("active_pieces"), City.ActiveFracturePieces);
	Out->SetNumberField(TEXT("sleeping_collections"), City.SleepingFractureCollections);
	Out->SetNumberField(TEXT("fractured_buildings"), City.FracturedBuildingCount);
	Out->SetNumberField(TEXT("reported_moved_fragments"), City.MovedFragmentCount);
	Out->SetNumberField(TEXT("forced_sleep"), City.ForcedSleepCount);
	Out->SetNumberField(TEXT("crater_changed_nodes"), City.CraterChangedNodeCount);
	Out->SetNumberField(TEXT("water_max_displacement_cm"), City.WaterMaxDisplacementCm);
	Out->SetBoolField(TEXT("queue_blocked"), City.bImpactQueueBlocked);
	Out->SetBoolField(TEXT("full_city_ready"), City.bAllBuildingsFractureReady);
	Out->SetBoolField(TEXT("partial_preview"), City.bAllowPartialBakePreview);
	Out->SetNumberField(TEXT("ready_buildings"), City.FractureReadyBuildingCount);
	Out->SetNumberField(TEXT("source_buildings"), City.BuildingCount);
	Out->SetStringField(TEXT("last_error"), City.LastDestructionError);
	return Out;
}

bool WithinBudgets(const ADublinCityWorld& City)
{
	return !City.bImpactQueueBlocked && City.QueuedImpactCount <= MaxQueuedImpacts
		&& City.ActiveFractureCollections <= MaxActiveCollections && City.ActiveFracturePieces <= MaxActivePieces;
}

bool SaveEvidence(const TCHAR* Field, const TSharedRef<FJsonObject>& Evidence)
{
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("destruction-pie-handoff.json"));
	TSharedPtr<FJsonObject> Root;
	FString Existing;
	if (FFileHelper::LoadFileToString(Existing, *Path))
	{
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Existing), Root) || !Root)
		{
			UE_LOG(LogDublinDestructionVerification, Error, TEXT("Refusing to overwrite invalid handoff JSON: %s"), *Path);
			return false;
		}
	}
	else { Root = MakeShared<FJsonObject>(); }
	Root->SetObjectField(Field, Evidence);
	FString Json;
	FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
	Json.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Json.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void SetPoint(FJsonObject& Object, const TCHAR* Name, const FVector& Point)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(Point.X));
	Values.Add(MakeShared<FJsonValueNumber>(Point.Y));
	Values.Add(MakeShared<FJsonValueNumber>(Point.Z));
	Object.SetArrayField(Name, Values);
}

bool Trace(UWorld& World, const FVector& Start, const FVector& End, FHitResult& Hit)
{
	// City's PMCs use complex-as-simple; simple queries also include actual Chaos convex pieces.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DublinDestructionPIE), false);
	if (APlayerController* Player = World.GetFirstPlayerController())
	{
		if (Player->GetPawn()) { Params.AddIgnoredActor(Player->GetPawn()); }
	}
	return World.LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
}

uint32 MeshFingerprint(UProceduralMeshComponent& Component)
{
	uint32 Hash = 0;
	for (int32 I = 0; I < Component.GetNumSections(); ++I)
	{
		const FProcMeshSection* Section = Component.GetProcMeshSection(I);
		if (!Section) { continue; }
		for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
		{
			Hash = FCrc::MemCrc32(&Vertex.Position, sizeof(Vertex.Position), Hash);
		}
		Hash = FCrc::MemCrc32(Section->ProcIndexBuffer.GetData(), Section->ProcIndexBuffer.Num() * sizeof(uint32), Hash);
		Hash = FCrc::MemCrc32(&Section->bEnableCollision, sizeof(Section->bEnableCollision), Hash);
		Hash = FCrc::MemCrc32(&Section->bSectionVisible, sizeof(Section->bSectionVisible), Hash);
	}
	return Hash;
}

struct FSelectedScene
{
	const FDublinCityData* Source = nullptr;
	TArray<FBox> Bounds;
	TArray<bool> Ready;
	TArray<int32> Sorted;
	int32 Target = INDEX_NONE;
	int32 Control = INDEX_NONE;
	FDublinFractureRecord Record;
	FDublinImpact Cannon;
	FDublinImpact SupportBomb;
	FDublinImpact GroundBomb;
	FDublinImpact WaterBomb;
	FVector ControlRoof = FVector::ZeroVector;
	FVector GroundBefore = FVector::ZeroVector;
	FVector FarWater = FVector::ZeroVector;
	float NearWaterBase = 0;
	float FarWaterBase = 0;
	int32 GroundChunk = INDEX_NONE;
	int32 GroundTriangle = INDEX_NONE;
	int32 NearWaterVertex = INDEX_NONE;
	int32 FarWaterVertex = INDEX_NONE;
	TWeakObjectPtr<UProceduralMeshComponent> ControlMesh;
	TWeakObjectPtr<UProceduralMeshComponent> GroundMesh;
	TWeakObjectPtr<UProceduralMeshComponent> WaterMesh;
	uint32 ControlFingerprint = 0;
	FTransform ControlTransform;
	FString Error;
	bool bRequireRecoveredBake = false;

	bool SafeImpact(const FDublinImpact& Impact) const
	{
		FString Why;
		if (!ValidateImpact(Impact, Why)) { return false; }
		if (FMath::Abs(Impact.PositionCm.X) + Impact.RadiusCm >= 38400
			|| FMath::Abs(Impact.PositionCm.Y) + Impact.RadiusCm >= 38400) { return false; }
		for (int32 I = 0; I < Bounds.Num(); ++I)
		{
			if (SphereTouchesBox(Impact.PositionCm, Impact.RadiusCm, Bounds[I]) && !Ready[I]) { return false; }
		}
		return true;
	}

	bool ClearFootprint(const FVector& Point, double Margin) const
	{
		for (const FBox& Box : Bounds)
		{
			const double DX = FMath::Max(0.0, FMath::Max(Box.Min.X - Point.X, Point.X - Box.Max.X));
			const double DY = FMath::Max(0.0, FMath::Max(Box.Min.Y - Point.Y, Point.Y - Box.Max.Y));
			if (DX * DX + DY * DY <= Margin * Margin) { return false; }
		}
		return true;
	}

	UProceduralMeshComponent* ChunkMesh(ADublinCityWorld& City, int32 Building) const
	{
		for (const FDublinCityChunk& Chunk : City.GetBuildingChunks())
		{
			if (!Chunk.BuildingIndices.Contains(Building)) { continue; }
			TInlineComponentArray<UProceduralMeshComponent*> Components;
			City.GetComponents(Components);
			for (UProceduralMeshComponent* Component : Components)
			{
				// Unreal may replace a name's numeric suffix when duplicating components for PIE.
				if (IsValid(Component) && Component->IsRegistered()
					&& Component->GetName().StartsWith(TEXT("DublinBuildings_"))
					&& Component->ComponentHasTag(TEXT("DublinFlight.City.GeneratedMesh.v1"))
					&& Component->GetRelativeLocation().Equals(Chunk.OriginCm, 0.1))
				{
					return Component;
				}
			}
		}
		return nullptr;
	}

	bool RoofProbe(ADublinCityWorld& City, int32 Index, FVector& Point) const
	{
		const FDublinCityBuilding& Building = Source->Buildings[Index];
		UProceduralMeshComponent* Mesh = ChunkMesh(City, Index);
		if (!Mesh) { return false; }
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			if (Building.MaterialIds[T] != 0) { continue; }
			const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]] + Building.PivotCm;
			const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]] + Building.PivotCm;
			const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]] + Building.PivotCm;
			Point = (A + B + C) / 3;
			FHitResult Hit;
			if (Trace(*City.GetWorld(), Point + FVector(0, 0, 1000), Point - FVector(0, 0, 100), Hit)
				&& Hit.GetComponent() == Mesh && Hit.ImpactPoint.Equals(Point, 10)) { return true; }
		}
		return false;
	}

	bool SelectBuildings(ADublinCityWorld& City)
	{
		TArray<FString> Rejections;
		for (int32 Index : Sorted)
		{
			const FDublinCityBuilding& Building = Source->Buildings[Index];
			const FDublinFractureRecord* Candidate = City.FractureLibrary->Find(Building.Id);
			const FBox& Box = Bounds[Index];
			const FVector Size = Box.GetSize();
			// Reject rooftop fixtures, narrow spires, bridges and clipped map-edge volumes.
			if (!Ready[Index] || !Candidate || (bRequireRecoveredBake && Candidate->BakeAttempts < 2)
				|| Candidate->PieceCount < 4 || Candidate->Anchors.Num() < 2
				|| FMath::Min(Size.X, Size.Y) < 600 || FMath::Max(Size.X, Size.Y) > 5000
				|| Size.Z < 500 || Size.Z > 4000 || Size.Z > 2 * FMath::Max(Size.X, Size.Y)
				|| Building.Name.Contains(TEXT("bridge"), ESearchCase::IgnoreCase)) { continue; }
			const double Radius = FVector2D(Box.GetExtent().X, Box.GetExtent().Y).Size() + 120;
			SupportBomb = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb,
				static_cast<float>(FMath::Pow(Radius / 600.0, 4.0)), DublinWeapons::FBombCurve());
			SupportBomb.PositionCm = FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z + 100);
			SupportBomb.Seed = 58015;
			if (SupportBomb.RadiusCm < Radius - 1 || !SafeImpact(SupportBomb))
			{
				Rejections.Add(Building.Id + TEXT(": unsafe support-bomb footprint"));
				continue;
			}
			float WaterZ;
			if (DublinDestruction::SourceWaterZ(Source->Water, SupportBomb.PositionCm, WaterZ)) { continue; }
			FVector Roof;
			if (!RoofProbe(City, Index, Roof))
			{
				Rejections.Add(Building.Id + TEXT(": roof collision or chunk lookup failed"));
				continue;
			}
			UProceduralMeshComponent* Mesh = ChunkMesh(City, Index);
			bool HasCannonPoint = false;
			for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
			{
				if (Building.MaterialIds[T] != 1) { continue; }
				const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]] + Building.PivotCm;
				const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]] + Building.PivotCm;
				const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]] + Building.PivotCm;
				Cannon = DublinWeapons::MakeImpact(EDublinImpactKind::Cannon, 0, DublinWeapons::FBombCurve());
				Cannon.PositionCm = (A + B + C) / 3;
				Cannon.Normal = DublinCity::ClockwiseNormal(A, B, C);
				Cannon.Seed = 58011;
				if (FMath::Abs(Cannon.Normal.Z) > 0.4 || !SafeImpact(Cannon)) { continue; }
				FHitResult Hit;
				if (Trace(*City.GetWorld(), Cannon.PositionCm + Cannon.Normal * 350,
					Cannon.PositionCm - Cannon.Normal * 50, Hit) && Hit.GetComponent() == Mesh
					&& Hit.ImpactPoint.Equals(Cannon.PositionCm, 10))
				{
					HasCannonPoint = true;
					break;
				}
			}
			if (!HasCannonPoint)
			{
				Rejections.Add(Building.Id + TEXT(": no safe collidable wall point"));
				continue;
			}
			UGeometryCollection* Asset = Candidate->Collection.LoadSynchronous();
			if (!Asset || Asset->IsEmpty() || (!Asset->HasMeshData() && !Asset->HasNaniteData()))
			{
				Rejections.Add(Building.Id + TEXT(": baked collection has no usable render data"));
				continue;
			}
			Target = Index;
			Record = *Candidate;
			break;
		}
		if (Target == INDEX_NONE)
		{
			Error = TEXT("No compact, collidable, multi-anchor baked building has a safe cannon AND support-bomb footprint. "
				"Partial preview cannot hit unbaked neighbors; bake an eligible neighborhood, not a spire/rooftop fixture. Rejections: ")
				+ FString::Join(Rejections, TEXT("; "));
			return false;
		}
		for (int32 Index : Sorted)
		{
			if (Bounds[Index].ComputeSquaredDistanceToPoint(SupportBomb.PositionCm) < FMath::Square(15000.0 + SupportBomb.RadiusCm)
				|| ChunkMesh(City, Index) == ChunkMesh(City, Target)) { continue; }
			if (RoofProbe(City, Index, ControlRoof))
			{
				Control = Index;
				ControlMesh = ChunkMesh(City, Index);
				ControlFingerprint = MeshFingerprint(*ControlMesh.Get());
				ControlTransform = ControlMesh->GetComponentTransform();
				break;
			}
		}
		if (Control == INDEX_NONE) { Error = TEXT("No distant unhit collision/geometry control could be selected."); return false; }
		return true;
	}

	bool SelectGround(ADublinCityWorld& City)
	{
		GroundBomb = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1, DublinWeapons::FBombCurve());
		GroundBomb.Seed = 58016;
		for (int32 ChunkIndex = 0; ChunkIndex < City.GetTerrainChunks().Num(); ++ChunkIndex)
		{
			const FDublinCityChunk& Chunk = City.GetTerrainChunks()[ChunkIndex];
			if (Chunk.Sections.IsEmpty()) { continue; }
			const FDublinCitySection& Section = Chunk.Sections[0];
			for (int32 T = 0; T + 2 < Section.Triangles.Num(); T += 12)
			{
				FVector Point = Chunk.OriginCm;
				Point += (Section.Vertices[Section.Triangles[T]] + Section.Vertices[Section.Triangles[T + 1]]
					+ Section.Vertices[Section.Triangles[T + 2]]) / 3;
				if (FMath::Abs(Point.X) > 33000 || FMath::Abs(Point.Y) > 33000 || !ClearFootprint(Point, 1200)
					|| FVector::DistXY(Point, SupportBomb.PositionCm) < SupportBomb.RadiusCm + 5000
					|| FVector::DistXY(Point, ControlRoof) < 5000) { continue; }
				bool Dry = true;
				for (int32 Ring = 0; Ring < 9; ++Ring)
				{
					const double Angle = Ring * PI / 4;
					const FVector P = Ring == 8 ? Point : Point + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * 1000;
					float Z;
					if (DublinDestruction::SourceWaterZ(Source->Water, P, Z)) { Dry = false; break; }
				}
				if (!Dry) { continue; }
				FHitResult Hit;
				if (!Trace(*City.GetWorld(), Point + FVector(0, 0, 2000), Point - FVector(0, 0, 1000), Hit)
					|| !Hit.ImpactPoint.Equals(Point, 10)) { continue; }
				UProceduralMeshComponent* Mesh = Cast<UProceduralMeshComponent>(Hit.GetComponent());
				if (!Mesh || Mesh->GetOwner() != &City || !Mesh->GetName().StartsWith(TEXT("DublinTerrain_"))) { continue; }
				GroundBefore = Point;
				GroundBomb.PositionCm = Point;
				GroundChunk = ChunkIndex;
				GroundTriangle = T;
				GroundMesh = Mesh;
				return true;
			}
		}
		Error = TEXT("No collision-backed, dry terrain triangle clear of buildings/river/other impacts was found.");
		return false;
	}

	bool SelectWater(ADublinCityWorld& City)
	{
		WaterBomb = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1, DublinWeapons::FBombCurve());
		WaterBomb.bWater = true;
		WaterBomb.Seed = 58017;
		for (int32 Y = -32000; Y <= 32000; Y += 800)
		{
			for (int32 X = -32000; X <= 32000; X += 800)
			{
				const FVector Near(X, Y, 0);
				float NearZ;
				if (!DublinDestruction::SourceWaterZ(Source->Water, Near, NearZ) || !ClearFootprint(Near, 1400)) { continue; }
				for (const FVector& Direction : {FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0)})
				{
					const FVector Side(-Direction.Y, Direction.X, 0);
					bool WetCorridor = true;
					for (int32 Step = -2; Step <= 8 && WetCorridor; ++Step)
					{
						for (int32 Offset = -2; Offset <= 2; ++Offset)
						{
							FVector P = Near + Direction * (Step * 400) + Side * (Offset * 400);
							float Z;
							if (!DublinDestruction::SourceWaterZ(Source->Water, P, Z) || !ClearFootprint(P, 200))
							{
								WetCorridor = false;
								break;
							}
							P.Z = Z;
							FHitResult Hit;
							if (Trace(*City.GetWorld(), P + FVector(0, 0, 10000), P + FVector(0, 0, 20), Hit))
							{
								WetCorridor = false;
								break;
							}
						}
					}
					if (!WetCorridor) { continue; }
					FarWater = Near + Direction * 2800;
					if (!DublinDestruction::SourceWaterZ(Source->Water, FarWater, FarWaterBase)) { continue; }
					NearWaterBase = NearZ;
					WaterBomb.PositionCm = FVector(Near.X, Near.Y, NearZ);
					FarWater.Z = FarWaterBase;
					if (!SafeImpact(WaterBomb)) { continue; }
					TInlineComponentArray<UProceduralMeshComponent*> Components;
					City.GetComponents(Components);
					for (UProceduralMeshComponent* Component : Components)
					{
						if (!IsValid(Component) || !Component->GetName().StartsWith(TEXT("DublinWater"))) { continue; }
						const FProcMeshSection* Section = Component->GetProcMeshSection(0);
						if (!Section) { continue; }
						NearWaterVertex = FarWaterVertex = INDEX_NONE;
						for (int32 I = 0; I < Section->ProcVertexBuffer.Num(); ++I)
						{
							const FVector P = Component->GetComponentTransform().TransformPosition(Section->ProcVertexBuffer[I].Position);
							if (FVector::DistXY(P, WaterBomb.PositionCm) < 1) { NearWaterVertex = I; }
							if (FVector::DistXY(P, FarWater) < 1) { FarWaterVertex = I; }
							if (NearWaterVertex != INDEX_NONE && FarWaterVertex != INDEX_NONE) { break; }
						}
						if (NearWaterVertex != INDEX_NONE && FarWaterVertex != INDEX_NONE)
						{
							WaterMesh = Component;
							return true;
						}
					}
				}
			}
		}
		Error = TEXT("No exact-river, bridge-free wet corridor with near/far rendered grid vertices was found.");
		return false;
	}

	bool Initialize(ADublinCityWorld& City)
	{
		Source = City.GetSourceData();
		if (!Source || !City.bCityReady || !City.bDestructionReady || !City.FractureLibrary
			|| !City.GetActorTransform().Equals(FTransform::Identity, 0.01)
			|| Source->ExtentMeters != DublinCity::ExtentMeters)
		{
			Error = TEXT("Requires READY actual 768m city at its authored identity transform with a saved fracture library.");
			return false;
		}
		if (!City.bAllBuildingsFractureReady && !City.bAllowPartialBakePreview)
		{
			Error = TEXT("Incomplete fracture library without explicit partial-bake preview.");
			return false;
		}
		if (!FPackageName::DoesPackageExist(City.FractureLibrary->GetOutermost()->GetName()))
		{
			Error = TEXT("The fracture library is not a saved asset.");
			return false;
		}
		for (int32 I = 0; I < Source->Buildings.Num(); ++I)
		{
			const FDublinCityBuilding& Building = Source->Buildings[I];
			FBox Box(ForceInit);
			for (const FVector& Vertex : Building.Mesh.VerticesCm) { Box += Vertex + Building.PivotCm; }
			Bounds.Add(Box);
			Sorted.Add(I);
			const FDublinFractureRecord* R = City.FractureLibrary->Find(Building.Id);
			Ready.Add(R && R->bReady && DublinFractureBake::HasValidPieceBudget(*R) && R->RootTransform != INDEX_NONE
				&& R->LeafTransforms.Num() == R->PieceCount && !R->Anchors.IsEmpty() && !R->Collection.IsNull()
				&& DublinFractureBake::IsCurrentRecord(Building, *R)
				&& FPackageName::DoesPackageExist(R->Collection.ToSoftObjectPath().GetLongPackageName()));
		}
		Sorted.Sort([this](int32 A, int32 B) { return Source->Buildings[A].Id < Source->Buildings[B].Id; });
		return SelectBuildings(City) && SelectGround(City) && SelectWater(City);
	}

	TSharedRef<FJsonObject> Describe() const
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Source && Target != INDEX_NONE) { Out->SetStringField(TEXT("target_id"), Source->Buildings[Target].Id); }
		if (Source && Control != INDEX_NONE) { Out->SetStringField(TEXT("distant_control_id"), Source->Buildings[Control].Id); }
		Out->SetStringField(TEXT("collection"), Record.Collection.ToSoftObjectPath().ToString());
		Out->SetNumberField(TEXT("authored_leaves"), Record.PieceCount);
		Out->SetNumberField(TEXT("anchors"), Record.Anchors.Num());
		SetPoint(*Out, TEXT("cannon_cm"), Cannon.PositionCm);
		SetPoint(*Out, TEXT("support_bomb_cm"), SupportBomb.PositionCm);
		Out->SetNumberField(TEXT("support_bomb_radius_cm"), SupportBomb.RadiusCm);
		Out->SetNumberField(TEXT("support_bomb_game_yield_tons"), SupportBomb.YieldTonsTNT);
		SetPoint(*Out, TEXT("crater_cm"), GroundBefore);
		SetPoint(*Out, TEXT("water_near_cm"), WaterBomb.PositionCm);
		SetPoint(*Out, TEXT("water_far_cm"), FarWater);
		Out->SetStringField(TEXT("selection_error"), Error);
		return Out;
	}
};

bool SubmitImpact(ADublinCityWorld& City, const FDublinImpact& Impact)
{
	UDublinImpactEffectsSubsystem* Effects = City.GetWorld()->GetSubsystem<UDublinImpactEffectsSubsystem>();
	if (!Effects || !City.ApplyImpact(Impact)) { return false; }
	// Identical accepted-impact ordering to weapon dispatch; never emit on rejection.
	Effects->EmitImpact(Impact);
	return true;
}

int32 ActiveImpactEffects(UWorld& World, bool WaterOnly = false)
{
	int32 Count = 0;
	for (TObjectIterator<UNiagaraComponent> It; It; ++It)
	{
		if (It->GetWorld() != &World || !It->IsActive() || !It->GetAsset()) { continue; }
		const FString Path = It->GetAsset()->GetPathName();
		if (WaterOnly ? Path.StartsWith(TEXT("/Game/FX/NS_WaterImpact.")) :
			(Path.StartsWith(TEXT("/Game/FX/NS_WaterImpact.")) || Path.StartsWith(TEXT("/Game/FX/NS_BombExplosion."))
				|| Path.StartsWith(TEXT("/Game/FX/NS_CannonImpact.")))) { ++Count; }
	}
	return Count;
}

UGeometryCollectionComponent* TargetCollection(ADublinCityWorld& City, const FSelectedScene& Scene)
{
	TInlineComponentArray<UGeometryCollectionComponent*> Components;
	City.GetComponents(Components);
	for (UGeometryCollectionComponent* Component : Components)
	{
		if (IsValid(Component) && Component->IsRegistered() && Component->GetRestCollection()
			&& Component->ComponentHasTag(TEXT("DublinFlight.City.Fracture.v1"))
			&& Component->GetRestCollection()->GetPathName() == Scene.Record.Collection.ToSoftObjectPath().ToString()
			&& Component->GetRelativeLocation().Equals(Scene.Source->Buildings[Scene.Target].PivotCm, 0.1)) { return Component; }
	}
	return nullptr;
}

void SuppressPlayer(UWorld& World)
{
	if (APlayerController* Player = World.GetFirstPlayerController())
	{
		Player->FlushPressedKeys();
		if (ADublinFlightPawn* Plane = Cast<ADublinFlightPawn>(Player->GetPawn()))
		{
			if (Plane->Weapons) { Plane->Weapons->SuppressInput(); Plane->Weapons->ClearProjectiles(); }
			if (!Plane->IsGodMode()) { Plane->ToggleGodMode(); }
		}
	}
}

class FSmokeAndCannonWaterVisuals final : public IAutomationLatentCommand
{
public:
	explicit FSmokeAndCannonWaterVisuals(FAutomationTestBase& InTest)
		: Test(InTest), StartedWall(FPlatformTime::Seconds())
	{
		RunName = TEXT("SmokeAndCannonWaterVisuals-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("Feedback-20260918"));
	}
	virtual ~FSmokeAndCannonWaterVisuals() override { Cleanup(); }

	virtual bool Update() override
	{
		if (FPlatformTime::Seconds() - StartedWall > 90.0)
		{
			Fail(TEXT("Smoke/water visual fixture exceeded its 90-second wall timeout."));
			return Finish();
		}
		if (bFailed && !bCapturePending) { return Finish(); }
		if (!bStarted) { return Bootstrap(); }
		if (!IsDublinPIE(World.Get()) || !City.IsValid() || !Player.IsValid()
			|| !Player->PlayerCameraManager || !Camera.IsValid())
		{
			Fail(TEXT("Owned visual-test PIE, city, player or camera was lost."));
			return Finish();
		}
		if (!WithinBudgets(*City.Get()) || ActiveImpactEffects(*World.Get()) > DublinImpactFX::MaxSystems)
		{
			Fail(TEXT("Visual-test impact queue or live effect budget exceeded."));
			return Finish();
		}
		const double Now = World->GetTimeSeconds();
		if (Now > LastWorldTime) { LastWorldTime = Now; ++Frames; }
		if (Phase == 0 || Phase == 3)
		{
			if (Frames < 3 || Now - PhaseStart < 0.35) { return false; }
			Player->PlayerCameraManager->UpdateCamera(0.0f);
			FVector Position;
			FRotator Rotation;
			Player->GetPlayerViewPoint(Position, Rotation);
			if (Player->GetViewTarget() != Camera.Get() || !Position.Equals(Camera->GetActorLocation(), 1.0)
				|| !Rotation.Equals(Camera->GetActorRotation(), 0.1))
			{
				Fail(TEXT("Player camera cache did not settle on the owned effect observer before impact admission."));
				return Finish();
			}
			if (!ValidateObserver()) { return Finish(); }
			if (Phase == 0)
			{
				if (!SubmitImpact(*City.Get(), Scene.GroundBomb))
				{
					Fail(TEXT("Selected real dry-ground bomb was rejected."));
					return Finish();
				}
				SmokeStart = Now;
				Phase = 1;
			}
			else
			{
				WaterStart = Now;
				NextCannon = Now;
				Phase = 4;
			}
		}
		if (Phase == 1 && Capture(TEXT("Smoke-04s"), 4.0, SmokeStart))
		{
			Phase = 2;
		}
		if (Phase == 2 && Now - SmokeStart >= 12.0)
		{
			if (!bSmokeChecked)
			{
				bSmokeChecked = true;
				BombComponentsAt12 = LiveBombComponents();
				SmokeCheckAge = Now - SmokeStart;
				if (BombComponentsAt12 == 0)
				{
					Fail(TEXT("No live NS_BombExplosion route at 12 seconds; a short burst is not sustained smoke."));
				}
			}
			// Still request the late image on route failure; do not substitute component counts for rendered proof.
			if (Capture(TEXT("Smoke-12s"), 12.0, SmokeStart))
			{
				if (bFailed) { return Finish(); }
				UseObserver(WaterObserver);
				BeginPhase(3);
			}
		}
		if (Phase == 4)
		{
			const double Age = Now - WaterStart;
			if (Age < 2.0 && Now >= NextCannon)
			{
				FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Cannon, 0, DublinWeapons::FBombCurve());
				Impact.PositionCm = Scene.WaterBomb.PositionCm;
				Impact.Normal = FVector::UpVector;
				Impact.bWater = true;
				Impact.Seed = 580180 + CannonImpacts;
				if (!SubmitImpact(*City.Get(), Impact)) { Fail(TEXT("Default cannon water impact was rejected.")); return Finish(); }
				++CannonImpacts;
				LastCannon = Now;
				NextCannon = Now + 0.2;
				CannonAges.Add(MakeShared<FJsonValueNumber>(Age));
			}
			const double TargetAge = WaterCapture == 0 ? 0.7 : 1.5;
			if (WaterCapture < 2 && Age >= TargetAge)
			{
				if (ActiveImpactEffects(*World.Get(), true) == 0)
				{
					Fail(TEXT("No live NS_WaterImpact component during the cannon-water capture interval."));
					return Finish();
				}
				if (Capture(WaterCapture == 0 ? TEXT("CannonWater-00_7s") : TEXT("CannonWater-01_5s"), TargetAge, WaterStart))
				{
					++WaterCapture;
				}
			}
			if (Age >= 2.0 && WaterCapture == 2)
			{
				if (CannonImpacts < 8) { Fail(TEXT("Too few real advancing-frame cannon impacts for the approximately 5 Hz, 2-second sequence.")); }
				BeginPhase(5);
			}
		}
		if (Phase == 5 && Now - LastCannon >= 6.0)
		{
			if (ActiveImpactEffects(*World.Get(), true) == 0)
			{
				bWaterExpired = true;
				return Finish();
			}
			if (Now - LastCannon > 12.0) { Fail(TEXT("Transient cannon-water components did not expire within 12 seconds after firing stopped.")); }
		}
		return bFailed && !bCapturePending ? Finish() : false;
	}

private:
	struct FObserver
	{
		FVector Position = FVector::ZeroVector;
		FVector Target = FVector::ZeroVector;
		FVector ContextPoint = FVector::ZeroVector;
		FBox EffectBounds = FBox(ForceInit);
		TArray<FVector> ColumnPoints;
	};

	FAutomationTestBase& Test;
	FSelectedScene Scene;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ADublinCityWorld> City;
	TWeakObjectPtr<APlayerController> Player;
	TWeakObjectPtr<AActor> PreviousViewTarget;
	TWeakObjectPtr<ACameraActor> Camera;
	FObserver GroundObserver;
	FObserver WaterObserver;
	const FObserver* CurrentObserver = nullptr;
	TArray<TSharedPtr<FJsonValue>> Captures;
	TArray<TSharedPtr<FJsonValue>> CannonAges;
	TArray<TSharedPtr<FJsonValue>> Errors;
	TSharedPtr<FJsonObject> PendingCapture;
	FString Directory;
	FString RunName;
	FString PendingPath;
	double StartedWall;
	double LastWorldTime = 0;
	double PhaseStart = 0;
	double SmokeStart = 0;
	double WaterStart = 0;
	double NextCannon = 0;
	double LastCannon = 0;
	double CaptureWaitWall = 0;
	double SmokeCheckAge = 0;
	int32 Phase = 0;
	int32 Frames = 0;
	int32 WaterCapture = 0;
	int32 CannonImpacts = 0;
	int32 BombComponentsAt12 = 0;
	bool bStarted = false;
	bool bFailed = false;
	bool bFinished = false;
	bool bCleaned = false;
	bool bCapturePending = false;
	bool bSmokeChecked = false;
	bool bWaterExpired = false;

	void Fail(const FString& Message)
	{
		bFailed = true;
		Test.AddError(Message);
		Errors.Add(MakeShared<FJsonValueString>(Message));
	}
	void BeginPhase(int32 Next)
	{
		Phase = Next;
		Frames = 0;
		PhaseStart = World->GetTimeSeconds();
	}
	int32 LiveBombComponents() const
	{
		int32 Count = 0;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			if (It->GetWorld() == World.Get() && It->IsActive() && It->GetAsset()
				&& It->GetAsset()->GetPathName().StartsWith(TEXT("/Game/FX/NS_BombExplosion."))) { ++Count; }
		}
		return Count;
	}
	void AimCamera(const FVector& Target, const FVector& Position)
	{
		Camera->SetActorLocationAndRotation(Position, (Target - Position).Rotation());
		Player->SetViewTarget(Camera.Get());
		Player->PlayerCameraManager->UpdateCamera(0.0f);
	}
	void UseObserver(const FObserver& Observer)
	{
		CurrentObserver = &Observer;
		AimCamera(Observer.Target, Observer.Position);
	}
	bool ClearSight(const FVector& Position, const FVector& Point) const
	{
		FHitResult Hit;
		return !Trace(*World.Get(), Position, Point, Hit)
			|| (!Hit.bStartPenetrating && Hit.ImpactPoint.Equals(Point, 25.0));
	}
	bool ClearCamera(const FVector& Position, const FBox& EffectBounds) const
	{
		if (EffectBounds.ExpandBy(300.0).IsInsideOrOn(Position)
			|| FMath::Abs(Position.X) > 38300.0 || FMath::Abs(Position.Y) > 38300.0) { return false; }
		for (const FBox& Building : Scene.Bounds)
		{
			if (Building.ExpandBy(100.0).IsInsideOrOn(Position)) { return false; }
		}
		FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinVisualObserver), false);
		Query.AddIgnoredActor(Camera.Get());
		if (Player->GetPawn()) { Query.AddIgnoredActor(Player->GetPawn()); }
		if (World->OverlapBlockingTestByChannel(Position, FQuat::Identity, ECC_Visibility,
			FCollisionShape::MakeSphere(100.0f), Query)) { return false; }
		FHitResult Hit;
		// A collision-free point below a roof/terrain surface is not a usable exterior camera.
		return !Trace(*World.Get(), Position + FVector(0, 0, 30000), Position, Hit)
			&& Trace(*World.Get(), Position, Position - FVector(0, 0, 30000), Hit)
			&& Hit.Distance > 100.0f;
	}
	TArray<FVector> ContextPoints(const FDublinImpact& Impact) const
	{
		TArray<FVector> Points;
		if (!Impact.bWater)
		{
			TArray<int32> Nearest = Scene.Sorted;
			Nearest.Sort([this, &Impact](int32 A, int32 B)
			{
				return FVector::DistSquaredXY(Scene.Bounds[A].GetCenter(), Impact.PositionCm)
					< FVector::DistSquaredXY(Scene.Bounds[B].GetCenter(), Impact.PositionCm);
			});
			for (int32 Index : Nearest)
			{
				if (FVector::DistXY(Scene.Bounds[Index].GetCenter(), Impact.PositionCm) > 12000.0 || Points.Num() >= 16) { break; }
				FVector Roof;
				if (Scene.RoofProbe(*City.Get(), Index, Roof)) { Points.Add(Roof + FVector(0, 0, 50)); }
			}
		}
		else
		{
			for (double Radius : { 1600.0, 3200.0, 4800.0, 6400.0 })
			{
				for (int32 Bearing = 0; Bearing < 16; ++Bearing)
				{
					const double Angle = Bearing * PI / 8.0;
					const FVector Point = Impact.PositionCm + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;
					float WaterZ;
					if (DublinDestruction::SourceWaterZ(Scene.Source->Water, Point, WaterZ)) { continue; }
					FHitResult Hit;
					if (Trace(*World.Get(), Point + FVector(0, 0, 10000), Point - FVector(0, 0, 2000), Hit)
						&& Hit.GetActor() == City.Get() && Hit.GetComponent()
						&& Hit.GetComponent()->GetName().StartsWith(TEXT("DublinTerrain_")))
					{
						Points.Add(Hit.ImpactPoint + FVector(0, 0, 50));
					}
				}
			}
		}
		return Points;
	}
	bool SelectObserver(const FDublinImpact& Impact, FObserver& Out)
	{
		const DublinImpactFX::FSmokeColumn Smoke = DublinImpactFX::MakeSmokeColumn(Impact, 0);
		const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Impact.Normal.GetSafeNormal());
		Out.EffectBounds = DublinImpactFX::EffectBounds(Impact, Smoke).TransformBy(
			FTransform(Rotation, Impact.PositionCm + Impact.Normal.GetSafeNormal() * 8.0));
		const double ColumnHeight = Smoke.IsEnabled()
			? Smoke.VelocityCmPerSecond.Z * DublinImpactFX::SmokeParticleLifetimeSeconds + Smoke.RadiusCm * 4.0
			: Out.EffectBounds.Max.Z - Impact.PositionCm.Z;
		Out.Target = Impact.PositionCm + FVector(0, 0, ColumnHeight * 0.6);
		for (int32 Level = 0; Level <= 4; ++Level)
		{
			Out.ColumnPoints.Add(Impact.PositionCm + FVector(0, 0, ColumnHeight * Level / 4.0));
		}
		if (Smoke.IsEnabled())
		{
			Out.ColumnPoints.Add(Impact.PositionCm + Smoke.VelocityCmPerSecond * DublinImpactFX::SmokeParticleLifetimeSeconds);
		}
		const TArray<FVector> Context = ContextPoints(Impact);
		const UCameraComponent* Lens = Camera->GetCameraComponent();
		const FIntPoint Size = World->GetGameViewport()->Viewport->GetSizeXY();
		const double Aspect = Lens->bConstrainAspectRatio ? Lens->AspectRatio : static_cast<double>(Size.X) / Size.Y;
		const double TanHorizontal = FMath::Tan(FMath::DegreesToRadians(Lens->FieldOfView * 0.5));
		const double TanVertical = TanHorizontal / Aspect;
		int32 CameraRejected = 0;
		int32 ColumnRejected = 0;
		int32 ContextRejected = 0;
		for (double Distance : { 6500.0, 8500.0, 11000.0, 14000.0 })
		{
			for (double DownDegrees : { 32.5, 25.0, 40.0 })
			{
				for (int32 Bearing = 0; Bearing < 16; ++Bearing)
				{
					const double Angle = Bearing * PI / 8.0;
					const FVector Position = Out.Target + FVector(FMath::Cos(Angle) * Distance,
						FMath::Sin(Angle) * Distance, Distance * FMath::Tan(FMath::DegreesToRadians(DownDegrees)));
					if (!ClearCamera(Position, Out.EffectBounds)) { ++CameraRejected; continue; }
					const FQuat View = (Out.Target - Position).Rotation().Quaternion();
					const auto Framed = [Position, View, TanHorizontal, TanVertical](const FVector& Point, bool bColumn)
					{
						const FVector Local = View.UnrotateVector(Point - Position);
						if (Local.X <= 0.0) { return false; }
						const double X = 0.5 + Local.Y / (2.0 * Local.X * TanHorizontal);
						const double Y = 0.5 - Local.Z / (2.0 * Local.X * TanVertical);
						// Keep the column below the HUD's top panels/warnings, with ground visible below it.
						return X >= 0.1 && X <= 0.9 && Y >= (bColumn ? 0.27 : 0.08) && Y <= 0.82;
					};
					bool bColumnClear = true;
					for (const FVector& Point : Out.ColumnPoints)
					{
						if (!Framed(Point, true) || !ClearSight(Position, Point)) { bColumnClear = false; break; }
					}
					if (!bColumnClear) { ++ColumnRejected; continue; }
					for (const FVector& Point : Context)
					{
						if (!Framed(Point, false) || !ClearSight(Position, Point)) { continue; }
						Out.Position = Position;
						Out.ContextPoint = Point;
						Test.AddInfo(FString::Printf(TEXT("Selected %s oblique camera: pitch=-%.1f, distance=%.0fcm, position=%s. "
							"Original impact base, upward column and real %s have clear sightlines; camera is outside FX/mesh bounds."),
							Impact.bWater ? TEXT("water") : TEXT("ground"), DownDegrees, Distance, *Position.ToString(),
							Impact.bWater ? TEXT("dry riverbank") : TEXT("city roof")));
						return true;
					}
					++ContextRejected;
				}
			}
		}
		Fail(FString::Printf(TEXT("No eligible 25-40 degree oblique %s view at original impact %s: context points=%d, "
			"camera/FX/mesh rejects=%d, column framing/LOS rejects=%d, context rejects=%d. No nadir fallback or relocated impact."),
			Impact.bWater ? TEXT("water") : TEXT("ground"), *Impact.PositionCm.ToString(), Context.Num(),
			CameraRejected, ColumnRejected, ContextRejected));
		return false;
	}
	bool ValidateObserver()
	{
		if (!CurrentObserver) { Fail(TEXT("No selected oblique observer.")); return false; }
		const FObserver& View = *CurrentObserver;
		FVector Position;
		FRotator Rotation;
		Player->GetPlayerViewPoint(Position, Rotation);
		const double DownDegrees = -FRotator::NormalizeAxis(Rotation.Pitch);
		if (DownDegrees < 24.99 || DownDegrees > 40.01 || !ClearCamera(Position, View.EffectBounds))
		{
			Fail(TEXT("Actual player camera is not a clear exterior 25-40 degree oblique view."));
			return false;
		}
		ULocalPlayer* Local = Player->GetLocalPlayer();
		FSceneViewProjectionData Projection;
		if (!Local || !Local->GetProjectionData(World->GetGameViewport()->Viewport, Projection, INDEX_NONE))
		{
			Fail(TEXT("Oblique observer has no actual viewport projection."));
			return false;
		}
		const FIntRect Rect = Projection.GetConstrainedViewRect();
		if (Rect.Width() <= 0 || Rect.Height() <= 0)
		{
			Fail(TEXT("Oblique observer has an empty constrained viewport."));
			return false;
		}
		TArray<FVector> Points = View.ColumnPoints;
		Points.Add(View.ContextPoint);
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			FVector2D Pixel;
			const bool bProjected = FSceneView::ProjectWorldToScreen(Points[Index], Rect, Projection.ComputeViewProjectionMatrix(), Pixel);
			const bool bColumn = Index < View.ColumnPoints.Num();
			if (!bProjected || Pixel.ContainsNaN()
				|| Pixel.X < Rect.Min.X + Rect.Width() * 0.1 || Pixel.X > Rect.Min.X + Rect.Width() * 0.9
				|| Pixel.Y < Rect.Min.Y + Rect.Height() * (bColumn ? 0.27 : 0.08) || Pixel.Y > Rect.Min.Y + Rect.Height() * 0.82
				|| !ClearSight(Position, Points[Index]))
			{
				Fail(FString::Printf(TEXT("Actual oblique view lost clear, framed %s probe %d at %s; no screenshot accepted."),
					bColumn ? TEXT("original impact/column") : TEXT("city/riverbank"), Index, *Points[Index].ToString()));
				return false;
			}
		}
		return true;
	}
	bool Bootstrap()
	{
		if (!GEngine) { Fail(TEXT("No engine for rendered visual fixture.")); return Finish(); }
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (!Candidate || Candidate->WorldType != EWorldType::PIE || Candidate->bIsTearingDown) { continue; }
			World = Candidate;
			if (!IsDublinPIE(Candidate)) { Fail(TEXT("Visual fixture requires saved Dublin PIE.")); return Finish(); }
			City = FindCity(*Candidate);
			Player = Candidate->GetFirstPlayerController();
			const ADublinFlightPawn* Pawn = Player.IsValid() ? Cast<ADublinFlightPawn>(Player->GetPawn()) : nullptr;
			UGameViewportClient* Client = Candidate->GetGameViewport();
			if (!City.IsValid() || !City->bCityReady || !City->bDestructionReady || !Player.IsValid()
				|| !Player->IsLocalController() || !Player->PlayerCameraManager || !Pawn || !Pawn->bSpawnCaptured
				|| !Pawn->Weapons || !Client || !Client->Viewport
				|| Client->Viewport->GetSizeXY().X <= 0 || Client->Viewport->GetSizeXY().Y <= 0) { continue; }
			for (TObjectIterator<UNiagaraSystem> It; It; ++It)
			{
				const FString Path = It->GetPathName();
				if ((Path.StartsWith(TEXT("/Game/FX/NS_BombExplosion.")) || Path.StartsWith(TEXT("/Game/FX/NS_WaterImpact.")))
					&& It->HasOutstandingCompilationRequests(true))
				{
					It->PollForCompilationComplete();
					return false;
				}
			}
			if (City->AcceptedImpactCount != 0 || City->FracturedBuildingCount != 0 || City->CraterChangedNodeCount != 0
				|| City->WaterActiveNodeCount != 0 || City->QueuedImpactCount != 0 || ActiveImpactEffects(*Candidate) != 0)
			{
				Fail(TEXT("Visual fixture requires fresh PIE without prior impacts, craters, waves or impact effects."));
				return Finish();
			}
			if (!Scene.Initialize(*City.Get())) { Fail(Scene.Error); return Finish(); }
			if (!IFileManager::Get().MakeDirectory(*Directory, true)) { Fail(TEXT("Could not create visual evidence directory.")); return Finish(); }
			PreviousViewTarget = Player->GetViewTarget();
			if (!PreviousViewTarget.IsValid()) { Fail(TEXT("Player has no view target to restore.")); return Finish(); }
			SuppressPlayer(*Candidate);
			Camera = Candidate->SpawnActor<ACameraActor>();
			if (!Camera.IsValid()) { Fail(TEXT("Could not spawn owned visual observer camera.")); return Finish(); }
			Camera->GetCameraComponent()->SetFieldOfView(65.0f);
			FDublinImpact WaterCannon = DublinWeapons::MakeImpact(EDublinImpactKind::Cannon, 0, DublinWeapons::FBombCurve());
			WaterCannon.PositionCm = Scene.WaterBomb.PositionCm;
			WaterCannon.bWater = true;
			if (!SelectObserver(Scene.GroundBomb, GroundObserver) || !SelectObserver(WaterCannon, WaterObserver)) { return Finish(); }
			UseObserver(GroundObserver);
			LastWorldTime = Candidate->GetTimeSeconds();
			bStarted = true;
			BeginPhase(0);
			Test.AddInfo(TEXT("SmokeAndCannonWaterVisuals uses accepted native impact dispatch, NOT physical gun input. "
				"Live Niagara components are route evidence, not rendered proof; all screenshots require parent visual inspection."));
			return false;
		}
		return false;
	}
	bool Capture(const TCHAR* Label, double TargetAge, double Start)
	{
		const double Age = World->GetTimeSeconds() - Start;
		if (Age < TargetAge) { return false; }
		const double Wall = FPlatformTime::Seconds();
		if (CaptureWaitWall == 0) { CaptureWaitWall = Wall; }
		if (bCapturePending)
		{
			if (!FScreenshotRequest::IsScreenshotRequested() && IFileManager::Get().FileSize(*PendingPath) > 0)
			{
				PendingCapture->SetBoolField(TEXT("file_written"), true);
				PendingCapture->SetNumberField(TEXT("observed_file_world_age_seconds"), Age);
				Test.AddInfo(TEXT("Viewport image written; parent inspection required: ") + PendingPath);
				bCapturePending = false;
				PendingCapture.Reset();
				CaptureWaitWall = 0;
				return true;
			}
			if (Wall - CaptureWaitWall > 10.0)
			{
				bCapturePending = false;
				Fail(TEXT("Queued viewport screenshot did not produce a nonempty file within 10 seconds: ") + PendingPath);
			}
			return false;
		}
		if (FScreenshotRequest::IsScreenshotRequested())
		{
			if (Wall - CaptureWaitWall > 5.0) { Fail(TEXT("Another screenshot request remained pending for 5 seconds; it was not overwritten.")); }
			return false;
		}
		if (!ValidateObserver()) { return false; }
		PendingPath = FPaths::Combine(Directory, RunName + TEXT("-") + Label + TEXT(".png"));
		PendingCapture = MakeShared<FJsonObject>();
		PendingCapture->SetStringField(TEXT("path"), PendingPath);
		PendingCapture->SetNumberField(TEXT("target_world_age_seconds"), TargetAge);
		PendingCapture->SetNumberField(TEXT("requested_world_age_seconds"), Age);
		PendingCapture->SetNumberField(TEXT("live_bomb_components"), LiveBombComponents());
		PendingCapture->SetNumberField(TEXT("live_water_components"), ActiveImpactEffects(*World.Get(), true));
		PendingCapture->SetBoolField(TEXT("file_written"), false);
		PendingCapture->SetBoolField(TEXT("visual_inspection_required"), true);
		SetPoint(*PendingCapture, TEXT("camera_position_cm"), Camera->GetActorLocation());
		SetPoint(*PendingCapture, TEXT("camera_direction"), Camera->GetActorForwardVector());
		PendingCapture->SetNumberField(TEXT("camera_down_degrees"), -Camera->GetActorRotation().Pitch);
		PendingCapture->SetBoolField(TEXT("impact_column_context_los_and_framing_checked"), true);
		SetPoint(*PendingCapture, TEXT("original_impact_cm"), CurrentObserver->ColumnPoints[0]);
		SetPoint(*PendingCapture, TEXT("column_top_cm"), CurrentObserver->ColumnPoints[4]);
		SetPoint(*PendingCapture, TEXT("city_or_riverbank_context_probe_cm"), CurrentObserver->ContextPoint);
		SetPoint(*PendingCapture, TEXT("excluded_effect_bounds_min_cm"), CurrentObserver->EffectBounds.Min);
		SetPoint(*PendingCapture, TEXT("excluded_effect_bounds_max_cm"), CurrentObserver->EffectBounds.Max);
		Captures.Add(MakeShared<FJsonValueObject>(PendingCapture));
		CaptureWaitWall = Wall;
		bCapturePending = true;
		FScreenshotRequest::RequestScreenshot(PendingPath, false, false);
		Test.AddInfo(FString::Printf(TEXT("Queued %s at actual age %.3fs (target %.1fs): %s"), Label, Age, TargetAge, *PendingPath));
		return false;
	}
	void Cleanup()
	{
		if (bCleaned) { return; }
		bCleaned = true;
		if (Player.IsValid() && PreviousViewTarget.IsValid())
		{
			Player->SetViewTarget(PreviousViewTarget.Get());
			if (Player->PlayerCameraManager) { Player->PlayerCameraManager->UpdateCamera(0.0f); }
		}
		if (Camera.IsValid()) { Camera->Destroy(); }
		if (GEditor && World.IsValid() && GEditor->PlayWorld == World.Get()) { GEditor->RequestEndPlayMap(); }
	}
	bool Finish()
	{
		if (bFinished) { return true; }
		bFinished = true;
		TSharedRef<FJsonObject> Result = City.IsValid() ? Scene.Describe() : MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("test"), TEXT("DublinFlight.Effects.PIE.SmokeAndCannonWaterVisuals"));
		Result->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
		Result->SetStringField(TEXT("status"), bFailed ? TEXT("failed") : TEXT("native_checks_passed_visual_inspection_required"));
		Result->SetStringField(TEXT("scope"), TEXT("Accepted cannon-impact route, NOT physical gun input. Live component != rendered proof."));
		Result->SetBoolField(TEXT("visual_inspection_required"), true);
		Result->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		Result->SetNumberField(TEXT("bomb_components_at_12s"), BombComponentsAt12);
		Result->SetNumberField(TEXT("actual_smoke_check_age_seconds"), SmokeCheckAge);
		Result->SetNumberField(TEXT("accepted_cannon_water_impacts"), CannonImpacts);
		Result->SetBoolField(TEXT("transient_water_components_expired"), bWaterExpired);
		Result->SetArrayField(TEXT("cannon_impact_ages_seconds"), CannonAges);
		Result->SetArrayField(TEXT("screenshots"), Captures);
		Result->SetArrayField(TEXT("errors"), Errors);
		Result->SetObjectField(TEXT("world_diagnostics"), WorldDiagnostics(World.Get(), City.Get()));
		const FString Path = FPaths::Combine(Directory, RunName + TEXT(".json"));
		if (!IFileManager::Get().MakeDirectory(*Directory, true)
			|| !FFileHelper::SaveStringToFile(DiagnosticText(Result), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(TEXT("Could not persist smoke/water visual evidence JSON: ") + Path);
		}
		else { Test.AddInfo(TEXT("Visual evidence manifest: ") + Path); }
		Cleanup();
		return true;
	}
};

enum class EVerificationScope : uint8 { ActualCityDamage, TerrainAndWater };

class FActualCityDamage final : public IAutomationLatentCommand
{
public:
	explicit FActualCityDamage(FAutomationTestBase& InTest,
		EVerificationScope InScope = EVerificationScope::ActualCityDamage, bool bRequireRecoveredBake = false)
		: Test(InTest), VerifyScope(InScope), StartedWall(FPlatformTime::Seconds())
	{
		Scene.bRequireRecoveredBake = bRequireRecoveredBake;
		PersistProgress(TEXT("bootstrapping"));
	}
	virtual ~FActualCityDamage() override
	{
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
		Cleanup();
	}

	virtual bool Update() override
	{
		if (bFailed) { return Finish(); }
		if (!bStarted) { return Bootstrap(); }
		if (bEnded || !World.IsValid() || !City.IsValid() || !IsDublinPIE(World.Get()))
		{
			Fail(TEXT("PIE ended during physical destruction verification."));
			return Finish();
		}
		if (FPlatformTime::Seconds() - StartedWall > 120)
		{
			TSharedRef<FJsonObject> State = WorldDiagnostics(World.Get(), City.Get());
			State->SetNumberField(TEXT("engine_frames_since_fixture_created"), static_cast<double>(GFrameCounter - StartedEngineFrame));
			State->SetNumberField(TEXT("phase_advancing_world_observations"), Frames);
			Fail(FString::Printf(TEXT("Physical destruction verification timed out in phase %d. "
				"Physics requires advancing world frames, NOT OS focus. State=%s"), Phase, *DiagnosticText(State)));
			return Finish();
		}
		if (!WithinBudgets(*City.Get()))
		{
			Fail(TEXT("Runtime queue blocked or exceeded its real activation budgets: ") + City->LastDestructionError);
			return Finish();
		}
		const double Now = World->GetTimeSeconds();
		if (Now <= LastWorldTime) { return false; }
		LastWorldTime = Now;
		++Frames;
		const double Elapsed = Now - PhaseStart;
		if (Phase == 0 && Elapsed >= 1.0 && Frames >= 2)
		{
			CheckCannonLocality();
			if (!bFailed) { BeginPhase(1); }
		}
		else if (Phase == 1)
		{
			if (RepeatedShots < 3 && Elapsed >= RepeatedShots * 0.3)
			{
				FDublinImpact Impact = Scene.Cannon;
				Impact.Seed += ++RepeatedShots;
				Require(TEXT("Repeated native cannon impact accepted"), SubmitImpact(*City.Get(), Impact));
			}
			if (RepeatedShots == 3 && Elapsed >= 1.5 && City->QueuedBuildingHitCount == 0)
			{
				if (UGeometryCollectionComponent* Component = TargetCollection(*City.Get(), Scene))
				{
					BombBaseline = WorldLeaves(*Component);
					PreviousLeaves = BombBaseline;
					const TArray<FTransform> Rest = Component->GetInitialLocalRestTransforms();
					for (int32 I = 0; I < BombBaseline.Num(); ++I)
					{
						const int32 Leaf = Scene.Record.LeafTransforms[I];
						if (Scene.Record.Anchors.Contains(Leaf) && Rest.IsValidIndex(Leaf)
							&& Component->GetComponentTransform().TransformPosition(Rest[Leaf].GetLocation()).Equals(BombBaseline[I], 5))
						{
							SupportedBeforeBomb.Add(I);
						}
					}
				}
				Require(TEXT("A real leaf baseline exists before support release"), BombBaseline.Num() == Scene.Record.PieceCount);
				Require(TEXT("Repeated local cannon leaves at least one authored support in its rest position"), SupportedBeforeBomb.Num() > 0);
				Require(TEXT("Game-tuned support bomb accepted"), SubmitImpact(*City.Get(), Scene.SupportBomb));
				BeginPhase(2);
			}
		}
		else if (Phase == 2)
		{
			ObserveMotion();
			if (Elapsed >= 8.0 && Frames >= 10)
			{
				Require(TEXT("Actual GC leaves moved at least 25cm after the support bomb"), MaxLeafTravel > 25 && MovedLeaves.Num() > 0);
				Require(TEXT("Actual GC leaves fell at least 25cm below their pre-bomb world positions"), MaxLeafDrop > 25 && FallenLeaves.Num() > 0);
				Require(TEXT("Leaf-pair distances change: separated fragments, not just a falling rigid building"), MaxPairDistanceChange > 25);
				Require(TEXT("The bomb actually moves a previously stationary authored support leaf"), ReleasedSupportLeaves.Num() > 0);
				Require(TEXT("Downward fragment motion was observed across multiple independent world frames"), DescendingFrames >= 2);
				Require(TEXT("Selected impact queue drained without discarding pending work"), City->QueuedBuildingHitCount == 0);
				if (!bFailed)
				{
					Require(TEXT("Real dry-ground bomb accepted"), SubmitImpact(*City.Get(), Scene.GroundBomb));
					BeginPhase(3);
				}
			}
		}
		else if (Phase == 3 && Elapsed >= 1.0 && Frames >= 2)
		{
			CheckCrater();
			++CraterObservations;
			if (!bFailed)
			{
				float Near = 0, Far = 0;
				Require(TEXT("Water begins at its exact source height before the pulse"),
					City->GetWaterSurfaceZ(Scene.WaterBomb.PositionCm, Near) && City->GetWaterSurfaceZ(Scene.FarWater, Far)
					&& FMath::Abs(Near - Scene.NearWaterBase) < 0.05 && FMath::Abs(Far - Scene.FarWaterBase) < 0.05);
				Require(TEXT("Real water bomb accepted and routed to original impact FX"), SubmitImpact(*City.Get(), Scene.WaterBomb));
				BeginPhase(4);
			}
		}
		else if (Phase == 4)
		{
			ObserveWater(Elapsed);
			if (Elapsed >= 6.0 && Frames >= 10)
			{
				Require(TEXT("Near water surface responds to the actual pulse"), NearPeak > 0.5);
				Require(TEXT("Far probe lies outside pulse radius plus the solver injection cell"),
					FVector::DistXY(Scene.FarWater, Scene.WaterBomb.PositionCm) > Scene.WaterBomb.RadiusCm + 800);
				Require(TEXT("Far water remains initially quiet before propagation"), EarlyFarPeak < 0.05);
				Require(TEXT("Early far-water observation actually sampled an advancing world frame"), EarlyWaterFrames > 0);
				Require(TEXT("Far water later responds, after the near probe"), FarPeak > 0.15 && NearArrival >= 0
					&& FarArrival > NearArrival + 0.1);
				Require(TEXT("Actual water render vertices move consistently with queried surface"), bNearRenderMoved && bFarRenderMoved);
				Require(TEXT("Original water Niagara route produced a live component (not visual proof)"), bWaterEffectSeen);
				if (VerifyScope == EVerificationScope::TerrainAndWater)
				{
					CheckCrater();
					++CraterObservations;
				}
				CheckControl();
				return Finish();
			}
		}
		return bFailed ? Finish() : false;
	}

private:
	FAutomationTestBase& Test;
	const EVerificationScope VerifyScope;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ADublinCityWorld> City;
	FSelectedScene Scene;
	TArray<FVector> BombBaseline;
	TArray<FVector> PreviousLeaves;
	TArray<FVector> LastLeaves;
	TSet<int32> MovedLeaves;
	TSet<int32> FallenLeaves;
	TSet<int32> SupportedBeforeBomb;
	TSet<int32> ReleasedSupportLeaves;
	double StartedWall = 0;
	double LastWorldTime = 0;
	double PhaseStart = 0;
	double MaxLeafTravel = 0;
	double MaxLeafDrop = 0;
	double MaxPairDistanceChange = 0;
	double CraterDepth = 0;
	double NearPeak = 0;
	double FarPeak = 0;
	double EarlyFarPeak = 0;
	double NearArrival = -1;
	double FarArrival = -1;
	double BootstrapLastWorldTime = -1;
	uint64 StartedEngineFrame = GFrameCounter;
	int32 BootstrapPolls = 0;
	int32 BootstrapWorldAdvances = 0;
	int32 Phase = 0;
	int32 Frames = 0;
	int32 RepeatedShots = 0;
	int32 DescendingFrames = 0;
	int32 RetainedCannonLeaves = 0;
	int32 EarlyWaterFrames = 0;
	int32 CraterObservations = 0;
	bool bStarted = false;
	bool bFailed = false;
	bool bEnded = false;
	bool bCleaned = false;
	bool bFinished = false;
	bool bWaterEffectSeen = false;
	bool bNearRenderMoved = false;
	bool bFarRenderMoved = false;
	TArray<TSharedPtr<FJsonValue>> Errors;
	TSharedRef<FJsonObject> BootstrapState = WorldDiagnostics(nullptr, nullptr);

	void Fail(const FString& Message)
	{
		bFailed = true;
		Test.AddError(Message);
		Errors.Add(MakeShared<FJsonValueString>(Message));
	}
	void Require(const TCHAR* Message, bool Condition) { if (!Condition) { Fail(Message); } }
	void OnPIEEnded(bool bSimulating) { bEnded = true; Cleanup(); }
	void Cleanup()
	{
		if (bCleaned) { return; }
		bCleaned = true;
		if (World.IsValid() && !World->bIsTearingDown) { SuppressPlayer(*World.Get()); }
	}
	void BeginPhase(int32 Next)
	{
		Phase = Next;
		Frames = 0;
		PhaseStart = World->GetTimeSeconds();
		PersistProgress(TEXT("running"));
	}

	const TCHAR* EvidenceField() const
	{
		if (Scene.bRequireRecoveredBake) { return TEXT("last_recovered_building_verification"); }
		return VerifyScope == EVerificationScope::TerrainAndWater
			? TEXT("last_terrain_water_verification") : TEXT("last_runtime_verification");
	}

	void DescribeScope(FJsonObject& Result) const
	{
		const bool TerrainAndWaterOnly = VerifyScope == EVerificationScope::TerrainAndWater;
		Result.SetStringField(TEXT("test"), TerrainAndWaterOnly
			? TEXT("DublinFlight.Destruction.PIE.TerrainAndWater")
			: Scene.bRequireRecoveredBake ? TEXT("DublinFlight.Destruction.PIE.RecoveredBuildingDamage")
			: TEXT("DublinFlight.Destruction.PIE.ActualCityDamage"));
		Result.SetBoolField(TEXT("requires_recovered_bake"), Scene.bRequireRecoveredBake);
		Result.SetNumberField(TEXT("selected_bake_attempts"), Scene.Record.BakeAttempts);
		Result.SetStringField(TEXT("verification_scope"), TerrainAndWaterOnly ? TEXT("terrain_and_water_only") : TEXT("actual_city_damage"));
		Result.SetBoolField(TEXT("building_damage_in_scope"), !TerrainAndWaterOnly);
		Result.SetStringField(TEXT("building_motion_status"), TerrainAndWaterOnly
			? TEXT("NOT_ASSESSED: no cannon or support bomb submitted; selected buildings are read-only fixture controls")
			: TEXT("See physical leaf observations and errors; inclusion in scope does not imply success"));
		Result.SetStringField(TEXT("acceptance_scope"), TEXT("Selected native physical sample only; not full-city, visual or performance acceptance"));
	}

	void PersistProgress(const TCHAR* Status)
	{
		if (VerifyScope != EVerificationScope::TerrainAndWater) { return; }
		TSharedRef<FJsonObject> Progress = Scene.Describe();
		DescribeScope(*Progress);
		Progress->SetStringField(TEXT("status"), Status);
		Progress->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
		Progress->SetNumberField(TEXT("phase"), bStarted ? Phase : -1);
		Progress->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		Progress->SetNumberField(TEXT("crater_observations"), CraterObservations);
		Progress->SetNumberField(TEXT("crater_depth_cm"), CraterDepth);
		Progress->SetObjectField(TEXT("bootstrap_diagnostics"), BootstrapState);
		Progress->SetObjectField(TEXT("world_diagnostics"), WorldDiagnostics(World.Get(), City.Get()));
		Progress->SetArrayField(TEXT("errors"), Errors);
		if (!SaveEvidence(EvidenceField(), Progress)) { Fail(TEXT("Could not persist terrain/water PIE progress.")); }
	}

	bool Bootstrap()
	{
		++BootstrapPolls;
		bool SawPIEWorld = false;
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (!Candidate || Candidate->WorldType != EWorldType::PIE) { continue; }
				SawPIEWorld = true;
				ADublinCityWorld* CandidateCity = FindCity(*Candidate);
				if (BootstrapLastWorldTime >= 0 && Candidate->GetTimeSeconds() > BootstrapLastWorldTime) { ++BootstrapWorldAdvances; }
				BootstrapLastWorldTime = Candidate->GetTimeSeconds();
				BootstrapState = WorldDiagnostics(Candidate, CandidateCity);
				UpdateBootstrapDiagnostics();
				if (Candidate->bIsTearingDown) { continue; }
				if (!IsDublinPIE(Candidate))
				{
					Fail(TEXT("Physical verification requires /Game/Maps/Dublin PIE. State=") + DiagnosticText(BootstrapState));
					return Finish();
				}
				APlayerController* Player = Candidate->GetFirstPlayerController();
				ADublinFlightPawn* Pawn = Player ? Cast<ADublinFlightPawn>(Player->GetPawn()) : nullptr;
				if (!CandidateCity || !CandidateCity->bCityReady || !Player || !Player->IsLocalController()
					|| !Pawn || !Pawn->bSpawnCaptured || !Pawn->Weapons) { continue; }
				World = Candidate;
				City = CandidateCity;
				if (!CandidateCity->bDestructionReady)
				{
					Fail(TEXT("PIE city is not destruction-ready. State=") + DiagnosticText(BootstrapState));
					return Finish();
				}
				Require(TEXT("Use a fresh PIE session: no prior impacts, craters, waves or fractured buildings"),
					City->AcceptedImpactCount == 0 && City->FracturedBuildingCount == 0
					&& City->CraterChangedNodeCount == 0 && City->WaterActiveNodeCount == 0 && City->QueuedImpactCount == 0);
				if (bFailed) { return Finish(); }
				SuppressPlayer(*Candidate);
				if (!Scene.Initialize(*CandidateCity)) { Fail(Scene.Error); return Finish(); }
				Test.AddInfo(TEXT("Physical-test native bootstrap ready without any OS-focus requirement. State=") + DiagnosticText(BootstrapState));
				Test.AddInfo(FString::Printf(TEXT("Real target=%s control=%s collection=%s; ready=%d/%d, partial=%s; "
					"support bomb=%.2f game tons, radius=%.1fcm. Coarse Voronoi volumes, NOT engineered structural analysis."),
					*Scene.Source->Buildings[Scene.Target].Id, *Scene.Source->Buildings[Scene.Control].Id,
					*Scene.Record.Collection.ToSoftObjectPath().ToString(), City->FractureReadyBuildingCount,
					City->BuildingCount, City->bAllowPartialBakePreview ? TEXT("true") : TEXT("false"),
					Scene.SupportBomb.YieldTonsTNT, Scene.SupportBomb.RadiusCm));
				FEditorDelegates::PrePIEEnded.AddRaw(this, &FActualCityDamage::OnPIEEnded);
				bStarted = true;
				LastWorldTime = Candidate->GetTimeSeconds();
				if (VerifyScope == EVerificationScope::TerrainAndWater)
				{
					Test.AddInfo(TEXT("TerrainAndWater scope: building damage/motion NOT ASSESSED. "
						"Using the same real-map selection, crater and water assertions without cannon/support-bomb phases."));
					Require(TEXT("Real dry-ground bomb accepted"), SubmitImpact(*City.Get(), Scene.GroundBomb));
					BeginPhase(3);
				}
				else
				{
					BeginPhase(0);
					Require(TEXT("Local native cannon hit accepted"), SubmitImpact(*City.Get(), Scene.Cannon));
				}
				return bFailed ? Finish() : false;
			}
		}
		if (!SawPIEWorld) { BootstrapState = WorldDiagnostics(nullptr, nullptr); }
		UpdateBootstrapDiagnostics();
		if (FPlatformTime::Seconds() - StartedWall >= BootstrapTimeout)
		{
			Fail(TEXT("Native Dublin PIE bootstrap timed out after 60s. Require a possessed ready pawn/city, "
				"not OS focus; few engine/world frames can indicate a stalled startup or compilation. State=") + DiagnosticText(BootstrapState));
			return Finish();
		}
		return false;
	}

	void UpdateBootstrapDiagnostics()
	{
		BootstrapState->SetNumberField(TEXT("bootstrap_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		BootstrapState->SetNumberField(TEXT("bootstrap_poll_count"), BootstrapPolls);
		BootstrapState->SetNumberField(TEXT("engine_frames_since_fixture_created"), static_cast<double>(GFrameCounter - StartedEngineFrame));
		BootstrapState->SetNumberField(TEXT("observed_world_time_advances"), BootstrapWorldAdvances);
	}

	TArray<FVector> WorldLeaves(UGeometryCollectionComponent& Component)
	{
		TArray<FVector> Out;
		if (!Component.GetPhysicsProxy() || !Component.IsPhysicsStateCreated()
			|| !Component.IsVisible() || !Component.IsRenderStateCreated()
			|| Component.GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics) { return Out; }
		const TArray<FTransform> Current = Component.GetCurrentTransforms();
		for (int32 Leaf : Scene.Record.LeafTransforms)
		{
			if (!Current.IsValidIndex(Leaf) || Current[Leaf].ContainsNaN()
				|| Current[Leaf].GetScale3D().GetAbsMin() < 0.01) { Out.Reset(); return Out; }
			const FVector Position = Component.GetComponentTransform().TransformPosition(Current[Leaf].GetLocation());
			if (Position.ContainsNaN()) { Out.Reset(); return Out; }
			Out.Add(Position);
		}
		return Out;
	}

	void CheckCannonLocality()
	{
		UGeometryCollectionComponent* Component = TargetCollection(*City.Get(), Scene);
		Require(TEXT("Real baked GC replaced only the selected intact target and owns native physics"), Component != nullptr);
		if (!Component) { return; }
		const TArray<FVector> Current = WorldLeaves(*Component);
		Require(TEXT("Local cannon retains all authored physical leaf transforms"), Current.Num() == Scene.Record.PieceCount);
		if (Current.Num() != Scene.Record.PieceCount) { return; }
		const TArray<FTransform> Initial = Component->GetInitialLocalRestTransforms();
		for (int32 I = 0; I < Scene.Record.LeafTransforms.Num(); ++I)
		{
			const int32 Leaf = Scene.Record.LeafTransforms[I];
			if (!Initial.IsValidIndex(Leaf)) { continue; }
			const FVector Rest = Component->GetComponentTransform().TransformPosition(Initial[Leaf].GetLocation());
			if (FVector::Dist(Rest, Scene.Cannon.PositionCm) > Scene.Cannon.RadiusCm * 2
				&& FVector::Dist(Rest, Current[I]) < 5) { ++RetainedCannonLeaves; }
		}
		Require(TEXT("Cannon radius leaves at least two remote pieces in place rather than erasing the whole building"),
			RetainedCannonLeaves >= 2);
		bool HasRetainedCollision = false;
		const FDublinCityBuilding& Building = Scene.Source->Buildings[Scene.Target];
		for (int32 T = 0; T < Building.MaterialIds.Num() && !HasRetainedCollision; ++T)
		{
			if (Building.MaterialIds[T] != 0) { continue; }
			FVector Roof = Building.PivotCm;
			Roof += (Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]]
				+ Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]]
				+ Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]]) / 3;
			if (FVector::DistXY(Roof, Scene.Cannon.PositionCm) < Scene.Cannon.RadiusCm * 2) { continue; }
			FHitResult Hit;
			HasRetainedCollision = Trace(*World.Get(), Roof + FVector(0, 0, 1000), Roof - FVector(0, 0, 300), Hit)
				&& Hit.GetComponent() == Component && Scene.Bounds[Scene.Target].ExpandBy(100).IsInsideOrOn(Hit.ImpactPoint);
		}
		Require(TEXT("An unhit part of the actual GC still answers a world collision ray"), HasRetainedCollision);
		CheckControl();
	}

	void ObserveMotion()
	{
		UGeometryCollectionComponent* Component = TargetCollection(*City.Get(), Scene);
		if (!Component) { Fail(TEXT("Selected GC disappeared after the bomb.")); return; }
		const TArray<FVector> Current = WorldLeaves(*Component);
		if (Current.Num() != BombBaseline.Num() || Current.IsEmpty())
		{
			Fail(TEXT("Bomb destroyed/invalidated the physical leaf data instead of simulating retained pieces."));
			return;
		}
		bool Descending = false;
		for (int32 I = 0; I < Current.Num(); ++I)
		{
			const double Travel = FVector::Dist(Current[I], BombBaseline[I]);
			const double Drop = BombBaseline[I].Z - Current[I].Z;
			MaxLeafTravel = FMath::Max(MaxLeafTravel, Travel);
			MaxLeafDrop = FMath::Max(MaxLeafDrop, Drop);
			if (Travel > 25) { MovedLeaves.Add(I); }
			if (Travel > 25 && SupportedBeforeBomb.Contains(I)) { ReleasedSupportLeaves.Add(I); }
			if (Drop > 25) { FallenLeaves.Add(I); }
			if (PreviousLeaves.IsValidIndex(I) && PreviousLeaves[I].Z - Current[I].Z > 1) { Descending = true; }
			for (int32 J = 0; J < I; ++J)
			{
				MaxPairDistanceChange = FMath::Max(MaxPairDistanceChange,
					FMath::Abs(FVector::Dist(Current[I], Current[J]) - FVector::Dist(BombBaseline[I], BombBaseline[J])));
			}
		}
		if (Descending) { ++DescendingFrames; }
		PreviousLeaves = Current;
		LastLeaves = Current;
	}

	void CheckCrater()
	{
		const FDublinCityChunk& Chunk = City->GetTerrainChunks()[Scene.GroundChunk];
		const FDublinCitySection& Section = Chunk.Sections[0];
		FVector Rendered = Chunk.OriginCm;
		for (int32 I = 0; I < 3; ++I) { Rendered += Section.Vertices[Section.Triangles[Scene.GroundTriangle + I]] / 3; }
		CraterDepth = Scene.GroundBefore.Z - Rendered.Z;
		Require(TEXT("Actual terrain chunk height dropped by a plausible game-tuned crater depth"),
			CraterDepth > 100 && FMath::Abs(CraterDepth - Scene.GroundBomb.CraterDepthCm) < 100);
		FHitResult Hit;
		const bool HitTerrain = Trace(*World.Get(), Scene.GroundBefore + FVector(0, 0, 2000),
			Scene.GroundBefore - FVector(0, 0, 4000), Hit) && Hit.GetComponent() == Scene.GroundMesh.Get();
		Require(TEXT("World ray still hits the deformed land terrain, not a building or river"), HitTerrain);
		Require(TEXT("Actual terrain collision follows rendered crater height within 10cm"),
			HitTerrain && FMath::Abs(Hit.ImpactPoint.Z - Rendered.Z) < 10);
		const FProcMeshSection* Actual = Scene.GroundMesh.IsValid() ? Scene.GroundMesh->GetProcMeshSection(0) : nullptr;
		bool RenderMatches = Actual != nullptr;
		for (int32 I = 0; I < 3 && RenderMatches; ++I)
		{
			const int32 Vertex = Section.Triangles[Scene.GroundTriangle + I];
			RenderMatches = Actual->ProcVertexBuffer.IsValidIndex(Vertex)
				&& Scene.GroundMesh->GetComponentTransform().TransformPosition(Actual->ProcVertexBuffer[Vertex].Position)
					.Equals(Chunk.OriginCm + Section.Vertices[Vertex], 0.1);
		}
		Require(TEXT("The live terrain mesh buffer contains the same crater as retained chunk data"), RenderMatches);
	}

	void ObserveWater(double Elapsed)
	{
		float Near = 0, Far = 0;
		if (!City->GetWaterSurfaceZ(Scene.WaterBomb.PositionCm, Near) || !City->GetWaterSurfaceZ(Scene.FarWater, Far))
		{
			Fail(TEXT("Exact river membership disappeared during water simulation."));
			return;
		}
		const double NearDelta = FMath::Abs(Near - Scene.NearWaterBase), FarDelta = FMath::Abs(Far - Scene.FarWaterBase);
		NearPeak = FMath::Max(NearPeak, NearDelta);
		FarPeak = FMath::Max(FarPeak, FarDelta);
		if (Elapsed <= 0.25) { EarlyFarPeak = FMath::Max(EarlyFarPeak, FarDelta); ++EarlyWaterFrames; }
		if (NearArrival < 0 && NearDelta > 0.15) { NearArrival = Elapsed; }
		if (FarArrival < 0 && FarDelta > 0.15) { FarArrival = Elapsed; }
		bWaterEffectSeen |= ActiveImpactEffects(*World.Get(), true) > 0;
		Require(TEXT("Original impact FX pool remains bounded"), ActiveImpactEffects(*World.Get()) <= DublinImpactFX::MaxSystems);
		const FProcMeshSection* Mesh = Scene.WaterMesh.IsValid() ? Scene.WaterMesh->GetProcMeshSection(0) : nullptr;
		if (!Mesh || !Mesh->ProcVertexBuffer.IsValidIndex(Scene.NearWaterVertex)
			|| !Mesh->ProcVertexBuffer.IsValidIndex(Scene.FarWaterVertex))
		{
			Fail(TEXT("Live river render mesh/probe vertices disappeared."));
			return;
		}
		const FTransform Transform = Scene.WaterMesh->GetComponentTransform();
		const double RenderNear = Transform.TransformPosition(Mesh->ProcVertexBuffer[Scene.NearWaterVertex].Position).Z;
		const double RenderFar = Transform.TransformPosition(Mesh->ProcVertexBuffer[Scene.FarWaterVertex].Position).Z;
		bNearRenderMoved |= NearDelta > 0.5 && FMath::Abs(RenderNear - Near) < 0.5;
		bFarRenderMoved |= FarDelta > 0.15 && FMath::Abs(RenderFar - Far) < 0.5;
	}

	void CheckControl()
	{
		Require(TEXT("Distant unhit building component remains registered and collidable"), Scene.ControlMesh.IsValid()
			&& Scene.ControlMesh->IsRegistered() && Scene.ControlMesh->IsVisible()
			&& Scene.ControlMesh->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
		if (!Scene.ControlMesh.IsValid()) { return; }
		Require(TEXT("Distant unhit building geometry/collision flags and transform remain byte-for-byte unchanged"),
			MeshFingerprint(*Scene.ControlMesh.Get()) == Scene.ControlFingerprint
			&& Scene.ControlMesh->GetComponentTransform().Equals(Scene.ControlTransform, 0.01));
		FHitResult Hit;
		Require(TEXT("Distant building roof still answers the same world collision ray at the original height"),
			Trace(*World.Get(), Scene.ControlRoof + FVector(0, 0, 1000), Scene.ControlRoof - FVector(0, 0, 100), Hit)
			&& Hit.GetComponent() == Scene.ControlMesh.Get() && Hit.ImpactPoint.Equals(Scene.ControlRoof, 10));
	}

	bool Finish()
	{
		if (bFinished) { return true; }
		bFinished = true;
		Cleanup();
		TSharedRef<FJsonObject> Result = Scene.Describe();
		Result->SetStringField(TEXT("status"), bFailed ? TEXT("failed") : TEXT("passed"));
		DescribeScope(*Result);
		Result->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
		Result->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		Result->SetNumberField(TEXT("phase"), Phase);
		Result->SetNumberField(TEXT("phase_advancing_world_observations"), Frames);
		Result->SetNumberField(TEXT("engine_frames_since_fixture_created"), static_cast<double>(GFrameCounter - StartedEngineFrame));
		Result->SetObjectField(TEXT("bootstrap_diagnostics"), BootstrapState);
		Result->SetObjectField(TEXT("final_world_diagnostics"), WorldDiagnostics(World.Get(), City.Get()));
		if (VerifyScope == EVerificationScope::ActualCityDamage)
		{
			Result->SetNumberField(TEXT("cannon_retained_remote_leaves"), RetainedCannonLeaves);
			Result->SetNumberField(TEXT("maximum_leaf_travel_cm"), MaxLeafTravel);
			Result->SetNumberField(TEXT("maximum_leaf_drop_cm"), MaxLeafDrop);
			Result->SetNumberField(TEXT("maximum_leaf_pair_distance_change_cm"), MaxPairDistanceChange);
			Result->SetNumberField(TEXT("stationary_supports_before_bomb"), SupportedBeforeBomb.Num());
			Result->SetNumberField(TEXT("supports_physically_moved_after_bomb"), ReleasedSupportLeaves.Num());
			Result->SetNumberField(TEXT("descending_world_frames"), DescendingFrames);
		}
		Result->SetNumberField(TEXT("crater_observations"), CraterObservations);
		Result->SetNumberField(TEXT("crater_depth_cm"), CraterDepth);
		Result->SetNumberField(TEXT("crater_requested_depth_cm"), Scene.GroundBomb.CraterDepthCm);
		Result->SetNumberField(TEXT("near_peak_cm"), NearPeak);
		Result->SetNumberField(TEXT("far_peak_cm"), FarPeak);
		Result->SetNumberField(TEXT("early_far_peak_cm"), EarlyFarPeak);
		Result->SetNumberField(TEXT("early_water_world_frames"), EarlyWaterFrames);
		Result->SetNumberField(TEXT("near_arrival_world_seconds"), NearArrival);
		Result->SetNumberField(TEXT("far_arrival_world_seconds"), FarArrival);
		Result->SetBoolField(TEXT("near_water_render_matches_surface"), bNearRenderMoved);
		Result->SetBoolField(TEXT("far_water_render_matches_surface"), bFarRenderMoved);
		Result->SetBoolField(TEXT("water_niagara_component_seen_not_visual_proof"), bWaterEffectSeen);
		Result->SetArrayField(TEXT("errors"), Errors);
		TArray<TSharedPtr<FJsonValue>> Leaves;
		for (int32 I = 0; I < BombBaseline.Num() && I < LastLeaves.Num() && I < 128; ++I)
		{
			TSharedRef<FJsonObject> Leaf = MakeShared<FJsonObject>();
			Leaf->SetNumberField(TEXT("transform_index"), Scene.Record.LeafTransforms[I]);
			SetPoint(*Leaf, TEXT("pre_bomb_world_cm"), BombBaseline[I]);
			SetPoint(*Leaf, TEXT("last_world_cm"), LastLeaves[I]);
			Leaf->SetBoolField(TEXT("observed_fall_over_25cm"), FallenLeaves.Contains(I));
			Leaf->SetBoolField(TEXT("stationary_authored_support_before_bomb"), SupportedBeforeBomb.Contains(I));
			Leaf->SetBoolField(TEXT("authored_support_moved_over_25cm"), ReleasedSupportLeaves.Contains(I));
			Leaves.Add(MakeShared<FJsonValueObject>(Leaf));
		}
		if (VerifyScope == EVerificationScope::ActualCityDamage) { Result->SetArrayField(TEXT("physical_leaf_observations"), Leaves); }
		if (City.IsValid()) { Result->SetObjectField(TEXT("counters"), Counters(*City.Get())); }
		if (!SaveEvidence(EvidenceField(), Result)) { Test.AddError(TEXT("Could not persist destruction PIE evidence.")); }
		Test.AddInfo(TEXT("Destruction fixture finished; no held inputs or fixture timers remain. "
			"Physics was observed, not manually ticked. Visual quality, weapon trajectory, full-city readiness and FPS are separate gates."));
		return true;
	}
};

enum class EVisibleScenario : uint8 { Idle, Sample, Maximal };

// These fixed developer scenarios do not launch PIE, change assets, execute arbitrary
// commands, reset the performance capture clock, or bypass the city's queues/caps.
class FVisibleScenario final
{
public:
	~FVisibleScenario()
	{
		Stop(TEXT("runner_destroyed"));
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
	}

	bool Start(UWorld* InWorld, EVisibleScenario InKind)
	{
		Kind = InKind;
		World = InWorld;
		if (!IsDublinPIE(InWorld))
		{
			UE_LOG(LogDublinDestructionVerification, Error, TEXT("Verify scenario needs existing stable /Game/Maps/Dublin PIE. State=%s"),
				*DiagnosticText(WorldDiagnostics(InWorld, nullptr)));
			return false;
		}
		City = FindCity(*InWorld);
		if (!City.IsValid() || !City->bDestructionReady || City->AcceptedImpactCount != 0
			|| City->QueuedImpactCount != 0 || City->FracturedBuildingCount != 0)
		{
			UE_LOG(LogDublinDestructionVerification, Error, TEXT("Verify scenario requires a fresh READY city, not OS focus. "
				"Restart PIE between scenarios. State=%s"), *DiagnosticText(WorldDiagnostics(InWorld, City.Get())));
			return false;
		}
		if (Kind == EVisibleScenario::Maximal && (!City->bAllBuildingsFractureReady || City->bAllowPartialBakePreview))
		{
			UE_LOG(LogDublinDestructionVerification, Error, TEXT("Maximal requires ALL saved volumes ready and partial preview OFF; use Verify.Sample for 12-building preview."));
			return false;
		}
		if (!Scene.Initialize(*City.Get()))
		{
			UE_LOG(LogDublinDestructionVerification, Error, TEXT("Verify scenario selection failed: %s"), *Scene.Error);
			return false;
		}
		if (Kind == EVisibleScenario::Sample)
		{
			Sequence = {Scene.Cannon, Scene.Cannon, Scene.Cannon, Scene.SupportBomb, Scene.GroundBomb, Scene.WaterBomb};
		}
		else if (Kind == EVisibleScenario::Maximal)
		{
			TArray<FVector> Centers;
			for (int32 Index : Scene.Sorted)
			{
				const FBox& Box = Scene.Bounds[Index];
				if (Box.GetSize().Z < 500 || Box.GetSize().Z > 10000 || !Scene.Ready[Index]) { continue; }
				FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, DublinWeapons::FBombCurve());
				Impact.PositionCm = FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z + 100);
				Impact.Seed = 58100 + Centers.Num();
				if (!Scene.SafeImpact(Impact) || Centers.ContainsByPredicate([&Impact](const FVector& Center)
					{ return FVector::DistXY(Center, Impact.PositionCm) < 5000; })) { continue; }
				Centers.Add(Impact.PositionCm);
				Sequence.Add(Impact);
				SelectedIds.Add(MakeShared<FJsonValueString>(Scene.Source->Buildings[Index].Id));
				if (Centers.Num() % 3 == 0)
				{
					// Water uses the proven bridge-free patch; its own effect/solver remains budgeted.
					Sequence.Add(Scene.WaterBomb);
				}
				if (Centers.Num() == 24) { break; }
			}
			if (Centers.Num() < 8)
			{
				UE_LOG(LogDublinDestructionVerification, Error, TEXT("Maximal could not select eight separated, in-bounds bomb neighborhoods."));
				return false;
			}
		}
		SuppressPlayer(*InWorld);
		StartedWorld = InWorld->GetTimeSeconds();
		StartedWall = FPlatformTime::Seconds();
		bRunning = true;
		FEditorDelegates::PrePIEEnded.AddRaw(this, &FVisibleScenario::OnPIEEnded);
		Watchdog = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FVisibleScenario::Watch), 0.25f);
		InWorld->GetTimerManager().SetTimer(Timer, FTimerDelegate::CreateRaw(this, &FVisibleScenario::Step), 0.25f, true);
		SampleCounters();
		if (!SaveEvidence(TEXT("last_visible_scenario"), Report(TEXT("running"))))
		{
			Stop(TEXT("evidence_write_failed"));
			return false;
		}
		UE_LOG(LogDublinDestructionVerification, Display, TEXT("Verify.%s started for 30 world seconds (45s wall watchdog). "
			"Target=%s at %s. Camera/PIE stays under coordinator control. Capture FPS independently; this is not an FPS pass."),
			Name(), *Scene.Source->Buildings[Scene.Target].Id, *Scene.SupportBomb.PositionCm.ToString());
		return true;
	}

	void Stop(const TCHAR* Reason, bool bFromWatchdog = false)
	{
		if (!bRunning) { return; }
		bRunning = false;
		if (World.IsValid()) { World->GetTimerManager().ClearTimer(Timer); }
		// RemoveTicker waits for an executing callback; the watchdog removes itself by returning false.
		if (!bFromWatchdog) { FTSTicker::RemoveTicker(Watchdog); }
		Watchdog.Reset();
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
		if (World.IsValid() && !World->bIsTearingDown) { SuppressPlayer(*World.Get()); }
		SampleCounters();
		if (!SaveEvidence(TEXT("last_visible_scenario"), Report(Reason)))
		{
			UE_LOG(LogDublinDestructionVerification, Error, TEXT("Could not save fixed-path visible scenario evidence."));
		}
		UE_LOG(LogDublinDestructionVerification, Display, TEXT("Verify.%s stopped: %s; submitted=%d, backpressure skips=%d. "
			"No scenario callbacks remain. Accepted native queues drain normally; rubble/waves/FX are not erased. PIE remains open."),
			Name(), Reason, Submitted, BackpressureSkips);
	}

	bool IsRunning() const { return bRunning; }

private:
	EVisibleScenario Kind = EVisibleScenario::Idle;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ADublinCityWorld> City;
	FSelectedScene Scene;
	TArray<FDublinImpact> Sequence;
	TArray<TSharedPtr<FJsonValue>> Samples;
	TArray<TSharedPtr<FJsonValue>> SelectedIds;
	FTimerHandle Timer;
	FTSTicker::FDelegateHandle Watchdog;
	double StartedWorld = 0;
	double StartedWall = 0;
	double NextImpact = 0;
	double NextSample = 1;
	int32 Submitted = 0;
	int32 BackpressureSkips = 0;
	bool bRunning = false;

	const TCHAR* Name() const
	{
		switch (Kind)
		{
		case EVisibleScenario::Sample: return TEXT("Sample");
		case EVisibleScenario::Maximal: return TEXT("Maximal");
		default: return TEXT("Idle");
		}
	}

	void OnPIEEnded(bool bSimulating) { Stop(TEXT("PIE_ended")); }

	bool Watch(float DeltaTime)
	{
		if (!bRunning) { return false; }
		if (!IsDublinPIE(World.Get()) || !City.IsValid()) { Stop(TEXT("world_lost"), true); return false; }
		if (FPlatformTime::Seconds() - StartedWall >= ScenarioWallSeconds)
		{
			Stop(TEXT("wall_timeout_including_paused_PIE"), true);
			return false;
		}
		return true;
	}

	void Step()
	{
		if (!bRunning) { return; }
		if (!IsDublinPIE(World.Get()) || !City.IsValid()) { Stop(TEXT("world_lost")); return; }
		const double Elapsed = World->GetTimeSeconds() - StartedWorld;
		if (Elapsed >= ScenarioWorldSeconds) { Stop(TEXT("completed_30_world_seconds")); return; }
		if (!WithinBudgets(*City.Get()) || ActiveImpactEffects(*World.Get()) > DublinImpactFX::MaxSystems)
		{
			Stop(TEXT("native_budget_or_queue_failure"));
			return;
		}
		if (Elapsed >= NextSample)
		{
			SampleCounters();
			NextSample = FMath::FloorToDouble(Elapsed) + 1;
		}
		if (!Sequence.IsEmpty() && Elapsed >= NextImpact)
		{
			// At most one admission per timer callback, two per second, and no catch-up burst.
			NextImpact = Elapsed + 0.5;
			if (City->QueuedImpactCount >= MaxQueuedImpacts / 4) { ++BackpressureSkips; return; }
			FDublinImpact Impact = Sequence[Submitted % Sequence.Num()];
			Impact.Seed += Submitted;
			if (!SubmitImpact(*City.Get(), Impact)) { Stop(TEXT("native_impact_rejected")); return; }
			++Submitted;
		}
	}

	void SampleCounters()
	{
		if (!City.IsValid() || !World.IsValid() || Samples.Num() >= 32) { return; }
		TSharedRef<FJsonObject> Sample = Counters(*City.Get());
		Sample->SetNumberField(TEXT("world_seconds"), World->GetTimeSeconds());
		Sample->SetNumberField(TEXT("elapsed_world_seconds"), World->GetTimeSeconds() - StartedWorld);
		Sample->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		Sample->SetNumberField(TEXT("live_impact_effect_components"), ActiveImpactEffects(*World.Get()));
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
	}

	TSharedRef<FJsonObject> Report(const TCHAR* Status)
	{
		TSharedRef<FJsonObject> Out = Scene.Describe();
		Out->SetStringField(TEXT("scenario"), Name());
		Out->SetStringField(TEXT("status"), Status);
		Out->SetStringField(TEXT("utc"), FDateTime::UtcNow().ToIso8601());
		Out->SetBoolField(TEXT("running"), bRunning);
		Out->SetObjectField(TEXT("world_diagnostics"), WorldDiagnostics(World.Get(), City.Get()));
		Out->SetNumberField(TEXT("start_world_seconds"), StartedWorld);
		Out->SetNumberField(TEXT("elapsed_world_seconds"), World.IsValid() ? World->GetTimeSeconds() - StartedWorld : 0);
		Out->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		Out->SetNumberField(TEXT("submitted_impacts"), Submitted);
		Out->SetNumberField(TEXT("backpressure_skips"), BackpressureSkips);
		Out->SetNumberField(TEXT("world_duration_limit_seconds"), ScenarioWorldSeconds);
		Out->SetNumberField(TEXT("wall_duration_limit_seconds"), ScenarioWallSeconds);
		Out->SetNumberField(TEXT("maximum_submission_rate_hz"), 2);
		Out->SetArrayField(TEXT("maximal_neighborhood_ids"), SelectedIds);
		Out->SetArrayField(TEXT("counter_samples_max32"), Samples);
		Out->SetStringField(TEXT("performance_status"), TEXT("NOT_ASSESSED: scenario does not reset or substitute for the performance capture clock"));
		TArray<TSharedPtr<FJsonValue>> Leaves;
		if (City.IsValid())
		{
			if (UGeometryCollectionComponent* Component = TargetCollection(*City.Get(), Scene))
			{
				const TArray<FTransform> Current = Component->GetCurrentTransforms();
				const TArray<FTransform> Initial = Component->GetInitialLocalRestTransforms();
				for (int32 Index : Scene.Record.LeafTransforms)
				{
					if (Leaves.Num() == 128) { break; }
					if (!Current.IsValidIndex(Index) || !Initial.IsValidIndex(Index)) { continue; }
					TSharedRef<FJsonObject> Leaf = MakeShared<FJsonObject>();
					Leaf->SetNumberField(TEXT("transform_index"), Index);
					SetPoint(*Leaf, TEXT("rest_world_cm"), Component->GetComponentTransform().TransformPosition(Initial[Index].GetLocation()));
					SetPoint(*Leaf, TEXT("current_world_cm"), Component->GetComponentTransform().TransformPosition(Current[Index].GetLocation()));
					Leaves.Add(MakeShared<FJsonValueObject>(Leaf));
				}
			}
		}
		Out->SetArrayField(TEXT("selected_target_leaf_observations_max128"), Leaves);
		return Out;
	}
};

TUniquePtr<FVisibleScenario> VisibleScenario;

void StartVisibleScenario(UWorld* World, EVisibleScenario Kind)
{
	if (VisibleScenario && VisibleScenario->IsRunning())
	{
		UE_LOG(LogDublinDestructionVerification, Error, TEXT("A verification scenario is already active. Use DublinFlight.Verify.Stop."));
		return;
	}
	VisibleScenario = MakeUnique<FVisibleScenario>();
	VisibleScenario->Start(World, Kind);
}

FAutoConsoleCommandWithWorld StartIdle(TEXT("DublinFlight.Verify.Idle"),
	TEXT("Developer verification only: fresh native Dublin PIE, 30s no-impact observation, no performance clock reset."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { StartVisibleScenario(World, EVisibleScenario::Idle); }));
FAutoConsoleCommandWithWorld StartSample(TEXT("DublinFlight.Verify.Sample"),
	TEXT("Developer verification only: fresh native Dublin PIE, 30s fixed sample cannon/bomb/land/water sequence. Partial bake allowed."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { StartVisibleScenario(World, EVisibleScenario::Sample); }));
FAutoConsoleCommandWithWorld StartMaximal(TEXT("DublinFlight.Verify.Maximal"),
	TEXT("Developer verification only: fresh fully baked Dublin PIE, 30s distributed game-yield bombs, real activation/FX caps."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { StartVisibleScenario(World, EVisibleScenario::Maximal); }));
FAutoConsoleCommand StopVisible(TEXT("DublinFlight.Verify.Stop"),
	TEXT("Stop only the bounded verification scheduler/watchdog; leave PIE and accepted city physics intact."),
	FConsoleCommandDelegate::CreateLambda([]() { if (VisibleScenario) { VisibleScenario->Stop(TEXT("explicit_stop")); } }));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionActualCityPIETest, "DublinFlight.Destruction.PIE.ActualCityDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinDestructionActualCityPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open the saved /Game/Maps/Dublin editor map; the native fixture starts its own PIE after automation teardown."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinDestruction::PIETests::FActualCityDamage(*this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionTerrainAndWaterPIETest, "DublinFlight.Destruction.PIE.TerrainAndWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinDestructionTerrainAndWaterPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open the saved /Game/Maps/Dublin editor map; the native fixture starts its own PIE after automation teardown."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinDestruction::PIETests::FActualCityDamage(*this,
		DublinDestruction::PIETests::EVerificationScope::TerrainAndWater));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionRecoveredBuildingPIETest, "DublinFlight.Destruction.PIE.RecoveredBuildingDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinDestructionRecoveredBuildingPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open the saved /Game/Maps/Dublin map with recovered fracture assets."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinDestruction::PIETests::FActualCityDamage(*this,
		DublinDestruction::PIETests::EVerificationScope::ActualCityDamage, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinSmokeAndCannonWaterVisualPIETest, "DublinFlight.Effects.PIE.SmokeAndCannonWaterVisuals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinSmokeAndCannonWaterVisualPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin")
		|| !FPackageName::DoesPackageExist(TEXT("/Game/Maps/Dublin")) || EditorWorld->GetOutermost()->IsDirty()
		|| GEditor->PlayWorld)
	{
		AddError(TEXT("Save /Game/Maps/Dublin and end existing PIE; run this visual test alone so it owns a fresh rendered PIE session."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinDestruction::PIETests::FSmokeAndCannonWaterVisuals(*this));
	return true;
}

#endif
