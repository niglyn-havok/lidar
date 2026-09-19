#include "City/DublinCityDestruction.h"

#include "City/DublinCityFractureLibrary.h"
#include "City/DublinCityStructural.h"
#include "City/DublinCityDetail.h"
#include "Effects/DublinMaximumImpact.h"
#include "Misc/SecureHash.h"

float DublinDestruction::ImpactStrain(const FDublinImpact& Impact)
{
	return FMath::Clamp(Impact.Strength * 150.0f, 100.0f, 1000000.0f);
}

FVector DublinDestruction::ImpactVelocityChange(const FDublinImpact& Impact, const FVector& WorldMassCenter)
{
	if (!Impact.IsValid() || WorldMassCenter.ContainsNaN()) { return FVector::ZeroVector; }
	FVector Offset = WorldMassCenter - Impact.PositionCm;
	if (IsHeightAwareBlast(Impact)) { Offset.Z = 0; }
	const double Distance = Offset.Size();
	if (!FMath::IsFinite(Distance) || Distance >= Impact.RadiusCm) { return FVector::ZeroVector; }
	// Game response is a bounded velocity change, not an impulse divided by solid-volume mass.
	const double Speed = FMath::Clamp(static_cast<double>(Impact.Strength) * 1500.0,
		1500.0, MaxFractureVelocityChangeCmPerSecond);
	const double CoreRadius = FMath::Min(6000.0, double(Impact.RadiusCm) * 0.5);
	const double Falloff = IsHeightAwareBlast(Impact)
		? FMath::Clamp((Impact.RadiusCm - Distance) / (Impact.RadiusCm - CoreRadius), 0.0, 1.0)
		: 1.0 - Distance / Impact.RadiusCm;
	return (Offset + FVector(0, 0, 100)).GetSafeNormal() * Speed * Falloff;
}

bool DublinDestruction::IsHeightAwareBlast(const FDublinImpact& Impact)
{
	return Impact.Kind == EDublinImpactKind::Bomb && !Impact.bWater && Impact.YieldTonsTNT >= HeightAwareBombYield;
}

int32 DublinDestruction::RegistrationLimitForImpact(const FDublinImpact& Impact)
{
	return DublinImpactFX::WantsMaximumPresentation(Impact) &&
		Impact.YieldTonsTNT == DublinImpactFX::MaximumTierYield &&
		Impact.RadiusCm <= DublinImpactFX::MaximumApprovedRadiusCm
		? MaxRegistrationsPerFrame : MaxOrdinaryRegistrationsPerFrame;
}

double DublinDestruction::FootprintDistanceSquared(const FDublinCityBuilding& Building, const FVector& Point)
{
	const FVector2D P(Point - Building.PivotCm);
	double Best = TNumericLimits<double>::Max();
	for (int32 T = 0; T + 2 < Building.Mesh.Triangles.Num(); T += 3)
	{
		const FVector2D A(Building.Mesh.VerticesCm[Building.Mesh.Triangles[T]]);
		const FVector2D B(Building.Mesh.VerticesCm[Building.Mesh.Triangles[T + 1]]);
		const FVector2D C(Building.Mesh.VerticesCm[Building.Mesh.Triangles[T + 2]]);
		const double Area = FVector2D::CrossProduct(B - A, C - A);
		if (FMath::Abs(Area) < 1.e-6) { continue; }
		const double AB = FVector2D::CrossProduct(B - A, P - A);
		const double BC = FVector2D::CrossProduct(C - B, P - B);
		const double CA = FVector2D::CrossProduct(A - C, P - C);
		if ((AB >= 0 && BC >= 0 && CA >= 0) || (AB <= 0 && BC <= 0 && CA <= 0)) { return 0; }
		const FVector2D Vertices[] = {A, B, C};
		for (int32 E = 0; E < 3; ++E)
		{
			const FVector2D Edge = Vertices[(E + 1) % 3] - Vertices[E];
			const double Alpha = Edge.SizeSquared() > 0
				? FMath::Clamp(FVector2D::DotProduct(P - Vertices[E], Edge) / Edge.SizeSquared(), 0.0, 1.0) : 0;
			Best = FMath::Min(Best, (P - Vertices[E] - Edge * Alpha).SizeSquared());
		}
	}
	return Best;
}

