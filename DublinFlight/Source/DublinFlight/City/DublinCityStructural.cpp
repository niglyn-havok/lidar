#include "City/DublinCityStructural.h"

#include "City/DublinCityDestruction.h"
#include "Algo/Reverse.h"
#include "Chaos/Convex.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Chaos/ImplicitObjectBVH.h"
#include "Chaos/PhysicsObjectCollisionInterface.h"
#include "Chaos/ShapeInstance.h"
#include "CollisionQueryParams.h"
#include "CompGeom/ConvexHull3.h"
#include "Distance/DistPoint3Triangle3.h"
#include "Engine/HitResult.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "GeometryCollectionProxyData.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "PhysicsEngine/PhysicsObjectExternalInterface.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/IConsoleManager.h"
#include <cmath>
#include <limits>
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#endif

namespace
{
	double Cross2(const FVector2D& A, const FVector2D& B) { return A.X * B.Y - A.Y * B.X; }

	bool Fail(FString& Error, const FString& Message)
	{
		Error = TEXT("StructuralPilot: ") + Message;
		return false;
	}

	FVector2D Inward(const FVector2D& A, const FVector2D& B)
	{
		const FVector2D E = (B - A).GetSafeNormal();
		return FVector2D(-E.Y, E.X);
	}

	bool NearVolume(double A, double B)
	{
		return FMath::IsFinite(A) && FMath::IsFinite(B) && A > 0 && B > 0 &&
			FMath::Abs(A - B) <= FMath::Max(1.0, B * 1.e-4);
	}

	FVector2D Center(const TArray<FVector2D>& Polygon)
	{
		FVector2D Result = FVector2D::ZeroVector;
		for (const FVector2D& P : Polygon) { Result += P; }
		return Result / Polygon.Num();
	}

	FTransform BoneGlobal(const FGeometryCollection& Geometry, int32 Bone)
	{
		return GeometryCollectionAlgo::GlobalMatrix(Geometry.Transform, Geometry.Parent, Bone);
	}

	bool ReadBoundary(const FDublinCityBuilding& Building, uint8 Material, TArray<FVector2D>& Boundary,
		double& Height, FString& Error)
	{
		TArray<FVector2D> Points;
		TMap<FIntPoint, int32> Edges;
		double TriArea = 0;
		bool bHeightSet = false;
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			if (Building.MaterialIds[T] != Material) { continue; }
			int32 V[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const FVector& P = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + K]];
				if (!bHeightSet) { Height = P.Z; bHeightSet = true; }
				if (!FMath::IsNearlyEqual(P.Z, Height, 1.e-6)) { return Fail(Error, TEXT("roof/base is not horizontal")); }
				const FVector2D XY(P.X, P.Y);
				V[K] = Points.AddUnique(XY);
			}
			const double SignedArea = Cross2(Points[V[1]] - Points[V[0]], Points[V[2]] - Points[V[0]]) * .5;
			if ((Material == 0 && SignedArea >= 0) || (Material == 2 && SignedArea <= 0))
			{
				return Fail(Error, TEXT("roof/base triangle has inconsistent UE clockwise outward winding"));
			}
			TriArea += FMath::Abs(SignedArea);
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A = V[K], B = V[(K + 1) % 3];
				++Edges.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
			}
		}
		if (!bHeightSet) { return Fail(Error, TEXT("missing roof/base")); }
		TMap<int32, TArray<int32>> Neighbors;
		for (const auto& Edge : Edges)
		{
			if (Edge.Value > 2) { return Fail(Error, TEXT("nonmanifold roof/base edge")); }
			if (Edge.Value == 1)
			{
				Neighbors.FindOrAdd(Edge.Key.X).Add(Edge.Key.Y);
				Neighbors.FindOrAdd(Edge.Key.Y).Add(Edge.Key.X);
			}
		}
		if (Neighbors.Num() < 3 || Neighbors.Num() > 16) { return Fail(Error, TEXT("pilot footprint needs 3..16 boundary corners")); }
		int32 Start = INDEX_NONE;
		for (const auto& Pair : Neighbors)
		{
			if (Pair.Value.Num() != 2) { return Fail(Error, TEXT("roof/base boundary is not a simple loop")); }
			if (Start == INDEX_NONE || Points[Pair.Key].X < Points[Start].X ||
				(Points[Pair.Key].X == Points[Start].X && Points[Pair.Key].Y < Points[Start].Y)) { Start = Pair.Key; }
		}
		int32 Previous = INDEX_NONE, Current = Start;
		TSet<int32> Visited;
		while (!Visited.Contains(Current))
		{
			Visited.Add(Current);
			Boundary.Add(Points[Current]);
			const TArray<int32>& Next = Neighbors.FindChecked(Current);
			const int32 Target = Next[0] == Previous ? Next[1] : Next[0];
			Previous = Current;
			Current = Target;
		}
		if (Current != Start || Visited.Num() != Neighbors.Num()) { return Fail(Error, TEXT("multiple footprint loops are outside this pilot")); }
		if (DublinStructural::Area(Boundary) < 0) { Algo::Reverse(Boundary); }
		for (int32 I = 0; I < Boundary.Num(); ++I)
		{
			const FVector2D& A = Boundary[I];
			const FVector2D& B = Boundary[(I + 1) % Boundary.Num()];
			const FVector2D& C = Boundary[(I + 2) % Boundary.Num()];
			if (Cross2(B - A, C - B) <= 1.e-6) { return Fail(Error, TEXT("actual source boundary is not strictly convex; normalization is forbidden")); }
		}
		const double PolygonArea = DublinStructural::Area(Boundary);
		if (FMath::Abs(TriArea - PolygonArea) > FMath::Max(.01, PolygonArea * 1.e-8))
		{
			return Fail(Error, TEXT("roof/base triangles do not cover the boundary exactly"));
		}
		return true;
	}
}

bool DublinStructural::IsPilotId(const FString& Id)
{
	return Id == TEXT("osm/way/233804861") || Id == TEXT("osm/way/233804877") || Id == TEXT("osm/way/389853640");
}

double DublinStructural::Area(const TArray<FVector2D>& Polygon)
{
	double Result = 0;
	for (int32 I = 0; I < Polygon.Num(); ++I) { Result += Cross2(Polygon[I], Polygon[(I + 1) % Polygon.Num()]); }
	return Result * .5;
}

TArray<FVector2D> DublinStructural::Clip(const TArray<FVector2D>& Polygon, const FVector2D& Normal, double Offset)
{
	TArray<FVector2D> Result;
	for (int32 I = 0; I < Polygon.Num(); ++I)
	{
		const FVector2D& A = Polygon[I];
		const FVector2D& B = Polygon[(I + 1) % Polygon.Num()];
		const double DA = FVector2D::DotProduct(Normal, A) - Offset;
		const double DB = FVector2D::DotProduct(Normal, B) - Offset;
		if (DA >= 0) { Result.Add(A); }
		if ((DA >= 0) != (DB >= 0)) { Result.Add(FMath::Lerp(A, B, DA / (DA - DB))); }
	}
	for (int32 I = Result.Num() - 1; I >= 0 && Result.Num() > 1; --I)
	{
		if (Result[I].Equals(Result[(I + 1) % Result.Num()], 1.e-7)) { Result.RemoveAt(I); }
	}
	return Result;
}

