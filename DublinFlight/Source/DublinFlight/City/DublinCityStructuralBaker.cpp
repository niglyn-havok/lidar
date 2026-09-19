#include "City/DublinCityStructural.h"

#if WITH_EDITOR
#include "City/DublinCityDestruction.h"
#include "City/DublinCityDetail.h"
#include "Algo/Reverse.h"
#include "CompGeom/ExactPredicates.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Distance/DistPoint3Triangle3.h"
#include "MeshDescription.h"
#include "MeshQueries.h"
#include "Misc/Crc.h"
#include "PlanarCut.h"
#include "StaticMeshAttributes.h"
#include "Spatial/PointHashGrid3.h"
#include "Voronoi/Voronoi.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	using DublinStructural::FAssembly;
	using DublinStructural::FMember;

	bool Fail(FString& Error, const FString& Message)
	{
		Error = TEXT("StructuralPilot: ") + Message;
		return false;
	}

	struct FCorner
	{
		FVector Position;
		FVector2D UV0;
		FVector2D UV1;
		FLinearColor Color;
		int32 VertexId = INDEX_NONE;
	};

	struct FFace
	{
		TArray<FCorner> Corners;
		FVector Normal;
		int32 Material = 3;
		int32 SourceTriangle = INDEX_NONE;
		bool bGeneratedCut = false;
	};

	FCorner Lerp(const FCorner& A, const FCorner& B, double T)
	{
		return {FMath::Lerp(A.Position, B.Position, T), FMath::Lerp(A.UV0, B.UV0, T),
			FMath::Lerp(A.UV1, B.UV1, T), FMath::Lerp(A.Color, B.Color, static_cast<float>(T))};
	}

	bool InsideSourceSolid(const FDublinCityBuilding& Building, const FVector& Point)
	{
		double SolidAngle = 0;
		for (int32 T = 0; T < Building.Mesh.Triangles.Num(); T += 3)
		{
			const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T]] - Point;
			const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T + 1]] - Point;
			const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T + 2]] - Point;
			const double LA = A.Size(), LB = B.Size(), LC = C.Size();
			const double Denominator = LA * LB * LC + FVector::DotProduct(A, B) * LC +
				FVector::DotProduct(B, C) * LA + FVector::DotProduct(C, A) * LB;
			SolidAngle += 2 * FMath::Atan2(FVector::DotProduct(A, FVector::CrossProduct(B, C)), Denominator);
		}
		return FMath::IsFinite(SolidAngle) && FMath::Abs(SolidAngle) > 2 * PI;
	}

	TArray<FPlane> MemberPlanes(const FMember& Member)
	{
		TArray<FPlane> Planes;
		for (int32 I = 0; I < Member.Polygon.Num(); ++I)
		{
			const FVector2D& A = Member.Polygon[I];
			const FVector2D E = (Member.Polygon[(I + 1) % Member.Polygon.Num()] - A).GetSafeNormal();
			const FVector N(-E.Y, E.X, 0);
			Planes.Emplace(FVector(A.X, A.Y, 0), N);
		}
		Planes.Emplace(FVector(0, 0, Member.MinZ), FVector::UpVector);
		Planes.Emplace(FVector(0, 0, Member.MaxZ), -FVector::UpVector);
		return Planes;
	}

	void ClipFace(TArray<FCorner>& Corners, const FPlane& Plane)
	{
		TArray<FCorner> Out;
		for (int32 I = 0; I < Corners.Num(); ++I)
		{
			const FCorner& A = Corners[I];
			const FCorner& B = Corners[(I + 1) % Corners.Num()];
			const double RawA = Plane.PlaneDot(A.Position), RawB = Plane.PlaneDot(B.Position);
			const double DA = FMath::Abs(RawA) < 1.e-7 ? 0 : RawA;
			const double DB = FMath::Abs(RawB) < 1.e-7 ? 0 : RawB;
			if (DA >= 0) { Out.Add(A); }
			if ((DA >= 0) != (DB >= 0)) { Out.Add(Lerp(A, B, DA / (DA - DB))); }
		}
		for (int32 I = Out.Num() - 1; I >= 0 && Out.Num() > 1; --I)
		{
			if (Out[I].Position.Equals(Out[(I + 1) % Out.Num()].Position, 1.e-7)) { Out.RemoveAt(I); }
		}
		Corners = MoveTemp(Out);
	}

	bool WriteFaces(TArray<FFace>& Faces, FMeshDescription& Mesh, FString& Error, bool bUseSharedSplitPositions = false);

	bool MakeMemberMesh(const FDublinCityBuilding& Building, const FAssembly& Assembly,
		const FMember& Member, FMeshDescription& Mesh, FString& Error)
	{
		TArray<FFace> Faces;
		const TArray<FPlane> Planes = MemberPlanes(Member);
		for (int32 FaceIndex = 0; FaceIndex < Planes.Num(); ++FaceIndex)
		{
			const FPlane& Plane = Planes[FaceIndex];
			const FVector Normal(-Plane.X, -Plane.Y, -Plane.Z);
			TArray<FVector> Polygon;
			if (FaceIndex < Member.Polygon.Num())
			{
				const FVector2D A = Member.Polygon[FaceIndex], B = Member.Polygon[(FaceIndex + 1) % Member.Polygon.Num()];
				Polygon = {FVector(A.X, A.Y, Member.MinZ), FVector(B.X, B.Y, Member.MinZ),
					FVector(B.X, B.Y, Member.MaxZ), FVector(A.X, A.Y, Member.MaxZ)};
			}
			else
			{
				const double Z = FaceIndex == Member.Polygon.Num() ? Member.MinZ : Member.MaxZ;
				for (const FVector2D& P : Member.Polygon) { Polygon.Emplace(P.X, P.Y, Z); }
			}
			bool bExterior = false;
			if (Normal.Z < -.9) { bExterior = FMath::IsNearlyEqual(Member.MinZ, Assembly.MinZ, 1.e-6); }
			else if (Normal.Z > .9) { bExterior = FMath::IsNearlyEqual(Member.MaxZ, Assembly.MaxZ, 1.e-6); }
			else
			{
				for (int32 I = 0; I < Assembly.Outer.Num(); ++I)
				{
					const FVector2D A = Assembly.Outer[I], B = Assembly.Outer[(I + 1) % Assembly.Outer.Num()];
					const FVector2D E = (B - A).GetSafeNormal();
					const FVector Out(E.Y, -E.X, 0);
					bExterior |= FVector::DotProduct(Out, Normal) > .999999 &&
						FMath::Abs(FVector::DotProduct(Polygon[0] - FVector(A.X, A.Y, Polygon[0].Z), Out)) < 1.e-6;
				}
			}
			if (bExterior)
			{
				double CoveredArea = 0;
				for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
				{
					FFace Face;
					Face.Normal = Normal;
					Face.Material = Building.MaterialIds[T];
					bool bOnPlane = true;
					for (int32 K = 0; K < 3; ++K)
					{
						const int32 V = Building.Mesh.Triangles[T * 3 + K];
						const FVector P = Building.Mesh.VerticesCm[V];
						bOnPlane &= FMath::Abs(Plane.PlaneDot(P)) < 1.e-6;
						const FVector2D UV = Building.Mesh.UV[V];
						Face.Corners.Add({P, Face.Material == 0 ? DublinCity::AerialUV(P + Building.PivotCm) : UV,
							UV, DublinDestruction::SourceVertexLinearColor(Building.Mesh, V)});
					}
					if (!bOnPlane) { continue; }
					for (const FPlane& ClipPlane : Planes) { ClipFace(Face.Corners, ClipPlane); }
					if (Face.Corners.Num() < 3) { continue; }
					double A = 0;
					for (int32 I = 1; I + 1 < Face.Corners.Num(); ++I)
					{
						A += FVector::CrossProduct(Face.Corners[I].Position - Face.Corners[0].Position,
							Face.Corners[I + 1].Position - Face.Corners[0].Position).Size() * .5;
					}
					if (A < 1.e-8) { continue; }
					CoveredArea += A;
					Faces.Add(MoveTemp(Face));
				}
				double Expected = 0;
				for (int32 I = 1; I + 1 < Polygon.Num(); ++I)
				{
					Expected += FVector::CrossProduct(Polygon[I] - Polygon[0], Polygon[I + 1] - Polygon[0]).Size() * .5;
				}
				if (FMath::Abs(CoveredArea - Expected) > FMath::Max(.01, Expected * 1.e-6))
				{
					return Fail(Error, TEXT("member exterior no longer exactly covers source triangles"));
				}
			}
			else
			{
				FFace Face;
				Face.Normal = Normal;
				const FVector U = FMath::Abs(Normal.Z) < .9 ? FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal() : FVector::ForwardVector;
				const FVector V = FVector::CrossProduct(Normal, U);
				for (const FVector& P : Polygon)
				{
					const FVector2D UV(FVector::DotProduct(P, U) / 100, FVector::DotProduct(P, V) / 100);
					Face.Corners.Add({P, UV, UV, FLinearColor::White});
				}
				Faces.Add(MoveTemp(Face));
			}
		}
		return WriteFaces(Faces, Mesh, Error);
	}

	bool WriteFaces(TArray<FFace>& Faces, FMeshDescription& Mesh, FString& Error, bool bUseSharedSplitPositions)
	{
		// Source triangle clipping splits boundary edges. Propagate those splits to adjacent faces.
		TArray<FVector> BoundaryVertices;
		for (const FFace& Face : Faces)
		{
			for (const FCorner& C : Face.Corners)
			{
				if (!BoundaryVertices.ContainsByPredicate([&C](const FVector& P) { return P.Equals(C.Position, 1.e-6); }))
				{
					BoundaryVertices.Add(C.Position);
				}
			}
		}
		FStaticMeshAttributes Attributes(Mesh);
		Attributes.Register();
		auto Positions = Attributes.GetVertexPositions();
		auto Normals = Attributes.GetVertexInstanceNormals();
		auto Tangents = Attributes.GetVertexInstanceTangents();
		auto Signs = Attributes.GetVertexInstanceBinormalSigns();
		auto UVs = Attributes.GetVertexInstanceUVs();
		auto Colors = Attributes.GetVertexInstanceColors();
		UVs.SetNumChannels(2);
		FPolygonGroupID Groups[4];
		for (int32 I = 0; I < 4; ++I)
		{
			Groups[I] = Mesh.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Groups[I]] = FName(*FString::Printf(TEXT("Material%d"), I));
		}
		TArray<FVector3f> WeldPositions;
		TArray<FLinearColor> WeldColors;
		TArray<FVertexID> WeldIds;
		for (FFace& Face : Faces)
		{
			TArray<FCorner> Conforming;
			for (int32 I = 0; I < Face.Corners.Num(); ++I)
			{
				const FCorner& A = Face.Corners[I];
				const FCorner& B = Face.Corners[(I + 1) % Face.Corners.Num()];
				const FVector Delta = B.Position - A.Position;
				if (Delta.SizeSquared() < 1.e-12) { continue; }
				TArray<TPair<double, FVector>> Splits;
				Splits.Emplace(0, A.Position);
				for (const FVector& P : BoundaryVertices)
				{
					const double T = FVector::DotProduct(P - A.Position, Delta) / Delta.SizeSquared();
					if (T > 1.e-7 && T < 1 - 1.e-7 && (A.Position + T * Delta).Equals(P, 1.e-6) &&
						!Splits.ContainsByPredicate([T](const auto& Split) { return Split.Key == T; })) { Splits.Emplace(T, P); }
				}
				Splits.Sort([](const auto& Left, const auto& Right) { return Left.Key < Right.Key; });
				for (const auto& Split : Splits)
				{
					FCorner Corner = Lerp(A, B, Split.Key);
					if (bUseSharedSplitPositions) { Corner.Position = Split.Value; }
					Conforming.Add(Corner);
				}
			}
			if (Conforming.Num() < 3)
			{
				FString Detail = FString::Printf(TEXT("collapsed member face rawCorners=%d conforming=%d normal=(%.17g,%.17g,%.17g)"),
					Face.Corners.Num(), Conforming.Num(), Face.Normal.X, Face.Normal.Y, Face.Normal.Z);
				TSet<FVector3f> FloatPositions;
				for (const FCorner& Corner : Face.Corners)
				{
					const FVector& P = Corner.Position;
					FloatPositions.Add(FVector3f(P));
					Detail += FString::Printf(TEXT(" (%.17g,%.17g,%.17g)"), P.X, P.Y, P.Z);
				}
				Detail += FString::Printf(TEXT(" distinctFloatPositions=%d"), FloatPositions.Num());
				return Fail(Error, Detail);
			}
			FCorner Mid = Conforming[0];
			for (int32 I = 1; I < Conforming.Num(); ++I) { Mid = Lerp(Mid, Conforming[I], 1.0 / (I + 1)); }
			for (int32 I = 0; I < Conforming.Num(); ++I)
			{
				FCorner Triangle[3] = {Mid, Conforming[I], Conforming[(I + 1) % Conforming.Num()]};
				FVector N = DublinCity::ClockwiseNormal(Triangle[0].Position, Triangle[1].Position, Triangle[2].Position);
				if (N.IsNearlyZero())
				{
					const FVector TrianglePositions[3]{Triangle[0].Position, Triangle[1].Position, Triangle[2].Position};
					if (!DublinStructural::FindFaceNormal(MakeArrayView(TrianglePositions), N)) { return Fail(Error, TEXT("exactly collinear member triangle")); }
				}
				if (FVector::DotProduct(N, Face.Normal) < 0) { Swap(Triangle[1], Triangle[2]); }
				TArray<FVertexInstanceID> Instances;
				for (const FCorner& Corner : Triangle)
				{
					int32 Weld = INDEX_NONE;
					for (int32 W = 0; W < WeldPositions.Num(); ++W)
					{
						if (WeldPositions[W].Equals(FVector3f(Corner.Position), 1.e-5f) && WeldColors[W].Equals(Corner.Color, 1.e-6f)) { Weld = W; break; }
					}
					if (Weld == INDEX_NONE)
					{
						Weld = WeldPositions.Add(FVector3f(Corner.Position));
						WeldColors.Add(Corner.Color);
						const FVertexID Vertex = Mesh.CreateVertex();
						WeldIds.Add(Vertex);
						Positions[Vertex] = WeldPositions[Weld];
					}
					const FVertexInstanceID Instance = Mesh.CreateVertexInstance(WeldIds[Weld]);
					Instances.Add(Instance);
					Normals[Instance] = FVector3f(Face.Normal);
					Tangents[Instance] = FVector3f(FMath::Abs(Face.Normal.Z) < .9 ?
						FVector::CrossProduct(FVector::UpVector, Face.Normal).GetSafeNormal() : FVector::ForwardVector);
					Signs[Instance] = 1;
					Colors[Instance] = FVector4f(Corner.Color);
					UVs.Set(Instance, 0, FVector2f(Corner.UV0));
					UVs.Set(Instance, 1, FVector2f(Corner.UV1));
				}
				Mesh.CreatePolygon(Groups[Face.Material], Instances);
			}
		}
		return true;
	}

	bool ValidateLeaf(FGeometryCollection& Geometry, int32 Bone, double& Volume, FString& Error)
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		FTransform Transform;
		TArray<int32> Selection{Bone};
		ConvertGeometryCollectionToDynamicMesh(Mesh, Transform, false, Geometry, true,
			Geometry.Transform.GetConstArray(), true, MakeArrayView(Selection));
		const bool bValid = Mesh.CheckValidity(UE::Geometry::FDynamicMesh3::FValidityOptions(), UE::Geometry::EValidityCheckFailMode::ReturnOnly);
		if (!bValid || !Mesh.IsClosed())
		{
			int32 BoundaryEdges = 0;
			FString Samples;
			for (int32 E : Mesh.EdgeIndicesItr())
			{
				if (!Mesh.IsBoundaryEdge(E)) { continue; }
				++BoundaryEdges;
				if (BoundaryEdges <= 6)
				{
					const auto EV = Mesh.GetEdgeV(E);
					const FVector A = Mesh.GetVertex(EV.A), B = Mesh.GetVertex(EV.B);
					Samples += FString::Printf(TEXT(" edge[(%.17g,%.17g,%.17g)->(%.17g,%.17g,%.17g)]"),
						A.X, A.Y, A.Z, B.X, B.Y, B.Z);
				}
			}
			TMap<FVector3f, int32> Positions;
			TMap<FIntPoint, FIntPoint> Incidence;
			int32 DegenerateFaces = 0;
			const int32 G = Geometry.TransformToGeometryIndex[Bone];
			for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
			{
				const FIntVector Triangle = Geometry.Indices[F];
				int32 V[3];
				for (int32 K = 0; K < 3; ++K)
				{
					const FVector3f P = Geometry.Vertex[Triangle[K]];
					if (const int32* Existing = Positions.Find(P)) { V[K] = *Existing; }
					else { V[K] = Positions.Num(); Positions.Add(P, V[K]); }
				}
				if (V[0] == V[1] || V[1] == V[2] || V[2] == V[0]) { ++DegenerateFaces; }
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 A = V[K], B = V[(K + 1) % 3];
					FIntPoint& Edge = Incidence.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)), FIntPoint::ZeroValue);
					++Edge.X; Edge.Y += A < B ? 1 : -1;
				}
			}
			int32 RawBoundary = 0, NonManifold = 0, WindingErrors = 0;
			for (const auto& Edge : Incidence)
			{
				RawBoundary += Edge.Value.X == 1;
				NonManifold += Edge.Value.X > 2;
				WindingErrors += Edge.Value.X == 2 && Edge.Value.Y != 0;
			}
			return Fail(Error, FString::Printf(TEXT("leaf %d is not a closed oriented manifold valid=%d vertices=%d triangles=%d boundaryEdges=%d rawPositionBoundaryEdges=%d nonmanifoldEdges=%d windingErrors=%d degenerateFaces=%d%s"),
				Bone, bValid, Mesh.VertexCount(), Mesh.TriangleCount(), BoundaryEdges, RawBoundary, NonManifold, WindingErrors, DegenerateFaces, *Samples));
		}
		Volume = UE::Geometry::TMeshQueries<UE::Geometry::FDynamicMesh3>::GetVolumeArea(Mesh).X;
		if (!FMath::IsFinite(Volume) || Volume <= 1) { return Fail(Error, FString::Printf(TEXT("leaf %d has invalid UE-oriented signed volume %.9g"), Bone, Volume)); }
		return true;
	}

	bool ValidateExterior(const FDublinCityBuilding& Building, const FGeometryCollection& Geometry,
		const FDublinFractureRecord& Record, FString& Error)
	{
		const auto UVs = GeometryCollection::UV::FindActiveUVLayers(Geometry);
		if (UVs.Num() != 2) { return Fail(Error, TEXT("fracture did not retain both exterior UV channels")); }
		double ExpectedArea[3] = {}, ActualArea[3] = {};
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]];
			const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]];
			const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]];
			ExpectedArea[Building.MaterialIds[T]] += FVector::CrossProduct(B - A, C - A).Size() * .5;
		}
		TArray<FVector> CutDirections;
		for (int32 Bone : Record.LeafTransforms)
		{
			const FTransform Global = GeometryCollectionAlgo::GlobalMatrix(Geometry.Transform, Geometry.Parent, Bone);
			const int32 G = Geometry.TransformToGeometryIndex[Bone];
			for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
			{
				if (!Geometry.Visible[F]) { return Fail(Error, TEXT("a structural leaf contains hidden faces")); }
				const FIntVector Tri = Geometry.Indices[F];
				FVector P[3];
				for (int32 K = 0; K < 3; ++K) { P[K] = Global.TransformPosition(FVector(Geometry.Vertex[Tri[K]])); }
				FVector N;
				if (!DublinStructural::FindFaceNormal(MakeArrayView(P), N))
				{
					return Fail(Error, FString::Printf(TEXT("exact zero-area face survived topology cleanup source=%s bone=%d face=%d"),
						*Building.Id, Bone, F));
				}
				const int32 Material = Geometry.MaterialID[F];
				if (Material < 0 || Material > 3) { return Fail(Error, TEXT("invalid fracture surface material")); }
				if (Geometry.Internal[F])
				{
					if (Material != 3) { return Fail(Error, TEXT("new fracture face uses exterior material")); }
					if (!CutDirections.ContainsByPredicate([&N](const FVector& Other) { return FMath::Abs(FVector::DotProduct(N, Other)) > .999; }))
					{
						CutDirections.Add(N);
					}
				}
				if (Material == 3) { continue; }
				ActualArea[Material] += FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).Size() * .5;
				for (int32 K = 0; K < 3; ++K)
				{
					bool bMatched = false;
					for (int32 T = 0; T < Building.MaterialIds.Num() && !bMatched; ++T)
					{
						if (Building.MaterialIds[T] != Material) { continue; }
						const int32 IA = Building.Mesh.Triangles[T * 3], IB = Building.Mesh.Triangles[T * 3 + 1], IC = Building.Mesh.Triangles[T * 3 + 2];
						const FVector A = Building.Mesh.VerticesCm[IA], B = Building.Mesh.VerticesCm[IB], C = Building.Mesh.VerticesCm[IC];
						const FVector SourceNormal = DublinCity::ClockwiseNormal(A, B, C);
						if (FVector::DotProduct(N, SourceNormal) < .999 ||
							FMath::Abs(FVector::DotProduct(P[K] - A, SourceNormal)) > .01) { continue; }
						UE::Geometry::TDistPoint3Triangle3<double> Distance(P[K], UE::Geometry::FTriangle3d(A, B, C));
						if (Distance.GetSquared() > DublinStructural::ContactToleranceCm * DublinStructural::ContactToleranceCm) { continue; }
						const double U = Distance.TriangleBaryCoords.X, V = Distance.TriangleBaryCoords.Y, W = Distance.TriangleBaryCoords.Z;
						const FVector2D SourceUV = Building.Mesh.UV[IA] * U + Building.Mesh.UV[IB] * V + Building.Mesh.UV[IC] * W;
						const FVector2D Primary = Material == 0 ? DublinCity::AerialUV(P[K] + Building.PivotCm) : SourceUV;
						const FLinearColor Color = DublinDestruction::SourceVertexLinearColor(Building.Mesh, IA) * static_cast<float>(U) +
							DublinDestruction::SourceVertexLinearColor(Building.Mesh, IB) * static_cast<float>(V) +
							DublinDestruction::SourceVertexLinearColor(Building.Mesh, IC) * static_cast<float>(W);
						bMatched = FVector2D(UVs[0][Tri[K]]).Equals(Primary, 2.e-4) &&
							FVector2D(UVs[1][Tri[K]]).Equals(SourceUV, 2.e-4) && Geometry.Color[Tri[K]].Equals(Color, 2.e-4f);
					}
					if (!bMatched)
					{
						const FVector RawCW = FVector::CrossProduct(P[2] - P[0], P[1] - P[0]);
						FVector ExactNormal;
						const bool bExactPositiveArea = DublinStructural::FindFaceNormal(MakeArrayView(P), ExactNormal);
						return Fail(Error, FString::Printf(TEXT("fracture changed exterior position, winding, UVs or vertex colors source=%s bone=%d face=%d corner=%d material=%d point=(%.17g,%.17g,%.17g) normal=%s uv0=%s uv1=%s color=%s triangle=[(%.17g,%.17g,%.17g),(%.17g,%.17g,%.17g),(%.17g,%.17g,%.17g)] rawCW=(%.17g,%.17g,%.17g) rawSquared=%.17g exactNoncollinear=%d exactNormal=(%.17g,%.17g,%.17g)"),
							*Building.Id, Bone, F, K, Material, P[K].X, P[K].Y, P[K].Z, *N.ToString(),
							*FVector2D(UVs[0][Tri[K]]).ToString(), *FVector2D(UVs[1][Tri[K]]).ToString(), *Geometry.Color[Tri[K]].ToString(),
							P[0].X, P[0].Y, P[0].Z, P[1].X, P[1].Y, P[1].Z, P[2].X, P[2].Y, P[2].Z,
							RawCW.X, RawCW.Y, RawCW.Z, RawCW.SizeSquared(), bExactPositiveArea, ExactNormal.X, ExactNormal.Y, ExactNormal.Z));
					}
				}
			}
		}
		for (int32 M = 0; M < 3; ++M)
		{
			if (FMath::Abs(ActualArea[M] - ExpectedArea[M]) > FMath::Max(1.0, ExpectedArea[M] * 1.e-4))
			{
				return Fail(Error, TEXT("fracture changed exterior surface coverage"));
			}
		}
		const int32 RequiredDirections = DublinFractureBake::IsDetailedRecipe(Record.Recipe)
			? FMath::Clamp(Record.PieceCount - Record.MemberCount, 1, 16) : 16;
		if (CutDirections.Num() < RequiredDirections) { return Fail(Error, TEXT("fracture lacks genuinely irregular cut directions")); }
		return true;
	}

	TArray<FVector> SitesForMember(const FMember& Member, uint32 Seed)
	{
		FRandomStream Random(static_cast<int32>(Seed));
		FBox Bounds(ForceInit);
		FVector2D Mid = FVector2D::ZeroVector;
		for (const FVector2D& P : Member.Polygon)
		{
			Mid += P;
			Bounds += FVector(P.X, P.Y, Member.MinZ);
			Bounds += FVector(P.X, P.Y, Member.MaxZ);
		}
		Mid /= Member.Polygon.Num();
		const TArray<FPlane> Planes = MemberPlanes(Member);
		TArray<FVector> Sites;
		for (int32 S = 0; S < Member.Seeds; ++S)
		{
			FVector Best = FVector::ZeroVector;
			double BestDistance = -1;
			for (int32 Candidate = 0; Candidate < 24; ++Candidate)
			{
				FVector P = FVector::ZeroVector;
				bool bInside = false;
				for (int32 Trial = 0; Trial < 1024 && !bInside; ++Trial)
				{
					if (Member.bWall)
					{
						const double Range = Bounds.GetSize().Size();
						const FVector2D XY = Mid + Member.WallTangent * Random.FRandRange(-Range, Range);
						P = FVector(XY.X, XY.Y, Random.FRandRange(Member.MinZ, Member.MaxZ));
					}
					else
					{
						P = FVector(Random.FRandRange(Bounds.Min.X, Bounds.Max.X), Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y),
							(Member.MinZ + Member.MaxZ) * .5);
					}
					bInside = true;
					for (const FPlane& Plane : Planes) { bInside &= Plane.PlaneDot(P) > 0.05; }
				}
				if (!bInside) { return {}; }
				double Distance = TNumericLimits<double>::Max();
				for (const FVector& Existing : Sites) { Distance = FMath::Min(Distance, FVector::DistSquared(P, Existing)); }
				if (Distance > BestDistance) { BestDistance = Distance; Best = P; }
			}
			if (BestDistance < 1) { return {}; }
			Sites.Add(Best);
		}
		return Sites;
	}
}

