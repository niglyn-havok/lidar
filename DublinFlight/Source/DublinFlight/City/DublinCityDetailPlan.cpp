#include "City/DublinCityDetail.h"

#if WITH_EDITOR
#include "City/DublinCityDestruction.h"
#include "Algo/Reverse.h"
#include "CompGeom/Delaunay2.h"
#include "CompGeom/ExactPredicates.h"
#include "Curve/PolygonIntersectionUtils.h"
#include "Curve/PolygonOffsetUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDublinCityDetail, Log, All);

namespace
{
	using namespace UE::Geometry;
	using FPolygons = TArray<FGeneralPolygon2d>;

	struct FPatch
	{
		FPolygons Domain;
		FVector Height = FVector::ZeroVector;
	};

	bool Fail(FString& Error, const FString& Text)
	{
		Error = TEXT("CityDetail: ") + Text;
		return false;
	}

	double Cross(const FVector2D& A, const FVector2D& B) { return A.X * B.Y - A.Y * B.X; }
	double PolygonArea(const TArray<FVector2D>& Points)
	{
		double Result = 0;
		for (int32 I = 1; I + 1 < Points.Num(); ++I) { Result += ExactPredicates::Orient2<double>(Points[0], Points[I], Points[I + 1]); }
		return Result * .5;
	}
	double ZAt(const FVector& Height, const FVector2D& P) { return Height.X * P.X + Height.Y * P.Y + Height.Z; }
	double RoofDepth(const FVector& Height) { return 20 * FMath::Sqrt(1 + Height.X * Height.X + Height.Y * Height.Y); }
	double Area(const FPolygons& Polygons)
	{
		double Sum = 0;
		for (const FGeneralPolygon2d& P : Polygons) { Sum += P.SignedArea(); }
		return Sum;
	}

	FGeneralPolygon2d Polygon(TArray<FVector2D> Points)
	{
		if (PolygonArea(Points) < 0) { Algo::Reverse(Points); }
		FPolygon2d Boundary;
		for (const FVector2D& P : Points) { Boundary.AppendVertex(P); }
		return FGeneralPolygon2d(Boundary);
	}

