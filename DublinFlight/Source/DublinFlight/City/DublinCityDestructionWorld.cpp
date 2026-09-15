#include "City/DublinCityWorld.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Field/FieldSystemObjects.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionSimulationTypes.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsInterfaceTypesCore.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "Misc/PackageName.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDublinDestruction, Log, All);

namespace
{
	const FName FractureMarker(TEXT("DublinFlight.City.Fracture.v1"));

	void WriteSection(UProceduralMeshComponent& Component, int32 Index, const FDublinCitySection& Section, bool Collision)
	{
		Component.CreateMeshSection(Index, Section.Vertices, Section.Triangles, Section.Normals,
			Section.UV0, Section.UV1, TArray<FVector2D>(), TArray<FVector2D>(), Section.Colors, Section.Tangents, Collision);
	}

	void UpdateSection(UProceduralMeshComponent& Component, const FDublinCitySection& Section)
	{
		Component.UpdateMeshSection(0, Section.Vertices, Section.Normals, Section.UV0, Section.UV1,
			TArray<FVector2D>(), TArray<FVector2D>(), Section.Colors, Section.Tangents);
	}
}

bool DublinFractureBake::RegisterRuntimePhysics(UGeometryCollectionComponent& Component,
	const FDublinFractureRecord& Record, FString& Error)
{
	const UGeometryCollection* Asset = Component.GetRestCollection();
	if (!Asset) { Error = TEXT("Fracture component has no rest collection"); return false; }
	if (!ValidateCollisionData(*Asset, Record, Error)) { return false; }
	if (!Component.IsRegistered() && Component.GetPhysicsProxy())
	{
		Error = TEXT("Fracture proxy was created before component registration");
		return false;
	}
	// UE5.8 SetSimulatePhysics on an unregistered GC bypasses OnCreatePhysicsState:
	// its fallback creates a proxy without particle states, owner data or query filters.
	Component.RegisterComponent();
	Component.SetSimulatePhysics(true);
	const FGeometryCollectionPhysicsProxy* Proxy = Component.GetPhysicsProxy();
	const FGeometryCollectionPhysicsProxy::FParticle* Root = Proxy ? Proxy->GetInitialRootParticle_External() : nullptr;
	const FGeometryDynamicCollection* Dynamic = Component.GetDynamicCollection();
	if (!Component.IsRegistered() || !Component.IsPhysicsStateCreated() || !Root || !Root->GetGeometry() ||
		FChaosUserData::Get<UPrimitiveComponent>(Root->UserData()) != &Component ||
		Root->ShapesArray().IsEmpty() || !Dynamic)
	{
		Error = TEXT("Fracture physics has no initialized root collision/owner data");
		return false;
	}
	for (const auto& Shape : Root->ShapesArray())
	{
		const auto Filter = Shape->GetShapeFilterData();
		if (!Shape->GetQueryEnabled() || !Shape->GetSimEnabled() || !Filter.IsSimValid() ||
			!(Filter.GetBlockChannels() & (uint64(1) << ECC_Visibility)))
		{
			Error = TEXT("Fracture root collision does not block visibility queries and physical contacts");
			return false;
		}
	}
	const Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Dynamic);
	for (int32 Leaf : Record.LeafTransforms)
	{
		const bool bAnchor = Record.Anchors.Contains(Leaf);
		const uint8 Expected = static_cast<uint8>(bAnchor
			? Chaos::EObjectStateType::Kinematic : Chaos::EObjectStateType::Dynamic);
		if (!Dynamic->SimulatableParticles[Leaf] || Dynamic->DynamicState[Leaf] != Expected ||
			Anchoring.IsAnchored(Leaf) != bAnchor)
		{
			Error = FString::Printf(TEXT("Fracture leaf %d has uninitialized activity or dynamic state"), Leaf);
			return false;
		}
	}
	Error.Reset();
	return true;
}

bool ADublinCityWorld::DestructionFailure(const FString& Error)
{
	LastDestructionError = Error;
	UE_LOG(LogDublinDestruction, Error, TEXT("%s: %s"), *GetName(), *Error);
	return false;
}