bool DublinStructural::FindFaceNormal(TConstArrayView<FVector> Positions, FVector& Normal)
{
	Normal = FVector::ZeroVector;
	if (Positions.Num() < 3) { return false; }
	FVector Best = FVector::ZeroVector;
	double Largest = 0;
	for (int32 I = 1; I + 1 < Positions.Num(); ++I)
	{
		const FVector& A = Positions[0];
		const FVector& B = Positions[I];
		const FVector& C = Positions[I + 1];
		if (A.ContainsNaN() || B.ContainsNaN() || C.ContainsNaN()) { return false; }
		const FVector Cross(
			UE::Geometry::ExactPredicates::Orient2<double>(FVector2D(A.Y, A.Z), FVector2D(C.Y, C.Z), FVector2D(B.Y, B.Z)),
			UE::Geometry::ExactPredicates::Orient2<double>(FVector2D(A.Z, A.X), FVector2D(C.Z, C.X), FVector2D(B.Z, B.X)),
			UE::Geometry::ExactPredicates::Orient2<double>(FVector2D(A.X, A.Y), FVector2D(C.X, C.Y), FVector2D(B.X, B.Y)));
		const double Scale = Cross.GetAbsMax();
		if (!Cross.ContainsNaN() && Scale > Largest) { Best = Cross; Largest = Scale; }
	}
	if (Largest == 0) { Normal = FVector::ZeroVector; return false; }
	// Normalize after scaling, so a finite positive-area face cannot fail the default area tolerance.
	Normal = (Best / Largest).GetSafeNormal();
	return !Normal.ContainsNaN() && !Normal.IsNearlyZero();
}

static bool MakeConvexAttributedFaces(const FDublinCityBuilding& Building,
	const TArray<TArray<FVector>>& InputFaces, TArray<FFace>& Faces, FString& Error)
{
	using DublinStructural::FindFaceNormal;
	Faces.Reset();
	FVector Center = FVector::ZeroVector;
	int32 Count = 0;
	for (const auto& Face : InputFaces) { for (const FVector& P : Face) { Center += P; ++Count; } }
	if (Count == 0) { return Fail(Error, TEXT("empty convex member")); }
	Center /= Count;
	TArray<TArray<FVector>> Polygons = InputFaces;
	TArray<FPlane> Planes;
	TArray<FVector> Normals;
	for (auto& Face : Polygons)
	{
		if (Face.Num() < 3) { return Fail(Error, TEXT("degenerate convex member face")); }
		FVector N;
		if (!FindFaceNormal(MakeArrayView(Face), N))
		{
			FString Points;
			for (const FVector& P : Face) { Points += FString::Printf(TEXT(" (%.17g,%.17g,%.17g)"), P.X, P.Y, P.Z); }
			return Fail(Error, FString::Printf(TEXT("exactly collinear convex member face source=%s face=%d points=%s"),
				*Building.Id, Normals.Num(), *Points));
		}
		if (FVector::DotProduct(N, Center - Face[0]) > 0) { Algo::Reverse(Face); N = -N; }
		Normals.Add(N);
		Planes.Emplace(Face[0], -N);
	}
	for (int32 F = 0; F < Polygons.Num(); ++F)
	{
		const auto& Polygon = Polygons[F];
		const FVector N = Normals[F];
		double Expected = 0, Covered = 0;
		for (int32 I = 1; I + 1 < Polygon.Num(); ++I) { Expected += FVector::CrossProduct(Polygon[I] - Polygon[0], Polygon[I + 1] - Polygon[0]).Size() * .5; }
		const int32 Before = Faces.Num();
		for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
		{
			FFace Face; Face.Normal = N; Face.Material = Building.MaterialIds[T]; Face.SourceTriangle = T;
			bool bOnPlane = true;
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 V = Building.Mesh.Triangles[T * 3 + K];
				const FVector P = Building.Mesh.VerticesCm[V];
				bOnPlane &= FMath::Abs(Planes[F].PlaneDot(P)) < 1.e-5;
				const FVector2D UV = Building.Mesh.UV[V];
				Face.Corners.Add({P, Face.Material == 0 ? DublinCity::AerialUV(P + Building.PivotCm) : UV,
					UV, DublinDestruction::SourceVertexLinearColor(Building.Mesh, V)});
			}
			if (!bOnPlane || FVector::DotProduct(N, DublinCity::ClockwiseNormal(Face.Corners[0].Position,
				Face.Corners[1].Position, Face.Corners[2].Position)) < .999999) { continue; }
			for (int32 EdgeIndex = 0; EdgeIndex < Polygon.Num(); ++EdgeIndex)
			{
				const FVector& A = Polygon[EdgeIndex];
				const FVector Edge = Polygon[(EdgeIndex + 1) % Polygon.Num()] - A;
				const FVector Inward = FVector::CrossProduct(Edge, N).GetSafeNormal(0);
				if (Inward.IsNearlyZero()) { return Fail(Error, TEXT("member face has an exactly collapsed boundary constraint")); }
				ClipFace(Face.Corners, FPlane(A, Inward));
			}
			if (Face.Corners.Num() < 3) { continue; }
			double Area = 0;
			for (int32 I = 1; I + 1 < Face.Corners.Num(); ++I)
			{
				Area += FVector::CrossProduct(Face.Corners[I].Position - Face.Corners[0].Position,
					Face.Corners[I + 1].Position - Face.Corners[0].Position).Size() * .5;
			}
			if (Area <= 1.e-10) { continue; }
			Covered += Area; Faces.Add(MoveTemp(Face));
		}
		if (Faces.Num() > Before)
		{
			const double Tolerance = FMath::Max(.01, Expected * 1.e-6);
			if (Covered > Expected + Tolerance)
			{
				const FVector Origin = Polygon[0];
				const FVector U = FVector::CrossProduct(FMath::Abs(N.Z) < .9 ? FVector::UpVector : FVector::ForwardVector, N).GetSafeNormal();
				const FVector V = FVector::CrossProduct(N, U);
				const auto Project = [&](const FVector& P) { const FVector D = P - Origin; return FVector2D(FVector::DotProduct(D, U), FVector::DotProduct(D, V)); };
				TArray<FVector2D> Boundary;
				for (const FVector& P : Polygon) { Boundary.Add(Project(P)); }
				TArray<TArray<FVector2D>> Exterior, Remaining;
				FString Sources;
				for (int32 I = Before; I < Faces.Num(); ++I)
				{
					TArray<FVector2D>& Patch = Exterior.AddDefaulted_GetRef();
					for (const FCorner& C : Faces[I].Corners) { Patch.Add(Project(C.Position)); }
					const int32 T = Faces[I].SourceTriangle;
					const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]];
					const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]];
					const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]];
					Sources += FString::Printf(TEXT(" tri=%d material=%d normalDot=%.17g planeMax=%.17g"), T, Faces[I].Material,
						FVector::DotProduct(N, DublinCity::ClockwiseNormal(A, B, C)),
						FMath::Max3(FMath::Abs(Planes[F].PlaneDot(A)), FMath::Abs(Planes[F].PlaneDot(B)), FMath::Abs(Planes[F].PlaneDot(C))));
				}
				FString PartitionError, Coverage;
				DublinDetail::PartitionFaceRemainder(Boundary, Exterior, Remaining, PartitionError, &Coverage);
				return Fail(Error, FString::Printf(TEXT("coplanar exterior overlap source=%s face=%d expected=%.17g covered=%.17g delta=%.17g %s%s"),
					*Building.Id, F, Expected, Covered, Covered - Expected, *Coverage, *Sources));
			}
			if (Expected - Covered > Tolerance)
			{
				const FVector Origin = Polygon[0];
				const FVector U = FVector::CrossProduct(FMath::Abs(N.Z) < .9 ? FVector::UpVector : FVector::ForwardVector, N).GetSafeNormal();
				const FVector V = FVector::CrossProduct(N, U);
				const auto Project = [&](const FVector& P) { const FVector D = P - Origin; return FVector2D(FVector::DotProduct(D, U), FVector::DotProduct(D, V)); };
				TArray<FVector2D> Boundary;
				for (const FVector& P : Polygon) { Boundary.Add(Project(P)); }
				TArray<TArray<FVector2D>> Exterior;
				for (int32 I = Before; I < Faces.Num(); ++I)
				{
					TArray<FVector2D>& Patch = Exterior.AddDefaulted_GetRef();
					for (const FCorner& C : Faces[I].Corners) { Patch.Add(Project(C.Position)); }
				}
				TArray<TArray<FVector2D>> Interior;
				if (!DublinDetail::PartitionFaceRemainder(Boundary, Exterior, Interior, Error)) { return false; }
				for (const auto& Patch : Interior)
				{
					FFace Face; Face.Normal = N;
					FVector PatchCenter = FVector::ZeroVector;
					for (const FVector2D& P : Patch) { PatchCenter += Origin + U * P.X + V * P.Y; }
					PatchCenter /= Patch.Num();
					if (!InsideSourceSolid(Building, PatchCenter - N * .001))
					{
						return Fail(Error, FString::Printf(TEXT("uncovered mixed-face region lies outside the source solid/courtyard source=%s face=%d center=%s"),
							*Building.Id, F, *PatchCenter.ToString()));
					}
					for (const FVector2D& P : Patch)
					{
						const FVector Position = Origin + U * P.X + V * P.Y;
						if (!InsideSourceSolid(Building, FMath::Lerp(Position, PatchCenter, .01) - N * .001))
						{
							return Fail(Error, TEXT("uncovered mixed-face boundary crosses the source exterior or courtyard"));
						}
						const FVector2D UV = P / 100;
						Face.Corners.Add({Position, UV, UV, FLinearColor::White});
					}
					Faces.Add(MoveTemp(Face));
				}
			}
		}
		else
		{
			FFace Face; Face.Normal = N;
			const FVector U = FMath::Abs(N.Z) < .9 ? FVector::CrossProduct(FVector::UpVector, N).GetSafeNormal() : FVector::ForwardVector;
			const FVector V = FVector::CrossProduct(N, U);
			for (const FVector& P : Polygon)
			{
				const FVector2D UV(FVector::DotProduct(P, U) / 100, FVector::DotProduct(P, V) / 100);
				Face.Corners.Add({P, UV, UV, FLinearColor::White});
			}
			Faces.Add(MoveTemp(Face));
		}
	}
	return true;
}