bool DublinStructural::BuildAssembly(const FDublinCityBuilding& Building, FAssembly& Out, FString& Error)
{
	Out = FAssembly();
	const auto& Mesh = Building.Mesh;
	if (Mesh.VerticesCm.IsEmpty() || Mesh.Triangles.Num() % 3 != 0 ||
		Building.MaterialIds.Num() * 3 != Mesh.Triangles.Num() || Mesh.UV.Num() != Mesh.VerticesCm.Num() ||
		(!Mesh.ColorsRGBA.IsEmpty() && Mesh.ColorsRGBA.Num() != Mesh.VerticesCm.Num()))
	{
		return Fail(Error, TEXT("invalid source array sizes"));
	}
	for (const FVector& P : Mesh.VerticesCm)
	{
		if (P.ContainsNaN() || P.GetAbsMax() > 200000) { return Fail(Error, TEXT("invalid source position")); }
	}
	for (int32 Index : Mesh.Triangles)
	{
		if (!Mesh.VerticesCm.IsValidIndex(Index)) { return Fail(Error, TEXT("invalid source triangle index")); }
	}
	TArray<FVector2D> Base;
	if (!ReadBoundary(Building, 0, Out.Outer, Out.MaxZ, Error) ||
		!ReadBoundary(Building, 2, Base, Out.MinZ, Error)) { return false; }
	if (Base.Num() != Out.Outer.Num()) { return Fail(Error, TEXT("roof/base footprint mismatch")); }
	for (const FVector2D& P : Base)
	{
		if (!Out.Outer.Contains(P)) { return Fail(Error, TEXT("roof/base footprints differ")); }
	}
	const double H = Out.MaxZ - Out.MinZ;
	if (H <= 2 * SlabCm + 100 || H > 10000) { return Fail(Error, TEXT("unsupported pilot height")); }
	Out.Inner = Out.Outer;
	for (int32 I = 0; I < Out.Outer.Num(); ++I)
	{
		const FVector2D N = Inward(Out.Outer[I], Out.Outer[(I + 1) % Out.Outer.Num()]);
		Out.Inner = Clip(Out.Inner, N, FVector2D::DotProduct(N, Out.Outer[I]) + WallCm);
	}
	if (Out.Inner.Num() < 3 || Area(Out.Inner) < 10000) { return Fail(Error, TEXT("30cm inset has no usable room")); }
	// Source faces must describe this exact prism, including outward wall orientation.
	for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
	{
		const FVector& A = Mesh.VerticesCm[Mesh.Triangles[T * 3]];
		const FVector& B = Mesh.VerticesCm[Mesh.Triangles[T * 3 + 1]];
		const FVector& C = Mesh.VerticesCm[Mesh.Triangles[T * 3 + 2]];
		Out.SourceVolumeCm3 += FVector::DotProduct(A, FVector::CrossProduct(C, B)) / 6;
		if (Building.MaterialIds[T] == 1)
		{
			bool bWall = false;
			const FVector Normal = DublinCity::ClockwiseNormal(A, B, C);
			for (int32 I = 0; I < Out.Outer.Num(); ++I)
			{
				const FVector2D N = Inward(Out.Outer[I], Out.Outer[(I + 1) % Out.Outer.Num()]);
				const double D = FVector2D::DotProduct(N, Out.Outer[I]);
				const auto OnWall = [&](const FVector& P)
				{
					return FMath::Abs(FVector2D::DotProduct(N, FVector2D(P.X, P.Y)) - D) < 1.e-6 &&
						P.Z >= Out.MinZ && P.Z <= Out.MaxZ;
				};
				bWall |= OnWall(A) && OnWall(B) && OnWall(C) && FVector::DotProduct(Normal, FVector(-N.X, -N.Y, 0)) > .999999;
			}
			if (!bWall) { return Fail(Error, TEXT("source wall does not match outward vertical prism boundary")); }
		}
		else if (Building.MaterialIds[T] != 0 && Building.MaterialIds[T] != 2) { return Fail(Error, TEXT("unsupported source material")); }
	}
	if (!NearVolume(Out.SourceVolumeCm3, Area(Out.Outer) * H)) { return Fail(Error, TEXT("source signed volume does not match its clockwise prism")); }
	double RingArea = 0;
	for (int32 I = 0; I < Out.Outer.Num(); ++I)
	{
		const FVector2D N = Inward(Out.Outer[I], Out.Outer[(I + 1) % Out.Outer.Num()]);
		const double D = FVector2D::DotProduct(N, Out.Outer[I]);
		FMember Member;
		Member.Polygon = Clip(Out.Outer, -N, -D - WallCm);
		for (int32 J = 0; J < Out.Outer.Num(); ++J)
		{
			if (I == J) { continue; }
			const FVector2D Other = Inward(Out.Outer[J], Out.Outer[(J + 1) % Out.Outer.Num()]);
			Member.Polygon = Clip(Member.Polygon, Other - N, FVector2D::DotProduct(Other, Out.Outer[J]) - D);
		}
		if (Member.Polygon.Num() < 3 || Area(Member.Polygon) <= 1) { return Fail(Error, TEXT("mitered wall partition collapsed")); }
		Member.MinZ = Out.MinZ + SlabCm;
		Member.MaxZ = Out.MaxZ - SlabCm;
		Member.WallTangent = (Out.Outer[(I + 1) % Out.Outer.Num()] - Out.Outer[I]).GetSafeNormal();
		Member.Weight = FVector2D::Distance(Out.Outer[I], Out.Outer[(I + 1) % Out.Outer.Num()]) * (H - 2 * SlabCm);
		Member.bWall = true;
		RingArea += Area(Member.Polygon);
		Out.Members.Add(MoveTemp(Member));
	}
	if (!NearVolume(RingArea, Area(Out.Outer) - Area(Out.Inner))) { return Fail(Error, TEXT("wall ring does not partition the inset")); }
	const auto AddSlab = [&](const TArray<FVector2D>& Polygon, double Bottom)
	{
		FMember Member;
		Member.Polygon = Polygon;
		Member.MinZ = Bottom;
		Member.MaxZ = Bottom + SlabCm;
		Member.Weight = Area(Polygon);
		Out.Members.Add(MoveTemp(Member));
	};
	AddSlab(Out.Outer, Out.MinZ);
	AddSlab(Out.Outer, Out.MaxZ - SlabCm);
	const int32 Storeys = FMath::Max(1, FMath::RoundToInt((H - 2 * SlabCm) / 320.0));
	const double Clear = (H - 2 * SlabCm - SlabCm * (Storeys - 1)) / Storeys;
	if (Clear < 100) { return Fail(Error, TEXT("inferred room clearance is too small")); }
	for (int32 K = 0; K < Storeys; ++K)
	{
		const double Bottom = Out.MinZ + SlabCm + K * (Clear + SlabCm);
		Out.RoomsZ.Emplace(Bottom, Bottom + Clear);
		if (K + 1 < Storeys) { AddSlab(Out.Inner, Bottom + Clear); }
	}
	double TotalWeight = 0;
	for (const FMember& Member : Out.Members)
	{
		TotalWeight += Member.Weight;
		Out.StructuralVolumeCm3 += Area(Member.Polygon) * (Member.MaxZ - Member.MinZ);
	}
	const double Expected = RingArea * (H - 40) + Area(Out.Outer) * 40 + Area(Out.Inner) * 20 * (Storeys - 1);
	if (!NearVolume(Out.StructuralVolumeCm3, Expected) || Out.StructuralVolumeCm3 >= Out.SourceVolumeCm3)
	{
		return Fail(Error, TEXT("invalid structural assembly volume"));
	}
	const int32 Remaining = SeedTarget - 3 * Out.Members.Num();
	if (Remaining < 0) { return Fail(Error, TEXT("too many members for pilot seed budget")); }
	TArray<double> Fractions;
	int32 Allocated = 0;
	for (FMember& Member : Out.Members)
	{
		const double Share = Remaining * Member.Weight / TotalWeight;
		Member.Seeds = 3 + FMath::FloorToInt(Share);
		Allocated += Member.Seeds;
		Fractions.Add(Share - FMath::FloorToDouble(Share));
	}
	while (Allocated++ < SeedTarget)
	{
		int32 Best = 0;
		for (int32 I = 1; I < Fractions.Num(); ++I) { if (Fractions[I] > Fractions[Best]) { Best = I; } }
		++Out.Members[Best].Seeds;
		Fractions[Best] = -1;
	}
	Error.Reset();
	return true;
}

namespace
{
	struct FCollisionHull
	{
		const Chaos::FConvex* Hull = nullptr;
		FTransform ToCollection;
		TArray<FVector> Vertices;
		FBox Bounds = FBox(ForceInit);
	};

	FCollisionHull HullView(const Chaos::FConvex& Hull, const FTransform& Transform)
	{
		FCollisionHull Out;
		Out.Hull = &Hull;
		Out.ToCollection = Transform;
		for (int32 I = 0; I < Hull.NumVertices(); ++I)
		{
			const FVector V = Transform.TransformPosition(FVector(Hull.GetVertex(I)));
			Out.Vertices.Add(V);
			Out.Bounds += V;
		}
		return Out;
	}

	const Chaos::FConvex* RigidConvex(const Chaos::FImplicitObject* Implicit, FTransform& ToCollection)
	{
		for (int32 Depth = 0; Implicit && Depth < 4; ++Depth)
		{
			if (const auto* Hull = Implicit->GetObject<Chaos::FConvex>()) { return Hull; }
			const auto* Wrapped = Implicit->GetObject<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>();
			if (!Wrapped) { return nullptr; }
			const FTransform Transform(Wrapped->GetTransform());
			if (Transform.ContainsNaN() || !Transform.GetScale3D().Equals(FVector::OneVector, 1.e-8)) { return nullptr; }
			ToCollection = Transform * ToCollection;
			Implicit = Wrapped->GetTransformedObject();
		}
		return nullptr;
	}

	double BoundsDelta(const FCollisionHull& A, const FCollisionHull& B)
	{
		return FMath::Max((A.Bounds.Min - B.Bounds.Min).GetAbsMax(), (A.Bounds.Max - B.Bounds.Max).GetAbsMax());
	}

