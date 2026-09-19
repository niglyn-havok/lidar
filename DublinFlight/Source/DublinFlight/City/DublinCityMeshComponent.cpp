#include "City/DublinCityMeshComponent.h"

#include "City/DublinCityData.h"
#include "Engine/StreamableRenderAsset.h"
#include "MeshUVChannelInfo.h"

namespace
{
	bool IsCityAerialTexture(const UStreamableRenderAsset* Asset)
	{
		if (!Asset) { return false; }
		const FString Path = Asset->GetPathName();
		return Path == TEXT("/Game/Textures/T_DublinOrtho.T_DublinOrtho") ||
			Path == TEXT("/Game/Textures/T_DublinCoverage.T_DublinCoverage");
	}
}

bool UDublinCityMeshComponent::IsGeneratedCityMeshClass(const UClass* Class)
{
	return Class == StaticClass() || Class == UProceduralMeshComponent::StaticClass();
}

bool UDublinCityMeshComponent::GetMaterialStreamingData(int32 MaterialIndex, FPrimitiveMaterialInfo& MaterialData) const
{
	// Only the two aerial texture entries consume this candidate density; other textures retain Super's data.
	static const FMeshUVChannelInfo AerialUVs = []
	{
		FMeshUVChannelInfo Result(0.0f);
		Result.LocalUVDensities[0] = static_cast<float>(DublinCity::ExtentMeters * 100.0);
		return Result;
	}();
	MaterialData.Material = GetMaterial(MaterialIndex);
	MaterialData.UVChannelData = &AerialUVs;
	MaterialData.PackedRelativeBox = PackedRelativeBox_Identity;
	return MaterialData.IsValid();
}

void UDublinCityMeshComponent::GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext,
	TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const
{
	const int32 FirstEntry = OutStreamingRenderAssets.Num();
	Super::GetStreamingRenderAssetInfo(LevelContext, OutStreamingRenderAssets);
	if (OutStreamingRenderAssets.Num() == FirstEntry) { return; }

	TArray<FStreamingRenderAssetPrimitiveInfo> AerialCandidates;
	// PMC's GetStreamingScale() is 1: unlike static meshes, its fallback already uses world bounds.
	// Supply world scale here exactly once; retain actual component bounds, not the full survey bounds.
	GetStreamingTextureInfoInner(LevelContext, nullptr,
		static_cast<float>(GetComponentTransform().GetMaximumAxisScale()), AerialCandidates);
	for (int32 Index = FirstEntry; Index < OutStreamingRenderAssets.Num(); ++Index)
	{
		FStreamingRenderAssetPrimitiveInfo& Info = OutStreamingRenderAssets[Index];
		if (!IsCityAerialTexture(Info.RenderAsset.Get())) { continue; }
		float Density = 0.0f;
		for (const FStreamingRenderAssetPrimitiveInfo& Candidate : AerialCandidates)
		{
			if (Candidate.RenderAsset == Info.RenderAsset)
			{
				Density = FMath::Max(Density, Candidate.TexelFactor);
			}
		}
		if (ensureMsgf(FMath::IsFinite(Density) && Density > 0.0f,
			TEXT("City aerial texture has no valid UV0 streaming density: %s"), *Info.RenderAsset->GetPathName()))
		{
			Info.TexelFactor = Density;
		}
	}
}
