#include "City/DublinCityGeometry.h"

namespace
{
	FProcMeshTangent MakeTangent(const FVector& A, const FVector& B, const FVector& C,
		const FVector2D& UVA, const FVector2D& UVB, const FVector2D& UVC, const FVector& Normal)
	{
		const FVector2D D1 = UVB - UVA;
		const FVector2D D2 = UVC - UVA;
		const double Det = D1.X * D2.Y - D1.Y * D2.X;
		FVector Tangent;
		FVector Bitangent;
		if (FMath::Abs(Det) > 1.e-12)
		{
			Tangent = ((B - A) * D2.Y - (C - A) * D1.Y) / Det;
			Bitangent = ((C - A) * D1.X - (B - A) * D2.X) / Det;
		}
		else
		{
			Tangent = FMath::Abs(Normal.Z) < 0.9 ?
				FVector::CrossProduct(FVector::UpVector, Normal) : FVector::ForwardVector;
			Bitangent = FVector::CrossProduct(Normal, Tangent);
		}
		Tangent = (Tangent - Normal * FVector::DotProduct(Tangent, Normal)).GetSafeNormal();
		return FProcMeshTangent(Tangent,
			FVector::DotProduct(FVector::CrossProduct(Normal, Tangent), Bitangent) < 0);
	}

	bool ValidMesh(const FDublinCityMesh& Source, FString& Error)
	{
		if (Source.UV.Num() != Source.VerticesCm.Num() || Source.Triangles.Num() % 3 != 0 ||
			(!Source.ColorsRGBA.IsEmpty() && Source.ColorsRGBA.Num() != Source.VerticesCm.Num()))
		{
			Error = TEXT("Mesh buffers have inconsistent lengths");
			return false;
		}
		for (int32 Index : Source.Triangles)
		{
			if (!Source.VerticesCm.IsValidIndex(Index))
			{
				Error = TEXT("Mesh triangle index is out of range");
				return false;
			}
		}
		return true;
	}

	void AddCorner(FDublinCitySection& Section, const FDublinCityMesh& Source, int32 SourceIndex,
		const FVector& Position, const FVector& Normal, const FVector2D& UV, const FVector2D& UV1,
		const FProcMeshTangent& Tangent)
	{
		Section.Vertices.Add(Position);
		Section.Normals.Add(Normal);
		Section.UV0.Add(UV);
		Section.UV1.Add(UV1);
		Section.Colors.Add(Source.ColorsRGBA.IsEmpty() ? FColor::White : Source.ColorsRGBA[SourceIndex]);
		Section.Tangents.Add(Tangent);
		Section.SourceVertexIndices.Add(SourceIndex);
	}
}

FIntPoint DublinCity::SpatialCell(const FVector& PositionCm)
{
	constexpr double HalfCm = ExtentMeters * 50;
	return FIntPoint(FMath::Clamp(FMath::FloorToInt((PositionCm.X + HalfCm) / ChunkSizeCm), 0, 11),
		FMath::Clamp(FMath::FloorToInt((PositionCm.Y + HalfCm) / ChunkSizeCm), 0, 11));
}

FVector2D DublinCity::AerialUV(const FVector& Position)
{
	constexpr double FullCm = ExtentMeters * 100;
	return FVector2D(Position.X / FullCm + 0.5, Position.Y / FullCm + 0.5);
}

FVector DublinCity::ClockwiseNormal(const FVector& A, const FVector& B, const FVector& C)
{
	return FVector::CrossProduct(C - A, B - A).GetSafeNormal();
}

