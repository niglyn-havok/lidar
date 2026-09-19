#include "City/DublinCityStructural.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "City/DublinCityDestruction.h"
#include "City/DublinCityWorld.h"
#include "Algo/Reverse.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/Convex.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Engine/World.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "GeometryCollectionProxyData.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/CustomVersion.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	const TArray<FString> PilotIds{
		TEXT("osm/way/233804861"), TEXT("osm/way/233804877"), TEXT("osm/way/389853640")};

	const FDublinCityBuilding* Find(const FDublinCityData& City, const FString& Id)
	{
		return City.Buildings.FindByPredicate([&](const FDublinCityBuilding& B) { return B.Id == Id; });
	}

	bool Load(FAutomationTestBase& Test, FDublinCityData& Data)
	{
		FString Error;
		if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { Test.AddError(Error); return false; }
		return true;
	}

	bool SetMaterials(UGeometryCollection& Asset, FAutomationTestBase& Test)
	{
		for (const TCHAR* Path : {
			TEXT("/Game/Materials/M_DublinAerial.M_DublinAerial"),
			TEXT("/Game/Materials/M_DublinFacade.M_DublinFacade"),
			TEXT("/Game/Materials/MI_DublinFoundation.MI_DublinFoundation"),
			TEXT("/Game/Materials/MI_DublinFoundation.MI_DublinFoundation")})
		{
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path);
			if (!Material) { Test.AddError(FString(TEXT("Missing structural test material: ")) + Path); return false; }
			Asset.Materials.Add(Material);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailFaceNormalsTest,
	"DublinFlight.Destruction.CityDetail.NativeFaceNormalsAndSeams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailFaceNormalsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector> Boundary{{0, 0, 0}, {200, 0, 0}, {400, 0, 0}, {400, 300, 0}, {0, 300, 0}};
	FVector Normal;
	if (!TestTrue(TEXT("Collinear first three vertices do not erase a positive-area face"),
		DublinStructural::FindFaceNormal(MakeArrayView(Boundary), Normal))) { return false; }
	TestTrue(TEXT("Face normal follows UE clockwise winding"), Normal.Equals(-FVector::UpVector, 1.e-12));
	TArray<FVector> Reversed = Boundary;
	Algo::Reverse(Reversed);
	TestTrue(TEXT("Reversed winding remains valid"), DublinStructural::FindFaceNormal(MakeArrayView(Reversed), Normal));
	TestTrue(TEXT("Reversed winding reverses its normal"), Normal.Equals(FVector::UpVector, 1.e-12));
	const TArray<FVector> RoundedPrefix{{0, 0, 0}, {1, 0, 0}, {2, .001, .00001}, {400, 300, 0}, {0, 300, 0}};
	TestTrue(TEXT("Rounded near-collinear prefixes do not define a large face's clipping plane"),
		DublinStructural::FindFaceNormal(MakeArrayView(RoundedPrefix), Normal));
	TestTrue(TEXT("Maximum-area face normal remains stable"), Normal.Equals(-FVector::UpVector, 1.e-12));
	const TArray<FVector> Thin{{800, 800, 0}, {800 + 1.e-12, 800, 0}, {800 + 1.e-12, 800, 20}, {800, 800, 20}};
	TestTrue(TEXT("Fixture reproduces default-normal positive-area rejection"),
		DublinCity::ClockwiseNormal(Thin[0], Thin[1], Thin[2]).IsNearlyZero());
	TestTrue(TEXT("Scale-safe predicate retains the positive-area face"),
		DublinStructural::FindFaceNormal(MakeArrayView(Thin), Normal));
	TestTrue(TEXT("Thin face receives finite unit normal"), !Normal.ContainsNaN() && FMath::IsNearlyEqual(Normal.SizeSquared(), 1.0, 1.e-12));
	const TArray<FVector> Collinear{{0, 0, 0}, {200, 0, 0}, {400, 0, 0}};
	TestFalse(TEXT("Genuinely collinear face is rejected"), DublinStructural::FindFaceNormal(MakeArrayView(Collinear), Normal));
	FDublinCityBuilding Building;
	Building.Id = TEXT("fixture/collinear-boundary-seam");
	DublinStructural::FAssembly Assembly;
	DublinStructural::FMember Member;
	for (const FVector& P : Boundary) { Member.Polygon.Emplace(P.X, P.Y); }
	Member.MinZ = 0; Member.MaxZ = 20; Member.bSourcePlaneExterior = true;
	FMeshDescription Mesh; FString Error;
	if (!DublinStructural::MakeMemberMeshDescription(Building, Assembly, Member, Mesh, Error)) { AddError(Error); return false; }
	const auto Positions = FStaticMeshAttributes(Mesh).GetVertexPositions();
	for (const FVector& P : Boundary)
	{
		for (double Z : {0.0, 20.0})
		{
			const FVector3f Expected(P.X, P.Y, Z);
			bool bFound = false;
			for (const FVertexID V : Mesh.Vertices().GetElementIDs()) { bFound |= Positions[V].Equals(Expected, 1.e-5f); }
			TestTrue(TEXT("Closed mesh retains every source boundary seam at both heights"), bFound);
		}
	}
	FGeometryCollection Geometry;
	FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform::Identity, &Geometry, nullptr, false, false, false);
	double Volume = 0;
	if (!DublinStructural::ValidateLeafGeometry(Geometry, 0, Volume, Error)) { AddError(Error); return false; }
	TestTrue(TEXT("Seam-preserving mesh remains closed with positive conserved volume"), FMath::IsNearlyEqual(Volume, 2400000.0, 1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailSeamRepairTest,
	"DublinFlight.Destruction.CityDetail.NativeLeafSeamRepair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailSeamRepairTest::RunTest(const FString& Parameters)
{
	FDublinCityBuilding Building;
	Building.Id = TEXT("fixture/bounded-leaf-seams");
	DublinStructural::FAssembly Assembly;
	DublinStructural::FMember Member;
	Member.Polygon = {{0, 0}, {400, 0}, {400, 300}, {0, 300}};
	Member.MinZ = 0; Member.MaxZ = 20; Member.bSourcePlaneExterior = true;
	FMeshDescription Mesh; FString Error;
	if (!DublinStructural::MakeMemberMeshDescription(Building, Assembly, Member, Mesh, Error)) { AddError(Error); return false; }
	FStaticMeshAttributes Attributes(Mesh);
	const auto Positions = Attributes.GetVertexPositions();
	auto Colors = Attributes.GetVertexInstanceColors();
	auto UVs = Attributes.GetVertexInstanceUVs();
	for (const FVertexInstanceID Instance : Mesh.VertexInstances().GetElementIDs())
	{
		const FVector3f P = Positions[Mesh.GetVertexInstanceVertex(Instance)];
		Colors[Instance] = FVector4f(P.X / 400, P.Y / 300, P.Z / 20, 1);
		UVs.Set(Instance, 0, FVector2f(P.X / 400, P.Y / 300));
		UVs.Set(Instance, 1, FVector2f(P.Y / 300, P.Z / 20));
	}
	for (int32 Mode : {0, 1, 2})
	{
		const bool bTJunction = Mode != 0;
		const float JunctionGap = Mode == 2 ? .1f : .0004f;
		FGeometryCollection Geometry;
		for (int32 I = 0; I < 2; ++I)
		{
			FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform::Identity, &Geometry, nullptr, false, false, false);
		}
		const int32 G = Geometry.TransformToGeometryIndex[0];
		TSet<int32> Midpoints;
		if (bTJunction)
		{
			for (int32 V = Geometry.VertexStart[G]; V < Geometry.VertexStart[G] + Geometry.VertexCount[G]; ++V)
			{
				if (Geometry.Normal[V].X < -.9 && Geometry.Vertex[V].Equals(FVector3f(0, 150, 10), 1.e-5f)) { Midpoints.Add(V); }
			}
			if (!TestTrue(TEXT("T-junction fixture contains the source face centre"), !Midpoints.IsEmpty())) { return false; }
			TArray<int32> Remove;
			for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
			{
				const FIntVector T = Geometry.Indices[F];
				int32 Mid = 0, Bottom = 0;
				for (int32 K = 0; K < 3; ++K) { Mid += Midpoints.Contains(T[K]); Bottom += Geometry.Vertex[T[K]].Z == 0; }
				if (Mid == 1 && Bottom == 2) { Remove.Add(F); }
			}
			if (!TestEqual(TEXT("Fixture removes one face whose centre has moved onto its boundary"), Remove.Num(), 1)) { return false; }
			auto Layers = GeometryCollection::UV::FindActiveUVLayers(Geometry);
			for (int32 V : Midpoints)
			{
				Geometry.Vertex[V].Z = JunctionGap;
				Layers[0][V] = FVector2f(0, .5f); Layers[1][V] = FVector2f(.5f, JunctionGap / 20);
				Geometry.Color[V] = FLinearColor(0, .5f, JunctionGap / 20, 1);
			}
			Geometry.RemoveElements(FGeometryCollection::FacesGroup, Remove);
		}
		else
		{
			for (int32 V = Geometry.VertexStart[G]; V < Geometry.VertexStart[G] + Geometry.VertexCount[G]; ++V)
			{
				if (Geometry.Normal[V].X < -.9) { Geometry.Vertex[V].X += .005f; }
			}
		}
		double Volume = 0;
		TestFalse(TEXT("Original fixture genuinely has open topology"),
			DublinStructural::ValidateLeafGeometry(Geometry, 0, Volume, Error));
		if (Mode == 2)
		{
			TestFalse(TEXT("A real opening wider than the precision contract cannot be filled"),
				DublinStructural::RepairLeafSeams(Geometry, 0, Error));
			continue;
		}
		if (!DublinStructural::RepairLeafSeams(Geometry, 0, Error) ||
			!DublinStructural::ValidateLeafGeometry(Geometry, 0, Volume, Error)) { AddError(Error); return false; }
		TestEqual(TEXT("Repair preserves transform IDs and sibling count"), Geometry.Transform.Num(), 2);
		TestTrue(TEXT("Repair conserves leaf volume within the original budget"), FMath::IsNearlyEqual(Volume, 2400000.0, 240.0));
		if (!DublinStructural::ValidateLeafGeometry(Geometry, 1, Volume, Error)) { AddError(Error); return false; }
		TestTrue(TEXT("Repair leaves sibling geometry unchanged"), FMath::IsNearlyEqual(Volume, 2400000.0, 1.0));
		const int32 RepairedG = Geometry.TransformToGeometryIndex[0];
		const auto Layers = GeometryCollection::UV::FindActiveUVLayers(Geometry);
		bool bRetainedJunctionPosition = false;
		for (int32 V = Geometry.VertexStart[RepairedG]; V < Geometry.VertexStart[RepairedG] + Geometry.VertexCount[RepairedG]; ++V)
		{
			const FVector3f P = Geometry.Vertex[V];
			bRetainedJunctionPosition |= P.Equals(FVector3f(0, 150, JunctionGap), 1.e-7f);
			TestTrue(TEXT("Repaired UV0 keeps its original source field"), FVector2f(Layers[0][V]).Equals(FVector2f(P.X / 400, P.Y / 300), 2.e-4f));
			TestTrue(TEXT("Repaired UV1 keeps its independent source field"), FVector2f(Layers[1][V]).Equals(FVector2f(P.Y / 300, P.Z / 20), 2.e-4f));
			TestTrue(TEXT("Repaired colors retain the source field"), Geometry.Color[V].Equals(FLinearColor(P.X / 400, P.Y / 300, P.Z / 20, 1), 2.e-4f));
		}
		if (bTJunction) { TestTrue(TEXT("Thin internal cut-seam stitching does not displace existing vertices"), bRetainedJunctionPosition); }
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailRepairedCookTest,
	"DublinFlight.Destruction.CityDetail.NativeRepairedGeometryCookOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailRepairedCookTest::RunTest(const FString& Parameters)
{
	FDublinCityBuilding Building;
	Building.Id = TEXT("fixture/repaired-cook-order");
	DublinStructural::FAssembly Assembly;
	DublinStructural::FMember Member;
	Member.Polygon = {{0, 0}, {400, 0}, {400, 300}, {0, 300}};
	Member.MinZ = 0; Member.MaxZ = 20; Member.bSourcePlaneExterior = true;
	FMeshDescription Mesh; FString Error;
	if (!DublinStructural::MakeMemberMeshDescription(Building, Assembly, Member, Mesh, Error)) { AddError(Error); return false; }
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
	for (int32 I = 0; I < 2; ++I)
	{
		FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform(FVector(I * 400, 0, 0)),
			Geometry.Get(), nullptr, false, false, false);
	}
	const int32 G = Geometry->TransformToGeometryIndex[0];
	for (int32 V = Geometry->VertexStart[G]; V < Geometry->VertexStart[G] + Geometry->VertexCount[G]; ++V)
	{
		if (Geometry->Normal[V].X < -.9) { Geometry->Vertex[V].X += .005f; }
	}
	double Volume = 0;
	TestFalse(TEXT("The early leaf needs real topology repair"), DublinStructural::ValidateLeafGeometry(*Geometry, 0, Volume, Error));
	if (!DublinStructural::RepairLeafSeams(*Geometry, 0, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("Repair preserves the later sibling transform"), Geometry->Transform.Num(), 2);
	for (int32 Geo = 0; Geo < Geometry->TransformIndex.Num(); ++Geo)
	{
		TestEqual(TEXT("Geometry reverse mapping remains consistent after reordered replacement"),
			Geometry->TransformToGeometryIndex[Geometry->TransformIndex[Geo]], Geo);
	}
	FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(Geometry.Get());
	TArray<int32> Roots;
	FGeometryCollectionClusteringUtility::GetRootBones(Geometry.Get(), Roots);
	if (!TestEqual(TEXT("Cook fixture has one compound root"), Roots.Num(), 1)) { return false; }
	FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Geometry.Get(), Roots[0]);
	if (!DublinStructural::NormalizeGeometryOrder(*Geometry, Error)) { AddError(Error); return false; }
	TStrongObjectPtr<UGeometryCollection> Asset(NewObject<UGeometryCollection>());
	if (!SetMaterials(*Asset.Get(), *this)) { return false; }
	Asset->SetGeometryCollection(Geometry);
	Asset->EnableClustering = true; Asset->DamageThreshold = {100};
	Asset->bOptimizeConvexes = false; Asset->bImportCollisionFromSource = false;
	Asset->bStripOnCook = false;
	if (Asset->SizeSpecificData.IsEmpty()) { Asset->SizeSpecificData.Add(UGeometryCollection::GeometryCollectionSizeSpecificDataDefaults()); }
	for (auto& Size : Asset->SizeSpecificData)
	{
		Size.CollisionShapes.SetNum(1);
		Size.CollisionShapes[0].CollisionType = ECollisionTypeEnum::Chaos_Volumetric;
		Size.CollisionShapes[0].ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Convex;
		Size.CollisionShapes[0].CollisionMarginFraction = 0;
		Size.CollisionShapes[0].CollisionObjectReductionPercentage = 0;
	}
	auto Properties = Geometry->GetConvexProperties();
	Properties.SimplificationThreshold = 0; Properties.OverlapRemovalShrinkPercent = 0;
	Geometry->SetConvexProperties(Properties);
	Asset->InvalidateCollection(); Asset->UpdateGeometryDependentProperties();
	TArray<int32> Leaves{0, 1};
	FGeometryCollectionConvexUtility::FLeafConvexHullSettings Settings(0, EGenerateConvexMethod::ComputedFromGeometry);
	FGeometryCollectionConvexUtility::GenerateLeafConvexHulls(*Geometry, true, MakeArrayView(Leaves), Settings);
	FGeometryCollectionConvexUtility::CopyChildConvexes(Geometry.Get(), MakeArrayView(Roots), Geometry.Get(), MakeArrayView(Roots), true);
	if (!DublinFractureBake::BuildConnectionGraph(*Asset.Get(), Error)) { AddError(Error); return false; }
	Asset->InvalidateCollection(); Asset->CreateSimulationData();
	if (!TestFalse(TEXT("Native Chaos cooking completes without stale simulation data"), Asset->IsSimulationDataDirty())) { return false; }
	const auto* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(
		FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
	if (!TestNotNull(TEXT("Cooked collision is generated for repaired leaf and untouched sibling"), Implicits)) { return false; }
	if (!TestTrue(TEXT("Cooked collision covers the compound root"), Implicits->IsValidIndex(Roots[0]))) { return false; }
	for (int32 Bone : Leaves)
	{
		if (!TestTrue(TEXT("Both source leaves retain cooked collision"),
			Implicits->IsValidIndex(Bone) && (*Implicits)[Bone] && (*Implicits)[Bone]->HasBoundingBox())) { return false; }
	}
	const auto* Root = (*Implicits)[Roots[0]] ? (*Implicits)[Roots[0]]->GetObject<Chaos::FImplicitObjectUnion>() : nullptr;
	if (!TestNotNull(TEXT("Cooked root remains a compound"), Root)) { return false; }
	TestEqual(TEXT("Cooked compound contains both original leaves"), Root->GetObjects().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailRecordedRoofTest,
	"DublinFlight.Destruction.CityDetail.NativeRecordedRoofTriangle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailRecordedRoofTest::RunTest(const FString& Parameters)
{
	// Captured without changing r16 seeds: bridge/osm/way/282571104/0, bone 619, face 25412.
	const TArray<FVector3f> Recorded{
		{607.67742919921875f, -994.135498046875f, 70},
		{607.62200927734375f, -994.29620361328125f, 70},
		{607.67742919921875f, -994.135498046875f, 70}};
	const TArray<FVector> Exact{FVector(Recorded[0]), FVector(Recorded[1]), FVector(Recorded[2])};
	FVector Normal;
	TestTrue(TEXT("Recorded roof corners zero and two are exactly identical"), Recorded[0] == Recorded[2]);
	TestFalse(TEXT("Recorded zero-area face must not be given an invented normal"),
		DublinStructural::FindFaceNormal(MakeArrayView(Exact), Normal));
	FDublinCityBuilding Building;
	Building.Id = TEXT("fixture/recorded-r16-roof-triangle");
	DublinStructural::FAssembly Assembly;
	DublinStructural::FMember Member;
	Member.Polygon = {{600, -1010}, {620, -1010}, {620, -980}, {600, -980}};
	Member.MinZ = 0; Member.MaxZ = 70; Member.bSourcePlaneExterior = true;
	FMeshDescription Mesh; FString Error;
	if (!DublinStructural::MakeMemberMeshDescription(Building, Assembly, Member, Mesh, Error)) { AddError(Error); return false; }
	for (bool bPositive : {false, true})
	{
		FGeometryCollection Geometry;
		FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform::Identity, &Geometry, nullptr, false, false, false);
		const int32 OriginalFaces = Geometry.Indices.Num(), OriginalVertices = Geometry.Vertex.Num();
		const int32 Start = Geometry.AddElements(3, FGeometryCollection::VerticesGroup);
		const int32 Face = Geometry.AddElements(1, FGeometryCollection::FacesGroup);
		auto UVs = GeometryCollection::UV::FindActiveUVLayers(Geometry);
		TArray<FVector> Points;
		for (int32 K = 0; K < 3; ++K)
		{
			FVector3f P = Recorded[K];
			if (bPositive && K == 2) { P.Y += 0.00006103515625f; }
			const int32 V = Start + K;
			Geometry.Vertex[V] = P; Geometry.Normal[V] = FVector3f::UpVector;
			Geometry.TangentU[V] = FVector3f::ForwardVector; Geometry.TangentV[V] = FVector3f::RightVector;
			Geometry.Color[V] = FLinearColor::White; Geometry.BoneMap[V] = 0;
			UVs[0][V] = FVector2f(P.X / 100, P.Y / 100); UVs[1][V] = UVs[0][V];
			Points.Add(FVector(P));
		}
		Geometry.Indices[Face] = FIntVector(Start, Start + 1, Start + 2);
		Geometry.Visible[Face] = true; Geometry.Internal[Face] = false; Geometry.MaterialID[Face] = 0;
		Geometry.VertexCount[0] += 3; Geometry.FaceCount[0] += 1;
		if (bPositive)
		{
			TestTrue(TEXT("Positive skinny variant reproduces default safe-normal epsilon rejection"),
				DublinCity::ClockwiseNormal(Points[0], Points[1], Points[2]).IsNearlyZero());
			TestTrue(TEXT("Exact scale-safe normal preserves the positive skinny variant"),
				DublinStructural::FindFaceNormal(MakeArrayView(Points), Normal));
			TestTrue(TEXT("Positive skinny roof retains the correct clockwise outward sign"), Normal.Equals(FVector::UpVector, 1.e-12));
		}
		int32 Removed = 0;
		if (!DublinStructural::RemoveExactZeroAreaFaces(Geometry, 0, Removed, Error)) { AddError(Error); return false; }
		TestEqual(TEXT("Only the captured exact-zero face is removed"), Removed, bPositive ? 0 : 1);
		TestEqual(TEXT("Positive-area faces are retained regardless of safe-normal epsilon"), Geometry.Indices.Num(), OriginalFaces + (bPositive ? 1 : 0));
		if (!bPositive)
		{
			TestEqual(TEXT("Only vertices orphaned by zero-area removal are removed"), Geometry.Vertex.Num(), OriginalVertices);
			double Volume = 0;
			if (!DublinStructural::ValidateLeafGeometry(Geometry, 0, Volume, Error)) { AddError(Error); return false; }
			TestTrue(TEXT("Zero-area cleanup preserves closed topology and volume"), FMath::IsNearlyEqual(Volume, 42000.0, .001));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStructuralAssemblyTest,
	"DublinFlight.Destruction.StructuralPilot.SourceAssembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStructuralAssemblyTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!Load(*this, Data)) { return false; }
	const int32 ExpectedCorners[] = {4, 5, 6};
	const int32 ExpectedRooms[] = {5, 5, 6};
	for (int32 Index = 0; Index < PilotIds.Num(); ++Index)
	{
		const auto* Building = Find(Data, PilotIds[Index]);
		if (!TestNotNull(*PilotIds[Index], Building)) { return false; }
		const FString Before = DublinDestruction::BuildingGeometryDigest(*Building);
		DublinStructural::FAssembly Assembly;
		FString Error;
		if (!TestTrue(TEXT("Actual source loop produces closed structural member plan"),
			DublinStructural::BuildAssembly(*Building, Assembly, Error))) { AddError(Error); return false; }
		TestEqual(TEXT("Actual boundary corner count; no convex hull substitution"), Assembly.Outer.Num(), ExpectedCorners[Index]);
		TestEqual(TEXT("Approved artistic storey count"), Assembly.RoomsZ.Num(), ExpectedRooms[Index]);
		const double Clear = Assembly.RoomsZ[0].Y - Assembly.RoomsZ[0].X;
		for (const FVector2D& Room : Assembly.RoomsZ)
		{
			TestTrue(TEXT("Every room has uniform clear height"), FMath::IsNearlyEqual(Room.Y - Room.X, Clear, 1.e-6));
			TestTrue(TEXT("No tiny attic"), Clear > 250);
		}
		int32 Seeds = 0;
		double Volume = 0;
		for (const auto& Member : Assembly.Members)
		{
			Seeds += Member.Seeds;
			Volume += DublinStructural::Area(Member.Polygon) * (Member.MaxZ - Member.MinZ);
			TestTrue(TEXT("Every member has an irregular multi-site allocation"), Member.Seeds >= 3);
			if (!Member.bWall) { TestTrue(TEXT("Every slab is exactly 20cm"), FMath::IsNearlyEqual(Member.MaxZ - Member.MinZ, 20.0, 1.e-6)); }
		}
		TestEqual(TEXT("Structural-only aggregate seed target"), Seeds, 288);
		TestTrue(TEXT("Structural solid volume is not envelope volume"), Volume > 0 && Volume < Assembly.SourceVolumeCm3 * .4);
		TestTrue(TEXT("Member volumes equal structural reference"), FMath::IsNearlyEqual(Volume, Assembly.StructuralVolumeCm3, 1.e-4));
		for (int32 I = 0; I < Assembly.Members.Num(); ++I)
		{
			for (int32 J = 0; J < I; ++J)
			{
				const auto& A = Assembly.Members[I];
				const auto& B = Assembly.Members[J];
				const double Height = FMath::Min(A.MaxZ, B.MaxZ) - FMath::Max(A.MinZ, B.MinZ);
				if (Height <= 1.e-6) { continue; }
				TArray<FVector2D> Intersection = A.Polygon;
				for (int32 E = 0; E < B.Polygon.Num(); ++E)
				{
					const FVector2D Edge = (B.Polygon[(E + 1) % B.Polygon.Num()] - B.Polygon[E]).GetSafeNormal();
					const FVector2D N(-Edge.Y, Edge.X);
					Intersection = DublinStructural::Clip(Intersection, N, FVector2D::DotProduct(N, B.Polygon[E]));
				}
				TestTrue(TEXT("Structural members have no positive-volume intersections"),
					FMath::Abs(DublinStructural::Area(Intersection)) * Height < 1.e-3);
			}
		}
		TestEqual(TEXT("Source geometry was not mutated"), DublinDestruction::BuildingGeometryDigest(*Building), Before);
		FDublinCityBuilding Reversed = *Building;
		Swap(Reversed.Mesh.Triangles[0], Reversed.Mesh.Triangles[1]);
		TestFalse(TEXT("Mixed source winding is rejected instead of hidden by abs(volume)"),
			DublinStructural::BuildAssembly(Reversed, Assembly, Error));
	}
	const auto* NearConvex = Find(Data, TEXT("osm/way/227766040"));
	if (TestNotNull(TEXT("Rejected near-convex former candidate"), NearConvex))
	{
		DublinStructural::FAssembly Assembly;
		FString Error;
		TestFalse(TEXT("Shallow concavity is not normalized away"), DublinStructural::BuildAssembly(*NearConvex, Assembly, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStructuralRecipeTest,
	"DublinFlight.Destruction.StructuralPilot.LegacyIdentityAndRequestGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStructuralRecipeTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!Load(*this, Data)) { return false; }
	const TCHAR* Golden[] = {TEXT("ce3bb690d4e7f5506f2e45e83cc1463a"),
		TEXT("0b1ab771644b16829959fb0d89a90038"), TEXT("b1f3c91c627a5d8a25b51212ef4a9ee0")};
	TestEqual(TEXT("Container version remains 3"), UDublinCityFractureLibrary::CurrentBakeVersion, 3);
	TestTrue(TEXT("Omitted recipe defaults to legacy"), FDublinFractureRecord().Recipe == EDublinFractureRecipe::SolidGrid);
	TestEqual(TEXT("Legacy structural volume defaults to zero"), FDublinFractureRecord().StructuralVolumeCm3, 0.0);
	UDublinCityFractureLibrary* SavedLibrary = DublinFractureBake::FindLibrary();
	if (!TestNotNull(TEXT("Existing v3 library loads with additive recipe fields"), SavedLibrary)) { return false; }
	int32 CompatibleLegacySources = 0;
	for (const FDublinCityBuilding& Building : Data.Buildings)
	{
		const FDublinFractureRecord* Saved = SavedLibrary->Find(Building.Id);
		if (!TestNotNull(*Building.Id, Saved)) { return false; }
		TestTrue(TEXT("Saved records retain their declared recipe identity"),
			DublinFractureBake::IsCurrentRecord(Building, *Saved));
		FDublinFractureRecord Legacy;
		Legacy.SourceId = Building.Id;
		Legacy.SourceDigest = DublinDestruction::BuildingDigest(Building);
		if (TestTrue(TEXT("Omitted recipe remains compatible for every source after detailed rollout"),
			DublinFractureBake::IsCurrentRecord(Building, Legacy))) { ++CompatibleLegacySources; }
		if (Saved->Recipe == EDublinFractureRecipe::SolidGrid)
		{
			TestEqual(TEXT("Every legacy saved digest still matches exact source"), DublinDestruction::BuildingDigest(Building), Saved->SourceDigest);
			TestEqual(TEXT("Old serialized records have no invented structural volume"), Saved->StructuralVolumeCm3, 0.0);
		}
	}
	TestEqual(TEXT("Every source retains legacy recipe compatibility"), CompatibleLegacySources, Data.Buildings.Num());
	for (int32 I = 0; I < PilotIds.Num(); ++I)
	{
		const auto* Building = Find(Data, PilotIds[I]);
		if (!TestNotNull(*PilotIds[I], Building)) { return false; }
		TestEqual(TEXT("Legacy digest is byte-for-byte unchanged"), DublinDestruction::BuildingDigest(*Building), FString(Golden[I]));
		FDublinFractureRecord Record;
		Record.SourceId = Building->Id;
		Record.SourceDigest = DublinDestruction::BuildingDigest(*Building);
		TestTrue(TEXT("Legacy record remains current"), DublinFractureBake::IsCurrentRecord(*Building, Record));
		Record.Recipe = EDublinFractureRecipe::StructuralPilot;
		TestFalse(TEXT("Changing recipe invalidates only this record"), DublinFractureBake::IsCurrentRecord(*Building, Record));
		Record.SourceDigest = DublinDestruction::BuildingDigest(*Building, Record.Recipe);
		TestTrue(TEXT("Structural digest uses record recipe"), DublinFractureBake::IsCurrentRecord(*Building, Record));
		TestTrue(TEXT("Structural and legacy cache names differ"), Record.SourceDigest != Golden[I]);
		Record.Recipe = static_cast<EDublinFractureRecipe>(255);
		TestFalse(TEXT("Unknown recipe fails closed without digest fallback"), DublinFractureBake::IsCurrentRecord(*Building, Record));
	}
	FString Error;
	const auto Structural = EDublinFractureRecipe::StructuralPilot;
	TestTrue(TEXT("Approved three-ID request accepted"), DublinFractureBake::ValidateBakeRequest(Structural, PilotIds, false, Error));
	TestFalse(TEXT("Structural BakeAll forbidden"), DublinFractureBake::ValidateBakeRequest(Structural, PilotIds, true, Error));
	TestFalse(TEXT("Empty structural request forbidden"), DublinFractureBake::ValidateBakeRequest(Structural, {}, false, Error));
	TestFalse(TEXT("Fourth ID forbidden"), DublinFractureBake::ValidateBakeRequest(Structural, {PilotIds[0], PilotIds[1], PilotIds[2], TEXT("other")}, false, Error));
	TestFalse(TEXT("Unapproved source forbidden"), DublinFractureBake::ValidateBakeRequest(Structural, {TEXT("osm/way/227766040")}, false, Error));
	TestFalse(TEXT("Duplicate IDs forbidden"), DublinFractureBake::ValidateBakeRequest(Structural, {PilotIds[0], PilotIds[0]}, false, Error));
	TestTrue(TEXT("Legacy bake-all behavior unchanged"), DublinFractureBake::ValidateBakeRequest(EDublinFractureRecipe::SolidGrid, {}, true, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStructuralPublishTest,
	"DublinFlight.Destruction.StructuralPilot.FailedPublicationRetainsLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStructuralPublishTest::RunTest(const FString& Parameters)
{
	UDublinCityFractureLibrary* Saved = DublinFractureBake::FindLibrary();
	if (!TestNotNull(TEXT("Existing saved library for read-only fixture"), Saved) || Saved->Records.IsEmpty()) { return false; }
	TStrongObjectPtr<UDublinCityFractureLibrary> Library(NewObject<UDublinCityFractureLibrary>());
	Library->Records = Saved->Records;
	const FDublinFractureRecord Original = Library->Records[0];
	FDublinFractureRecord Candidate = Original;
	Candidate.Recipe = EDublinFractureRecipe::StructuralPilot;
	Candidate.SourceDigest += TEXT("-test-candidate");
	FString Error;
	int32 Saves = 0;
	const auto RejectSave = [&](UDublinCityFractureLibrary&, FString& E)
	{
		++Saves;
		E = TEXT("Injected library-save failure");
		return false;
	};
	TestFalse(TEXT("Library-save failure is returned"), DublinFractureBake::PublishRecord(*Library.Get(), Candidate, RejectSave, Error));
	TestEqual(TEXT("Save was attempted exactly once"), Saves, 1);
	TestEqual(TEXT("All unrelated rows retained"), Library->Records.Num(), Saved->Records.Num());
	TestEqual(TEXT("Prior digest restored"), Library->Records[0].SourceDigest, Original.SourceDigest);
	TestTrue(TEXT("Prior recipe restored"), Library->Records[0].Recipe == Original.Recipe);
	TestEqual(TEXT("Prior package retained"), Library->Records[0].Collection.ToSoftObjectPath(), Original.Collection.ToSoftObjectPath());
	TestTrue(TEXT("Late-failure candidate is explicitly reported"), Error.Contains(TEXT("Unpublished candidate")));
	Candidate.bReady = false;
	TestFalse(TEXT("Failed bake is never published"), DublinFractureBake::PublishRecord(*Library.Get(), Candidate, RejectSave, Error));
	TestEqual(TEXT("Failed bake never calls library save"), Saves, 1);
	TestEqual(TEXT("On-disk loaded library remains untouched"), Saved->Records[0].SourceDigest, Original.SourceDigest);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStructuralNativeTest,
	"DublinFlight.Destruction.StructuralPilot.NativeCollectionsAndCookedVoids",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStructuralNativeTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!Load(*this, Data)) { return false; }
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Structural physics fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Structural physics fixture owner"), Owner)) { return false; }
	for (const FString& Id : PilotIds)
	{
		const auto* Building = Find(Data, Id);
		if (!TestNotNull(*Id, Building)) { return false; }
		TStrongObjectPtr<UGeometryCollection> Asset(NewObject<UGeometryCollection>());
		if (!SetMaterials(*Asset.Get(), *this)) { return false; }
		FDublinFractureRecord Record;
		FString Error;
		if (!TestTrue(TEXT("Native structural construction succeeds without saving any package"),
			DublinStructural::BuildCollection(*Building, *Asset.Get(), Record, Error))) { AddError(Id + TEXT(": ") + Error); return false; }
		TestEqual(TEXT("Exactly 288 real physical leaves"), Record.PieceCount, 288);
		TestTrue(TEXT("Nanite is mandatory, not fallback"), Record.bNaniteReady && Asset->HasNaniteData());
		Asset->bOptimizeConvexes = true;
		TestFalse(TEXT("Runtime single-convex optimization cannot silently fill rooms"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), Record, Error));
		Asset->bOptimizeConvexes = false;
		TestTrue(TEXT("Derived solid, not original envelope, is conserved"),
			FMath::Abs(Record.RetainedVolumeCm3 - Record.StructuralVolumeCm3) < Record.StructuralVolumeCm3 * 1.e-4 &&
			Record.RetainedVolumeCm3 < Record.SourceVolumeCm3 * .4);
		TestTrue(TEXT("Anchors are a strict subset of collidable leaves"), Record.Anchors.Num() > 0 && Record.Anchors.Num() < Record.PieceCount);
		const auto Geometry = Asset->GetGeometryCollection();
		auto& Implicits = Geometry->ModifyAttribute<Chaos::FImplicitObjectPtr>(FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
		const auto Root = Implicits[Record.RootTransform];
		FGeometryCollection Envelope;
		const int32 EnvelopeGeometry = Envelope.AddElements(1, FGeometryCollection::GeometryGroup);
		const int32 VertexCount = Building->Mesh.VerticesCm.Num();
		const int32 VertexStart = Envelope.AddElements(VertexCount, FGeometryCollection::VerticesGroup);
		Envelope.VertexStart[EnvelopeGeometry] = VertexStart;
		Envelope.VertexCount[EnvelopeGeometry] = VertexCount;
		const auto& MassToLocal = Geometry->GetAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
		const FTransform RootMassToCollection = MassToLocal[Record.RootTransform] * FTransform(Geometry->Transform[Record.RootTransform]);
		for (int32 V = 0; V < VertexCount; ++V)
		{
			Envelope.Vertex[VertexStart + V] = FVector3f(RootMassToCollection.InverseTransformPosition(Building->Mesh.VerticesCm[V]));
		}
		// Construct inside Chaos.dll, avoiding inline convex/box vtables in this test module.
		const Chaos::FConvexPtr EnvelopeHull = FGeometryCollectionConvexUtility::GetConvexHull(&Envelope, EnvelopeGeometry);
		if (!TestNotNull(TEXT("Exported native solid-envelope fixture"), EnvelopeHull.GetReference())) { return false; }
		Implicits[Record.RootTransform] = Chaos::FImplicitObjectPtr(EnvelopeHull.GetReference());
		TestFalse(TEXT("A valid-looking single convex root is explicitly rejected"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), Record, Error));
		TestFalse(TEXT("Solid-envelope collision actually fills and fails the room probes"),
			DublinStructural::ValidateBuildingCollision(*Asset.Get(), *Building, Record, Error));
		Implicits[Record.RootTransform] = Root;
		auto* Union = Root->GetObject<Chaos::FImplicitObjectUnion>();
		if (!TestNotNull(TEXT("Real cooked root union"), Union)) { return false; }
		auto& Parts = Union->GetObjects();
		const auto Last = Parts.Last();
		Parts.Last() = Parts[0];
		TestFalse(TEXT("Duplicate/overlapping compound constituent cannot pass validation"),
			DublinStructural::ValidateCookedCollision(*Asset.Get(), Record, Error));
		Parts.Last() = Last;
		TArray<uint8> Bytes;
		FCustomVersionContainer CustomVersions;
		{
			FMemoryWriter Writer(Bytes, true);
			Chaos::FChaosArchive Archive(Writer);
			Geometry->Serialize(Archive);
			CustomVersions = Writer.GetCustomVersions();
		}
		auto ReloadedGeometry = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
		{
			FMemoryReader Reader(Bytes, true);
			Reader.SetCustomVersions(CustomVersions);
			Chaos::FChaosArchive Archive(Reader);
			ReloadedGeometry->Serialize(Archive);
		}
		TStrongObjectPtr<UGeometryCollection> Reloaded(NewObject<UGeometryCollection>());
		Reloaded->bOptimizeConvexes = false;
		Reloaded->bImportCollisionFromSource = false;
		Reloaded->SetGeometryCollection(ReloadedGeometry);
		Reloaded->InvalidateCollection();
		if (!TestTrue(TEXT("Cooked collision survives native archive round trip"),
			DublinStructural::ValidateCookedCollision(*Reloaded.Get(), Record, Error)) ||
			!TestTrue(TEXT("Root and released-leaf shapes preserve empty rooms after serialization"),
				DublinStructural::ValidateBuildingCollision(*Reloaded.Get(), *Building, Record, Error)))
		{
			AddError(Error);
			return false;
		}
		UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(Owner);
		Owner->AddInstanceComponent(Component);
		Component->SetWorldLocation(Building->PivotCm);
		Component->SetRestCollection(Asset.Get());
		Component->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetCollisionObjectType(ECC_WorldDynamic);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		if (!TestTrue(TEXT("Every structural leaf has initialized native physics"),
			DublinFractureBake::RegisterRuntimePhysics(*Component, Record, Error)) ||
			!TestTrue(TEXT("Registered root void/floor/wall collision works at source pivot"),
				DublinStructural::ValidateRegisteredCollision(*Component, *Building, Record, Error)))
		{
			AddError(Error);
			return false;
		}
		Owner->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStructuralLiveBVHPosesTest,
	"DublinFlight.Destruction.StructuralPilot.NativeLiveBVHQueriesAndPoses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStructuralLiveBVHPosesTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	if (!Load(*this, Data)) { return false; }
	TStrongObjectPtr<UDublinCityFractureLibrary> Library(DublinFractureBake::FindLibrary());
	if (!TestNotNull(TEXT("Saved certified library"), Library.Get())) { return false; }
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Native BVH pose world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Native BVH pose owner"), Owner)) { return false; }
	const TCHAR* Ids[] = {TEXT("osm/way/233804861"), TEXT("osm/way/227766040"), TEXT("osm/way/1482695121"),
		TEXT("osm/way/1488401199"), TEXT("osm/relation/289528"), TEXT("osm/way/1488418443"),
		TEXT("osm/way/14047783/volume/1"), TEXT("bridge/osm/way/282571104/0"),
		TEXT("bridge/osm/way/399007550/0"), TEXT("landmark/spire"), TEXT("osm/relation/3375620"),
		TEXT("osm/way/233797910"), TEXT("osm/way/657456422"), TEXT("osm/way/233804853"),
		TEXT("osm/way/51245099"), TEXT("osm/way/83174299")};
	uint64 Possible = 0, Narrow = 0;
	int32 Passed = 0;
	for (const TCHAR* Id : Ids)
	{
		const auto* Building = Find(Data, Id);
		const auto* Record = Library->Find(Id);
		if (!TestNotNull(Id, Building) || !TestNotNull(TEXT("Saved probe contract"), Record)) { return false; }
		TStrongObjectPtr<UGeometryCollection> Asset(Record->Collection.LoadSynchronous());
		if (!TestNotNull(TEXT("Actual saved collision"), Asset.Get())) { return false; }
		for (const FRotator Rotation : {FRotator(17, -31, 9), FRotator(-23, 117, -11)})
		{
			UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(Owner);
			Owner->AddInstanceComponent(Component);
			Component->SetWorldTransform(FTransform(Rotation, Building->PivotCm + FVector(321.25, -177.5, 275.75)));
			Component->SetCanEverAffectNavigation(false);
			FString Error;
			DublinStructural::FProbeEquivalenceStats Stats;
			const bool PassedPose = DublinFractureBake::PrepareAndRegisterRuntimePhysics(*Component, *Asset.Get(), *Record, Error) &&
				DublinStructural::ValidateRegisteredCollision(*Component, *Building, *Record, Error) &&
				DublinStructural::ValidateRegisteredProbeEquivalence(*Component, *Record, Error, &Stats) &&
				DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), *Record, Error);
			if (PassedPose)
			{
				FDublinFractureRecord Wrong = *Record;
				Wrong.CollisionProbes[0].bExpectHit = !Wrong.CollisionProbes[0].bExpectHit;
				TestFalse(TEXT("Acceleration cannot waive an authored expected-hit failure"),
					DublinStructural::ValidateRegisteredCollision(*Component, *Building, Wrong, Error));
			}
			Owner->RemoveInstanceComponent(Component);
			Component->DestroyComponent();
			if (!PassedPose) { AddError(FString(Id) + TEXT(": ") + Error); return false; }
			Possible += Stats.PossibleCandidates; Narrow += Stats.NarrowphaseCandidates;
			++Passed;
		}
	}
	TestEqual(TEXT("All sixteen saved sources checked at both nonidentity poses"), Passed, 32);
	TestTrue(TEXT("Valid native BVHs actually reduce tested candidates"), Possible > 0 && Narrow < Possible);
	AddInfo(FString::Printf(TEXT("Native BVH equivalence poses=%d narrowCandidates=%llu possibleCandidates=%llu; no FPS claim"),
		Passed, Narrow, Possible));
	return !HasAnyErrors();
}
#endif