bool DublinStructural::MakeConvexMeshDescription(const FDublinCityBuilding& Building,
	const TArray<TArray<FVector>>& InputFaces, FMeshDescription& Mesh, FString& Error)
{
	TArray<FFace> Faces;
	return MakeConvexAttributedFaces(Building, InputFaces, Faces, Error) && WriteFaces(Faces, Mesh, Error);
}

static bool WriteConformingFloatPiece(const FDublinCityBuilding& Building, const TArray<FFace>& Input,
	const FTransform& Frame, FMeshDescription& Mesh, FString& Error)
{
	struct FVertex { FVector3f Position; FVector Intended; double Error; };
	TArray<FVertex> Vertices;
	TMap<FVector3f, int32> VertexIds;
	const auto AddVertex = [&](const FVector& World) -> int32
	{
		const FVector Intended = Frame.InverseTransformPosition(World);
		const FVector3f Position(Intended);
		if (FVector::Distance(Frame.TransformPosition(FVector(Position)), World) > DublinStructural::ContactToleranceCm) { return INDEX_NONE; }
		const double Bound = FVector::Distance(FVector(Position), Intended);
		if (const int32* Existing = VertexIds.Find(Position))
		{
			Vertices[*Existing].Error = FMath::Max(Vertices[*Existing].Error, Bound); return *Existing;
		}
		const int32 I = Vertices.Add({Position, Intended, Bound});
		VertexIds.Add(Position, I); return I;
	};
	const auto SourceAttributes = [&](const FFace& Face, FCorner& Corner)
	{
		if (Face.Material >= 3) { return true; }
		if (!Building.MaterialIds.IsValidIndex(Face.SourceTriangle)) { return false; }
		const int32 T = Face.SourceTriangle;
		const FIntVector V(Building.Mesh.Triangles[T * 3], Building.Mesh.Triangles[T * 3 + 1], Building.Mesh.Triangles[T * 3 + 2]);
		const FVector World = Frame.TransformPosition(Corner.Position);
		UE::Geometry::TDistPoint3Triangle3<double> D(World, UE::Geometry::FTriangle3d(Building.Mesh.VerticesCm[V.X], Building.Mesh.VerticesCm[V.Y], Building.Mesh.VerticesCm[V.Z]));
		if (D.GetSquared() > DublinStructural::ContactToleranceCm * DublinStructural::ContactToleranceCm) { return false; }
		const FVector W = D.TriangleBaryCoords;
		Corner.UV1 = Building.Mesh.UV[V.X] * W.X + Building.Mesh.UV[V.Y] * W.Y + Building.Mesh.UV[V.Z] * W.Z;
		Corner.UV0 = Face.Material == 0 ? DublinCity::AerialUV(World + Building.PivotCm) : Corner.UV1;
		Corner.Color = DublinDestruction::SourceVertexLinearColor(Building.Mesh, V.X) * static_cast<float>(W.X) +
			DublinDestruction::SourceVertexLinearColor(Building.Mesh, V.Y) * static_cast<float>(W.Y) +
			DublinDestruction::SourceVertexLinearColor(Building.Mesh, V.Z) * static_cast<float>(W.Z);
		return true;
	};
	TArray<FFace> Faces = Input;
	TArray<FCorner> Centers; Centers.SetNum(Faces.Num());
	TArray<int32> CenterIds; CenterIds.Init(INDEX_NONE, Faces.Num());
	for (int32 F = 0; F < Faces.Num(); ++F)
	{
		FFace& Face = Faces[F];
		Face.Normal = Frame.InverseTransformVectorNoScale(Face.Normal);
		for (FCorner& C : Face.Corners)
		{
			C.VertexId = AddVertex(C.Position);
			if (C.VertexId == INDEX_NONE) { return Fail(Error, TEXT("float frame exceeds the certified source-position error")); }
			C.Position = FVector(Vertices[C.VertexId].Position);
		}
		for (int32 I = Face.Corners.Num() - 1; I >= 0 && Face.Corners.Num() > 1; --I)
		{
			if (Face.Corners[I].VertexId == Face.Corners[(I + 1) % Face.Corners.Num()].VertexId) { Face.Corners.RemoveAt(I); }
		}
		if (Face.Corners.Num() < 3) { return Fail(Error, TEXT("float frame collapses a positive double face")); }
	}
	const auto Conform = [&](TArray<FCorner>& Corners, const FFace& Face)
	{
		TArray<FCorner> Result;
		for (int32 I = 0; I < Corners.Num(); ++I)
		{
			const FCorner A = Corners[I], B = Corners[(I + 1) % Corners.Num()];
			const FVector Delta = B.Position - A.Position;
			if (Delta.SizeSquared() == 0) { continue; }
			TArray<TPair<double, int32>> Splits;
			for (int32 V = 0; V < Vertices.Num(); ++V)
			{
				if (V == A.VertexId || V == B.VertexId) { continue; }
				const FVector P(Vertices[V].Position);
				const double T = FVector::DotProduct(P - A.Position, Delta) / Delta.SizeSquared();
				if (T <= 0 || T >= 1) { continue; }
				const FVector IntendedDelta = Vertices[B.VertexId].Intended - Vertices[A.VertexId].Intended;
				const double IntendedLength = IntendedDelta.SizeSquared();
				bool bIntendedSubdivision = false;
				if (IntendedLength > 0)
				{
					const double Along = FVector::DotProduct(Vertices[V].Intended - Vertices[A.VertexId].Intended, IntendedDelta) / IntendedLength;
					bIntendedSubdivision = Along > 0 && Along < 1 &&
						(Vertices[A.VertexId].Intended + Along * IntendedDelta).Equals(Vertices[V].Intended, 1.e-7);
				}
				const TArray<FVector> Collinearity{A.Position, B.Position, P};
				FVector CollinearNormal;
				const bool bExactlyOnFloatEdge = !DublinStructural::FindFaceNormal(MakeArrayView(Collinearity), CollinearNormal);
				if (!bIntendedSubdivision && !bExactlyOnFloatEdge) { continue; }
				const double Roundoff = Vertices[V].Error + FMath::Max(Vertices[A.VertexId].Error, Vertices[B.VertexId].Error) + 1.e-9;
				if (FVector::Distance(A.Position + T * Delta, P) <= Roundoff) { Splits.Emplace(T, V); }
			}
			Splits.Sort([](const auto& Left, const auto& Right) { return Left.Key < Right.Key; });
			Result.Add(A);
			for (const auto& Split : Splits)
			{
				if (Result.Last().VertexId == Split.Value) { continue; }
				FCorner C = Lerp(A, B, Split.Key); C.VertexId = Split.Value; C.Position = FVector(Vertices[Split.Value].Position);
				if (!SourceAttributes(Face, C)) { return false; }
				Result.Add(C);
			}
		}
		Corners = MoveTemp(Result); return true;
	};
	for (int32 Pass = 0; Pass <= Faces.Num(); ++Pass)
	{
		bool bAdded = false;
		for (int32 F = 0; F < Faces.Num(); ++F)
		{
			if (!Conform(Faces[F].Corners, Faces[F])) { return Fail(Error, TEXT("float subdivision leaves the original source surface")); }
			if (Faces[F].Corners.Num() <= 3 || CenterIds[F] != INDEX_NONE) { continue; }
			const auto& Corners = Input[F].Corners;
			double Weight = 0;
			FCorner Center{FVector::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, FLinearColor(0, 0, 0, 0)};
			for (int32 I = 1; I + 1 < Corners.Num(); ++I)
			{
				const double Area = FVector::CrossProduct(Corners[I].Position - Corners[0].Position, Corners[I + 1].Position - Corners[0].Position).Size();
				if (Area == 0) { continue; }
				const FCorner TriCenter = Lerp(Lerp(Corners[0], Corners[I], .5), Corners[I + 1], 1.0 / 3);
				Center = Weight == 0 ? TriCenter : Lerp(Center, TriCenter, Area / (Weight + Area));
				Weight += Area;
			}
			if (Weight == 0) { return Fail(Error, TEXT("float topology received an exactly zero double face")); }
			const int32 Id = AddVertex(Center.Position);
			if (Id == INDEX_NONE) { return Fail(Error, TEXT("float face center exceeds source-position error")); }
			Center.VertexId = Id; Center.Position = FVector(Vertices[Id].Position);
			if (!SourceAttributes(Faces[F], Center)) { return Fail(Error, TEXT("float face center leaves source provenance")); }
			Centers[F] = Center; CenterIds[F] = Id; bAdded = true;
		}
		if (!bAdded) { break; }
		if (Pass == Faces.Num()) { return Fail(Error, TEXT("float topology subdivision did not reach a bounded fixed point")); }
	}
	FStaticMeshAttributes Attributes(Mesh); Attributes.Register();
	auto MeshPositions = Attributes.GetVertexPositions();
	auto Normals = Attributes.GetVertexInstanceNormals();
	auto Tangents = Attributes.GetVertexInstanceTangents();
	auto Signs = Attributes.GetVertexInstanceBinormalSigns();
	auto UVs = Attributes.GetVertexInstanceUVs(); UVs.SetNumChannels(2);
	auto Colors = Attributes.GetVertexInstanceColors();
	FPolygonGroupID Groups[4];
	for (int32 I = 0; I < 4; ++I)
	{
		Groups[I] = Mesh.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Groups[I]] = FName(*FString::Printf(TEXT("Material%d"), I));
	}
	TMap<int32, FVertexID> MeshIds;
	TMap<FIntPoint, FIntPoint> Edges;
	for (int32 F = 0; F < Faces.Num(); ++F)
	{
		const FFace& Face = Faces[F];
		const bool bFan = CenterIds[F] != INDEX_NONE;
		for (int32 I = 0; I < (bFan ? Face.Corners.Num() : 1); ++I)
		{
			const FCorner T[3] = {bFan ? Centers[F] : Face.Corners[0],
				bFan ? Face.Corners[I] : Face.Corners[1], bFan ? Face.Corners[(I + 1) % Face.Corners.Num()] : Face.Corners[2]};
			const TArray<FVector> Local{T[0].Position, T[1].Position, T[2].Position};
			FVector N;
			if (!DublinStructural::FindFaceNormal(MakeArrayView(Local), N)) { continue; }
			const double Alignment = FVector::DotProduct(N, Face.Normal);
			if (Alignment <= 0 || (Face.Material < 3 && Alignment < .999))
			{
				return Fail(Error, FString::Printf(TEXT("float frame fails authored face winding/normal face=%d material=%d alignment=%.17g ids=(%d,%d,%d) points=%s/%s/%s"),
					F, Face.Material, Alignment, T[0].VertexId, T[1].VertexId, T[2].VertexId,
					*T[0].Position.ToString(), *T[1].Position.ToString(), *T[2].Position.ToString()));
			}
			TArray<FVertexInstanceID> Instances;
			for (int32 K = 0; K < 3; ++K)
			{
				FCorner Expected = T[K];
				if (!SourceAttributes(Face, Expected) || !T[K].UV0.Equals(Expected.UV0, 2.e-4) ||
					!T[K].UV1.Equals(Expected.UV1, 2.e-4) || !T[K].Color.Equals(Expected.Color, 2.e-4f))
				{
					return Fail(Error, TEXT("float frame changes source UV or color provenance"));
				}
				FVertexID Vertex;
				if (const FVertexID* Existing = MeshIds.Find(T[K].VertexId)) { Vertex = *Existing; }
				else { Vertex = Mesh.CreateVertex(); MeshIds.Add(T[K].VertexId, Vertex); MeshPositions[Vertex] = Vertices[T[K].VertexId].Position; }
				const FVertexInstanceID Instance = Mesh.CreateVertexInstance(Vertex);
				Instances.Add(Instance); Normals[Instance] = FVector3f(Face.Normal);
				Tangents[Instance] = FVector3f(FMath::Abs(Face.Normal.Z) < .9 ? FVector::CrossProduct(FVector::UpVector, Face.Normal).GetSafeNormal() : FVector::ForwardVector);
				Signs[Instance] = 1; Colors[Instance] = FVector4f(T[K].Color);
				UVs.Set(Instance, 0, FVector2f(T[K].UV0)); UVs.Set(Instance, 1, FVector2f(T[K].UV1));
				const int32 A = T[K].VertexId, B = T[(K + 1) % 3].VertexId;
				FIntPoint& Edge = Edges.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)), FIntPoint::ZeroValue);
				++Edge.X; Edge.Y += A < B ? 1 : -1;
			}
			Mesh.CreatePolygon(Groups[Face.Material], Instances);
		}
	}
	if (Edges.IsEmpty()) { return Fail(Error, TEXT("float frame emitted no positive-area surface")); }
	for (const auto& Edge : Edges)
	{
		if (Edge.Value != FIntPoint(2, 0))
		{
			return Fail(Error, FString::Printf(TEXT("shared final-float subdivision topology is not closed edge=(%d,%d) incidence=%d balance=%d points=%s/%s"),
				Edge.Key.X, Edge.Key.Y, Edge.Value.X, Edge.Value.Y,
				*FVector(Vertices[Edge.Key.X].Position).ToString(), *FVector(Vertices[Edge.Key.Y].Position).ToString()));
		}
	}
	Error.Reset(); return true;
}

