#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DublinCityFractureLibrary.generated.h"

class UGeometryCollection;
class UGeometryCollectionComponent;
class ADublinCityWorld;
struct FDublinCityBuilding;

UENUM(BlueprintType)
enum class EDublinFractureRecipe : uint8
{
	SolidGrid = 0,
	StructuralPilot = 1,
	StructuralCity = 2,
	LayeredMasonry = 3
};

USTRUCT()
struct FDublinFractureProbe
{
	GENERATED_BODY()
	UPROPERTY() FVector StartCm = FVector::ZeroVector;
	UPROPERTY() FVector EndCm = FVector::ZeroVector;
	UPROPERTY() bool bExpectHit = false;
};

USTRUCT(BlueprintType)
struct FDublinFractureRecord
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) EDublinFractureRecipe Recipe = EDublinFractureRecipe::SolidGrid;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString SourceId;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString GeometryDigest;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString SourceDigest;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TSoftObjectPtr<UGeometryCollection> Collection;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 PieceCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 RootTransform = INDEX_NONE;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<int32> Anchors;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<int32> LeafTransforms;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bReady = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bNaniteReady = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 BakeAttempts = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 FractureSiteCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 MergedMicroShards = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) double SourceVolumeCm3 = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) double StructuralVolumeCm3 = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) double RetainedVolumeCm3 = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 DetailTier = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 HullCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 CollisionStorageVersion = 0;
	// 0: legacy full proof; 1: MD5/32 hex; 2: BLAKE3/64 hex over the same v1 physical bytes.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 CollisionValidationVersion = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString CollisionValidationDigest;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 MemberCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString PlanReason;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) double BroadSurfaceAreaM2 = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) double LargestMemberAreaM2 = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bDenseForOversize = false;
	UPROPERTY() int32 ProbeVersion = 0;
	UPROPERTY() TArray<FVector> VoidSamplesCm;
	UPROPERTY() TArray<FDublinFractureProbe> CollisionProbes;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FString> BakeDiagnostics;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString Error;
};

UCLASS(BlueprintType)
class DUBLINFLIGHT_API UDublinCityFractureLibrary : public UDataAsset
{
	GENERATED_BODY()
public:
	static constexpr int32 CurrentBakeVersion = 3;
	static constexpr double TargetPieceSizeCm = 175.0;
	static constexpr int32 MaxFractureSites = 192;
	static constexpr int32 MaxPiecesPerBuilding = 384;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 BakeVersion = CurrentBakeVersion;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FDublinFractureRecord> Records;
	const FDublinFractureRecord* Find(const FString& Id) const;
};

namespace DublinFractureBake
{
	DUBLINFLIGHT_API bool IsSupportedRecipe(EDublinFractureRecipe Recipe);
	DUBLINFLIGHT_API bool IsDetailedRecipe(EDublinFractureRecipe Recipe);
	DUBLINFLIGHT_API bool HasValidPieceBudget(const FDublinFractureRecord& Record);
	DUBLINFLIGHT_API bool HasCompleteDetailCoverage(const TArray<FDublinCityBuilding>& Buildings,
		const UDublinCityFractureLibrary& Library, TArray<FString>& PendingIds);
	DUBLINFLIGHT_API bool IsCurrentRecord(const FDublinCityBuilding& Building, const FDublinFractureRecord& Record);
	DUBLINFLIGHT_API bool ValidateBakeRequest(EDublinFractureRecipe Recipe, const TArray<FString>& Ids,
		bool bBakeAll, FString& Error);
	DUBLINFLIGHT_API UDublinCityFractureLibrary* FindLibrary();
	DUBLINFLIGHT_API bool ValidateCollisionData(const UGeometryCollection& Collection,
		const FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool ComputeCollisionValidationDigest(const UGeometryCollection& Collection,
		const FDublinFractureRecord& Record, FString& Digest, FString& Error);
	DUBLINFLIGHT_API bool ValidateRuntimeCollisionData(const UGeometryCollection& Collection,
		const FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool RegisterRuntimePhysics(UGeometryCollectionComponent& Component,
		const FDublinFractureRecord& Record, FString& Error);
	// Validate before binding an asset; apply its defaults, then the city's existing fracture overrides.
	DUBLINFLIGHT_API bool PrepareAndRegisterRuntimePhysics(UGeometryCollectionComponent& Component,
		const UGeometryCollection& Asset, const FDublinFractureRecord& Record, FString& Error);
#if WITH_EDITOR
	DUBLINFLIGHT_API bool CertifyCollisionData(const UGeometryCollection& Collection, const FDublinCityBuilding& Building,
		FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool CertifyCurrentLibrary(FString& BackupPath, FString& Error);
	DUBLINFLIGHT_API TArray<FVector> MakeFractureSites(const FBox& Bounds, const FString& SourceId, int32 Attempt);
	DUBLINFLIGHT_API bool BuildConnectionGraph(UGeometryCollection& Collection, FString& Error);
	DUBLINFLIGHT_API bool HasCurrentMaterialBindings(const UGeometryCollection& Collection, const ADublinCityWorld& City);
	DUBLINFLIGHT_API UDublinCityFractureLibrary* CreateLibrary(FString& Error);
	DUBLINFLIGHT_API bool BakeBuilding(const FDublinCityBuilding& Building, ADublinCityWorld& City,
		FDublinFractureRecord& Record, FString& Error, EDublinFractureRecipe Recipe = EDublinFractureRecipe::SolidGrid);
	DUBLINFLIGHT_API bool SaveLibrary(UDublinCityFractureLibrary& Library, FString& Error);
	DUBLINFLIGHT_API bool PublishRecord(UDublinCityFractureLibrary& Library, const FDublinFractureRecord& Record,
		TFunctionRef<bool(UDublinCityFractureLibrary&, FString&)> Save, FString& Error);
#endif
}