	double DirectedConvexDistanceSquared(const TArray<FVector>& From, const TArray<FVector>& To)
	{
		if (From.Num() < 4 || To.Num() < 4) { return TNumericLimits<double>::Max(); }
		for (const TArray<FVector>* Vertices : {&From, &To})
		{
			for (const FVector& V : *Vertices) { if (V.ContainsNaN()) { return TNumericLimits<double>::Max(); } }
		}
		const FVector Origin = FBox(To).GetCenter();
		TArray<FVector> Local;
		FVector Interior = FVector::ZeroVector;
		for (const FVector& V : To)
		{
			Local.Add(V - Origin);
			Interior += Local.Last();
		}
		Interior /= Local.Num();
		UE::Geometry::FConvexHull3d Hull;
		if (!Hull.Solve(Local.Num(), [&Local](int32 I) { return Local[I]; }) || !Hull.IsSolutionAvailable())
		{
			return TNumericLimits<double>::Max();
		}
		TArray<FPlane> SupportPlanes;
		TArray<UE::Geometry::FTriangle3d> Triangles;
		for (const UE::Geometry::FIndex3i& T : Hull.GetTriangles())
		{
			const FVector A = Local[T.A], B = Local[T.B], C = Local[T.C];
			FVector Normal = FVector::CrossProduct(B - A, C - A);
			const double LengthSquared = Normal.SizeSquared();
			if (!FMath::IsFinite(LengthSquared) || LengthSquared <= 1.e-24) { return TNumericLimits<double>::Max(); }
			Normal /= FMath::Sqrt(LengthSquared);
			if (FVector::DotProduct(Normal, Interior - A) > 0) { Normal = -Normal; }
			double Offset = -TNumericLimits<double>::Max();
			for (const FVector& V : Local) { Offset = FMath::Max(Offset, FVector::DotProduct(Normal, V)); }
			SupportPlanes.Emplace(Normal, Offset);
			Triangles.Emplace(A, B, C);
		}
		if (Triangles.Num() < 4) { return TNumericLimits<double>::Max(); }
		double MaximumSquared = 0;
		for (const FVector& V : From)
		{
			const FVector Point = V - Origin;
			bool bInside = true;
			for (const FPlane& Plane : SupportPlanes) { bInside &= Plane.PlaneDot(Point) <= 0; }
			if (bInside) { continue; }
			double NearestSquared = TNumericLimits<double>::Max();
			for (const UE::Geometry::FTriangle3d& Triangle : Triangles)
			{
				UE::Geometry::TDistPoint3Triangle3<double> Distance(Point, Triangle);
				const double Squared = Distance.GetSquared();
				if (!FMath::IsFinite(Squared) || Squared < 0) { return TNumericLimits<double>::Max(); }
				NearestSquared = FMath::Min(NearestSquared, Squared);
			}
			MaximumSquared = FMath::Max(MaximumSquared, NearestSquared);
		}
		return MaximumSquared;
	}

	bool SameConvexGeometry(const TArray<FVector>& A, const TArray<FVector>& B)
	{
		// Distance to a convex set is convex: checking all input vertices bounds the entire hull.
		// Rebuild unsimplified double-precision boundaries; do not use Chaos' merged planes or Phi.
		const double ToleranceSquared = FMath::Square(DublinStructural::ContactToleranceCm);
		return DirectedConvexDistanceSquared(A, B) <= ToleranceSquared &&
			DirectedConvexDistanceSquared(B, A) <= ToleranceSquared;
	}

	FString DescribeHullPair(const FCollisionHull& A, const FCollisionHull& B)
	{
		FString Result = FString::Printf(TEXT("boundsDeltaCm=%.9g minDelta=%s maxDelta=%s A[v=%d p=%d vol=%.9g margin=%.9g bounds=%s] B[v=%d p=%d vol=%.9g margin=%.9g bounds=%s]"),
			BoundsDelta(A, B), *(A.Bounds.Min - B.Bounds.Min).ToString(), *(A.Bounds.Max - B.Bounds.Max).ToString(),
			A.Hull->NumVertices(), A.Hull->NumPlanes(), static_cast<double>(A.Hull->GetVolume()), static_cast<double>(A.Hull->GetMargin()), *A.Bounds.ToString(),
			B.Hull->NumVertices(), B.Hull->NumPlanes(), static_cast<double>(B.Hull->GetVolume()), static_cast<double>(B.Hull->GetMargin()), *B.Bounds.ToString());
		Result += FString::Printf(TEXT(" convexSetDistanceCm[AtoB=%.9g BtoA=%.9g]"),
			FMath::Sqrt(DirectedConvexDistanceSquared(A.Vertices, B.Vertices)),
			FMath::Sqrt(DirectedConvexDistanceSquared(B.Vertices, A.Vertices)));
		for (const auto& Pair : {TPair<const TCHAR*, const FCollisionHull*>(TEXT("A"), &A), TPair<const TCHAR*, const FCollisionHull*>(TEXT("B"), &B)})
		{
			Result += FString::Printf(TEXT("\n%sVertices["), Pair.Key);
			for (const FVector& P : Pair.Value->Vertices) { Result += FString::Printf(TEXT("(%.17g,%.17g,%.17g),"), P.X, P.Y, P.Z); }
			Result += TEXT("]");
		}
		for (int32 Direction = 0; Direction < 2; ++Direction)
		{
			const FCollisionHull& From = Direction == 0 ? A : B;
			const FCollisionHull& To = Direction == 0 ? B : A;
			double MaxPhi = -TNumericLimits<double>::Max(), MaxPlane = MaxPhi, MaxNearestVertex = 0;
			int32 Worst = INDEX_NONE, Outside = 0;
			for (int32 V = 0; V < From.Vertices.Num(); ++V)
			{
				const FVector Local = To.ToCollection.InverseTransformPosition(From.Vertices[V]);
				Chaos::FVec3 Normal;
				const double Phi = To.Hull->PhiWithNormal(Local, Normal);
				if (Phi > MaxPhi) { MaxPhi = Phi; Worst = V; }
				Outside += !To.Hull->Overlap(Local, DublinStructural::ContactToleranceCm);
				for (int32 P = 0; P < To.Hull->NumPlanes(); ++P)
				{
					const auto Plane = To.Hull->GetPlane(P);
					MaxPlane = FMath::Max(MaxPlane, FVector::DotProduct(Local - FVector(Plane.X()), FVector(Plane.Normal())));
				}
				double Nearest = TNumericLimits<double>::Max();
				for (const FVector& Other : To.Vertices) { Nearest = FMath::Min(Nearest, FVector::Distance(From.Vertices[V], Other)); }
				MaxNearestVertex = FMath::Max(MaxNearestVertex, Nearest);
			}
			Result += FString::Printf(TEXT(" %s[maxPhiCm=%.9g maxPlaneCm=%.9g maxNearestVertexCm=%.9g outside=%d worstVertex=%d collectionPoint=%s]"),
				Direction == 0 ? TEXT("AtoB") : TEXT("BtoA"), MaxPhi, MaxPlane, MaxNearestVertex, Outside, Worst,
				Worst == INDEX_NONE ? TEXT("none") : *From.Vertices[Worst].ToString());
		}
		return Result;
	}

	bool HasPositiveOverlap(const FCollisionHull& A, const FCollisionHull& B)
	{
		if (!A.Bounds.Intersect(B.Bounds)) { return false; }
		const auto Separated = [&](FVector Axis)
		{
			if (!Axis.Normalize()) { return false; }
			double AMin = TNumericLimits<double>::Max(), AMax = -AMin, BMin = AMin, BMax = AMax;
			for (const FVector& V : A.Vertices) { const double D = FVector::DotProduct(Axis, V); AMin = FMath::Min(AMin, D); AMax = FMath::Max(AMax, D); }
			for (const FVector& V : B.Vertices) { const double D = FVector::DotProduct(Axis, V); BMin = FMath::Min(BMin, D); BMax = FMath::Max(BMax, D); }
			return FMath::Min(AMax, BMax) - FMath::Max(AMin, BMin) <= DublinStructural::ContactToleranceCm;
		};
		for (const FCollisionHull* H : {&A, &B})
		{
			for (int32 P = 0; P < H->Hull->NumPlanes(); ++P)
			{
				if (Separated(H->ToCollection.TransformVectorNoScale(FVector(H->Hull->GetPlane(P).Normal())))) { return false; }
			}
		}
		for (int32 EA = 0; EA < A.Hull->NumEdges(); ++EA)
		{
			const FVector DA = A.Vertices[A.Hull->GetEdgeVertex(EA, 1)] - A.Vertices[A.Hull->GetEdgeVertex(EA, 0)];
			for (int32 EB = 0; EB < B.Hull->NumEdges(); ++EB)
			{
				const FVector DB = B.Vertices[B.Hull->GetEdgeVertex(EB, 1)] - B.Vertices[B.Hull->GetEdgeVertex(EB, 0)];
				if (Separated(FVector::CrossProduct(DA.GetSafeNormal(), DB.GetSafeNormal()))) { return false; }
			}
		}
		return true;
	}

	bool NoOverlaps(const TArray<FCollisionHull>& Hulls, FString& Error)
	{
		for (int32 I = 0; I < Hulls.Num(); ++I)
		{
			for (int32 J = 0; J < I; ++J)
			{
				if (HasPositiveOverlap(Hulls[I], Hulls[J])) { return Fail(Error, FString::Printf(TEXT("cooked hulls %d and %d overlap"), I, J)); }
			}
		}
		return true;
	}

	bool Ray(const Chaos::FImplicitObject& Shape, const FTransform& ToCollection, const FVector& A, const FVector& B)
	{
		const FVector Start = ToCollection.InverseTransformPosition(A);
		const FVector Delta = ToCollection.InverseTransformPosition(B) - Start;
		Chaos::FReal Time = 0;
		Chaos::FVec3 Position, Normal;
		int32 Face = INDEX_NONE;
		return Shape.Raycast(Start, Delta.GetSafeNormal(), Delta.Size(), 0, Time, Position, Normal, Face);
	}