bool DublinStructural::AppendDoubleClippedCells(const FDublinCityBuilding& Building,
	const TArray<TArray<FVector>>& InputFaces, const FPlanarCells& Cells, FGeometryCollection& Geometry,
	TArray<int32>& LeafBones, double& MemberVolumeCm3, FString& Error)
{
	LeafBones.Reset(); MemberVolumeCm3 = 0;
	if (Cells.NumCells < 1) { return Fail(Error, TEXT("double clipping requires explicit cells")); }
	TArray<FFace> Source;
	if (!MakeConvexAttributedFaces(Building, InputFaces, Source, Error)) { return false; }
	TArray<FVector> Positions;
	TArray<bool> Protected;
	TMap<FVector, int32> Exact;
	UE::Geometry::TPointHashGrid3<int32, double> Grid(.01, INDEX_NONE);
	const auto AddPoint = [&](const FVector& P, bool bOriginal)
	{
		if (const int32* Existing = Exact.Find(P)) { return *Existing; }
		if (!bOriginal)
		{
			const auto Near = Grid.FindNearestInRadius(P, 1.e-7, [&](int32 I) { return FVector::DistSquared(P, Positions[I]); });
			if (Near.Key != INDEX_NONE) { return Near.Key; }
		}
		const int32 I = Positions.Add(P);
		Protected.Add(bOriginal); Exact.Add(P, I); Grid.InsertPointUnsafe(I, P);
		return I;
	};
	for (const FVector& P : Building.Mesh.VerticesCm) { AddPoint(P, true); }
	for (FFace& Face : Source)
	{
		for (FCorner& Corner : Face.Corners)
		{
			Corner.VertexId = AddPoint(Corner.Position, false);
			Corner.Position = Positions[Corner.VertexId];
			Protected[Corner.VertexId] = true;
		}
	}
	const auto Conform = [&](TArray<FFace>& Faces)
	{
		TArray<int32> Vertices;
		for (const FFace& Face : Faces) { for (const FCorner& C : Face.Corners) { Vertices.AddUnique(C.VertexId); } }
		for (FFace& Face : Faces)
		{
			TArray<FCorner> Corners;
			for (int32 I = 0; I < Face.Corners.Num(); ++I)
			{
				const FCorner A = Face.Corners[I], B = Face.Corners[(I + 1) % Face.Corners.Num()];
				const FVector Delta = B.Position - A.Position;
				if (Delta.SizeSquared() == 0) { continue; }
				TArray<TPair<double, int32>> Splits;
				for (int32 V : Vertices)
				{
					if (V == A.VertexId || V == B.VertexId) { continue; }
					const double T = FVector::DotProduct(Positions[V] - A.Position, Delta) / Delta.SizeSquared();
					if (T > 1.e-10 && T < 1 - 1.e-10 && (A.Position + T * Delta).Equals(Positions[V], 1.e-7)) { Splits.Emplace(T, V); }
				}
				Splits.Sort([](const auto& Left, const auto& Right) { return Left.Key < Right.Key; });
				Corners.Add(A);
				for (const auto& Split : Splits)
				{
					FCorner C = Lerp(A, B, Split.Key); C.VertexId = Split.Value; C.Position = Positions[Split.Value];
					if (Corners.Last().VertexId != C.VertexId) { Corners.Add(C); }
				}
			}
			Face.Corners = MoveTemp(Corners);
		}
	};
	const auto ClosedVolume = [&](const TArray<FFace>& Faces, double& Volume)
	{
		Volume = 0;
		TMap<FIntPoint, FIntPoint> Edges;
		FVector Reference = FVector::ZeroVector; int32 Count = 0;
		for (const FFace& Face : Faces) { for (const FCorner& C : Face.Corners) { Reference += C.Position; ++Count; } }
		if (Count == 0) { return false; }
		Reference /= Count;
		for (const FFace& Face : Faces)
		{
			if (Face.Corners.Num() < 3) { return false; }
			for (int32 I = 0; I < Face.Corners.Num(); ++I)
			{
				const int32 A = Face.Corners[I].VertexId, B = Face.Corners[(I + 1) % Face.Corners.Num()].VertexId;
				if (A == B) { return false; }
				FIntPoint& Edge = Edges.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)), FIntPoint::ZeroValue);
				++Edge.X; Edge.Y += A < B ? 1 : -1;
			}
			for (int32 I = 1; I + 1 < Face.Corners.Num(); ++I)
			{
				Volume += FVector::DotProduct(Face.Corners[0].Position - Reference,
					FVector::CrossProduct(Face.Corners[I + 1].Position - Reference, Face.Corners[I].Position - Reference)) / 6;
			}
		}
		for (const auto& Edge : Edges) { if (Edge.Value != FIntPoint(2, 0)) { return false; } }
		return FMath::IsFinite(Volume) && Volume > 0;
	};
	Conform(Source);
	if (!ClosedVolume(Source, MemberVolumeCm3)) { return Fail(Error, TEXT("attributed double member is not a closed positive solid")); }
	FVector BasisN = FVector::UpVector, BasisU = FVector::ForwardVector;
	double LargestFace = 0;
	for (const FFace& Face : Source)
	{
		double Area = 0;
		for (int32 I = 1; I + 1 < Face.Corners.Num(); ++I)
		{
			Area += FVector::CrossProduct(Face.Corners[I].Position - Face.Corners[0].Position,
				Face.Corners[I + 1].Position - Face.Corners[0].Position).Size();
		}
		if (Area <= LargestFace) { continue; }
		LargestFace = Area; BasisN = Face.Normal;
		double Longest = 0;
		for (int32 I = 0; I < Face.Corners.Num(); ++I)
		{
			const FVector Edge = Face.Corners[(I + 1) % Face.Corners.Num()].Position - Face.Corners[I].Position;
			if (Edge.SizeSquared() > Longest) { Longest = Edge.SizeSquared(); BasisU = Edge.GetSafeNormal(0); }
		}
	}
	BasisU = (BasisU - FVector::DotProduct(BasisU, BasisN) * BasisN).GetSafeNormal();
	const FVector BasisV = FVector::CrossProduct(BasisN, BasisU).GetSafeNormal();
	TMap<FIntVector, int32> Intersections;
	TArray<TArray<FFace>> Pieces;
	double ClippedVolume = 0;
	for (int32 Cell = 0; Cell < Cells.NumCells; ++Cell)
	{
		TArray<FFace> Faces = Source;
		for (int32 PlaneIndex = 0; PlaneIndex < Cells.Planes.Num(); ++PlaneIndex)
		{
			const auto Pair = Cells.PlaneCells[PlaneIndex];
			if (Pair.Key != Cell && Pair.Value != Cell) { continue; }
			const double Sign = Pair.Key == Cell ? 1.0 : -1.0;
			const FPlane Plane = Cells.Planes[PlaneIndex] * Sign;
			const FVector Outward(Plane.X, Plane.Y, Plane.Z);
			bool bInside = false, bOutside = false;
			for (const FFace& Face : Faces)
			{
				for (const FCorner& C : Face.Corners)
				{
					const double D = Plane.PlaneDot(C.Position);
					bInside |= D < -1.e-8; bOutside |= D > 1.e-8;
				}
			}
			if (!bOutside) { continue; }
			if (!bInside) { Faces.Reset(); break; }
			TArray<FFace> Kept;
			TArray<int32> Section;
			for (const FFace& Face : Faces)
			{
				FFace Result = Face; Result.Corners.Reset();
				for (int32 I = 0; I < Face.Corners.Num(); ++I)
				{
					const FCorner& A = Face.Corners[I];
					const FCorner& B = Face.Corners[(I + 1) % Face.Corners.Num()];
					double DA = Plane.PlaneDot(A.Position), DB = Plane.PlaneDot(B.Position);
					if (FMath::Abs(DA) <= 1.e-8) { DA = 0; }
					if (FMath::Abs(DB) <= 1.e-8) { DB = 0; }
					if (DA <= 0) { Result.Corners.Add(A); if (DA == 0) { Section.AddUnique(A.VertexId); } }
					if ((DA < 0 && DB > 0) || (DA > 0 && DB < 0))
					{
						const double T = DA / (DA - DB);
						FCorner C = Lerp(A, B, T);
						const FIntVector Key(FMath::Min(A.VertexId, B.VertexId), FMath::Max(A.VertexId, B.VertexId), PlaneIndex);
						if (const int32* Existing = Intersections.Find(Key)) { C.VertexId = *Existing; }
						else
						{
							C.Position -= Outward * Plane.PlaneDot(C.Position);
							C.VertexId = AddPoint(C.Position, false); Intersections.Add(Key, C.VertexId);
						}
						C.Position = Positions[C.VertexId]; Result.Corners.Add(C); Section.AddUnique(C.VertexId);
					}
				}
				for (int32 I = Result.Corners.Num() - 1; I >= 0 && Result.Corners.Num() > 1; --I)
				{
					if (Result.Corners[I].VertexId == Result.Corners[(I + 1) % Result.Corners.Num()].VertexId) { Result.Corners.RemoveAt(I); }
				}
				if (Result.Corners.Num() >= 3)
				{
					TArray<FVector> P; for (const FCorner& C : Result.Corners) { P.Add(C.Position); }
					FVector N;
					if (FindFaceNormal(MakeArrayView(P), N)) { Kept.Add(MoveTemp(Result)); }
				}
			}
			if (Section.Num() < 3) { return Fail(Error, TEXT("double half-space cut has no closed cap boundary")); }
			FVector Center = FVector::ZeroVector;
			for (int32 I : Section) { Center += Positions[I]; }
			Center /= Section.Num();
			const FVector U = FVector::CrossProduct(FMath::Abs(Outward.Z) < .9 ? FVector::UpVector : FVector::ForwardVector, Outward).GetSafeNormal();
			const FVector V = FVector::CrossProduct(Outward, U);
			Section.Sort([&](int32 A, int32 B)
			{
				const FVector PA = Positions[A] - Center, PB = Positions[B] - Center;
				return FMath::Atan2(FVector::DotProduct(PA, V), FVector::DotProduct(PA, U)) <
					FMath::Atan2(FVector::DotProduct(PB, V), FVector::DotProduct(PB, U));
			});
			Algo::Reverse(Section);
			FFace Cap; Cap.Normal = Outward; Cap.bGeneratedCut = true;
			for (int32 I : Section)
			{
				const FVector P = Positions[I];
				const FVector2D UV(FVector::DotProduct(P, U) / 100, FVector::DotProduct(P, V) / 100);
				FCorner C{P, UV, UV, FLinearColor::White}; C.VertexId = I; Cap.Corners.Add(C);
			}
			Kept.Add(MoveTemp(Cap)); Faces = MoveTemp(Kept);
		}
		Conform(Faces);
		double Volume = 0;
		if (!ClosedVolume(Faces, Volume)) { return Fail(Error, FString::Printf(TEXT("double cell %d is not closed with positive volume"), Cell)); }
		ClippedVolume += Volume; Pieces.Add(MoveTemp(Faces));
	}
	if (FMath::Abs(ClippedVolume - MemberVolumeCm3) > FMath::Max(1.0, MemberVolumeCm3 * 1.e-4))
	{
		return Fail(Error, TEXT("double cells do not conserve the original attributed member"));
	}
	TArray<int32> Parent;
	for (int32 I = 0; I < Positions.Num(); ++I) { Parent.Add(I); }
	const auto Root = [&](int32 I) { while (Parent[I] != I) { I = Parent[I]; } return I; };
	const auto CanMerge = [&](int32 From, int32 To)
	{
		if (Protected[From]) { return false; }
		for (int32 I = 0; I < Positions.Num(); ++I)
		{
			if (Root(I) == From && FVector::Distance(Positions[I], Positions[To]) > ContactToleranceCm) { return false; }
		}
		for (const auto& Faces : Pieces)
		{
			for (const FFace& Face : Faces)
			{
				if (Face.bGeneratedCut) { continue; }
				for (const FCorner& C : Face.Corners)
				{
					if (Root(C.VertexId) == From && FMath::Abs(FVector::DotProduct(Positions[To] - Face.Corners[0].Position, Face.Normal)) > 1.e-6) { return false; }
				}
			}
		}
		return true;
	};
	for (const auto& Faces : Pieces)
	{
		for (const FFace& Face : Faces)
		{
			for (int32 I = 0; I < Face.Corners.Num(); ++I)
			{
				int32 A = Root(Face.Corners[I].VertexId), B = Root(Face.Corners[(I + 1) % Face.Corners.Num()].VertexId);
				if (A == B || FVector::Distance(Positions[A], Positions[B]) > ContactToleranceCm) { continue; }
				if (Protected[B]) { Swap(A, B); }
				if (CanMerge(B, A)) { Parent[B] = A; }
				else if (CanMerge(A, B)) { Parent[A] = B; }
			}
		}
	}
	double FinalVolume = 0;
	for (int32 Cell = 0; Cell < Pieces.Num(); ++Cell)
	{
		auto& Faces = Pieces[Cell];
		for (int32 F = Faces.Num() - 1; F >= 0; --F)
		{
			FFace& Face = Faces[F];
			for (FCorner& C : Face.Corners)
			{
				C.VertexId = Root(C.VertexId); C.Position = Positions[C.VertexId];
				if (Face.Material < 3 && Face.SourceTriangle != INDEX_NONE)
				{
					const int32 T = Face.SourceTriangle;
					const FIntVector V(Building.Mesh.Triangles[T * 3], Building.Mesh.Triangles[T * 3 + 1], Building.Mesh.Triangles[T * 3 + 2]);
					UE::Geometry::TDistPoint3Triangle3<double> D(C.Position, UE::Geometry::FTriangle3d(Building.Mesh.VerticesCm[V.X], Building.Mesh.VerticesCm[V.Y], Building.Mesh.VerticesCm[V.Z]));
					D.GetSquared(); const FVector W = D.TriangleBaryCoords;
					C.UV1 = Building.Mesh.UV[V.X] * W.X + Building.Mesh.UV[V.Y] * W.Y + Building.Mesh.UV[V.Z] * W.Z;
					C.UV0 = Face.Material == 0 ? DublinCity::AerialUV(C.Position + Building.PivotCm) : C.UV1;
					C.Color = DublinDestruction::SourceVertexLinearColor(Building.Mesh, V.X) * static_cast<float>(W.X) +
						DublinDestruction::SourceVertexLinearColor(Building.Mesh, V.Y) * static_cast<float>(W.Y) +
						DublinDestruction::SourceVertexLinearColor(Building.Mesh, V.Z) * static_cast<float>(W.Z);
				}
			}
			for (int32 I = Face.Corners.Num() - 1; I >= 0 && Face.Corners.Num() > 1; --I)
			{
				if (Face.Corners[I].VertexId == Face.Corners[(I + 1) % Face.Corners.Num()].VertexId) { Face.Corners.RemoveAt(I); }
			}
			TArray<FVector> P; for (const FCorner& C : Face.Corners) { P.Add(C.Position); }
			FVector N;
			if (P.Num() < 3 || !FindFaceNormal(MakeArrayView(P), N)) { Faces.RemoveAt(F); }
		}
		Conform(Faces);
		double DoubleVolume = 0;
		if (!ClosedVolume(Faces, DoubleVolume)) { return Fail(Error, TEXT("generated-seam coalescence broke the closed double solid")); }
		FBox Bounds(ForceInit);
		for (const FFace& Face : Faces) { for (const FCorner& C : Face.Corners) { Bounds += C.Position; } }
		const FVector Center = Bounds.GetCenter();
		FMatrix Matrix = FMatrix::Identity;
		Matrix.SetAxes(&BasisU, &BasisV, &BasisN, &Center);
		const FTransform Desired(Matrix);
		const TArray<FTransform> Frames{FTransform(FVector(FVector3f(Center))), FTransform(FTransform3f(Desired))};
		bool bEmitted = false;
		TArray<FString> Diagnostics;
		for (int32 Candidate = 0; Candidate < Frames.Num(); ++Candidate)
		{
			FMeshDescription Mesh;
			FString CandidateError;
			if (!WriteConformingFloatPiece(Building, Faces, Frames[Candidate], Mesh, CandidateError))
			{
				Diagnostics.Add(FString::Printf(TEXT("frame=%d %s"), Candidate, *CandidateError)); continue;
			}
			FGeometryCollection Piece;
			FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, Frames[Candidate], &Piece, nullptr, false, false, false);
			double Volume = 0;
			if (!ValidateLeafGeometry(Piece, 0, Volume, CandidateError))
			{
				Diagnostics.Add(FString::Printf(TEXT("frame=%d %s"), Candidate, *CandidateError)); continue;
			}
			if (FMath::Abs(Volume - DoubleVolume) > FMath::Max(1.0, DoubleVolume * 1.e-4))
			{
				Diagnostics.Add(FString::Printf(TEXT("frame=%d floatVolume=%.17g doubleVolume=%.17g"), Candidate, Volume, DoubleVolume)); continue;
			}
			const int32 Bone = Geometry.Transform.Num();
			if (!FGeometryCollectionEngineConversion::AppendGeometryCollection(&Piece, 0, FTransform::Identity, &Geometry, false))
			{
				return Fail(Error, TEXT("could not append the qualified final-float piece"));
			}
			LeafBones.Add(Bone); FinalVolume += Volume; bEmitted = true; break;
		}
		if (!bEmitted) { return Fail(Error, FString::Printf(TEXT("double cell=%d has no qualified float representation: %s"), Cell, *FString::Join(Diagnostics, TEXT(" | ")))); }
	}
	if (FMath::Abs(FinalVolume - MemberVolumeCm3) > FMath::Max(1.0, MemberVolumeCm3 * 1.e-4))
	{
		return Fail(Error, TEXT("final convex pieces do not conserve the double member volume ledger"));
	}
	Geometry.ReindexMaterials();
	for (int32 Bone : LeafBones)
	{
		const int32 G = Geometry.TransformToGeometryIndex[Bone];
		for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F) { Geometry.Internal[F] = Geometry.MaterialID[F] == 3; }
	}
	Error.Reset(); return true;
}

