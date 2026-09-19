#include "City/DublinCityDetail.h"

#if WITH_EDITOR
#include "City/DublinCityDestruction.h"
#include "Algo/Reverse.h"
#include "Chaos/Convex.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "CompGeom/ConvexHull2.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollectionProxyData.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "MeshDescription.h"
#include "Misc/Crc.h"
#include "Misc/SecureHash.h"
#include "PlanarCut.h"
#include "Voronoi/Voronoi.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	using namespace UE::Geometry;

	bool Fail(FString& Error, const FString& Text) { Error = TEXT("CityDetail: ") + Text; return false; }

	bool AuthorExactCollision(FGeometryCollection& Geometry, FDublinFractureRecord& Record, FString& Error)
	{
		const auto* Implicits = Geometry.FindAttribute<Chaos::FImplicitObjectPtr>(
			FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
		const auto* MassToLocal = Geometry.FindAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
		if (!Implicits || !MassToLocal) { return Fail(Error, TEXT("exact collision requires cooked native leaf hulls")); }
		auto& External = Geometry.AddAttribute<Chaos::FImplicitObjectPtr>(
			FGeometryCollection::ExternalCollisionsAttribute, FGeometryCollection::TransformGroup);
		const FTransform Root = GeometryCollectionAlgo::GlobalMatrix(Geometry.Transform, Geometry.Parent, Record.RootTransform);
		TArray<Chaos::FImplicitObjectPtr> Children;
		for (int32 Bone : Record.LeafTransforms)
		{
			if (!Implicits->IsValidIndex(Bone) || !MassToLocal->IsValidIndex(Bone) || !(*Implicits)[Bone])
			{
				return Fail(Error, TEXT("exact collision requires one convex per leaf"));
			}
			const Chaos::FConvex* Hull = (*Implicits)[Bone]->GetObject<Chaos::FConvex>();
			if (!Hull || Hull->NumVertices() < 4 || Hull->NumEdges() < 6 ||
				!FMath::IsFinite(Hull->GetVolume()) || Hull->GetVolume() <= 0)
			{
				return Fail(Error, FString::Printf(TEXT("invalid authored exact hull source=%s bone=%d"), *Record.SourceId, Bone));
			}
			const Chaos::FImplicitObjectPtr Copy(Hull->RawCopyAsConvex());
			const FTransform LeafMassToLocal = (*MassToLocal)[Bone];
			const FTransform ToRoot = (LeafMassToLocal * GeometryCollectionAlgo::GlobalMatrix(
				Geometry.Transform, Geometry.Parent, Bone)).GetRelativeTransform(Root);
			if (ToRoot.ContainsNaN() || !ToRoot.GetScale3D().Equals(FVector::OneVector, 1.e-8))
			{
				return Fail(Error, TEXT("exact collision requires finite rigid leaf transforms"));
			}
			External[Bone] = MakeImplicitObjectPtr<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>(
				Copy, Chaos::FRigidTransform3(LeafMassToLocal));
			Children.Add(MakeImplicitObjectPtr<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>(
				Copy, Chaos::FRigidTransform3(ToRoot)));
		}
		// Native convex recooking re-hulls transformed vertices with a 1cm face-merge
		// tolerance. Authored implicits preserve the same convex topology at both levels.
		External[Record.RootTransform] = MakeImplicitObjectPtr<Chaos::FImplicitObjectUnion>(MoveTemp(Children));
		Record.CollisionStorageVersion = 1;
		return true;
	}

	TArray<TArray<FVector>> TetraFaces(const TArray<FVector>& V)
	{
		return {{V[0], V[1], V[2]}, {V[0], V[3], V[1]}, {V[0], V[2], V[3]}, {V[1], V[3], V[2]}};
	}

	TArray<FVector> MemberPoints(const DublinStructural::FMember& M)
	{
		TArray<FVector> Points;
		for (const FVector2D& P : M.Polygon)
		{
			Points.Emplace(P.X, P.Y, M.MinZ + FVector2D::DotProduct(M.BottomSlope, P));
			Points.Emplace(P.X, P.Y, M.MaxZ + FVector2D::DotProduct(M.TopSlope, P));
		}
		return Points;
	}

	TArray<TArray<FVector>> MemberFaces(const DublinStructural::FMember& M)
	{
		TArray<FVector> Bottom, Top;
		for (const FVector2D& P : M.Polygon)
		{
			Bottom.Emplace(P.X, P.Y, M.MinZ + FVector2D::DotProduct(M.BottomSlope, P));
			Top.Emplace(P.X, P.Y, M.MaxZ + FVector2D::DotProduct(M.TopSlope, P));
		}
		TArray<TArray<FVector>> Faces{Bottom, Top};
		for (int32 I = 0; I < Bottom.Num(); ++I)
		{
			const int32 J = (I + 1) % Bottom.Num();
			Faces.Add({Bottom[I], Bottom[J], Top[J], Top[I]});
		}
		return Faces;
	}

	bool Cells2D(const TArray<FVector>& Points, const FVector& U, const FVector& V, int32 SiteCount,
		uint32 Seed, FPlanarCells& Cells, FString& Error)
	{
		const FVector N = FVector::CrossProduct(U, V).GetSafeNormal();
		TArray<FVector2D> Projected;
		double MinN = TNumericLimits<double>::Max(), MaxN = -MinN;
		for (const FVector& P : Points)
		{
			Projected.Emplace(FVector::DotProduct(U, P), FVector::DotProduct(V, P));
			MinN = FMath::Min(MinN, FVector::DotProduct(N, P)); MaxN = FMath::Max(MaxN, FVector::DotProduct(N, P));
		}
		FConvexHull2d Hull;
		if (!Hull.Solve(Projected.Num(), [&](int32 I) { return Projected[I]; })) { return Fail(Error, TEXT("member has no broad-face projection")); }
		TArray<FVector2D> Boundary;
		for (int32 I : Hull.GetPolygonIndices()) { Boundary.Add(Projected[I]); }
		if (DublinStructural::Area(Boundary) < 0) { Algo::Reverse(Boundary); }
		FAxisAlignedBox2d Bounds;
		for (const auto& P : Boundary) { Bounds.Contain(P); }
		const auto Inside = [&](const FVector2D& P)
		{
			for (int32 I = 0; I < Boundary.Num(); ++I)
			{
				const FVector2D A = Boundary[I], E = Boundary[(I + 1) % Boundary.Num()] - A, D = P - A;
				if (E.X * D.Y - E.Y * D.X < 0) { return false; }
			}
			return true;
		};
		FRandomStream Random(static_cast<int32>(Seed));
		TArray<FVector2d> Sites;
		for (int32 I = 0; I < SiteCount; ++I)
		{
			FVector2D Best = FVector2D::ZeroVector;
			double BestDistance = -1;
			for (int32 C = 0; C < 24; ++C)
			{
				FVector2D P = FVector2D::ZeroVector;
				bool bInside = false;
				for (int32 Trial = 0; Trial < 4096 && !bInside; ++Trial)
				{
					P = FVector2D(Random.FRandRange(Bounds.Min.X, Bounds.Max.X), Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y));
					bInside = Inside(P);
				}
				if (!bInside) { return Fail(Error, TEXT("bounded broad-face seed sampling failed")); }
				double Nearest = TNumericLimits<double>::Max();
				for (const FVector2D& S : Sites) { Nearest = FMath::Min(Nearest, FVector2D::DistSquared(P, S)); }
				if (Nearest > BestDistance) { Best = P; BestDistance = Nearest; }
			}
			if (BestDistance < 1.e-8) { return Fail(Error, TEXT("duplicate fracture seeds")); }
			Sites.Add(Best);
		}
		Bounds.Expand(10);
		TArray<FVector> PlanarSites;
		for (const FVector2D& Site : Sites) { PlanarSites.Emplace(Site.X, Site.Y, (MinN + MaxN) * .5); }
		const FBox LocalBounds(FVector(Bounds.Min.X, Bounds.Min.Y, MinN - 10), FVector(Bounds.Max.X, Bounds.Max.Y, MaxN + 10));
		// Equal local Z makes every internal bisector a through-thickness prism plane. Native Voronoi
		// owns shared facets and endcaps, rather than independently clipped 2D cell boundaries.
		FVoronoiDiagram Voronoi(MakeArrayView(PlanarSites), LocalBounds, 0, 0);
		Cells = FPlanarCells(MakeArrayView(PlanarSites), Voronoi);
		if (Cells.NumCells != SiteCount) { return Fail(Error, TEXT("native planar-site Voronoi omitted cells")); }
		for (FVector& P : Cells.PlaneBoundaryVertices) { P = U * P.X + V * P.Y + N * P.Z; }
		for (FPlane& Plane : Cells.Planes)
		{
			const FVector Normal = U * Plane.X + V * Plane.Y + N * Plane.Z;
			Plane = FPlane(Normal, Plane.W);
		}
		Cells.InternalSurfaceMaterials.GlobalMaterialID = 3;
		Cells.InternalSurfaceMaterials.GlobalUVScale = .01f;
		if (!Cells.HasValidPlaneBoundaryOrientations()) { return Fail(Error, TEXT("invalid native 2D prism cutting planes")); }
		return true;
	}

	FString DescribeMember(const DublinStructural::FMember& M)
	{
		FString Result = FString::Printf(TEXT("polygonVertices=%d minZ=%.17g maxZ=%.17g bottomSlope=(%.17g,%.17g) topSlope=(%.17g,%.17g) wall=%d"),
			M.Polygon.Num(), M.MinZ, M.MaxZ, M.BottomSlope.X, M.BottomSlope.Y, M.TopSlope.X, M.TopSlope.Y, M.bWall);
		for (const FVector2D& P : M.Polygon)
		{
			Result += FString::Printf(TEXT(" XY=(%.17g,%.17g) Z=(%.17g,%.17g)"), P.X, P.Y,
				M.MinZ + FVector2D::DotProduct(M.BottomSlope, P), M.MaxZ + FVector2D::DotProduct(M.TopSlope, P));
		}
		return Result;
	}

	bool ImportMembers(const FDublinCityBuilding& Building, const DublinDetail::FPlan& Plan,
		FGeometryCollection& Geometry, TArray<int32>& Bones, FString& Error)
	{
		double Volume = 0;
		for (int32 I = 0; I < Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num(); ++I)
		{
			const auto Faces = I < Plan.Assembly.Members.Num() ? MemberFaces(Plan.Assembly.Members[I]) :
				TetraFaces(Plan.Tetrahedra[I - Plan.Assembly.Members.Num()]);
			FPlanarCells Cells; Cells.NumCells = 1;
			TArray<int32> Imported;
			double MemberVolume = 0;
			if (!DublinStructural::AppendDoubleClippedCells(Building, Faces, Cells, Geometry, Imported, MemberVolume, Error))
			{
				Error = FString::Printf(TEXT("source=%s doubleMember=%d: %s"), *Building.Id, I, *Error); return false;
			}
			Volume += MemberVolume; Bones.Append(Imported);
		}
		if (FMath::Abs(Volume - Plan.Assembly.StructuralVolumeCm3) > FMath::Max(1.0, Plan.Assembly.StructuralVolumeCm3 * 1.e-4))
		{
			return Fail(Error, TEXT("imported members changed planned structural solid volume"));
		}
		Geometry.ReindexMaterials();
		for (int32 F = 0; F < Geometry.MaterialID.Num(); ++F) { Geometry.Internal[F] = Geometry.MaterialID[F] == 3; }
		return true;
	}
}