bool DublinCity::MakeTerrainChunks(const FDublinCityMesh& Source, TArray<FDublinCityChunk>& Out,
	FString& Error, int32 GridSide, double SpacingCm)
{
	Out.Reset();
	Error.Reset();
	if (GridSide < 2 || GridSide > TerrainGridSide || !FMath::IsFinite(SpacingCm) || SpacingCm <= 0 ||
		Source.VerticesCm.Num() != GridSide * GridSide ||
		Source.Triangles.IsEmpty() || Source.Triangles.Num() > (GridSide - 1) * (GridSide - 1) * 6 ||
		!ValidMesh(Source, Error))
	{
		Error = TEXT("Terrain requires a complete regular node grid with at most two triangles per cell (production: 385x385, 2m)");
		return false;
	}
	const double HalfCm = (GridSide - 1) * SpacingCm / 2;
	TArray<FIntPoint> Nodes;
	TArray<int32> NodeOwners;
	TArray<FVector> Normals;
	Nodes.SetNumUninitialized(Source.VerticesCm.Num());
	NodeOwners.Init(INDEX_NONE, Source.VerticesCm.Num());
	Normals.Init(FVector::ZeroVector, Source.VerticesCm.Num());
	for (int32 Index = 0; Index < Source.VerticesCm.Num(); ++Index)
	{
		const FVector& Position = Source.VerticesCm[Index];
		const double X = (Position.X + HalfCm) / SpacingCm;
		const double Y = (Position.Y + HalfCm) / SpacingCm;
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Position.Z) ||
			X < -0.001 || Y < -0.001 || X > GridSide - 1 + 0.001 || Y > GridSide - 1 + 0.001)
		{
			Error = TEXT("Terrain node outside grid");
			return false;
		}
		const FIntPoint Node(FMath::RoundToInt(X), FMath::RoundToInt(Y));
		const int32 Slot = Node.Y * GridSide + Node.X;
		if (FMath::Abs(X - Node.X) > 0.001 || FMath::Abs(Y - Node.Y) > 0.001 || NodeOwners[Slot] != INDEX_NONE)
		{
			Error = TEXT("Terrain has duplicate or off-grid nodes");
			return false;
		}
		NodeOwners[Slot] = Index;
		Nodes[Index] = Node;
	}
	TArray<uint8> CellTriangles;
	TArray<uint8> CellCorners;
	CellTriangles.Init(0, (GridSide - 1) * (GridSide - 1));
	CellCorners.Init(0, CellTriangles.Num());
	TMap<FIntPoint, int32> ChunkLookup;
	TArray<TMap<int32, int32>> Remaps;
	for (int32 Offset = 0; Offset < Source.Triangles.Num(); Offset += 3)
	{
		const int32 IA = Source.Triangles[Offset];
		const int32 IB = Source.Triangles[Offset + 1];
		const int32 IC = Source.Triangles[Offset + 2];
		const FIntPoint A = Nodes[IA], B = Nodes[IB], C = Nodes[IC];
		const int32 MinX = FMath::Min3(A.X, B.X, C.X), MinY = FMath::Min3(A.Y, B.Y, C.Y);
		const int32 MaxX = FMath::Max3(A.X, B.X, C.X), MaxY = FMath::Max3(A.Y, B.Y, C.Y);
		const FVector Cross = FVector::CrossProduct(Source.VerticesCm[IC] - Source.VerticesCm[IA],
			Source.VerticesCm[IB] - Source.VerticesCm[IA]);
		if (MaxX - MinX != 1 || MaxY - MinY != 1 || Cross.Z <= 0)
		{
			Error = TEXT("Terrain triangles must stay within one cell and face up using UE clockwise winding");
			return false;
		}
		const int32 CellIndex = MinY * (GridSide - 1) + MinX;
		uint8 Mask = 0;
		for (FIntPoint Node : {A, B, C}) { Mask |= 1 << ((Node.Y - MinY) * 2 + Node.X - MinX); }
		const uint8 SharedCorners = CellCorners[CellIndex] & Mask;
		if (++CellTriangles[CellIndex] > 2 || (CellCorners[CellIndex] != 0 &&
			((CellCorners[CellIndex] | Mask) != 15 || (SharedCorners != 6 && SharedCorners != 9))))
		{
			Error = TEXT("Terrain cell contains overlapping or duplicate triangles");
			return false;
		}
		CellCorners[CellIndex] |= Mask;
		Normals[IA] += Cross;
		Normals[IB] += Cross;
		Normals[IC] += Cross;
		const FVector Center((MinX + 0.5) * SpacingCm - HalfCm, (MinY + 0.5) * SpacingCm - HalfCm, 0);
		const FIntPoint Key = SpatialCell(Center);
		int32* Existing = ChunkLookup.Find(Key);
		int32 ChunkIndex;
		if (Existing) { ChunkIndex = *Existing; }
		else
		{
			if (Out.Num() >= MaxSpatialChunks) { Error = TEXT("Terrain chunk limit exceeded"); return false; }
			ChunkIndex = Out.AddDefaulted();
			ChunkLookup.Add(Key, ChunkIndex);
			Remaps.AddDefaulted();
			Out[ChunkIndex].Cell = Key;
			Out[ChunkIndex].OriginCm = FVector((Key.X + 0.5) * ChunkSizeCm - ExtentMeters * 50,
				(Key.Y + 0.5) * ChunkSizeCm - ExtentMeters * 50, 0);
			Out[ChunkIndex].Sections.SetNum(1);
		}
		FDublinCitySection& Section = Out[ChunkIndex].Sections[0];
		for (int32 SourceIndex : {IA, IB, IC})
		{
			int32* Mapped = Remaps[ChunkIndex].Find(SourceIndex);
			if (!Mapped)
			{
				const int32 NewIndex = Section.Vertices.Num();
				Remaps[ChunkIndex].Add(SourceIndex, NewIndex);
				AddCorner(Section, Source, SourceIndex, Source.VerticesCm[SourceIndex] - Out[ChunkIndex].OriginCm,
					FVector::UpVector, Source.UV[SourceIndex], Source.UV[SourceIndex], FProcMeshTangent());
				Section.Triangles.Add(NewIndex);
			}
			else { Section.Triangles.Add(*Mapped); }
		}
		Section.SourceTriangleIndices.Add(Offset / 3);
	}
	// The fixed source topology may omit either triangle at river-bank cells. Do not fill the water mask.
	for (FDublinCityChunk& Chunk : Out)
	{
		FDublinCitySection& Section = Chunk.Sections[0];
		for (int32 Index = 0; Index < Section.Vertices.Num(); ++Index)
		{
			const FVector Normal = Normals[Section.SourceVertexIndices[Index]].GetSafeNormal();
			Section.Normals[Index] = Normal;
			Section.Tangents[Index] = FProcMeshTangent(
				(FVector::ForwardVector - Normal * Normal.X).GetSafeNormal(), false);
		}
	}
	return true;
}