bool DublinStructural::MakeMemberMeshDescription(const FDublinCityBuilding& Building, const FAssembly& Assembly,
	const FMember& Member, FMeshDescription& Mesh, FString& Error)
{
	if (!Member.bSourcePlaneExterior) { return MakeMemberMesh(Building, Assembly, Member, Mesh, Error); }
	TArray<TArray<FVector>> Faces;
	TArray<FVector> Bottom, Top;
	for (const FVector2D& P : Member.Polygon)
	{
		Bottom.Emplace(P.X, P.Y, Member.MinZ + FVector2D::DotProduct(Member.BottomSlope, P));
		Top.Emplace(P.X, P.Y, Member.MaxZ + FVector2D::DotProduct(Member.TopSlope, P));
	}
	Faces.Add(Bottom); Faces.Add(Top);
	for (int32 I = 0; I < Bottom.Num(); ++I)
	{
		const int32 J = (I + 1) % Bottom.Num();
		Faces.Add({Bottom[I], Bottom[J], Top[J], Top[I]});
	}
	return MakeConvexMeshDescription(Building, Faces, Mesh, Error);
}

bool DublinStructural::ValidateLeafGeometry(FGeometryCollection& Geometry, int32 Bone, double& Volume, FString& Error)
{
	return ValidateLeaf(Geometry, Bone, Volume, Error);
}

bool DublinStructural::RemoveExactZeroAreaFaces(FGeometryCollection& Geometry, int32 Bone, int32& Removed, FString& Error)
{
	Removed = 0;
	if (!Geometry.TransformToGeometryIndex.IsValidIndex(Bone)) { return Fail(Error, TEXT("zero-area cleanup requires a valid leaf")); }
	const int32 G = Geometry.TransformToGeometryIndex[Bone];
	if (!Geometry.FaceStart.IsValidIndex(G)) { return Fail(Error, TEXT("zero-area cleanup requires owned geometry")); }
	TArray<int32> Faces;
	TSet<int32> CandidateVertices;
	for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
	{
		const FIntVector T = Geometry.Indices[F];
		TArray<FVector> Points;
		for (int32 K = 0; K < 3; ++K)
		{
			if (!Geometry.Vertex.IsValidIndex(T[K]) || Geometry.Vertex[T[K]].ContainsNaN())
			{
				return Fail(Error, TEXT("zero-area cleanup refuses invalid/nonfinite source corners"));
			}
			Points.Add(FVector(Geometry.Vertex[T[K]]));
		}
		FVector Normal;
		if (!FindFaceNormal(MakeArrayView(Points), Normal))
		{
			Faces.Add(F);
			for (int32 K = 0; K < 3; ++K) { CandidateVertices.Add(T[K]); }
		}
	}
	if (!Faces.IsEmpty())
	{
		Removed = Faces.Num();
		Geometry.RemoveElements(FGeometryCollection::FacesGroup, Faces);
		for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
		{
			const FIntVector T = Geometry.Indices[F];
			for (int32 K = 0; K < 3; ++K) { CandidateVertices.Remove(T[K]); }
		}
		TArray<int32> Unused = CandidateVertices.Array(); Unused.Sort();
		Geometry.RemoveElements(FGeometryCollection::VerticesGroup, Unused);
		Geometry.BoundingBox[G].Init();
		for (int32 V = Geometry.VertexStart[G]; V < Geometry.VertexStart[G] + Geometry.VertexCount[G]; ++V)
		{
			Geometry.BoundingBox[G] += FVector(Geometry.Vertex[V]);
		}
		Geometry.ReindexMaterials();
	}
	Error.Reset();
	return true;
}

bool DublinStructural::NormalizeGeometryOrder(FGeometryCollection& Geometry, FString& Error)
{
	TArray<int32> Order;
	TSet<int32> GeometryBones;
	for (int32 G = 0; G < Geometry.TransformIndex.Num(); ++G)
	{
		const int32 Bone = Geometry.TransformIndex[G];
		if (!Geometry.Transform.IsValidIndex(Bone) || GeometryBones.Contains(Bone))
		{
			return Fail(Error, TEXT("cook ordering requires one geometry block per valid transform"));
		}
		GeometryBones.Add(Bone); Order.Add(G);
	}
	Order.Sort([&](int32 A, int32 B) { return Geometry.TransformIndex[A] < Geometry.TransformIndex[B]; });
	Geometry.ReorderElements(FGeometryCollection::GeometryGroup, Order);
	// This reverse mapping has no GeometryGroup dependency and is not remapped by ReorderElements.
	for (int32 Bone = 0; Bone < Geometry.TransformToGeometryIndex.Num(); ++Bone) { Geometry.TransformToGeometryIndex[Bone] = INDEX_NONE; }
	int32 VertexStart = 0, FaceStart = 0;
	for (int32 G = 0; G < Geometry.TransformIndex.Num(); ++G)
	{
		const int32 Bone = Geometry.TransformIndex[G];
		Geometry.TransformToGeometryIndex[Bone] = G;
		if (Geometry.VertexStart[G] != VertexStart || Geometry.FaceStart[G] != FaceStart)
		{
			return Fail(Error, TEXT("cook ordering did not preserve contiguous geometry blocks"));
		}
		VertexStart += Geometry.VertexCount[G]; FaceStart += Geometry.FaceCount[G];
	}
	if (VertexStart != Geometry.Vertex.Num() || FaceStart != Geometry.Indices.Num())
	{
		return Fail(Error, TEXT("cook ordering found unowned vertices or faces"));
	}
	int32 Previous = INDEX_NONE;
	for (int32 F = 0; F < Geometry.Indices.Num(); ++F)
	{
		if (!Geometry.Visible[F]) { continue; }
		const FIntVector T = Geometry.Indices[F];
		const int32 Bone = Geometry.BoneMap[T.X];
		if (!Geometry.Transform.IsValidIndex(Bone) || Bone < Previous ||
			Geometry.BoneMap[T.Y] != Bone || Geometry.BoneMap[T.Z] != Bone)
		{
			return Fail(Error, TEXT("visible face bone IDs are not monotonically ordered for Chaos cooking"));
		}
		Previous = Bone;
	}
	Error.Reset();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
// Historical failure reproductions only. Detailed authoring uses double half-spaces, not these retired float-cut repairs.
enum class EExteriorSeamMatch { Absent, Unique, Ambiguous };

static EExteriorSeamMatch AttributeExteriorSeam(const FDublinCityBuilding& Building, const FTransform& ToSource,
	const TArray<FVector>& Loop, TArray<FFace>& Patches, FString& Error)
{
	Patches.Reset();
	FVector LocalNormal;
	if (Loop.Num() < 3 || Loop.Num() > 4 || !DublinStructural::FindFaceNormal(MakeArrayView(Loop), LocalNormal))
	{
		Error = TEXT("exterior seam has no supported oriented polygon");
		return EExteriorSeamMatch::Absent;
	}
	const FVector SourceNormal = ToSource.TransformVectorNoScale(LocalNormal);
	int32 Owner = INDEX_NONE;
	TArray<FVector> Barycentrics;
	for (int32 T = 0; T < Building.MaterialIds.Num(); ++T)
	{
		const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3]];
		const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 1]];
		const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[T * 3 + 2]];
		// Use the same winding/normal bound as final exterior validation: skinny float-space
		// triangles can have noisier computed normals despite sub-millimetre source distances.
		if (Building.MaterialIds[T] > 2 || FVector::DotProduct(SourceNormal, DublinCity::ClockwiseNormal(A, B, C)) < .999) { continue; }
		bool bCovered = true;
		TArray<FVector> Candidate;
		for (const FVector& P : Loop)
		{
			UE::Geometry::TDistPoint3Triangle3<double> Distance(ToSource.TransformPosition(P), UE::Geometry::FTriangle3d(A, B, C));
			bCovered &= Distance.GetSquared() <= DublinStructural::ContactToleranceCm * DublinStructural::ContactToleranceCm;
			Candidate.Add(Distance.TriangleBaryCoords);
		}
		if (!bCovered) { continue; }
		if (Owner != INDEX_NONE)
		{
			Error = FString::Printf(TEXT("ambiguous original-source exterior seam attribution: triangles=%d,%d"), Owner, T);
			return EExteriorSeamMatch::Ambiguous;
		}
		Owner = T; Barycentrics = MoveTemp(Candidate);
	}
	if (Owner == INDEX_NONE)
	{
		Error = TEXT("exterior seam is not completely covered by one unambiguous original source triangle");
		return EExteriorSeamMatch::Absent;
	}
	const FIntVector SourceIndices(Building.Mesh.Triangles[Owner * 3], Building.Mesh.Triangles[Owner * 3 + 1], Building.Mesh.Triangles[Owner * 3 + 2]);
	TArray<FCorner> Corners;
	for (int32 I = 0; I < Loop.Num(); ++I)
	{
		const FVector W = Barycentrics[I];
		const FVector2D UV = Building.Mesh.UV[SourceIndices.X] * W.X + Building.Mesh.UV[SourceIndices.Y] * W.Y + Building.Mesh.UV[SourceIndices.Z] * W.Z;
		const FLinearColor Color = DublinDestruction::SourceVertexLinearColor(Building.Mesh, SourceIndices.X) * static_cast<float>(W.X) +
			DublinDestruction::SourceVertexLinearColor(Building.Mesh, SourceIndices.Y) * static_cast<float>(W.Y) +
			DublinDestruction::SourceVertexLinearColor(Building.Mesh, SourceIndices.Z) * static_cast<float>(W.Z);
		Corners.Add({Loop[I], Building.MaterialIds[Owner] == 0 ? DublinCity::AerialUV(ToSource.TransformPosition(Loop[I]) + Building.PivotCm) : UV, UV, Color});
	}
	const TArray<TArray<FIntVector>> Options = Loop.Num() == 3 ? TArray<TArray<FIntVector>>{{{0, 1, 2}}} :
		TArray<TArray<FIntVector>>{{{0, 1, 2}, {0, 2, 3}}, {{0, 1, 3}, {1, 2, 3}}};
	for (const auto& Triangles : Options)
	{
		bool bConsistent = true;
		TArray<FFace> Candidate;
		for (const FIntVector& T : Triangles)
		{
			FFace Face; Face.Material = Building.MaterialIds[Owner]; Face.SourceTriangle = Owner;
			const TArray<FVector> Points{Loop[T.X], Loop[T.Y], Loop[T.Z]};
			bConsistent &= DublinStructural::FindFaceNormal(MakeArrayView(Points), Face.Normal) &&
				FVector::DotProduct(Face.Normal, LocalNormal) > 0;
			Face.Corners = {Corners[T.X], Corners[T.Y], Corners[T.Z]};
			Candidate.Add(MoveTemp(Face));
		}
		if (bConsistent)
		{
			// Each patch is contained in a single authoritative source triangle; its source attributes
			// are recovered barycentrically. This is not a neutral internal cap or invented exterior.
			Patches = MoveTemp(Candidate); Error.Reset(); return EExteriorSeamMatch::Unique;
		}
	}
	Error = TEXT("original-source exterior loop cannot be triangulated with consistent winding");
	return EExteriorSeamMatch::Absent;
}

bool DublinStructural::StabilizeExteriorTriangulation(const FDublinCityBuilding& Building, FGeometryCollection& Geometry,
	int32 Bone, int32& Flipped, FString& Error)
{
	Flipped = 0;
	const int32 G = Geometry.TransformToGeometryIndex[Bone];
	const FTransform Global = GeometryCollectionAlgo::GlobalMatrix(Geometry.Transform, Geometry.Parent, Bone);
	const auto UVs = GeometryCollection::UV::FindActiveUVLayers(Geometry);
	for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
	{
		if (Geometry.MaterialID[F] >= 3) { continue; }
		const FIntVector OriginalTri = Geometry.Indices[F];
		const TArray<FVector> Points{FVector(Geometry.Vertex[OriginalTri.X]), FVector(Geometry.Vertex[OriginalTri.Y]), FVector(Geometry.Vertex[OriginalTri.Z])};
		FVector Normal;
		if (!FindFaceNormal(MakeArrayView(Points), Normal)) { return Fail(Error, TEXT("exterior stabilization requires exact-zero cleanup first")); }
		const FVector Authored = FVector(Geometry.Normal[OriginalTri.X]).GetSafeNormal();
		if (FVector::DotProduct(Normal, Authored) >= .999) { continue; }
		TArray<int32> Edges{0, 1, 2};
		Edges.Sort([&](int32 A, int32 B) { return FVector::DistSquared(Points[A], Points[(A + 1) % 3]) > FVector::DistSquared(Points[B], Points[(B + 1) % 3]); });
		bool bChanged = false;
		for (int32 E : Edges)
		{
			const int32 A = OriginalTri[(E + 2) % 3], B = OriginalTri[E], C = OriginalTri[(E + 1) % 3];
			for (int32 Other = Geometry.FaceStart[G]; Other < Geometry.FaceStart[G] + Geometry.FaceCount[G] && !bChanged; ++Other)
			{
				if (Other == F || Geometry.MaterialID[Other] != Geometry.MaterialID[F]) { continue; }
				const FIntVector Neighbour = Geometry.Indices[Other];
				for (int32 K = 0; K < 3 && !bChanged; ++K)
				{
					if (Geometry.Vertex[Neighbour[K]] != Geometry.Vertex[C] || Geometry.Vertex[Neighbour[(K + 1) % 3]] != Geometry.Vertex[B]) { continue; }
					const int32 D = Neighbour[(K + 2) % 3];
					const TArray<FVector> Quad{FVector(Geometry.Vertex[A]), FVector(Geometry.Vertex[B]), FVector(Geometry.Vertex[D]), FVector(Geometry.Vertex[C])};
					TArray<FFace> Source;
					FString AttributionError;
					if (AttributeExteriorSeam(Building, Global, Quad, Source, AttributionError) != EExteriorSeamMatch::Unique || Source.Num() != 2) { continue; }
					const int32 SourceTriangle = Source[0].SourceTriangle;
					const FVector SA = Building.Mesh.VerticesCm[Building.Mesh.Triangles[SourceTriangle * 3]];
					const FVector SB = Building.Mesh.VerticesCm[Building.Mesh.Triangles[SourceTriangle * 3 + 1]];
					const FVector SC = Building.Mesh.VerticesCm[Building.Mesh.Triangles[SourceTriangle * 3 + 2]];
					const FVector SourceNormal = DublinCity::ClockwiseNormal(SA, SB, SC);
					bool bProvenance = Source[0].Material == Geometry.MaterialID[F];
					for (int32 V : TArray<int32>{A, B, C, D, Neighbour[K], Neighbour[(K + 1) % 3]})
					{
						bool bFound = false;
						for (const FFace& Patch : Source)
						{
							for (const FCorner& Corner : Patch.Corners)
							{
								if (Corner.Position != FVector(Geometry.Vertex[V])) { continue; }
								bFound |= FVector2D(UVs[0][V]).Equals(Corner.UV0, 2.e-4) && FVector2D(UVs[1][V]).Equals(Corner.UV1, 2.e-4) &&
									Geometry.Color[V].Equals(Corner.Color, 2.e-4f) &&
									FVector::DotProduct(Global.TransformVectorNoScale(FVector(Geometry.Normal[V])), SourceNormal) >= .999;
							}
						}
						bProvenance &= bFound;
					}
					const TArray<FVector> First{Quad[0], Quad[1], Quad[2]}, Second{Quad[0], Quad[2], Quad[3]};
					FVector FirstNormal, SecondNormal;
					if (!bProvenance || !FindFaceNormal(MakeArrayView(First), FirstNormal) || !FindFaceNormal(MakeArrayView(Second), SecondNormal) ||
						FVector::DotProduct(Global.TransformVectorNoScale(FirstNormal), SourceNormal) < .999 ||
						FVector::DotProduct(Global.TransformVectorNoScale(SecondNormal), SourceNormal) < .999) { continue; }
					const auto Area = [](const FVector& P, const FVector& Q, const FVector& R) { return FVector::CrossProduct(Q - P, R - P).Size() * .5; };
					const double BeforeArea = Area(Points[0], Points[1], Points[2]) +
						Area(FVector(Geometry.Vertex[Neighbour.X]), FVector(Geometry.Vertex[Neighbour.Y]), FVector(Geometry.Vertex[Neighbour.Z]));
					const double AfterArea = Area(First[0], First[1], First[2]) + Area(Second[0], Second[1], Second[2]);
					if (FMath::Abs(AfterArea - BeforeArea) > FMath::Max(1.e-8, BeforeArea * 1.e-4)) { continue; }
					Geometry.Indices[F] = FIntVector(A, B, D);
					Geometry.Indices[Other] = FIntVector(A, D, C);
					++Flipped; bChanged = true;
				}
			}
			if (bChanged) { break; }
		}
	}
	if (Flipped) { Geometry.ReindexMaterials(); }
	Error.Reset();
	return true;
}

