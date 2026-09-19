#include "City/DublinCityWorld.h"
#include "City/DublinCityStructural.h"
#include "City/DublinCityDetail.h"
#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Weapons/DublinWeaponModel.h"
#include "Chaos/ImplicitObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "HAL/PlatformTime.h"

#include "Components/SceneComponent.h"
#include "CoreGlobals.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
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
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDublinDestruction, Log, All);

namespace
{
	const FName FractureMarker(TEXT("DublinFlight.City.Fracture.v1"));

	void WriteSection(UProceduralMeshComponent& Component, int32 Index, const FDublinCitySection& Section, bool Collision)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_PMC_CreateSection);
		Component.CreateMeshSection(Index, Section.Vertices, Section.Triangles, Section.Normals,
			Section.UV0, Section.UV1, TArray<FVector2D>(), TArray<FVector2D>(), Section.Colors, Section.Tangents, Collision);
	}

	void UpdateSection(UProceduralMeshComponent& Component, const FDublinCitySection& Section)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_PMC_UpdateSection);
		Component.UpdateMeshSection(0, Section.Vertices, Section.Normals, Section.UV0, Section.UV1,
			TArray<FVector2D>(), TArray<FVector2D>(), Section.Colors, Section.Tangents);
	}

	bool RegisterValidatedRuntimePhysics(UGeometryCollectionComponent& Component,
		const FDublinFractureRecord& Record, FString& Error)
	{
		if (!Component.IsRegistered() && Component.GetPhysicsProxy())
		{
			Error = TEXT("Fracture proxy was created before component registration");
			return false;
		}
		// UE5.8 SetSimulatePhysics on an unregistered GC bypasses OnCreatePhysicsState:
		// its fallback creates a proxy without particle states, owner data or query filters.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Chaos_RegisterAndSimulate);
			Component.RegisterComponent();
			Component.SetSimulatePhysics(true);
		}
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
}

bool DublinFractureBake::RegisterRuntimePhysics(UGeometryCollectionComponent& Component,
	const FDublinFractureRecord& Record, FString& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_RegisterRuntimePhysics);
	const UGeometryCollection* Asset = Component.GetRestCollection();
	if (!Asset) { Error = TEXT("Fracture component has no rest collection"); return false; }
	if (!ValidateRuntimeCollisionData(*Asset, Record, Error)) { return false; }
	return RegisterValidatedRuntimePhysics(Component, Record, Error);
}

bool DublinFractureBake::PrepareAndRegisterRuntimePhysics(UGeometryCollectionComponent& Component,
	const UGeometryCollection& Asset, const FDublinFractureRecord& Record, FString& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_PrepareAndRegisterRuntimePhysics);
	if (Component.IsRegistered() || Component.IsPhysicsStateCreated() || Component.GetPhysicsProxy())
	{
		Error = TEXT("Fracture preparation requires an unregistered component without physics state");
		return false;
	}
	if (!ValidateRuntimeCollisionData(Asset, Record, Error)) { return false; }
	// Binding can create embedded components. Validate before binding, then apply the same
	// city overrides after asset defaults and before registration; independent callers still validate above.
	Component.SetRestCollection(&Asset, true);
	Component.EnableClustering = true;
	Component.ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
	Component.SetDamageModel(EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold);
	Component.SetDamageThreshold({100, 75, 50});
	FGeometryCollectionDamagePropagationData Propagation;
	Propagation.bEnabled = false;
	Propagation.BreakDamagePropagationFactor = 0;
	Propagation.ShockDamagePropagationFactor = 0;
	Component.SetDamagePropagationData(Propagation);
	Component.SetEnableDamageFromCollision(false);
	Component.bAllowRemovalOnSleep = false;
	Component.bAllowRemovalOnBreak = false;
	Component.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component.SetCollisionObjectType(ECC_WorldDynamic);
	Component.SetCollisionResponseToAllChannels(ECR_Block);
	Component.SetEnableGravity(true);
	return RegisterValidatedRuntimePhysics(Component, Record, Error);
}

bool ADublinCityWorld::DestructionFailure(const FString& Error)
{
	LastDestructionError = Error;
	UE_LOG(LogDublinDestruction, Error, TEXT("%s: %s"), *GetName(), *Error);
	return false;
}