	bool ProbeRooms(const DublinStructural::FAssembly& Assembly,
		TFunctionRef<bool(const FVector&, const FVector&)> HasHit,
		TFunctionRef<bool(const FVector&)> IsOccupied, FString& Error)
	{
		const FVector2D Mid = Center(Assembly.Inner);
		for (const FVector2D& Room : Assembly.RoomsZ)
		{
			const double Z = (Room.X + Room.Y) * .5;
			const FVector C(Mid.X, Mid.Y, Z);
			for (int32 I = 0; I < Assembly.Inner.Num(); ++I)
			{
				const FVector2D P = FMath::Lerp(Mid, Assembly.Inner[I], .75);
				const FVector Q(P.X, P.Y, Z);
				if (IsOccupied(Q) || IsOccupied(C) || HasHit(C, Q)) { return Fail(Error, TEXT("room void is filled by cooked collision")); }
			}
			if (!HasHit(C, FVector(C.X, C.Y, Room.X - 10)) ||
				!HasHit(C, FVector(C.X, C.Y, Room.Y + 10)))
			{
				return Fail(Error, TEXT("cooked floor/ceiling does not block"));
			}
			for (int32 I = 0; I < Assembly.Outer.Num(); ++I)
			{
				const FVector2D P = (Assembly.Outer[I] + Assembly.Outer[(I + 1) % Assembly.Outer.Num()]) * .5;
				const FVector2D N = Inward(Assembly.Outer[I], Assembly.Outer[(I + 1) % Assembly.Outer.Num()]);
				if (!HasHit(C, FVector(P.X - N.X * 10, P.Y - N.Y * 10, Z))) { return Fail(Error, TEXT("cooked wall does not block")); }
			}
		}
		return true;
	}

	bool ProbeRecord(const FDublinFractureRecord& Record,
		TFunctionRef<bool(const FVector&, const FVector&)> HasHit,
		TFunctionRef<bool(const FVector&)> IsOccupied, FString& Error)
	{
		if (Record.ProbeVersion != 1 || Record.CollisionProbes.IsEmpty() || Record.CollisionProbes.Num() > 4096 ||
			Record.VoidSamplesCm.Num() > 4096 ||
			(Record.Recipe == EDublinFractureRecipe::StructuralCity && Record.VoidSamplesCm.IsEmpty()))
		{
			return Fail(Error, TEXT("missing or unsupported cooked city-detail probe contract"));
		}
		for (const FVector& P : Record.VoidSamplesCm)
		{
			if (P.ContainsNaN() || IsOccupied(P)) { return Fail(Error, TEXT("city-detail room/courtyard point is occupied")); }
		}
		for (int32 I = 0; I < Record.CollisionProbes.Num(); ++I)
		{
			const auto& P = Record.CollisionProbes[I];
			if (P.StartCm.ContainsNaN() || P.EndCm.ContainsNaN() || P.StartCm.Equals(P.EndCm, 1.e-8) ||
				HasHit(P.StartCm, P.EndCm) != P.bExpectHit)
			{
				return Fail(Error, FString::Printf(TEXT("city-detail probe %d failed expectedHit=%d start=%s end=%s"),
					I, P.bExpectHit, *P.StartCm.ToString(), *P.EndCm.ToString()));
			}
		}
		return true;
	}

	struct FQueryCounts
	{
		uint64 Queries = 0;
		uint64 PossibleCandidates = 0;
		uint64 NarrowphaseCandidates = 0;
		uint64 FallbackQueries = 0;
		uint64 LocalBVHs = 0;
	};

	class FUnionCandidates
	{
	public:
		explicit FUnionCandidates(const Chaos::FImplicitObject& Shape)
		{
			Union = Shape.AsA<Chaos::FImplicitObjectUnion>();
			const IConsoleVariable* Enabled = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Collision.UnionBVH.Enabled"));
			if (!Union || !Enabled || Enabled->GetInt() == 0) { return; }
			Reason = TEXT("no-bvh");
			const auto& Parts = Union->GetObjects();
			if (Parts.IsEmpty()) { return; }
			QueryUnion = Union;
			if (!Union->GetBVH())
			{
				// Cold external particles may not yet have the physics-thread BVH.
				// This is a private query index over the SAME live children, never a collider.
				TArray<Chaos::FImplicitObjectPtr> References = Parts;
				LocalIndex = MakeUnique<Chaos::FImplicitObjectUnion>(MoveTemp(References));
				LocalIndex->SetAllowBVH(true);
				QueryUnion = LocalIndex.Get();
				bLocalBVH = QueryUnion->GetBVH() != nullptr;
			}
			if (!QueryUnion->GetBVH()) { return; }
			const auto& Cached = QueryUnion->GetBVH()->GetObjects();
			Reason = TEXT("non-flat-bvh");
			if (Parts.IsEmpty() || Cached.Num() != Parts.Num()) { return; }
			TBitArray<> Seen(false, Parts.Num());
			for (const auto& Object : Cached)
			{
				const int32 Index = Object.GetRootObjectIndex();
				Reason = TEXT("invalid-root-index");
				if (!Parts.IsValidIndex(Index) || Seen[Index] || !Parts[Index]) { return; }
				Seen[Index] = true;
				int32 Leaves = 0;
				bool bCovered = true;
				Parts[Index]->VisitLeafObjects([&](const Chaos::FImplicitObject* Leaf, const Chaos::FRigidTransform3& Transform,
					int32, int32, int32)
				{
					++Leaves;
					if (!Leaf || !Leaf->GetObject<Chaos::FConvex>() || Leaf != Object.GetGeometry() ||
						!FTransform(Transform).GetScale3D().Equals(FVector::OneVector, 1.e-8))
					{
						bCovered = false; return;
					}
					const auto Exact = Leaf->CalculateTransformedBounds(Transform);
					const auto& Box = Object.GetBounds();
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						bCovered &= FMath::IsFinite(Exact.Min()[Axis]) && FMath::IsFinite(Exact.Max()[Axis]) &&
							FMath::IsFinite(Box.Min()[Axis]) && FMath::IsFinite(Box.Max()[Axis]) &&
							double(Box.Min()[Axis]) - Exact.Min()[Axis] <= DublinStructural::ContactToleranceCm * .25 &&
							Exact.Max()[Axis] - double(Box.Max()[Axis]) <= DublinStructural::ContactToleranceCm * .25;
					}
				});
				Reason = TEXT("leaf-identity-or-bounds");
				if (Leaves != 1 || !bCovered) { return; }
			}
			bUsable = true;
			Reason = TEXT("supported");
		}