	bool Surface(const FDublinCityBuilding& Building, int32 Material, FPolygons& Domain, TArray<FPatch>& Patches,
		double& MinZ, double& MaxZ, FString& Error)
	{
		TArray<FVector2D> Points;
		TMap<FVector2D, int32> PointIds;
		TMap<FIntPoint, int32> Edges;
		TMap<FVector2D, double> Heights;
		double TriangleArea = 0;
		MinZ = TNumericLimits<double>::Max();
		MaxZ = -MinZ;
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			if (Building.MaterialIds[T] != Material) { continue; }
			FVector V[3];
			TArray<FVector2D> XY;
			int32 Ids[3];
			for (int32 K = 0; K < 3; ++K)
			{
				V[K] = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + K]];
				XY.Emplace(V[K].X, V[K].Y);
				if (const double* Existing = Heights.Find(XY.Last()))
				{
					if (!FMath::IsNearlyEqual(*Existing, V[K].Z, 1.e-6)) { return Fail(Error, TEXT("surface is not a single-valued height field")); }
				}
				Heights.Add(XY.Last(), V[K].Z);
				const int32* ExistingId = PointIds.Find(XY.Last());
				if (ExistingId) { Ids[K] = *ExistingId; }
				else { Ids[K] = Points.Add(XY.Last()); PointIds.Add(XY.Last(), Ids[K]); }
				MinZ = FMath::Min(MinZ, V[K].Z);
				MaxZ = FMath::Max(MaxZ, V[K].Z);
			}
			const double Signed = Cross(XY[1] - XY[0], XY[2] - XY[0]);
			if ((Material == 0 && Signed >= 0) || (Material == 2 && Signed <= 0)) { return Fail(Error, TEXT("invalid projected surface winding")); }
			TriangleArea += FMath::Abs(Signed) * .5;
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A = Ids[K], B = Ids[(K + 1) % 3];
				++Edges.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
			}
			const FVector N = FVector::CrossProduct(V[1] - V[0], V[2] - V[0]);
			const FVector Height(-N.X / N.Z, -N.Y / N.Z, FVector::DotProduct(N, V[0]) / N.Z);
			int32 PatchIndex = Patches.IndexOfByPredicate([&](const FPatch& P) { return P.Height.Equals(Height, 1.e-10); });
			if (PatchIndex == INDEX_NONE) { PatchIndex = Patches.AddDefaulted(); Patches[PatchIndex].Height = Height; }
			Patches[PatchIndex].Domain.Add(Polygon(MoveTemp(XY)));
		}
		TMap<int32, TArray<int32>> Adjacent;
		for (const auto& E : Edges)
		{
			if (E.Value > 2) { return Fail(Error, TEXT("surface has nonmanifold projected edges")); }
			if (E.Value == 1) { Adjacent.FindOrAdd(E.Key.X).Add(E.Key.Y); Adjacent.FindOrAdd(E.Key.Y).Add(E.Key.X); }
		}
		if (Adjacent.IsEmpty()) { return Fail(Error, TEXT("missing surface boundary")); }
		for (const auto& E : Adjacent) { if (E.Value.Num() != 2) { return Fail(Error, TEXT("surface boundary is not a loop")); } }
		TArray<FPolygon2d> Loops;
		TSet<int32> Visited;
		for (int32 Start = 0; Start < Points.Num(); ++Start)
		{
			if (!Adjacent.Contains(Start) || Visited.Contains(Start)) { continue; }
			FPolygon2d Loop;
			int32 Current = Start, Previous = INDEX_NONE;
			while (!Visited.Contains(Current))
			{
				Visited.Add(Current);
				Loop.AppendVertex(Points[Current]);
				const auto& Next = Adjacent.FindChecked(Current);
				const int32 Target = Next[0] == Previous ? Next[1] : Next[0];
				Previous = Current;
				Current = Target;
			}
			if (Current != Start || Loop.VertexCount() < 3) { return Fail(Error, TEXT("invalid boundary traversal")); }
			Loops.Add(MoveTemp(Loop));
		}
		Loops.Sort([](const FPolygon2d& A, const FPolygon2d& B) { return FMath::Abs(A.SignedArea()) > FMath::Abs(B.SignedArea()); });
		for (FPolygon2d& Loop : Loops)
		{
			int32 Owner = Domain.IndexOfByPredicate([&](const FGeneralPolygon2d& P) { return P.GetOuter().Contains(Loop); });
			if (Owner == INDEX_NONE)
			{
				if (Loop.IsClockwise()) { Loop.Reverse(); }
				Domain.Emplace(Loop);
			}
			else
			{
				if (!Loop.IsClockwise()) { Loop.Reverse(); }
				if (!Domain[Owner].AddHole(Loop, true, true)) { return Fail(Error, TEXT("intersecting or improperly nested courtyard boundary")); }
			}
		}
		if (FMath::Abs(Area(Domain) - TriangleArea) > FMath::Max(.01, TriangleArea * 1.e-8))
		{
			return Fail(Error, TEXT("source surface coverage does not equal its boundary domain"));
		}
		FPolygons AllTriangles;
		for (FPatch& Patch : Patches)
		{
			AllTriangles.Append(Patch.Domain);
			FPolygons Combined;
			if (!PolygonsUnion(Patch.Domain, Combined, false)) { return Fail(Error, TEXT("native roof patch union failed")); }
			Patch.Domain = MoveTemp(Combined);
		}
		FPolygons Covered;
		if (!PolygonsUnion(AllTriangles, Covered, false) ||
			FMath::Abs(Area(Covered) - TriangleArea) > FMath::Max(.01, TriangleArea * 1.e-8))
		{
			return Fail(Error, TEXT("source triangles overlap or native surface union failed"));
		}
		return true;
	}

	bool ValidateFilledTriangles(const FGeneralPolygon2d& P, TArray<FIndex3i>& Triangles,
		const TArray<FVector2d>& Vertices, FString& Error)
	{
		double Covered = 0;
		for (int32 I = 0; I < Triangles.Num();)
		{
			const FIndex3i T = Triangles[I];
			if (!Vertices.IsValidIndex(T.A) || !Vertices.IsValidIndex(T.B) || !Vertices.IsValidIndex(T.C) ||
				T.A == T.B || T.B == T.C || T.C == T.A)
			{
				return Fail(Error, FString::Printf(TEXT("invalid native filled triangle (%d,%d,%d), returned vertices=%d"),
					T.A, T.B, T.C, Vertices.Num()));
			}
			const double Signed = ExactPredicates::Orient2<double>(Vertices[T.A], Vertices[T.B], Vertices[T.C]);
			if (!FMath::IsFinite(Signed)) { return Fail(Error, TEXT("nonfinite native filled triangle")); }
			if (Signed == 0)
			{
				// Only an exact predicate can establish zero coverage. Boundary vertices are retained below.
				Triangles.RemoveAt(I);
				continue;
			}
			Covered += FMath::Abs(Signed) * .5;
			++I;
		}
		return (!Triangles.IsEmpty() && FMath::Abs(Covered - P.SignedArea()) <= FMath::Max(.01, Covered * 1.e-8)) ||
			Fail(Error, TEXT("native filled triangles do not conserve the complete polygon-with-holes area"));
	}

	bool TriangulateDomain(const FGeneralPolygon2d& P, TArray<FIndex3i>& Triangles,
		TArray<FVector2d>& Vertices, FString& Error)
	{
		Vertices.Reset(); Triangles.Reset();
		TArray<FIndex2i> Edges;
		const auto AppendLoop = [&](const FPolygon2d& Loop, bool bHole)
		{
			TArray<FVector2d> LoopVertices;
			for (const FVector2d& V : Loop.GetVertices())
			{
				if (!FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y)) { return Fail(Error, TEXT("nonfinite native constraint endpoint")); }
				if (LoopVertices.IsEmpty() || LoopVertices.Last() != V) { LoopVertices.Add(V); }
			}
			// Native constraints close the loop themselves; repeated endpoints create an invalid self-edge.
			if (LoopVertices.Num() > 1 && LoopVertices.Last() == LoopVertices[0]) { LoopVertices.Pop(); }
			if (LoopVertices.Num() < 3 || Loop.IsClockwise() != (bHole ? !P.OuterIsClockwise() : P.OuterIsClockwise()))
			{
				return Fail(Error, TEXT("degenerate or inconsistently wound native boundary/hole constraint"));
			}
			const int32 Start = Vertices.Num();
			Vertices.Append(LoopVertices);
			for (int32 I = 0; I < LoopVertices.Num(); ++I)
			{
				Edges.Emplace(Start + I, Start + (I + 1) % LoopVertices.Num());
			}
			return true;
		};
		if (!AppendLoop(P.GetOuter(), false)) { return false; }
		for (const FPolygon2d& Hole : P.GetHoles()) { if (!AppendLoop(Hole, true)) { return false; } }
		FDelaunay2 Triangulation;
		Triangulation.bAutomaticallyFixEdgesToDuplicateVertices = true;
		if (!Triangulation.Triangulate(MakeArrayView(Vertices), MakeArrayView(Edges)))
		{
			return Fail(Error, TEXT("native hole-aware constrained triangulation failed"));
		}
		const auto FillMode = P.OuterIsClockwise() ? FDelaunay2::EFillMode::NegativeWinding : FDelaunay2::EFillMode::PositiveWinding;
		if (!Triangulation.GetFilledTriangles(Triangles, MakeArrayView(Edges), FillMode) || Triangles.IsEmpty())
		{
			return Fail(Error, TEXT("native hole-aware winding fill failed or returned no filled triangles"));
		}
		return ValidateFilledTriangles(P, Triangles, Vertices, Error);
	}

	bool ConvexParts(const FPolygons& Domain, TArray<TArray<FVector2D>>& Parts, FString& Error)
	{
		TArray<FVector2D> BoundaryVertices;
		for (const FGeneralPolygon2d& P : Domain)
		{
			TArray<FIndex3i> Triangles;
			TArray<FVector2d> Vertices;
			if (!TriangulateDomain(P, Triangles, Vertices, Error)) { return false; }
			for (const FVector2D& V : Vertices) { BoundaryVertices.AddUnique(V); }
			for (const FIndex3i& T : Triangles)
			{
				TArray<FVector2D> Triangle{Vertices[T.A], Vertices[T.B], Vertices[T.C]};
				if (PolygonArea(Triangle) < 0) { Algo::Reverse(Triangle); }
				if (PolygonArea(Triangle) <= 0) { return Fail(Error, TEXT("native triangulation produced a nonpositive piece")); }
				Parts.Add(MoveTemp(Triangle));
			}
		}
		bool bMerged = true;
		while (bMerged)
		{
			bMerged = false;
			for (int32 I = 0; !bMerged && I < Parts.Num(); ++I)
			{
				for (int32 J = I + 1; !bMerged && J < Parts.Num(); ++J)
				{
					for (int32 A = 0; !bMerged && A < Parts[I].Num(); ++A)
					{
						for (int32 B = 0; !bMerged && B < Parts[J].Num(); ++B)
						{
							const auto& P = Parts[I]; const auto& Q = Parts[J];
							if (P[A] != Q[(B + 1) % Q.Num()] || P[(A + 1) % P.Num()] != Q[B]) { continue; }
							TArray<FVector2D> Joined;
							for (int32 K = 0; K < P.Num(); ++K) { Joined.Add(P[(A + 1 + K) % P.Num()]); }
							for (int32 K = 2; K < Q.Num(); ++K) { Joined.Add(Q[(B + K) % Q.Num()]); }
							bool bConvex = Joined.Num() <= 16;
							for (int32 K = 0; K < Joined.Num(); ++K)
							{
								bConvex &= ExactPredicates::Orient2<double>(Joined[K], Joined[(K + 1) % Joined.Num()],
									Joined[(K + 2) % Joined.Num()]) >= 0;
							}
							const double Expected = PolygonArea(P) + PolygonArea(Q);
							if (bConvex && FMath::Abs(PolygonArea(Joined) - Expected) < FMath::Max(1.e-6, Expected * 1.e-10))
							{
								Parts[I] = MoveTemp(Joined); Parts.RemoveAt(J); bMerged = true;
							}
						}
					}
				}
			}
		}
		double Covered = 0;
		for (auto& P : Parts)
		{
			TArray<FVector2D> Conforming;
			for (int32 I = 0; I < P.Num(); ++I)
			{
				const FVector2D A = P[I], B = P[(I + 1) % P.Num()], Edge = B - A;
				if (Edge.SizeSquared() == 0) { return Fail(Error, TEXT("convex partition created a zero-length edge")); }
				TArray<FVector2D> Splits;
				for (const FVector2D& V : BoundaryVertices)
				{
					if (V == A || V == B || ExactPredicates::Orient2<double>(A, B, V) != 0) { continue; }
					const double Along = FVector2D::DotProduct(V - A, Edge);
					if (Along > 0 && Along < Edge.SizeSquared()) { Splits.Add(V); }
				}
				Splits.Sort([&](const FVector2D& L, const FVector2D& R) { return FVector2D::DistSquared(A, L) < FVector2D::DistSquared(A, R); });
				Conforming.Add(A); Conforming.Append(Splits);
			}
			P = MoveTemp(Conforming);
			Covered += PolygonArea(P);
		}
		for (const FVector2D& V : BoundaryVertices)
		{
			if (!Parts.ContainsByPredicate([&](const TArray<FVector2D>& P) { return P.Contains(V); }))
			{
				return Fail(Error, TEXT("convex partition lost a source boundary/seam vertex"));
			}
		}
		return FMath::Abs(Covered - Area(Domain)) <= FMath::Max(.01, Covered * 1.e-8) ||
			Fail(Error, TEXT("convex partition changed domain area"));
	}

	double MemberVolume(const DublinStructural::FMember& M)
	{
		double V = 0;
		for (int32 I = 1; I + 1 < M.Polygon.Num(); ++I)
		{
			const FVector2D P = (M.Polygon[0] + M.Polygon[I] + M.Polygon[I + 1]) / 3;
			const double A = Cross(M.Polygon[I] - M.Polygon[0], M.Polygon[I + 1] - M.Polygon[0]) * .5;
			V += A * (M.MaxZ - M.MinZ + FVector2D::DotProduct(M.TopSlope - M.BottomSlope, P));
		}
		return V;
	}

	bool CoalesceMemberPrecision(const FDublinCityBuilding& Building, DublinDetail::FPlan& Plan, FString& Error)
	{
		constexpr double PrecisionCm = .01;
		TArray<FVector2D> SourceCorners;
		for (const FVector& P : Building.Mesh.VerticesCm) { SourceCorners.AddUnique(FVector2D(P.X, P.Y)); }
		const auto RestoreSourceCorner = [&](FVector2D P)
		{
			double Best = PrecisionCm * PrecisionCm;
			FVector2D Result = P;
			for (const FVector2D& Source : SourceCorners)
			{
				const double Distance = FVector2D::DistSquared(P, Source);
				if (Distance < Best) { Best = Distance; Result = Source; }
			}
			return Result;
		};
		const auto Compact = [&](const DublinStructural::FMember& M, TArray<FVector2D>& Polygon)
		{
			bool bChanged = true;
			while (bChanged && Polygon.Num() >= 3)
			{
				bChanged = false;
				for (int32 I = 0; I < Polygon.Num(); ++I)
				{
					const int32 J = (I + 1) % Polygon.Num();
					const FVector2D A = Polygon[I], B = Polygon[J];
					if (FVector2D::DistSquared(A, B) > PrecisionCm * PrecisionCm) { continue; }
					const FVector3f BottomA(A.X, A.Y, M.MinZ + FVector2D::DotProduct(M.BottomSlope, A));
					const FVector3f BottomB(B.X, B.Y, M.MinZ + FVector2D::DotProduct(M.BottomSlope, B));
					const FVector3f TopA(A.X, A.Y, M.MaxZ + FVector2D::DotProduct(M.TopSlope, A));
					const FVector3f TopB(B.X, B.Y, M.MaxZ + FVector2D::DotProduct(M.TopSlope, B));
					// Match the existing MeshDescription vertex weld, bounded by the interior precision contract.
					if (!BottomA.Equals(BottomB, 1.e-5f) || !TopA.Equals(TopB, 1.e-5f)) { continue; }
					const bool bSourceA = SourceCorners.Contains(A), bSourceB = SourceCorners.Contains(B);
					if (A != B && bSourceA && bSourceB) { continue; }
					Polygon.RemoveAt(bSourceB ? I : J);
					bChanged = true;
					break;
				}
			}
		};
		auto& Members = Plan.Assembly.Members;
		int32 CollapsedSeams = 0, MergedSlivers = 0, RoundoffVertices = 0, ZeroResidues = 0;
		for (auto& M : Members)
		{
			for (FVector2D& P : M.Polygon)
			{
				if (SourceCorners.Contains(P)) { continue; }
				for (const FVector2D& Source : SourceCorners)
				{
					const double Scale = FMath::Max(1.0, FMath::Max(P.GetAbsMax(), Source.GetAbsMax()));
					const double Roundoff = FMath::Min(PrecisionCm, Scale * 64 * 2.2204460492503131e-16);
					if (!P.Equals(Source, Roundoff)) { continue; }
					P = Source; ++RoundoffVertices; break;
				}
			}
		}
		for (int32 I = 0; I < Members.Num();)
		{
			auto& M = Members[I];
			if (PolygonArea(M.Polygon) == 0 && MemberVolume(M) == 0)
			{
				Members.RemoveAt(I); ++ZeroResidues; continue;
			}
			TArray<FVector2D> Compacted = M.Polygon;
			Compact(M, Compacted);
			if (Compacted.Num() < 3 || PolygonArea(Compacted) <= 0 || MemberVolume(M) <= 1.0)
			{
				bool bMerged = false;
				for (int32 J = 0; J < Members.Num() && !bMerged; ++J)
				{
					if (I == J) { continue; }
					const auto& Other = Members[J];
					if (M.MinZ != Other.MinZ || M.MaxZ != Other.MaxZ || M.BottomSlope != Other.BottomSlope ||
						M.TopSlope != Other.TopSlope || M.bWall != Other.bWall) { continue; }
					FPolygons Union;
					if (!PolygonsUnion({Polygon(M.Polygon), Polygon(Other.Polygon)}, Union, false) ||
						Union.Num() != 1 || !Union[0].GetHoles().IsEmpty()) { continue; }
					TArray<FVector2D> Joined = Union[0].GetOuter().GetVertices();
					for (FVector2D& P : Joined) { P = RestoreSourceCorner(P); }
					if (PolygonArea(Joined) < 0) { Algo::Reverse(Joined); }
					Compact(M, Joined);
					if (Joined.Num() < 3) { continue; }
					bool bConvex = true;
					for (int32 K = 0; K < Joined.Num(); ++K)
					{
						bConvex &= ExactPredicates::Orient2<double>(Joined[K], Joined[(K + 1) % Joined.Num()],
							Joined[(K + 2) % Joined.Num()]) >= 0;
					}
					const double Before = MemberVolume(M) + MemberVolume(Other);
					TArray<TArray<FVector2D>> Repartitioned;
					if (bConvex) { Repartitioned.Add(MoveTemp(Joined)); }
					else
					{
						FString PartitionError;
						if (!ConvexParts({Polygon(Joined)}, Repartitioned, PartitionError)) { continue; }
					}
					TArray<DublinStructural::FMember> Replacement;
					double After = 0;
					bool bRepresentable = true;
					for (auto& Part : Repartitioned)
					{
						for (FVector2D& P : Part) { P = RestoreSourceCorner(P); }
						Compact(M, Part);
						DublinStructural::FMember Combined = M;
						Combined.Polygon = MoveTemp(Part);
						const double Volume = MemberVolume(Combined);
						bRepresentable &= Combined.Polygon.Num() >= 3 && Volume > 1.0;
						After += Volume;
						Replacement.Add(MoveTemp(Combined));
					}
					if (!bRepresentable || Replacement.IsEmpty() ||
						FMath::Abs(After - Before) > FMath::Max(1.0, Before * 1.e-4)) { continue; }
					Members.RemoveAt(FMath::Max(I, J)); Members.RemoveAt(FMath::Min(I, J));
					Members.Append(MoveTemp(Replacement));
					++MergedSlivers; bMerged = true;
					break;
				}
				if (!bMerged)
				{
					FString Points;
					for (const FVector2D& P : M.Polygon) { Points += FString::Printf(TEXT(" (%.17g,%.17g)"), P.X, P.Y); }
					return Fail(Error, FString::Printf(TEXT("precision-limited member=%d volumeCm3=%.17g z=(%.17g,%.17g) slopes=(%.17g,%.17g)/(%.17g,%.17g) has no representable coplanar union/repartition; refusing positive-volume deletion points=%s"),
						I, MemberVolume(M), M.MinZ, M.MaxZ, M.BottomSlope.X, M.BottomSlope.Y, M.TopSlope.X, M.TopSlope.Y, *Points));
				}
				I = 0;
				continue;
			}
			if (Compacted.Num() != M.Polygon.Num())
			{
				const double Before = MemberVolume(M);
				DublinStructural::FMember Combined = M;
				Combined.Polygon = MoveTemp(Compacted);
				const double After = MemberVolume(Combined);
				if (After <= 0 || FMath::Abs(After - Before) > FMath::Max(1.0, Before * 1.e-4))
				{
					return Fail(Error, TEXT("coalescing float-weld partition seams changed member volume"));
				}
				CollapsedSeams += M.Polygon.Num() - Combined.Polygon.Num();
				M = MoveTemp(Combined);
			}
			++I;
		}
		double Retained = 0;
		Plan.LargestMemberAreaM2 = 0;
		for (auto& M : Members)
		{
			Retained += MemberVolume(M);
			double Longest = 0, Height = 0;
			for (int32 I = 0; I < M.Polygon.Num(); ++I)
			{
				const FVector2D P = M.Polygon[I], Edge = M.Polygon[(I + 1) % M.Polygon.Num()] - P;
				if (Edge.Size() > Longest) { Longest = Edge.Size(); M.WallTangent = Edge.GetSafeNormal(); }
				Height = FMath::Max(Height, M.MaxZ - M.MinZ + FVector2D::DotProduct(M.TopSlope - M.BottomSlope, P));
			}
			M.Weight = M.bWall ? Longest * Height : PolygonArea(M.Polygon) * FMath::Sqrt(1 + M.TopSlope.SizeSquared());
			Plan.LargestMemberAreaM2 = FMath::Max(Plan.LargestMemberAreaM2, M.Weight / 10000);
		}
		if (FMath::Abs(Retained - Plan.Assembly.StructuralVolumeCm3) > FMath::Max(1.0, Plan.Assembly.StructuralVolumeCm3 * 1.e-4))
		{
			return Fail(Error, TEXT("interior precision consolidation changed total derived structural volume"));
		}
		if (CollapsedSeams || MergedSlivers || RoundoffVertices || ZeroResidues)
		{
			Plan.Reason += FString::Printf(TEXT("; interiorPrecisionCm=0.01 floatWeldSeams=%d compatibleSliverUnions=%d sourceRoundoffVertices=%d exactZeroResidues=%d sourceCornersProtected"),
				CollapsedSeams, MergedSlivers, RoundoffVertices, ZeroResidues);
		}
		return true;
	}

	bool AddMembers(DublinDetail::FPlan& Plan, const FPolygons& Domain, const FVector& Bottom, const FVector& Top,
		bool bWall, FString& Error)
	{
		if (Domain.IsEmpty()) { return true; }
		TArray<TArray<FVector2D>> Parts;
		if (!ConvexParts(Domain, Parts, Error)) { return false; }
		for (auto& P : Parts)
		{
			DublinStructural::FMember M;
			M.Polygon = MoveTemp(P); M.MinZ = Bottom.Z; M.MaxZ = Top.Z;
			M.BottomSlope = FVector2D(Bottom.X, Bottom.Y); M.TopSlope = FVector2D(Top.X, Top.Y);
			M.bWall = bWall; M.bSourcePlaneExterior = true;
			double MaxHeight = 0, Longest = 0;
			for (int32 I = 0; I < M.Polygon.Num(); ++I)
			{
				const double H = ZAt(Top, M.Polygon[I]) - ZAt(Bottom, M.Polygon[I]);
				if (!FMath::IsFinite(H) || H <= 1.e-5) { return Fail(Error, TEXT("member lower/upper surfaces intersect")); }
				MaxHeight = FMath::Max(MaxHeight, H);
				const FVector2D E = M.Polygon[(I + 1) % M.Polygon.Num()] - M.Polygon[I];
				if (E.Size() > Longest) { Longest = E.Size(); M.WallTangent = E.GetSafeNormal(); }
			}
			M.Weight = bWall ? Longest * MaxHeight : PolygonArea(M.Polygon) * FMath::Sqrt(1 + Top.X * Top.X + Top.Y * Top.Y);
			Plan.Assembly.StructuralVolumeCm3 += MemberVolume(M);
			Plan.LargestMemberAreaM2 = FMath::Max(Plan.LargestMemberAreaM2, M.Weight / 10000);
			Plan.Assembly.Members.Add(MoveTemp(M));
		}
		return true;
	}

	bool AddIntersectionMembers(DublinDetail::FPlan& Plan, const FPolygons& A, const FPolygons& B,
		const FVector& Bottom, const FVector& Top, bool bWall, FString& Error)
	{
		FPolygons Domain;
		if (!PolygonsIntersection(A, B, Domain)) { return Fail(Error, TEXT("native member intersection failed")); }
		return AddMembers(Plan, Domain, Bottom, Top, bWall, Error);
	}

	bool InteriorPoint(const FGeneralPolygon2d& P, FVector2D& Point, FString& Error)
	{
		TArray<FIndex3i> Triangles; TArray<FVector2d> Vertices;
		if (!TriangulateDomain(P, Triangles, Vertices, Error)) { return false; }
		double Best = -1;
		for (const auto& T : Triangles)
		{
			const double A = FMath::Abs(Cross(Vertices[T.B] - Vertices[T.A], Vertices[T.C] - Vertices[T.A]));
			if (A > Best) { Best = A; Point = (Vertices[T.A] + Vertices[T.B] + Vertices[T.C]) / 3; }
		}
		return true;
	}

	void AddProbe(DublinDetail::FPlan& Plan, const FVector& A, const FVector& B, bool bHit)
	{
		FDublinFractureProbe P; P.StartCm = A; P.EndCm = B; P.bExpectHit = bHit; Plan.Probes.Add(P);
	}

	bool ValidateSource(const FDublinCityBuilding& B, double& Volume, FString& Error)
	{
		if (B.Mesh.Triangles.IsEmpty() || B.Mesh.Triangles.Num() != B.MaterialIds.Num() * 3 ||
			B.Mesh.UV.Num() != B.Mesh.VerticesCm.Num() ||
			(!B.Mesh.ColorsRGBA.IsEmpty() && B.Mesh.ColorsRGBA.Num() != B.Mesh.VerticesCm.Num()))
		{
			return Fail(Error, TEXT("source arrays are inconsistent"));
		}
		TMap<FVector, int32> VertexIds;
		TMap<FIntPoint, FIntPoint> Edges;
		for (int32 T = 0; T < B.MaterialIds.Num(); ++T)
		{
			FVector V[3]; int32 Id[3];
			if (B.MaterialIds[T] > 2) { return Fail(Error, TEXT("unsupported source material")); }
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 Index = B.Mesh.Triangles[T * 3 + K];
				if (!B.Mesh.VerticesCm.IsValidIndex(Index)) { return Fail(Error, TEXT("invalid source index")); }
				V[K] = B.Mesh.VerticesCm[Index];
				if (V[K].ContainsNaN()) { return Fail(Error, TEXT("nonfinite source vertex")); }
				if (const int32* Existing = VertexIds.Find(V[K])) { Id[K] = *Existing; }
				else { Id[K] = VertexIds.Num(); VertexIds.Add(V[K], Id[K]); }
			}
			if (FVector::CrossProduct(V[2] - V[0], V[1] - V[0]).SizeSquared() < 1.e-12) { return Fail(Error, TEXT("degenerate source face")); }
			Volume += FVector::DotProduct(V[0], FVector::CrossProduct(V[2], V[1])) / 6;
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A = Id[K], C = Id[(K + 1) % 3];
				auto& E = Edges.FindOrAdd(FIntPoint(FMath::Min(A, C), FMath::Max(A, C)), FIntPoint::ZeroValue);
				++E.X; E.Y += A < C ? 1 : -1;
			}
		}
		for (const auto& E : Edges) { if (E.Value != FIntPoint(2, 0)) { return Fail(Error, TEXT("source is not a closed consistently wound surface")); } }
		return (FMath::IsFinite(Volume) && Volume > 0) || Fail(Error, TEXT("source volume is not positive in UE winding"));
	}
}