void ADublinCityWorld::ResetDestructionState()
{
#if WITH_EDITOR
	FTSTicker::GetCoreTicker().RemoveTicker(BakeTickerHandle);
	BakeTickerHandle.Reset();
#endif
	CancelPendingFractureLoad();
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
	BakeQueueRecipes.Reset();
	BakeCursor = 0;
	WaterAccumulator = 0;
	AcceptedImpactCount = QueuedImpactCount = QueuedBuildingHitCount = 0;
	QueuedCoreBuildingHitCount = 0;
	ActiveFractureCollections = SleepingFractureCollections = ActiveFracturePieces = 0;
	CatalogFractureCollections = CatalogFractureLeafSlots = CatalogFractureHullSlots = 0;
	AllocatedFractureCollections = AllocatedFracturePieces = AllocatedFractureHullSlots = 0;
	SampledAwakeLeafBodies = SampledSleepingLeafBodies = 0;
	PendingFractureAssetLoads = LastFrameFractureRegistrations = LastFrameOrdinaryFractureRegistrations = PeakFrameFractureRegistrations = 0;
	DeferredImpactEffectsRequests = 0;
	MaxFirstDamageLatencySeconds = MaxCoreDamageLatencySeconds = 0;
	bCatalogBudgetValid = false;
	ImpactEpoch = FGuid::NewGuid();
	NextImpactEventId = 1;
	LastAcceptedImpactEventId = LastPresentedImpactEventId = 0;
	LastImpactProcessingFrame = MAX_uint64;
	ImpactQueueCursor = 0;
	LastNativeBodyPoll = -1;
	LastDestructionTickWorldTime = -1;
	PrimaryActorTick.UpdateTickIntervalAndCoolDown(DublinDestruction::OrdinaryTickIntervalSeconds);
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
	bCatalogBudgetValid = false;
	CatalogFractureCollections = CatalogFractureLeafSlots = CatalogFractureHullSlots = 0;
	if (!bCityReady || !SourceData || !bDestructionInitialized) { return; }
	if (!FractureLibrary) { FractureLibrary = DublinFractureBake::FindLibrary(); }
	DublinDestruction::FCatalogBudget Budget;
	FString BudgetError;
	bool bBudgetFits = true;
	TSet<FString> SourceIds;
	if (FractureLibrary && FractureLibrary->BakeVersion == UDublinCityFractureLibrary::CurrentBakeVersion)
	{
		TSet<FString> RecordIds;
		for (const FDublinFractureRecord& Record : FractureLibrary->Records)
		{
			if (RecordIds.Contains(Record.SourceId))
			{
				bBudgetFits = false;
				BudgetError = TEXT("Duplicate fracture record source identity in destruction catalog");
				break;
			}
			RecordIds.Add(Record.SourceId);
		}
		for (const FDublinCityBuilding& Building : SourceData->Buildings)
		{
			const FDublinFractureRecord* Record = FractureLibrary->Find(Building.Id);
			if (SourceIds.Contains(Building.Id))
			{
				bBudgetFits = false;
				BudgetError = TEXT("Duplicate source building identity in destruction catalog");
				continue;
			}
			SourceIds.Add(Building.Id);
			if (Record && Record->bReady && DublinFractureBake::HasValidPieceBudget(*Record) &&
				((Record->Recipe != EDublinFractureRecipe::StructuralPilot && !DublinFractureBake::IsDetailedRecipe(Record->Recipe)) || Record->bNaniteReady) &&
				Record->RootTransform != INDEX_NONE && !Record->Anchors.IsEmpty() && Record->LeafTransforms.Num() == Record->PieceCount &&
				!Record->Collection.IsNull() && DublinFractureBake::IsCurrentRecord(Building, *Record) &&
				FPackageName::DoesPackageExist(Record->Collection.ToSoftObjectPath().GetLongPackageName()))
			{
				++FractureReadyBuildingCount;
				if (bBudgetFits && !DublinDestruction::AddCatalogRecord(*Record, Budget, BudgetError)) { bBudgetFits = false; }
			}
		}
	}
	bAllBuildingsFractureReady = FractureReadyBuildingCount == SourceData->Buildings.Num() && FractureReadyBuildingCount > 0;
	CatalogFractureCollections = Budget.Collections;
	CatalogFractureLeafSlots = Budget.LeafSlots;
	CatalogFractureHullSlots = Budget.HullSlots;
	bCatalogBudgetValid = bBudgetFits && FractureReadyBuildingCount > 0;
	bDestructionReady = bCatalogBudgetValid &&
		(bAllBuildingsFractureReady || (bAllowPartialBakePreview && FractureReadyBuildingCount > 0));
	if (!bDestructionReady)
	{
		if (!bBudgetFits) { LastDestructionError = BudgetError; return; }
		LastDestructionError = FString::Printf(TEXT("Destruction not ready: %d/%d source volumes have validated fracture records. Bake all volumes, or explicitly enable partial-bake preview."),
			FractureReadyBuildingCount, SourceData->Buildings.Num());
	}
	else if (BakeErrorCount == 0) { LastDestructionError.Reset(); }
}

void ADublinCityWorld::RetryDestructionQueue()
{
	CancelPendingFractureLoad();
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
					if (DublinDestruction::IsHeightAwareBlast(Impact)
						? DublinDestruction::FootprintDistanceSquared(SourceData->Buildings[Building], Impact.PositionCm) <= FMath::Square(double(Impact.RadiusCm))
						: DublinDestruction::SphereTouchesBox(Impact.PositionCm, Impact.RadiusCm, BuildingBounds[Building]))
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
		const double DA = BuildingBounds[A].ComputeSquaredDistanceToPoint(Impact.PositionCm);
		const double DB = BuildingBounds[B].ComputeSquaredDistanceToPoint(Impact.PositionCm);
		// The actually struck roof precedes even taller neighbours in the same XY footprint.
		if (DA <= 4 || DB <= 4) { return DA == DB ? A < B : DA < DB; }
		const double FA = DublinDestruction::FootprintDistanceSquared(SourceData->Buildings[A], Impact.PositionCm);
		const double FB = DublinDestruction::FootprintDistanceSquared(SourceData->Buildings[B], Impact.PositionCm);
		return FA == FB ? A < B : FA < FB;
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
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_ApplyGroundCrater);
	TSet<int32> Changed;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Crater_CanonicalMutation);
		if (!GroundMutation.Apply(SourceData->Terrain, Impact, Changed)) { LastCraterChunkCount = 0; return false; }
	}
	CraterChangedNodeCount += Changed.Num();
	LastCraterChunkCount = 0;
	for (int32 ChunkIndex = 0; ChunkIndex < TerrainChunks.Num(); ++ChunkIndex)
	{
		FDublinCityChunk& Chunk = TerrainChunks[ChunkIndex];
		if (FMath::Abs(Chunk.OriginCm.X - Impact.PositionCm.X) > Impact.RadiusCm + 3600 ||
			FMath::Abs(Chunk.OriginCm.Y - Impact.PositionCm.Y) > Impact.RadiusCm + 3600) { continue; }
		FDublinCitySection& Section = Chunk.Sections[0];
		bool Updated = false;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Crater_SyncGroundChunk);
			Updated = DublinDestruction::SyncGroundChunk(Chunk, GroundMutation, Changed);
		}
		if (Updated && TerrainComponents.IsValidIndex(ChunkIndex) && IsValid(TerrainComponents[ChunkIndex]))
		{
			// Rebuild the deformed triangle collision, including sweep data, before accepting another projectile.
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Crater_SynchronousCollisionRecook);
				WriteSection(*TerrainComponents[ChunkIndex], 0, Section, true);
			}
			++LastCraterChunkCount;
		}
	}
	return true;
}

bool ADublinCityWorld::ApplyImpact(const FDublinImpact& Impact)
{
	uint64 EventId = 0;
	return ApplyImpactInternal(Impact, false, nullptr, EventId);
}

bool ADublinCityWorld::ApplyImpactWithPresentation(const FDublinImpact& Impact,
	UDublinWeaponComponent* Accounting, uint64& OutEventId)
{
	return ApplyImpactInternal(Impact, DublinImpactFX::WantsMaximumPresentation(Impact), Accounting, OutEventId);
}

bool ADublinCityWorld::HasWeaponImpactCapacity() const
{
	return bDestructionReady && !bImpactQueueBlocked &&
		PendingImpacts.Num() < DublinDestruction::MaxQueuedImpacts - 64;
}