bool DublinCity::MakeBuildingChunks(const TArray<FDublinCityBuilding>& Buildings,
	TArray<FDublinCityChunk>& Out, FString& Error)
{
	Out.Reset();
	Error.Reset();
	if (Buildings.Num() > MaxBuildings) { Error = TEXT("Building limit exceeded"); return false; }
	TMap<FIntPoint, int32> Lookup;
	int64 RenderedVertices = 0;
	for (int32 BuildingIndex = 0; BuildingIndex < Buildings.Num(); ++BuildingIndex)
	{
		const FDublinCityBuilding& Building = Buildings[BuildingIndex];
		const FDublinCityMesh& Mesh = Building.Mesh;
		if (!ValidMesh(Mesh, Error) || Building.MaterialIds.Num() != Mesh.Triangles.Num() / 3)
		{
			if (Error.IsEmpty()) { Error = TEXT("Building material count mismatch"); }
			return false;
		}
		RenderedVertices += Mesh.Triangles.Num();
		if (RenderedVertices > MaxRenderedVertices) { Error = TEXT("Split building vertex budget exceeded"); return false; }
		const FIntPoint Key = SpatialCell(Building.PivotCm);
		int32* Existing = Lookup.Find(Key);
		int32 ChunkIndex;
		if (Existing) { ChunkIndex = *Existing; }
		else
		{
			if (Out.Num() >= MaxSpatialChunks) { Error = TEXT("Building chunk limit exceeded"); return false; }
			ChunkIndex = Out.AddDefaulted();
			Lookup.Add(Key, ChunkIndex);
			Out[ChunkIndex].Cell = Key;
			Out[ChunkIndex].OriginCm = FVector((Key.X + 0.5) * ChunkSizeCm - ExtentMeters * 50,
				(Key.Y + 0.5) * ChunkSizeCm - ExtentMeters * 50, Building.PivotCm.Z);
			Out[ChunkIndex].Sections.SetNum(3);
		}
		FDublinCityChunk& Chunk = Out[ChunkIndex];
		Chunk.BuildingIndices.Add(BuildingIndex);
		for (int32 Offset = 0; Offset < Mesh.Triangles.Num(); Offset += 3)
		{
			const uint8 Material = Building.MaterialIds[Offset / 3];
			if (Material > 2) { Error = TEXT("Building material ID outside 0..2"); return false; }
			FDublinCitySection& Section = Chunk.Sections[Material];
			const int32 IA = Mesh.Triangles[Offset], IB = Mesh.Triangles[Offset + 1], IC = Mesh.Triangles[Offset + 2];
			const FVector A = Mesh.VerticesCm[IA] + Building.PivotCm;
			const FVector B = Mesh.VerticesCm[IB] + Building.PivotCm;
			const FVector C = Mesh.VerticesCm[IC] + Building.PivotCm;
			const FVector Normal = ClockwiseNormal(A, B, C);
			if (Normal.IsNearlyZero() || Normal.ContainsNaN()) { Error = TEXT("Degenerate building triangle"); return false; }
			const FVector2D UVA = Material == 0 ? AerialUV(A) : Mesh.UV[IA];
			const FVector2D UVB = Material == 0 ? AerialUV(B) : Mesh.UV[IB];
			const FVector2D UVC = Material == 0 ? AerialUV(C) : Mesh.UV[IC];
			const FProcMeshTangent Tangent = MakeTangent(A, B, C, UVA, UVB, UVC, Normal);
			// Separate triangle corners guarantee hard facade/roof boundaries without assuming source smoothing groups.
			for (int32 SourceIndex : {IA, IB, IC})
			{
				const FVector World = Mesh.VerticesCm[SourceIndex] + Building.PivotCm;
				Section.Triangles.Add(Section.Vertices.Num());
				AddCorner(Section, Mesh, SourceIndex, World - Chunk.OriginCm, Normal,
					Material == 0 ? AerialUV(World) : Mesh.UV[SourceIndex], Mesh.UV[SourceIndex], Tangent);
			}
			Section.SourceBuildingPerTriangle.Add(BuildingIndex);
			Section.SourceTriangleIndices.Add(Offset / 3);
		}
	}
	return true;
}

