#pragma once

#include "City/DublinCityStructural.h"
#include "City/DublinCityFractureLibrary.h"

class FGeometryCollection;
struct FMeshDescription;
struct FPlanarCells;

namespace DublinDetail
{
	inline constexpr TCHAR RecipeRevision[] = TEXT("Dublin.Detail.r19.holes.offset30.floor20.normalRoof20.equalClear320.nativePlanarSiteVoronoi.doubleAttributedHalfSpaceClip.sharedIntersectionCache.singleFloatConversion.noPostCutRescue.seedBudgetExact.canonicalSourceRoundoff.exactZeroOnly.preserveBoundarySeams.interiorPrecision001cm.closedMemberVolumeLedger.orderedGeometryBlocks.tiers128-384-768-dense1536.compound.noStrip");
#if WITH_EDITOR
	struct FPlan
	{
		FString SourceId;
		bool bSupported = false;
		EDublinFractureRecipe Recipe = EDublinFractureRecipe::StructuralCity;
		FString SourceDigest;
		FString Reason;
		int32 Tier = 128;
		int32 HoleCount = 0;
		bool bVariableRoof = false;
		bool bDenseForOversize = false;
		double BroadAreaM2 = 0;
		double LargestMemberAreaM2 = 0;
		DublinStructural::FAssembly Assembly;
		TArray<TArray<FVector>> Tetrahedra;
		TArray<FVector> VoidSamplesCm;
		TArray<FDublinFractureProbe> Probes;
	};

	// Analysis and preflight never create/save packages. AnalyzeCity retains a row for every failed ID.
	DUBLINFLIGHT_API bool AnalyzeBuilding(const FDublinCityBuilding& Building, FPlan& Plan, FString& Error);
	DUBLINFLIGHT_API bool AnalyzeCity(const FDublinCityData& City, TArray<FPlan>& Plans, TArray<FString>& Unsupported);
	DUBLINFLIGHT_API bool ResolveBakeRecipe(const FDublinCityBuilding& Building, EDublinFractureRecipe Requested,
		EDublinFractureRecipe& Resolved, FString& Error);
	// Candidate must be caller-owned, not an existing working collection. Publication is BakeBuilding's job.
	DUBLINFLIGHT_API bool BuildCollection(const FDublinCityBuilding& Building, UGeometryCollection& Candidate,
		FDublinFractureRecord& Record, FString& Error, int32 Attempt = 0);
	DUBLINFLIGHT_API bool PreflightGeometry(const FDublinCityBuilding& Building, FPlan& Plan, FString& Error);
	DUBLINFLIGHT_API bool PartitionFaceRemainder(const TArray<FVector2D>& Boundary,
		const TArray<TArray<FVector2D>>& ExteriorPatches, TArray<TArray<FVector2D>>& InteriorPatches, FString& Error,
		FString* CoverageDiagnostics = nullptr);
#endif
}