bool ADublinCityWorld::ApplyImpactInternal(const FDublinImpact& Impact, bool bDeferredPresentation,
	UDublinWeaponComponent* Accounting, uint64& OutEventId)
{
	OutEventId = 0;
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_CityApplyImpact);
	TRACE_BOOKMARK(TEXT("DublinFlight city impact kind=%d seed=%d water=%d frame=%llu"),
		static_cast<int32>(Impact.Kind), Impact.Seed, Impact.bWater ? 1 : 0, GFrameCounter);
	FString Error;
	if (!DublinDestruction::ValidateImpact(Impact, Error)) { return DestructionFailure(Error); }
	if (!bDestructionReady || !bCatalogBudgetValid || !SourceData || !FractureLibrary)
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
	if (PendingImpacts.Num() >= DublinDestruction::MaxQueuedImpacts || NextImpactEventId == MAX_uint64)
	{
		return DestructionFailure(TEXT("Finite impact event queue/identity capacity exhausted; hit rejected without mutation"));
	}
	if (bDeferredPresentation && !GetWorld()->GetSubsystem<UDublinImpactEffectsSubsystem>())
	{
		return DestructionFailure(TEXT("Maximum impact presentation recipient unavailable; hit rejected without mutation"));
	}
	// Validate the entire selected set before changing ground, water, or accepting a building hit.
	for (int32 Index : Buildings)
	{
		const FDublinFractureRecord* Record = FractureLibrary->Find(SourceData->Buildings[Index].Id);
		if (!Record || !Record->bReady || !DublinFractureBake::HasValidPieceBudget(*Record) || Record->Collection.IsNull() ||
			!DublinFractureBake::IsCurrentRecord(SourceData->Buildings[Index], *Record))
		{
			return DestructionFailure(TEXT("Impact includes a source building without current valid fracture data: ") + SourceData->Buildings[Index].Id);
		}
	}
	const bool WaterChanged = Impact.bWater && WaterWaves.AddImpact(SourceData->Water, Impact);
	const bool GroundChanged = !Impact.bWater && ApplyGroundCrater(DublinWeapons::MakeGroundImpact(Impact));
	if (Buildings.IsEmpty() && !WaterChanged && !GroundChanged)
	{
		return DestructionFailure(TEXT("Impact touches no deformable ground, water nodes or source building"));
	}
	OutEventId = NextImpactEventId++;
	LastAcceptedImpactEventId = OutEventId;
	if (!Buildings.IsEmpty() || bDeferredPresentation)
	{
		FDublinQueuedWorldImpact Queue;
		Queue.Impact = Impact;
		Queue.Buildings = MoveTemp(Buildings);
		Queue.EventId = OutEventId;
		Queue.AcceptedWorldTime = GetWorld()->GetTimeSeconds();
		Queue.bDeferredPresentation = bDeferredPresentation;
		Queue.Accounting = Accounting;
		Queue.bSurfaceCommitted = GroundChanged || WaterChanged;
		const double CoreRadius = FMath::Min(double(Impact.RadiusCm), 6000.0);
		for (int32 Index : Queue.Buildings)
		{
			if (DublinDestruction::FootprintDistanceSquared(SourceData->Buildings[Index], Impact.PositionCm)
				<= FMath::Square(CoreRadius)) { ++Queue.RemainingCoreBuildings; }
		}
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
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_RewriteBuildingChunk);
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
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_PMC_RecookBuildingCollision);
		Component.SetProcMeshSection(Last, LastCopy);
	}
	const UBodySetup* Body = Component.GetBodySetup();
	return Body && !Body->bFailedToCreatePhysicsMeshes && !Body->TriMeshGeometries.IsEmpty();
}

void ADublinCityWorld::CancelPendingFractureLoad()
{
	if (PendingFractureLoad) { PendingFractureLoad->CancelHandle(); }
	PendingFractureLoad.Reset();
	PendingFracturePath.Reset();
	PendingFractureBuilding = INDEX_NONE;
	PendingFractureStartedWall = 0;
	for (auto& Pair : AdditionalFractureLoads)
	{
		if (Pair.Value.Handle) { Pair.Value.Handle->CancelHandle(); }
	}
	AdditionalFractureLoads.Reset();
	PendingFractureAssetLoads = 0;
}

void ADublinCityWorld::ReleaseFractureLoad(int32 Index)
{
	if (PendingFractureBuilding == Index)
	{
		PendingFractureLoad.Reset();
		PendingFracturePath.Reset();
		PendingFractureBuilding = INDEX_NONE;
		PendingFractureStartedWall = 0;
	}
	AdditionalFractureLoads.Remove(Index);
	PendingFractureAssetLoads = AdditionalFractureLoads.Num() + (PendingFractureLoad ? 1 : 0);
}

void ADublinCityWorld::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
#if WITH_EDITOR
	FTSTicker::GetCoreTicker().RemoveTicker(BakeTickerHandle);
	BakeTickerHandle.Reset();
#endif
	CancelPendingFractureLoad();
	Super::EndPlay(EndPlayReason);
}

bool ADublinCityWorld::PrepareFractureAsset(int32 Index, bool& bReady)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_PrepareFractureAsset);
	bReady = false;
	if (!SourceData || !SourceData->Buildings.IsValidIndex(Index) || !FractureLibrary)
	{
		return DestructionFailure(TEXT("Fracture asset request has no valid source record"));
	}
	const FDublinCityBuilding& Building = SourceData->Buildings[Index];
	const FDublinFractureRecord* Record = FractureLibrary->Find(Building.Id);
	if (!Record || !Record->bReady || Record->Collection.IsNull() ||
		!DublinFractureBake::IsCurrentRecord(Building, *Record))
	{
		return DestructionFailure(TEXT("Fracture asset request has no ready collection for ") + Building.Id);
	}
	if (FractureComponents.Contains(Index)) { bReady = true; return true; }
	const FSoftObjectPath Target = Record->Collection.ToSoftObjectPath();
	TSharedPtr<FStreamableHandle> Handle;
	double Started = 0;
	if (PendingFractureLoad && PendingFractureBuilding == Index)
	{
		if (PendingFracturePath != Target) { return DestructionFailure(TEXT("Pending fracture asset identity changed")); }
		Handle = PendingFractureLoad;
		Started = PendingFractureStartedWall;
	}
	else if (const FDublinPendingFractureAsset* Pending = AdditionalFractureLoads.Find(Index))
	{
		if (Pending->Path != Target) { return DestructionFailure(TEXT("Prefetched fracture asset identity changed")); }
		Handle = Pending->Handle;
		Started = Pending->StartedWallTime;
	}
	if (!Handle)
	{
		if (Record->Collection.IsValid()) { bReady = true; return true; }
		if (AdditionalFractureLoads.Num() + (PendingFractureLoad ? 1 : 0) >= DublinDestruction::MaxConcurrentFractureLoads) { return true; }
		if (!UAssetManager::GetIfInitialized())
		{
			return DestructionFailure(TEXT("Asset manager unavailable for fracture loading"));
		}
		Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Target);
		Started = FPlatformTime::Seconds();
		if (!Handle)
		{
			return DestructionFailure(TEXT("Could not request fracture asset asynchronously for ") + Building.Id);
		}
		if (!PendingFractureLoad)
		{
			PendingFractureBuilding = Index;
			PendingFracturePath = Target;
			PendingFractureLoad = Handle;
			PendingFractureStartedWall = Started;
		}
		else { AdditionalFractureLoads.Add(Index, {Handle, Target, Started}); }
		PendingFractureAssetLoads = AdditionalFractureLoads.Num() + (PendingFractureLoad ? 1 : 0);
	}
	if (Handle->WasCanceled())
	{
		return DestructionFailure(TEXT("Fracture asset request canceled for ") + Building.Id);
	}
	// A pending request retains the queue head and intact collision; it is not an activation failure.
	if (!Handle->HasLoadCompleted())
	{
		if (Started > 0 && FPlatformTime::Seconds() - Started > DublinDestruction::FractureLoadTimeoutSeconds)
		{
			return DestructionFailure(TEXT("Fracture asset load timed out; accepted event retained for explicit retry: ") + Building.Id);
		}
		return true;
	}
	if (!Record->Collection.IsValid())
	{
		return DestructionFailure(TEXT("Fracture asset failed to load for ") + Building.Id + TEXT("; intact mesh retained"));
	}
	bReady = true;
	return true;
}