bool DublinDetail::PartitionFaceRemainder(const TArray<FVector2D>& Boundary,
	const TArray<TArray<FVector2D>>& ExteriorPatches, TArray<TArray<FVector2D>>& InteriorPatches, FString& Error,
	FString* CoverageDiagnostics)
{
	InteriorPatches.Reset();
	const FPolygons Face{Polygon(Boundary)};
	FPolygons Source;
	double ExteriorArea = 0;
	for (const auto& Patch : ExteriorPatches)
	{
		Source.Add(Polygon(Patch));
		ExteriorArea += FMath::Abs(PolygonArea(Patch));
	}
	FPolygons Covered, Outside, Remaining;
	if (!PolygonsUnion(Source, Covered, false) || !PolygonsDifference(Covered, Face, Outside) ||
		!PolygonsDifference(Face, Covered, Remaining))
	{
		return Fail(Error, TEXT("native mixed-face exterior/interior partition failed"));
	}
	const double Tolerance = FMath::Max(.01, Area(Face) * 1.e-6);
	if (CoverageDiagnostics)
	{
		*CoverageDiagnostics = FString::Printf(TEXT("faceArea=%.17g patchSum=%.17g unionArea=%.17g remainderArea=%.17g outsideArea=%.17g overlapDelta=%.17g"),
			Area(Face), ExteriorArea, Area(Covered), Area(Remaining), Area(Outside), ExteriorArea - Area(Covered));
	}
	if (Area(Outside) > Tolerance || FMath::Abs(Area(Covered) - ExteriorArea) > Tolerance ||
		FMath::Abs(Area(Covered) + Area(Remaining) - Area(Face)) > Tolerance)
	{
		return Fail(Error, TEXT("mixed-face partition overlaps exterior patches or changes complete face area"));
	}
	return ConvexParts(Remaining, InteriorPatches, Error);
}