int32 DublinDestruction::ReservedHullSlots(const FDublinFractureRecord& Record)
{
	// Legacy v3 records predate HullCount. Reserve a disclosed conservative allowance.
	return Record.HullCount > 0 ? Record.HullCount : Record.PieceCount * LegacyHullSlotsPerLeaf;
}

bool DublinDestruction::AddCatalogRecord(const FDublinFractureRecord& Record, FCatalogBudget& Budget, FString& Error)
{
	const int64 Hulls = Record.HullCount > 0 ? int64(Record.HullCount) : int64(Record.PieceCount) * LegacyHullSlotsPerLeaf;
	if (!DublinFractureBake::HasValidPieceBudget(Record) || Record.HullCount < 0 || Hulls < Record.PieceCount
		|| int64(Budget.Collections) + 1 > MaxActiveCollections
		|| int64(Budget.LeafSlots) + Record.PieceCount > MaxActivePieces
		|| int64(Budget.HullSlots) + Hulls > MaxCatalogHullSlots)
	{
		Error = FString::Printf(TEXT("Catalog cost exceeds finite limits or has invalid piece/hull metadata at %s: "
			"collections=%lld/%d leaves=%lld/%d hullSlots=%lld/%d"),
			*Record.SourceId, int64(Budget.Collections) + 1, MaxActiveCollections,
			int64(Budget.LeafSlots) + Record.PieceCount, MaxActivePieces,
			int64(Budget.HullSlots) + Hulls, MaxCatalogHullSlots);
		return false;
	}
	++Budget.Collections;
	Budget.LeafSlots += Record.PieceCount;
	Budget.HullSlots += static_cast<int32>(Hulls);
	Error.Reset();
	return true;
}

bool DublinDestruction::ValidateImpact(const FDublinImpact& Impact, FString& Error)
{
	Error.Reset();
	if (!Impact.IsValid() || Impact.RadiusCm > MaxRadiusCm || Impact.CraterDepthCm > MaxDepthCm ||
		Impact.Strength > 1000000 || Impact.YieldTonsTNT > 1000 ||
		FMath::Abs(Impact.PositionCm.X) > 44800 || FMath::Abs(Impact.PositionCm.Y) > 44800 ||
		FMath::Abs(Impact.PositionCm.Z) > 200000 ||
		(Impact.Kind != EDublinImpactKind::Cannon && Impact.Kind != EDublinImpactKind::Bomb))
	{
		Error = TEXT("Impact exceeds finite game-world, radius, depth, strength or 0..1000 yield limits");
		return false;
	}
	return true;
}

double DublinDestruction::CraterDelta(double Distance, double Radius, double Depth)
{
	if (!FMath::IsFinite(Distance) || !FMath::IsFinite(Radius) || !FMath::IsFinite(Depth) ||
		Radius <= 0 || Depth <= 0 || Distance < 0) { return 0; }
	const double T = Distance / Radius;
	if (T < 0.8) { return -Depth * FMath::Square(1 - FMath::Square(T / 0.8)); }
	if (T < 1) { return Depth * 0.12 * FMath::Square(FMath::Sin(PI * (T - 0.8) / 0.2)); }
	return 0;
}

bool DublinDestruction::SphereTouchesBox(const FVector& Center, double Radius, const FBox& Box)
{
	return Box.IsValid && Box.ComputeSquaredDistanceToPoint(Center) <= Radius * Radius;
}

bool DublinDestruction::SourceWaterZ(const FDublinCityMesh& Source, const FVector& Point, float& Z)
{
	if (Point.ContainsNaN()) { return false; }
	for (int32 Offset = 0; Offset < Source.Triangles.Num(); Offset += 3)
	{
		const FVector& A = Source.VerticesCm[Source.Triangles[Offset]];
		const FVector& B = Source.VerticesCm[Source.Triangles[Offset + 1]];
		const FVector& C = Source.VerticesCm[Source.Triangles[Offset + 2]];
		const double Den = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
		if (FMath::Abs(Den) < 1.e-9) { continue; }
		const double U = ((B.Y - C.Y) * (Point.X - C.X) + (C.X - B.X) * (Point.Y - C.Y)) / Den;
		const double V = ((C.Y - A.Y) * (Point.X - C.X) + (A.X - C.X) * (Point.Y - C.Y)) / Den;
		if (U >= -1.e-7 && V >= -1.e-7 && U + V <= 1.0000001)
		{
			Z = static_cast<float>(U * A.Z + V * B.Z + (1 - U - V) * C.Z);
			return true;
		}
	}
	return false;
}

