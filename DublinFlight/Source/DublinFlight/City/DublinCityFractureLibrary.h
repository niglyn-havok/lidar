#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DublinCityFractureLibrary.generated.h"

class UGeometryCollection;
class UGeometryCollectionComponent;
class ADublinCityWorld;
struct FDublinCityBuilding;

USTRUCT(BlueprintType)
struct FDublinFractureRecord
{
	GENERATED_BODY()
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
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) double RetainedVolumeCm3 = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FString> BakeDiagnostics;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString Error;
};

UCLASS(BlueprintType)
class DUBLINFLIGHT_API UDublinCityFractureLibrary : public UDataAsset
{
	GENERATED_BODY()
public:
	static constexpr int32 CurrentBakeVersion = 2;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 BakeVersion = CurrentBakeVersion;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FDublinFractureRecord> Records;
	const FDublinFractureRecord* Find(const FString& Id) const;
};

namespace DublinFractureBake
{
	DUBLINFLIGHT_API UDublinCityFractureLibrary* FindLibrary();
	DUBLINFLIGHT_API bool ValidateCollisionData(const UGeometryCollection& Collection,
		const FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool RegisterRuntimePhysics(UGeometryCollectionComponent& Component,
		const FDublinFractureRecord& Record, FString& Error);
#if WITH_EDITOR
	DUBLINFLIGHT_API TArray<FVector> MakeFractureSites(const FBox& Bounds, const FString& SourceId, int32 Attempt);
	DUBLINFLIGHT_API bool BuildConnectionGraph(UGeometryCollection& Collection, FString& Error);
	DUBLINFLIGHT_API bool HasCurrentMaterialBindings(const UGeometryCollection& Collection, const ADublinCityWorld& City);
	DUBLINFLIGHT_API UDublinCityFractureLibrary* CreateLibrary(FString& Error);
	DUBLINFLIGHT_API bool BakeBuilding(const FDublinCityBuilding& Building, ADublinCityWorld& City,
		FDublinFractureRecord& Record, FString& Error);
	DUBLINFLIGHT_API bool SaveLibrary(UDublinCityFractureLibrary& Library, FString& Error);
#endif
}