void ADublinCityWorld::ResetDestructionState()
{
	TInlineComponentArray<UGeometryCollectionComponent*> Components;
	GetComponents(Components);
	for (UGeometryCollectionComponent* Component : Components)
	{
		if (IsValid(Component) && Component->GetOwner() == this && Component->GetOuter() == this &&
			Component->GetClass() == UGeometryCollectionComponent::StaticClass() &&
			Component->ComponentHasTag(FractureMarker) && Component->GetName().StartsWith(TEXT("DublinFracture_")))
		{
			RemoveInstanceComponent(Component);
			Component->DestroyComponent();
		}
	}
	FractureComponents.Reset();
	FractureActivity.Reset();
	PendingImpacts.Reset();
	RemovedIntactBuildings.Reset();
	BuildingBounds.Reset();
	BuildingToChunk.Reset();
	BuildingSpatialIndex.Reset();
	GroundMutation = FDublinGroundMutation();
	WaterWaves = FDublinWaterWaves();
	bDestructionInitialized = false;
	bDestructionReady = false;
	bAllBuildingsFractureReady = false;
	bImpactQueueBlocked = false;
	bBakeInProgress = false;
	BakeQueue.Reset();
	BakeCursor = 0;
	WaterAccumulator = 0;
	AcceptedImpactCount = QueuedImpactCount = QueuedBuildingHitCount = 0;
	ActiveFractureCollections = SleepingFractureCollections = ActiveFracturePieces = 0;
	FracturedBuildingCount = MovedFragmentCount = ForcedSleepCount = 0;
	CraterChangedNodeCount = LastCraterChunkCount = 0;
	WaterMaxDisplacementCm = 0;
	WaterActiveNodeCount = WaterSimulationTriangleCount = 0;
	SetActorTickEnabled(false);
}

void ADublinCityWorld::InitializeDestructionState()
{
	if (!SourceData) { return; }
	GroundMutation.Initialize(SourceData->Terrain);
	FString Error;
	if (!WaterWaves.Initialize(SourceData->Water, Error)) { DestructionFailure(Error); return; }
	if (WaterComponent && !WaterWaves.RenderMesh.Triangles.IsEmpty())
	{
		WriteSection(*WaterComponent, 0, WaterWaves.RenderMesh, false);
		WaterSimulationTriangleCount = WaterWaves.RenderMesh.Triangles.Num() / 3;
	}
	BuildingToChunk.Init(INDEX_NONE, SourceData->Buildings.Num());
	for (int32 Chunk = 0; Chunk < BuildingChunks.Num(); ++Chunk)
	{
		for (int32 Building : BuildingChunks[Chunk].BuildingIndices) { BuildingToChunk[Building] = Chunk; }
	}
	for (int32 I = 0; I < SourceData->Buildings.Num(); ++I)
	{
		const FDublinCityBuilding& Building = SourceData->Buildings[I];
		FBox Bounds(ForceInit);
		for (const FVector& Vertex : Building.Mesh.VerticesCm) { Bounds += Vertex + Building.PivotCm; }
		BuildingBounds.Add(Bounds);
		const FIntPoint Min = DublinCity::SpatialCell(Bounds.Min), Max = DublinCity::SpatialCell(Bounds.Max);
		for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			for (int32 X = Min.X; X <= Max.X; ++X)
				BuildingSpatialIndex.FindOrAdd(FIntPoint(X, Y)).Add(I);
	}
	bDestructionInitialized = true;
	RefreshDestructionReadiness();
}