bool DublinDetail::AnalyzeBuilding(const FDublinCityBuilding& Building, FPlan& Plan, FString& Error)
{
	UE_LOG(LogDublinCityDetail, Display, TEXT("CityDetail analyze begin source=%s"), *Building.Id);
	Plan = FPlan();
	Plan.SourceId = Building.Id;
	if (!ValidateSource(Building, Plan.Assembly.SourceVolumeCm3, Error)) { return false; }
	FBox Bounds(ForceInit);
	for (const FVector& V : Building.Mesh.VerticesCm) { Bounds += V; }
	Plan.Assembly.MinZ = Bounds.Min.Z; Plan.Assembly.MaxZ = Bounds.Max.Z;
	if (Building.Id == TEXT("landmark/spire"))
	{
		Plan.Recipe = EDublinFractureRecipe::LayeredMasonry;
		Plan.Reason = TEXT("Named tapered landmark: source-face star tetrahedra and irregular longitudinal cuts; no hollow-room inference");
		const FVector Center = Bounds.GetCenter();
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			TArray<FVector> Tet{Center};
			for (int32 K = 0; K < 3; ++K) { Tet.Add(Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + K]]); }
			const double V = FVector::DotProduct(Tet[1] - Center, FVector::CrossProduct(Tet[3] - Center, Tet[2] - Center)) / 6;
			if (V <= 0) { return Fail(Error, TEXT("named landmark is not star-shaped about its source center")); }
			Plan.Assembly.StructuralVolumeCm3 += V;
			Plan.Tetrahedra.Add(Tet);
			const FVector FaceCenter = (Tet[1] + Tet[2] + Tet[3]) / 3;
			const FVector N = DublinCity::ClockwiseNormal(Tet[1], Tet[2], Tet[3]);
			AddProbe(Plan, FaceCenter + N * 2, FaceCenter - N * 2, true);
		}
		Plan.BroadAreaM2 = 2 * Bounds.GetSize().Z * Bounds.GetSize().GetAbsMin() / 10000;
		Plan.Tier = 384;
	}
	else
	{
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			if (Building.MaterialIds[T] != 1) { continue; }
			const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]];
			const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]];
			const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]];
			if (FMath::Abs(FVector::CrossProduct(B - A, C - A).Z) > 1.e-8) { return Fail(Error, TEXT("nonvertical ordinary wall is not supported by the height-field recipe")); }
		}
		FPolygons P, Base; TArray<FPatch> RoofPatches, BasePatches;
		double MinRoof, MaxRoof, MinBase, MaxBase;
		if (!Surface(Building, 0, P, RoofPatches, MinRoof, MaxRoof, Error) ||
			!Surface(Building, 2, Base, BasePatches, MinBase, MaxBase, Error)) { return false; }
		FPolygons Difference;
		if (!PolygonsExclusiveOr(P, Base, Difference) || Area(Difference) > FMath::Max(.01, Area(P) * 1.e-8))
		{
			return Fail(Error, TEXT("roof/base projected domains differ; unsupported non-heightfield source"));
		}
		for (const auto& Polygon : P) { Plan.HoleCount += Polygon.GetHoles().Num(); }
		Plan.bVariableRoof = MaxRoof - MinRoof > 1.e-6;
		double MinRoofUnderside = TNumericLimits<double>::Max();
		for (const FPatch& Roof : RoofPatches)
		{
			for (const auto& Patch : Roof.Domain)
			{
				for (const FVector2D& Point : Patch.GetOuter().GetVertices())
				{
					MinRoofUnderside = FMath::Min(MinRoofUnderside, ZAt(Roof.Height, Point) - RoofDepth(Roof.Height));
				}
				for (const auto& Hole : Patch.GetHoles())
				{
					for (const FVector2D& Point : Hole.GetVertices())
					{
						MinRoofUnderside = FMath::Min(MinRoofUnderside, ZAt(Roof.Height, Point) - RoofDepth(Roof.Height));
					}
				}
			}
		}
		FPolygons Q, Wall;
		const bool bBridge = Building.HeightMethod.StartsWith(TEXT("bridge_"));
		const bool bShallow = MinRoofUnderside - MaxBase - 20 <= 100;
		if (!bBridge && !bShallow)
		{
			if (MaxBase - MinBase > 1.e-6) { return Fail(Error, TEXT("ordinary building has unsupported variable base")); }
			if (!PolygonsOffset(-30, P, Q, false, 2.0, EPolygonOffsetJoinType::Miter))
			{
				return Fail(Error, TEXT("native inward offset failed; no masonry fallback from failure"));
			}
		}
		if (bBridge || bShallow || Q.IsEmpty() || Area(Q) < 100)
		{
			Plan.Recipe = EDublinFractureRecipe::LayeredMasonry;
			Plan.Reason = bBridge ? TEXT("Source bridge deck/arch: layered solid plates preserving both height fields") :
				bShallow ? TEXT("Source shallow volume: solid irregular plates, no tiny-room inference") :
				TEXT("30cm native inset has no usable interior: source-conserving narrow masonry");
			for (const FPatch& Top : RoofPatches)
			{
				for (const FPatch& Bottom : BasePatches)
				{
					FPolygons Domain;
					if (!PolygonsIntersection(Top.Domain, Bottom.Domain, Domain)) { return Fail(Error, TEXT("deck height-field overlay failed")); }
					if (Domain.IsEmpty()) { continue; }
					const double Height = MinRoof - MaxBase;
					const int32 Layers = FMath::Clamp(FMath::CeilToInt(FMath::Max(Height, 1.0) / (bBridge ? 35.0 : 90.0)), 2, 16);
					for (int32 K = 0; K < Layers; ++K)
					{
						const FVector Low = FMath::Lerp(Bottom.Height, Top.Height, static_cast<double>(K) / Layers);
						const FVector High = FMath::Lerp(Bottom.Height, Top.Height, static_cast<double>(K + 1) / Layers);
						if (!AddMembers(Plan, Domain, Low, High, !bBridge && !bShallow, Error)) { return false; }
					}
				}
			}
		}
		else
		{
			Plan.Reason = FString::Printf(TEXT("Source-preserving hollow height field: %d courtyard holes; 30cm walls; 20cm normal roof thickness and base/floors; closed vertical joins at roof facets; equal clear rooms below minimum roof underside"), Plan.HoleCount);
			if (!PolygonsDifference(P, Q, Wall)) { return Fail(Error, TEXT("native wall-ring difference failed")); }
			if (!AddMembers(Plan, P, FVector(0, 0, MinBase), FVector(0, 0, MinBase + 20), false, Error)) { return false; }
			for (const FPatch& Roof : RoofPatches)
			{
				const FVector Underside = Roof.Height - FVector(0, 0, RoofDepth(Roof.Height));
				if (!AddMembers(Plan, Roof.Domain, Underside, Roof.Height, false, Error) ||
					!AddIntersectionMembers(Plan, Wall, Roof.Domain, FVector(0, 0, MinBase + 20),
						Underside, true, Error)) { return false; }
			}
			const double H = MinRoofUnderside + 20 - MinBase;
			const int32 S = FMath::Max(1, FMath::RoundToInt((H - 40) / 320));
			const double Clear = (H - 40 - 20 * (S - 1)) / S;
			for (int32 K = 0; K < S; ++K)
			{
				const double Z = MinBase + 20 + K * (Clear + 20);
				Plan.Assembly.RoomsZ.Emplace(Z, Z + Clear);
				if (K + 1 < S && !AddMembers(Plan, Q, FVector(0, 0, Z + Clear), FVector(0, 0, Z + Clear + 20), false, Error)) { return false; }
				for (const FGeneralPolygon2d& Room : Q)
				{
					FVector2D XY;
					if (!InteriorPoint(Room, XY, Error)) { return false; }
					const FVector C(XY.X, XY.Y, Z + Clear * .5);
					Plan.VoidSamplesCm.Add(C);
					AddProbe(Plan, C, FVector(C.X, C.Y, Z - 5), true);
					double Ceiling = Z + Clear;
					if (K + 1 == S)
					{
						bool bFound = false;
						for (const FPatch& Roof : RoofPatches)
						{
							for (const FGeneralPolygon2d& Patch : Roof.Domain)
							{
								if (Patch.Contains(XY)) { Ceiling = ZAt(Roof.Height, XY) - RoofDepth(Roof.Height); bFound = true; break; }
							}
							if (bFound) { break; }
						}
						if (!bFound) { return Fail(Error, TEXT("room probe has no source roof")); }
					}
					AddProbe(Plan, C, FVector(C.X, C.Y, Ceiling + 5), true);
					AddProbe(Plan, FVector(C.X, C.Y, Z + Clear * .25), FVector(C.X, C.Y, Z + Clear * .75), false);
				}
			}
		}
		for (const FGeneralPolygon2d& Poly : P)
		{
			for (const FPolygon2d& Hole : Poly.GetHoles())
			{
				FVector2D XY;
				if (!InteriorPoint(Polygon(Hole.GetVertices()), XY, Error)) { return false; }
				Plan.VoidSamplesCm.Emplace(XY.X, XY.Y, (MinRoof + MaxBase) * .5);
				AddProbe(Plan, FVector(XY.X, XY.Y, MinBase - 10), FVector(XY.X, XY.Y, MaxRoof + 10), false);
			}
		}
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]];
			const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]];
			const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]];
			const FVector N = DublinCity::ClockwiseNormal(A, B, C), Mid = (A + B + C) / 3;
			AddProbe(Plan, Mid + N * 1, Mid - N * 1, true);
		}
		if (!CoalesceMemberPrecision(Building, Plan, Error)) { return false; }
		for (const auto& M : Plan.Assembly.Members) { Plan.BroadAreaM2 += M.Weight / 10000; }
		Plan.Tier = Plan.BroadAreaM2 <= 300 ? 128 : Plan.BroadAreaM2 <= 1500 ? 384 : Plan.BroadAreaM2 <= 6000 ? 768 : 1536;
	}
	if (Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num() > Plan.Tier)
	{
		if (Plan.LargestMemberAreaM2 > 16 && Plan.Assembly.Members.Num() + Plan.Tetrahedra.Num() <= 1536)
		{
			Plan.Tier = 1536; Plan.bDenseForOversize = true;
			Plan.Reason += TEXT("; Dense approved by recorded member oversize");
		}
		else { return Fail(Error, TEXT("source-exact convex member count exceeds declared tier; explicit unsupported, no geometry dropped")); }
	}
	double Weight = 0;
	for (const auto& M : Plan.Assembly.Members) { Weight += M.Weight; }
	const int32 Minimum = Plan.Assembly.Members.Num();
	const int32 Target = FMath::Min(Plan.Tier, FMath::Max(Minimum + 1, FMath::CeilToInt(Plan.BroadAreaM2 / 1.5)));
	int32 Allocated = 0;
	for (auto& M : Plan.Assembly.Members)
	{
		M.Seeds = 1 + FMath::FloorToInt((Target - Minimum) * M.Weight / FMath::Max(Weight, 1.0)); Allocated += M.Seeds;
	}
	while (Allocated < Target && !Plan.Assembly.Members.IsEmpty())
	{
		int32 Best = 0;
		for (int32 I = 1; I < Plan.Assembly.Members.Num(); ++I)
		{
			if (Plan.Assembly.Members[I].Weight / Plan.Assembly.Members[I].Seeds >
				Plan.Assembly.Members[Best].Weight / Plan.Assembly.Members[Best].Seeds) { Best = I; }
		}
		++Plan.Assembly.Members[Best].Seeds; ++Allocated;
	}
	if (Plan.Probes.IsEmpty() || Plan.Probes.Num() > 4096 || Plan.VoidSamplesCm.Num() > 4096 ||
		Plan.Assembly.StructuralVolumeCm3 <= 0 || Plan.Assembly.StructuralVolumeCm3 > Plan.Assembly.SourceVolumeCm3 * 1.0001)
	{
		return Fail(Error, TEXT("invalid probe contract or structural volume"));
	}
	if (Plan.Recipe == EDublinFractureRecipe::LayeredMasonry &&
		FMath::Abs(Plan.Assembly.StructuralVolumeCm3 - Plan.Assembly.SourceVolumeCm3) > FMath::Max(1.0, Plan.Assembly.SourceVolumeCm3 * 1.e-4))
	{
		return Fail(Error, TEXT("masonry assembly does not conserve original solid"));
	}
	Plan.SourceDigest = DublinDestruction::BuildingDigest(Building, Plan.Recipe);
	Plan.bSupported = true;
	Error.Reset();
	return true;
}

