#pragma once

#include "CoreMinimal.h"
#include "DublinImpact.h"
#include "InputCoreTypes.h"
#include "Templates/Function.h"

namespace DublinWeapons
{
inline constexpr float MaximumBombRadiusCm = 12000.0f;
inline constexpr float BombRadiusExponent = 0.4336766652f;

struct FBombCurve
{
	float RadiusAtOneCm = 600.0f;
	float DepthAtOneCm = 250.0f;
	float StrengthAtOne = 2.0f;
	float Exponent = 0.25f;
	float MaxRadiusCm = MaximumBombRadiusCm;
	float MaxDepthCm = 2000.0f;
	float MaxStrength = 50.0f;
};

// Deliberately bounded game tuning, not an explosives-engineering model.
DUBLINFLIGHT_API float ClampYield(float Yield);
DUBLINFLIGHT_API FDublinImpact MakeImpact(EDublinImpactKind Kind, float Yield, const FBombCurve& Curve);
DUBLINFLIGHT_API FDublinImpact MakeGroundImpact(const FDublinImpact& Impact);
DUBLINFLIGHT_API FVector BallisticPosition(const FVector& Start, const FVector& InheritedVelocity,
	double GravityZ, double Seconds);
DUBLINFLIGHT_API bool FindWaterCrossing(const FVector& Start, const FVector& End,
	TFunctionRef<bool(const FVector&, float&)> QuerySurface, FVector& OutPosition);
DUBLINFLIGHT_API bool IsWeaponKey(const FKey& Key);

class DUBLINFLIGHT_API FCooldown
{
public:
	bool TryConsume(double Now, double Interval);
	double GetNextAllowedTime() const { return NextAllowed; }
private:
	double NextAllowed = -1.0;
};

class DUBLINFLIGHT_API FImpactOnce
{
public:
	bool TryConsume(const void* HitActor, const void* Owner);
	bool IsConsumed() const { return bConsumed; }
private:
	bool bConsumed = false;
};
}
