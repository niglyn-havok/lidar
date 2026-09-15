#include "City/DublinCityDestruction.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "City/DublinCityWorld.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include <limits>
#if WITH_EDITOR
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "GeometryCollectionProxyData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#endif

namespace
{
	FDublinCityMesh GroundFixture()
	{
		FDublinCityMesh Mesh;
		for (int32 Y = 0; Y < 5; ++Y)
			for (int32 X = 0; X < 5; ++X)
			{
				Mesh.VerticesCm.Emplace(X * 200 - 400, Y * 200 - 400, 25);
				Mesh.UV.Emplace(X / 4.0, Y / 4.0);
			}
		for (int32 Y = 0; Y < 4; ++Y)
			for (int32 X = 0; X < 4; ++X)
			{
				const int32 A = Y * 5 + X, B = A + 1, C = A + 5, D = C + 1;
				Mesh.Triangles.Append({A, C, B, B, C, D});
			}
		Mesh.Triangles.RemoveAt(0, 9);
		return Mesh;
	}

	FDublinCityMesh WaterFixture()
	{
		FDublinCityMesh Mesh;
		Mesh.VerticesCm = {FVector(-1600, -1600, 110), FVector(-1600, 1600, 110),
			FVector(1600, -1600, 110), FVector(1600, 1600, 110)};
		Mesh.Triangles = {0, 1, 2, 2, 1, 3};
		Mesh.UV = {FVector2D(0, 0), FVector2D(0, 1), FVector2D(1, 0), FVector2D(1, 1)};
		return Mesh;
	}