void ADublinCityWorld::RefreshDestructionReadiness()
{
	FractureReadyBuildingCount = 0;
	bDestructionReady = false;
	bAllBuildingsFractureReady = false;
	if (!bCityReady || !SourceData || !bDestructionInitialized) { return; }
	if (!FractureLibrary) { FractureLibrary = DublinFractureBake::FindLibrary(); }
	if (FractureLibrary && FractureLibrary->BakeVersion == UDublinCityFractureLibrary::CurrentBakeVersion)
	{
		for (const FDublinCityBuilding& Building : SourceData->Buildings)
		{
			const FDublinFractureRecord* Record = FractureLibrary->Find(Building.Id);
			if (Record && Record->bReady && Record->PieceCount >= 2 && Record->PieceCount <= 128 &&
				Record->RootTransform != INDEX_NONE && !Record->Anchors.IsEmpty() && Record->LeafTransforms.Num() == Record->PieceCount &&
				!Record->Collection.IsNull() && Record->SourceDigest == DublinDestruction::BuildingDigest(Building) &&
				FPackageName::DoesPackageExist(Record->Collection.ToSoftObjectPath().GetLongPackageName()))
			{
				++FractureReadyBuildingCount;
			}
		}
	}
	bAllBuildingsFractureReady = FractureReadyBuildingCount == SourceData->Buildings.Num() && FractureReadyBuildingCount > 0;
	bDestructionReady = bAllBuildingsFractureReady || (bAllowPartialBakePreview && FractureReadyBuildingCount > 0);
	if (!bDestructionReady)
	{
		LastDestructionError = FString::Printf(TEXT("Destruction not ready: %d/%d source volumes have validated fracture records. Bake all volumes, or explicitly enable partial-bake preview."),
			FractureReadyBuildingCount, SourceData->Buildings.Num());
	}
	else { LastDestructionError.Reset(); }
}

void ADublinCityWorld::RetryDestructionQueue()
{
	RefreshDestructionReadiness();
	if (bDestructionReady) { bImpactQueueBlocked = false; UpdateDestructionTickEnabled(); }
}

TArray<int32> ADublinCityWorld::SelectImpactedBuildings(const FDublinImpact& Impact) const
{
	TSet<int32> Selected;
	const FIntPoint Min = DublinCity::SpatialCell(Impact.PositionCm - FVector(Impact.RadiusCm));
	const FIntPoint Max = DublinCity::SpatialCell(Impact.PositionCm + FVector(Impact.RadiusCm));
	for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
	{
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			if (const TArray<int32>* Cell = BuildingSpatialIndex.Find(FIntPoint(X, Y)))
			{
				for (int32 Building : *Cell)
				{
					if (DublinDestruction::SphereTouchesBox(Impact.PositionCm, Impact.RadiusCm, BuildingBounds[Building]))
					{
						Selected.Add(Building);
					}
				}
			}
		}
	}
	TArray<int32> Result = Selected.Array();
	Result.Sort([this, &Impact](int32 A, int32 B)
	{
		return BuildingBounds[A].ComputeSquaredDistanceToPoint(Impact.PositionCm) <
			BuildingBounds[B].ComputeSquaredDistanceToPoint(Impact.PositionCm);
	});
	return Result;
}

bool ADublinCityWorld::GetWaterSurfaceZ(const FVector& PositionCm, float& OutSurfaceZCm) const
{
	if (!bCityReady || !SourceData || !DublinDestruction::SourceWaterZ(SourceData->Water, PositionCm, OutSurfaceZCm))
	{
		return false;
	}
	OutSurfaceZCm += WaterWaves.SampleDisplacement(PositionCm);
	return true;
}

bool ADublinCityWorld::ApplyGroundCrater(const FDublinImpact& Impact)
{
	TSet<int32> Changed;
	if (!GroundMutation.Apply(SourceData->Terrain, Impact, Changed)) { LastCraterChunkCount = 0; return false; }
	CraterChangedNodeCount += Changed.Num();
	LastCraterChunkCount = 0;
	for (int32 ChunkIndex = 0; ChunkIndex < TerrainChunks.Num(); ++ChunkIndex)
	{
		FDublinCityChunk& Chunk = TerrainChunks[ChunkIndex];
		if (FMath::Abs(Chunk.OriginCm.X - Impact.PositionCm.X) > Impact.RadiusCm + 3600 ||
			FMath::Abs(Chunk.OriginCm.Y - Impact.PositionCm.Y) > Impact.RadiusCm + 3600) { continue; }
		FDublinCitySection& Section = Chunk.Sections[0];
		const bool Updated = DublinDestruction::SyncGroundChunk(Chunk, GroundMutation, Changed);
		if (Updated && TerrainComponents.IsValidIndex(ChunkIndex) && IsValid(TerrainComponents[ChunkIndex]))
		{
			// PMC's fixed-topology update synchronously updates the existing triangle-mesh collision vertices.
			UpdateSection(*TerrainComponents[ChunkIndex], Section);
			++LastCraterChunkCount;
		}
	}
	return true;
}