		bool Select(const Chaos::FVec3& A, const Chaos::FVec3& B)
		{
			Candidates.Reset();
			if (!bUsable) { return false; }
			Chaos::FVec3 Min, Max;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				// The BVH stores float bounds. Round the expanded query OUTWARD before its float conversion.
				const float Low = std::nextafter(static_cast<float>(FMath::Min(A[Axis], B[Axis]) - DublinStructural::ContactToleranceCm),
					-std::numeric_limits<float>::infinity());
				const float High = std::nextafter(static_cast<float>(FMath::Max(A[Axis], B[Axis]) + DublinStructural::ContactToleranceCm),
					std::numeric_limits<float>::infinity());
				if (!FMath::IsFinite(Low) || !FMath::IsFinite(High)) { return false; }
				Min[Axis] = Low; Max[Axis] = High;
			}
			bool bValid = true;
			QueryUnion->VisitOverlappingLeafObjects(Chaos::FAABB3(Min, Max),
				[&](const Chaos::FImplicitObject*, const Chaos::FRigidTransform3&, int32 RootIndex, int32, int32)
				{
					if (!Union->GetObjects().IsValidIndex(RootIndex)) { bValid = false; return; }
					Candidates.Add(RootIndex);
				});
			// Restore native union order, including its nearest-hit tie behaviour.
			Candidates.Sort();
			return bValid;
		}

		const Chaos::FImplicitObjectUnion* Union = nullptr;
		TArray<int32, TInlineAllocator<64>> Candidates;
		bool bUsable = false;
		bool bLocalBVH = false;
		const TCHAR* Reason = TEXT("not-union-or-disabled");
	private:
		const Chaos::FImplicitObjectUnion* QueryUnion = nullptr;
		TUniquePtr<Chaos::FImplicitObjectUnion> LocalIndex;
	};

	class FNativeUnionQueries
	{
	public:
		FUnionCandidates& Context(const Chaos::FImplicitObject& Shape)
		{
			auto& Entry = Contexts.FindOrAdd(&Shape);
			if (!Entry)
			{
				Entry = MakeUnique<FUnionCandidates>(Shape);
				Counts.LocalBVHs += Entry->bLocalBVH ? 1 : 0;
			}
			return *Entry;
		}

		bool Raycast(const Chaos::FImplicitObject& Shape, const Chaos::FVec3& Start, const Chaos::FVec3& Dir,
			Chaos::FReal Length, Chaos::FReal Thickness, Chaos::FReal& Time, Chaos::FVec3& Position,
			Chaos::FVec3& Normal, int32& Face)
		{
			++Counts.Queries;
			FUnionCandidates& C = Context(Shape);
			if (Thickness != 0 || !C.Select(Start, Start + Dir * Length))
			{
				++Counts.FallbackQueries;
				return Shape.Raycast(Start, Dir, Length, Thickness, Time, Position, Normal, Face);
			}
			Counts.PossibleCandidates += C.Union->GetObjects().Num();
			bool bHit = false;
			for (int32 Index : C.Candidates)
			{
				++Counts.NarrowphaseCandidates;
				Chaos::FReal T; Chaos::FVec3 P, N; int32 F;
				// Do NOT use the visitor's float-rounded transform for narrowphase.
				// Query the actual original child, with its exact native wrapper/transform.
				if (C.Union->GetObjects()[Index]->Raycast(Start, Dir, Length, Thickness, T, P, N, F) && (!bHit || T < Time))
				{
					bHit = true; Time = T; Position = P; Normal = N; Face = F;
				}
			}
			return bHit;
		}

		bool Overlap(const Chaos::FImplicitObject& Shape, const Chaos::FVec3& Point, Chaos::FReal Thickness)
		{
			++Counts.Queries;
			FUnionCandidates& C = Context(Shape);
			if (Thickness != 0 || !C.Select(Point, Point))
			{
				++Counts.FallbackQueries;
				return Shape.Overlap(Point, Thickness);
			}
			Counts.PossibleCandidates += C.Union->GetObjects().Num();
			for (int32 Index : C.Candidates)
			{
				++Counts.NarrowphaseCandidates;
				if (C.Union->GetObjects()[Index]->Overlap(Point, Thickness)) { return true; }
			}
			return false;
		}

		FQueryCounts Counts;
	private:
		TMap<const Chaos::FImplicitObject*, TUniquePtr<FUnionCandidates>> Contexts;
	};

	class FRegisteredProbeBatch
	{
	public:
		explicit FRegisteredProbeBatch(UGeometryCollectionComponent& Component)
			: Objects(Component.GetAllPhysicsObjects())
			, Interface(FPhysicsObjectExternalInterface::LockRead(Objects))
			, Collision(Interface.GetInterface())
		{
			// Match UPrimitiveComponent::LineTraceComponent's GC path, but prepare once.
			Objects = Objects.FilterByPredicate([this](Chaos::FPhysicsObjectHandle Handle)
			{
				return !Interface->AreAllDisabled({&Handle, 1});
			});
			for (Chaos::FPhysicsObjectHandle Object : Objects)
			{
				FObjectShapes& Live = LiveObjects.AddDefaulted_GetRef();
				Live.Object = Object;
				Live.Geometry = Interface->GetGeometry(Object);
				Live.Shapes = Interface->GetAllThreadShapes({&Object, 1});
				if (!Live.Geometry) { continue; }
				FUnionCandidates& C = Queries.Context(*Live.Geometry);
				Live.bWholeUnion = C.bUsable && Live.Shapes.Num() == 1 && Live.Shapes[0] &&
					Live.Shapes[0]->GetGeometry() == Live.Geometry;
				Live.bMapped = C.bUsable && Live.Shapes.Num() == C.Union->GetObjects().Num();
				for (int32 I = 0; Live.bMapped && I < Live.Shapes.Num(); ++I)
				{
					Live.bMapped = Live.Shapes[I] && Live.Shapes[I]->GetGeometry() == C.Union->GetObjects()[I].GetReference();
				}
			}
		}

		bool Ray(const FVector& A, const FVector& B, float* OutDistance = nullptr)
		{
			const FVector Delta = B - A;
			const double Length = Delta.Size();
			if (Length < UE_KINDA_SMALL_NUMBER) { return false; }
			float Best = TNumericLimits<float>::Max();
			bool bHit = false;
			for (const FObjectShapes& Live : LiveObjects)
			{
				const FTransform WorldTM = Interface->GetTransform(Live.Object);
				const FVector Start = WorldTM.InverseTransformPositionNoScale(A);
				const FVector LocalDelta = WorldTM.InverseTransformVectorNoScale(Delta);
				if (Live.bWholeUnion)
				{
					if (!Live.Shapes[0]->GetShapeFilterData().HasFlag(Chaos::EFilterFlags::SimpleCollision)) { continue; }
					Chaos::FReal Distance; Chaos::FVec3 Position, Normal; int32 Face;
					if (Queries.Raycast(*Live.Shapes[0]->GetGeometry(), Start, LocalDelta / Length, Length, 0,
						Distance, Position, Normal, Face) && Distance < Best)
					{
						Best = static_cast<float>(Distance); bHit = true;
					}
					continue;
				}
				FUnionCandidates* C = Live.bMapped ? &Queries.Context(*Live.Geometry) : nullptr;
				if (!C || !C->Select(Start, Start + LocalDelta))
				{
					++Queries.Counts.FallbackQueries;
					ChaosInterface::FRaycastHit Hit;
					if (Collision.LineTrace({&Live.Object, 1}, A, B, false, Hit) && Hit.Distance < Best)
					{
						Best = Hit.Distance; bHit = true;
					}
					continue;
				}
				++Queries.Counts.Queries;
				Queries.Counts.PossibleCandidates += Live.Shapes.Num();
				for (int32 Index : C->Candidates)
				{
					const auto* Shape = Live.Shapes[Index];
					if (!Shape->GetShapeFilterData().HasFlag(Chaos::EFilterFlags::SimpleCollision)) { continue; }
					++Queries.Counts.NarrowphaseCandidates;
					Chaos::FReal Distance; Chaos::FVec3 Position, Normal; int32 Face;
					if (Shape->GetGeometry()->Raycast(Start, LocalDelta / Length, Length, 0, Distance, Position, Normal, Face) &&
						Distance < Best)
					{
						Best = static_cast<float>(Distance); bHit = true;
					}
				}
			}
			if (OutDistance) { *OutDistance = Best; }
			return bHit;
		}
		FNativeUnionQueries Queries;
	private:
		struct FObjectShapes
		{
			Chaos::FPhysicsObjectHandle Object = nullptr;
			const Chaos::FImplicitObject* Geometry = nullptr;
			TArray<Chaos::FShapeInstanceProxy*> Shapes;
			bool bMapped = false;
			bool bWholeUnion = false;
		};
		TArray<Chaos::FPhysicsObjectHandle> Objects;
		FLockedReadPhysicsObjectExternalInterface Interface;
		Chaos::FPhysicsObjectCollisionInterface_External Collision;
		TArray<FObjectShapes> LiveObjects;
	};
}