FString DublinDestruction::BuildingGeometryDigest(const FDublinCityBuilding& Building)
{
	FMD5 Hash;
	const FTCHARToUTF8 Id(*Building.Id);
	Hash.Update(reinterpret_cast<const uint8*>(Id.Get()), Id.Length());
	const double Pivot[] = {Building.PivotCm.X, Building.PivotCm.Y, Building.PivotCm.Z};
	Hash.Update(reinterpret_cast<const uint8*>(Pivot), sizeof(Pivot));
	for (const FVector& V : Building.Mesh.VerticesCm)
	{
		const double XYZ[] = {V.X, V.Y, V.Z};
		Hash.Update(reinterpret_cast<const uint8*>(XYZ), sizeof(XYZ));
	}
	for (const FVector2D& UV : Building.Mesh.UV)
	{
		const double Values[] = {UV.X, UV.Y};
		Hash.Update(reinterpret_cast<const uint8*>(Values), sizeof(Values));
	}
	Hash.Update(reinterpret_cast<const uint8*>(Building.Mesh.Triangles.GetData()),
		Building.Mesh.Triangles.Num() * sizeof(int32));
	Hash.Update(Building.MaterialIds.GetData(), Building.MaterialIds.Num());
	uint8 Digest[16];
	Hash.Final(Digest);
	return BytesToHex(Digest, 16).ToLower();
}

FString DublinDestruction::BuildingDigest(const FDublinCityBuilding& Building, EDublinFractureRecipe Recipe)
{
	if (!DublinFractureBake::IsSupportedRecipe(Recipe))
	{
		UE_LOG(LogTemp, Error, TEXT("Unsupported Dublin fracture recipe %d"), static_cast<int32>(Recipe));
		return FString();
	}
	const FString GeometryDigest = BuildingGeometryDigest(Building);
	FMD5 Hash;
	const int32 BakeVersion = UDublinCityFractureLibrary::CurrentBakeVersion;
	Hash.Update(reinterpret_cast<const uint8*>(&BakeVersion), sizeof(BakeVersion));
	const FTCHARToUTF8 Geometry(*GeometryDigest);
	Hash.Update(reinterpret_cast<const uint8*>(Geometry.Get()), Geometry.Length());
	for (const FColor& Color : Building.Mesh.ColorsRGBA)
	{
		const uint8 RGBA[] = {Color.R, Color.G, Color.B, Color.A};
		Hash.Update(RGBA, sizeof(RGBA));
	}
	if (Recipe == EDublinFractureRecipe::StructuralPilot)
	{
		const FTCHARToUTF8 Revision(DublinStructural::RecipeRevision);
		Hash.Update(reinterpret_cast<const uint8*>(Revision.Get()), Revision.Length());
	}
	else if (DublinFractureBake::IsDetailedRecipe(Recipe))
	{
		const FTCHARToUTF8 Revision(DublinDetail::RecipeRevision);
		Hash.Update(reinterpret_cast<const uint8*>(Revision.Get()), Revision.Length());
		const uint8 Kind = static_cast<uint8>(Recipe);
		Hash.Update(&Kind, sizeof(Kind));
	}
	uint8 Digest[16];
	Hash.Final(Digest);
	return BytesToHex(Digest, 16).ToLower();
}

