#pragma once

#include "City/DublinCityData.h"

class UGeometryCollection;
class UGeometryCollectionComponent;
struct FDublinFractureRecord;
class FGeometryCollection;
struct FMeshDescription;
struct FPlanarCells;

namespace DublinStructural
{
	// Bump for any geometry, attribute-transfer or collision policy change; legacy hashes never include this.
	inline constexpr TCHAR RecipeRevision[] =
		TEXT("Dublin.StructuralPilot.r1.wall30.slab20.equalClear320.seeds288.voronoiNoNoiseNoGrout.cw.exteriorUV2.interior3.leafCompound");
	inline constexpr int32 SeedTarget = 288;
	inline constexpr double WallCm = 30;
	inline constexpr double SlabCm = 20;
	inline constexpr double ContactToleranceCm = 0.01;

	struct FMember
	{
		TArray<FVector2D> Polygon;
		double MinZ = 0;
		double MaxZ = 0;
		FVector2D BottomSlope = FVector2D::ZeroVector;
		FVector2D TopSlope = FVector2D::ZeroVector;
		FVector2D WallTangent = FVector2D::ZeroVector;
		double Weight = 0;
		int32 Seeds = 0;
		bool bWall = false;
		bool bSourcePlaneExterior = false;
	};

	struct FAssembly
	{
		TArray<FVector2D> Outer;
		TArray<FVector2D> Inner;
		TArray<FMember> Members;
		TArray<FVector2D> RoomsZ;
		double MinZ = 0;
		double MaxZ = 0;
		double SourceVolumeCm3 = 0;
		double StructuralVolumeCm3 = 0;
	};

	DUBLINFLIGHT_API bool IsPilotId(const FString& Id);
	DUBLINFLIGHT_API bool BuildAssembly(const FDublinCityBuilding& Building, FAssembly& Out, FString& Error);
	DUBLINFLIGHT_API double Area(const TArray<FVector2D>& Polygon);
	DUBLINFLIGHT_API TArray<FVector2D> Clip(const TArray<FVector2D>& Polygon, const FVector2D& Normal, double Offset);
	DUBLINFLIGHT_API bool ValidateCookedCollision(const UGeometryCollection& Collection,
		const FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool ValidateBuildingCollision(const UGeometryCollection& Collection, const FDublinCityBuilding& Building,
		const FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool ValidateRegisteredCollision(UGeometryCollectionComponent& Component, const FDublinCityBuilding& Building,
		const FDublinFractureRecord& Record, FString& Error);
#if WITH_EDITOR
	DUBLINFLIGHT_API bool FindFaceNormal(TConstArrayView<FVector> Positions, FVector& Normal);
	DUBLINFLIGHT_API bool MakeMemberMeshDescription(const FDublinCityBuilding& Building, const FAssembly& Assembly,
		const FMember& Member, FMeshDescription& Mesh, FString& Error);
	DUBLINFLIGHT_API bool MakeConvexMeshDescription(const FDublinCityBuilding& Building,
		const TArray<TArray<FVector>>& Faces, FMeshDescription& Mesh, FString& Error);
	DUBLINFLIGHT_API bool AppendDoubleClippedCells(const FDublinCityBuilding& Building,
		const TArray<TArray<FVector>>& MemberFaces, const FPlanarCells& Cells, FGeometryCollection& Geometry,
		TArray<int32>& LeafBones, double& MemberVolumeCm3, FString& Error);
	DUBLINFLIGHT_API bool ValidateLeafGeometry(FGeometryCollection& Geometry, int32 Bone, double& Volume, FString& Error);
	DUBLINFLIGHT_API bool RemoveExactZeroAreaFaces(FGeometryCollection& Geometry, int32 Bone, int32& Removed, FString& Error);
	DUBLINFLIGHT_API bool NormalizeGeometryOrder(FGeometryCollection& Geometry, FString& Error);
#if WITH_DEV_AUTOMATION_TESTS
	struct FProbeEquivalenceStats
	{
		uint64 PossibleCandidates = 0;
		uint64 NarrowphaseCandidates = 0;
		uint64 FallbackQueries = 0;
		double ReferenceMilliseconds = 0;
		double AcceleratedMilliseconds = 0;
	};
	DUBLINFLIGHT_API bool ValidateRegisteredProbeEquivalence(UGeometryCollectionComponent& Component,
		const FDublinFractureRecord& Record, FString& Error, FProbeEquivalenceStats* Stats = nullptr);
	DUBLINFLIGHT_API bool StabilizeExteriorTriangulation(const FDublinCityBuilding& Building, FGeometryCollection& Geometry,
		int32 Bone, int32& Flipped, FString& Error);
	// Candidate-only topology repair; retains separate UV/color corners and validates the replacement before publication.
	DUBLINFLIGHT_API bool RepairLeafSeams(FGeometryCollection& Geometry, int32 Bone, FString& Error,
		double ClosedMemberVolumeCm3 = 0, const FDublinCityBuilding* SourceBuilding = nullptr);
#endif
	DUBLINFLIGHT_API bool ValidateExteriorGeometry(const FDublinCityBuilding& Building, const FGeometryCollection& Geometry,
		const FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool BuildCollection(const FDublinCityBuilding& Building, UGeometryCollection& Asset,
		FDublinFractureRecord& Record, FString& Error, int32 Attempt = 0);
#endif
}
