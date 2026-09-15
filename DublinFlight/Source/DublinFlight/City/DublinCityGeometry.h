#pragma once

#include "City/DublinCityData.h"
#include "ProceduralMeshComponent.h"

struct FDublinCitySection
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FVector2D> UV1;
	TArray<FColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	// Terrain entries address immutable source nodes; building entries address source corners.
	TArray<int32> SourceVertexIndices;
	TArray<int32> SourceBuildingPerTriangle;
	TArray<int32> SourceTriangleIndices;
};

struct FDublinCityChunk
{
	FIntPoint Cell = FIntPoint::ZeroValue;
	FVector OriginCm = FVector::ZeroVector;
	TArray<FDublinCitySection> Sections;
	TArray<int32> BuildingIndices;
};

namespace DublinCity
{
	DUBLINFLIGHT_API FIntPoint SpatialCell(const FVector& PositionCm);
	DUBLINFLIGHT_API FVector2D AerialUV(const FVector& CityPositionCm);
	// Clockwise UE face normal: (C-A) x (B-A). Indices are never flipped.
	DUBLINFLIGHT_API FVector ClockwiseNormal(const FVector& A, const FVector& B, const FVector& C);
	DUBLINFLIGHT_API bool MakeTerrainChunks(const FDublinCityMesh& Source,
		TArray<FDublinCityChunk>& Out, FString& Error, int32 GridSide = TerrainGridSide,
		double SpacingCm = TerrainSpacingCm);
	DUBLINFLIGHT_API bool MakeBuildingChunks(const TArray<FDublinCityBuilding>& Buildings,
		TArray<FDublinCityChunk>& Out, FString& Error);
	DUBLINFLIGHT_API bool MakeWaterSection(const FDublinCityMesh& Source, FDublinCitySection& Out, FString& Error);
}