bool DublinStructural::ValidateCookedCollision(const UGeometryCollection& Collection,
	const FDublinFractureRecord& Record, FString& Error)
{
	const auto Geometry = Collection.GetGeometryCollection();
	const bool bExactCollision = Record.CollisionStorageVersion == 1;
	if (Collection.bOptimizeConvexes || Collection.bImportCollisionFromSource != bExactCollision ||
		!Geometry || !Geometry->Transform.IsValidIndex(Record.RootTransform) ||
		Geometry->Transform.Num() != Record.PieceCount + 1 || Geometry->Children[Record.RootTransform].Num() != Record.PieceCount ||
		Record.LeafTransforms.Num() != Record.PieceCount || !DublinFractureBake::HasValidPieceBudget(Record) ||
		!NearVolume(Record.RetainedVolumeCm3, Record.StructuralVolumeCm3))
	{
		return Fail(Error, TEXT("invalid structural record or conservation baseline"));
	}
	const auto* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
	const auto* MassToLocal = Geometry->FindAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
	if (!Implicits || !MassToLocal || Implicits->Num() != Geometry->Transform.Num() ||
		MassToLocal->Num() != Geometry->Transform.Num() || !(*Implicits)[Record.RootTransform]) { return Fail(Error, TEXT("missing cooked structural implicits")); }
	const auto* External = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(
		FGeometryCollection::ExternalCollisionsAttribute, FGeometryCollection::TransformGroup);
	if (bExactCollision && (!External || External->Num() != Geometry->Transform.Num() || !(*External)[Record.RootTransform]))
	{
		return Fail(Error, TEXT("exact collision lacks its persistent authored shapes"));
	}
	const auto* AuthoredCompound = bExactCollision
		? (*External)[Record.RootTransform]->GetObject<Chaos::FImplicitObjectUnion>() : nullptr;
	if (bExactCollision && (!AuthoredCompound || AuthoredCompound->GetObjects().Num() != Record.PieceCount))
	{
		return Fail(Error, TEXT("exact collision lacks its complete authored compound"));
	}
	const auto* Union = (*Implicits)[Record.RootTransform]->GetObject<Chaos::FImplicitObjectUnion>();
	if (!Union || Union->GetObjects().Num() != Record.PieceCount) { return Fail(Error, TEXT("cooked root is not the complete leaf compound")); }
	TArray<FCollisionHull> Leaves, RootHulls;
	double LeafVolume = 0, RootVolume = 0;
	for (int32 Bone : Record.LeafTransforms)
	{
		if (!Geometry->Transform.IsValidIndex(Bone) || Geometry->Parent[Bone] != Record.RootTransform ||
			!Geometry->IsRigid(Bone) || !Geometry->Children[Bone].IsEmpty() || !(*Implicits)[Bone])
		{
			return Fail(Error, TEXT("structural leaves are not rigid root siblings"));
		}
		FTransform LeafToCollection = (*MassToLocal)[Bone] * BoneGlobal(*Geometry, Bone);
		const auto* Hull = RigidConvex((*Implicits)[Bone].GetReference(), LeafToCollection);
		if (!Hull || Hull->NumVertices() < 4 || Hull->NumEdges() < 6 || !FMath::IsFinite(Hull->GetVolume()) || Hull->GetVolume() <= 0)
		{
			return Fail(Error, TEXT("structural leaf lost its positive convex collision"));
		}
		LeafVolume += Hull->GetVolume();
		Leaves.Add(HullView(*Hull, LeafToCollection));
		if (bExactCollision)
		{
			FTransform AuthoredToCollection = BoneGlobal(*Geometry, Bone);
			const auto* Authored = RigidConvex((*External)[Bone].GetReference(), AuthoredToCollection);
			if (!Authored || !SameConvexGeometry(Leaves.Last().Vertices,
				HullView(*Authored, AuthoredToCollection).Vertices))
			{
				return Fail(Error, TEXT("cooked exact leaf differs from its authored convex"));
			}
			FTransform AuthoredPartToCollection = BoneGlobal(*Geometry, Record.RootTransform);
			const auto* AuthoredPart = RigidConvex(
				AuthoredCompound->GetObjects()[Leaves.Num() - 1].GetReference(), AuthoredPartToCollection);
			if (!AuthoredPart || !SameConvexGeometry(Leaves.Last().Vertices,
				HullView(*AuthoredPart, AuthoredPartToCollection).Vertices))
			{
				return Fail(Error, TEXT("authored exact compound differs from its declared leaf order"));
			}
		}
	}
	const FTransform RootToCollection = (*MassToLocal)[Record.RootTransform] * BoneGlobal(*Geometry, Record.RootTransform);
	int32 RootPart = 0;
	for (const auto& Implicit : Union->GetObjects())
	{
		FTransform PartToCollection = RootToCollection;
		const auto* Hull = RigidConvex(Implicit.GetReference(), PartToCollection);
		if (!Hull || Hull->NumVertices() < 4 || Hull->NumEdges() < 6 ||
			!FMath::IsFinite(Hull->GetVolume()) || Hull->GetVolume() <= 0)
		{
			FString Detail = FString::Printf(TEXT("root compound has invalid constituent source=%s part=%d type=%s vertices=%d edges=%d planes=%d storedVolume=%.17g rootToCollection=%s"),
				*Record.SourceId, RootPart, Implicit ? *Implicit->GetTypeName().ToString() : TEXT("null"),
				Hull ? Hull->NumVertices() : 0, Hull ? Hull->NumEdges() : 0, Hull ? Hull->NumPlanes() : 0,
				Hull ? static_cast<double>(Hull->GetVolume()) : 0, *RootToCollection.ToString());
			if (Hull)
			{
				const auto View = HullView(*Hull, PartToCollection);
				Detail += FString::Printf(TEXT(" doubleHullVolume=%.17g vertices["), UE::Geometry::FConvexHull3d::ComputeVolume(MakeArrayView(View.Vertices)));
				for (const FVector& P : View.Vertices) { Detail += FString::Printf(TEXT("(%.17g,%.17g,%.17g),"), P.X, P.Y, P.Z); }
				Detail += TEXT("]");
			}
			return Fail(Error, Detail);
		}
		RootVolume += Hull->GetVolume();
		RootHulls.Add(HullView(*Hull, PartToCollection));
		++RootPart;
	}
	if (!NearVolume(LeafVolume, Record.StructuralVolumeCm3) || !NearVolume(RootVolume, Record.StructuralVolumeCm3))
	{
		return Fail(Error, TEXT("cooked collision volume differs from structural solid"));
	}
	TSet<int32> Matched;
	for (int32 LeafIndex = 0; LeafIndex < Leaves.Num(); ++LeafIndex)
	{
		const FCollisionHull& Leaf = Leaves[LeafIndex];
		int32 Match = INDEX_NONE;
		for (int32 I = 0; I < RootHulls.Num(); ++I)
		{
			if (Matched.Contains(I) || !Leaf.Bounds.Min.Equals(RootHulls[I].Bounds.Min, ContactToleranceCm) ||
				!Leaf.Bounds.Max.Equals(RootHulls[I].Bounds.Max, ContactToleranceCm)) { continue; }
			if (SameConvexGeometry(Leaf.Vertices, RootHulls[I].Vertices)) { Match = I; break; }
		}
		if (Match == INDEX_NONE)
		{
			const int32 Bone = Record.LeafTransforms[LeafIndex];
			FString Detail = FString::Printf(TEXT("root compound changed or omitted a child hull [StructuralHullMatch] source=%s root=%d leaf=%d leafOrdinal=%d rootType=%s leafType=%s rootParts=%d matched=%d toleranceCm=%.9g\nrootBoneGlobal=%s rootMassToLocal=%s rootToCollection=%s\nleafBoneGlobal=%s leafMassToLocal=%s leafToCollection=%s"),
				*Record.SourceId, Record.RootTransform, Bone, LeafIndex, *Union->GetTypeName().ToString(), *Leaf.Hull->GetTypeName().ToString(),
				RootHulls.Num(), Matched.Num(), ContactToleranceCm,
				*BoneGlobal(*Geometry, Record.RootTransform).ToString(), *(*MassToLocal)[Record.RootTransform].ToString(), *RootToCollection.ToString(),
				*BoneGlobal(*Geometry, Bone).ToString(), *(*MassToLocal)[Bone].ToString(), *Leaf.ToCollection.ToString());
			TArray<int32> Closest;
			for (int32 I = 0; I < RootHulls.Num(); ++I) { Closest.Add(I); }
			Closest.Sort([&](int32 A, int32 B) { return BoundsDelta(Leaf, RootHulls[A]) < BoundsDelta(Leaf, RootHulls[B]); });
			for (int32 I = 0; I < FMath::Min(3, Closest.Num()); ++I)
			{
				const int32 Part = Closest[I];
				Detail += FString::Printf(TEXT("\ncookedRootPart=%d alreadyMatched=%d %s"),
					Part, Matched.Contains(Part), *DescribeHullPair(Leaf, RootHulls[Part]));
			}
			const auto* Indices = Geometry->FindAttribute<TSet<int32>>(TEXT("TransformToConvexIndices"), FGeometryCollection::TransformGroup);
			const auto* Authored = Geometry->FindAttribute<Chaos::FConvexPtr>(FGeometryCollection::ConvexHullAttribute, FGeometryCollection::ConvexGroup);
			if (Indices && Authored && Indices->IsValidIndex(Bone) && Indices->IsValidIndex(Record.RootTransform))
			{
				for (int32 LeafHullIndex : (*Indices)[Bone])
				{
					if (!Authored->IsValidIndex(LeafHullIndex) || !(*Authored)[LeafHullIndex]) { continue; }
					const FCollisionHull AuthoredLeaf = HullView(*(*Authored)[LeafHullIndex], BoneGlobal(*Geometry, Bone));
					Detail += FString::Printf(TEXT("\nauthoredLeafHull=%d vsCookedLeaf %s"), LeafHullIndex, *DescribeHullPair(AuthoredLeaf, Leaf));
					int32 Nearest = INDEX_NONE;
					double Delta = TNumericLimits<double>::Max();
					for (int32 RootHullIndex : (*Indices)[Record.RootTransform])
					{
						if (!Authored->IsValidIndex(RootHullIndex) || !(*Authored)[RootHullIndex]) { continue; }
						const FCollisionHull AuthoredRoot = HullView(*(*Authored)[RootHullIndex], BoneGlobal(*Geometry, Record.RootTransform));
						const double D = BoundsDelta(AuthoredLeaf, AuthoredRoot);
						if (D < Delta) { Delta = D; Nearest = RootHullIndex; }
					}
					if (Nearest != INDEX_NONE)
					{
						const FCollisionHull AuthoredRoot = HullView(*(*Authored)[Nearest], BoneGlobal(*Geometry, Record.RootTransform));
						Detail += FString::Printf(TEXT("\nauthoredRootHull=%d vsAuthoredLeaf %s"), Nearest, *DescribeHullPair(AuthoredLeaf, AuthoredRoot));
					}
				}
			}
			return Fail(Error, Detail);
		}
		Matched.Add(Match);
	}
	if (!NoOverlaps(Leaves, Error) || !NoOverlaps(RootHulls, Error)) { return false; }
	const GeometryCollection::Facades::FCollectionConnectionGraphFacade Graph(*Geometry);
	if (!Graph.IsValid() || !Graph.HasValidConnections()) { return Fail(Error, TEXT("invalid structural contact graph")); }
	TSet<int32> Reached;
	for (int32 Anchor : Record.Anchors) { Reached.Add(Anchor); }
	bool bChanged = true;
	while (bChanged)
	{
		bChanged = false;
		for (int32 I = 0; I < Graph.NumConnections(); ++I)
		{
			const auto Edge = Graph.GetConnection(I);
			if (!Record.LeafTransforms.Contains(Edge.Key) || !Record.LeafTransforms.Contains(Edge.Value)) { return Fail(Error, TEXT("graph edge is not between structural siblings")); }
			if (Reached.Contains(Edge.Key) != Reached.Contains(Edge.Value))
			{
				Reached.Add(Edge.Key); Reached.Add(Edge.Value); bChanged = true;
			}
		}
	}
	if (Reached.Num() != Record.PieceCount) { return Fail(Error, TEXT("structural contact graph has unsupported islands")); }
	Error.Reset();
	return true;
}