bool DublinCity::MakeWaterSection(const FDublinCityMesh& Source, FDublinCitySection& Out, FString& Error)
{
	Out = FDublinCitySection();
	Error.Reset();
	if (!ValidMesh(Source, Error)) { return false; }
	if (Source.VerticesCm.Num() > 250000 || Source.Triangles.Num() > 750000)
	{
		Error = TEXT("Water surface exceeds single-component budget");
		return false;
	}
	Out.Vertices = Source.VerticesCm;
	Out.Triangles = Source.Triangles;
	Out.UV0 = Source.UV;
	Out.UV1 = Source.UV;
	Out.Colors = Source.ColorsRGBA;
	Out.Normals.Init(FVector::ZeroVector, Out.Vertices.Num());
	Out.Tangents.Init(FProcMeshTangent(), Out.Vertices.Num());
	for (int32 Offset = 0; Offset < Out.Triangles.Num(); Offset += 3)
	{
		const int32 A = Out.Triangles[Offset], B = Out.Triangles[Offset + 1], C = Out.Triangles[Offset + 2];
		const FVector Normal = ClockwiseNormal(Out.Vertices[A], Out.Vertices[B], Out.Vertices[C]);
		if (Normal.Z <= 0) { Error = TEXT("Water must face up using UE clockwise winding"); return false; }
		Out.Normals[A] += Normal;
		Out.Normals[B] += Normal;
		Out.Normals[C] += Normal;
	}
	for (FVector& Normal : Out.Normals) { Normal.Normalize(); }
	return true;
}