bool ADublinCityWorld::ApplyImpact(const FDublinImpact& Impact)
{
	FString Error;
	if (!DublinDestruction::ValidateImpact(Impact, Error)) { return DestructionFailure(Error); }
	if (!bDestructionReady || !SourceData || !FractureLibrary)
	{
		return DestructionFailure(TEXT("Destruction not ready; no geometry removed and no impact accepted"));
	}
	if (bImpactQueueBlocked) { return DestructionFailure(TEXT("Destruction queue blocked by an explicit activation error; use RetryDestructionQueue after correction")); }
	float WaterZ;
	if (Impact.bWater && !GetWaterSurfaceZ(Impact.PositionCm, WaterZ))
	{
		return DestructionFailure(TEXT("Water impact is outside the actual source river triangles"));
	}
	TArray<int32> Buildings = SelectImpactedBuildings(Impact);
	if (!Buildings.IsEmpty() && PendingImpacts.Num() >= DublinDestruction::MaxQueuedImpacts)
	{
		return DestructionFailure(TEXT("Bounded 64-impact queue is full; hit rejected without mutation"));
	}
	// Validate the entire selected set before changing ground, water, or accepting a building hit.
	for (int32 Index : Buildings)
	{
		const FDublinFractureRecord* Record = FractureLibrary->Find(SourceData->Buildings[Index].Id);
		if (!Record || !Record->bReady || Record->Collection.IsNull() ||
			Record->SourceDigest != DublinDestruction::BuildingDigest(SourceData->Buildings[Index]))
		{
			return DestructionFailure(TEXT("Impact includes a source building without current valid fracture data: ") + SourceData->Buildings[Index].Id);
		}
	}
	const bool WaterChanged = Impact.bWater && WaterWaves.AddImpact(SourceData->Water, Impact);
	const bool GroundChanged = !Impact.bWater && ApplyGroundCrater(Impact);
	if (Buildings.IsEmpty() && !WaterChanged && !GroundChanged)
	{
		return DestructionFailure(TEXT("Impact touches no deformable ground, water nodes or source building"));
	}
	if (!Buildings.IsEmpty())
	{
		FDublinQueuedWorldImpact Queue;
		Queue.Impact = Impact;
		Queue.Buildings = MoveTemp(Buildings);
		PendingImpacts.Add(MoveTemp(Queue));
	}
	++AcceptedImpactCount;
	LastDestructionError.Reset();
	UpdateDestructionCounters();
	UpdateDestructionTickEnabled();
	return true;
}

bool ADublinCityWorld::RewriteBuildingChunk(int32 ChunkIndex, const TSet<int32>& Removed)
{
	if (!BuildingComponents.IsValidIndex(ChunkIndex) || !IsValid(BuildingComponents[ChunkIndex])) { return false; }
	UProceduralMeshComponent& Component = *BuildingComponents[ChunkIndex];
	int32 Last = INDEX_NONE;
	Component.ClearAllMeshSections();
	for (int32 Material = 0; Material < 3; ++Material)
	{
		const FDublinCitySection Section = DublinDestruction::FilterIntactSection(BuildingChunks[ChunkIndex].Sections[Material], Removed);
		if (Section.Triangles.IsEmpty()) { continue; }
		WriteSection(Component, Material, Section, false);
		Last = Material;
	}
	if (Last == INDEX_NONE) { return true; }
	for (int32 I = 0; I < Component.GetNumSections(); ++I)
	{
		if (FProcMeshSection* Section = Component.GetProcMeshSection(I)) { Section->bEnableCollision = !Section->ProcIndexBuffer.IsEmpty(); }
	}
	const FProcMeshSection LastCopy = *Component.GetProcMeshSection(Last);
	Component.SetProcMeshSection(Last, LastCopy);
	const UBodySetup* Body = Component.GetBodySetup();
	return Body && !Body->bFailedToCreatePhysicsMeshes && !Body->TriMeshGeometries.IsEmpty();
}