void ADublinCityWorld::PrefetchFractureAssets()
{
	for (int32 Offset = 0; Offset < DublinDestruction::MaxActiveCollections; ++Offset)
	{
		bool bAny = false;
		for (int32 Q = 0; Q < PendingImpacts.Num(); ++Q)
		{
			FDublinQueuedWorldImpact& Event = PendingImpacts[(ImpactQueueCursor + Q) % PendingImpacts.Num()];
			const int32 Slot = Event.NextBuilding + Offset;
			if (!Event.Buildings.IsValidIndex(Slot)) { continue; }
			const int32 Index = Event.Buildings[Slot];
			if (Index == INDEX_NONE || FractureComponents.Contains(Index)) { continue; }
			bAny = true;
			bool bReady = false;
			if (!PrepareFractureAsset(Index, bReady)) { bImpactQueueBlocked = true; return; }
			if (PendingFractureAssetLoads >= DublinDestruction::MaxConcurrentFractureLoads) { return; }
		}
		if (!bAny && Offset > DublinDestruction::MaxConcurrentFractureLoads) { break; }
	}
}

bool ADublinCityWorld::ActivateBuilding(int32 Index)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_ActivateBuilding);
	if (!SourceData || !SourceData->Buildings.IsValidIndex(Index) || !FractureLibrary)
	{
		return DestructionFailure(TEXT("Fracture activation has no valid source/library"));
	}
	TRACE_BOOKMARK(TEXT("DublinFlight activate building=%d frame=%llu"), Index, GFrameCounter);
	const FDublinCityBuilding& Building = SourceData->Buildings[Index];
	const FDublinFractureRecord* Record = FractureLibrary->Find(Building.Id);
	if (!Record || !Record->bReady || !DublinFractureBake::IsCurrentRecord(Building, *Record))
	{
		return DestructionFailure(TEXT("Fracture activation rejected stale/unsupported recipe for ") + Building.Id);
	}
	if (FractureComponents.Contains(Index)) { return true; }
	if (FractureComponents.Num() >= DublinDestruction::MaxActiveCollections ||
		int64(AllocatedFracturePieces) + Record->PieceCount > DublinDestruction::MaxActivePieces ||
		int64(AllocatedFractureHullSlots) + DublinDestruction::ReservedHullSlots(*Record) > DublinDestruction::MaxCatalogHullSlots)
	{
		return DestructionFailure(TEXT("Catalog/resident allocation budget mismatch; accepted hit retained for correction"));
	}
	UGeometryCollection* Asset = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_GetResidentFractureCollection);
		Asset = Record ? Record->Collection.Get() : nullptr;
	}
	if (!Asset || Asset->IsEmpty() || (!Asset->HasMeshData() && !Asset->HasNaniteData()) ||
		!DublinFractureBake::HasValidPieceBudget(*Record))
	{
		return DestructionFailure(TEXT("Missing/invalid real fracture asset for ") + Building.Id + TEXT("; intact mesh retained and queued hit blocked"));
	}
	FString CollisionError;
	const auto Geometry = Asset->GetGeometryCollection();
	if (!Geometry)
	{
		return DestructionFailure(Building.Id + TEXT(": missing rest geometry; intact mesh retained"));
	}
	const auto* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(TEXT("Implicits"), FGeometryCollection::TransformGroup);
	int64 ActualLeafHulls = 0;
	if (Implicits)
	{
		for (int32 Leaf : Record->LeafTransforms)
		{
			if (Implicits->IsValidIndex(Leaf) && (*Implicits)[Leaf])
			{
				ActualLeafHulls += (*Implicits)[Leaf]->CountLeafObjectsInHierarchy();
			}
		}
	}
	if (ActualLeafHulls < Record->PieceCount || ActualLeafHulls > DublinDestruction::ReservedHullSlots(*Record) ||
		(Record->HullCount > 0 && ActualLeafHulls != Record->HullCount))
	{
		return DestructionFailure(Building.Id + TEXT(": cooked leaf collision exceeds/mismatches reserved hull metadata; intact mesh retained"));
	}
	UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(this,
		MakeUniqueObjectName(this, UGeometryCollectionComponent::StaticClass(), FName(*FString::Printf(TEXT("DublinFracture_%d"), Index))), RF_Transient);
	Component->ComponentTags.AddUnique(FractureMarker);
	AddInstanceComponent(Component);
	Component->SetupAttachment(CityRoot);
	Component->SetRelativeLocation(Building.PivotCm);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCanEverAffectNavigation(false);
	if (!DublinFractureBake::PrepareAndRegisterRuntimePhysics(*Component, *Asset, *Record, CollisionError) ||
		((Record->Recipe == EDublinFractureRecipe::StructuralPilot || DublinFractureBake::IsDetailedRecipe(Record->Recipe)) &&
			!DublinStructural::ValidateRegisteredCollision(*Component, Building, *Record, CollisionError)))
	{
		RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		return DestructionFailure(Building.Id + TEXT(": ") + CollisionError + TEXT("; intact building retained"));
	}
	for (int32 Anchor : Record->Anchors) { Component->SetAnchoredByIndex(Anchor, true); }
	FractureComponents.Add(Index, Component);
	FDublinFractureActivity& State = FractureActivity.Add(Index);
	State.Pieces = Record->PieceCount;
	State.HullSlots = DublinDestruction::ReservedHullSlots(*Record);
	State.LeafTransforms = Record->LeafTransforms;
	State.InitialTransforms = Component->GetInitialLocalRestTransforms();
	State.PreviousTransforms = State.InitialTransforms;
	State.LastImpactTime = GetWorld()->GetTimeSeconds();
	State.LastMotionPoll = State.LastImpactTime;
	++AllocatedFractureCollections;
	AllocatedFracturePieces += State.Pieces;
	AllocatedFractureHullSlots += State.HullSlots;
	State.MassLocalBounds.SetNum(State.InitialTransforms.Num());
	for (FBox& Box : State.MassLocalBounds) { Box = FBox(ForceInit); }
	if (Implicits)
	{
		for (int32 Leaf : State.LeafTransforms)
		{
			if (!Implicits->IsValidIndex(Leaf) || !(*Implicits)[Leaf] || !(*Implicits)[Leaf]->HasBoundingBox()) { continue; }
			const auto Bounds = (*Implicits)[Leaf]->BoundingBox();
			State.MassLocalBounds[Leaf] = FBox(FVector(Bounds.Min()), FVector(Bounds.Max()));
		}
	}
	return true;
}