bool DublinStructural::ValidateBuildingCollision(const UGeometryCollection& Collection, const FDublinCityBuilding& Building,
	const FDublinFractureRecord& Record, FString& Error)
{
	const bool bDetail = DublinFractureBake::IsDetailedRecipe(Record.Recipe);
	FAssembly Assembly;
	if (bDetail)
	{
		if (!DublinFractureBake::IsCurrentRecord(Building, Record)) { return Fail(Error, TEXT("stale city-detail probe/source identity")); }
	}
	else
	{
		if (!BuildAssembly(Building, Assembly, Error)) { return false; }
		if (!NearVolume(Record.StructuralVolumeCm3, Assembly.StructuralVolumeCm3)) { return Fail(Error, TEXT("structural volume no longer matches source-derived assembly")); }
	}
	const auto Geometry = Collection.GetGeometryCollection();
	if (!Geometry) { return Fail(Error, TEXT("missing geometry")); }
	const auto* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
	const auto* Mass = Geometry->FindAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
	if (!Implicits || !Mass || !Geometry->Transform.IsValidIndex(Record.RootTransform) ||
		!Implicits->IsValidIndex(Record.RootTransform) || !Mass->IsValidIndex(Record.RootTransform) ||
		!(*Implicits)[Record.RootTransform]) { return Fail(Error, TEXT("missing root implicit")); }
	for (int32 Bone : Record.LeafTransforms)
	{
		if (!Geometry->Transform.IsValidIndex(Bone) || !Implicits->IsValidIndex(Bone) || !Mass->IsValidIndex(Bone) ||
			!(*Implicits)[Bone] || (*Mass)[Bone].ContainsNaN())
		{
			return Fail(Error, TEXT("cannot probe a missing structural leaf"));
		}
	}
	for (bool bRoot : {true, false})
	{
		const TArray<int32> Bones = bRoot ? TArray<int32>{Record.RootTransform} : Record.LeafTransforms;
		const auto Hit = [&](const FVector& A, const FVector& B)
		{
			for (int32 Bone : Bones)
			{
				if (Ray(*(*Implicits)[Bone], (*Mass)[Bone] * BoneGlobal(*Geometry, Bone), A, B)) { return true; }
			}
			return false;
		};
		const auto Occupied = [&](const FVector& P)
		{
			for (int32 Bone : Bones)
			{
				const FVector Local = ((*Mass)[Bone] * BoneGlobal(*Geometry, Bone)).InverseTransformPosition(P);
				if ((*Implicits)[Bone]->Overlap(Local, 0)) { return true; }
			}
			return false;
		};
		if (bDetail ? !ProbeRecord(Record, Hit, Occupied, Error) : !ProbeRooms(Assembly, Hit, Occupied, Error)) { return false; }
	}
	Error.Reset();
	return true;
}