bool ADublinCityWorld::ActivateBuilding(int32 Index)
{
	if (FractureComponents.Contains(Index)) { return true; }
	const FDublinCityBuilding& Building = SourceData->Buildings[Index];
	const FDublinFractureRecord* Record = FractureLibrary->Find(Building.Id);
	UGeometryCollection* Asset = Record ? Record->Collection.LoadSynchronous() : nullptr;
	if (!Asset || Asset->IsEmpty() || (!Asset->HasMeshData() && !Asset->HasNaniteData()) || Record->PieceCount < 2)
	{
		return DestructionFailure(TEXT("Missing/invalid real fracture asset for ") + Building.Id + TEXT("; intact mesh retained and queued hit blocked"));
	}
	FString CollisionError;
	if (!DublinFractureBake::ValidateCollisionData(*Asset, *Record, CollisionError))
	{
		return DestructionFailure(Building.Id + TEXT(": ") + CollisionError + TEXT("; intact mesh retained"));
	}
	UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(this,
		MakeUniqueObjectName(this, UGeometryCollectionComponent::StaticClass(), FName(*FString::Printf(TEXT("DublinFracture_%d"), Index))), RF_Transient);
	Component->ComponentTags.AddUnique(FractureMarker);
	AddInstanceComponent(Component);
	Component->SetupAttachment(CityRoot);
	Component->SetRelativeLocation(Building.PivotCm);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCanEverAffectNavigation(false);
	Component->SetRestCollection(Asset, true);
	Component->EnableClustering = true;
	Component->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
	Component->SetDamageModel(EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold);
	Component->SetDamageThreshold({100, 75, 50});
	FGeometryCollectionDamagePropagationData Propagation;
	Propagation.bEnabled = false;
	Propagation.BreakDamagePropagationFactor = 0;
	Propagation.ShockDamagePropagationFactor = 0;
	Component->SetDamagePropagationData(Propagation);
	Component->SetEnableDamageFromCollision(false);
	Component->bAllowRemovalOnSleep = false;
	Component->bAllowRemovalOnBreak = false;
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component->SetCollisionObjectType(ECC_WorldDynamic);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	Component->SetEnableGravity(true);
	if (!DublinFractureBake::RegisterRuntimePhysics(*Component, *Record, CollisionError))
	{
		RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		return DestructionFailure(Building.Id + TEXT(": ") + CollisionError + TEXT("; intact building retained"));
	}
	for (int32 Anchor : Record->Anchors) { Component->SetAnchoredByIndex(Anchor, true); }
	TSet<int32> Candidate = RemovedIntactBuildings;
	Candidate.Add(Index);
	if (!RewriteBuildingChunk(BuildingToChunk[Index], Candidate))
	{
		RewriteBuildingChunk(BuildingToChunk[Index], RemovedIntactBuildings);
		RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		return DestructionFailure(TEXT("Intact chunk collision rewrite failed; activation rolled back"));
	}
	RemovedIntactBuildings = MoveTemp(Candidate);
	FractureComponents.Add(Index, Component);
	FDublinFractureActivity& State = FractureActivity.Add(Index);
	State.Pieces = Record->PieceCount;
	State.LeafTransforms = Record->LeafTransforms;
	State.InitialTransforms = Component->GetInitialLocalRestTransforms();
	State.PreviousTransforms = State.InitialTransforms;
	State.LastImpactTime = GetWorld()->GetTimeSeconds();
	State.LastMotionPoll = State.LastImpactTime;
	return true;
}