bool ADublinCityWorld::ApplyCollectionImpact(int32 Index, const FDublinImpact& Impact, TArray<int32>& OutHitLeaves)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_ApplyCollectionImpact);
	UGeometryCollectionComponent* Component = FractureComponents.FindRef(Index);
	OutHitLeaves.Reset();
	if (!IsValid(Component)) { return false; }
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
	OutHitLeaves = DublinDestruction::SelectImpactedFractureLeaves(State.LeafTransforms,
		CurrentMassTransforms, ComponentToWorld, Impact, &State.MassLocalBounds);
	// The original root is disabled after its first break. Stable leaf indices target their current
	// cluster parents instead, and unanchoring a struck support also restores Dynamic state in Chaos.
	for (int32 Leaf : OutHitLeaves)
	{
		if (Record && Record->Anchors.Contains(Leaf)) { Component->SetAnchoredByIndex(Leaf, false); }
		Component->ApplyExternalStrain(Leaf, Impact.PositionCm, 0,
			DublinDestruction::StrainPropagationDepth, DublinDestruction::StrainPropagationFactor, Strain);
		// Chaos retains a disabled child's own velocity when strain releases it. Target it once,
		// not the obsolete root or a field's repeated internal-parent samples; keep real masses.
		const FVector WorldMassCenter = ComponentToWorld.TransformPosition(CurrentMassTransforms[Leaf].GetLocation());
		Component->ApplyLinearVelocity(Leaf, DublinDestruction::ImpactVelocityChange(Impact, WorldMassCenter));
	}
	// Debris may have left the admitted source footprint while a previous event was streaming.
	// This is a completed no-op, not an excuse to strain a remote nearest piece or block the queue.
	return !OutHitLeaves.IsEmpty() || CurrentMassTransforms.Num() == State.InitialTransforms.Num();
}

void ADublinCityWorld::AddCommittedDamageSamples(FDublinQueuedWorldImpact& Event, int32 Index,
	const TArray<int32>& HitLeaves)
{
	if (!Event.bDeferredPresentation || Event.bPresentationRequested ||
		Event.DamageSamples.Num() >= DublinImpactFX::MaxDamageSamples || HitLeaves.IsEmpty()) { return; }
	const FDublinCityBuilding& Building = SourceData->Buildings[Index];
	const FDublinFractureActivity& State = FractureActivity.FindChecked(Index);
	UGeometryCollectionComponent* Component = FractureComponents.FindRef(Index);
	const TArray<FTransform> Current = Component->GetCurrentTransforms();
	const auto Geometry = Component->GetRestCollection()->GetGeometryCollection();
	const auto* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(TEXT("Implicits"), FGeometryCollection::TransformGroup);
	int32 Added = 0;
	for (int32 T = 0; T + 2 < Building.Mesh.Triangles.Num() && Added < 4 &&
		Event.DamageSamples.Num() < DublinImpactFX::MaxDamageSamples; T += 3)
	{
		const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T]];
		const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T + 1]];
		const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T + 2]];
		const FVector Surface = FMath::ClosestPointOnTriangleToPoint(Event.Impact.PositionCm - Building.PivotCm, A, B, C);
		for (int32 Leaf : HitLeaves)
		{
			if (!State.MassLocalBounds.IsValidIndex(Leaf) || !State.MassLocalBounds[Leaf].IsValid ||
				!State.InitialTransforms.IsValidIndex(Leaf) || !Current.IsValidIndex(Leaf)) { continue; }
			const FVector MassLocal = State.InitialTransforms[Leaf].InverseTransformPosition(Surface);
			if (!State.MassLocalBounds[Leaf].ExpandBy(2).IsInsideOrOn(MassLocal)) { continue; }
			if (!Implicits || !Implicits->IsValidIndex(Leaf) || !(*Implicits)[Leaf] ||
				!(*Implicits)[Leaf]->Overlap(MassLocal, 2)) { continue; }
			const FVector Position = Component->GetComponentTransform().TransformPosition(Current[Leaf].TransformPosition(MassLocal));
			if (Position.ContainsNaN() || !BuildingBounds[Index].ExpandBy(2).IsInsideOrOn(Position) ||
				FVector::DistSquaredXY(Position, Event.Impact.PositionCm) > FMath::Square(double(Event.Impact.RadiusCm)) ||
				FMath::Abs(Position.Z) > DublinImpactFX::MaximumDamageSampleAbsZCm) { continue; }
			bool bDuplicate = false;
			for (const FDublinConfirmedDamageSample& Sample : Event.DamageSamples)
			{
				if (FVector::DistSquared(Sample.PositionCm, Position) < 10000) { bDuplicate = true; break; }
			}
			if (bDuplicate) { break; }
			FVector Normal = FVector::CrossProduct(C - A, B - A).GetSafeNormal();
			if (Normal.IsNearlyZero()) { break; }
			if (Building.MaterialIds.IsValidIndex(T / 3) && Building.MaterialIds[T / 3] == 0 && Normal.Z < 0) { Normal *= -1; }
			Normal = Component->GetComponentTransform().TransformVectorNoScale(Current[Leaf].TransformVectorNoScale(
				State.InitialTransforms[Leaf].InverseTransformVectorNoScale(Normal))).GetSafeNormal();
			Event.DamageSamples.Add({Position, Normal});
			++Added;
			break;
		}
	}
}

