#pragma once

#include "CoreMinimal.h"

struct FDublinCityMesh
{
	TArray<FVector> VerticesCm;
	TArray<int32> Triangles;
	TArray<FVector2D> UV;
	TArray<FColor> ColorsRGBA;
};

struct FDublinCityBuilding
{
	FString Id;
	FString Name;
	double Confidence = 0;
	FString HeightMethod;
	FVector PivotCm = FVector::ZeroVector;
	FDublinCityMesh Mesh;
	TArray<uint8> MaterialIds;
};

struct FDublinCityData
{
	FVector2D OriginEN = FVector2D::ZeroVector;
	double ExtentMeters = 0;
	TArray<FString> Attribution;
	FDublinCityMesh Terrain;
	FDublinCityMesh Water;
	TArray<FDublinCityBuilding> Buildings;
};

namespace DublinCity
{
	inline constexpr int64 MaxFileBytes = 100LL * 1024 * 1024;
	inline constexpr int32 MaxBuildings = 5000;
	inline constexpr int32 MaxSourceVertices = 2000000;
	inline constexpr int32 MaxSourceTriangles = 2000000;
	inline constexpr int32 MaxRenderedVertices = 4000000;
	inline constexpr double ExtentMeters = 768;
	inline constexpr double ChunkSizeCm = 6400;
	inline constexpr int32 MaxSpatialChunks = 144;
	inline constexpr int32 TerrainGridSide = 385;
	inline constexpr double TerrainSpacingCm = 200;

	DUBLINFLIGHT_API FVector SurveyToCity(double East, double North, double HeightMeters);
	DUBLINFLIGHT_API bool ResolveContentJsonPath(const FString& ContentDirectory,
		const FString& RelativePath, FString& OutPath, FString& OutError);
	DUBLINFLIGHT_API bool ParseCityJson(const FString& Json, FDublinCityData& OutData, FString& OutError);
	DUBLINFLIGHT_API bool LoadCityJson(const FString& RelativePath, FDublinCityData& OutData, FString& OutError);
}