bool DublinDetail::AnalyzeCity(const FDublinCityData& City, TArray<FPlan>& Plans, TArray<FString>& Unsupported)
{
	Plans.Reset(); Unsupported.Reset();
	for (const auto& Building : City.Buildings)
	{
		FPlan& Plan = Plans.AddDefaulted_GetRef();
		FString Error;
		if (!AnalyzeBuilding(Building, Plan, Error))
		{
			Plan.Reason = TEXT("Unsupported: ") + Error;
			Unsupported.Add(Building.Id + TEXT(": ") + Error);
		}
	}
	return Unsupported.IsEmpty() && !Plans.IsEmpty();
}

bool DublinDetail::ResolveBakeRecipe(const FDublinCityBuilding& Building, EDublinFractureRecipe Requested,
	EDublinFractureRecipe& Resolved, FString& Error)
{
	Resolved = Requested;
	if (!DublinFractureBake::IsDetailedRecipe(Requested))
	{
		if (!DublinFractureBake::IsSupportedRecipe(Requested)) { return Fail(Error, TEXT("unsupported requested bake recipe")); }
		Error.Reset();
		return true;
	}
	FPlan Plan;
	if (!AnalyzeBuilding(Building, Plan, Error)) { return false; }
	if (Requested == EDublinFractureRecipe::LayeredMasonry && Plan.Recipe != Requested)
	{
		return Fail(Error, TEXT("refusing solid masonry for a structurally supported building"));
	}
	Resolved = Plan.Recipe;
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailBoundaryPartitionTest,
	"DublinFlight.Destruction.CityDetail.NativeBoundaryPartition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailBoundaryPartitionTest::RunTest(const FString& Parameters)
{
	const TArray<TArray<FVector2D>> Boundaries{
		{{0, 0}, {400, 0}, {400, 300}, {0, 300}},
		{{0, 0}, {400, 0}, {400, 300}, {0, 300}, {0, 0}},
		{{0, 0}, {400, 0}, {400, 0}, {400, 300}, {0, 300}, {0, 0}},
		{{0, 0}, {200, 0}, {400, 0}, {400, 300}, {0, 300}, {0, 0}}};
	for (int32 I = 0; I < Boundaries.Num(); ++I)
	{
		const FPolygons Domain{Polygon(Boundaries[I])};
		TArray<TArray<FVector2D>> Parts; FString Error;
		if (!ConvexParts(Domain, Parts, Error)) { AddError(FString::Printf(TEXT("boundary %d: %s"), I, *Error)); return false; }
		TestEqual(TEXT("Native rectangle partition merges its final array element without stale indexing"), Parts.Num(), 1);
		if (Parts.Num() != 1) { return false; }
		TestTrue(TEXT("Explicit closure and duplicate constraint endpoints retain the complete source area"),
			FMath::IsNearlyEqual(DublinStructural::Area(Parts[0]), 120000.0, .001));
		for (const FVector2D& V : Boundaries[I])
		{
			TestTrue(TEXT("Every exterior boundary seam survives convex merging"), Parts[0].Contains(V));
		}
		FVector2D Point;
		if (!InteriorPoint(Domain[0], Point, Error)) { AddError(Error); return false; }
		TestTrue(TEXT("Native room point remains inside the duplicate-endpoint boundary"), Domain[0].Contains(Point));
	}
	FGeneralPolygon2d Courtyard = Polygon(Boundaries.Last());
	FPolygon2d Hole;
	for (const FVector2D& P : TArray<FVector2D>{{100, 100}, {100, 200}, {100, 200}, {200, 200}, {200, 100}, {100, 100}}) { Hole.AppendVertex(P); }
	if (!TestTrue(TEXT("Closed duplicate-endpoint hole is retained"), Courtyard.AddHole(Hole, true, true))) { return false; }
	TArray<TArray<FVector2D>> Parts; FString Error;
	if (!ConvexParts({Courtyard}, Parts, Error)) { AddError(Error); return false; }
	double Covered = 0;
	for (const auto& P : Parts) { Covered += DublinStructural::Area(P); }
	TestTrue(TEXT("Native constrained partition preserves courtyard subtraction"), FMath::IsNearlyEqual(Covered, 110000.0, .001));
	FVector2D Point;
	if (!InteriorPoint(Courtyard, Point, Error)) { AddError(Error); return false; }
	TestTrue(TEXT("Native room point avoids the courtyard hole"), Courtyard.Contains(Point));
	FPolygon2d ClockwiseOuter = Courtyard.GetOuter();
	ClockwiseOuter.Reverse(); Hole.Reverse();
	FGeneralPolygon2d ClockwiseCourtyard(ClockwiseOuter);
	if (!TestTrue(TEXT("Reversed hole matches reversed outer winding"), ClockwiseCourtyard.AddHole(Hole, true, true))) { return false; }
	Parts.Reset();
	if (!ConvexParts({ClockwiseCourtyard}, Parts, Error)) { AddError(Error); return false; }
	Covered = 0;
	for (const auto& P : Parts) { Covered += DublinStructural::Area(P); }
	TestTrue(TEXT("Negative winding fill preserves the same courtyard subtraction"), FMath::IsNearlyEqual(Covered, 110000.0, .001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailZeroAreaPartitionTest,
	"DublinFlight.Destruction.CityDetail.NativeZeroAreaPartition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailZeroAreaPartitionTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Vertices{{0, 0}, {200, 0}, {400, 0}, {400, 300}, {0, 300}};
	const FGeneralPolygon2d Domain = Polygon(Vertices);
	TArray<FIndex3i> Triangles{{0, 2, 3}, {0, 3, 4}, {0, 1, 2}};
	FString Error;
	if (!ValidateFilledTriangles(Domain, Triangles, Vertices, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("Only exactly collinear zero-coverage triangle is excluded"), Triangles.Num(), 2);
	TestEqual(TEXT("No native boundary/seam vertex is removed"), Vertices.Num(), 5);
	TArray<FVector2D> Thin{{0, 0}, {1, 0}, {1, 1.e-11}};
	Triangles = {{0, 1, 2}};
	if (!ValidateFilledTriangles(Polygon(Thin), Triangles, Thin, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("Positive-area triangle below old epsilon is never deleted"), Triangles.Num(), 1);
	TestTrue(TEXT("Thin positive area remains representable"), PolygonArea(Thin) > 0 && PolygonArea(Thin) < 1.e-10);
	Triangles = {{0, 1, 99}};
	TestFalse(TEXT("Invalid native index is rejected, not skipped"), ValidateFilledTriangles(Polygon(Thin), Triangles, Thin, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailMemberPrecisionTest,
	"DublinFlight.Destruction.CityDetail.NativeMemberPrecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailMemberPrecisionTest::RunTest(const FString& Parameters)
{
	FDublinCityBuilding Building;
	const double OuterX = 400 + 1.e-12;
	Building.Mesh.VerticesCm = {{0, 0, 0}, {OuterX, 0, 0}, {OuterX, 300, 0}, {0, 300, 0}};
	DublinStructural::FMember Main;
	Main.Polygon = {{0, 0}, {400, 0}, {400, 300}, {0, 300}};
	Main.MinZ = 0; Main.MaxZ = 20; Main.bSourcePlaneExterior = true;
	DublinStructural::FMember Sliver = Main;
	Sliver.Polygon = {{400, 0}, {OuterX, 0}, {OuterX, 300}, {400, 300}};
	TestTrue(TEXT("Fixture is a genuinely positive-volume partition sliver"), MemberVolume(Sliver) > 0);
	DublinDetail::FPlan Plan;
	Plan.Assembly.Members = {Main, Sliver};
	Plan.Assembly.StructuralVolumeCm3 = MemberVolume(Main) + MemberVolume(Sliver);
	const double Before = Plan.Assembly.StructuralVolumeCm3;
	FString Error;
	if (!CoalesceMemberPrecision(Building, Plan, Error)) { AddError(Error); return false; }
	if (!TestEqual(TEXT("Unrepresentable positive sliver is unioned with its compatible neighbour"), Plan.Assembly.Members.Num(), 1)) { return false; }
	TestTrue(TEXT("Union conserves original derived-volume reference"),
		FMath::IsNearlyEqual(MemberVolume(Plan.Assembly.Members[0]), Before, FMath::Max(1.0, Before * 1.e-4)));
	for (const FVector& P : Building.Mesh.VerticesCm)
	{
		TestTrue(TEXT("Compatible union preserves every original exterior corner exactly"),
			Plan.Assembly.Members[0].Polygon.Contains(FVector2D(P.X, P.Y)));
	}
	TestTrue(TEXT("Precision treatment is explicit in persisted classification reason"),
		Plan.Reason.Contains(TEXT("interiorPrecisionCm=0.01")) &&
		(Plan.Reason.Contains(TEXT("compatibleSliverUnions=1")) || Plan.Reason.Contains(TEXT("exactZeroResidues=1"))));
	for (FVector& P : Building.Mesh.VerticesCm) { if (P.X == OuterX) { P.X = 400.00003; } }
	Sliver.Polygon = {{400, 0}, {400.00003, 0}, {400.00003, 300}, {400, 300}};
	TestTrue(TEXT("Representable positive sliver is below the existing one-cubic-centimetre native leaf gate"),
		MemberVolume(Sliver) > 0 && MemberVolume(Sliver) <= 1.0 &&
		FVector3f(400, 0, 0) != FVector3f(400.00003f, 0, 0));
	Plan = DublinDetail::FPlan();
	Plan.Assembly.Members = {Main, Sliver};
	Plan.Assembly.StructuralVolumeCm3 = MemberVolume(Main) + MemberVolume(Sliver);
	if (!CoalesceMemberPrecision(Building, Plan, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("Positive sub-cubic-centimetre member is unioned, not deleted or forced into physics"),
		Plan.Assembly.Members.Num(), 1);
	Plan = DublinDetail::FPlan();
	Main.Polygon.Insert(FVector2D(200 + 1.e-7, 0), 1);
	Main.Polygon.Insert(FVector2D(200, 0), 1);
	Plan.Assembly.Members = {Main};
	Plan.Assembly.StructuralVolumeCm3 = MemberVolume(Main);
	if (!CoalesceMemberPrecision(Building, Plan, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("A generated seam already collapsed by the mesh weld is coalesced before conversion"),
		Plan.Assembly.Members[0].Polygon.Num(), 5);
	Building.Mesh.VerticesCm = {{0, 0, 0}, {400.00003, 0, 0}, {400.00003, 150, 0},
		{400, 150, 0}, {400, 300, 0}, {0, 300, 0}};
	Sliver.Polygon = {{400, 0}, {400.00003, 0}, {400.00003, 150}, {400, 150}};
	Plan = DublinDetail::FPlan();
	Plan.Assembly.Members = {Main, Sliver};
	Plan.Assembly.StructuralVolumeCm3 = MemberVolume(Main) + MemberVolume(Sliver);
	if (!CoalesceMemberPrecision(Building, Plan, Error)) { AddError(Error); return false; }
	double NonconvexRetained = 0;
	for (const auto& Part : Plan.Assembly.Members)
	{
		NonconvexRetained += MemberVolume(Part);
		TestTrue(TEXT("Nonconvex compatible union is repartitioned into usable positive-volume members"), MemberVolume(Part) > 1.0);
		TestFalse(TEXT("Repartition never fills the original concave exterior notch"),
			Polygon(Part.Polygon).Contains(FVector2D(400.00002, 250)));
	}
	TestTrue(TEXT("Nonconvex repartition conserves the original derived solid"),
		FMath::IsNearlyEqual(NonconvexRetained, Plan.Assembly.StructuralVolumeCm3, FMath::Max(1.0, NonconvexRetained * 1.e-4)));
	Plan = DublinDetail::FPlan();
	Sliver.MinZ = 100; Sliver.MaxZ = 120;
	Plan.Assembly.Members = {Main, Sliver};
	Plan.Assembly.StructuralVolumeCm3 = MemberVolume(Main) + MemberVolume(Sliver);
	TestFalse(TEXT("No compatible neighbour never permits deleting a positive-volume member"),
		CoalesceMemberPrecision(Building, Plan, Error));
	TestEqual(TEXT("Incompatible positive sliver remains accounted for"), Plan.Assembly.Members.Num(), 2);
	Building.Mesh.VerticesCm = {{0, 0, 0}, {400, 0, 0}, {400, 300, 0}, {0, 300, 0}};
	DublinStructural::FMember Residue = Main;
	Residue.Polygon = {{400 - 4.e-13, 0}, {400 + 4.e-13, 0}, {0, 300}};
	Residue.MinZ = 20; Residue.MaxZ = 1600; Residue.TopSlope = FVector2D(.03, .02);
	TestTrue(TEXT("Machine-roundoff fixture initially has a spurious positive determinant"), MemberVolume(Residue) > 0);
	Plan = DublinDetail::FPlan();
	Plan.Assembly.Members = {Main, Residue};
	Plan.Assembly.StructuralVolumeCm3 = MemberVolume(Main) + MemberVolume(Residue);
	if (!CoalesceMemberPrecision(Building, Plan, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("Exact source-corner canonicalization identifies a zero-volume construction residue"),
		Plan.Assembly.Members.Num(), 1);
	TestTrue(TEXT("Zero-residue treatment remains explicitly accounted"), Plan.Reason.Contains(TEXT("exactZeroResidues=1")));
	return true;
}
#endif
#endif
