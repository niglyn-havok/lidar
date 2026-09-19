#pragma once

#include "ProceduralMeshComponent.h"
#include "DublinCityMeshComponent.generated.h"

UCLASS()
class DUBLINFLIGHT_API UDublinCityMeshComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	virtual void GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext,
		TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const override;

	static bool IsGeneratedCityMeshClass(const UClass* Class);

protected:
	virtual bool GetMaterialStreamingData(int32 MaterialIndex, FPrimitiveMaterialInfo& MaterialData) const override;
};