bool DublinDetail::PreflightGeometry(const FDublinCityBuilding& Building, FPlan& Plan, FString& Error)
{
	if (!AnalyzeBuilding(Building, Plan, Error)) { return false; }
	FGeometryCollection Geometry;
	TArray<int32> Bones;
	if (!ImportMembers(Building, Plan, Geometry, Bones, Error)) { return false; }
	FDublinFractureRecord Record;
	Record.Recipe = Plan.Recipe;
	Record.LeafTransforms = Bones;
	Record.PieceCount = Record.MemberCount = Bones.Num();
	return DublinStructural::ValidateExteriorGeometry(Building, Geometry, Record, Error);
}

bool DublinDetail::BuildCollection(const FDublinCityBuilding& Building, UGeometryCollection& Candidate,
	FDublinFractureRecord& Record, FString& Error, int32 Attempt)
{
	if (Attempt < 0 || Attempt > 2 || Candidate.Materials.Num() != 4 || !Candidate.Materials[3] ||
		Candidate.Materials[1] == Candidate.Materials[3]) { return Fail(Error, TEXT("invalid attempt or distinct exterior/interior material slots")); }
	FPlan Plan;
	if (!AnalyzeBuilding(Building, Plan, Error)) { return false; }
	Record = FDublinFractureRecord();
	Record.Recipe = Plan.Recipe; Record.SourceId = Building.Id; Record.SourceDigest = Plan.SourceDigest;
	Record.GeometryDigest = DublinDestruction::BuildingGeometryDigest(Building);
	Record.SourceVolumeCm3 = Plan.Assembly.SourceVolumeCm3; Record.StructuralVolumeCm3 = Plan.Assembly.StructuralVolumeCm3;
	Record.DetailTier = Plan.Tier; Record.BroadSurfaceAreaM2 = Plan.BroadAreaM2; Record.LargestMemberAreaM2 = Plan.LargestMemberAreaM2;
	Record.bDenseForOversize = Plan.bDenseForOversize; Record.PlanReason = Plan.Reason; Record.ProbeVersion = 1;
	Record.CollisionProbes = Plan.Probes; Record.VoidSamplesCm = Plan.VoidSamplesCm; Record.BakeAttempts = Attempt + 1;
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
	Candidate.SetGeometryCollection(Geometry);
	Record.MemberCount = Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num();
	double DoubleReference = 0;
	for (int32 I = 0; I < Record.MemberCount; ++I)
	{
		const bool bTet = I >= Plan.Assembly.Members.Num();
		const int32 Seeds = bTet ? 2 : Plan.Assembly.Members[I].Seeds;
		Record.FractureSiteCount += Seeds;
		TArray<FVector> Points;
		FVector U, V;
		if (bTet)
		{
			Points = Plan.Tetrahedra[I - Plan.Assembly.Members.Num()];
			U = FVector::ForwardVector; V = FVector::UpVector;
		}
		else
		{
			const auto& M = Plan.Assembly.Members[I]; Points = MemberPoints(M);
			U = M.bWall ? FVector(M.WallTangent.X, M.WallTangent.Y, 0) : FVector::ForwardVector;
			V = M.bWall ? FVector::UpVector : FVector::RightVector;
		}
		const uint32 Seed = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%d:%d"), *Plan.SourceDigest, I, Attempt));
		FPlanarCells Cells;
		if (Seeds == 1) { Cells.NumCells = 1; }
		else if (!Cells2D(Points, U, V, Seeds, Seed, Cells, Error)) { return false; }
		const auto Faces = bTet ? TetraFaces(Points) : MemberFaces(Plan.Assembly.Members[I]);
		TArray<int32> Pieces;
		double SourceVolume = 0;
		if (!DublinStructural::AppendDoubleClippedCells(Building, Faces, Cells, *Geometry, Pieces, SourceVolume, Error))
		{
			const FString Member = bTet ? TEXT("tetrahedron") : DescribeMember(Plan.Assembly.Members[I]);
			Error = FString::Printf(TEXT("source=%s doubleMember=%d seeds=%d seed=%u %s: %s"),
				*Building.Id, I, Seeds, Seed, *Member, *Error); return false;
		}
		if (Pieces.Num() != Seeds) { return Fail(Error, TEXT("double clipping did not retain every requested cell")); }
		DoubleReference += SourceVolume;
	}
	if (FMath::Abs(DoubleReference - Record.StructuralVolumeCm3) > FMath::Max(1.0, Record.StructuralVolumeCm3 * 1.e-4))
	{
		return Fail(Error, TEXT("attributed double members changed the source structural volume ledger"));
	}
	TArray<int32> Leaves;
	TArray<FTransform> Before;
	for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
	{
		if (!Geometry->IsRigid(Bone) || !Geometry->Children[Bone].IsEmpty()) { continue; }
		double Volume = 0;
		if (!DublinStructural::ValidateLeafGeometry(*Geometry, Bone, Volume, Error)) { return false; }
		Record.RetainedVolumeCm3 += Volume; Leaves.Add(Bone);
		Before.Add(GeometryCollectionAlgo::GlobalMatrix(Geometry->Transform, Geometry->Parent, Bone));
	}
	if (Leaves.Num() < 2 || Leaves.Num() > Plan.Tier ||
		FMath::Abs(Record.RetainedVolumeCm3 - Record.StructuralVolumeCm3) > FMath::Max(1.0, Record.StructuralVolumeCm3 * 1.e-4))
	{
		return Fail(Error, FString::Printf(TEXT("cut exceeds leaf tier or loses volume: leaves=%d tier=%d planned=%.9g retained=%.9g"),
			Leaves.Num(), Plan.Tier, Record.StructuralVolumeCm3, Record.RetainedVolumeCm3));
	}
	FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(Geometry.Get());
	TArray<int32> Roots; FGeometryCollectionClusteringUtility::GetRootBones(Geometry.Get(), Roots);
	if (Roots.Num() != 1) { return Fail(Error, TEXT("cannot create detail root")); }
	FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Geometry.Get(), Roots[0]);
	FGeometryCollectionClusteringUtility::ClusterBonesUnderExistingRoot(Geometry.Get(), Leaves);
	Roots.Reset(); FGeometryCollectionClusteringUtility::GetRootBones(Geometry.Get(), Roots);
	if (Roots.Num() != 1) { return Fail(Error, TEXT("flattening lost unique detail root")); }
	Record.RootTransform = Roots[0];
	if (!DublinStructural::NormalizeGeometryOrder(*Geometry, Error)) { return false; }
	if (!Geometry->HasAttribute(TEXT("InitialDynamicState"), FGeometryCollection::TransformGroup))
	{
		Geometry->AddAttribute<int32>(TEXT("InitialDynamicState"), FGeometryCollection::TransformGroup);
	}
	Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Geometry); Anchoring.AddAnchoredAttribute();
	int32 LeafIndex = 0;
	for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
	{
		Anchoring.SetInitialDynamicState(Bone, Chaos::EObjectStateType::Dynamic);
		if (!Geometry->IsRigid(Bone) || !Geometry->Children[Bone].IsEmpty()) { continue; }
		const FTransform Global = GeometryCollectionAlgo::GlobalMatrix(Geometry->Transform, Geometry->Parent, Bone);
		if (!Before.IsValidIndex(LeafIndex) || !Before[LeafIndex++].Equals(Global, .001)) { return Fail(Error, TEXT("flatten changed detail transform")); }
		Record.LeafTransforms.Add(Bone);
		const int32 G = Geometry->TransformToGeometryIndex[Bone];
		double MinZ = TNumericLimits<double>::Max();
		for (int32 V = Geometry->VertexStart[G]; V < Geometry->VertexStart[G] + Geometry->VertexCount[G]; ++V)
		{
			MinZ = FMath::Min(MinZ, Global.TransformPosition(FVector(Geometry->Vertex[V])).Z);
		}
		if (MinZ <= Plan.Assembly.MinZ + .01)
		{
			Record.Anchors.Add(Bone); Anchoring.SetAnchored(Bone, true);
			Anchoring.SetInitialDynamicState(Bone, Chaos::EObjectStateType::Kinematic);
		}
	}
	Record.PieceCount = Record.LeafTransforms.Num(); Record.HullCount = Record.PieceCount;
	if (!DublinFractureBake::HasValidPieceBudget(Record) || Record.Anchors.IsEmpty() ||
		Record.Anchors.Num() == Record.PieceCount) { return Fail(Error, TEXT("invalid physical detail cost or foundation anchors")); }
	if (!DublinStructural::ValidateExteriorGeometry(Building, *Geometry, Record, Error)) { return false; }
	Candidate.EnableClustering = true;
	Candidate.DamageModel = EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
	Candidate.DamageThreshold = {100, 75, 50};
	Candidate.bUseSizeSpecificDamageThreshold = false;
	Candidate.bRemoveOnMaxSleep = false; Candidate.bAutomaticCrumblePartialClusters = false;
	Candidate.bImportCollisionFromSource = false; Candidate.bOptimizeConvexes = false;
	Candidate.bStripOnCook = false; Candidate.bStripRenderDataOnCook = false;
	Candidate.DamagePropagationData.bEnabled = false;
	Candidate.DamagePropagationData.BreakDamagePropagationFactor = 0; Candidate.DamagePropagationData.ShockDamagePropagationFactor = 0;
	if (Candidate.SizeSpecificData.IsEmpty()) { Candidate.SizeSpecificData.Add(UGeometryCollection::GeometryCollectionSizeSpecificDataDefaults()); }
	for (auto& Size : Candidate.SizeSpecificData)
	{
		Size.CollisionShapes.SetNum(1);
		Size.CollisionShapes[0].CollisionType = ECollisionTypeEnum::Chaos_Volumetric;
		Size.CollisionShapes[0].ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Convex;
		Size.CollisionShapes[0].CollisionMarginFraction = 0;
		Size.CollisionShapes[0].CollisionObjectReductionPercentage = 0;
	}
	auto Properties = Geometry->GetConvexProperties(); Properties.SimplificationThreshold = 0; Properties.OverlapRemovalShrinkPercent = 0;
	Geometry->SetConvexProperties(Properties);
	Candidate.InvalidateCollection(); Candidate.UpdateGeometryDependentProperties();
	FGeometryCollectionConvexUtility::FLeafConvexHullSettings Settings(0, EGenerateConvexMethod::ComputedFromGeometry);
	FGeometryCollectionConvexUtility::GenerateLeafConvexHulls(*Geometry, true, MakeArrayView(Record.LeafTransforms), Settings);
	FGeometryCollectionConvexUtility::CopyChildConvexes(Geometry.Get(), MakeArrayView(Roots), Geometry.Get(), MakeArrayView(Roots), true);
	if (!DublinFractureBake::BuildConnectionGraph(Candidate, Error)) { return false; }
	Candidate.bEnableNaniteFallback = true; Candidate.SetEnableNanite(true);
	Candidate.InvalidateCollection(); Candidate.CreateSimulationData();
	if (!AuthorExactCollision(*Geometry, Record, Error)) { return false; }
	Candidate.bImportCollisionFromSource = true;
	Candidate.InvalidateCollection(); Candidate.CreateSimulationData(); Candidate.SetConvertVertexColorsToSRGB(true); Candidate.RebuildRenderData();
	Record.bNaniteReady = Candidate.HasNaniteData();
	if (!Record.bNaniteReady || Candidate.IsSimulationDataDirty() || !Candidate.HasVisibleGeometry()) { return Fail(Error, TEXT("detail cook/render failed; no fallback")); }
	if (!DublinFractureBake::ValidateCollisionData(Candidate, Record, Error) ||
		!DublinStructural::ValidateBuildingCollision(Candidate, Building, Record, Error)) { return false; }
	Record.BakeDiagnostics.Add(FString::Printf(TEXT("recipe=%d reason=%s tier=%d members=%d leaves=%d hulls=%d structuralCm3=%.9g sourceCm3=%.9g probes=%d"),
		static_cast<int32>(Record.Recipe), *Plan.Reason, Plan.Tier, Record.MemberCount, Record.PieceCount, Record.HullCount,
		Record.StructuralVolumeCm3, Record.SourceVolumeCm3, Record.CollisionProbes.Num()));
	Error.Reset();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailDoubleFrameTest,
	"DublinFlight.Destruction.CityDetail.NativeDoubleClipPrecisionFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailDoubleFrameTest::RunTest(const FString& Parameters)
{
	const FVector2D Along(.6, .8), Across(-.8, .6), Origin(10000, -20000);
	DublinStructural::FMember M;
	M.Polygon = {Origin, Origin + Along * 1000, Origin + Along * 1000 + Across * .001, Origin + Across * .001};
	M.MinZ = 0; M.MaxZ = 2000; M.bWall = true; M.WallTangent = Along; M.bSourcePlaneExterior = true;
	FDublinCityBuilding B; B.Id = TEXT("fixture/double-local-precision-frame");
	FPlanarCells Cells; FString Error;
	if (!Cells2D(MemberPoints(M), FVector(Along.X, Along.Y, 0), FVector::UpVector, 2, 314159u, Cells, Error)) { AddError(Error); return false; }
	FGeometryCollection Geometry; TArray<int32> Bones; double Reference = 0;
	if (!DublinStructural::AppendDoubleClippedCells(B, MemberFaces(M), Cells, Geometry, Bones, Reference, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("Thin translated member preserves every double-clipped cell"), Bones.Num(), 2);
	double Retained = 0;
	for (int32 Bone : Bones)
	{
		double Volume = 0;
		if (!DublinStructural::ValidateLeafGeometry(Geometry, Bone, Volume, Error)) { AddError(Error); return false; }
		Retained += Volume;
	}
	TestTrue(TEXT("Local precision frame preserves thin-member volume under the original tolerance"),
		FMath::Abs(Retained - Reference) <= FMath::Max(1.0, Reference * 1.e-4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailDoubleSeedTest,
	"DublinFlight.Destruction.CityDetail.NativeDoubleClipRecordedSeeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailDoubleSeedTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FString Error;
	if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { AddError(Error); return false; }
	struct FCase { FString Id; int32 Member; int32 Seeds; uint32 Seed; };
	const TArray<FCase> Cases{
		{TEXT("osm/way/1488401199"), 6, 74, 1898815813u},
		{TEXT("osm/way/1488401199"), 11, 3, 2734675131u},
		{TEXT("osm/relation/289528"), 79, 4, 80873342u},
		{TEXT("bridge/osm/way/282571104/0"), 3, 98, 2815543327u}};
	for (const FCase& Case : Cases)
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Source) { return Source.Id == Case.Id; });
		if (!TestNotNull(*Case.Id, B)) { return false; }
		DublinDetail::FPlan Plan;
		if (!DublinDetail::AnalyzeBuilding(*B, Plan, Error)) { AddError(Error); return false; }
		if (!TestTrue(TEXT("Recorded member remains present"), Plan.Assembly.Members.IsValidIndex(Case.Member))) { return false; }
		const auto& M = Plan.Assembly.Members[Case.Member];
		const FVector U = M.bWall ? FVector(M.WallTangent.X, M.WallTangent.Y, 0) : FVector::ForwardVector;
		const FVector V = M.bWall ? FVector::UpVector : FVector::RightVector;
		FPlanarCells Cells;
		if (!Cells2D(MemberPoints(M), U, V, Case.Seeds, Case.Seed, Cells, Error)) { AddError(Error); return false; }
		FGeometryCollection Geometry;
		TArray<int32> Leaves;
		double Reference = 0, Retained = 0;
		if (!DublinStructural::AppendDoubleClippedCells(*B, MemberFaces(M), Cells, Geometry, Leaves, Reference, Error))
		{
			AddError(FString::Printf(TEXT("%s member=%d fixedSeed=%u: %s"), *Case.Id, Case.Member, Case.Seed, *Error)); return false;
		}
		TestEqual(TEXT("Recorded seed produces every intended convex piece"), Leaves.Num(), Case.Seeds);
		for (int32 Bone : Leaves)
		{
			double Volume = 0;
			if (!DublinStructural::ValidateLeafGeometry(Geometry, Bone, Volume, Error)) { AddError(Error); return false; }
			Retained += Volume;
		}
		TestTrue(TEXT("Recorded-seed pieces conserve the double member reference"),
			FMath::Abs(Retained - Reference) <= FMath::Max(1.0, Reference * 1.e-4));
	}
	const auto SeedDigest = [](const FDublinCityBuilding& B, EDublinFractureRecipe Recipe, const FString& Revision)
	{
		FMD5 Hash;
		const int32 Version = UDublinCityFractureLibrary::CurrentBakeVersion;
		Hash.Update(reinterpret_cast<const uint8*>(&Version), sizeof(Version));
		const FTCHARToUTF8 Geometry(*DublinDestruction::BuildingGeometryDigest(B));
		Hash.Update(reinterpret_cast<const uint8*>(Geometry.Get()), Geometry.Length());
		for (const FColor& Color : B.Mesh.ColorsRGBA)
		{
			const uint8 RGBA[]{Color.R, Color.G, Color.B, Color.A}; Hash.Update(RGBA, sizeof(RGBA));
		}
		const FTCHARToUTF8 Text(*Revision);
		Hash.Update(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
		const uint8 Kind = static_cast<uint8>(Recipe); Hash.Update(&Kind, sizeof(Kind));
		uint8 Bytes[16]; Hash.Final(Bytes); return BytesToHex(Bytes, 16).ToLower();
	};
	const TArray<TPair<FString, int32>> Historical{
		{TEXT("bridge/osm/way/282571104/0"), 16}, {TEXT("osm/relation/289528"), 17}, {TEXT("osm/way/227766040"), 18}};
	for (const auto& Case : Historical)
	{
		const auto* B = Data.Buildings.FindByPredicate([&](const auto& Source) { return Source.Id == Case.Key; });
		if (!TestNotNull(*Case.Key, B)) { return false; }
		DublinDetail::FPlan Plan;
		if (!DublinDetail::AnalyzeBuilding(*B, Plan, Error)) { AddError(Error); return false; }
		TestEqual(TEXT("Historical seed fixture mirrors the current hash contract"), SeedDigest(*B, Plan.Recipe, DublinDetail::RecipeRevision), Plan.SourceDigest);
		const FString Extra = Case.Value >= 18 ? TEXT(".exactZeroAreaFaceCleanup.sourceProvenanceEdgeFlips") :
			Case.Value >= 17 ? TEXT(".exactZeroAreaFaceCleanup") : TEXT("");
		const FString Revision = FString::Printf(TEXT("Dublin.Detail.r%d.holes.offset30.floor20.normalRoof20.equalClear320.nativePlanarSiteVoronoi.seedBudgetExact.partitionMergeSafe.exactLoopClosure.checkedWindingFill.canonicalSourceRoundoff.exactZeroOnly.preserveBoundarySeams.maxAreaFaceNormals.interiorPrecision001cm.compatibleUnionRepartition.minMemberVolume1cm3.boundedFaceClipping.fixedVertexInternalSeamStitch.max4edges.sourceAttributedExteriorSeam%s.oneResidualStitch.max4edges.closedMemberVolumeLedger.orderedGeometryBlocks.tiers128-384-768-dense1536.compound.noStrip"),
			Case.Value, *Extra);
		const FString Digest = SeedDigest(*B, Plan.Recipe, Revision);
		FGeometryCollection Geometry;
		FDublinFractureRecord Record;
		Record.Recipe = Plan.Recipe; Record.MemberCount = Plan.Assembly.Members.Num();
		double Volume = 0;
		for (int32 I = 0; I < Plan.Assembly.Members.Num(); ++I)
		{
			const auto& M = Plan.Assembly.Members[I];
			const uint32 Seed = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%d:0"), *Digest, I));
			FPlanarCells Cells;
			if (M.Seeds == 1) { Cells.NumCells = 1; }
			else if (!Cells2D(MemberPoints(M), M.bWall ? FVector(M.WallTangent.X, M.WallTangent.Y, 0) : FVector::ForwardVector,
				M.bWall ? FVector::UpVector : FVector::RightVector, M.Seeds, Seed, Cells, Error)) { AddError(Error); return false; }
			TArray<int32> Leaves; double Reference = 0;
			if (!DublinStructural::AppendDoubleClippedCells(*B, MemberFaces(M), Cells, Geometry, Leaves, Reference, Error))
			{
				AddError(FString::Printf(TEXT("historical-r%d %s member=%d seed=%u: %s"), Case.Value, *Case.Key, I, Seed, *Error)); return false;
			}
			Record.LeafTransforms.Append(Leaves); Volume += Reference;
		}
		Record.PieceCount = Record.LeafTransforms.Num();
		if (!DublinStructural::ValidateExteriorGeometry(*B, Geometry, Record, Error)) { AddError(Error); return false; }
		TestTrue(TEXT("Historical failures keep source volume and exterior provenance"),
			FMath::Abs(Volume - Plan.Assembly.StructuralVolumeCm3) <= FMath::Max(1.0, Plan.Assembly.StructuralVolumeCm3 * 1.e-4));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailPlanarPrismTest,
	"DublinFlight.Destruction.CityDetail.NativePlanarPrismPartition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailPlanarPrismTest::RunTest(const FString& Parameters)
{
	for (bool bWall : {false, true})
	{
		for (int32 Seeds : {2, 7, 13})
		{
			FDublinCityBuilding Building;
			Building.Id = TEXT("fixture/native-planar-prism");
			DublinStructural::FMember Member;
			const double Depth = bWall ? 20.0 : 400.0;
			Member.Polygon = {{0, 0}, {300, 0}, {300, Depth}, {0, Depth}};
			Member.MinZ = 0; Member.MaxZ = bWall ? 400.0 : 20.0;
			Member.bSourcePlaneExterior = true;
			FString Error;
			FGeometryCollection Geometry;
			FPlanarCells Cells;
			const FVector U = FVector::ForwardVector, V = bWall ? FVector::UpVector : FVector::RightVector;
			const FVector ThicknessAxis = FVector::CrossProduct(U, V);
			if (!Cells2D(MemberPoints(Member), U, V, Seeds, 1827 + Seeds, Cells, Error)) { AddError(Error); return false; }
			for (int32 P = 0; P < Cells.Planes.Num(); ++P)
			{
				if (Cells.PlaneCells[P].Value < 0) { continue; }
				TestTrue(TEXT("Native coplanar sites produce only through-thickness internal prism cuts"),
					FMath::Abs(FVector::DotProduct(Cells.Planes[P].GetNormal(), ThicknessAxis)) < 1.e-10);
			}
			TArray<int32> Bones;
			double Reference = 0;
			if (!DublinStructural::AppendDoubleClippedCells(Building, MemberFaces(Member), Cells, Geometry, Bones, Reference, Error))
			{
				AddError(Error); return false;
			}
			int32 Leaves = 0;
			double Retained = 0;
			for (int32 Bone : Bones)
			{
				double Volume = 0;
				if (!DublinStructural::ValidateLeafGeometry(Geometry, Bone, Volume, Error))
				{
					AddError(FString::Printf(TEXT("wall=%d seeds=%d: %s"), bWall, Seeds, *Error)); return false;
				}
				Retained += Volume; ++Leaves;
			}
			TestEqual(TEXT("Every requested irregular prism has one closed physical leaf"), Leaves, Seeds);
			TestTrue(TEXT("Native shared facets/endcaps conserve the complete member volume"),
				FMath::IsNearlyEqual(Retained, 2400000.0, 240.0));
		}
	}
	return true;
}
#endif
#endif
