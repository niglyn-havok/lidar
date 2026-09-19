#include "City/DublinCityDetail.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "City/DublinCityDestruction.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Engine/World.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollectionProxyData.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	const TArray<FString> RepresentativeIds{TEXT("osm/way/233804861"), TEXT("osm/way/227766040"), TEXT("osm/way/1482695121"),
		TEXT("osm/way/1488401199"), TEXT("osm/relation/289528"), TEXT("osm/way/1488418443"),
		TEXT("osm/way/14047783/volume/1"), TEXT("bridge/osm/way/282571104/0"),
		TEXT("bridge/osm/way/399007550/0"), TEXT("landmark/spire")};
	const TArray<FString> SixCookIds{TEXT("osm/relation/3375620"), TEXT("osm/way/233797910"),
		TEXT("osm/way/657456422"), TEXT("osm/way/233804853"), TEXT("osm/way/51245099"), TEXT("osm/way/83174299")};

	bool LoadCity(FAutomationTestBase& Test, FDublinCityData& Data)
	{
		FString Error;
		if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { Test.AddError(Error); return false; }
		return true;
	}

	bool DetailMaterials(FAutomationTestBase& Test, UGeometryCollection& Asset)
	{
		for (const TCHAR* Path : {TEXT("/Game/Materials/M_DublinAerial.M_DublinAerial"),
			TEXT("/Game/Materials/M_DublinFacade.M_DublinFacade"),
			TEXT("/Game/Materials/MI_DublinFoundation.MI_DublinFoundation"),
			TEXT("/Game/Materials/MI_DublinFoundation.MI_DublinFoundation")})
		{
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path);
			if (!Material) { Test.AddError(FString(TEXT("Missing material: ")) + Path); return false; }
			Asset.Materials.Add(Material);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailRecipesTest, "DublinFlight.Destruction.CityDetail.RecipeCompatibilityAndTiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailRecipesTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	const auto* B = Data.Buildings.FindByPredicate([](const auto& Building) { return Building.Id == TEXT("osm/way/233804861"); });
	if (!TestNotNull(TEXT("Source golden fixture"), B)) { return false; }
	TestEqual(TEXT("Legacy hash is unchanged"), DublinDestruction::BuildingDigest(*B), FString(TEXT("ce3bb690d4e7f5506f2e45e83cc1463a")));
	TestEqual(TEXT("Validated pilot hash is unchanged"), DublinDestruction::BuildingDigest(*B, EDublinFractureRecipe::StructuralPilot),
		FString(TEXT("7e2a857fab2d1abf7197ea700809ad12")));
	TestTrue(TEXT("New recipe hashes are domain-separated"), DublinDestruction::BuildingDigest(*B, EDublinFractureRecipe::StructuralCity) !=
		DublinDestruction::BuildingDigest(*B, EDublinFractureRecipe::LayeredMasonry));
	TestEqual(TEXT("Container stays v3"), UDublinCityFractureLibrary::CurrentBakeVersion, 3);
	FDublinFractureRecord R;
	R.Recipe = EDublinFractureRecipe::StructuralCity;
	R.MemberCount = 4; R.PlanReason = TEXT("Tier fixture");
	for (int32 Tier : {128, 384, 768})
	{
		R.DetailTier = Tier; R.HullCount = R.PieceCount = Tier;
		TestTrue(TEXT("Declared finite tier accepted"), DublinFractureBake::HasValidPieceBudget(R));
		++R.PieceCount; ++R.HullCount;
		TestFalse(TEXT("Exceeding tier rejected"), DublinFractureBake::HasValidPieceBudget(R));
	}
	R.DetailTier = 1536; R.PieceCount = R.HullCount = 1536;
	TestFalse(TEXT("Dense is not globally enabled"), DublinFractureBake::HasValidPieceBudget(R));
	R.BroadSurfaceAreaM2 = 6001;
	TestTrue(TEXT("Large surface qualifies for Dense"), DublinFractureBake::HasValidPieceBudget(R));
	R.BroadSurfaceAreaM2 = 500; R.bDenseForOversize = true; R.LargestMemberAreaM2 = 17;
	TestTrue(TEXT("Recorded oversize member qualifies for Dense"), DublinFractureBake::HasValidPieceBudget(R));
	R.Recipe = EDublinFractureRecipe::SolidGrid;
	TestFalse(TEXT("New tiers do not reinterpret legacy cap"), DublinFractureBake::HasValidPieceBudget(R));
	DublinDetail::FPlan Plan; FString Error;
	if (!DublinDetail::AnalyzeBuilding(*B, Plan, Error)) { AddError(Error); return false; }
	int32 Seeds = 0;
	for (const auto& Member : Plan.Assembly.Members) { Seeds += Member.Seeds; }
	const int32 Target = FMath::Min(Plan.Tier, FMath::Max(Plan.Assembly.Members.Num() + 1, FMath::CeilToInt(Plan.BroadAreaM2 / 1.5)));
	TestEqual(TEXT("Seed allocation counts each minimum member seed exactly once"), Seeds, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailMixedFaceTest, "DublinFlight.Destruction.CityDetail.NativeMixedExteriorInteriorFace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailMixedFaceTest::RunTest(const FString& Parameters)
{
	FDublinCityBuilding Building;
	Building.Id = TEXT("fixture/mixed-exterior-interior");
	const TArray<FVector2D> Footprint{{0, 0}, {400, 0}, {400, 300}, {-100, 300}, {-100, 150}, {0, 150}};
	for (double Z : {0.0, 20.0})
	{
		for (const FVector2D& P : Footprint)
		{
			Building.Mesh.VerticesCm.Emplace(P.X, P.Y, Z);
			Building.Mesh.UV.Add(P / 100);
			Building.Mesh.ColorsRGBA.Add(FColor::White);
		}
	}
	const auto AddTriangle = [&](int32 A, int32 B, int32 C, uint8 Material)
	{
		Building.Mesh.Triangles.Append({A, B, C}); Building.MaterialIds.Add(Material);
	};
	for (const FIntVector T : TArray<FIntVector>{{0, 1, 5}, {1, 2, 5}, {2, 3, 5}, {3, 4, 5}})
	{
		AddTriangle(T.X, T.Y, T.Z, 2);
		AddTriangle(T.X + 6, T.Z + 6, T.Y + 6, 0);
	}
	for (int32 I = 0; I < 6; ++I)
	{
		const int32 J = (I + 1) % 6;
		AddTriangle(I, I + 6, J + 6, 1); AddTriangle(I, J + 6, J, 1);
	}
	const TArray<TArray<FVector2D>> Parts{
		{{0, 0}, {400, 0}, {400, 300}, {0, 300}},
		{{-100, 150}, {0, 150}, {0, 300}, {-100, 300}}};
	FGeometryCollection Geometry;
	FDublinFractureRecord Record;
	Record.Recipe = EDublinFractureRecipe::StructuralCity;
	Record.MemberCount = Record.PieceCount = Parts.Num();
	DublinStructural::FAssembly Assembly;
	double Retained = 0;
	FString Error;
	for (const auto& Polygon : Parts)
	{
		DublinStructural::FMember M;
		M.Polygon = Polygon; M.MinZ = 0; M.MaxZ = 20; M.bSourcePlaneExterior = true;
		FMeshDescription Mesh;
		if (!DublinStructural::MakeMemberMeshDescription(Building, Assembly, M, Mesh, Error)) { AddError(Error); return false; }
		const int32 Bone = Geometry.Transform.Num();
		FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform::Identity, &Geometry, nullptr, false, false, false);
		Record.LeafTransforms.Add(Bone);
		double Volume = 0;
		if (!DublinStructural::ValidateLeafGeometry(Geometry, Bone, Volume, Error)) { AddError(Error); return false; }
		Retained += Volume;
	}
	Geometry.ReindexMaterials();
	for (int32 F = 0; F < Geometry.MaterialID.Num(); ++F) { Geometry.Internal[F] = Geometry.MaterialID[F] == 3; }
	if (!DublinStructural::ValidateExteriorGeometry(Building, Geometry, Record, Error)) { AddError(Error); return false; }
	TestTrue(TEXT("Mixed coplanar faces preserve closed positive volume and full exterior provenance"),
		FMath::IsNearlyEqual(Retained, 2700000.0, 1.0));
	DublinStructural::FMember Outside;
	Outside.Polygon = {{-100, 0}, {0, 0}, {0, 300}, {-100, 300}};
	Outside.MinZ = 0; Outside.MaxZ = 20; Outside.bSourcePlaneExterior = true;
	FMeshDescription Rejected;
	TestFalse(TEXT("Missing source coverage outside the concave source solid cannot become a neutral cap"),
		DublinStructural::MakeMemberMeshDescription(Building, Assembly, Outside, Rejected, Error));
	TestTrue(TEXT("Exterior void rejection is explicit"), Error.Contains(TEXT("source solid")) || Error.Contains(TEXT("courtyard")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailFloatBoundaryTest, "DublinFlight.Destruction.CityDetail.NativeExteriorFloatBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailFloatBoundaryTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	for (const FString& Id : TArray<FString>{TEXT("osm/way/1482697300"), TEXT("bridge/osm/way/286891495/0"),
		TEXT("bridge/osm/way/286891496/0"), TEXT("bridge/osm/way/43319604/0"), TEXT("bridge/osm/way/43325903/0")})
	{
		const auto* Building = Data.Buildings.FindByPredicate([&](const auto& B) { return B.Id == Id; });
		if (!TestNotNull(*Id, Building)) { return false; }
		const FString Before = DublinDestruction::BuildingGeometryDigest(*Building);
		DublinDetail::FPlan Plan; FString Error;
		if (!DublinDetail::PreflightGeometry(*Building, Plan, Error)) { AddError(Id + TEXT(": ") + Error); return false; }
		TestEqual(TEXT("Float-boundary verification never changes original source coordinates or attributes"),
			DublinDestruction::BuildingGeometryDigest(*Building), Before);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailFinalFloatTest, "DublinFlight.Destruction.CityDetail.NativeFinalFloatRepresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailFinalFloatTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	for (const FString& Id : TArray<FString>{TEXT("osm/way/1482697300"), TEXT("osm/way/228198482")})
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Building) { return Building.Id == Id; });
		if (!TestNotNull(*Id, B)) { return false; }
		const FString Before = DublinDestruction::BuildingGeometryDigest(*B);
		DublinDetail::FPlan Plan; FString Error;
		if (!DublinDetail::PreflightGeometry(*B, Plan, Error)) { AddError(Id + TEXT(": ") + Error); return false; }
		TestEqual(TEXT("Final-float construction preserves the original data and attributes"),
			DublinDestruction::BuildingGeometryDigest(*B), Before);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailClassificationTest, "DublinFlight.Destruction.CityDetail.ClassifyAllSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailClassificationTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	int32 Structural = 0, Masonry = 0, Unsupported = 0, Courtyards = 0, Variable = 0;
	TArray<FString> BeforeDigests;
	for (const auto& B : Data.Buildings) { BeforeDigests.Add(DublinDestruction::BuildingGeometryDigest(B)); }
	TArray<DublinDetail::FPlan> Plans;
	TArray<FString> Failures;
	DublinDetail::AnalyzeCity(Data, Plans, Failures);
	TestEqual(TEXT("Whole-city planner retains every source row including unsupported cases"), Plans.Num(), Data.Buildings.Num());
	for (int32 I = 0; I < Data.Buildings.Num(); ++I)
	{
		const auto& B = Data.Buildings[I];
		const auto& Plan = Plans[I];
		if (!Plan.bSupported) { ++Unsupported; AddError(B.Id + TEXT(": ") + Plan.Reason); continue; }
		TestEqual(TEXT("Planner row keeps exact source identity"), Plan.SourceId, B.Id);
		Structural += Plan.Recipe == EDublinFractureRecipe::StructuralCity;
		Masonry += Plan.Recipe == EDublinFractureRecipe::LayeredMasonry;
		Courtyards += Plan.HoleCount > 0; Variable += Plan.bVariableRoof;
		TestTrue(TEXT("Every successful plan selects an explicit new recipe"), DublinFractureBake::IsDetailedRecipe(Plan.Recipe));
		TestFalse(TEXT("Classification reason is required"), Plan.Reason.IsEmpty());
		TestTrue(TEXT("Finite declared member budget"), Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num() <= Plan.Tier);
		int32 PlannedLeaves = Plan.Tetrahedra.Num() * 2;
		for (const auto& Member : Plan.Assembly.Members) { PlannedLeaves += Member.Seeds; }
		TestTrue(TEXT("Planned physical leaves fit the declared tier"), PlannedLeaves >= 2 && PlannedLeaves <= Plan.Tier);
		AddInfo(FString::Printf(TEXT("CityDetail source=%s recipe=%d tier=%d members=%d plannedLeaves=%d holes=%d variableRoof=%d reason=%s"),
			*B.Id, static_cast<int32>(Plan.Recipe), Plan.Tier, Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num(),
			PlannedLeaves, Plan.HoleCount, Plan.bVariableRoof, *Plan.Reason));
		TestTrue(TEXT("Every plan contains collision probes"), !Plan.Probes.IsEmpty());
		if (Plan.Recipe == EDublinFractureRecipe::StructuralCity) { TestFalse(TEXT("Hollow recipe has explicit empty-room probes"), Plan.VoidSamplesCm.IsEmpty()); }
		TestEqual(TEXT("Source geometry remains unchanged"), DublinDestruction::BuildingGeometryDigest(B), BeforeDigests[I]);
	}
	AddInfo(FString::Printf(TEXT("CityDetail classification: total=%d structural=%d masonry=%d unsupported=%d courtyardBuildings=%d variableRoofs=%d"),
		Data.Buildings.Num(), Structural, Masonry, Unsupported, Courtyards, Variable));
	TestEqual(TEXT("All 1044 records accounted"), Structural + Masonry + Unsupported, 1044);
	TestEqual(TEXT("All 16 source courtyards supported"), Courtyards, 16);
	TestEqual(TEXT("No silent legacy fallback or unsupported source"), Unsupported, 0);
	return Unsupported == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailRepresentativePreflightTest, "DublinFlight.Destruction.CityDetail.PreflightRepresentativeMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailRepresentativePreflightTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	for (const FString& Id : RepresentativeIds)
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Building) { return Building.Id == Id; });
		if (!TestNotNull(*Id, B)) { return false; }
		DublinDetail::FPlan Plan; FString Error;
		if (!DublinDetail::PreflightGeometry(*B, Plan, Error)) { AddError(Id + TEXT(": ") + Error); return false; }
		AddInfo(FString::Printf(TEXT("CityDetail representative preflight source=%s members=%d tier=%d"),
			*Id, Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num(), Plan.Tier));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailPreflightTest, "DublinFlight.Destruction.CityDetail.PreflightAllMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailPreflightTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	int32 Passed = 0, Failed = 0;
	for (const auto& B : Data.Buildings)
	{
		DublinDetail::FPlan Plan; FString Error;
		if (!DublinDetail::PreflightGeometry(B, Plan, Error)) { ++Failed; AddError(B.Id + TEXT(": ") + Error); }
		else { ++Passed; }
	}
	AddInfo(FString::Printf(TEXT("Native source-only mesh preflight: passed=%d failed=%d. No GC/library packages saved."), Passed, Failed));
	TestEqual(TEXT("All sources produce closed source-conserving member meshes before any bulk bake"), Passed, 1044);
	return Failed == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailSixCookTest, "DublinFlight.Destruction.CityDetail.NativeSixSourceFirstAttemptCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailSixCookTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	int32 Passed = 0;
	const TMap<FString, FString> FrozenDigests{
		{TEXT("osm/relation/3375620"), TEXT("5be787ac3cc270d80953b45036d0c2c7")},
		{TEXT("osm/way/233797910"), TEXT("94900caf4232026e5ec81e2943ce90ac")},
		{TEXT("osm/way/657456422"), TEXT("e9802583b3a6528989b6f8347800f4a0")},
		{TEXT("osm/way/233804853"), TEXT("02c0a6cd0f1c155c408f844fd661d3d3")},
		{TEXT("osm/way/51245099"), TEXT("713bd063bc5ddc8145ac360db9bcf170")},
		{TEXT("osm/way/83174299"), TEXT("5d2222fc1d5c20049c28cf9b7c9377c0")}};
	for (const FString& Id : SixCookIds)
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Building) { return Building.Id == Id; });
		if (!TestNotNull(*Id, B)) { continue; }
		if (!TestEqual(TEXT("Recorded failing first-attempt seed contract remains unchanged"),
			DublinDestruction::BuildingDigest(*B, EDublinFractureRecipe::StructuralCity), FrozenDigests.FindChecked(Id))) { continue; }
		TStrongObjectPtr<UGeometryCollection> Asset(NewObject<UGeometryCollection>());
		if (!DetailMaterials(*this, *Asset.Get())) { continue; }
		FDublinFractureRecord R; FString Error;
		if (!DublinDetail::BuildCollection(*B, *Asset.Get(), R, Error, 0))
		{
			AddError(Id + TEXT(" fixedAttempt=0 currentDigest=") + R.SourceDigest + TEXT(": ") + Error);
			continue;
		}
		TestEqual(TEXT("Lossless native collision storage is explicit"), R.CollisionStorageVersion, 1);
		Asset->bImportCollisionFromSource = false;
		TestFalse(TEXT("Exact collision cannot silently switch back to lossy recooking"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), R, Error));
		Asset->bImportCollisionFromSource = true;
		R.CollisionStorageVersion = 2;
		TestFalse(TEXT("Unknown collision storage is rejected"), DublinFractureBake::HasValidPieceBudget(R));
		R.CollisionStorageVersion = 1;
		const auto Geometry = Asset->GetGeometryCollection();
		auto& External = Geometry->ModifyAttribute<Chaos::FImplicitObjectPtr>(
			FGeometryCollection::ExternalCollisionsAttribute, FGeometryCollection::TransformGroup);
		const auto SavedLeaf = External[R.LeafTransforms[0]];
		External[R.LeafTransforms[0]] = nullptr;
		TestFalse(TEXT("Missing persistent leaf collision cannot pass"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), R, Error));
		External[R.LeafTransforms[0]] = SavedLeaf;
		auto* AuthoredRoot = External[R.RootTransform]->GetObject<Chaos::FImplicitObjectUnion>();
		if (!TestNotNull(TEXT("Persistent exact compound"), AuthoredRoot)) { continue; }
		auto& AuthoredParts = AuthoredRoot->GetObjects();
		const auto SavedPart = AuthoredParts.Last();
		AuthoredParts.Last() = AuthoredParts[0];
		TestFalse(TEXT("Persistent duplicate hull cannot hide behind correct cached collision"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), R, Error));
		AuthoredParts.Last() = SavedPart;
		auto& Implicits = Geometry->ModifyAttribute<Chaos::FImplicitObjectPtr>(
			FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
		auto* CookedRoot = Implicits[R.RootTransform]->GetObject<Chaos::FImplicitObjectUnion>();
		if (!TestNotNull(TEXT("Cooked exact compound"), CookedRoot)) { continue; }
		auto& CookedParts = CookedRoot->GetObjects();
		const auto SavedCookedPart = CookedParts.Last();
		CookedParts.Last() = CookedParts[0];
		TestFalse(TEXT("Cooked duplicate hull remains rejected"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), R, Error));
		CookedParts.Last() = SavedCookedPart;
		TArray<uint8> Bytes;
		FCustomVersionContainer Versions;
		{
			FMemoryWriter Writer(Bytes, true);
			Chaos::FChaosArchive Archive(Writer);
			Geometry->Serialize(Archive);
			Versions = Writer.GetCustomVersions();
		}
		auto ReloadedGeometry = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
		{
			FMemoryReader Reader(Bytes, true);
			Reader.SetCustomVersions(Versions);
			Chaos::FChaosArchive Archive(Reader);
			ReloadedGeometry->Serialize(Archive);
		}
		TStrongObjectPtr<UGeometryCollection> Reloaded(NewObject<UGeometryCollection>());
		Reloaded->bOptimizeConvexes = false;
		Reloaded->bImportCollisionFromSource = true;
		Reloaded->SetGeometryCollection(ReloadedGeometry);
		if (!DublinStructural::ValidateCookedCollision(*Reloaded.Get(), R, Error) ||
			!DublinStructural::ValidateBuildingCollision(*Reloaded.Get(), *B, R, Error))
		{
			AddError(Id + TEXT(" serialized exact collision: ") + Error);
			continue;
		}
		Asset->InvalidateCollection();
		Asset->CreateSimulationData();
		if (!DublinStructural::ValidateCookedCollision(*Asset.Get(), R, Error) ||
			!DublinStructural::ValidateBuildingCollision(*Asset.Get(), *B, R, Error))
		{
			AddError(Id + TEXT(" rebuilt exact collision: ") + Error);
			continue;
		}
		++Passed;
		AddInfo(FString::Printf(TEXT("Fixed-first-attempt source=%s digest=%s pieces=%d hulls=%d"), *Id, *R.SourceDigest, R.PieceCount, R.HullCount));
	}
	TestEqual(TEXT("All six recorded sources pass without seed retries"), Passed, 6);
	return Passed == 6;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailSavedCatalogTest, "DublinFlight.Destruction.CityDetail.NativeSavedCatalogCollisions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailSavedCatalogTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	const UDublinCityFractureLibrary* Library = DublinFractureBake::FindLibrary();
	if (!TestNotNull(TEXT("Saved detailed catalog"), Library)) { return false; }
	TArray<FString> Pending;
	if (!TestTrue(TEXT("All saved sources have current detail"), DublinFractureBake::HasCompleteDetailCoverage(Data.Buildings, *Library, Pending)))
	{
		for (const FString& Id : Pending) { AddError(TEXT("Missing saved detail: ") + Id); }
		return false;
	}
	for (const FDublinFractureRecord& R : Library->Records)
	{
		if (!TestTrue(TEXT("No hidden solid-grid or pilot fallback"), DublinFractureBake::IsDetailedRecipe(R.Recipe))) { return false; }
	}
	if (!TestEqual(TEXT("Exact full-city record count"), Library->Records.Num(), 1044)) { return false; }
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Saved collision fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Saved collision fixture owner"), Owner)) { return false; }
	TArray<FString> Ids = RepresentativeIds;
	Ids.Append(SixCookIds);
	for (const FString& Id : Ids)
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Building) { return Building.Id == Id; });
		const auto* R = Library->Find(Id);
		if (!TestNotNull(*Id, B) || !TestNotNull(TEXT("Saved source record"), R)) { return false; }
		const UGeometryCollection* Asset = R->Collection.LoadSynchronous();
		if (!TestNotNull(TEXT("Load the published asset, not a generated replacement"), Asset)) { return false; }
		FString Error;
		if (!DublinFractureBake::ValidateCollisionData(*Asset, *R, Error) ||
			!DublinStructural::ValidateBuildingCollision(*Asset, *B, *R, Error))
		{
			AddError(Id + TEXT(" saved collision: ") + Error); return false;
		}
		UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(Owner);
		Owner->AddInstanceComponent(Component);
		Component->SetWorldLocation(B->PivotCm);
		Component->SetRestCollection(Asset);
		Component->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetCollisionObjectType(ECC_WorldDynamic);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		const bool bPassed = DublinFractureBake::RegisterRuntimePhysics(*Component, *R, Error) &&
			DublinStructural::ValidateRegisteredCollision(*Component, *B, *R, Error) &&
			DublinStructural::ValidateRegisteredProbeEquivalence(*Component, *R, Error);
		Owner->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		if (!bPassed) { AddError(Id + TEXT(" saved registered collision: ") + Error); return false; }
		AddInfo(FString::Printf(TEXT("Published source=%s storage=%d pieces=%d hulls=%d"),
			*Id, R->CollisionStorageVersion, R->PieceCount, R->HullCount));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailNativeTest, "DublinFlight.Destruction.CityDetail.NativeRepresentativeCollections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailNativeTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!LoadCity(*this, Data)) { return false; }
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient native detail fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Detail fixture owner"), Owner)) { return false; }
	for (const FString& Id : RepresentativeIds)
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Building) { return Building.Id == Id; });
		if (!TestNotNull(*Id, B)) { return false; }
		TStrongObjectPtr<UGeometryCollection> Asset(NewObject<UGeometryCollection>());
		if (!DetailMaterials(*this, *Asset.Get())) { return false; }
		FDublinFractureRecord R; FString Error;
		if (!DublinDetail::BuildCollection(*B, *Asset.Get(), R, Error)) { AddError(Id + TEXT(": ") + Error); return false; }
		TestTrue(TEXT("Every physical leaf has declared collision cost"), DublinFractureBake::HasValidPieceBudget(R));
		TestTrue(TEXT("Source stripping cannot remove the authored contact graph"), !Asset->bStripOnCook);
		UGeometryCollectionComponent* C = NewObject<UGeometryCollectionComponent>(Owner);
		Owner->AddInstanceComponent(C); C->SetWorldLocation(B->PivotCm); C->SetRestCollection(Asset.Get());
		C->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
		C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); C->SetCollisionObjectType(ECC_WorldDynamic);
		C->SetCollisionResponseToAllChannels(ECR_Block);
		if (!DublinFractureBake::RegisterRuntimePhysics(*C, R, Error) ||
			!DublinStructural::ValidateRegisteredCollision(*C, *B, R, Error))
		{
			AddError(Id + TEXT(": ") + Error); return false;
		}
		Owner->RemoveInstanceComponent(C); C->DestroyComponent();
		AddInfo(FString::Printf(TEXT("%s: recipe=%d tier=%d members=%d pieces=%d hulls=%d"),
			*Id, static_cast<int32>(R.Recipe), R.DetailTier, R.MemberCount, R.PieceCount, R.HullCount));
	}
	return true;
}
#endif