void ADublinCityWorld::ApplyCollectionImpact(int32 Index, const FDublinImpact& Impact)
{
	UGeometryCollectionComponent* Component = FractureComponents.FindRef(Index);
	if (!IsValid(Component)) { return; }
	FDublinFractureActivity& State = FractureActivity.FindChecked(Index);
	if (State.bSleeping)
	{
		UUniformInteger* Wake = NewObject<UUniformInteger>(Component);
		Wake->SetUniformInteger(static_cast<int32>(EObjectStateTypeEnum::Chaos_Object_Dynamic));
		UFieldSystemMetaDataFilter* SleepingOnly = NewObject<UFieldSystemMetaDataFilter>(Component);
		SleepingOnly->SetMetaDataFilterType(EFieldFilterType::Field_Filter_Sleeping,
			EFieldObjectType::Field_Object_All, EFieldPositionType::Field_Position_CenterOfMass);
		Component->ApplyPhysicsField(true, EGeometryCollectionPhysicsTypeEnum::Chaos_DynamicState, SleepingOnly, Wake);
	}
	State.bSleeping = false;
	State.QuietPolls = 0;
	State.LastImpactTime = GetWorld()->GetTimeSeconds();
	Component->SetComponentTickEnabled(true);
	const float Strain = DublinDestruction::ImpactStrain(Impact);
	const FDublinFractureRecord* Record = FractureLibrary->Find(SourceData->Buildings[Index].Id);
	const TArray<FTransform> CurrentMassTransforms = Component->GetCurrentTransforms();
	const FTransform ComponentToWorld = Component->GetComponentTransform();
	const TArray<int32> HitLeaves = DublinDestruction::SelectImpactedFractureLeaves(State.LeafTransforms,
		CurrentMassTransforms, ComponentToWorld, Impact);
	// The original root is disabled after its first break. Stable leaf indices target their current
	// cluster parents instead, and unanchoring a struck support also restores Dynamic state in Chaos.
	for (int32 Leaf : HitLeaves)
	{
		if (Record && Record->Anchors.Contains(Leaf)) { Component->SetAnchoredByIndex(Leaf, false); }
		Component->ApplyExternalStrain(Leaf, Impact.PositionCm, 0,
			DublinDestruction::StrainPropagationDepth, DublinDestruction::StrainPropagationFactor, Strain);
		// Chaos retains a disabled child's own velocity when strain releases it. Target it once,
		// not the obsolete root or a field's repeated internal-parent samples; keep real masses.
		const FVector WorldMassCenter = ComponentToWorld.TransformPosition(CurrentMassTransforms[Leaf].GetLocation());
		Component->ApplyLinearVelocity(Leaf, DublinDestruction::ImpactVelocityChange(Impact, WorldMassCenter));
	}
}

void ADublinCityWorld::UpdateDestructionCounters()
{
	QueuedImpactCount = PendingImpacts.Num();
	QueuedBuildingHitCount = 0;
	for (const FDublinQueuedWorldImpact& Impact : PendingImpacts) { QueuedBuildingHitCount += Impact.Buildings.Num() - Impact.NextBuilding; }
	ActiveFractureCollections = SleepingFractureCollections = ActiveFracturePieces = MovedFragmentCount = 0;
	for (const TPair<int32, FDublinFractureActivity>& Pair : FractureActivity)
	{
		if (Pair.Value.bSleeping) { ++SleepingFractureCollections; }
		else { ++ActiveFractureCollections; ActiveFracturePieces += Pair.Value.Pieces; }
		MovedFragmentCount += Pair.Value.MovedPieces;
	}
	FracturedBuildingCount = RemovedIntactBuildings.Num();
	WaterMaxDisplacementCm = WaterWaves.MaxHeightCm;
	WaterActiveNodeCount = WaterWaves.bActive ? WaterWaves.WetNodes.Num() : 0;
}

void ADublinCityWorld::UpdateDestructionTickEnabled()
{
	SetActorTickEnabled(bBakeInProgress || WaterWaves.bActive || ActiveFractureCollections > 0 ||
		(!PendingImpacts.IsEmpty() && !bImpactQueueBlocked));
}