TArray<int32> DublinDestruction::SelectImpactedFractureLeaves(const TArray<int32>& LeafTransforms,
	const TArray<FTransform>& CurrentMassTransforms, const FTransform& ComponentToWorld, const FDublinImpact& Impact,
	const TArray<FBox>* MassLocalBounds)
{
	TArray<int32> Selected;
	int32 Closest = INDEX_NONE;
	double ClosestDistanceSquared = TNumericLimits<double>::Max();
	const double RadiusSquared = FMath::Square(static_cast<double>(Impact.RadiusCm));
	for (int32 Leaf : LeafTransforms)
	{
		if (!CurrentMassTransforms.IsValidIndex(Leaf) || CurrentMassTransforms[Leaf].ContainsNaN()) { continue; }
		const FVector Position = ComponentToWorld.TransformPosition(CurrentMassTransforms[Leaf].GetLocation());
		double DistanceSquared = FVector::DistSquared(Position, Impact.PositionCm);
		if (IsHeightAwareBlast(Impact))
		{
			DistanceSquared = FVector::DistSquaredXY(Position, Impact.PositionCm);
			if (MassLocalBounds && MassLocalBounds->IsValidIndex(Leaf) && (*MassLocalBounds)[Leaf].IsValid)
			{
				const FBox Bounds = (*MassLocalBounds)[Leaf].TransformBy(CurrentMassTransforms[Leaf] * ComponentToWorld);
				const FVector Query(Impact.PositionCm.X, Impact.PositionCm.Y,
					FMath::Clamp(Impact.PositionCm.Z, Bounds.Min.Z, Bounds.Max.Z));
				DistanceSquared = Bounds.ComputeSquaredDistanceToPoint(Query);
			}
		}
		if (DistanceSquared <= RadiusSquared) { Selected.Add(Leaf); }
		if (DistanceSquared < ClosestDistanceSquared)
		{
			Closest = Leaf;
			ClosestDistanceSquared = DistanceSquared;
		}
	}
	// Small impacts retain their coarse-piece fallback; large blasts never damage beyond their footprint.
	if (Selected.IsEmpty() && Closest != INDEX_NONE && !IsHeightAwareBlast(Impact)) { Selected.Add(Closest); }
	return Selected;
}

FLinearColor DublinDestruction::SourceVertexLinearColor(const FDublinCityMesh& Mesh, int32 SourceIndex)
{
	// Decode RGB from sRGB, but preserve alpha as normalized byte data (including facade style codes).
	return FLinearColor(Mesh.ColorsRGBA.IsEmpty() ? FColor::White : Mesh.ColorsRGBA[SourceIndex]);
}

FDublinCitySection DublinDestruction::FilterIntactSection(const FDublinCitySection& Source,
	const TSet<int32>& RemovedBuildings)
{
	FDublinCitySection Out;
	for (int32 Triangle = 0; Triangle < Source.Triangles.Num() / 3; ++Triangle)
	{
		if (RemovedBuildings.Contains(Source.SourceBuildingPerTriangle[Triangle])) { continue; }
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 I = Source.Triangles[Triangle * 3 + Corner];
			Out.Triangles.Add(Out.Vertices.Num());
			Out.Vertices.Add(Source.Vertices[I]);
			Out.Normals.Add(Source.Normals[I]);
			Out.UV0.Add(Source.UV0[I]);
			Out.UV1.Add(Source.UV1[I]);
			Out.Colors.Add(Source.Colors[I]);
			Out.Tangents.Add(Source.Tangents[I]);
			Out.SourceVertexIndices.Add(Source.SourceVertexIndices[I]);
		}
		Out.SourceBuildingPerTriangle.Add(Source.SourceBuildingPerTriangle[Triangle]);
		Out.SourceTriangleIndices.Add(Source.SourceTriangleIndices[Triangle]);
	}
	return Out;
}

bool DublinDestruction::SyncGroundChunk(FDublinCityChunk& Chunk, const FDublinGroundMutation& Ground,
	const TSet<int32>& ChangedNodes)
{
	FDublinCitySection& Section = Chunk.Sections[0];
	bool Updated = false;
	for (int32 Vertex = 0; Vertex < Section.SourceVertexIndices.Num(); ++Vertex)
	{
		const int32 Source = Section.SourceVertexIndices[Vertex];
		if (!ChangedNodes.Contains(Source)) { continue; }
		Section.Vertices[Vertex].Z = Ground.Heights[Source] - Chunk.OriginCm.Z;
		Section.Normals[Vertex] = Ground.Normals[Source];
		const FVector& N = Section.Normals[Vertex];
		Section.Tangents[Vertex] = FProcMeshTangent((FVector::ForwardVector - N * N.X).GetSafeNormal(), false);
		Updated = true;
	}
	return Updated;
}

