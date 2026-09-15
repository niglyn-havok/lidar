#pragma once

#include "City/DublinCityGeometry.h"
#include "DublinImpact.h"

struct FDublinGroundMutation;

namespace DublinDestruction
{
	inline constexpr int32 MaxQueuedImpacts = 64;
	inline constexpr int32 MaxActiveCollections = 24;
	inline constexpr int32 MaxActivePieces = 1536;
	inline constexpr float MaxRadiusCm = 38400;
	inline constexpr float MaxDepthCm = 10000;
	inline constexpr int32 StrainPropagationDepth = 0;
	inline constexpr float StrainPropagationFactor = 0;
	inline constexpr double MaxFractureVelocityChangeCmPerSecond = 5000;
	DUBLINFLIGHT_API float ImpactStrain(const FDublinImpact& Impact);
	DUBLINFLIGHT_API FVector ImpactVelocityChange(const FDublinImpact& Impact, const FVector& WorldMassCenter);
	DUBLINFLIGHT_API bool ValidateImpact(const FDublinImpact& Impact, FString& Error);
	DUBLINFLIGHT_API double CraterDelta(double DistanceCm, double RadiusCm, double DepthCm);
	DUBLINFLIGHT_API bool SphereTouchesBox(const FVector& Center, double Radius, const FBox& Box);
	DUBLINFLIGHT_API bool SourceWaterZ(const FDublinCityMesh& Source, const FVector& Point, float& Z);
	DUBLINFLIGHT_API FString BuildingGeometryDigest(const FDublinCityBuilding& Building);
	DUBLINFLIGHT_API FString BuildingDigest(const FDublinCityBuilding& Building);
	DUBLINFLIGHT_API TArray<int32> SelectImpactedFractureLeaves(const TArray<int32>& LeafTransforms,
		const TArray<FTransform>& CurrentMassTransforms, const FTransform& ComponentToWorld, const FDublinImpact& Impact);
	DUBLINFLIGHT_API FLinearColor SourceVertexLinearColor(const FDublinCityMesh& Mesh, int32 SourceIndex);
	DUBLINFLIGHT_API FDublinCitySection FilterIntactSection(const FDublinCitySection& Source,
		const TSet<int32>& RemovedBuildings);
	DUBLINFLIGHT_API bool SyncGroundChunk(FDublinCityChunk& Chunk, const FDublinGroundMutation& Ground,
		const TSet<int32>& ChangedNodes);
}

struct FDublinGroundMutation
{
	TArray<double> Heights;
	TArray<int32> GridToSource;
	TArray<TArray<int32>> NodeTriangles;
	TArray<FVector> Normals;
	void Initialize(const FDublinCityMesh& Source);
	bool Apply(const FDublinCityMesh& Source, const FDublinImpact& Impact, TSet<int32>& ChangedNodes);
};

struct FDublinWaterWaves
{
	static constexpr int32 Side = 193;
	static constexpr double CellCm = 400;
	TArray<float> Height;
	TArray<float> Velocity;
	TArray<float> NextVelocity;
	TArray<uint8> Wet;
	TArray<int32> WetNodes;
	FDublinCitySection RenderMesh;
	TArray<FVector> BaseVertices;
	bool bActive = false;
	float MaxHeightCm = 0;
	float AgeSeconds = 0;

	bool Initialize(const FDublinCityMesh& Source, FString& Error);
	bool AddImpact(const FDublinCityMesh& Source, const FDublinImpact& Impact);
	bool Step(float DeltaSeconds);
	float SampleDisplacement(const FVector& Point) const;
	void UpdateRenderVertices();
};