	FDublinCityBuilding BuildingFixture(int32 Id)
	{
		FDublinCityBuilding Building;
		Building.Id = FString::FromInt(Id);
		Building.PivotCm = FVector(100 + Id * 1000, 100, 0);
		Building.Mesh.VerticesCm = {FVector(0, 0, 0), FVector(0, 100, 0), FVector(100, 0, 0)};
		Building.Mesh.Triangles = {0, 1, 2};
		Building.Mesh.UV = {FVector2D(0, 0), FVector2D(0, 1), FVector2D(1, 0)};
		Building.MaterialIds = {1};
		return Building;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionImpactTest, "DublinFlight.Destruction.Impact.BoundsLocalityAndStrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDestructionImpactTest::RunTest(const FString& Parameters)
{
	FDublinImpact Impact;
	FString Error;
	TestTrue(TEXT("Valid cannon impact"), DublinDestruction::ValidateImpact(Impact, Error));
	TestEqual(TEXT("Local strain has no graph propagation"), DublinDestruction::StrainPropagationDepth, 0);
	TestEqual(TEXT("No break cascade"), DublinDestruction::StrainPropagationFactor, 0.0f);
	TestEqual(TEXT("Default cannon strain exceeds authored local threshold"), DublinDestruction::ImpactStrain(Impact), 150.0f);
	const FBox Nearby(FVector(-100), FVector(100));
	const FBox Far(FVector(1000), FVector(1200));
	TestTrue(TEXT("Local volume selected"), DublinDestruction::SphereTouchesBox(FVector::ZeroVector, 200, Nearby));
	TestFalse(TEXT("Unhit neighboring volume excluded"), DublinDestruction::SphereTouchesBox(FVector::ZeroVector, 200, Far));
	Impact.YieldTonsTNT = 1000;
	TestTrue(TEXT("Maximum game-only yield supported"), DublinDestruction::ValidateImpact(Impact, Error));
	Impact.YieldTonsTNT = 1001;
	TestFalse(TEXT("Unbounded yield rejected"), DublinDestruction::ValidateImpact(Impact, Error));
	Impact.YieldTonsTNT = 0;
	Impact.RadiusCm = DublinDestruction::MaxRadiusCm + 1;
	TestFalse(TEXT("Unbounded radius rejected"), DublinDestruction::ValidateImpact(Impact, Error));
	Impact.RadiusCm = 200;
	Impact.PositionCm.X = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("Nonfinite impact rejected"), DublinDestruction::ValidateImpact(Impact, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionCraterTest, "DublinFlight.Destruction.Ground.PersistentCraterAndBorderContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDestructionCraterTest::RunTest(const FString& Parameters)
{
	const FDublinCityMesh Source = GroundFixture();
	TArray<FDublinCityChunk> Chunks;
	FString Error;
	if (!TestTrue(TEXT("Sparse shoreline fixture"), DublinCity::MakeTerrainChunks(Source, Chunks, Error, 5, 200))) { return false; }
	FDublinGroundMutation Ground;
	Ground.Initialize(Source);
	FDublinImpact Impact;
	Impact.PositionCm = FVector(0, 0, 25);
	Impact.RadiusCm = 300;
	Impact.CraterDepthCm = 100;
	TSet<int32> Changed;
	TestTrue(TEXT("First crater changes geometry"), Ground.Apply(Source, Impact, Changed));
	TestEqual(TEXT("Crater center lowered"), Ground.Heights[12], -75.0);
	TestEqual(TEXT("Outside radius unchanged"), Ground.Heights[0], 25.0);
	TestEqual(TEXT("Immutable source remains unchanged"), Source.VerticesCm[12].Z, 25.0);
	TestEqual(TEXT("Profile outside radius is exactly zero"), DublinDestruction::CraterDelta(301, 300, 100), 0.0);
	TestTrue(TEXT("Rim is inside the requested radius"), DublinDestruction::CraterDelta(270, 300, 100) > 0);
	TestTrue(TEXT("Second impact accumulates"), Ground.Apply(Source, Impact, Changed));
	TestEqual(TEXT("Accumulated crater center"), Ground.Heights[12], -175.0);
	int32 CenterCopies = 0;
	int32 TotalIndices = 0;
	for (FDublinCityChunk& Chunk : Chunks)
	{
		const TArray<int32> OriginalTopology = Chunk.Sections[0].Triangles;
		DublinDestruction::SyncGroundChunk(Chunk, Ground, Changed);
		TestTrue(TEXT("Exact supplied topology retained"), Chunk.Sections[0].Triangles == OriginalTopology);
		TotalIndices += OriginalTopology.Num();
		for (int32 I = 0; I < Chunk.Sections[0].SourceVertexIndices.Num(); ++I)
		{
			const int32 SourceIndex = Chunk.Sections[0].SourceVertexIndices[I];
			TestEqual(TEXT("Every changed duplicate addresses the same source-height slot"),
				Chunk.Sections[0].Vertices[I].Z + Chunk.OriginCm.Z, Ground.Heights[SourceIndex]);
			if (SourceIndex == 12) { ++CenterCopies; }
		}
	}
	TestTrue(TEXT("Shared border vertex tested in multiple chunks"), CenterCopies > 1);
	TestEqual(TEXT("No water-mask triangles regenerated"), TotalIndices, Source.Triangles.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionWaveTest, "DublinFlight.Destruction.Water.PropagationDampingAndRiverMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDestructionWaveTest::RunTest(const FString& Parameters)
{
	const FDublinCityMesh Source = WaterFixture();
	FDublinWaterWaves Waves;
	FString Error;
	if (!TestTrue(TEXT("Clipped 4m wave mesh initialized"), Waves.Initialize(Source, Error))) { return false; }
	TestTrue(TEXT("Wave geometry is finer than original two triangles"), Waves.RenderMesh.Triangles.Num() > Source.Triangles.Num());
	float Z = 0;
	TestTrue(TEXT("Actual source triangle hit"), DublinDestruction::SourceWaterZ(Source, FVector::ZeroVector, Z));
	TestFalse(TEXT("Outside river does not become water"), DublinDestruction::SourceWaterZ(Source, FVector(2000, 0, 0), Z));
	FDublinImpact Impact;
	Impact.bWater = true;
	Impact.PositionCm = FVector(0, 0, 110);
	Impact.RadiusCm = 200;
	Impact.Strength = 2;
	TestTrue(TEXT("Impact adds velocity pulse"), Waves.AddImpact(Source, Impact));
	const FVector Probe(800, 0, 110);
	TestEqual(TEXT("Distant surface initially flat"), Waves.SampleDisplacement(Probe), 0.0f);
	float Peak = 0;
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Waves.Step(1.0f / 30);
		Peak = FMath::Max(Peak, Waves.MaxHeightCm);
	}
	TestTrue(TEXT("Disturbance propagates beyond initial pulse"), FMath::Abs(Waves.SampleDisplacement(Probe)) > 0.001f);
	for (int32 Frame = 0; Frame < 1200; ++Frame) { Waves.Step(1.0f / 30); }
	TestTrue(TEXT("Damping reduces wave amplitude"), Waves.MaxHeightCm < Peak * .1f);
	for (int32 I = 0; I < Waves.Height.Num(); ++I)
	{
		TestTrue(TEXT("Stable finite height"), FMath::IsFinite(Waves.Height[I]) && FMath::Abs(Waves.Height[I]) <= 300);
		if (!Waves.Wet[I]) { TestEqual(TEXT("Dry bank slots are never displaced"), Waves.Height[I], 0.0f); }
	}
	Waves.UpdateRenderVertices();
	for (const FVector& Vertex : Waves.RenderMesh.Vertices)
	{
		TestTrue(TEXT("Resampled rendering remains within true source triangles"),
			DublinDestruction::SourceWaterZ(Source, Vertex, Z));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionFilterTest, "DublinFlight.Destruction.Buildings.SelectiveMeshRemovalAndIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDestructionFilterTest::RunTest(const FString& Parameters)
{
	TArray<FDublinCityBuilding> Buildings{BuildingFixture(0), BuildingFixture(1), BuildingFixture(2)};
	TArray<FDublinCityChunk> Chunks;
	FString Error;
	if (!TestTrue(TEXT("Three nearby buildings grouped"), DublinCity::MakeBuildingChunks(Buildings, Chunks, Error))) { return false; }
	const FDublinCitySection& Source = Chunks[0].Sections[1];
	TSet<int32> Removed;
	Removed.Add(1);
	const FDublinCitySection Out = DublinDestruction::FilterIntactSection(Source, Removed);
	TestEqual(TEXT("Only selected building triangles removed"), Out.Triangles.Num(), 6);
	TestEqual(TEXT("First unhit source id retained"), Out.SourceBuildingPerTriangle[0], 0);
	TestEqual(TEXT("Second unhit source id retained"), Out.SourceBuildingPerTriangle[1], 2);
	TestEqual(TEXT("Original renderer buffers are immutable rebuild input"), Source.Triangles.Num(), 9);
	const FString Hash = DublinDestruction::BuildingDigest(Buildings[0]);
	TestEqual(TEXT("Bake digest idempotent"), DublinDestruction::BuildingDigest(Buildings[0]), Hash);
	Buildings[0].PivotCm.X += 100;
	TestTrue(TEXT("Moved source invalidates baked aerial mapping"), DublinDestruction::BuildingDigest(Buildings[0]) != Hash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionColorTest, "DublinFlight.Destruction.Fracture.RgbaAndCosmeticIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDestructionColorTest::RunTest(const FString& Parameters)
{
	FDublinCityBuilding Building = BuildingFixture(0);
	const FString GeometryDigest = DublinDestruction::BuildingGeometryDigest(Building);
	const FString UncoloredDigest = DublinDestruction::BuildingDigest(Building);
	TestTrue(TEXT("Versioned fracture digest rejects the legacy geometry-only cache key"),
		UncoloredDigest != GeometryDigest);
	TestTrue(TEXT("Omitted colors remain white"),
		DublinDestruction::SourceVertexLinearColor(Building.Mesh, 0).ToFColor(true) == FColor::White);
	Building.Mesh.ColorsRGBA.Init(FColor(64, 128, 192, 1), Building.Mesh.VerticesCm.Num());
	for (uint8 Style = 1; Style <= 8; ++Style)
	{
		Building.Mesh.ColorsRGBA[0].A = Style;
		const FLinearColor Linear = DublinDestruction::SourceVertexLinearColor(Building.Mesh, 0);
		TestTrue(TEXT("sRGB RGB and style alpha round-trip through collection color packing"),
			Linear.ToFColor(true) == Building.Mesh.ColorsRGBA[0]);
		TestTrue(TEXT("Alpha remains normalized byte data, not gamma converted"),
			FMath::IsNearlyEqual(Linear.A * 255, static_cast<float>(Style), 1.e-5f));
	}
	TestEqual(TEXT("Cosmetics do not change geometry identity"),
		DublinDestruction::BuildingGeometryDigest(Building), GeometryDigest);
	const FString ColoredDigest = DublinDestruction::BuildingDigest(Building);
	TestTrue(TEXT("New colors invalidate an uncolored bake"), ColoredDigest != UncoloredDigest);
	Building.Mesh.ColorsRGBA[0].R = 65;
	TestTrue(TEXT("Tint changes invalidate baked attributes"),
		DublinDestruction::BuildingDigest(Building) != ColoredDigest);
	TestEqual(TEXT("Tint changes still do not invalidate footprints/UVs"),
		DublinDestruction::BuildingGeometryDigest(Building), GeometryDigest);
	const FString TintDigest = DublinDestruction::BuildingDigest(Building);
	Building.Mesh.ColorsRGBA[0].A = 2;
	TestTrue(TEXT("Style-only changes invalidate baked attributes"),
		DublinDestruction::BuildingDigest(Building) != TintDigest);
	TArray<FDublinCityChunk> Chunks;
	FString Error;
	if (TestTrue(TEXT("Colored intact geometry builds"),
		DublinCity::MakeBuildingChunks({Building}, Chunks, Error)))
	{
		TestTrue(TEXT("Intact PMC and fracture input preserve the same RGBA bytes"),
			Chunks[0].Sections[1].Colors[0] == Building.Mesh.ColorsRGBA[0]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDestructionReadinessTest, "DublinFlight.Destruction.Readiness.RejectsUnavailableAssetsWithoutMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDestructionReadinessTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Readiness fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	ADublinCityWorld* City = World->SpawnActorDeferred<ADublinCityWorld>(ADublinCityWorld::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Readiness fixture actor"), City)) { return false; }
	City->bAutoBuild = false;
	City->FinishSpawning(FTransform::Identity);
	AddExpectedError(TEXT("Destruction not ready"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Missing fracture readiness rejects hit"), City->ApplyImpact(FDublinImpact()));
	TestEqual(TEXT("Rejected hit is not accepted or queued"), City->AcceptedImpactCount, 0);
	TestEqual(TEXT("Rejected hit removes no building"), City->FracturedBuildingCount, 0);
	TestFalse(TEXT("Readable failure provided"), City->LastDestructionError.IsEmpty());
	float Z;
	TestFalse(TEXT("Uninitialized city does not fake a water surface"), City->GetWaterSurfaceZ(FVector::ZeroVector, Z));
	TestTrue(TEXT("Rejected hit does not start a simulation tick"), !City->IsActorTickEnabled());
#if WITH_EDITOR
	City->WallMaterial = UMaterialInstanceDynamic::Create(City->RoofMaterial.Get(), City);
	City->FoundationMaterial = UMaterialInstanceDynamic::Create(City->RoofMaterial.Get(), City);
	UGeometryCollection* Collection = NewObject<UGeometryCollection>();
	Collection->Materials = {City->RoofMaterial, City->WallMaterial, City->FoundationMaterial, City->WallMaterial};
	TestFalse(TEXT("Cached facade material on new interior faces is rejected"),
		DublinFractureBake::HasCurrentMaterialBindings(*Collection, *City));
	Collection->Materials[3] = City->FoundationMaterial;
	TestTrue(TEXT("Roof, exterior wall, foundation and neutral interior slots match"),
		DublinFractureBake::HasCurrentMaterialBindings(*Collection, *City));
#endif
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFracturePhysicsInitializationTest,
	"DublinFlight.Destruction.Fracture.RegisteredCollisionInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFracturePhysicsInitializationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Fracture fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Fracture fixture owner"), Owner)) { return false; }
	UGeometryCollection* Asset = NewObject<UGeometryCollection>(Owner);
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = Asset->GetGeometryCollection();
	FDublinFractureRecord Record;
	Record.LeafTransforms.Add(Geometry->AppendGeometry(
		*GeometryCollection::MakeCubeElement(FTransform(FVector(0, 0, 50)), FVector(100))));
	Record.LeafTransforms.Add(Geometry->AppendGeometry(
		*GeometryCollection::MakeCubeElement(FTransform(FVector(0, 0, 150)), FVector(100))));
	Record.PieceCount = Record.LeafTransforms.Num();
	Record.Anchors = {Record.LeafTransforms[0]};
	FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(Geometry.Get());
	Asset->InvalidateCollection();
	Record.RootTransform = Asset->GetRootIndex();
	Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Geometry);
	Anchoring.AddAnchoredAttribute();
	Anchoring.SetAnchored(Record.Anchors[0], true);
	Anchoring.SetInitialDynamicState(Record.Anchors[0], Chaos::EObjectStateType::Kinematic);
	Asset->Materials = {UMaterial::GetDefaultMaterial(MD_Surface), UMaterial::GetDefaultMaterial(MD_Surface)};
	FString Error;
	if (!TestTrue(TEXT("Runtime connections are authored from actual piece contacts"),
		DublinFractureBake::BuildConnectionGraph(*Asset, Error)))
	{
		AddError(Error);
		return false;
	}
	Asset->UpdateGeometryDependentProperties();
	Asset->CreateSimulationData();
	if (!TestTrue(TEXT("Every cooked leaf and the root have collision"),
		DublinFractureBake::ValidateCollisionData(*Asset, Record, Error)))
	{
		AddError(Error);
		return false;
	}
	FDublinFractureRecord AllAnchored = Record;
	AllAnchored.Anchors = AllAnchored.LeafTransforms;
	TestFalse(TEXT("A collection with every authored piece permanently anchored is rejected"),
		DublinFractureBake::ValidateCollisionData(*Asset, AllAnchored, Error));
	auto& Implicits = Geometry->ModifyAttribute<Chaos::FImplicitObjectPtr>(
		FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
	const int32 UpperLeaf = Record.LeafTransforms[1];
	const Chaos::FImplicitObjectPtr UpperCollision = Implicits[UpperLeaf];
	Implicits[UpperLeaf] = nullptr;
	TestFalse(TEXT("A rendered authored leaf without collision is rejected"),
		DublinFractureBake::ValidateCollisionData(*Asset, Record, Error));
	Implicits[UpperLeaf] = UpperCollision;

	UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(Owner);
	Owner->AddInstanceComponent(Component);
	const FVector Pivot(1200, 3400, 500);
	Component->SetWorldLocation(Pivot);
	Component->SetRestCollection(Asset);
	Component->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component->SetCollisionObjectType(ECC_WorldDynamic);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	TestNull(TEXT("Rest assignment does not create an unregistered proxy"), Component->GetPhysicsProxy());
	if (!TestTrue(TEXT("Registration initializes real query geometry, owner and particle states"),
		DublinFractureBake::RegisterRuntimePhysics(*Component, Record, Error)))
	{
		AddError(Error);
		return false;
	}
	const FGeometryDynamicCollection* Dynamic = Component->GetDynamicCollection();
	TestEqual(TEXT("Ground support starts kinematic"), Dynamic->DynamicState[Record.Anchors[0]],
		static_cast<uint8>(Chaos::EObjectStateType::Kinematic));
	TestEqual(TEXT("Unanchored piece is dynamic, not permanently anchored"), Dynamic->DynamicState[UpperLeaf],
		static_cast<uint8>(Chaos::EObjectStateType::Dynamic));
	const Chaos::Facades::FCollectionAnchoringFacade DynamicAnchoring(*Dynamic);
	TestTrue(TEXT("Registration copied the ground anchor"), DynamicAnchoring.IsAnchored(Record.Anchors[0]));
	TestFalse(TEXT("Registration does not anchor the upper piece"), DynamicAnchoring.IsAnchored(UpperLeaf));
	FHitResult Hit;
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinFractureRegistration), false);
	TestTrue(TEXT("Actual initialized GC collision answers a roof ray at a nonzero world pivot"),
		Component->LineTraceComponent(Hit, Pivot + FVector(0, 0, 300), Pivot + FVector(0, 0, 100), Query)
		&& Hit.GetComponent() == Component && Hit.ImpactPoint.Equals(Pivot + FVector(0, 0, 200), 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFractureAuthoredGraphTest,
	"DublinFlight.Destruction.Fracture.AuthoredContactGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFractureAuthoredGraphTest::RunTest(const FString& Parameters)
{
	UGeometryCollection* Asset = NewObject<UGeometryCollection>();
	const auto Geometry = Asset->GetGeometryCollection();
	Geometry->AppendGeometry(*GeometryCollection::MakeCubeElement(FTransform(FVector::ZeroVector), FVector(100)));
	Geometry->AppendGeometry(*GeometryCollection::MakeCubeElement(FTransform(FVector(100, 0, 0)), FVector(100)));
	// Distinct pieces can have coincident mass centers; the graph must not rely on Voronoi sites.
	Geometry->AppendGeometry(*GeometryCollection::MakeCubeElement(FTransform(FVector::ZeroVector), FVector(100)));
	FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(Geometry.Get());
	FString Error;
	if (!TestTrue(TEXT("Contact graph builds with coincident piece centers"),
		DublinFractureBake::BuildConnectionGraph(*Asset, Error)))
	{
		AddError(Error);
		return false;
	}
	const GeometryCollection::Facades::FCollectionConnectionGraphFacade Graph(*Geometry);
	TestTrue(TEXT("Saved graph satisfies the Chaos authored-graph branch"),
		Graph.IsValid() && Graph.HasValidConnections() && Graph.NumConnections() >= 2);
	TestTrue(TEXT("Subsequent proximity rebuilds preserve the authored graph"),
		Geometry->GetProximityProperties().bUseAsConnectionGraph);
	for (int32 I = 0; I < Graph.NumConnections(); ++I)
	{
		const auto Edge = Graph.GetConnection(I);
		TestTrue(TEXT("Every connection joins distinct siblings"),
			Edge.Key != Edge.Value && Geometry->Parent[Edge.Key] == Geometry->Parent[Edge.Value]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFractureRetrySitesTest,
	"DublinFlight.Destruction.Fracture.BoundedConservativeRetrySites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFractureRetrySitesTest::RunTest(const FString& Parameters)
{
	const FBox Bounds(FVector::ZeroVector, FVector(800, 800, 1600));
	const TArray<FVector> Original = DublinFractureBake::MakeFractureSites(Bounds, TEXT("retry-fixture"), 0);
	if (!TestEqual(TEXT("Original successful-grid density is unchanged"), Original.Num(), 16)) { return false; }
	TestTrue(TEXT("Original successful-grid placement is unchanged"), Original[0].Equals(FVector(200, 200, 200), 0));
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		const TArray<FVector> Sites = DublinFractureBake::MakeFractureSites(Bounds, TEXT("retry-fixture"), Attempt);
		TestTrue(TEXT("Retry sites are deterministic"),
			Sites == DublinFractureBake::MakeFractureSites(Bounds, TEXT("retry-fixture"), Attempt));
		TestTrue(TEXT("Site counts remain bounded and destructible"), Sites.Num() >= 2 && Sites.Num() <= (Attempt == 0 ? 64 : 16));
		for (int32 I = 0; I < Sites.Num(); ++I)
		{
			TestTrue(TEXT("Every retry site lies within source bounds"), Bounds.IsInside(Sites[I]));
			for (int32 J = 0; J < I; ++J)
			{
				TestTrue(TEXT("No coincident Voronoi sites"), FVector::DistSquared(Sites[I], Sites[J]) > 1.e-6);
			}
		}
	}
	const TArray<FVector> Final = DublinFractureBake::MakeFractureSites(Bounds, TEXT("retry-fixture"), 2);
	if (!TestEqual(TEXT("Last attempt is exactly two horizontal slices"), Final.Num(), 2)) { return false; }
	TestTrue(TEXT("Last attempt avoids fragile footprint corner cuts"),
		Final[0].X == Final[1].X && Final[0].Y == Final[1].Y && Final[0].Z < Final[1].Z);
	TestTrue(TEXT("No fourth attempt is available"), DublinFractureBake::MakeFractureSites(Bounds, TEXT("retry-fixture"), 3).IsEmpty());
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFractureRepeatedLeafSelectionTest,
	"DublinFlight.Destruction.Fracture.RepeatedHitsUseCurrentLeaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFractureRepeatedLeafSelectionTest::RunTest(const FString& Parameters)
{
	const TArray<int32> Leaves{1, 2, 3, 4};
	TArray<FTransform> Current;
	Current.Init(FTransform::Identity, 5);
	Current[1].SetLocation(FVector(-100, 0, 0));
	Current[2].SetLocation(FVector(100, 0, 0));
	Current[3].SetLocation(FVector(0, 0, 800));
	Current[4].SetLocation(FVector(0, 0, 1600));
	const FTransform ToWorld(FVector(1200, 3400, 500));
	FDublinImpact Cannon;
	Cannon.PositionCm = ToWorld.TransformPosition(FVector(0, 0, 800));
	Cannon.RadiusCm = 200;
	const TArray<int32> First = DublinDestruction::SelectImpactedFractureLeaves(Leaves, Current, ToWorld, Cannon);
	TestTrue(TEXT("Local cannon targets only the nearby authored leaf"), First == TArray<int32>{3});
	Current[0].SetLocation(FVector(99999));
	Current[3].SetLocation(FVector(0, 0, 1200));
	FDublinImpact Bomb;
	Bomb.PositionCm = ToWorld.GetLocation();
	Bomb.RadiusCm = 1000;
	const TArray<int32> Later = DublinDestruction::SelectImpactedFractureLeaves(Leaves, Current, ToWorld, Bomb);
	TestTrue(TEXT("Support hit finds both current support leaves, not the obsolete root"),
		Later == TArray<int32>({1, 2}));
	TestFalse(TEXT("Moved debris outside the impact is not selected from its old rest position"), Later.Contains(3));
	Cannon.RadiusCm = 1;
	const TArray<int32> Nearest = DublinDestruction::SelectImpactedFractureLeaves(Leaves, Current, ToWorld, Cannon);
	TestTrue(TEXT("Small impacts retain the nearest coarse-piece fallback"), Nearest == TArray<int32>{3});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFractureImpactVelocityTest,
	"DublinFlight.Destruction.Fracture.BoundedLocalImpactVelocity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFractureImpactVelocityTest::RunTest(const FString& Parameters)
{
	FDublinImpact Impact;
	TestTrue(TEXT("Game strength produces cm/s directly, independent of cooked fragment mass"),
		DublinDestruction::ImpactVelocityChange(Impact, FVector::ZeroVector).Equals(FVector(0, 0, 1500), 1.e-6));
	Impact.Kind = EDublinImpactKind::Bomb;
	Impact.Strength = 2;
	Impact.RadiusCm = 1000;
	const FVector HalfRadius(500, 0, 0);
	const FVector Velocity = DublinDestruction::ImpactVelocityChange(Impact, HalfRadius);
	TestTrue(TEXT("Radial velocity retains linear falloff"), FMath::IsNearlyEqual(Velocity.Size(), 1500.0, 1.e-6));
	TestTrue(TEXT("Direction retains the 100cm upward origin offset"),
		Velocity.GetSafeNormal().Equals(FVector(500, 0, 100).GetSafeNormal(), 1.e-6));
	const FVector Pivot(1200, 3400, 500);
	Impact.PositionCm = Pivot;
	TestTrue(TEXT("Velocity uses current world mass centers, not local coordinates"),
		DublinDestruction::ImpactVelocityChange(Impact, Pivot + HalfRadius).Equals(Velocity, 1.e-6));
	TestTrue(TEXT("The spherical boundary receives no velocity"),
		DublinDestruction::ImpactVelocityChange(Impact, Pivot + FVector(1000, 0, 0)).IsZero());
	TestTrue(TEXT("A nearest coarse leaf outside the radius receives strain but no remote kick"),
		DublinDestruction::ImpactVelocityChange(Impact, Pivot + FVector(1001, 0, 0)).IsZero());
	Impact.Strength = 1000000;
	TestTrue(TEXT("Maximum accepted strength cannot reuse the old impulse cap as a speed"),
		FMath::IsNearlyEqual(DublinDestruction::ImpactVelocityChange(Impact, Pivot).Size(),
			DublinDestruction::MaxFractureVelocityChangeCmPerSecond, 1.e-6));
	TestTrue(TEXT("A coarse support inside the bomb radius receives a bounded nonzero velocity"),
		DublinDestruction::ImpactVelocityChange(Impact, Pivot + FVector(300, 300, 170)).Size() > 1000);
	Impact.RadiusCm = 0;
	TestTrue(TEXT("Invalid radius cannot generate a nonfinite velocity"),
		DublinDestruction::ImpactVelocityChange(Impact, Pivot).IsZero());
	Impact.RadiusCm = 1000;
	Impact.Strength = std::numeric_limits<float>::infinity();
	TestTrue(TEXT("Nonfinite strength is rejected by the response helper"),
		DublinDestruction::ImpactVelocityChange(Impact, Pivot).IsZero());
	Impact.Strength = 2;
	TestTrue(TEXT("Nonfinite mass center cannot reach a physics command"),
		DublinDestruction::ImpactVelocityChange(Impact, FVector(std::numeric_limits<double>::infinity())).IsZero());
	return true;
}
#endif