static bool RepairLeafSeamsInternal(FGeometryCollection& Geometry, int32 Bone, FString& Error,
	double ClosedMemberVolumeCm3, bool bStitchOnly, bool bAllowResidualStitch,
	const FDublinCityBuilding* SourceBuilding = nullptr, const FTransform& ToSource = FTransform::Identity)
{
	using DublinStructural::ContactToleranceCm;
	using DublinStructural::FindFaceNormal;
	const FTransform Global = GeometryCollectionAlgo::GlobalMatrix(Geometry.Transform, Geometry.Parent, Bone);
	if (!Global.GetScale3D().Equals(FVector::OneVector, 1.e-6)) { return Fail(Error, TEXT("seam repair requires unit-scale authored leaves")); }
	const int32 G = Geometry.TransformToGeometryIndex[Bone];
	if (!Geometry.FaceStart.IsValidIndex(G)) { return Fail(Error, TEXT("seam repair requires a concrete leaf geometry")); }
	const auto UVs = GeometryCollection::UV::FindActiveUVLayers(Geometry);
	if (UVs.Num() != 2) { return Fail(Error, TEXT("seam repair requires both original UV layers")); }
	TArray<FFace> Faces;
	TArray<FIntVector> FacePositions;
	TArray<FVector> Original;
	TArray<int32> ExteriorUses;
	TMap<FVector3f, int32> PositionIds;
	TMap<FIntPoint, int32> EdgeUses;
	for (int32 F = Geometry.FaceStart[G]; F < Geometry.FaceStart[G] + Geometry.FaceCount[G]; ++F)
	{
		const FIntVector T = Geometry.Indices[F];
		FFace Face; Face.Material = Geometry.MaterialID[F];
		FIntVector IDs;
		TArray<FVector> Points;
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 V = T[K];
			const FVector3f P = Geometry.Vertex[V];
			if (const int32* Existing = PositionIds.Find(P)) { IDs[K] = *Existing; }
			else { IDs[K] = Original.Add(FVector(P)); PositionIds.Add(P, IDs[K]); ExteriorUses.Add(0); }
			ExteriorUses[IDs[K]] += Face.Material < 3;
			Points.Add(FVector(P));
			Face.Corners.Add({FVector(P), FVector2D(UVs[0][V]), FVector2D(UVs[1][V]), Geometry.Color[V]});
		}
		if (!FindFaceNormal(MakeArrayView(Points), Face.Normal)) { return Fail(Error, TEXT("seam repair encountered an already collinear source triangle")); }
		for (int32 K = 0; K < 3; ++K)
		{
			++EdgeUses.FindOrAdd(FIntPoint(FMath::Min(IDs[K], IDs[(K + 1) % 3]), FMath::Max(IDs[K], IDs[(K + 1) % 3])));
		}
		FacePositions.Add(IDs); Faces.Add(MoveTemp(Face));
	}
	TArray<FIntPoint> BoundaryEdges;
	TArray<int32> BoundaryVertices;
	for (const auto& Edge : EdgeUses)
	{
		if (Edge.Value > 2) { return Fail(Error, TEXT("seam repair refuses existing nonmanifold edges")); }
		if (Edge.Value != 1) { continue; }
		BoundaryEdges.Add(Edge.Key); BoundaryVertices.AddUnique(Edge.Key.X); BoundaryVertices.AddUnique(Edge.Key.Y);
	}
	if (BoundaryEdges.IsEmpty()) { return Fail(Error, TEXT("no boundary seams to repair")); }
	FFace CutSeam;
	TArray<FFace> CutSeams;
	bool bStitchCutSeam = BoundaryEdges.Num() == 3 && BoundaryVertices.Num() == 3;
	if (bStitchCutSeam)
	{
		const FIntPoint First = BoundaryEdges[0];
		int32 Third = INDEX_NONE;
		for (int32 V : BoundaryVertices) { if (V != First.X && V != First.Y) { Third = V; } }
		FIntVector Order(First.X, First.Y, Third);
		for (const FIntVector& T : FacePositions)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				if (T[K] == First.X && T[(K + 1) % 3] == First.Y) { Order = FIntVector(First.Y, First.X, Third); }
			}
		}
		for (int32 K = 0; K < 3; ++K)
		{
			bool bMatchedInternalEdge = false;
			for (int32 F = 0; F < FacePositions.Num(); ++F)
			{
				for (int32 E = 0; E < 3; ++E)
				{
					if (FacePositions[F][E] != Order[(K + 1) % 3] || FacePositions[F][(E + 1) % 3] != Order[K]) { continue; }
					bMatchedInternalEdge = Faces[F].Material == 3;
					if (bMatchedInternalEdge) { CutSeam.Corners.Add(Faces[F].Corners[(E + 1) % 3]); }
				}
			}
			bStitchCutSeam &= bMatchedInternalEdge;
		}
		const FVector A = Original[Order.X], B = Original[Order.Y], C = Original[Order.Z];
		const double AB = FVector::Distance(A, B), BC = FVector::Distance(B, C), CA = FVector::Distance(C, A);
		const double Longest = FMath::Max3(AB, BC, CA);
		const double TwiceArea = FVector::CrossProduct(B - A, C - A).Size();
		const double Altitude = Longest > 0 ? TwiceArea / Longest : 0;
		// With fixed endpoints, the maximum distance of added surface to the existing boundary
		// is the triangle's inradius. The residual pass moves no original vertex at all.
		const double BoundaryDistance = bStitchOnly ? TwiceArea / (AB + BC + CA) : Altitude;
		const TArray<FVector> Points{A, B, C};
		bStitchCutSeam &= CutSeam.Corners.Num() == 3 && BoundaryDistance > 0 && BoundaryDistance <= ContactToleranceCm &&
			FindFaceNormal(MakeArrayView(Points), CutSeam.Normal);
	}
	if (bStitchCutSeam) { CutSeams.Add(MoveTemp(CutSeam)); }
	if (BoundaryEdges.Num() == 4 && BoundaryVertices.Num() == 4)
	{
		TMap<int32, TArray<int32>> Adjacent;
		for (const FIntPoint& Edge : BoundaryEdges) { Adjacent.FindOrAdd(Edge.X).Add(Edge.Y); Adjacent.FindOrAdd(Edge.Y).Add(Edge.X); }
		bool bSimple = true;
		for (const auto& Pair : Adjacent) { bSimple &= Pair.Value.Num() == 2; }
		TArray<int32> Loop{BoundaryEdges[0].X, BoundaryEdges[0].Y};
		for (const FIntVector& T : FacePositions)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				if (T[K] == BoundaryEdges[0].X && T[(K + 1) % 3] == BoundaryEdges[0].Y)
				{
					Loop[0] = BoundaryEdges[0].Y; Loop[1] = BoundaryEdges[0].X;
				}
			}
		}
		while (bSimple && Loop.Num() < 4)
		{
			const auto& Neighbours = Adjacent.FindChecked(Loop.Last());
			const int32 Next = Neighbours[0] == Loop[Loop.Num() - 2] ? Neighbours[1] : Neighbours[0];
			if (Loop.Contains(Next)) { bSimple = false; break; }
			Loop.Add(Next);
		}
		TArray<FCorner> Corners;
		if (bSimple && Adjacent.FindChecked(Loop.Last()).Contains(Loop[0]))
		{
			for (int32 K = 0; K < 4; ++K)
			{
				bool bInternal = false;
				for (int32 F = 0; F < FacePositions.Num(); ++F)
				{
					for (int32 E = 0; E < 3; ++E)
					{
						if (FacePositions[F][E] != Loop[(K + 1) % 4] || FacePositions[F][(E + 1) % 3] != Loop[K]) { continue; }
						bInternal = Faces[F].Material == 3;
						if (bInternal) { Corners.Add(Faces[F].Corners[(E + 1) % 3]); }
					}
				}
				bSimple &= bInternal;
			}
		}
		else { bSimple = false; }
		if (bSimple && Corners.Num() == 4)
		{
			TArray<FVector> Points;
			for (const FCorner& Corner : Corners) { Points.Add(Corner.Position); }
			FVector PatchNormal;
			bSimple = FindFaceNormal(MakeArrayView(Points), PatchNormal);
			for (const auto& Triangles : TArray<TArray<FIntVector>>{{{0, 1, 2}, {0, 2, 3}}, {{0, 1, 3}, {1, 2, 3}}})
			{
				if (!bSimple || bStitchCutSeam) { break; }
				TArray<FFace> Patches;
				bool bBounded = true;
				for (const FIntVector& T : Triangles)
				{
					FFace Patch;
					const TArray<FVector> TriPoints{Points[T.X], Points[T.Y], Points[T.Z]};
					bBounded &= FindFaceNormal(MakeArrayView(TriPoints), Patch.Normal) && FVector::DotProduct(Patch.Normal, PatchNormal) > 0;
					double BestBound = TNumericLimits<double>::Max();
					for (int32 E = 0; E < 4; ++E)
					{
						const FVector A = Points[E], Delta = Points[(E + 1) % 4] - A;
						if (Delta.SizeSquared() == 0) { continue; }
						double Bound = 0;
						for (const FVector& P : TriPoints)
						{
							const double Along = FMath::Clamp(FVector::DotProduct(P - A, Delta) / Delta.SizeSquared(), 0.0, 1.0);
							Bound = FMath::Max(Bound, FVector::Distance(P, A + Along * Delta));
						}
						BestBound = FMath::Min(BestBound, Bound);
					}
					// Distance to one existing boundary segment is convex, so the vertex bound
					// bounds every point of this added triangle without moving any old vertex.
					bBounded &= BestBound <= ContactToleranceCm;
					Patch.Corners = {Corners[T.X], Corners[T.Y], Corners[T.Z]};
					Patches.Add(MoveTemp(Patch));
				}
				if (bBounded) { CutSeams = MoveTemp(Patches); bStitchCutSeam = true; }
			}
		}
	}
	if (SourceBuilding && BoundaryEdges.Num() == BoundaryVertices.Num() && BoundaryEdges.Num() >= 3 && BoundaryEdges.Num() <= 4)
	{
		TMap<int32, TArray<int32>> Adjacent;
		for (const FIntPoint& Edge : BoundaryEdges) { Adjacent.FindOrAdd(Edge.X).Add(Edge.Y); Adjacent.FindOrAdd(Edge.Y).Add(Edge.X); }
		bool bSimple = true, bExteriorBoundary = false;
		for (const auto& Pair : Adjacent) { bSimple &= Pair.Value.Num() == 2; }
		TArray<int32> Order{BoundaryEdges[0].X, BoundaryEdges[0].Y};
		for (int32 F = 0; F < FacePositions.Num(); ++F)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A = FacePositions[F][K], B = FacePositions[F][(K + 1) % 3];
				if (EdgeUses.FindRef(FIntPoint(FMath::Min(A, B), FMath::Max(A, B))) == 1) { bExteriorBoundary |= Faces[F].Material < 3; }
				if (A == BoundaryEdges[0].X && B == BoundaryEdges[0].Y) { Order[0] = B; Order[1] = A; }
			}
		}
		while (bSimple && Order.Num() < BoundaryVertices.Num())
		{
			const auto& Neighbours = Adjacent.FindChecked(Order.Last());
			const int32 Next = Neighbours[0] == Order[Order.Num() - 2] ? Neighbours[1] : Neighbours[0];
			if (Order.Contains(Next)) { bSimple = false; break; }
			Order.Add(Next);
		}
		if (bSimple && Adjacent.FindChecked(Order.Last()).Contains(Order[0]))
		{
			TArray<FVector> Loop;
			for (int32 V : Order) { Loop.Add(Original[V]); }
			TArray<FFace> Exterior;
			FString AttributionError;
			const EExteriorSeamMatch Match = AttributeExteriorSeam(*SourceBuilding, ToSource, Loop, Exterior, AttributionError);
			if (Match == EExteriorSeamMatch::Ambiguous)
			{
				return Fail(Error, AttributionError);
			}
			if (Match == EExteriorSeamMatch::Absent && bExteriorBoundary)
			{
				// A source/cut interface may need the existing bounded seam match rather than an
				// exterior patch. Never turn failed source attribution into a neutral cap.
				CutSeams.Reset(); bStitchCutSeam = false;
				if (bStitchOnly) { return Fail(Error, AttributionError); }
			}
			if (Match == EExteriorSeamMatch::Unique) { CutSeams = MoveTemp(Exterior); bStitchCutSeam = true; }
		}
	}
	if (bStitchOnly && !bStitchCutSeam)
	{
		return Fail(Error, TEXT("residual closure requires one simple internal three/four-edge loop with added surface within 0.01cm of its original boundary"));
	}
	BoundaryVertices.Sort([&](int32 A, int32 B) { return ExteriorUses[A] != ExteriorUses[B] ? ExteriorUses[A] > ExteriorUses[B] : A < B; });
	TArray<FVector> Target = Original;
	TSet<int32> Assigned;
	for (int32 A : bStitchCutSeam ? TArray<int32>() : BoundaryVertices)
	{
		if (Assigned.Contains(A)) { continue; }
		Assigned.Add(A);
		for (int32 B : BoundaryVertices)
		{
			if (Assigned.Contains(B) || FVector::Distance(Original[A], Original[B]) > ContactToleranceCm) { continue; }
			Target[B] = Target[A]; Assigned.Add(B);
		}
	}
	for (int32 V : bStitchCutSeam ? TArray<int32>() : BoundaryVertices)
	{
		const FVector Current = Target[V];
		FVector Best = Current;
		double Distance = ContactToleranceCm * ContactToleranceCm;
		for (const FIntPoint& Edge : BoundaryEdges)
		{
			const FVector A = Target[Edge.X], B = Target[Edge.Y], Delta = B - A;
			if (Current == A || Current == B || Delta.SizeSquared() == 0) { continue; }
			const double T = FVector::DotProduct(Current - A, Delta) / Delta.SizeSquared();
			if (T <= 1.e-7 || T >= 1 - 1.e-7) { continue; }
			const FVector Projected = A + T * Delta;
			const double CandidateDistance = FVector::DistSquared(Current, Projected);
			if (CandidateDistance >= Distance) { continue; }
			bool bWithinOriginalTolerance = true;
			for (int32 I = 0; I < Target.Num(); ++I)
			{
				if (Target[I] == Current) { bWithinOriginalTolerance &= FVector::Distance(Original[I], Projected) <= ContactToleranceCm; }
			}
			if (bWithinOriginalTolerance) { Best = Projected; Distance = CandidateDistance; }
		}
		if (Best != Current) { for (FVector& P : Target) { if (P == Current) { P = Best; } } }
	}
	FVector Reference = FVector::ZeroVector;
	for (const FVector& P : Original) { Reference += P; }
	Reference /= Original.Num();
	double BeforeVolume = 0;
	TArray<FFace> RepairedFaces;
	for (int32 F = 0; F < Faces.Num(); ++F)
	{
		const FIntVector IDs = FacePositions[F];
		BeforeVolume += FVector::DotProduct(Original[IDs.X] - Reference,
			FVector::CrossProduct(Original[IDs.Z] - Reference, Original[IDs.Y] - Reference)) / 6;
		FFace Face = Faces[F];
		TArray<FVector> Points;
		for (int32 K = 0; K < 3; ++K)
		{
			if (FVector::Distance(Target[IDs[K]], Original[IDs[K]]) > ContactToleranceCm)
			{
				return Fail(Error, TEXT("seam repair exceeded the original 0.01cm displacement bound"));
			}
			Face.Corners[K].Position = Target[IDs[K]]; Points.Add(Target[IDs[K]]);
		}
		FVector Normal;
		if (!FindFaceNormal(MakeArrayView(Points), Normal)) { continue; }
		if (FVector::DotProduct(Normal, Face.Normal) <= 0) { return Fail(Error, TEXT("seam repair would invert a positive-area triangle")); }
		RepairedFaces.Add(MoveTemp(Face));
	}
	if (bStitchCutSeam) { RepairedFaces.Append(CutSeams); }
	FGeometryCollection Replacement;
	if (bStitchCutSeam)
	{
		const int32 OldVertexStart = Geometry.VertexStart[G], OldFaceStart = Geometry.FaceStart[G];
		const int32 OldVertexCount = Geometry.VertexCount[G], OldFaceCount = Geometry.FaceCount[G];
		Replacement.AddElements(1, FGeometryCollection::TransformGroup);
		Replacement.AddElements(1, FGeometryCollection::GeometryGroup);
		Replacement.SetNumUVLayers(2);
		Replacement.AddElements(OldVertexCount + CutSeams.Num() * 3, FGeometryCollection::VerticesGroup);
		Replacement.AddElements(OldFaceCount + CutSeams.Num(), FGeometryCollection::FacesGroup);
		Replacement.Transform[0] = FTransform3f::Identity; Replacement.Parent[0] = INDEX_NONE;
		Replacement.SimulationType[0] = FGeometryCollection::FST_Rigid;
		Replacement.TransformToGeometryIndex[0] = 0; Replacement.TransformIndex[0] = 0;
		Replacement.VertexStart[0] = 0; Replacement.VertexCount[0] = Replacement.Vertex.Num();
		Replacement.FaceStart[0] = 0; Replacement.FaceCount[0] = Replacement.Indices.Num();
		Replacement.BoundingBox[0] = Geometry.BoundingBox[G];
		Replacement.InnerRadius[0] = Geometry.InnerRadius[G]; Replacement.OuterRadius[0] = Geometry.OuterRadius[G];
		auto ReplacementUVs = GeometryCollection::UV::FindActiveUVLayers(Replacement);
		for (int32 V = 0; V < OldVertexCount; ++V)
		{
			const int32 OldV = OldVertexStart + V;
			Replacement.Vertex[V] = Geometry.Vertex[OldV]; Replacement.Normal[V] = Geometry.Normal[OldV];
			Replacement.TangentU[V] = Geometry.TangentU[OldV]; Replacement.TangentV[V] = Geometry.TangentV[OldV];
			Replacement.Color[V] = Geometry.Color[OldV];
			Replacement.BoneMap[V] = 0;
			ReplacementUVs[0][V] = UVs[0][OldV]; ReplacementUVs[1][V] = UVs[1][OldV];
		}
		for (int32 F = 0; F < OldFaceCount; ++F)
		{
			const int32 OldF = OldFaceStart + F;
			Replacement.Indices[F] = Geometry.Indices[OldF] - FIntVector(OldVertexStart);
			Replacement.Visible[F] = Geometry.Visible[OldF]; Replacement.Internal[F] = Geometry.Internal[OldF];
			Replacement.MaterialID[F] = Geometry.MaterialID[OldF];
		}
		for (int32 S = 0; S < CutSeams.Num(); ++S)
		{
			const FFace& Patch = CutSeams[S];
			const int32 VertexStart = OldVertexCount + S * 3, Face = OldFaceCount + S;
			const FVector Tangent = FVector::CrossProduct(FMath::Abs(Patch.Normal.Z) < .9 ? FVector::UpVector : FVector::ForwardVector,
				Patch.Normal).GetSafeNormal();
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 V = VertexStart + K;
				const FCorner& Corner = Patch.Corners[K];
				Replacement.Vertex[V] = FVector3f(Corner.Position); Replacement.Normal[V] = FVector3f(Patch.Normal);
				Replacement.TangentU[V] = FVector3f(Tangent); Replacement.TangentV[V] = FVector3f(FVector::CrossProduct(Patch.Normal, Tangent));
				Replacement.Color[V] = Corner.Color; Replacement.BoneMap[V] = 0;
				ReplacementUVs[0][V] = FVector2f(Corner.UV0); ReplacementUVs[1][V] = FVector2f(Corner.UV1);
			}
			Replacement.Indices[Face] = FIntVector(VertexStart, VertexStart + 1, VertexStart + 2);
			Replacement.Visible[Face] = true; Replacement.Internal[Face] = Patch.Material == 3; Replacement.MaterialID[Face] = Patch.Material;
		}
		Replacement.ReindexMaterials();
	}
	else
	{
		FMeshDescription Mesh;
		if (!WriteFaces(RepairedFaces, Mesh, Error, true)) { return false; }
		FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, TEXT("DetailRepairedLeaf"), 0, FTransform::Identity,
			&Replacement, nullptr, false, false, false);
	}
	const double VolumeReference = ClosedMemberVolumeCm3 > 0 ? ClosedMemberVolumeCm3 : FMath::Abs(BeforeVolume);
	double AfterVolume = 0;
	int32 RemovedZeroFaces = 0;
	if (!DublinStructural::RemoveExactZeroAreaFaces(Replacement, 0, RemovedZeroFaces, Error)) { return false; }
	if (!ValidateLeaf(Replacement, 0, AfterVolume, Error))
	{
		const FString FirstError = Error;
		if (!bAllowResidualStitch || bStitchOnly ||
			!RepairLeafSeamsInternal(Replacement, 0, Error, VolumeReference, true, false, SourceBuilding, ToSource) ||
			!ValidateLeaf(Replacement, 0, AfterVolume, Error))
		{
			Error = FirstError + TEXT("; boundedResidualStitch=") + Error;
			return false;
		}
	}
	if (bStitchCutSeam)
	{
		const auto ReplacementUVs = GeometryCollection::UV::FindActiveUVLayers(Replacement);
		for (int32 V = Geometry.VertexStart[G]; V < Geometry.VertexStart[G] + Geometry.VertexCount[G]; ++V)
		{
			bool bRetained = false;
			for (int32 R = 0; R < Replacement.Vertex.Num() && !bRetained; ++R)
			{
				bRetained = Replacement.Vertex[R] == Geometry.Vertex[V] &&
					ReplacementUVs[0][R].Equals(UVs[0][V], 2.e-4f) && ReplacementUVs[1][R].Equals(UVs[1][V], 2.e-4f) &&
					Replacement.Color[R].Equals(Geometry.Color[V], 2.e-4f) &&
					Replacement.Normal[R] == Geometry.Normal[V] && Replacement.TangentU[R] == Geometry.TangentU[V] &&
					Replacement.TangentV[R] == Geometry.TangentV[V];
			}
			if (!bRetained) { return Fail(Error, TEXT("residual stitch displaced an existing vertex or changed its UV/color provenance")); }
		}
	}
	// A cut leaf with an open seam is not a conserved-volume reference. The caller still checks
	// every repaired child's aggregate against the original closed member and the whole assembly.
	if (FMath::Abs(AfterVolume - BeforeVolume) > FMath::Max(1.0, VolumeReference * 1.e-4))
	{
		return Fail(Error, FString::Printf(TEXT("seam repair exceeded conserved member volume budget: %.17g -> %.17g reference=%.17g"),
			BeforeVolume, AfterVolume, VolumeReference));
	}
	const int32 AddedBone = Geometry.Transform.Num();
	if (!FGeometryCollectionEngineConversion::AppendGeometryCollection(&Replacement, 0, FTransform::Identity, &Geometry, false))
	{
		return Fail(Error, TEXT("could not append the validated seam replacement"));
	}
	Geometry.RemoveElements(FGeometryCollection::GeometryGroup, {G});
	const int32 AddedGeometry = Geometry.TransformToGeometryIndex[AddedBone];
	Geometry.TransformIndex[AddedGeometry] = Bone;
	Geometry.TransformToGeometryIndex[Bone] = AddedGeometry;
	Geometry.TransformToGeometryIndex[AddedBone] = INDEX_NONE;
	for (int32 V = Geometry.VertexStart[AddedGeometry]; V < Geometry.VertexStart[AddedGeometry] + Geometry.VertexCount[AddedGeometry]; ++V)
	{
		Geometry.BoneMap[V] = Bone;
	}
	Geometry.RemoveElements(FGeometryCollection::TransformGroup, {AddedBone});
	for (int32 F = Geometry.FaceStart[AddedGeometry]; F < Geometry.FaceStart[AddedGeometry] + Geometry.FaceCount[AddedGeometry]; ++F)
	{
		Geometry.Internal[F] = Geometry.MaterialID[F] == 3;
	}
	if (!DublinStructural::NormalizeGeometryOrder(Geometry, Error)) { return false; }
	Geometry.ReindexMaterials();
	Error.Reset();
	return true;
}

