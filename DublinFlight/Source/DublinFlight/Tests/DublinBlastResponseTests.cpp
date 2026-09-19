#include "City/DublinCityDestruction.h"
#include "Weapons/DublinWeaponModel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastGameCurveTest,
	"DublinFlight.Destruction.BlastResponse.Native.GameRadiusAndIndependentCrater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastGameCurveTest::RunTest(const FString& Parameters)
{
	const FDublinImpact Low = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1, {});
	const FDublinImpact High = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	TestEqual(TEXT("One game ton retains six metre radius"), Low.RadiusCm, 600.0f);
	TestTrue(TEXT("Maximum game yield reaches 120m radius, not diameter"), FMath::IsNearlyEqual(High.RadiusCm, 12000.0f, .1f));
	const FDublinImpact Ground = DublinWeapons::MakeGroundImpact(High);
	TestTrue(TEXT("Terrain footprint retains the independent original 33.74m curve"),
		FMath::IsNearlyEqual(Ground.RadiusCm, 3374.048f, .1f));
	TestEqual(TEXT("Terrain depth does not follow expanded structural radius"), Ground.CraterDepthCm, High.CraterDepthCm);
	TestEqual(TEXT("Cannon radius unchanged"), DublinWeapons::MakeImpact(EDublinImpactKind::Cannon, 1000, {}).RadiusCm, FDublinImpact().RadiusCm);
	TestEqual(TEXT("No demolition velocity beyond approved radius"),
		DublinDestruction::ImpactVelocityChange(High, FVector(12001, 0, 0)), FVector::ZeroVector);
	TestTrue(TEXT("Strong core remains bounded to sixty metres"),
		DublinDestruction::ImpactVelocityChange(High, FVector(6000, 0, 0)).Size() <= 5000.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastCatalogBudgetTest,
	"DublinFlight.Destruction.BlastResponse.Native.FiniteCatalogPreflightAndLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastCatalogBudgetTest::RunTest(const FString& Parameters)
{
	FDublinFractureRecord Record;
	Record.SourceId = TEXT("catalog-budget");
	Record.PieceCount = 384;
	FString Error;
	DublinDestruction::FCatalogBudget Budget;
	for (int32 I = 0; I < DublinDestruction::MaxActiveCollections; ++I)
	{
		if (!TestTrue(TEXT("Entire 1044-source legacy catalog fits, not just awake subset"),
			DublinDestruction::AddCatalogRecord(Record, Budget, Error))) { return false; }
	}
	const auto Full = Budget;
	TestFalse(TEXT("Collection 1045 is rejected before admission"), DublinDestruction::AddCatalogRecord(Record, Budget, Error));
	TestEqual(TEXT("Rejected record does not consume allocation"), Budget.Collections, Full.Collections);
	TestEqual(TEXT("Rejected record does not mutate leaf cost"), Budget.LeafSlots, Full.LeafSlots);
	Budget = {};
	Budget.LeafSlots = DublinDestruction::MaxActivePieces - Record.PieceCount;
	TestTrue(TEXT("Exact leaf ceiling fits"), DublinDestruction::AddCatalogRecord(Record, Budget, Error));
	TestFalse(TEXT("Beyond leaf ceiling fails explicitly"), DublinDestruction::AddCatalogRecord(Record, Budget, Error));
	Budget = {};
	Budget.HullSlots = DublinDestruction::MaxCatalogHullSlots;
	TestFalse(TEXT("Collision-hull ceiling is independent of leaf ceiling"), DublinDestruction::AddCatalogRecord(Record, Budget, Error));
	Budget = {};
	Record.HullCount = -1;
	TestFalse(TEXT("Negative hull metadata rejected"), DublinDestruction::AddCatalogRecord(Record, Budget, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastFootprintTest,
	"DublinFlight.Destruction.BlastResponse.Native.ActualFootprintAndLeafBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastFootprintTest::RunTest(const FString& Parameters)
{
	FDublinCityBuilding Building;
	Building.Mesh.VerticesCm = {FVector(0, 0, 6000), FVector(1000, 0, 6000), FVector(0, 1000, 6000)};
	Building.Mesh.Triangles = {0, 1, 2};
	TestEqual(TEXT("Tall roof footprint includes support XY"), DublinDestruction::FootprintDistanceSquared(Building, FVector(100, 100, 0)), 0.0);
	TestTrue(TEXT("AABB corner outside actual triangle is not occupied"), DublinDestruction::FootprintDistanceSquared(Building, FVector(900, 900, 0)) > 0);
	FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	TArray<FTransform> Centers{FTransform(FVector(12100, 0, 100)), FTransform(FVector(15000, 0, 6000))};
	TArray<FBox> Bounds{FBox(FVector(-200, -100, -100), FVector(200, 100, 100)),
		FBox(FVector(-100), FVector(100))};
	const TArray<int32> Hit = DublinDestruction::SelectImpactedFractureLeaves({0, 1}, Centers, FTransform::Identity, Impact, &Bounds);
	TestTrue(TEXT("Actual leaf bounds overlapping radius are selected even when mass centre is outside"), Hit.Contains(0));
	TestFalse(TEXT("Unrelated leaf beyond radius remains intact"), Hit.Contains(1));
	Centers[0] = FTransform(FVector(16000, 0, 100));
	TestTrue(TEXT("When all current debris leaves the footprint, maximum blast does not strain a remote fallback"),
		DublinDestruction::SelectImpactedFractureLeaves({0, 1}, Centers, FTransform::Identity, Impact, &Bounds).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastTallRoofReachTest,
	"DublinFlight.Destruction.BlastResponse.Native.MaximumYieldReachesTallBuildingSupports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastTallRoofReachTest::RunTest(const FString& Parameters)
{
	// Game-balance regression, not an explosives model: a 60m building with six occupied floors.
	TArray<FTransform> Centers;
	TArray<int32> Leaves;
	for (int32 Floor = 0; Floor < 6; ++Floor)
	{
		Leaves.Add(Centers.Emplace(FVector(0, 0, 500 + Floor * 1000)));
	}
	FDublinImpact Low = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1, {});
	Low.PositionCm = FVector(0, 0, 6000);
	FDublinImpact High = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	High.PositionCm = Low.PositionCm;
	const TArray<int32> LowLeaves = DublinDestruction::SelectImpactedFractureLeaves(
		Leaves, Centers, FTransform::Identity, Low);
	const TArray<int32> HighLeaves = DublinDestruction::SelectImpactedFractureLeaves(
		Leaves, Centers, FTransform::Identity, High);
	TestTrue(TEXT("Small roof hit remains local and preserves bottom-floor support"), !LowLeaves.Contains(0));
	TestTrue(TEXT("Maximum game yield reaches bottom-floor support, not only upper leaves"), HighLeaves.Contains(0));
	TestTrue(TEXT("Maximum game yield covers at least five of six floor bands"), HighLeaves.Num() >= 5);
	TestTrue(TEXT("Maximum game yield materially exceeds low-yield coverage"), HighLeaves.Num() >= LowLeaves.Num() * 4);
	TestTrue(TEXT("Requested support velocity is finite and within the unchanged native speed cap"),
		!DublinDestruction::ImpactVelocityChange(High, Centers[0].GetLocation()).ContainsNaN()
		&& DublinDestruction::ImpactVelocityChange(High, Centers[0].GetLocation()).Size()
			<= DublinDestruction::MaxFractureVelocityChangeCmPerSecond);
	AddInfo(FString::Printf(TEXT("roof=6000cm lowRadius=%.2f highRadius=%.2f lowFloors=%d highFloors=%d supportSelected=%d"),
		Low.RadiusCm, High.RadiusCm, LowLeaves.Num(), HighLeaves.Num(), HighLeaves.Contains(0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastTypicalRoofSelectionTest,
	"DublinFlight.Destruction.BlastResponse.Native.TypicalRoofSelectionAndLeafItemIndices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastTypicalRoofSelectionTest::RunTest(const FString& Parameters)
{
	TArray<FTransform> Centers;
	TArray<int32> Leaves;
	for (int32 Floor = 0; Floor < 6; ++Floor)
	{
		const int32 Leaf = Centers.Emplace(FVector(500, 0, 150 + Floor * 300));
		Leaves.Add(Leaf);
		const FGeometryCollectionItemIndex Item = FGeometryCollectionItemIndex::CreateFromExistingItemIndex(Leaf);
		TestFalse(TEXT("Nonnegative authored leaf index is not decoded as an internal cluster"), Item.IsInternalCluster());
		TestEqual(TEXT("Native item decoding preserves the authored transform index"), Item.GetTransformIndex(), Leaf);
	}
	FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	Impact.PositionCm = FVector(0, 0, 1800);
	const TArray<int32> Selected = DublinDestruction::SelectImpactedFractureLeaves(
		Leaves, Centers, FTransform::Identity, Impact);
	TestEqual(TEXT("Current maximum yield selects all floor bands beneath a typical 18m roof"), Selected.Num(), Leaves.Num());
	TestTrue(TEXT("The typical roof fixture includes its bottom support"), Selected.Contains(0));
	return true;
}

#if WITH_EDITOR

#include "City/DublinCityWorld.h"
#include "Dom/JsonObject.h"
#include "DublinFlightPawn.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StreamableManager.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Materials/Material.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/DublinProjectile.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Effects/DublinImpactEffectsSubsystem.h"

namespace
{
	FDublinImpact ConfiguredWeaponImpact(const UDublinWeaponComponent& Weapon, float Yield)
	{
		DublinWeapons::FBombCurve Curve;
		Curve.RadiusAtOneCm = Weapon.BombRadiusAtOneCm;
		Curve.DepthAtOneCm = Weapon.BombDepthAtOneCm;
		Curve.StrengthAtOne = Weapon.BombStrengthAtOne;
		Curve.Exponent = Weapon.BombYieldExponent;
		Curve.MaxRadiusCm = Weapon.BombMaximumRadiusCm;
		Curve.MaxDepthCm = Weapon.BombMaximumDepthCm;
		Curve.MaxStrength = Weapon.BombMaximumStrength;
		return DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, Yield, Curve);
	}

	class FRunMaximumOutboxWhenReady final : public IAutomationLatentCommand
	{
	public:
		FRunMaximumOutboxWhenReady(FAutomationTestBase& InTest, UNiagaraSystem* InSystem, TFunction<bool()> InBody)
			: Test(InTest), System(InSystem), Body(MoveTemp(InBody)) {}

		virtual bool Update() override
		{
			if (Started == 0) { Started = FPlatformTime::Seconds(); }
			System->PollForCompilationComplete();
			if (System->NeedsRequestCompile() && !System->HasOutstandingCompilationRequests(true))
			{
				DublinImpactFX::PrepareSystem(System.Get());
			}
			const bool bCompiling = System->HasOutstandingCompilationRequests(true);
			const bool bNeedsCompile = System->NeedsRequestCompile();
			const bool bValid = System->IsValid();
			const bool bReady = System->IsReadyToRun();
			if (bCompiling || bNeedsCompile || (bValid && !bReady))
			{
				if (FPlatformTime::Seconds() - Started < 120.0) { return false; }
				Test.AddError(FString::Printf(TEXT("Maximum outbox asset readiness timed out before world creation: pending=%d valid=%d ready=%d needs_compile=%d"),
					bCompiling, bValid, bReady, bNeedsCompile));
				return true;
			}
			if (!Test.TestTrue(TEXT("Maximum outbox asset is compiled and GPU-ready before world creation"), bValid && bReady))
			{
				return true;
			}
			FString Failure;
			const bool bContractValid = DublinImpactFX::ValidateMaximumAsset(System.Get(), Failure);
			if (!Test.TestTrue(TEXT("Maximum outbox authored contract: ") + Failure, bContractValid))
			{
				return true;
			}
			Test.AddInfo(FString::Printf(TEXT("Maximum outbox asset ready before world creation after %.3fs: pending=0 valid=1 ready=1 needs_compile=0"),
				FPlatformTime::Seconds() - Started));
			Test.TestTrue(TEXT("Outbox fixture reached all original assertions"), Body());
			return true;
		}

	private:
		FAutomationTestBase& Test;
		TStrongObjectPtr<UNiagaraSystem> System;
		TFunction<bool()> Body;
		double Started = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastWeaponConfigurationTest,
	"DublinFlight.Destruction.BlastResponse.Native.WeaponComponentMaximumYieldConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastWeaponConfigurationTest::RunTest(const FString& Parameters)
{
	UDublinWeaponComponent* Weapon = NewObject<UDublinWeaponComponent>();
	if (!TestNotNull(TEXT("Native weapon component with real reflected property defaults"), Weapon)) { return false; }
	Weapon->BombYieldTonsTNT = 1000;
	TestEqual(TEXT("Weapon component radius cap is 120m, not the old 50m cap"), Weapon->BombMaximumRadiusCm, 12000.0f);
	const FDublinImpact Maximum = ConfiguredWeaponImpact(*Weapon, Weapon->BombYieldTonsTNT);
	TestTrue(TEXT("Launch property wiring produces 120m maximum radius"),
		FMath::IsNearlyEqual(Maximum.RadiusCm, 12000.0f, .1f));
	Weapon->BombYieldExponent = .1f;
	const FDublinImpact DifferentDepth = ConfiguredWeaponImpact(*Weapon, Weapon->BombYieldTonsTNT);
	TestEqual(TEXT("Depth/strength exponent never restores the old radius growth"), DifferentDepth.RadiusCm, Maximum.RadiusCm);
	TestTrue(TEXT("Depth exponent remains independently configurable"), DifferentDepth.CraterDepthCm < Maximum.CraterDepthCm);
	Weapon->BombMaximumRadiusCm = 5000;
	TestEqual(TEXT("Deliberate reflected radius-cap override remains respected"),
		ConfiguredWeaponImpact(*Weapon, Weapon->BombYieldTonsTNT).RadiusCm, 5000.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastCatalogAdmissionTest,
	"DublinFlight.Destruction.BlastResponse.Native.CatalogAndEventAdmissionBeforeMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastCatalogAdmissionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Admission fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	ADublinCityWorld* City = World->SpawnActorDeferred<ADublinCityWorld>(ADublinCityWorld::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	City->bAutoBuild = false;
	City->FinishSpawning(FTransform::Identity);
	City->SourceData = MakeUnique<FDublinCityData>();
	City->FractureLibrary = NewObject<UDublinCityFractureLibrary>(City);
	City->bDestructionReady = true;
	City->bCatalogBudgetValid = false;
	AddExpectedError(TEXT("Destruction not ready"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Ready flag cannot bypass whole-catalog reservation"), City->ApplyImpact(FDublinImpact()));
	City->bCatalogBudgetValid = true;
	City->PendingImpacts.SetNum(DublinDestruction::MaxQueuedImpacts);
	AddExpectedError(TEXT("Finite impact event queue/identity capacity exhausted"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Full finite queue rejects before mutating ground or reserving an identity"), City->ApplyImpact(FDublinImpact()));
	TestEqual(TEXT("No impact accepted after either failed reservation"), City->AcceptedImpactCount, 0);
	TestEqual(TEXT("No source collision removed"), City->RemovedIntactBuildings.Num(), 0);
	TestEqual(TEXT("No crater changed"), City->CraterChangedNodeCount, 0);
	TestEqual(TEXT("No effect request made"), City->DeferredImpactEffectsRequests, 0);
	TestEqual(TEXT("No event identity consumed"), City->NextImpactEventId, uint64(1));
	TestFalse(TEXT("Launch throttles while event capacity is reserved"), City->HasWeaponImpactCapacity());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastDuplicateAllocationTest,
	"DublinFlight.Destruction.BlastResponse.Native.DuplicateAllocationAndRepeatBypassesColdHead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastDuplicateAllocationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Native repeat fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	ADublinCityWorld* City = World->SpawnActorDeferred<ADublinCityWorld>(ADublinCityWorld::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	City->bAutoBuild = false;
	City->FinishSpawning(FTransform::Identity);
	City->SourceData = MakeUnique<FDublinCityData>();
	FDublinCityBuilding Building;
	Building.Id = TEXT("repeat-native");
	Building.Mesh.VerticesCm = {FVector(-50, -50, 200), FVector(50, -50, 200), FVector(-50, 50, 200)};
	Building.Mesh.Triangles = {0, 1, 2};
	City->SourceData->Buildings.Add(Building);
	City->BuildingBounds.Add(FBox(FVector(-50, -50, 0), FVector(50, 50, 200)));
	Building.Id = TEXT("cold-head");
	City->SourceData->Buildings.Add(Building);
	City->FractureLibrary = NewObject<UDublinCityFractureLibrary>(City);
	UGeometryCollection* Asset = NewObject<UGeometryCollection>(City);
	const auto Geometry = Asset->GetGeometryCollection();
	FDublinFractureRecord Record;
	Record.LeafTransforms.Add(Geometry->AppendGeometry(*GeometryCollection::MakeCubeElement(FTransform(FVector(0, 0, 50)), FVector(100))));
	Record.LeafTransforms.Add(Geometry->AppendGeometry(*GeometryCollection::MakeCubeElement(FTransform(FVector(0, 0, 150)), FVector(100))));
	Record.PieceCount = 2;
	Record.Anchors = {Record.LeafTransforms[0]};
	FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(Geometry.Get());
	Asset->InvalidateCollection();
	Record.RootTransform = Asset->GetRootIndex();
	Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Geometry);
	Anchoring.AddAnchoredAttribute();
	Anchoring.SetAnchored(Record.Anchors[0], true);
	Anchoring.SetInitialDynamicState(Record.Anchors[0], Chaos::EObjectStateType::Kinematic);
	Asset->Materials = {UMaterial::GetDefaultMaterial(MD_Surface), UMaterial::GetDefaultMaterial(MD_Surface)};
	Asset->SetEnableNanite(false);
	Asset->ReindexMaterialSections();
	FString Error;
	if (!TestTrue(TEXT("Real touching leaf connection graph"), DublinFractureBake::BuildConnectionGraph(*Asset, Error))) { AddError(Error); return false; }
	Asset->UpdateGeometryDependentProperties();
	Asset->CreateSimulationData();
	Asset->RebuildRenderData();
	if (!TestTrue(TEXT("Native activation fixture has built render data, not only simulation data"),
		!Asset->IsEmpty() && Asset->HasVisibleGeometry() && Asset->HasMeshData())) { return false; }
	Record.SourceId = City->SourceData->Buildings[0].Id;
	Record.SourceDigest = DublinDestruction::BuildingDigest(City->SourceData->Buildings[0]);
	Record.Collection = Asset;
	if (!TestTrue(TEXT("Transient fracture record resolves to its authored native asset"),
		Record.Collection.Get() == Asset)) { return false; }
	Record.bReady = true;
	City->FractureLibrary->Records.Add(Record);
	if (!TestTrue(TEXT("Real cooked collection registers"), City->ActivateBuilding(0))) { return false; }
	UGeometryCollectionComponent* Original = City->FractureComponents.FindRef(0);
	TestTrue(TEXT("Repeat allocation succeeds without making another collection"), City->ActivateBuilding(0));
	TestTrue(TEXT("Source owns exactly the original native collection"), City->FractureComponents.FindRef(0) == Original);
	TestEqual(TEXT("Only two leaf slots allocated"), City->AllocatedFracturePieces, 2);
	City->RemovedIntactBuildings.Add(0);
	City->FractureActivity.FindChecked(0).bSleeping = true;
	City->UpdateDestructionCounters();
	TestEqual(TEXT("Native sleeping never frees resident allocation"), City->AllocatedFracturePieces, 2);
	const FSoftObjectPath Target(TEXT("/Game/Tests/DublinColdRepeatFixture.DublinColdRepeatFixture"));
	Record.SourceId = City->SourceData->Buildings[1].Id;
	Record.SourceDigest = DublinDestruction::BuildingDigest(City->SourceData->Buildings[1]);
	Record.Collection = TSoftObjectPtr<UGeometryCollection>(Target);
	City->FractureLibrary->Records.Add(Record);
	FStreamableManager Loader;
	const auto Stalled = Loader.RequestAsyncLoad(Target, FStreamableDelegate(),
		FStreamableManager::DefaultAsyncLoadPriority, false, true);
	if (!TestTrue(TEXT("Native cold request deliberately stalled"), Stalled.IsValid())) { return false; }
	ON_SCOPE_EXIT { City->CancelPendingFractureLoad(); };
	City->PendingFractureLoad = Stalled;
	City->PendingFracturePath = Target;
	City->PendingFractureBuilding = 1;
	FDublinQueuedWorldImpact Cold;
	Cold.Buildings = {1};
	Cold.EventId = 1;
	City->PendingImpacts.Add(Cold);
	FDublinQueuedWorldImpact Repeat;
	Repeat.Impact.PositionCm = FVector(0, 0, 200);
	Repeat.Buildings = {0};
	Repeat.EventId = 2;
	Repeat.RemainingCoreBuildings = 1;
	City->PendingImpacts.Add(Repeat);
	City->ProcessImpactQueue();
	TestFalse(TEXT("Cold stream is waiting, not a failed queue"), City->bImpactQueueBlocked);
	TestEqual(TEXT("Active repeat consumed while earlier cold event remains"), City->PendingImpacts.Num(), 1);
	TestEqual(TEXT("Remaining event is the cold head"), City->PendingImpacts[0].EventId, uint64(1));
	TestEqual(TEXT("Repeat registers no collection"), City->LastFrameFractureRegistrations, 0);
	TestEqual(TEXT("Repeat retains the same resident allocation"), City->AllocatedFracturePieces, 2);
	TestTrue(TEXT("Cold asset was neither flushed nor started"), Stalled->IsStalled());
	City->ProcessImpactQueue();
	TestEqual(TEXT("Second processing call in one engine frame changes nothing"), City->PendingImpacts.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastOutboxTest,
	"DublinFlight.Destruction.BlastResponse.Native.CommittedOutboxExactlyOnceAndWeakWeapon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastOutboxTest::RunTest(const FString& Parameters)
{
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, DublinImpactFX::MaximumSystemPath, nullptr, LOAD_NoWarn);
	if (!TestNotNull(TEXT("Required maximum system for the real outbox recipient"), System)) { return false; }
	DublinImpactFX::PrepareSystem(System);
	ADD_LATENT_AUTOMATION_COMMAND(FRunMaximumOutboxWhenReady(*this, System, [this]()
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Outbox fixture world"), World)) { return false; }
		ON_SCOPE_EXIT { World->DestroyWorld(false); };
		if (!TestNotNull(TEXT("Real world effects recipient"), World->GetSubsystem<UDublinImpactEffectsSubsystem>())) { return false; }
		ADublinCityWorld* City = World->SpawnActorDeferred<ADublinCityWorld>(ADublinCityWorld::StaticClass(),
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		City->bAutoBuild = false;
		City->FinishSpawning(FTransform::Identity);
		UDublinWeaponComponent* Weapon = NewObject<UDublinWeaponComponent>(City);
		FDublinQueuedWorldImpact Event;
		Event.Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
		Event.Impact.Seed = 7;
		Event.bDeferredPresentation = true;
		Event.Accounting = Weapon;
		Event.EventId = 1;
		City->PendingImpacts.Add(Event);
		City->RequestCommittedPresentations();
		TestEqual(TEXT("Acceptance without committed mutation cannot present"), City->DeferredImpactEffectsRequests, 0);
		City->PendingImpacts[0].bSurfaceCommitted = true;
		City->RequestCommittedPresentations();
		City->RequestCommittedPresentations();
		TestEqual(TEXT("Ground-only maximum emits once with zero structural samples"), City->DeferredImpactEffectsRequests, 1);
		TestEqual(TEXT("Weapon effects accounting increments only once"), Weapon->EffectsRequests, 1);
		TestEqual(TEXT("Ground-only event does not invent building samples"), City->PendingImpacts[0].DamageSamples.Num(), 0);
		Event.EventId = 2;
		Event.bSurfaceCommitted = true;
		City->PendingImpacts.Add(Event);
		Weapon->MarkAsGarbage();
		City->RequestCommittedPresentations();
		City->RequestCommittedPresentations();
		TestEqual(TEXT("City still presents after weak weapon recipient disappears"), City->DeferredImpactEffectsRequests, 2);
		TestEqual(TEXT("Repeated seed is not used as event identity"), City->LastPresentedImpactEventId, uint64(2));
		return true;
	}));
	return true;
}

namespace DublinBlastResponse
{
	constexpr double ResponseLimitSeconds = 1.0;
	constexpr double ObservationSeconds = 4.0;
	constexpr int32 HistoricalPiecePressure = 1536;
	const TCHAR* TallSourceId = TEXT("osm/way/1488413001");
	struct FBudgetCase { const TCHAR* Name; const TCHAR* SourceId; bool bOccupied; };
	const FBudgetCase BudgetCases[] = {
		{TEXT("SmallRotated.EmptyBudget"), TEXT("osm/way/233804861"), false},
		{TEXT("SmallRotated.OccupiedBudget"), TEXT("osm/way/233804861"), true},
		{TEXT("IrregularFootprint.EmptyBudget"), TEXT("osm/way/233804877"), false},
		{TEXT("IrregularFootprint.OccupiedBudget"), TEXT("osm/way/233804877"), true},
		{TEXT("LargeBlock.EmptyBudget"), TEXT("osm/way/389853640"), false},
		{TEXT("LargeBlock.OccupiedBudget"), TEXT("osm/way/389853640"), true}
	};

	struct FResult
	{
		bool bCompleted = false;
		double ChangedRoofAreaCm2 = 0;
		double FirstDamageSeconds = -1;
		int32 ChangedBuildings = 0;
		int32 MovedLeaves = 0;
		int32 MovedSupports = 0;
	};

	struct FRoofProbe
	{
		int32 Building = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
		double AreaCm2 = 0;
	};

	void PointField(FJsonObject& Object, const TCHAR* Name, const FVector& Point)
	{
		Object.SetArrayField(Name, {MakeShared<FJsonValueNumber>(Point.X),
			MakeShared<FJsonValueNumber>(Point.Y), MakeShared<FJsonValueNumber>(Point.Z)});
	}

	FBox Bounds(const FDublinCityBuilding& Building)
	{
		FBox Box(ForceInit);
		for (const FVector& V : Building.Mesh.VerticesCm) { Box += V + Building.PivotCm; }
		return Box;
	}

	TArray<FRoofProbe> RoofProbes(const FDublinCityBuilding& Building, int32 Index)
	{
		TArray<FRoofProbe> Probes;
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			if (Building.MaterialIds[T] != 0) { continue; }
			const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]];
			const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]];
			const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]];
			const double Area = FMath::Abs(FVector::CrossProduct(B - A, C - A).Z) * 0.5;
			if (Area > 1) { Probes.Add({Index, (A + B + C) / 3 + Building.PivotCm, Area}); }
		}
		Probes.Sort([](const FRoofProbe& A, const FRoofProbe& B) { return A.AreaCm2 > B.AreaCm2; });
		return Probes;
	}

	// Each case owns a fresh PIE. Only the parent runs/builds these fixtures.
	// This seam deliberately bypasses key/focus/cooldown, not projectile sweep, resolution,
	// weapon dispatch, city admission, fair activation, native budgets or Chaos.
	class FShot final : public IAutomationLatentCommand
	{
	public:
		FShot(FAutomationTestBase& InTest, float InYield, bool bInOccupied,
			TSharedRef<FResult> InResult, FString InDirectory, FString InSourceId = TallSourceId,
			bool bInLatencyOnly = false, bool bInCold = false)
			: Test(InTest), Yield(InYield), bOccupied(bInOccupied), bLatencyOnly(bInLatencyOnly), bCold(bInCold), Result(InResult),
				Directory(MoveTemp(InDirectory)), SourceId(MoveTemp(InSourceId))
		{
		}

		virtual bool Update() override
		{
			if (StartedWall == 0) { StartedWall = FPlatformTime::Seconds(); }
			if (FPlatformTime::Seconds() - StartedWall > 75) { return Fail(TEXT("Bootstrap/physics wall watchdog expired")); }
			if (!World.IsValid()) { return Bootstrap(); }
			if (World->bIsTearingDown || !City.IsValid() || !Weapon.IsValid()) { return Fail(TEXT("Owned PIE ended")); }
			if (City->bImpactQueueBlocked) { return Fail(TEXT("Activation/cooked collision rejected: ") + City->LastDestructionError); }
			if (City->ActiveFractureCollections > DublinDestruction::MaxActiveCollections
				|| City->ActiveFracturePieces > DublinDestruction::MaxActivePieces
				|| City->QueuedImpactCount > DublinDestruction::MaxQueuedImpacts)
			{
				return Fail(TEXT("Runtime exceeded its explicitly configured finite physics/admission limits"));
			}
			const double Now = World->GetTimeSeconds();
			if (Now <= LastWorld) { return false; }
			LastWorld = Now;
			if (bPriming)
			{
				if (Now - PrimingWorld > (bLatencyOnly ? 1.0 : 2.0))
				{
					return Fail(TEXT("Occupied-budget preparation exceeded its bound; cannot isolate the pre-sleep response window"));
				}
				if (City->QueuedImpactCount != 0) { return false; }
				const bool bOldPressure = City->AllocatedFracturePieces + TargetRecord.PieceCount > HistoricalPiecePressure;
				if (!bOldPressure) { return Fail(TEXT("Preparation did not exceed the historical 1536-leaf admission pressure")); }
				bPriming = false;
				return Launch();
			}
			if (!bAccepted)
			{
				if (Weapon->RejectedImpacts != RejectedBefore) { return Fail(TEXT("Real roof projectile rejected: ") + Weapon->LastFailure); }
				if (Weapon->AcceptedBombImpacts == AcceptedBefore)
				{
					if (Now - LaunchWorld > 1) { return Fail(TEXT("Swept rooftop projectile did not dispatch within one world second")); }
					return false;
				}
				if (Weapon->AcceptedBombImpacts != AcceptedBefore + 1) { return Fail(TEXT("Unexpected extra projectile impact")); }
				Impact = Weapon->GetLastImpact();
				if (Impact.bWater || FVector::Dist(Impact.PositionCm, Roof) > 50
					|| Impact.Normal.Z < 0.5 || !FMath::IsNearlyEqual(Impact.YieldTonsTNT, Yield))
				{
					return Fail(TEXT("Projectile did not hit the fixed roof with the requested yield"));
				}
				bAccepted = true;
				ImpactWorld = Now;
				ImpactWall = FPlatformTime::Seconds();
				Evidence->SetNumberField(TEXT("impact_world_seconds"), Now);
				PointField(*Evidence, TEXT("projectile_impact_point_cm"), Impact.PositionCm);
				Evidence->SetNumberField(TEXT("projectile_to_roof_trace_distance_cm"), FVector::Dist(Impact.PositionCm, Roof));
				Evidence->SetNumberField(TEXT("impact_radius_cm"), Impact.RadiusCm);
				Evidence->SetNumberField(TEXT("impact_strength"), Impact.Strength);
				Evidence->SetNumberField(TEXT("queued_hits_at_acceptance"), City->QueuedBuildingHitCount);
				Evidence->SetNumberField(TEXT("effects_requests_at_acceptance"), Weapon->EffectsRequests);
			}
			Observe(Now);
			if (bLatencyOnly && Now - ImpactWorld >= ResponseLimitSeconds + 0.1)
			{
				Result->bCompleted = true;
				Test.TestTrue(TEXT("Accepted maximum-yield roof impact changes the struck roof collision within one second despite occupied physics capacity"),
					FirstTargetDamageSeconds >= 0 && FirstTargetDamageSeconds <= ResponseLimitSeconds);
				return Finish();
			}
			if (Now - ImpactWorld >= ObservationSeconds)
			{
				Result->bCompleted = true;
				Test.TestTrue(TEXT("Visible/collidable damage responds within preloaded one-second or cold four-second limit"),
					Result->FirstDamageSeconds >= 0 && Result->FirstDamageSeconds <= (bCold ? ObservationSeconds : ResponseLimitSeconds));
				Test.TestTrue(TEXT("At least one real target fragment moves more than 25cm"), Result->MovedLeaves > 0);
				Test.TestTrue(TEXT("Fragments separate, rather than merely translating an intact building"), MaxSeparationCm > 25);
				if (Yield == 1000)
				{
					Test.TestTrue(TEXT("Maximum rooftop bomb moves at least half the target's authored leaves"),
						Result->MovedLeaves * 2 >= TargetRecord.PieceCount);
					Test.TestTrue(TEXT("Maximum rooftop bomb releases and moves an authored lower support"), Result->MovedSupports > 0);
					Test.TestTrue(TEXT("Maximum rooftop bomb changes roofs on at least three source buildings"), Result->ChangedBuildings >= 3);
					if (SourceId != TallSourceId)
					{
						Test.TestTrue(TEXT("Struck roof responds within preloaded one-second or cold four-second limit"),
							FirstTargetDamageSeconds >= 0 && FirstTargetDamageSeconds <= (bCold ? ObservationSeconds : ResponseLimitSeconds));
					}
					Test.TestTrue(TEXT("Core activation completes within a few seconds"),
						City->QueuedCoreBuildingHitCount == 0 && City->MaxCoreDamageLatencySeconds <= ObservationSeconds);
					Test.TestEqual(TEXT("Maximum projectile owns exactly one committed effects request"), Weapon->EffectsRequests, 1);
				}
				Test.TestTrue(TEXT("An unhit control roof remains intact"), !bControlChanged);
				return Finish();
			}
			if (FPlatformTime::Seconds() - ImpactWall > 15) { return Fail(TEXT("Four world seconds of physical observation exceeded fifteen wall seconds")); }
			return false;
		}

	private:
		FAutomationTestBase& Test;
		float Yield;
		bool bOccupied;
		bool bLatencyOnly;
		bool bCold;
		TSharedRef<FResult> Result;
		FString Directory, SourceId;
		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Samples;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ADublinCityWorld> City;
		TWeakObjectPtr<ADublinFlightPawn> Pawn;
		TWeakObjectPtr<UDublinWeaponComponent> Weapon;
		TWeakObjectPtr<ADublinProjectile> Projectile;
		TArray<TStrongObjectPtr<UGeometryCollection>> ResidentAssets;
		TArray<FRoofProbe> Probes;
		FRoofProbe Control;
		TArray<FBox> SourceBounds;
		TArray<FTransform> InitialLeaves;
		FDublinFractureRecord TargetRecord;
		FDublinImpact Impact;
		FVector Roof = FVector::ZeroVector;
		int32 Target = INDEX_NONE;
		int32 AcceptedBefore = 0;
		int32 RejectedBefore = 0;
		double StartedWall = 0, LastWorld = -1, PrimingWorld = 0, LaunchWorld = 0, ImpactWorld = 0, ImpactWall = 0;
		double LastSample = -1, ActivationSeconds = -1, PhysicsReadySeconds = -1, MaxSeparationCm = 0;
		double FirstTargetDamageSeconds = -1;
		bool bPriming = false, bAccepted = false, bControlChanged = false, bEffectSeen = false;
		TSet<int32> EverMoved, EverMovedSupports;
		TSet<int32> CoveredRoofBuildings, RespondedWithinDeadline;

		bool TraceRoof(const FRoofProbe& Probe, FHitResult& Hit) const
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(DublinBlastResponseRoof), false);
			if (Pawn.IsValid()) { Params.AddIgnoredActor(Pawn.Get()); }
			if (Projectile.IsValid()) { Params.AddIgnoredActor(Projectile.Get()); }
			return World->LineTraceSingleByChannel(Hit, Probe.Position + FVector(0, 0, 200),
				Probe.Position - FVector(0, 0, 200), ECC_Visibility, Params);
		}

		bool IsChanged(const FRoofProbe& Probe) const
		{
			FHitResult Hit;
			return !TraceRoof(Probe, Hit) || FMath::Abs(Hit.ImpactPoint.Z - Probe.Position.Z) > 25;
		}

		bool RetainAsset(const FDublinFractureRecord& Record)
		{
			UGeometryCollection* Asset = Record.Collection.LoadSynchronous();
			FString Error;
			if (!Asset || !DublinFractureBake::ValidateCollisionData(*Asset, Record, Error))
			{
				Test.AddError(TEXT("Fixture asset/cooked-collision prerequisite failed: ") + Record.SourceId + TEXT(": ") + Error);
				return false;
			}
			ResidentAssets.Emplace(Asset);
			return true;
		}

		bool Bootstrap()
		{
			if (!GEngine) { return Fail(TEXT("Engine unavailable")); }
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (!Candidate || Candidate->WorldType != EWorldType::PIE || Candidate->bIsTearingDown) { continue; }
				if (UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) != TEXT("/Game/Maps/Dublin"))
				{
					return Fail(TEXT("Wrong PIE map"));
				}
				ADublinCityWorld* Found = nullptr;
				for (TActorIterator<ADublinCityWorld> It(Candidate); It; ++It)
				{
					if (Found) { return Fail(TEXT("More than one city in PIE")); }
					Found = *It;
				}
				APlayerController* Player = Candidate->GetFirstPlayerController();
				ADublinFlightPawn* Plane = Player ? Cast<ADublinFlightPawn>(Player->GetPawn()) : nullptr;
				if (!Found || !Found->bCityReady || !Plane || !Plane->bSpawnCaptured || !Plane->Weapons) { return false; }
				World = Candidate;
				City = Found;
				Pawn = Plane;
				Weapon = Plane->Weapons;
				if (!City->bDestructionReady || !City->bAllBuildingsFractureReady || City->bAllowPartialBakePreview
					|| !City->GetSourceData() || !City->FractureLibrary
					|| City->AcceptedImpactCount != 0 || City->FracturedBuildingCount != 0 || City->QueuedImpactCount != 0)
				{
					return Fail(TEXT("Fixture requires a fresh, fully baked city; partial preview is forbidden"));
				}
				if (!Plane->IsGodMode()) { Plane->ToggleGodMode(); }
				Plane->Weapons->SuppressInput();
				const TArray<FDublinCityBuilding>& Buildings = City->GetSourceData()->Buildings;
				Target = Buildings.IndexOfByPredicate([this](const FDublinCityBuilding& B) { return B.Id == SourceId; });
				if (Target == INDEX_NONE) { return Fail(TEXT("Fixed source building is missing")); }
				for (const FDublinCityBuilding& Building : Buildings) { SourceBounds.Add(Bounds(Building)); }
				const FDublinFractureRecord* Record = City->FractureLibrary->Find(SourceId);
				if (!Record || !Record->bReady) { return Fail(TEXT("Fixed target has no ready fracture record")); }
				TargetRecord = *Record;
				const TArray<FRoofProbe> TargetRoofs = RoofProbes(Buildings[Target], Target);
				if (TargetRoofs.IsEmpty()) { return Fail(TEXT("Fixed target has no source roof triangle")); }
				FHitResult RoofHit;
				if (!TraceRoof(TargetRoofs[0], RoofHit) || RoofHit.GetActor() != City.Get()
					|| RoofHit.ImpactNormal.Z < 0.5 || FVector::Dist(RoofHit.ImpactPoint, TargetRoofs[0].Position) > 2)
				{
					return Fail(TEXT("Fixed target roof raycast prerequisite failed; no leaf-centre substitution"));
				}
				Roof = RoofHit.ImpactPoint;
				Evidence->SetStringField(TEXT("source_id"), SourceId);
				Evidence->SetStringField(TEXT("route"), TEXT("Raycast fixed source roof -> spawn real gravity projectile above Hit.ImpactPoint -> swept stop -> Resolve -> Weapon.DispatchImpact -> City.ApplyImpact"));
				PointField(*Evidence, TEXT("source_roof_probe_cm"), TargetRoofs[0].Position);
				PointField(*Evidence, TEXT("roof_trace_impact_point_cm"), Roof);
				Evidence->SetStringField(TEXT("asset_policy"), bCold
					? TEXT("No fixture preloading; measures runtime soft-asset path. Process cache may remain warm, not a fresh-process cold guarantee.")
					: TEXT("Resident fixture assets isolate physical response from cold streaming; runtime validation still mandatory. Not packaged certification."));
				Evidence->SetStringField(TEXT("area_metric"), TEXT("Sum of horizontal source roof-triangle areas whose centroid collision changed by >25cm; sampled estimate, not pixel/render proof."));
				Evidence->SetNumberField(TEXT("yield_game_label"), Yield);
				Evidence->SetNumberField(TEXT("source_height_cm"), SourceBounds[Target].GetSize().Z);
				Evidence->SetBoolField(TEXT("occupied_budget_case"), bOccupied);
				Evidence->SetBoolField(TEXT("latency_only_case"), bLatencyOnly);
				Evidence->SetNumberField(TEXT("response_limit_seconds"), ResponseLimitSeconds);
				const FDublinImpact Maximum = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
				int32 SelectedPieces = 0, SelectedBuildings = 0;
				for (int32 I = 0; I < Buildings.Num(); ++I)
				{
					const bool bInBlast = DublinDestruction::FootprintDistanceSquared(Buildings[I], Roof) <= FMath::Square(double(Maximum.RadiusCm));
					if (bInBlast)
					{
						const FDublinFractureRecord* R = City->FractureLibrary->Find(Buildings[I].Id);
						if (!R) { return Fail(TEXT("Selected building has no fracture record")); }
						if (!bCold && !RetainAsset(*R)) { return Finish(); }
						SelectedPieces += R->PieceCount;
						++SelectedBuildings;
					}
					if (!bInBlast && (Control.Building != INDEX_NONE
						|| FVector::DistXY(SourceBounds[I].GetCenter(), Roof) < Maximum.RadiusCm * 2)) { continue; }
					const TArray<FRoofProbe> Roofs = RoofProbes(Buildings[I], I);
					for (const FRoofProbe& Probe : Roofs)
					{
						FHitResult Hit;
						if (!TraceRoof(Probe, Hit) || Hit.GetActor() != City.Get()
							|| FMath::Abs(Hit.ImpactPoint.Z - Probe.Position.Z) > 2) { continue; }
						if (bInBlast)
						{
							Probes.Add(Probe);
							if (FVector::DistXY(Probe.Position, Roof) <= Maximum.RadiusCm) { CoveredRoofBuildings.Add(I); }
						}
						else { Control = Probe; break; }
					}
				}
				Evidence->SetNumberField(TEXT("max_yield_selected_buildings"), SelectedBuildings);
				Evidence->SetNumberField(TEXT("max_yield_selected_authored_pieces"), SelectedPieces);
				Evidence->SetNumberField(TEXT("max_yield_piece_budget_batches_lower_bound"),
					FMath::DivideAndRoundUp(SelectedPieces, DublinDestruction::MaxActivePieces));
				Evidence->SetNumberField(TEXT("sampled_roofs_inside_maximum_radius"), CoveredRoofBuildings.Num());
				const bool bHeightWithinRadius = SourceBounds[Target].GetSize().Z <= Maximum.RadiusCm;
				Evidence->SetBoolField(TEXT("height_only_within_maximum_radius"), bHeightWithinRadius);
				if (SourceId != TallSourceId && (!bHeightWithinRadius || SelectedPieces <= HistoricalPiecePressure))
				{
					return Fail(TEXT("Typical-roof budget fixture prerequisite changed: require height within radius and selected pieces above 1536"));
				}
				if (Probes.IsEmpty() || Control.Building == INDEX_NONE)
				{
					return Fail(TEXT("Fixed target/control roof collision prerequisites failed"));
				}
				if (bOccupied) { return PrimeBudget(); }
				return Launch();
			}
			return false;
		}

		bool PrimeBudget()
		{
			const TArray<FDublinCityBuilding>& Buildings = City->GetSourceData()->Buildings;
			const FDublinImpact Maximum = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
			TArray<int32> Candidates;
			for (int32 I = 0; I < Buildings.Num(); ++I)
			{
				if (!DublinDestruction::SphereTouchesBox(Roof, Maximum.RadiusCm + 5000, SourceBounds[I])
					&& I != Control.Building) { Candidates.Add(I); }
			}
			Candidates.Sort([&](int32 A, int32 B)
			{
				const auto* RA = City->FractureLibrary->Find(Buildings[A].Id);
				const auto* RB = City->FractureLibrary->Find(Buildings[B].Id);
				const int32 PA = RA ? RA->PieceCount : 0, PB = RB ? RB->PieceCount : 0;
				return PA == PB ? Buildings[A].Id < Buildings[B].Id : PA > PB;
			});
			TArray<FDublinImpact> Primers;
			int32 Pieces = 0;
			for (int32 I : Candidates)
			{
				const FDublinFractureRecord* Record = City->FractureLibrary->Find(Buildings[I].Id);
				if (!Record || Pieces + Record->PieceCount > DublinDestruction::MaxActivePieces) { continue; }
				const TArray<FRoofProbe> Roofs = RoofProbes(Buildings[I], I);
				if (Roofs.IsEmpty()) { continue; }
				FHitResult RoofHit;
				if (!TraceRoof(Roofs[0], RoofHit) || RoofHit.GetActor() != City.Get()
					|| RoofHit.ImpactNormal.Z < 0.5 || FVector::Dist(RoofHit.ImpactPoint, Roofs[0].Position) > 2) { continue; }
				FDublinImpact Primer;
				Primer.PositionCm = RoofHit.ImpactPoint;
				Primer.RadiusCm = 1;
				Primer.CraterDepthCm = 0;
				bool bOnlyThisBuilding = true;
				for (int32 J = 0; J < Buildings.Num(); ++J)
				{
					if (J != I && DublinDestruction::SphereTouchesBox(Primer.PositionCm, Primer.RadiusCm, SourceBounds[J]))
					{
						bOnlyThisBuilding = false;
						break;
					}
				}
				if (!bOnlyThisBuilding) { continue; }
				if (!RetainAsset(*Record)) { return Finish(); }
				Primers.Add(Primer);
				Pieces += Record->PieceCount;
				if (Pieces + TargetRecord.PieceCount > HistoricalPiecePressure) { break; }
			}
			if (Pieces + TargetRecord.PieceCount <= HistoricalPiecePressure)
			{
				return Fail(TEXT("Could not choose isolated source buildings to occupy native capacity"));
			}
			for (const FDublinImpact& Primer : Primers)
			{
				if (!City->ApplyImpact(Primer)) { return Fail(TEXT("Native preparation impact rejected: ") + City->LastDestructionError); }
			}
			bPriming = true;
			PrimingWorld = World->GetTimeSeconds();
			Evidence->SetNumberField(TEXT("preparation_collections"), Primers.Num());
			Evidence->SetNumberField(TEXT("preparation_pieces"), Pieces);
			return false;
		}

		bool Launch()
		{
			FHitResult RoofHit;
			if (!TraceRoof({Target, Roof, 0}, RoofHit) || RoofHit.GetActor() != City.Get()
				|| RoofHit.ImpactNormal.Z < 0.5 || FVector::Dist(RoofHit.ImpactPoint, Roof) > 2)
			{
				return Fail(TEXT("Fixed roof changed before projectile launch; refusing a different impact point"));
			}
			Roof = RoofHit.ImpactPoint;
			PointField(*Evidence, TEXT("launch_roof_trace_impact_point_cm"), Roof);
			Evidence->SetNumberField(TEXT("active_collections_at_launch"), City->ActiveFractureCollections);
			Evidence->SetNumberField(TEXT("active_pieces_at_launch"), City->ActiveFracturePieces);
			Evidence->SetNumberField(TEXT("free_piece_capacity_at_launch"),
				DublinDestruction::MaxActivePieces - City->ActiveFracturePieces);
			Evidence->SetNumberField(TEXT("queued_hits_at_launch"), City->QueuedBuildingHitCount);
			AcceptedBefore = Weapon->AcceptedBombImpacts;
			RejectedBefore = Weapon->RejectedImpacts;
			Impact = ConfiguredWeaponImpact(*Weapon.Get(), Yield);
			Evidence->SetNumberField(TEXT("live_weapon_radius_at_one_cm"), Weapon->BombRadiusAtOneCm);
			Evidence->SetNumberField(TEXT("live_weapon_radius_cap_cm"), Weapon->BombMaximumRadiusCm);
			Evidence->SetNumberField(TEXT("live_weapon_depth_strength_exponent"), Weapon->BombYieldExponent);
			Evidence->SetNumberField(TEXT("configured_launch_radius_cm"), Impact.RadiusCm);
			if (Yield == 1000 && !FMath::IsNearlyEqual(Impact.RadiusCm, 12000.0f, .1f))
			{
				return Fail(TEXT("Live map weapon tuning does not produce the approved 120m maximum radius; inspect serialized overrides"));
			}
			Impact.Seed = 20260918;
			const FTransform Spawn(FRotator::ZeroRotator, Roof + FVector(0, 0, 100));
			ADublinProjectile* Bomb = World->SpawnActorDeferred<ADublinProjectile>(ADublinProjectile::StaticClass(),
				Spawn, Pawn.Get(), Pawn.Get(), ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Bomb) { return Fail(TEXT("Native projectile spawn failed")); }
			Projectile = Bomb;
			Bomb->Initialize(Impact, FVector(0, 0, -1000), City.Get(), Weapon.Get());
			Bomb->FinishSpawning(Spawn);
			LaunchWorld = World->GetTimeSeconds();
			return false;
		}

		void Observe(double Now)
		{
			const double Elapsed = Now - ImpactWorld;
			TInlineComponentArray<UGeometryCollectionComponent*> Collections;
			City->GetComponents(Collections);
			for (UGeometryCollectionComponent* Component : Collections)
			{
				if (!Component || Component->GetRestCollection() != TargetRecord.Collection.Get()) { continue; }
				if (ActivationSeconds < 0)
				{
					ActivationSeconds = Elapsed;
					InitialLeaves = Component->GetInitialLocalRestTransforms();
					const TArray<int32> Selected = DublinDestruction::SelectImpactedFractureLeaves(
						TargetRecord.LeafTransforms, InitialLeaves, Component->GetComponentTransform(), Impact);
					int32 SelectedSupports = 0;
					for (int32 Leaf : Selected) { SelectedSupports += TargetRecord.Anchors.Contains(Leaf) ? 1 : 0; }
					Evidence->SetNumberField(TEXT("rest_selected_leaf_count"), Selected.Num());
					Evidence->SetNumberField(TEXT("rest_selected_support_count"), SelectedSupports);
					Evidence->SetNumberField(TEXT("authored_support_count"), TargetRecord.Anchors.Num());
					Evidence->SetBoolField(TEXT("first_observed_component_has_physics_state"), Component->IsPhysicsStateCreated());
					Evidence->SetBoolField(TEXT("first_observed_component_has_render_state"), Component->IsRenderStateCreated());
				}
				if (Component->GetPhysicsProxy() && Component->GetPhysicsProxy()->IsInitializedOnPhysicsThread()
					&& PhysicsReadySeconds < 0) { PhysicsReadySeconds = Elapsed; }
				const TArray<FTransform> Current = Component->GetCurrentTransforms();
				FVector FirstRest = FVector::ZeroVector, FirstCurrent = FVector::ZeroVector;
				bool bFirstLeaf = true;
				for (int32 Leaf : TargetRecord.LeafTransforms)
				{
					if (!Current.IsValidIndex(Leaf) || !InitialLeaves.IsValidIndex(Leaf)
						|| Current[Leaf].ContainsNaN()) { continue; }
					const FVector Delta = Component->GetComponentTransform().TransformVector(
						Current[Leaf].GetLocation() - InitialLeaves[Leaf].GetLocation());
					if (Delta.Size() > 25)
					{
						EverMoved.Add(Leaf);
						if (TargetRecord.Anchors.Contains(Leaf)) { EverMovedSupports.Add(Leaf); }
					}
					const FVector Rest = Component->GetComponentTransform().TransformPosition(InitialLeaves[Leaf].GetLocation());
					const FVector Moved = Component->GetComponentTransform().TransformPosition(Current[Leaf].GetLocation());
					if (bFirstLeaf) { FirstRest = Rest; FirstCurrent = Moved; bFirstLeaf = false; }
					else
					{
						MaxSeparationCm = FMath::Max(MaxSeparationCm,
							FMath::Abs(FVector::Dist(Moved, FirstCurrent) - FVector::Dist(Rest, FirstRest)));
					}
				}
			}
			Result->MovedLeaves = EverMoved.Num();
			Result->MovedSupports = EverMovedSupports.Num();
			for (TObjectIterator<UNiagaraComponent> It; It; ++It)
			{
				if (It->GetWorld() == World.Get() && It->IsActive()
					&& FVector::Dist(It->GetComponentLocation(), Impact.PositionCm) < 100) { bEffectSeen = true; }
			}
			if (LastSample >= 0 && Elapsed - LastSample < 0.1) { return; }
			LastSample = Elapsed;
			double Area = 0;
			TSet<int32> Changed;
			for (const FRoofProbe& Probe : Probes)
			{
				if (IsChanged(Probe))
				{
					Area += Probe.AreaCm2;
					Changed.Add(Probe.Building);
					if (Probe.Building == Target && FirstTargetDamageSeconds < 0) { FirstTargetDamageSeconds = Elapsed; }
					if (Elapsed <= ResponseLimitSeconds && CoveredRoofBuildings.Contains(Probe.Building))
					{
						RespondedWithinDeadline.Add(Probe.Building);
					}
				}
			}
			if (Area > 0 && Result->FirstDamageSeconds < 0) { Result->FirstDamageSeconds = Elapsed; }
			Result->ChangedRoofAreaCm2 = FMath::Max(Result->ChangedRoofAreaCm2, Area);
			Result->ChangedBuildings = FMath::Max(Result->ChangedBuildings, Changed.Num());
			bControlChanged |= IsChanged(Control);
			TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
			Sample->SetNumberField(TEXT("seconds_after_impact"), Elapsed);
			Sample->SetNumberField(TEXT("changed_roof_area_cm2"), Area);
			Sample->SetNumberField(TEXT("changed_buildings"), Changed.Num());
			Sample->SetNumberField(TEXT("moved_target_leaves"), EverMoved.Num());
			Sample->SetNumberField(TEXT("moved_target_supports"), EverMovedSupports.Num());
			Sample->SetNumberField(TEXT("queued_building_hits"), City->QueuedBuildingHitCount);
			Sample->SetNumberField(TEXT("active_pieces"), City->ActiveFracturePieces);
			Samples.Add(MakeShared<FJsonValueObject>(Sample));
		}

		bool Fail(const FString& Error)
		{
			Test.AddError(Error);
			Evidence->SetStringField(TEXT("failure"), Error);
			return Finish();
		}

		bool Finish()
		{
			Evidence->SetBoolField(TEXT("observation_completed"), Result->bCompleted);
			Evidence->SetBoolField(TEXT("projectile_impact_accepted"), bAccepted);
			Evidence->SetBoolField(TEXT("live_effect_component_observed"), bEffectSeen);
			Evidence->SetNumberField(TEXT("activation_seconds"), ActivationSeconds);
			Evidence->SetNumberField(TEXT("first_observed_physics_initialized_seconds"), PhysicsReadySeconds);
			Evidence->SetNumberField(TEXT("first_damage_seconds"), Result->FirstDamageSeconds);
			Evidence->SetNumberField(TEXT("first_target_roof_damage_seconds"), FirstTargetDamageSeconds);
			Evidence->SetNumberField(TEXT("peak_changed_roof_area_cm2"), Result->ChangedRoofAreaCm2);
			Evidence->SetNumberField(TEXT("peak_changed_buildings"), Result->ChangedBuildings);
			Evidence->SetNumberField(TEXT("covered_roofs_responded_within_deadline"), RespondedWithinDeadline.Num());
			Evidence->SetNumberField(TEXT("target_moved_leaves"), Result->MovedLeaves);
			Evidence->SetNumberField(TEXT("target_moved_supports"), Result->MovedSupports);
			Evidence->SetNumberField(TEXT("max_relative_leaf_displacement_cm"), MaxSeparationCm);
			Evidence->SetArrayField(TEXT("samples"), Samples);
			if (City.IsValid())
			{
				Evidence->SetNumberField(TEXT("final_queued_hits"), City->QueuedBuildingHitCount);
				Evidence->SetNumberField(TEXT("final_queued_core_hits"), City->QueuedCoreBuildingHitCount);
				Evidence->SetNumberField(TEXT("max_core_commit_world_seconds"), City->MaxCoreDamageLatencySeconds);
				Evidence->SetNumberField(TEXT("observation_wall_seconds"), ImpactWall > 0 ? FPlatformTime::Seconds() - ImpactWall : 0);
				Evidence->SetNumberField(TEXT("resident_collections"), City->AllocatedFractureCollections);
				Evidence->SetNumberField(TEXT("resident_leaf_slots"), City->AllocatedFracturePieces);
				Evidence->SetNumberField(TEXT("peak_frame_registrations"), City->PeakFrameFractureRegistrations);
				Evidence->SetBoolField(TEXT("activation_error_blocked_queue"), City->bImpactQueueBlocked);
				Evidence->SetStringField(TEXT("destruction_error"), City->LastDestructionError);
				Evidence->SetBoolField(TEXT("accepted_but_target_still_intact_with_pending_work"),
					bAccepted && FirstTargetDamageSeconds < 0 && ActivationSeconds < 0 && City->QueuedBuildingHitCount > 0);
			}
			FString Json;
			const FString Path = FPaths::Combine(Directory, FString::Printf(TEXT("yield-%.0f-occupied-%d.json"), Yield, bOccupied));
			if (!FJsonSerializer::Serialize(Evidence, TJsonWriterFactory<>::Create(&Json))
				|| !IFileManager::Get().MakeDirectory(*Directory, true)
				|| !FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Test.AddError(TEXT("Could not persist blast response evidence: ") + Path);
			}
			Test.AddInfo(TEXT("Blast response evidence: ") + Path);
			return true;
		}
	};

	class FWaitForPIEEnd final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForPIEEnd(FAutomationTestBase& InTest) : Test(InTest) {}
		virtual bool Update() override
		{
			if (Started == 0) { Started = FPlatformTime::Seconds(); }
			if (!GEditor || !GEditor->PlayWorld) { return true; }
			if (FPlatformTime::Seconds() - Started > 10) { Test.AddError(TEXT("Owned PIE teardown timed out")); return true; }
			return false;
		}
	private:
		FAutomationTestBase& Test;
		double Started = 0;
	};

	class FCompare final : public IAutomationLatentCommand
	{
	public:
		FCompare(FAutomationTestBase& InTest, TSharedRef<FResult> InLow, TSharedRef<FResult> InHigh)
			: Test(InTest), Low(InLow), High(InHigh) {}
		virtual bool Update() override
		{
			Test.TestTrue(TEXT("Both fresh-world rooftop observations completed"), Low->bCompleted && High->bCompleted);
			Test.TestTrue(TEXT("Maximum yield changes at least four times the low-yield roof area, and at least 100 square metres"),
				High->ChangedRoofAreaCm2 >= FMath::Max(1000000.0, Low->ChangedRoofAreaCm2 * 4));
			Test.AddInfo(FString::Printf(TEXT("Roof damage estimate low=%.1fm2 high=%.1fm2; target supports moved low=%d high=%d"),
				Low->ChangedRoofAreaCm2 / 10000, High->ChangedRoofAreaCm2 / 10000, Low->MovedSupports, High->MovedSupports));
			return true;
		}
	private:
		FAutomationTestBase& Test;
		TSharedRef<FResult> Low, High;
	};

	bool ReadyEditor(FAutomationTestBase& Test)
	{
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		return Test.TestTrue(TEXT("Open saved /Game/Maps/Dublin, end existing PIE, and run this destructive fixture alone"),
			EditorWorld && EditorWorld->GetPackage()->GetName() == TEXT("/Game/Maps/Dublin")
				&& !EditorWorld->GetPackage()->IsDirty() && !GEditor->PlayWorld);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastLowVsMaximumRoofPIETest,
	"DublinFlight.Destruction.BlastResponse.PIE.LowVsMaximumTallRoof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastLowVsMaximumRoofPIETest::RunTest(const FString& Parameters)
{
	using namespace DublinBlastResponse;
	if (!ReadyEditor(*this)) { return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("BlastResponse"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedRef<FResult> Low = MakeShared<FResult>(), High = MakeShared<FResult>();
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FShot(*this, 1, false, Low, Directory));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPIEEnd(*this));
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FShot(*this, 1000, false, High, Directory));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPIEEnd(*this));
	ADD_LATENT_AUTOMATION_COMMAND(FCompare(*this, Low, High));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastOccupiedBudgetRoofPIETest,
	"DublinFlight.Destruction.BlastResponse.PIE.MaximumTallRoofOccupiedBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastOccupiedBudgetRoofPIETest::RunTest(const FString& Parameters)
{
	using namespace DublinBlastResponse;
	if (!ReadyEditor(*this)) { return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("BlastResponse"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FShot(*this, 1000, true, MakeShared<FResult>(), Directory));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPIEEnd(*this));
	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDublinBlastTypicalRoofBudgetPIETest,
	"DublinFlight.Destruction.BlastResponse.PIE.TypicalRoofBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FDublinBlastTypicalRoofBudgetPIETest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const DublinBlastResponse::FBudgetCase& Case : DublinBlastResponse::BudgetCases)
	{
		OutBeautifiedNames.Add(Case.Name);
		OutTestCommands.Add(Case.Name);
	}
}

bool FDublinBlastTypicalRoofBudgetPIETest::RunTest(const FString& Parameters)
{
	using namespace DublinBlastResponse;
	const FBudgetCase* Selected = nullptr;
	for (const FBudgetCase& Case : BudgetCases)
	{
		if (Parameters == Case.Name) { Selected = &Case; break; }
	}
	if (!Selected) { AddError(TEXT("Select one exact typical-roof budget case")); return false; }
	if (!ReadyEditor(*this)) { return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("BlastResponse"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FShot(*this, 1000, Selected->bOccupied, MakeShared<FResult>(), Directory, Selected->SourceId));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPIEEnd(*this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastOccupiedBudgetRoofLatencyPIETest,
	"DublinFlight.Destruction.BlastResponse.PIE.OccupiedBudgetRoofLatency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastOccupiedBudgetRoofLatencyPIETest::RunTest(const FString& Parameters)
{
	using namespace DublinBlastResponse;
	if (!ReadyEditor(*this)) { return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("BlastResponse"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FShot(*this, 1000, true, MakeShared<FResult>(), Directory,
		TEXT("osm/way/233804861"), true));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPIEEnd(*this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBlastColdCorePIETest,
	"DublinFlight.Destruction.BlastResponse.PIE.MaximumColdCoreCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBlastColdCorePIETest::RunTest(const FString& Parameters)
{
	using namespace DublinBlastResponse;
	if (!ReadyEditor(*this)) { return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("BlastResponse"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FShot(*this, 1000, false, MakeShared<FResult>(), Directory,
		TEXT("osm/way/233804861"), false, true));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPIEEnd(*this));
	return true;
}

#endif
#endif