void FDublinGroundMutation::Initialize(const FDublinCityMesh& Source)
{
	Heights.SetNumUninitialized(Source.VerticesCm.Num());
	Normals.Init(FVector::UpVector, Source.VerticesCm.Num());
	GridToSource.Init(INDEX_NONE, DublinCity::TerrainGridSide * DublinCity::TerrainGridSide);
	NodeTriangles.Reset();
	NodeTriangles.SetNum(Source.VerticesCm.Num());
	for (int32 I = 0; I < Source.VerticesCm.Num(); ++I)
	{
		Heights[I] = Source.VerticesCm[I].Z;
		const int32 X = FMath::RoundToInt((Source.VerticesCm[I].X + 38400) / 200);
		const int32 Y = FMath::RoundToInt((Source.VerticesCm[I].Y + 38400) / 200);
		if (X >= 0 && X < 385 && Y >= 0 && Y < 385) { GridToSource[Y * 385 + X] = I; }
	}
	for (int32 Offset = 0; Offset < Source.Triangles.Num(); Offset += 3)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner) { NodeTriangles[Source.Triangles[Offset + Corner]].Add(Offset); }
	}
}

bool FDublinGroundMutation::Apply(const FDublinCityMesh& Source, const FDublinImpact& Impact, TSet<int32>& ChangedNodes)
{
	ChangedNodes.Reset();
	if (Heights.Num() != Source.VerticesCm.Num() || Impact.CraterDepthCm <= 0) { return false; }
	const double Reach = Impact.RadiusCm;
	const int32 MinX = FMath::Clamp(FMath::FloorToInt((Impact.PositionCm.X - Reach + 38400) / 200), 0, 384);
	const int32 MaxX = FMath::Clamp(FMath::CeilToInt((Impact.PositionCm.X + Reach + 38400) / 200), 0, 384);
	const int32 MinY = FMath::Clamp(FMath::FloorToInt((Impact.PositionCm.Y - Reach + 38400) / 200), 0, 384);
	const int32 MaxY = FMath::Clamp(FMath::CeilToInt((Impact.PositionCm.Y + Reach + 38400) / 200), 0, 384);
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const int32 I = GridToSource[Y * 385 + X];
			if (I == INDEX_NONE || NodeTriangles[I].IsEmpty() ||
				FMath::Abs(Heights[I] - Impact.PositionCm.Z) > Impact.RadiusCm + Impact.CraterDepthCm) { continue; }
			const double Distance = FVector::DistXY(Source.VerticesCm[I], Impact.PositionCm);
			const double Delta = DublinDestruction::CraterDelta(Distance, Impact.RadiusCm, Impact.CraterDepthCm);
			if (FMath::Abs(Delta) <= 1.e-6) { continue; }
			Heights[I] = FMath::Clamp(Heights[I] + Delta, Source.VerticesCm[I].Z - 10000, Source.VerticesCm[I].Z + 2000);
			ChangedNodes.Add(I);
		}
	}
	TSet<int32> NormalNodes = ChangedNodes;
	for (int32 Node : ChangedNodes)
	{
		for (int32 Offset : NodeTriangles[Node])
		{
			for (int32 Corner = 0; Corner < 3; ++Corner) { NormalNodes.Add(Source.Triangles[Offset + Corner]); }
		}
	}
	for (int32 Node : NormalNodes)
	{
		FVector Sum = FVector::ZeroVector;
		for (int32 Offset : NodeTriangles[Node])
		{
			FVector P[3];
			for (int32 C = 0; C < 3; ++C)
			{
				const int32 I = Source.Triangles[Offset + C];
				P[C] = Source.VerticesCm[I];
				P[C].Z = Heights[I];
			}
			Sum += FVector::CrossProduct(P[2] - P[0], P[1] - P[0]);
		}
		Normals[Node] = Sum.GetSafeNormal();
	}
	ChangedNodes = MoveTemp(NormalNodes);
	return !ChangedNodes.IsEmpty();
}