bool DublinStructural::RepairLeafSeams(FGeometryCollection& Geometry, int32 Bone, FString& Error,
	double ClosedMemberVolumeCm3, const FDublinCityBuilding* SourceBuilding)
{
	const FTransform ToSource = GeometryCollectionAlgo::GlobalMatrix(Geometry.Transform, Geometry.Parent, Bone);
	return RepairLeafSeamsInternal(Geometry, Bone, Error, ClosedMemberVolumeCm3, false, true, SourceBuilding, ToSource);
}
#endif

bool DublinStructural::ValidateExteriorGeometry(const FDublinCityBuilding& Building, const FGeometryCollection& Geometry,
	const FDublinFractureRecord& Record, FString& Error)
{
	return ValidateExterior(Building, Geometry, Record, Error);
}

bool DublinStructural::BuildCollection(const FDublinCityBuilding& Building, UGeometryCollection& Asset,
	FDublinFractureRecord& Record, FString& Error, int32 Attempt)
{
	Record = FDublinFractureRecord();
	Record.Recipe = EDublinFractureRecipe::StructuralPilot;
	Record.SourceId = Building.Id;
	Record.GeometryDigest = DublinDestruction::BuildingGeometryDigest(Building);
	Record.SourceDigest = DublinDestruction::BuildingDigest(Building, Record.Recipe);
	Record.BakeAttempts = Attempt + 1;
	if (!IsPilotId(Building.Id) || Attempt < 0 || Attempt > 2) { return Fail(Error, TEXT("unapproved source ID or attempt")); }
	if (Asset.Materials.Num() != 4 || !Asset.Materials[3] || Asset.Materials[3] == Asset.Materials[1])
	{
		return Fail(Error, TEXT("requires four explicit slots and an interior material distinct from the facade"));
	}
	FAssembly Assembly;
	if (!BuildAssembly(Building, Assembly, Error)) { return false; }
	Record.SourceVolumeCm3 = Assembly.SourceVolumeCm3;
	Record.StructuralVolumeCm3 = Assembly.StructuralVolumeCm3;
	auto Geometry = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
	Asset.SetGeometryCollection(Geometry);
	TArray<int32> MemberBones;
	for (const FMember& Member : Assembly.Members)
	{
		FMeshDescription Mesh;
		if (!MakeMemberMesh(Building, Assembly, Member, Mesh, Error)) { return false; }
		const int32 Bone = Geometry->Transform.Num();
		FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform::Identity,
			&Geometry.Get(), nullptr, false, false, false);
		if (Geometry->Transform.Num() != Bone + 1) { return Fail(Error, TEXT("member conversion did not produce one bone")); }
		double Volume = 0;
		if (!ValidateLeaf(*Geometry, Bone, Volume, Error)) { return false; }
		const double Expected = Area(Member.Polygon) * (Member.MaxZ - Member.MinZ);
		if (FMath::Abs(Volume - Expected) > FMath::Max(1.0, Expected * 1.e-4)) { return Fail(Error, TEXT("member conversion changed structural volume")); }
		MemberBones.Add(Bone);
	}
	Geometry->ReindexMaterials();
	for (int32 I = 0; I < MemberBones.Num(); ++I)
	{
		const FMember& Member = Assembly.Members[I];
		const uint32 Seed = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%d:%d"), *Record.SourceDigest, I, Attempt));
		const TArray<FVector> Sites = SitesForMember(Member, Seed);
		if (Sites.Num() != Member.Seeds) { return Fail(Error, TEXT("bounded irregular site sampling failed")); }
		FBox Bounds(ForceInit);
		for (const FVector2D& P : Member.Polygon)
		{
			Bounds += FVector(P.X, P.Y, Member.MinZ);
			Bounds += FVector(P.X, P.Y, Member.MaxZ);
		}
		FVoronoiDiagram Voronoi(MakeArrayView(Sites), Bounds, 50.0, 0.0);
		FPlanarCells Cells(MakeArrayView(Sites), Voronoi);
		if (Cells.NumCells != Sites.Num() || !Cells.HasValidPlaneBoundaryOrientations())
		{
			return Fail(Error, TEXT("native Voronoi cells are incomplete or invalid"));
		}
		Cells.InternalSurfaceMaterials.GlobalMaterialID = 3;
		Cells.InternalSurfaceMaterials.GlobalUVScale = .01f;
		TArray<int32> Selection{MemberBones[I]};
		const int32 NewGeometry = CutMultipleWithPlanarCells(Cells, *Geometry, MakeArrayView(Selection),
			0, 50, static_cast<int32>(Seed), TOptional<FTransform>(), true, false, nullptr, FVector::ZeroVector,
			FIslandSplitSettings(true, .001, 0));
		if (NewGeometry < 0) { return Fail(Error, TEXT("native member Voronoi cutting failed; no fallback permitted")); }
		double Retained = 0;
		int32 Pieces = 0;
		for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
		{
			if (Geometry->Parent[Bone] == MemberBones[I] && Geometry->IsRigid(Bone) && Geometry->Children[Bone].IsEmpty())
			{
				double Volume = 0;
				if (!ValidateLeaf(*Geometry, Bone, Volume, Error)) { return false; }
				Retained += Volume;
				++Pieces;
			}
		}
		const double Expected = Area(Member.Polygon) * (Member.MaxZ - Member.MinZ);
		if (Pieces != Sites.Num() || FMath::Abs(Retained - Expected) > FMath::Max(1.0, Expected * 1.e-4))
		{
			return Fail(Error, FString::Printf(TEXT("member %d lost cells/volume: %d/%d, %.9g/%.9g"), I, Pieces, Sites.Num(), Retained, Expected));
		}
		Record.FractureSiteCount += Sites.Num();
		Record.RetainedVolumeCm3 += Retained;
	}
	TArray<int32> SourceGeometry;
	for (int32 Bone : MemberBones)
	{
		const int32 GeometryIndex = Geometry->TransformToGeometryIndex[Bone];
		if (GeometryIndex != INDEX_NONE) { SourceGeometry.Add(GeometryIndex); }
	}
	SourceGeometry.Sort();
	Geometry->RemoveElements(FGeometryCollection::GeometryGroup, SourceGeometry);
	TArray<int32> Leaves;
	TArray<FTransform> Before;
	for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
	{
		if (Geometry->IsRigid(Bone) && Geometry->Children[Bone].IsEmpty())
		{
			Leaves.Add(Bone);
			Before.Add(GeometryCollectionAlgo::GlobalMatrix(Geometry->Transform, Geometry->Parent, Bone));
		}
	}
	if (Leaves.Num() != SeedTarget || Leaves.Num() > UDublinCityFractureLibrary::MaxPiecesPerBuilding) { return Fail(Error, TEXT("structural piece budget violated")); }
	FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(&Geometry.Get());
	TArray<int32> InitialRoots;
	FGeometryCollectionClusteringUtility::GetRootBones(&Geometry.Get(), InitialRoots);
	if (InitialRoots.Num() != 1) { return Fail(Error, TEXT("cannot create structural root")); }
	FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Geometry.Get(), InitialRoots[0]);
	FGeometryCollectionClusteringUtility::ClusterBonesUnderExistingRoot(&Geometry.Get(), Leaves);
	TArray<int32> Roots;
	FGeometryCollectionClusteringUtility::GetRootBones(&Geometry.Get(), Roots);
	if (Roots.Num() != 1) { return Fail(Error, TEXT("structural flattening lost unique root")); }
	Record.RootTransform = Roots[0];
	int32 LeafNumber = 0;
	if (!Geometry->HasAttribute(TEXT("InitialDynamicState"), FGeometryCollection::TransformGroup))
	{
		Geometry->AddAttribute<int32>(TEXT("InitialDynamicState"), FGeometryCollection::TransformGroup);
	}
	Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Geometry);
	Anchoring.AddAnchoredAttribute();
	for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
	{
		Anchoring.SetInitialDynamicState(Bone, Chaos::EObjectStateType::Dynamic);
		if (!Geometry->IsRigid(Bone) || !Geometry->Children[Bone].IsEmpty()) { continue; }
		const FTransform Global = GeometryCollectionAlgo::GlobalMatrix(Geometry->Transform, Geometry->Parent, Bone);
		if (!Before.IsValidIndex(LeafNumber) || !Before[LeafNumber++].Equals(Global, 1.e-3)) { return Fail(Error, TEXT("flattening changed a leaf transform")); }
		Record.LeafTransforms.Add(Bone);
		const int32 G = Geometry->TransformToGeometryIndex[Bone];
		const auto& Box = Geometry->BoundingBox[G];
		const FBox WorldBox = FBox(FVector(Box.Min), FVector(Box.Max)).TransformBy(Global);
		if (WorldBox.Min.Z <= Assembly.MinZ + .01)
		{
			Record.Anchors.Add(Bone);
			Anchoring.SetAnchored(Bone, true);
			Anchoring.SetInitialDynamicState(Bone, Chaos::EObjectStateType::Kinematic);
		}
	}
	Record.PieceCount = Record.LeafTransforms.Num();
	if (!ValidateExterior(Building, *Geometry, Record, Error)) { return false; }
	Asset.EnableClustering = true;
	Asset.DamageModel = EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
	Asset.DamageThreshold = {100, 75, 50};
	Asset.bUseSizeSpecificDamageThreshold = false;
	Asset.bRemoveOnMaxSleep = false;
	Asset.bAutomaticCrumblePartialClusters = false;
	Asset.bImportCollisionFromSource = false;
	Asset.bOptimizeConvexes = false;
	Asset.DamagePropagationData.bEnabled = false;
	Asset.DamagePropagationData.BreakDamagePropagationFactor = 0;
	Asset.DamagePropagationData.ShockDamagePropagationFactor = 0;
	if (Asset.SizeSpecificData.IsEmpty()) { Asset.SizeSpecificData.Add(UGeometryCollection::GeometryCollectionSizeSpecificDataDefaults()); }
	for (FGeometryCollectionSizeSpecificData& Size : Asset.SizeSpecificData)
	{
		Size.CollisionShapes.SetNum(1);
		Size.CollisionShapes[0].CollisionType = ECollisionTypeEnum::Chaos_Volumetric;
		Size.CollisionShapes[0].ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Convex;
		Size.CollisionShapes[0].CollisionMarginFraction = 0;
		Size.CollisionShapes[0].CollisionObjectReductionPercentage = 0;
	}
	auto Convex = Geometry->GetConvexProperties();
	Convex.SimplificationThreshold = 0;
	Convex.OverlapRemovalShrinkPercent = 0;
	Geometry->SetConvexProperties(Convex);
	Asset.InvalidateCollection();
	Asset.UpdateGeometryDependentProperties();
	FGeometryCollectionConvexUtility::FLeafConvexHullSettings HullSettings(0, EGenerateConvexMethod::ComputedFromGeometry);
	FGeometryCollectionConvexUtility::GenerateLeafConvexHulls(*Geometry, true, MakeArrayView(Record.LeafTransforms), HullSettings);
	FGeometryCollectionConvexUtility::CopyChildConvexes(&Geometry.Get(), MakeArrayView(Roots), &Geometry.Get(), MakeArrayView(Roots), true);
	if (!DublinFractureBake::BuildConnectionGraph(Asset, Error)) { return false; }
	Asset.bEnableNaniteFallback = true;
	Asset.SetEnableNanite(true);
	Asset.InvalidateCollection();
	Asset.CreateSimulationData();
	Asset.SetConvertVertexColorsToSRGB(true);
	Asset.RebuildRenderData();
	Record.bNaniteReady = Asset.HasNaniteData();
	if (!Record.bNaniteReady || !Asset.HasVisibleGeometry() || Asset.IsSimulationDataDirty())
	{
		return Fail(Error, TEXT("structural Nanite/render/simulation data is not ready; fallback forbidden"));
	}
	if (!DublinFractureBake::ValidateCollisionData(Asset, Record, Error) ||
		!ValidateBuildingCollision(Asset, Building, Record, Error)) { return false; }
	Record.BakeDiagnostics.Add(FString::Printf(TEXT("Artistic structural inference: walls30cm slabs20cm equal-clear rooms=%d seeds=%d; envelope=%.9g structural=%.9g retained=%.9g cm3"),
		Assembly.RoomsZ.Num(), Record.FractureSiteCount, Record.SourceVolumeCm3, Record.StructuralVolumeCm3, Record.RetainedVolumeCm3));
	Error.Reset();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailRecordedFacadeTest,
	"DublinFlight.Destruction.CityDetail.NativeRecordedFacadeSliver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailRecordedFacadeTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FString Error;
	if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { AddError(Error); return false; }
	const auto* Building = Data.Buildings.FindByPredicate([](const auto& B) { return B.Id == TEXT("osm/relation/289528"); });
	if (!TestNotNull(TEXT("Recorded façade source"), Building)) { return false; }
	const int32 SourceFace = 145;
	const FIntVector SourceIds(Building->Mesh.Triangles[SourceFace * 3], Building->Mesh.Triangles[SourceFace * 3 + 1], Building->Mesh.Triangles[SourceFace * 3 + 2]);
	const FVector SA = Building->Mesh.VerticesCm[SourceIds.X], SB = Building->Mesh.VerticesCm[SourceIds.Y], SC = Building->Mesh.VerticesCm[SourceIds.Z];
	const FVector SourceNormal = DublinCity::ClockwiseNormal(SA, SB, SC);
	const TArray<FVector3f> Points{
		{1810.860107421875f, -4018.537109375f, 1574.0244140625f},
		{1810.6888427734375f, -4018.48486328125f, 1574.04931640625f},
		{1813.4283447265625f, -4019.322265625f, 1573.6510009765625f},
		FVector3f(SC)};
	FGeometryCollection Geometry;
	Geometry.AddElements(1, FGeometryCollection::TransformGroup);
	Geometry.AddElements(1, FGeometryCollection::GeometryGroup);
	Geometry.SetNumUVLayers(2);
	Geometry.AddElements(4, FGeometryCollection::VerticesGroup);
	Geometry.AddElements(2, FGeometryCollection::FacesGroup);
	Geometry.Transform[0] = FTransform3f::Identity; Geometry.Parent[0] = INDEX_NONE;
	Geometry.TransformToGeometryIndex[0] = 0; Geometry.TransformIndex[0] = 0;
	Geometry.VertexStart[0] = 0; Geometry.VertexCount[0] = 4; Geometry.FaceStart[0] = 0; Geometry.FaceCount[0] = 2;
	auto UVs = GeometryCollection::UV::FindActiveUVLayers(Geometry);
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		Geometry.Vertex[I] = Points[I]; Geometry.Normal[I] = FVector3f(SourceNormal); Geometry.BoneMap[I] = 0;
		UE::Geometry::TDistPoint3Triangle3<double> Distance(FVector(Points[I]), UE::Geometry::FTriangle3d(SA, SB, SC));
		Distance.GetSquared();
		const FVector W = Distance.TriangleBaryCoords;
		const FVector2D UV = Building->Mesh.UV[SourceIds.X] * W.X + Building->Mesh.UV[SourceIds.Y] * W.Y + Building->Mesh.UV[SourceIds.Z] * W.Z;
		UVs[0][I] = FVector2f(UV); UVs[1][I] = FVector2f(UV);
		Geometry.Color[I] = DublinDestruction::SourceVertexLinearColor(Building->Mesh, SourceIds.X) * static_cast<float>(W.X) +
			DublinDestruction::SourceVertexLinearColor(Building->Mesh, SourceIds.Y) * static_cast<float>(W.Y) +
			DublinDestruction::SourceVertexLinearColor(Building->Mesh, SourceIds.Z) * static_cast<float>(W.Z);
	}
	Geometry.Indices[0] = FIntVector(0, 1, 2); Geometry.Indices[1] = FIntVector(2, 1, 3);
	for (int32 F = 0; F < 2; ++F) { Geometry.Visible[F] = true; Geometry.Internal[F] = false; Geometry.MaterialID[F] = 1; }
	const TArray<FVector> Recorded{FVector(Points[0]), FVector(Points[1]), FVector(Points[2])};
	FVector Normal;
	TestTrue(TEXT("Recorded façade sliver has genuine positive area"), DublinStructural::FindFaceNormal(MakeArrayView(Recorded), Normal));
	TestTrue(TEXT("Recorded float sliver reproduces failed source winding-normal agreement"), FVector::DotProduct(Normal, SourceNormal) < .999);
	int32 Flipped = 0;
	if (!DublinStructural::StabilizeExteriorTriangulation(*Building, Geometry, 0, Flipped, Error)) { AddError(Error); return false; }
	TestEqual(TEXT("One source-provenance edge flip replaces the ill-conditioned diagonal"), Flipped, 1);
	TestEqual(TEXT("Retriangulation drops no positive-area faces"), Geometry.Indices.Num(), 2);
	TestEqual(TEXT("Retriangulation moves or removes no vertex"), Geometry.Vertex.Num(), 4);
	for (int32 I = 0; I < 4; ++I) { TestTrue(TEXT("Recorded vertex positions remain exact"), Geometry.Vertex[I] == Points[I]); }
	for (const FIntVector T : Geometry.Indices.GetConstArray())
	{
		const TArray<FVector> Triangle{FVector(Geometry.Vertex[T.X]), FVector(Geometry.Vertex[T.Y]), FVector(Geometry.Vertex[T.Z])};
		TestTrue(TEXT("Retriangulated source faces have stable clockwise normals"), DublinStructural::FindFaceNormal(MakeArrayView(Triangle), Normal));
		TestTrue(TEXT("Original winding-normal threshold is retained"), FVector::DotProduct(Normal, SourceNormal) >= .999);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailExteriorSeamTest,
	"DublinFlight.Destruction.CityDetail.NativeSourceAttributedExteriorSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailExteriorSeamTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FString Error;
	if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { AddError(Error); return false; }
	const auto* Building = Data.Buildings.FindByPredicate([](const auto& B) { return B.Id == TEXT("bridge/osm/way/282571104/0"); });
	if (!TestNotNull(TEXT("Recorded bridge source"), Building)) { return false; }
	const auto* Courtyard = Data.Buildings.FindByPredicate([](const auto& B) { return B.Id == TEXT("osm/way/1488401199"); });
	if (!TestNotNull(TEXT("Recorded skinny source/cut interface"), Courtyard)) { return false; }
	const TArray<FVector> RoundedLoop{
		{648.637451171875, -93.416542053222656, 1787.05712890625},
		{648.6455078125, -93.532691955566406, 1786.5628662109375},
		{648.64459228515625, -93.519142150878906, 1786.5745849609375}};
	TArray<FFace> RoundedAttribution;
	TestTrue(TEXT("Float-normal noise cannot reject an otherwise source-valid attributed seam"),
		AttributeExteriorSeam(*Courtyard, FTransform::Identity, RoundedLoop, RoundedAttribution, Error) == EExteriorSeamMatch::Unique);
	if (RoundedAttribution.IsEmpty()) { AddError(Error); return false; }
	const TArray<FVector2D> Boundary{{1100, 740}, {1120, 740}, {1120, 770}, {1100, 770}};
	const TArray<FVector2D> Gap{{1108.4212646484375, 752.31781005859375}, {1111.8538818359375, 755.50469970703125},
		{1108.431396484375, 752.34552001953125}, {1108.420654296875, 752.33563232421875}};
	TArray<TArray<FVector2D>> Ring;
	if (!DublinDetail::PartitionFaceRemainder(Boundary, {Gap}, Ring, Error)) { AddError(Error); return false; }
	TArray<TArray<FVector>> Faces;
	TArray<FVector> Top;
	for (const FVector2D& P : Boundary) { Top.Emplace(P.X, P.Y, 20); }
	Faces.Add(Top);
	for (const auto& Part : Ring)
	{
		TArray<FVector> Bottom;
		for (const FVector2D& P : Part) { Bottom.Emplace(P.X, P.Y, 0); }
		Faces.Add(MoveTemp(Bottom));
	}
	for (int32 I = 0; I < Boundary.Num(); ++I)
	{
		const FVector2D A = Boundary[I], B = Boundary[(I + 1) % Boundary.Num()];
		Faces.Add({FVector(A.X, A.Y, 0), FVector(B.X, B.Y, 0), FVector(B.X, B.Y, 20), FVector(A.X, A.Y, 20)});
	}
	FMeshDescription Mesh;
	if (!DublinStructural::MakeConvexMeshDescription(*Building, Faces, Mesh, Error)) { AddError(Error); return false; }
	FDublinCityBuilding Ambiguous = *Building, Missing;
	const int32 DuplicateStart = Ambiguous.Mesh.VerticesCm.Num();
	for (int32 K = 0; K < 3; ++K)
	{
		const int32 V = Building->Mesh.Triangles[21 * 3 + K];
		Ambiguous.Mesh.VerticesCm.Add(Building->Mesh.VerticesCm[V]);
		Ambiguous.Mesh.UV.Add(Building->Mesh.UV[V] + FVector2D(.5, 0));
		Ambiguous.Mesh.ColorsRGBA.Add(Building->Mesh.ColorsRGBA[V]);
		Ambiguous.Mesh.Triangles.Add(DuplicateStart + K);
	}
	Ambiguous.MaterialIds.Add(2);
	for (int32 Case = 0; Case < 3; ++Case)
	{
		FGeometryCollection Geometry;
		FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building->Id, 0, FTransform::Identity, &Geometry, nullptr, false, false, false);
		double Volume = 0;
		TestFalse(TEXT("Recorded exterior seam fixture is initially open"), ValidateLeaf(Geometry, 0, Volume, Error));
		TSet<FVector3f> Before;
		for (const FVector3f& P : Geometry.Vertex.GetConstArray()) { Before.Add(P); }
		const FDublinCityBuilding* Source = Case == 0 ? &Missing : Case == 1 ? &Ambiguous : Building;
		const bool bRepaired = DublinStructural::RepairLeafSeams(Geometry, 0, Error, 12000, Source);
		if (Case != 2)
		{
			TestFalse(TEXT("Missing or ambiguous source attribution cannot invent an exterior face"), bRepaired);
			TestTrue(TEXT("Exterior attribution rejection is explicit"), Error.Contains(TEXT("source")) || Error.Contains(TEXT("ambiguous")));
			continue;
		}
		if (!bRepaired || !ValidateLeaf(Geometry, 0, Volume, Error)) { AddError(Error); return false; }
		TSet<FVector3f> After;
		for (const FVector3f& P : Geometry.Vertex.GetConstArray()) { After.Add(P); }
		for (const FVector3f& P : Before) { TestTrue(TEXT("Exterior recovery moves no original vertex"), After.Contains(P)); }
		const auto UVs = GeometryCollection::UV::FindActiveUVLayers(Geometry);
		double BaseArea = 0;
		for (int32 F = 0; F < Geometry.Indices.Num(); ++F)
		{
			if (Geometry.MaterialID[F] != 2) { continue; }
			TestFalse(TEXT("Recovered source base is never marked as neutral interior"), Geometry.Internal[F]);
			const FIntVector T = Geometry.Indices[F];
			const TArray<FVector> Points{FVector(Geometry.Vertex[T.X]), FVector(Geometry.Vertex[T.Y]), FVector(Geometry.Vertex[T.Z])};
			TArray<FFace> Attributed;
			if (AttributeExteriorSeam(*Building, FTransform::Identity, Points, Attributed, Error) != EExteriorSeamMatch::Unique)
			{
				AddError(Error); return false;
			}
			BaseArea += FVector::CrossProduct(Points[1] - Points[0], Points[2] - Points[0]).Size() * .5;
			for (int32 K = 0; K < 3; ++K)
			{
				const auto& Expected = Attributed[0].Corners[K];
				TestTrue(TEXT("Recovered base keeps original UV0"), FVector2D(UVs[0][T[K]]).Equals(Expected.UV0, 2.e-4));
				TestTrue(TEXT("Recovered base keeps original UV1"), FVector2D(UVs[1][T[K]]).Equals(Expected.UV1, 2.e-4));
				TestTrue(TEXT("Recovered base keeps original colors"), Geometry.Color[T[K]].Equals(Expected.Color, 2.e-4f));
			}
		}
		TestTrue(TEXT("Source-attributed patch restores complete base coverage"), FMath::IsNearlyEqual(BaseArea, 600.0, .01));
		TestTrue(TEXT("Source-attributed patch preserves the closed reference volume"), FMath::IsNearlyEqual(Volume, 12000.0, 1.2));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinDetailResidualSeamTest,
	"DublinFlight.Destruction.CityDetail.NativeResidualInternalCutSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinDetailResidualSeamTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Boundary{{-700, -820}, {-600, -820}, {-600, -720}, {-700, -720}};
	const TArray<TArray<FVector2D>> Gaps{
		{{-647.3980712890625, -767.1048583984375}, {-647.38714599609375, -767.0931396484375},
			{-647.82568359375, -767.12774658203125}},
		{{-647.39501953125, -767.10162353515625}, {-647.38714599609375, -767.0931396484375},
			{-647.70184326171875, -767.11273193359375}, {-648.79718017578125, -767.18084716796875}}};
	for (int32 Case = 0; Case < 3; ++Case)
	{
	const auto& Gap = Gaps[FMath::Min(Case, Gaps.Num() - 1)];
	TArray<TArray<FVector2D>> Ring;
	FString Error;
	if (!DublinDetail::PartitionFaceRemainder(Boundary, {Gap}, Ring, Error)) { AddError(Error); return false; }
	TArray<TArray<FVector>> Faces;
	TArray<FVector> Bottom;
	for (const FVector2D& P : Boundary) { Bottom.Emplace(P.X, P.Y, 0); }
	Faces.Add(Bottom);
	for (const auto& Part : Ring)
	{
		TArray<FVector> Top;
		for (const FVector2D& P : Part) { Top.Emplace(P.X, P.Y, 20); }
		Faces.Add(MoveTemp(Top));
	}
	for (int32 I = 0; I < Boundary.Num(); ++I)
	{
		const FVector2D A = Boundary[I], B = Boundary[(I + 1) % Boundary.Num()];
		Faces.Add({FVector(A.X, A.Y, 0), FVector(B.X, B.Y, 0), FVector(B.X, B.Y, 20), FVector(A.X, A.Y, 20)});
	}
	FDublinCityBuilding Building;
	Building.Id = TEXT("fixture/residual-internal-cut-seam");
	FMeshDescription Mesh;
	if (!DublinStructural::MakeConvexMeshDescription(Building, Faces, Mesh, Error)) { AddError(Error); return false; }
	FGeometryCollection Geometry;
	FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, 0, FTransform::Identity, &Geometry, nullptr, false, false, false);
	double Volume = 0;
	TestFalse(TEXT("The observed internal cut seam is genuinely open"), ValidateLeaf(Geometry, 0, Volume, Error));
	if (Case == 0)
	{
		TestFalse(TEXT("Single-pass endpoint repair cannot resolve this residual without exceeding its movement bound"),
			RepairLeafSeamsInternal(Geometry, 0, Error, 200000, false, false));
	}
	TSet<FVector3f> Before;
	for (const FVector3f& P : Geometry.Vertex.GetConstArray()) { Before.Add(P); }
	if (!RepairLeafSeamsInternal(Geometry, 0, Error, 200000, Case != 1, false) ||
		!ValidateLeaf(Geometry, 0, Volume, Error)) { AddError(Error); return false; }
	TSet<FVector3f> After;
	for (const FVector3f& P : Geometry.Vertex.GetConstArray()) { After.Add(P); }
	for (const FVector3f& P : Before) { TestTrue(TEXT("Residual closure retains every prior vertex exactly"), After.Contains(P)); }
	TestTrue(TEXT("Residual closure restores the closed reference volume without widening conservation limits"),
		FMath::IsNearlyEqual(Volume, 200000.0, 20.0));
	}
	return true;
}
#endif
#endif
