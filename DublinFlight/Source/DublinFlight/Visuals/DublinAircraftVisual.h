#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

namespace DublinAircraftVisual
{
	enum class EFinish : uint8
	{
		Pearl,
		Petrol,
		Graphite,
		Canopy,
		Brass,
		Count
	};

	inline constexpr int32 FinishCount = static_cast<int32>(EFinish::Count);
	inline constexpr double PropellerOffsetCm = 390.0;
	inline constexpr int32 MinimumTriangles = 2000;
	inline constexpr int32 MaximumTriangles = 6000;

	struct FSection
	{
		EFinish Finish = EFinish::Pearl;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UV;
		TArray<FProcMeshTangent> Tangents;
	};

	struct FGeometry
	{
		TArray<FSection> Airframe;
		// Local coordinates; the pawn places this at PropellerOffsetCm on +X.
		FSection Propeller;
	};

	struct FFinish
	{
		FLinearColor Tint;
		float Roughness;
	};

	// Pure, deterministic geometry in centimetres: +X nose, +Y right, +Z up.
	DUBLINFLIGHT_API FGeometry BuildGeometry();
	DUBLINFLIGHT_API FFinish GetFinish(EFinish Finish);
}