void ADublinCityWorld::RequestCommittedPresentations()
{
	UDublinImpactEffectsSubsystem* Effects = GetWorld() ? GetWorld()->GetSubsystem<UDublinImpactEffectsSubsystem>() : nullptr;
	for (FDublinQueuedWorldImpact& Event : PendingImpacts)
	{
		const bool bTargetsFinished = !Event.Buildings.ContainsByPredicate([](int32 Index) { return Index != INDEX_NONE; });
		if (!Event.bDeferredPresentation || Event.bPresentationRequested ||
			(!Event.bCoreDamageCommitted && !(Event.RemainingCoreBuildings == 0 && Event.FirstCommitWorldTime >= 0) &&
				!(bTargetsFinished && (Event.bSurfaceCommitted || !Event.Buildings.IsEmpty())))) { continue; }
		if (!Effects) { bImpactQueueBlocked = true; DestructionFailure(TEXT("Accepted impact presentation subsystem unavailable; event retained")); return; }
		// Consume before dispatch: no seed-based identity and no retry/double emit after later batches.
		Event.bPresentationRequested = true;
		LastPresentedImpactEventId = Event.EventId;
		++DeferredImpactEffectsRequests;
		Effects->EmitImpact(Event.Impact, Event.DamageSamples);
		if (UDublinWeaponComponent* Recipient = Event.Accounting.Get())
		{
			Recipient->NotifyDeferredImpactEffectsRequested(ImpactEpoch, Event.EventId);
		}
	}
}

void ADublinCityWorld::ProcessImpactQueue()
{
	if (LastImpactProcessingFrame == GFrameCounter) { return; }
	LastImpactProcessingFrame = GFrameCounter;
	LastFrameFractureRegistrations = 0;
	LastFrameOrdinaryFractureRegistrations = 0;
	if (PendingImpacts.IsEmpty() || bImpactQueueBlocked) { return; }
	PrefetchFractureAssets();
	if (bImpactQueueBlocked) { return; }
	struct FWork { int32 Event; int32 Slot; int32 Building; };
	TArray<FWork> Work;
	TMap<int32, TArray<int32>> NewByChunk;
	TSet<int32> Staged;
	int32 Commands = 0;
	const bool bHasMaximumWork = PendingImpacts.ContainsByPredicate([](const FDublinQueuedWorldImpact& Event)
	{
		return DublinDestruction::RegistrationLimitForImpact(Event.Impact) == DublinDestruction::MaxRegistrationsPerFrame;
	});
	// Existing debris is visited across all events before any cold admission; no FIFO starvation.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (int32 Offset = 0; Offset < DublinDestruction::MaxActiveCollections; ++Offset)
		{
			bool bAny = false;
			for (int32 Q = 0; Q < PendingImpacts.Num(); ++Q)
			{
				const int32 EventIndex = (ImpactQueueCursor + Q) % PendingImpacts.Num();
				FDublinQueuedWorldImpact& Event = PendingImpacts[EventIndex];
				const int32 Slot = Event.NextBuilding + Offset;
				if (!Event.Buildings.IsValidIndex(Slot)) { continue; }
				bAny = true;
				const int32 Index = Event.Buildings[Slot];
				if (Index == INDEX_NONE || ((Pass == 0) != RemovedIntactBuildings.Contains(Index))) { continue; }
				if (Commands >= DublinDestruction::MaxCollectionImpactsPerFrame) { break; }
				if (Pass == 1 && !Staged.Contains(Index))
				{
					const bool bNewCollection = !FractureComponents.Contains(Index);
					const bool bMaximum = DublinDestruction::RegistrationLimitForImpact(Event.Impact) == DublinDestruction::MaxRegistrationsPerFrame;
					if (bNewCollection && (LastFrameFractureRegistrations >= DublinDestruction::MaxRegistrationsPerFrame ||
						(!bMaximum && LastFrameOrdinaryFractureRegistrations >= DublinDestruction::MaxOrdinaryRegistrationsPerFrame))) { continue; }
					bool bReady = false;
					if (!PrepareFractureAsset(Index, bReady)) { bImpactQueueBlocked = true; break; }
					if (!bReady) { continue; }
					if (!ActivateBuilding(Index)) { bImpactQueueBlocked = true; break; }
					if (bNewCollection)
					{
						++LastFrameFractureRegistrations;
						LastFrameOrdinaryFractureRegistrations += bMaximum ? 0 : 1;
					}
					Staged.Add(Index);
					NewByChunk.FindOrAdd(BuildingToChunk[Index]).Add(Index);
				}
				Work.Add({EventIndex, Slot, Index});
				++Commands;
			}
			if (!bAny || bImpactQueueBlocked || Commands >= DublinDestruction::MaxCollectionImpactsPerFrame) { break; }
			if (Pass == 1 && (LastFrameFractureRegistrations >= DublinDestruction::MaxRegistrationsPerFrame ||
				(!bHasMaximumWork && LastFrameOrdinaryFractureRegistrations >= DublinDestruction::MaxOrdinaryRegistrationsPerFrame))) { break; }
		}
		if (bImpactQueueBlocked) { break; }
	}
	for (const auto& Pair : NewByChunk)
	{
		TSet<int32> Candidate = RemovedIntactBuildings;
		for (int32 Index : Pair.Value) { Candidate.Add(Index); }
		if (!RewriteBuildingChunk(Pair.Key, Candidate))
		{
			RewriteBuildingChunk(Pair.Key, RemovedIntactBuildings);
			for (int32 Index : Pair.Value)
			{
				if (UGeometryCollectionComponent* Component = FractureComponents.FindRef(Index))
				{
					RemoveInstanceComponent(Component);
					Component->DestroyComponent();
				}
				FractureComponents.Remove(Index);
				FractureActivity.Remove(Index);
			}
			bImpactQueueBlocked = true;
			DestructionFailure(TEXT("Grouped intact collision rewrite failed; uncommitted registrations rolled back"));
			continue;
		}
		RemovedIntactBuildings = MoveTemp(Candidate);
	}
	const double Now = GetWorld()->GetTimeSeconds();
	for (const FWork& Item : Work)
	{
		if (!RemovedIntactBuildings.Contains(Item.Building)) { continue; }
		FDublinQueuedWorldImpact& Event = PendingImpacts[Item.Event];
		TArray<int32> Leaves;
		if (!ApplyCollectionImpact(Item.Building, Event.Impact, Leaves))
		{
			bImpactQueueBlocked = true;
			DestructionFailure(TEXT("Registered collection has no valid damage leaves; accepted hit retained"));
			continue;
		}
		ReleaseFractureLoad(Item.Building);
		AddCommittedDamageSamples(Event, Item.Building, Leaves);
		if (!Leaves.IsEmpty() && Event.FirstCommitWorldTime < 0)
		{
			Event.FirstCommitWorldTime = Now;
			MaxFirstDamageLatencySeconds = FMath::Max(MaxFirstDamageLatencySeconds, float(Now - Event.AcceptedWorldTime));
		}
		const double CoreRadius = FMath::Min(double(Event.Impact.RadiusCm), 6000.0);
		if (DublinDestruction::FootprintDistanceSquared(SourceData->Buildings[Item.Building], Event.Impact.PositionCm)
			<= FMath::Square(CoreRadius))
		{
			Event.bCoreDamageCommitted |= !Leaves.IsEmpty();
			if (--Event.RemainingCoreBuildings == 0)
			{
				MaxCoreDamageLatencySeconds = FMath::Max(MaxCoreDamageLatencySeconds, float(Now - Event.AcceptedWorldTime));
			}
		}
		Event.Buildings[Item.Slot] = INDEX_NONE;
	}
	RequestCommittedPresentations();
	for (FDublinQueuedWorldImpact& Event : PendingImpacts)
	{
		while (Event.Buildings.IsValidIndex(Event.NextBuilding) && Event.Buildings[Event.NextBuilding] == INDEX_NONE) { ++Event.NextBuilding; }
	}
	PendingImpacts.RemoveAll([](const FDublinQueuedWorldImpact& Event)
	{
		return Event.NextBuilding == Event.Buildings.Num() && (!Event.bDeferredPresentation || Event.bPresentationRequested);
	});
	ImpactQueueCursor = PendingImpacts.IsEmpty() ? 0 : (ImpactQueueCursor + 1) % PendingImpacts.Num();
	PeakFrameFractureRegistrations = FMath::Max(PeakFrameFractureRegistrations, LastFrameFractureRegistrations);
	TRACE_BOOKMARK(TEXT("DublinFlight activation batch frame=%llu registered=%d ordinary=%d maximum=%d commands=%d pendingEvents=%d"),
		GFrameCounter, LastFrameFractureRegistrations, LastFrameOrdinaryFractureRegistrations,
		LastFrameFractureRegistrations - LastFrameOrdinaryFractureRegistrations, Commands, PendingImpacts.Num());
}