void ADublinCityWorld::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bBakeInProgress) { TickFractureBake(); return; }
	if (!GetWorld() || !GetWorld()->IsGameWorld() || !bDestructionInitialized) { return; }
	const double Now = GetWorld()->GetTimeSeconds();
	for (TPair<int32, FDublinFractureActivity>& Pair : FractureActivity)
	{
		FDublinFractureActivity& State = Pair.Value;
		UGeometryCollectionComponent* Component = FractureComponents.FindRef(Pair.Key);
		if (State.bSleeping || !IsValid(Component) || Now - State.LastMotionPoll < 0.5) { continue; }
		TArray<FTransform> Current = Component->GetCurrentTransforms();
		double MaxMove = 0;
		State.MovedPieces = 0;
		for (int32 I : State.LeafTransforms)
		{
			if (!Current.IsValidIndex(I)) { continue; }
			if (State.PreviousTransforms.IsValidIndex(I)) { MaxMove = FMath::Max(MaxMove, FVector::Dist(Current[I].GetLocation(), State.PreviousTransforms[I].GetLocation())); }
			if (State.InitialTransforms.IsValidIndex(I) && FVector::Dist(Current[I].GetLocation(), State.InitialTransforms[I].GetLocation()) > 10) { ++State.MovedPieces; }
		}
		State.PreviousTransforms = MoveTemp(Current);
		State.LastMotionPoll = Now;
		State.QuietPolls = MaxMove < 2 ? State.QuietPolls + 1 : 0;
		if ((Now - State.LastImpactTime > 3 && State.QuietPolls >= 4) || Now - State.LastImpactTime > 30)
		{
			if (State.QuietPolls < 4) { ++ForcedSleepCount; }
			UUniformInteger* Sleep = NewObject<UUniformInteger>(Component);
			Sleep->SetUniformInteger(static_cast<int32>(EObjectStateTypeEnum::Chaos_Object_Sleeping));
			UFieldSystemMetaDataFilter* DynamicOnly = NewObject<UFieldSystemMetaDataFilter>(Component);
			DynamicOnly->SetMetaDataFilterType(EFieldFilterType::Field_Filter_Dynamic,
				EFieldObjectType::Field_Object_All, EFieldPositionType::Field_Position_CenterOfMass);
			Component->ApplyPhysicsField(true, EGeometryCollectionPhysicsTypeEnum::Chaos_DynamicState, DynamicOnly, Sleep);
			Component->SetComponentTickEnabled(false);
			State.bSleeping = true;
		}
	}
	UpdateDestructionCounters();
	if (!PendingImpacts.IsEmpty() && !bImpactQueueBlocked)
	{
		FDublinQueuedWorldImpact& Batch = PendingImpacts[0];
		const int32 Building = Batch.Buildings[Batch.NextBuilding];
		const FDublinFractureRecord* Record = FractureLibrary->Find(SourceData->Buildings[Building].Id);
		const FDublinFractureActivity* Existing = FractureActivity.Find(Building);
		const bool NeedsSlot = !Existing || Existing->bSleeping;
		if (!NeedsSlot || (Record && ActiveFractureCollections < DublinDestruction::MaxActiveCollections &&
			ActiveFracturePieces + Record->PieceCount <= DublinDestruction::MaxActivePieces))
		{
			if (!ActivateBuilding(Building)) { bImpactQueueBlocked = true; }
			else
			{
				ApplyCollectionImpact(Building, Batch.Impact);
				if (++Batch.NextBuilding == Batch.Buildings.Num()) { PendingImpacts.RemoveAt(0); }
			}
		}
	}
	if (WaterWaves.bActive)
	{
		WaterAccumulator += FMath::Clamp(static_cast<double>(DeltaSeconds), 0.0, 0.25);
		int32 Steps = 0;
		while (WaterAccumulator >= 1.0 / 30 && Steps++ < 4)
		{
			WaterWaves.Step(1.0f / 30);
			WaterAccumulator -= 1.0 / 30;
		}
		if (WaterAccumulator > 0.25) { WaterAccumulator = 0.25; }
		if (Steps > 0 && WaterComponent)
		{
			WaterWaves.UpdateRenderVertices();
			UpdateSection(*WaterComponent, WaterWaves.RenderMesh);
		}
	}
	UpdateDestructionCounters();
	UpdateDestructionTickEnabled();
}