namespace
{
	TArray<FVector> ClipWaterPolygon(const TArray<FVector>& Polygon, int32 Axis, double Bound, bool KeepGreater)
	{
		TArray<FVector> Out;
		if (Polygon.IsEmpty()) { return Out; }
		FVector A = Polygon.Last();
		for (const FVector& B : Polygon)
		{
			const double DA = (A[Axis] - Bound) * (KeepGreater ? 1 : -1);
			const double DB = (B[Axis] - Bound) * (KeepGreater ? 1 : -1);
			if ((DA >= 0) != (DB >= 0)) { Out.Add(FMath::Lerp(A, B, DA / (DA - DB))); }
			if (DB >= 0) { Out.Add(B); }
			A = B;
		}
		return Out;
	}
}

bool FDublinWaterWaves::Initialize(const FDublinCityMesh& Source, FString& Error)
{
	*this = FDublinWaterWaves();
	Height.Init(0, Side * Side);
	Velocity.Init(0, Side * Side);
	NextVelocity.Init(0, Side * Side);
	Wet.Init(0, Side * Side);
	for (int32 Y = 0; Y < Side; ++Y)
	{
		for (int32 X = 0; X < Side; ++X)
		{
			float Z;
			if (DublinDestruction::SourceWaterZ(Source, FVector(X * CellCm - 38400, Y * CellCm - 38400, 0), Z))
			{
				const int32 I = Y * Side + X;
				Wet[I] = 1;
				WetNodes.Add(I);
			}
		}
	}
	for (int32 Offset = 0; Offset < Source.Triangles.Num(); Offset += 3)
	{
		TArray<FVector> Triangle;
		FBox Box(ForceInit);
		for (int32 C = 0; C < 3; ++C) { Triangle.Add(Source.VerticesCm[Source.Triangles[Offset + C]]); Box += Triangle.Last(); }
		const int32 X0 = FMath::Clamp(FMath::FloorToInt((Box.Min.X + 38400) / CellCm), 0, Side - 2);
		const int32 X1 = FMath::Clamp(FMath::FloorToInt((Box.Max.X + 38400) / CellCm), 0, Side - 2);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt((Box.Min.Y + 38400) / CellCm), 0, Side - 2);
		const int32 Y1 = FMath::Clamp(FMath::FloorToInt((Box.Max.Y + 38400) / CellCm), 0, Side - 2);
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				TArray<FVector> P = ClipWaterPolygon(Triangle, 0, X * CellCm - 38400, true);
				P = ClipWaterPolygon(P, 0, (X + 1) * CellCm - 38400, false);
				P = ClipWaterPolygon(P, 1, Y * CellCm - 38400, true);
				P = ClipWaterPolygon(P, 1, (Y + 1) * CellCm - 38400, false);
				for (int32 C = 1; C + 1 < P.Num(); ++C)
				{
					if (FVector::CrossProduct(P[C + 1] - P[0], P[C] - P[0]).Z <= 1.e-6) { continue; }
					for (const FVector& V : {P[0], P[C], P[C + 1]})
					{
						RenderMesh.Triangles.Add(RenderMesh.Vertices.Num());
						RenderMesh.Vertices.Add(V);
						RenderMesh.UV0.Add(DublinCity::AerialUV(V));
						RenderMesh.UV1.Add(DublinCity::AerialUV(V));
						RenderMesh.Normals.Add(FVector::UpVector);
						RenderMesh.Tangents.Add(FProcMeshTangent());
					}
				}
				if (RenderMesh.Vertices.Num() > 250000)
				{
					Error = TEXT("Clipped 4m water simulation mesh exceeds 250000 vertices");
					return false;
				}
			}
		}
	}
	BaseVertices = RenderMesh.Vertices;
	return true;
}

float FDublinWaterWaves::SampleDisplacement(const FVector& Point) const
{
	if (Height.Num() != Side * Side) { return 0; }
	const double GX = FMath::Clamp((Point.X + 38400) / CellCm, 0.0, static_cast<double>(Side - 1));
	const double GY = FMath::Clamp((Point.Y + 38400) / CellCm, 0.0, static_cast<double>(Side - 1));
	const int32 X = FMath::Min(FMath::FloorToInt(GX), Side - 2), Y = FMath::Min(FMath::FloorToInt(GY), Side - 2);
	return FMath::Lerp(FMath::Lerp(Height[Y * Side + X], Height[Y * Side + X + 1], GX - X),
		FMath::Lerp(Height[(Y + 1) * Side + X], Height[(Y + 1) * Side + X + 1], GX - X), GY - Y);
}

