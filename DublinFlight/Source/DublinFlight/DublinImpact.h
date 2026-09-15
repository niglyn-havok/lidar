#pragma once

#include "CoreMinimal.h"

enum class EDublinImpactKind : uint8
{
	Cannon,
	Bomb
};

struct FDublinImpact
{
	EDublinImpactKind Kind = EDublinImpactKind::Cannon;
	FVector PositionCm = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	float RadiusCm = 200.0f;
	float CraterDepthCm = 50.0f;
	float Strength = 1.0f;
	float YieldTonsTNT = 0.0f;
	int32 Seed = 0;
	bool bWater = false;

	bool IsValid() const
	{
		return !PositionCm.ContainsNaN() && !Normal.ContainsNaN()
			&& FMath::IsFinite(RadiusCm) && RadiusCm > 0.0f
			&& FMath::IsFinite(CraterDepthCm) && CraterDepthCm >= 0.0f
			&& FMath::IsFinite(Strength) && Strength > 0.0f
			&& FMath::IsFinite(YieldTonsTNT) && YieldTonsTNT >= 0.0f;
	}
};