void ADublinCityWorld::BeginFractureBake()
{
	bBakeRequested = false;
#if WITH_EDITOR
	if (!GetWorld() || GetWorld()->WorldType != EWorldType::Editor || IsTemplate() || (GEditor && GEditor->PlayWorld))
	{
		DestructionFailure(TEXT("Fracture asset baking is editor-world only"));
		return;
	}
	if (bBakeInProgress) { return; }
	// A new derivative may change only vertex attributes. Refresh once per explicit bake request, never per tick.
	BuildCity();
	if (!bCityReady || !SourceData) { DestructionFailure(TEXT("Cannot bake without validated source city data")); return; }
	FString Error;
	FractureLibrary = DublinFractureBake::CreateLibrary(Error);
	if (!FractureLibrary) { DestructionFailure(Error); return; }
	BakeQueue.Reset();
	if (!BakeBuildingIds.IsEmpty())
	{
		TSet<int32> Selected;
		for (const FString& Id : BakeBuildingIds)
		{
			const int32 Index = SourceData->Buildings.IndexOfByPredicate([&Id](const FDublinCityBuilding& Building) { return Building.Id == Id; });
			if (Index == INDEX_NONE) { DestructionFailure(TEXT("Unknown bake source building id: ") + Id); return; }
			Selected.Add(Index);
		}
		BakeQueue = Selected.Array();
		BakeQueue.Sort();
	}
	else
	{
		const int32 Count = bBakeAllBuildings ? SourceData->Buildings.Num() : FMath::Clamp(BakeLimit, 1, SourceData->Buildings.Num());
		for (int32 I = 0; I < Count; ++I) { BakeQueue.Add(I); }
	}
	BakeCursor = BakeCompleted = BakeErrorCount = 0;
	BakeTotal = BakeQueue.Num();
	BakeErrors.Reset();
	bCancelBake = false;
	bBakeInProgress = !BakeQueue.IsEmpty();
	UpdateDestructionTickEnabled();
#else
	DestructionFailure(TEXT("Fracture baking is unavailable in packaged runtime; use saved baked assets"));
#endif
}

void ADublinCityWorld::TickFractureBake()
{
#if WITH_EDITOR
	if (!SourceData || bCancelBake || BakeCursor >= BakeQueue.Num())
	{
		bBakeInProgress = false;
		BakeCurrentSourceId.Reset();
		RefreshDestructionReadiness();
		UpdateDestructionTickEnabled();
		return;
	}
	const FDublinCityBuilding& Building = SourceData->Buildings[BakeQueue[BakeCursor]];
	BakeCurrentSourceId = Building.Id;
	const FDublinFractureRecord* Existing = FractureLibrary->Find(Building.Id);
	bool Skip = false;
	if (Existing && Existing->bReady && Existing->LeafTransforms.Num() == Existing->PieceCount &&
		Existing->SourceDigest == DublinDestruction::BuildingDigest(Building))
	{
		UGeometryCollection* Asset = Existing->Collection.LoadSynchronous();
		FString CollisionError;
		Skip = Asset && !Asset->IsEmpty() && (Asset->HasMeshData() || Asset->HasNaniteData()) &&
			DublinFractureBake::HasCurrentMaterialBindings(*Asset, *this) &&
			DublinFractureBake::ValidateCollisionData(*Asset, *Existing, CollisionError);
	}
	if (!Skip)
	{
		FDublinFractureRecord Record;
		FString Error;
		if (!DublinFractureBake::BakeBuilding(Building, *this, Record, Error))
		{
			++BakeErrorCount;
			Record.Error = Error;
			if (BakeErrors.Num() < 128) { BakeErrors.Add(Building.Id + TEXT(": ") + Error); }
			DestructionFailure(Building.Id + TEXT(": ") + Error);
		}
		const int32 Index = FractureLibrary->Records.IndexOfByPredicate([&Building](const FDublinFractureRecord& Row) { return Row.SourceId == Building.Id; });
		if (Index == INDEX_NONE) { FractureLibrary->Records.Add(MoveTemp(Record)); }
		else { FractureLibrary->Records[Index] = MoveTemp(Record); }
		if (!DublinFractureBake::SaveLibrary(*FractureLibrary, Error))
		{
			bBakeInProgress = false;
			DestructionFailure(Error);
			UpdateDestructionTickEnabled();
			return;
		}
	}
	++BakeCursor;
	BakeCompleted = BakeCursor;
	if (BakeCursor == BakeQueue.Num())
	{
		bBakeInProgress = false;
		BakeCurrentSourceId.Reset();
		RefreshDestructionReadiness();
		UpdateDestructionTickEnabled();
	}
#endif
}