void ADublinCityWorld::UpdateDestructionCounters()
{
	QueuedImpactCount = PendingImpacts.Num();
	QueuedBuildingHitCount = 0;
	QueuedCoreBuildingHitCount = 0;
	for (const FDublinQueuedWorldImpact& Impact : PendingImpacts)
	{
		QueuedCoreBuildingHitCount += Impact.RemainingCoreBuildings;
		for (int32 I = Impact.NextBuilding; I < Impact.Buildings.Num(); ++I) { QueuedBuildingHitCount += Impact.Buildings[I] != INDEX_NONE ? 1 : 0; }
	}
	ActiveFractureCollections = SleepingFractureCollections = ActiveFracturePieces = MovedFragmentCount = 0;
	AllocatedFractureCollections = FractureComponents.Num();
	AllocatedFracturePieces = AllocatedFractureHullSlots = 0;
	for (const TPair<int32, FDublinFractureActivity>& Pair : FractureActivity)
	{
		AllocatedFracturePieces += Pair.Value.Pieces;
		AllocatedFractureHullSlots += Pair.Value.HullSlots;
		if (Pair.Value.bSleeping) { ++SleepingFractureCollections; }
		if (Pair.Value.SampledAwakeBodies > 0) { ++ActiveFractureCollections; }
		ActiveFracturePieces += Pair.Value.SampledAwakeLeaves;
		MovedFragmentCount += Pair.Value.MovedPieces;
	}
	FracturedBuildingCount = RemovedIntactBuildings.Num();
	WaterMaxDisplacementCm = WaterWaves.MaxHeightCm;
	WaterActiveNodeCount = WaterWaves.bActive ? WaterWaves.WetNodes.Num() : 0;
}

void ADublinCityWorld::UpdateDestructionTickEnabled()
{
	const bool bPendingCold = !bImpactQueueBlocked && PendingImpacts.ContainsByPredicate([this](const FDublinQueuedWorldImpact& Event)
	{
		return Event.Buildings.ContainsByPredicate([this](int32 Index)
		{
			return Index != INDEX_NONE && !RemovedIntactBuildings.Contains(Index);
		});
	});
	const float Interval = bPendingCold ? 0.0f : DublinDestruction::OrdinaryTickIntervalSeconds;
	if (PrimaryActorTick.TickInterval != Interval) { PrimaryActorTick.UpdateTickIntervalAndCoolDown(Interval); }
	const bool bEnabled = WaterWaves.bActive || !FractureComponents.IsEmpty() ||
		(!PendingImpacts.IsEmpty() && !bImpactQueueBlocked);
	if (!bEnabled) { LastDestructionTickWorldTime = -1; }
	SetActorTickEnabled(bEnabled);
}