bool FDublinWaterWaves::AddImpact(const FDublinCityMesh& Source, const FDublinImpact& Impact)
{
	float Surface;
	if (!DublinDestruction::SourceWaterZ(Source, Impact.PositionCm, Surface)) { return false; }
	bool Added = false;
	const double Radius = Impact.RadiusCm + CellCm;
	for (int32 I : WetNodes)
	{
		const FVector P((I % Side) * CellCm - 38400, (I / Side) * CellCm - 38400, Surface);
		const double Q = FVector::DistXY(P, Impact.PositionCm) / Radius;
		if (Q >= 1) { continue; }
		Velocity[I] = FMath::Clamp(Velocity[I] + static_cast<float>(FMath::Clamp(Impact.Strength * 35, 40.0f, 1200.0f) *
			FMath::Square(1 - Q * Q)), -1500.0f, 1500.0f);
		Added = true;
	}
	if (Added) { bActive = true; AgeSeconds = 0; }
	return Added;
}

bool FDublinWaterWaves::Step(float DeltaSeconds)
{
	if (!bActive || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0 || DeltaSeconds > 1.0f / 30 + 1.e-5f) { return false; }
	// Four stable substeps: c*dt/dx=0.01875, well below the 2D CFL limit.
	const float Dt = DeltaSeconds / 4;
	constexpr float C2OverH2 = 900.0f * 900.0f / (400.0f * 400.0f);
	for (int32 Substep = 0; Substep < 4; ++Substep)
	{
		for (int32 I : WetNodes)
		{
			float Laplace = 0;
			const int32 X = I % Side, Y = I / Side;
			for (int32 Neighbor : {X > 0 ? I - 1 : -1, X + 1 < Side ? I + 1 : -1,
				Y > 0 ? I - Side : -1, Y + 1 < Side ? I + Side : -1})
			{
				// Reflecting (zero-normal-gradient) bank boundary.
				Laplace += Neighbor >= 0 && Wet[Neighbor] ? Height[Neighbor] - Height[I] : 0;
			}
			NextVelocity[I] = (Velocity[I] + C2OverH2 * Laplace * Dt) * FMath::Exp(-1.0f * Dt);
		}
		for (int32 I : WetNodes)
		{
			Velocity[I] = NextVelocity[I];
			Height[I] = FMath::Clamp(Height[I] + Velocity[I] * Dt, -300.0f, 300.0f);
		}
	}
	AgeSeconds += DeltaSeconds;
	MaxHeightCm = 0;
	float Speed = 0;
	for (int32 I : WetNodes) { MaxHeightCm = FMath::Max(MaxHeightCm, FMath::Abs(Height[I])); Speed = FMath::Max(Speed, FMath::Abs(Velocity[I])); }
	// A weak restoring term removes the non-propagating zero-frequency offset of an impulsive finite patch.
	for (int32 I : WetNodes) { Height[I] *= FMath::Exp(-0.15f * DeltaSeconds); }
	if (AgeSeconds > 1 && MaxHeightCm < 0.05f && Speed < 0.05f)
	{
		for (int32 I : WetNodes) { Height[I] = 0; Velocity[I] = 0; }
		bActive = false;
		MaxHeightCm = 0;
	}
	return true;
}

void FDublinWaterWaves::UpdateRenderVertices()
{
	for (int32 I = 0; I < BaseVertices.Num(); ++I)
	{
		const FVector& P = BaseVertices[I];
		RenderMesh.Vertices[I].Z = P.Z + SampleDisplacement(P);
		const float DX = (SampleDisplacement(P + FVector(200, 0, 0)) - SampleDisplacement(P - FVector(200, 0, 0))) / 400;
		const float DY = (SampleDisplacement(P + FVector(0, 200, 0)) - SampleDisplacement(P - FVector(0, 200, 0))) / 400;
		RenderMesh.Normals[I] = FVector(-DX, -DY, 1).GetSafeNormal();
		RenderMesh.Tangents[I] = FProcMeshTangent(FVector(1, 0, DX).GetSafeNormal(), false);
	}
}