bool DublinStructural::ValidateRegisteredCollision(UGeometryCollectionComponent& Component, const FDublinCityBuilding& Building,
	const FDublinFractureRecord& Record, FString& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_RegisteredLiveProbes);
	const auto* Proxy = Component.GetPhysicsProxy();
	const auto* Root = Proxy ? Proxy->GetInitialRootParticle_External() : nullptr;
	const UGeometryCollection* Asset = Component.GetRestCollection();
	if (!Root || !Root->GetGeometry() || !Asset || !Component.IsPhysicsStateCreated()) { return Fail(Error, TEXT("registered root has no physics geometry")); }
	FAssembly Assembly;
	const bool bDetail = DublinFractureBake::IsDetailedRecipe(Record.Recipe);
	if (bDetail)
	{
		if (!DublinFractureBake::IsCurrentRecord(Building, Record)) { return Fail(Error, TEXT("registered city-detail source identity is stale")); }
	}
	else if (!BuildAssembly(Building, Assembly, Error)) { return false; }
	const auto Geometry = Asset->GetGeometryCollection();
	const auto* Mass = Geometry ? Geometry->FindAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup) : nullptr;
	if (!Mass || !Mass->IsValidIndex(Record.RootTransform)) { return Fail(Error, TEXT("registered root has no mass transform")); }
	const FTransform ToWorld = Component.GetComponentTransform();
	FRegisteredProbeBatch Batch(Component);
	const FTransform RootToWorld(Root->R(), Root->X());
	TMap<FVector, bool> VoidResults;
	const auto HitRoot = [&](const FVector& A, const FVector& B)
	{
		const FVector Start = RootToWorld.InverseTransformPosition(ToWorld.TransformPosition(A));
		const FVector Delta = RootToWorld.InverseTransformPosition(ToWorld.TransformPosition(B)) - Start;
		Chaos::FReal Time; Chaos::FVec3 Position, Normal; int32 Face;
		return Batch.Queries.Raycast(*Root->GetGeometry(), Start, Delta.GetSafeNormal(), Delta.Size(), 0, Time, Position, Normal, Face);
	};
	const auto Occupied = [&](const FVector& P)
	{
		if (const bool* Cached = VoidResults.Find(P)) { return *Cached; }
		const bool Result = Batch.Queries.Overlap(*Root->GetGeometry(),
			RootToWorld.InverseTransformPosition(ToWorld.TransformPosition(P)), 0);
		VoidResults.Add(P, Result);
		return Result;
	};
	if (bDetail ? !ProbeRecord(Record, HitRoot, Occupied, Error) : !ProbeRooms(Assembly, HitRoot, Occupied, Error)) { return false; }
	const auto HitRegistered = [&](const FVector& A, const FVector& B)
		{
			return Batch.Ray(ToWorld.TransformPosition(A), ToWorld.TransformPosition(B));
		};
	return bDetail ? ProbeRecord(Record, HitRegistered, Occupied, Error) : ProbeRooms(Assembly, HitRegistered, Occupied, Error);
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
bool DublinStructural::ValidateRegisteredProbeEquivalence(UGeometryCollectionComponent& Component,
	const FDublinFractureRecord& Record, FString& Error, FProbeEquivalenceStats* Stats)
{
	struct FTestRay
	{
		FVector A, B;
		int32 Expected = -1;
		bool Raw = false, Registered = false;
		Chaos::FReal RawDistance = 0;
		float RegisteredDistance = 0;
	};
	struct FTestPoint { FVector World; bool Occupied = false; };
	const FTransform ToWorld = Component.GetComponentTransform();
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinStructuralProbeEquivalence), false);
	const auto* Proxy = Component.GetPhysicsProxy();
	const auto* Root = Proxy ? Proxy->GetInitialRootParticle_External() : nullptr;
	if (!Root || !Root->GetGeometry()) { return Fail(Error, TEXT("equivalence requires actual registered root geometry")); }
	TArray<FTestRay> Rays;
	TArray<FTestPoint> Points;
	for (const FDublinFractureProbe& Probe : Record.CollisionProbes)
	{
		Rays.Add({ToWorld.TransformPosition(Probe.StartCm), ToWorld.TransformPosition(Probe.EndCm), Probe.bExpectHit ? 1 : 0});
	}
	for (const FVector& P : Record.VoidSamplesCm) { Points.Add({ToWorld.TransformPosition(P)}); }
	double ReferenceMs = 0;
	{
		const TArray<Chaos::FPhysicsObjectHandle> Objects = Component.GetAllPhysicsObjects();
		auto Lock = FPhysicsObjectExternalInterface::LockRead(Objects);
		const FTransform RootToWorld(Root->R(), Root->X());
		const auto Bounds = Root->GetGeometry()->BoundingBox();
		const FVector Center = FVector(Bounds.Center());
		const FVector Outside = FVector(Bounds.Max()) + FVector(100);
		Rays.Add({RootToWorld.TransformPosition(Outside), RootToWorld.TransformPosition(Outside + FVector(100, 0, 0)), 0});
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (double Offset : {-0.02, -0.005, 0.0, 0.005, 0.02})
			{
				FVector A = Center, B = Center;
				const int32 Along = (Axis + 1) % 3;
				A[Axis] = B[Axis] = Bounds.Max()[Axis] + Offset;
				A[Along] = Bounds.Min()[Along] - 10;
				B[Along] = Bounds.Max()[Along] + 10;
				Rays.Add({RootToWorld.TransformPosition(A), RootToWorld.TransformPosition(B)});
				Rays.Add({RootToWorld.TransformPosition(B), RootToWorld.TransformPosition(A)});
				Points.Add({RootToWorld.TransformPosition((A + B) * .5)});
			}
		}
		for (int32 I = 0; I < FMath::Min(16, Record.CollisionProbes.Num()); ++I)
		{
			Rays.Add({Rays[I].B, Rays[I].A});
			Points.Add({Rays[I].A}); Points.Add({Rays[I].B});
		}
		const double Start = FPlatformTime::Seconds();
		for (FTestRay& RayCase : Rays)
		{
			const FVector A = RootToWorld.InverseTransformPosition(RayCase.A);
			const FVector Delta = RootToWorld.InverseTransformPosition(RayCase.B) - A;
			Chaos::FVec3 P, N; int32 Face;
			RayCase.Raw = Root->GetGeometry()->Raycast(A, Delta.GetSafeNormal(), Delta.Size(), 0, RayCase.RawDistance, P, N, Face);
		}
		for (FTestPoint& P : Points) { P.Occupied = Root->GetGeometry()->Overlap(RootToWorld.InverseTransformPosition(P.World), 0); }
		ReferenceMs += (FPlatformTime::Seconds() - Start) * 1000;
	}
	const double RegisteredReferenceStart = FPlatformTime::Seconds();
	for (FTestRay& RayCase : Rays)
	{
		FHitResult Hit;
		RayCase.Registered = Component.LineTraceComponent(Hit, RayCase.A, RayCase.B, Query);
		RayCase.RegisteredDistance = Hit.Distance;
	}
	ReferenceMs += (FPlatformTime::Seconds() - RegisteredReferenceStart) * 1000;
	const double BatchStart = FPlatformTime::Seconds();
	FRegisteredProbeBatch Batch(Component);
	const FTransform RootToWorld(Root->R(), Root->X());
	for (int32 I = 0; I < Rays.Num(); ++I)
	{
		const FTestRay& R = Rays[I];
		const FVector A = RootToWorld.InverseTransformPosition(R.A);
		const FVector Delta = RootToWorld.InverseTransformPosition(R.B) - A;
		Chaos::FReal Distance = 0; Chaos::FVec3 P, N; int32 Face;
		const bool Raw = Batch.Queries.Raycast(*Root->GetGeometry(), A, Delta.GetSafeNormal(), Delta.Size(), 0, Distance, P, N, Face);
		float RegisteredDistance = 0;
		const bool Registered = Batch.Ray(R.A, R.B, &RegisteredDistance);
		if (Raw != R.Raw || Registered != R.Registered || (Raw && Distance != R.RawDistance) ||
			(Registered && RegisteredDistance != R.RegisteredDistance) ||
			(R.Expected >= 0 && (Raw != bool(R.Expected) || Registered != bool(R.Expected))))
		{
			return Fail(Error, FString::Printf(TEXT("BVH/native probe %d mismatch raw=%d/%d registered=%d/%d distance=%.17g/%.17g registeredDistance=%.9g/%.9g expected=%d"),
				I, Raw, R.Raw, Registered, R.Registered, Distance, R.RawDistance, RegisteredDistance, R.RegisteredDistance, R.Expected));
		}
	}
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		if (Batch.Queries.Overlap(*Root->GetGeometry(), RootToWorld.InverseTransformPosition(Points[I].World), 0) != Points[I].Occupied)
		{
			return Fail(Error, FString::Printf(TEXT("BVH/native point-overlap %d mismatch"), I));
		}
	}
	// Non-union shapes and nonzero thickness must explicitly retain their native query path.
	if (const auto* Union = Root->GetGeometry()->AsA<Chaos::FImplicitObjectUnion>(); Union && !Union->GetObjects().IsEmpty())
	{
		const auto& Child = *Union->GetObjects()[0];
		const auto Bounds = Child.BoundingBox();
		const FVector A = FVector(Bounds.Center()) + FVector(0, 0, Bounds.Extents().Z + 10);
		const FVector B = FVector(Bounds.Center()) - FVector(0, 0, Bounds.Extents().Z + 10);
		const FVector Delta = B - A;
		for (double Thickness : {0.0, 0.5})
		{
			Chaos::FReal T1 = 0, T2 = 0; Chaos::FVec3 P, N; int32 Face;
			const bool Native = Child.Raycast(A, Delta.GetSafeNormal(), Delta.Size(), Thickness, T1, P, N, Face);
			const bool Fast = Batch.Queries.Raycast(Child, A, Delta.GetSafeNormal(), Delta.Size(), Thickness, T2, P, N, Face);
			if (Native != Fast || (Native && T1 != T2) ||
				Child.Overlap(A, Thickness) != Batch.Queries.Overlap(Child, A, Thickness))
			{
				return Fail(Error, TEXT("unsupported-shape/thickness native fallback changed semantics"));
			}
		}
	}
	const double BatchMs = (FPlatformTime::Seconds() - BatchStart) * 1000;
	if (Stats)
	{
		Stats->PossibleCandidates = Batch.Queries.Counts.PossibleCandidates;
		Stats->NarrowphaseCandidates = Batch.Queries.Counts.NarrowphaseCandidates;
		Stats->FallbackQueries = Batch.Queries.Counts.FallbackQueries;
		Stats->ReferenceMilliseconds = ReferenceMs;
		Stats->AcceleratedMilliseconds = BatchMs;
	}
	const auto& RootContext = Batch.Queries.Context(*Root->GetGeometry());
	UE_LOG(LogTemp, Display, TEXT("BVHProbeEquivalence source=%s rays=%d points=%d nativeMs=%.3f acceleratedMs=%.3f narrowCandidates=%llu possibleCandidates=%llu fallbackQueries=%llu localBVHs=%llu rootCoverage=%s; diagnostic only"),
		*Record.SourceId, Rays.Num(), Points.Num(), ReferenceMs, BatchMs, Batch.Queries.Counts.NarrowphaseCandidates,
		Batch.Queries.Counts.PossibleCandidates, Batch.Queries.Counts.FallbackQueries, Batch.Queries.Counts.LocalBVHs, RootContext.Reason);
	Error.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStructuralConvexIdentityTest,
	"DublinFlight.Destruction.StructuralPilot.ConvexVertexIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStructuralConvexIdentityTest::RunTest(const FString& Parameters)
{
	const TArray<FVector> Original{
		FVector(100, -500, 1400), FVector(200, -500, 1400), FVector(100, -300, 1400), FVector(200, -300, 1400),
		FVector(100, -500, 1680), FVector(200, -500, 1680), FVector(100, -300, 1680), FVector(200, -300, 1680)};
	TArray<FVector> Rounded;
	for (int32 I = Original.Num() - 1; I >= 0; --I)
	{
		Rounded.Add(Original[I] + FVector(2.e-5, -1.e-5, 1.e-5));
	}
	TestTrue(TEXT("Reordered vertices with captured-scale cook rounding identify the same convex geometry"),
		SameConvexGeometry(Original, Rounded));
	TestTrue(TEXT("Identity is symmetric"), SameConvexGeometry(Rounded, Original));
	TArray<FVector> Redundant = Original;
	Redundant.Append({FVector(150, -400, 1400), FVector(150, -500, 1400), FVector(150, -400, 1540), Original[0]});
	TestTrue(TEXT("Coplanar face, edge, interior and duplicate vertices do not change the convex set"),
		SameConvexGeometry(Original, Redundant));
	TestTrue(TEXT("Retessellated convex-set identity is symmetric"), SameConvexGeometry(Redundant, Original));
	TArray<FVector> DiagonalShift;
	for (const FVector& V : Original) { DiagonalShift.Add(V + FVector(.008, .008, .008)); }
	TestFalse(TEXT("Euclidean corner distance over 0.01cm fails even when every axis/plane shift is under tolerance"),
		SameConvexGeometry(Original, DiagonalShift));
	TArray<FVector> Changed = Original;
	Changed[0] += FVector(.02, .02, .02);
	TestTrue(TEXT("Shape corruption fixture retains identical AABB"),
		FBox(Original).Min == FBox(Changed).Min && FBox(Original).Max == FBox(Changed).Max);
	TestFalse(TEXT("Changed corner beyond unchanged 0.01cm tolerance is rejected despite identical bounds"),
		SameConvexGeometry(Original, Changed));
	Changed = Original;
	Changed.RemoveAt(0);
	TestFalse(TEXT("Missing extreme vertex is rejected"), SameConvexGeometry(Original, Changed));
	TestFalse(TEXT("Subset cannot match in the other direction"), SameConvexGeometry(Changed, Original));
	Changed = Original;
	Changed.Add(FVector(150, -400, 1800));
	TestFalse(TEXT("Additional extreme vertex is rejected"), SameConvexGeometry(Original, Changed));
	TestFalse(TEXT("Empty vertex set cannot pass vacuously"), SameConvexGeometry({}, {}));
	const TArray<FVector> Planar{Original[0], Original[1], Original[2], Original[3]};
	TestFalse(TEXT("Zero-volume point sets are not accepted as solid convex geometry"), SameConvexGeometry(Planar, Planar));
	return true;
}
#endif