void ADublinCityWorld::Tick(float DeltaSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_CityDestructionTick);
	Super::Tick(DeltaSeconds);
	if (bBakeInProgress || !GetWorld() || !GetWorld()->IsGameWorld() || !bDestructionInitialized)
	{
		LastDestructionTickWorldTime = -1;
		return;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	// Interval transitions can reset the engine's interval delta. Preserve elapsed water time;
	// the fixed 30 Hz steps, clamps and integration below are unchanged.
	const float WaterDeltaSeconds = LastDestructionTickWorldTime >= 0
		? static_cast<float>(FMath::Max(0.0, Now - LastDestructionTickWorldTime)) : DeltaSeconds;
	LastDestructionTickWorldTime = Now;
	const bool bPollBodies = LastNativeBodyPoll < 0 || Now - LastNativeBodyPoll >= 0.5;
	if (bPollBodies) { SampledAwakeLeafBodies = SampledSleepingLeafBodies = 0; LastNativeBodyPoll = Now; }
	for (TPair<int32, FDublinFractureActivity>& Pair : FractureActivity)
	{
		FDublinFractureActivity& State = Pair.Value;
		UGeometryCollectionComponent* Component = FractureComponents.FindRef(Pair.Key);
		if (!bPollBodies || !IsValid(Component)) { continue; }
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
		bool bAnyAwake = false;
		bool bAnySleeping = false;
		State.SampledAwakeBodies = State.SampledAwakeLeaves = 0;
		if (const FGeometryCollectionPhysicsProxy* Proxy = Component->GetPhysicsProxy())
		{
			for (int32 I = 0; I < Proxy->GetNumTransforms(); ++I)
			{
				const auto* Particle = Proxy->GetParticleByIndex_External(I);
				const auto* Rigid = Particle ? Particle->CastToRigidParticle() : nullptr;
				if (!Rigid || Rigid->Disabled()) { continue; }
				const bool bAwake = Rigid->ObjectState() == Chaos::EObjectStateType::Dynamic;
				const bool bSleeping = Rigid->ObjectState() == Chaos::EObjectStateType::Sleeping;
				bAnyAwake |= bAwake;
				bAnySleeping |= bSleeping;
				State.SampledAwakeBodies += bAwake ? 1 : 0;
				if (State.MassLocalBounds.IsValidIndex(I) && State.MassLocalBounds[I].IsValid)
				{
					SampledAwakeLeafBodies += bAwake ? 1 : 0;
					SampledSleepingLeafBodies += bSleeping ? 1 : 0;
					State.SampledAwakeLeaves += bAwake ? 1 : 0;
				}
			}
		}
		// Native sleep only. Never freeze airborne debris to recover an admission slot.
		State.bSleeping = !bAnyAwake && bAnySleeping;
	}
	UpdateDestructionCounters();
	ProcessImpactQueue();
	if (WaterWaves.bActive)
	{
		WaterAccumulator += FMath::Clamp(static_cast<double>(WaterDeltaSeconds), 0.0, 0.25);
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
	FString Error;
	const EDublinFractureRecipe RequestedRecipe = BakeRecipe;
	const TArray<FString> RequestedIds = BakeBuildingIds;
	const bool bRequestedAll = bBakeAllBuildings;
	if (!DublinFractureBake::ValidateBakeRequest(RequestedRecipe, RequestedIds, bRequestedAll, Error))
	{
		++BakeErrorCount;
		if (BakeErrors.Num() < 128) { BakeErrors.Add(Error); }
		DestructionFailure(Error);
		return;
	}
	// A new derivative may change only vertex attributes. Refresh once per explicit bake request, never per tick.
	BuildCity();
	if (!bCityReady || !SourceData) { DestructionFailure(TEXT("Cannot bake without validated source city data")); return; }
	BakeQueue.Reset();
	if (!RequestedIds.IsEmpty())
	{
		TSet<int32> Selected;
		for (const FString& Id : RequestedIds)
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
		const int32 Count = bRequestedAll ? SourceData->Buildings.Num() : FMath::Clamp(BakeLimit, 1, SourceData->Buildings.Num());
		for (int32 I = 0; I < Count; ++I) { BakeQueue.Add(I); }
	}
	BakeQueueRecipes.Reset();
	for (int32 Index : BakeQueue)
	{
		EDublinFractureRecipe Resolved = RequestedRecipe;
		if (!DublinDetail::ResolveBakeRecipe(SourceData->Buildings[Index], RequestedRecipe, Resolved, Error))
		{
			++BakeErrorCount;
			if (BakeErrors.Num() < 128) { BakeErrors.Add(SourceData->Buildings[Index].Id + TEXT(": ") + Error); }
			DestructionFailure(SourceData->Buildings[Index].Id + TEXT(": ") + Error);
			BakeQueue.Reset();
			BakeQueueRecipes.Reset();
			return;
		}
		BakeQueueRecipes.Add(Resolved);
	}
	FractureLibrary = DublinFractureBake::CreateLibrary(Error);
	if (!FractureLibrary) { DestructionFailure(Error); return; }
	ActiveBakeRecipe = RequestedRecipe;
	BakeCursor = BakeCompleted = BakeErrorCount = 0;
	BakeTotal = BakeQueue.Num();
	BakeErrors.Reset();
	bCancelBake = false;
	bBakeInProgress = !BakeQueue.IsEmpty();
	if (bBakeInProgress)
	{
		// Asset authoring must continue when the editor viewport is not realtime.
		BakeTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(
			this, [this](float)
			{
				if (GEditor && GEditor->PlayWorld)
				{
					bCancelBake = true;
					DestructionFailure(TEXT("Fracture bake stopped because a play session started"));
				}
				TickFractureBake();
				if (!bBakeInProgress) { BakeTickerHandle.Reset(); }
				return bBakeInProgress;
			}));
	}
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
	if (BakeQueueRecipes.Num() != BakeQueue.Num())
	{
		++BakeErrorCount;
		bBakeInProgress = false;
		DestructionFailure(TEXT("Bake queue has no captured per-building recipe plan"));
		UpdateDestructionTickEnabled();
		return;
	}
	const EDublinFractureRecipe CurrentRecipe = BakeQueueRecipes[BakeCursor];
	const FDublinCityBuilding& Building = SourceData->Buildings[BakeQueue[BakeCursor]];
	BakeCurrentSourceId = Building.Id;
	const FDublinFractureRecord* Existing = FractureLibrary->Find(Building.Id);
	bool Skip = false;
	if (Existing && Existing->bReady && Existing->Recipe == CurrentRecipe &&
		Existing->LeafTransforms.Num() == Existing->PieceCount && DublinFractureBake::IsCurrentRecord(Building, *Existing))
	{
		UGeometryCollection* Asset = Existing->Collection.LoadSynchronous();
		FString CollisionError;
		Skip = Asset && !Asset->IsEmpty() && (Asset->HasMeshData() || Asset->HasNaniteData()) &&
			DublinFractureBake::HasCurrentMaterialBindings(*Asset, *this) &&
			DublinFractureBake::ValidateCollisionData(*Asset, *Existing, CollisionError) &&
			((CurrentRecipe != EDublinFractureRecipe::StructuralPilot && !DublinFractureBake::IsDetailedRecipe(CurrentRecipe)) ||
				(Asset->HasNaniteData() && DublinStructural::ValidateBuildingCollision(*Asset, Building, *Existing, CollisionError)));
	}
	if (!Skip)
	{
		FDublinFractureRecord Record;
		FString Error;
		if (!DublinFractureBake::BakeBuilding(Building, *this, Record, Error, CurrentRecipe))
		{
			++BakeErrorCount;
			Record.Error = Error;
			if (BakeErrors.Num() < 128) { BakeErrors.Add(Building.Id + TEXT(": ") + Error); }
			DestructionFailure(Building.Id + TEXT(": ") + Error);
		}
		else if (!DublinFractureBake::PublishRecord(*FractureLibrary, Record, DublinFractureBake::SaveLibrary, Error))
		{
			++BakeErrorCount;
			if (BakeErrors.Num() < 128) { BakeErrors.Add(Building.Id + TEXT(": ") + Error); }
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
