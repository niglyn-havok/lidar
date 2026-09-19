#include "City/DublinCityDestruction.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "City/DublinCityWorld.h"
#include "Weapons/DublinWeaponModel.h"
#include "CoreGlobals.h"
#include "Engine/World.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FPacingFixture
	{
		UWorld* World = nullptr;
		ADublinCityWorld* Ordinary = nullptr;
		ADublinCityWorld* Maximum = nullptr;
		UGeometryCollectionComponent* FirstComponent = nullptr;
		TArray<TStrongObjectPtr<UGeometryCollection>> Assets;
		uint64 LastFrame = MAX_uint64;
		double StartedWall = FPlatformTime::Seconds();
		int32 Steps = 0;
		~FPacingFixture() { if (World) { World->DestroyWorld(false); } }
	};

	class FPacingFrames final : public IAutomationLatentCommand
	{
	public:
		explicit FPacingFrames(TFunction<bool()> InStep) : Step(MoveTemp(InStep)) {}
		virtual bool Update() override { return Step(); }
	private:
		TFunction<bool()> Step;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityActivationTierTest,
	"DublinFlight.Destruction.ActivationPacing.NativeTierPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityActivationTierTest::RunTest(const FString& Parameters)
{
	for (float Yield : {0.1f, 1.0f, 10.0f, 100.0f})
	{
		const FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, Yield, {});
		TestEqual(TEXT("Ordinary game yield has one cold activation slot"), DublinDestruction::RegistrationLimitForImpact(Impact), 1);
	}
	FDublinImpact Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	TestEqual(TEXT("True maximum retains its existing eight-slot ceiling"), DublinDestruction::RegistrationLimitForImpact(Impact), 8);
	Impact.Seed = 123;
	TestEqual(TEXT("Presentation identity does not determine the tier"), DublinDestruction::RegistrationLimitForImpact(Impact), 8);
	Impact.YieldTonsTNT = 1;
	TestEqual(TEXT("Maximum radius alone cannot promote ordinary yield"), DublinDestruction::RegistrationLimitForImpact(Impact), 1);
	Impact.YieldTonsTNT = 1000;
	Impact.RadiusCm = DublinImpactFX::MaximumApprovedRadiusCm + 1;
	TestEqual(TEXT("Out-of-policy radius cannot promote cold work"), DublinDestruction::RegistrationLimitForImpact(Impact), 1);
	Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	Impact.bWater = true;
	TestEqual(TEXT("Water is not maximum structural activation"), DublinDestruction::RegistrationLimitForImpact(Impact), 1);
	Impact.bWater = false; Impact.Kind = EDublinImpactKind::Cannon;
	TestEqual(TEXT("A cannon cannot claim maximum bomb pacing"), DublinDestruction::RegistrationLimitForImpact(Impact), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityActivationPacingNativeTest,
	"DublinFlight.Destruction.ActivationPacing.NativeEngineFramesRoofAndRepeatFairness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityActivationPacingNativeTest::RunTest(const FString& Parameters)
{
	const auto State = MakeShared<FPacingFixture>();
	State->World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Native pacing fixture world"), State->World)) { return false; }
	auto MakeCity = [&]() -> ADublinCityWorld*
	{
		ADublinCityWorld* City = State->World->SpawnActorDeferred<ADublinCityWorld>(ADublinCityWorld::StaticClass(),
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!City) { return nullptr; }
		City->bAutoBuild = false;
		City->FinishSpawning(FTransform::Identity);
		City->SourceData = MakeUnique<FDublinCityData>();
		City->FractureLibrary = NewObject<UDublinCityFractureLibrary>(City);
		UGeometryCollection* Asset = NewObject<UGeometryCollection>(City);
		State->Assets.Emplace(Asset);
		const auto Geometry = Asset->GetGeometryCollection();
		FDublinFractureRecord Record;
		for (double Z : {50.0, 150.0})
		{
			Record.LeafTransforms.Add(Geometry->AppendGeometry(
				*GeometryCollection::MakeCubeElement(FTransform(FVector(0, 0, Z)), FVector(100))));
		}
		Record.PieceCount = 2; Record.Anchors = {Record.LeafTransforms[0]};
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
		if (!DublinFractureBake::BuildConnectionGraph(*Asset, Error)) { AddError(Error); return nullptr; }
		Asset->UpdateGeometryDependentProperties(); Asset->CreateSimulationData(); Asset->RebuildRenderData();
		Record.Collection = Asset; Record.bReady = true;
		const FVector Pivots[] = {FVector(0), FVector(200, 0, 0), FVector(-200, 0, 0),
			FVector(0, 200, 0), FVector(0, -200, 0), FVector(200, 200, 0), FVector(-200, -200, 0),
			FVector(400, 0, 0), FVector(-400, 0, 0), FVector(0, 400, 0)};
		for (int32 I = 0; I < UE_ARRAY_COUNT(Pivots); ++I)
		{
			FDublinCityBuilding& Building = City->SourceData->Buildings.AddDefaulted_GetRef();
			Building.Id = FString::Printf(TEXT("native-pacing-%d"), I);
			Building.PivotCm = Pivots[I];
			Building.Mesh.VerticesCm = {FVector(-50, -50, 200), FVector(-50, 50, 200),
				FVector(50, -50, 200), FVector(50, 50, 200)};
			Building.Mesh.Triangles = {0, 1, 2, 2, 1, 3};
			Building.Mesh.UV = {FVector2D(0, 0), FVector2D(0, 1), FVector2D(1, 0), FVector2D(1, 1)};
			Building.MaterialIds = {0, 0};
			City->BuildingBounds.Add(FBox(Pivots[I] + FVector(-50, -50, 0), Pivots[I] + FVector(50, 50, 200)));
			const FIntPoint Min = DublinCity::SpatialCell(City->BuildingBounds.Last().Min);
			const FIntPoint Max = DublinCity::SpatialCell(City->BuildingBounds.Last().Max);
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
				for (int32 X = Min.X; X <= Max.X; ++X) { City->BuildingSpatialIndex.FindOrAdd(FIntPoint(X, Y)).Add(I); }
			Record.SourceId = Building.Id;
			Record.SourceDigest = DublinDestruction::BuildingDigest(Building);
			City->FractureLibrary->Records.Add(Record);
		}
		if (!DublinCity::MakeBuildingChunks(City->SourceData->Buildings, City->BuildingChunks, Error))
		{
			AddError(Error); return nullptr;
		}
		City->BuildingToChunk.Init(INDEX_NONE, City->SourceData->Buildings.Num());
		for (int32 I = 0; I < City->BuildingChunks.Num(); ++I)
		{
			const auto& Chunk = City->BuildingChunks[I];
			UProceduralMeshComponent* Component = City->CreateChunkComponent(TEXT("DublinBuildings_Pacing"),
				Chunk.OriginCm, Chunk.Sections, {}, true);
			if (!Component) { AddError(TEXT("Native intact collision fixture failed")); return nullptr; }
			City->BuildingComponents.Add(Component);
			for (int32 Building : Chunk.BuildingIndices) { City->BuildingToChunk[Building] = I; }
		}
		City->bDestructionInitialized = City->bDestructionReady = City->bCatalogBudgetValid = true;
		return City;
	};
	State->Ordinary = MakeCity(); State->Maximum = MakeCity();
	if (!TestNotNull(TEXT("Ordinary native city"), State->Ordinary) || !TestNotNull(TEXT("Maximum native city"), State->Maximum)) { return false; }
	FDublinImpact Ordinary = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1, {});
	Ordinary.PositionCm = FVector(0, 0, 200); Ordinary.CraterDepthCm = 0;
	FDublinImpact Maximum = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 1000, {});
	Maximum.PositionCm = Ordinary.PositionCm; Maximum.CraterDepthCm = 0;
	for (auto Pair : {TPair<ADublinCityWorld*, FDublinImpact>(State->Ordinary, Ordinary),
		TPair<ADublinCityWorld*, FDublinImpact>(State->Maximum, Maximum)})
	{
		if (!TestTrue(TEXT("Actual native fixture impact accepted"), Pair.Key->ApplyImpact(Pair.Value))) { return false; }
		TestEqual(TEXT("All ten covered buildings retained"), Pair.Key->PendingImpacts[0].Buildings.Num(), 10);
		TestEqual(TEXT("Actually struck roof has first priority"), Pair.Key->PendingImpacts[0].Buildings[0], 0);
		TestEqual(TEXT("Cold work requests next engine-frame cadence"), Pair.Key->GetActorTickInterval(), 0.0f);
		Pair.Key->ProcessImpactQueue();
	}
	TestEqual(TEXT("Ordinary cold batch registers exactly one"), State->Ordinary->LastFrameFractureRegistrations, 1);
	TestEqual(TEXT("Maximum cold batch retains eight"), State->Maximum->LastFrameFractureRegistrations, 8);
	TestTrue(TEXT("Struck roof commits in its first eligible engine frame"), State->Ordinary->RemovedIntactBuildings.Contains(0));
	TestTrue(TEXT("First-frame roof damage is not merely registration"), State->Ordinary->PendingImpacts[0].FirstCommitWorldTime >= 0);
	TestTrue(TEXT("Cold backlog remains accepted"), State->Ordinary->PendingImpacts[0].Buildings[1] != INDEX_NONE);
	State->FirstComponent = State->Ordinary->FractureComponents.FindRef(0);
	State->Ordinary->FractureActivity.FindChecked(0).bSleeping = true;
	if (!TestTrue(TEXT("Active repeat admitted behind cold backlog"), State->Ordinary->ApplyImpact(Ordinary))) { return false; }
	State->Ordinary->ProcessImpactQueue(); State->Maximum->ProcessImpactQueue();
	TestEqual(TEXT("Repeated processing in one engine frame cannot spend another slot"), State->Ordinary->FractureComponents.Num(), 1);
	TestEqual(TEXT("Maximum cannot register more than eight in one engine frame"), State->Maximum->FractureComponents.Num(), 8);
	State->LastFrame = GFrameCounter;
	ADD_LATENT_AUTOMATION_COMMAND(FPacingFrames([this, State]()
	{
		if (GFrameCounter == State->LastFrame) { return false; }
		State->LastFrame = GFrameCounter;
		if (++State->Steps > 24 || FPlatformTime::Seconds() - State->StartedWall > 15)
		{
			AddError(TEXT("Native pacing queues failed to drain without dropping hits")); return true;
		}
		for (ADublinCityWorld* City : {State->Ordinary, State->Maximum})
		{
			const int32 Before = City->FractureComponents.Num();
			City->ProcessImpactQueue();
			const int32 New = City->FractureComponents.Num() - Before;
			TestEqual(TEXT("Telemetry is actual new collection count"), City->LastFrameFractureRegistrations, New);
			TestTrue(TEXT("Ordinary per-engine-frame quota is never exceeded"), City->LastFrameOrdinaryFractureRegistrations <= 1);
			TestTrue(TEXT("Global native ceiling remains eight"), New <= 8);
			if (City == State->Ordinary) { TestTrue(TEXT("Ordinary batch stays at one"), New <= 1); }
			City->UpdateDestructionTickEnabled();
			const int32 After = City->FractureComponents.Num();
			City->ProcessImpactQueue();
			TestEqual(TEXT("Same-engine-frame reentry cannot allocate"), City->FractureComponents.Num(), After);
			if (City->bImpactQueueBlocked) { AddError(City->LastDestructionError); return true; }
		}
		if (State->Steps == 1)
		{
			TestFalse(TEXT("Active repeat wakes despite earlier cold backlog"), State->Ordinary->FractureActivity.FindChecked(0).bSleeping);
			TestTrue(TEXT("Active repeat never reallocates its collection"), State->Ordinary->FractureComponents.FindRef(0) == State->FirstComponent);
			const FDublinQueuedWorldImpact* Repeat = State->Ordinary->PendingImpacts.FindByPredicate(
				[](const FDublinQueuedWorldImpact& Event) { return Event.EventId == 2; });
			TestTrue(TEXT("Active repeat slot commits ahead of the cold backlog"), Repeat && Repeat->Buildings[0] == INDEX_NONE);
			TestTrue(TEXT("Ordinary cold backlog remains for subsequent frames"), !State->Ordinary->PendingImpacts.IsEmpty());
		}
		if (!State->Ordinary->PendingImpacts.IsEmpty() || !State->Maximum->PendingImpacts.IsEmpty()) { return false; }
		TestEqual(TEXT("Every ordinary target eventually activated"), State->Ordinary->FractureComponents.Num(), 10);
		TestEqual(TEXT("Every maximum target eventually activated"), State->Maximum->FractureComponents.Num(), 10);
		TestEqual(TEXT("Both ordinary events remain accounted"), State->Ordinary->AcceptedImpactCount, 2);
		TestEqual(TEXT("Maximum event remains accounted"), State->Maximum->AcceptedImpactCount, 1);
		TestEqual(TEXT("Ordinary/water cadence restored after cold drain"),
			State->Ordinary->GetActorTickInterval(), DublinDestruction::OrdinaryTickIntervalSeconds);
		State->Ordinary->WaterWaves.bActive = true;
		State->Ordinary->UpdateDestructionTickEnabled();
		TestEqual(TEXT("Water alone cannot request per-frame cold cadence"),
			State->Ordinary->GetActorTickInterval(), DublinDestruction::OrdinaryTickIntervalSeconds);
		State->Ordinary->WaterWaves.bActive = false;
		return true;
	}));
	return !HasAnyErrors();
}
#endif
