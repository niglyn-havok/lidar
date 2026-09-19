#pragma once

#include "City/DublinCityGeometry.h"
#include "City/DublinCityFractureLibrary.h"
#include "DublinImpact.h"

struct FDublinGroundMutation;

namespace DublinDestruction
{
	inline constexpr int32 BudgetPolicyVersion = 2;
	// Whole-catalog resident ceilings, never an awake-body throttle. No slot is reclaimed by sleep.
	inline constexpr int32 MaxQueuedImpacts = 1024;
	inline constexpr int32 MaxActiveCollections = 1044;
	inline constexpr int32 MaxActivePieces = 1048576;
	inline constexpr int32 MaxCatalogHullSlots = 8388608;
	inline constexpr int32 LegacyHullSlotsPerLeaf = 16;
	inline constexpr int32 MaxConcurrentFractureLoads = 16;
	inline constexpr int32 MaxRegistrationsPerFrame = 8;
	inline constexpr int32 MaxOrdinaryRegistrationsPerFrame = 1;
	inline constexpr float OrdinaryTickIntervalSeconds = 1.0f / 30;
	inline constexpr int32 MaxCollectionImpactsPerFrame = 256;
	inline constexpr double FractureLoadTimeoutSeconds = 20;
	inline constexpr float HeightAwareBombYield = 100;
	inline constexpr float MaxRadiusCm = 38400;
	inline constexpr float MaxDepthCm = 10000;
	inline constexpr int32 StrainPropagationDepth = 0;
	inline constexpr float StrainPropagationFactor = 0;
	inline constexpr double MaxFractureVelocityChangeCmPerSecond = 5000;
	DUBLINFLIGHT_API float ImpactStrain(const FDublinImpact& Impact);
	DUBLINFLIGHT_API FVector ImpactVelocityChange(const FDublinImpact& Impact, const FVector& WorldMassCenter);
	DUBLINFLIGHT_API bool IsHeightAwareBlast(const FDublinImpact& Impact);
	DUBLINFLIGHT_API int32 RegistrationLimitForImpact(const FDublinImpact& Impact);
	DUBLINFLIGHT_API double FootprintDistanceSquared(const FDublinCityBuilding& Building, const FVector& Point);
	DUBLINFLIGHT_API int32 ReservedHullSlots(const FDublinFractureRecord& Record);
	struct FCatalogBudget
	{
		int32 Collections = 0;
		int32 LeafSlots = 0;
		int32 HullSlots = 0;
	};
	DUBLINFLIGHT_API bool AddCatalogRecord(const FDublinFractureRecord& Record, FCatalogBudget& Budget, FString& Error);
	DUBLINFLIGHT_API bool ValidateImpact(const FDublinImpact& Impact, FString& Error);
	DUBLINFLIGHT_API double CraterDelta(double DistanceCm, double RadiusCm, double DepthCm);
	DUBLINFLIGHT_API bool SphereTouchesBox(const FVector& Center, double Radius, const FBox& Box);
	DUBLINFLIGHT_API bool SourceWaterZ(const FDublinCityMesh& Source, const FVector& Point, float& Z);
	DUBLINFLIGHT_API FString BuildingGeometryDigest(const FDublinCityBuilding& Building);
	DUBLINFLIGHT_API FString BuildingDigest(const FDublinCityBuilding& Building,
		EDublinFractureRecipe Recipe = EDublinFractureRecipe::SolidGrid);
	DUBLINFLIGHT_API TArray<int32> SelectImpactedFractureLeaves(const TArray<int32>& LeafTransforms,
		const TArray<FTransform>& CurrentMassTransforms, const FTransform& ComponentToWorld, const FDublinImpact& Impact,
		const TArray<FBox>* MassLocalBounds = nullptr);
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
