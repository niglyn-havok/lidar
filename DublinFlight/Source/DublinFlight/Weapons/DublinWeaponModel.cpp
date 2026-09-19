#include "Weapons/DublinWeaponModel.h"

namespace DublinWeapons
{
float ClampYield(float Yield)
{
	return FMath::IsFinite(Yield) ? FMath::Clamp(Yield, 0.1f, 1000.0f) : 1.0f;
}

FDublinImpact MakeImpact(EDublinImpactKind Kind, float Yield, const FBombCurve& Curve)
{
	FDublinImpact Impact;
	Impact.Kind = Kind;
	if (Kind == EDublinImpactKind::Cannon)
	{
		return Impact;
	}
	const auto Safe = [](float Value, float Fallback, float Minimum, float Maximum)
	{
		return FMath::Clamp(FMath::IsFinite(Value) ? Value : Fallback, Minimum, Maximum);
	};
	Impact.YieldTonsTNT = ClampYield(Yield);
	const float Growth = FMath::Pow(Impact.YieldTonsTNT, Safe(Curve.Exponent, 0.25f, 0.1f, 0.5f));
	const float RadiusGrowth = FMath::Pow(Impact.YieldTonsTNT,
		Impact.YieldTonsTNT < 1.0f ? 0.25f : BombRadiusExponent);
	Impact.RadiusCm = FMath::Clamp(Safe(Curve.RadiusAtOneCm, 600.0f, 100.0f, 5000.0f) * RadiusGrowth,
		100.0f, Safe(Curve.MaxRadiusCm, MaximumBombRadiusCm, 100.0f, MaximumBombRadiusCm));
	Impact.CraterDepthCm = FMath::Clamp(Safe(Curve.DepthAtOneCm, 250.0f, 10.0f, 2000.0f) * Growth,
		10.0f, Safe(Curve.MaxDepthCm, 2000.0f, 10.0f, 2000.0f));
	Impact.Strength = FMath::Clamp(Safe(Curve.StrengthAtOne, 2.0f, 0.1f, 50.0f) * Growth,
		0.1f, Safe(Curve.MaxStrength, 50.0f, 0.1f, 50.0f));
	return Impact;
}

FDublinImpact MakeGroundImpact(const FDublinImpact& Impact)
{
	FDublinImpact Ground = Impact;
	if (Impact.Kind == EDublinImpactKind::Bomb)
	{
		// Keep the pre-spectacle terrain work footprint; building/FX radius is independent.
		Ground.RadiusCm = FMath::Min(Impact.RadiusCm, 600.0f * FMath::Pow(ClampYield(Impact.YieldTonsTNT), 0.25f));
	}
	return Ground;
}

FVector BallisticPosition(const FVector& Start, const FVector& InheritedVelocity, double GravityZ, double Seconds)
{
	if (Start.ContainsNaN() || InheritedVelocity.ContainsNaN()
		|| !FMath::IsFinite(GravityZ) || !FMath::IsFinite(Seconds) || Seconds < 0.0 || Seconds > 30.0)
	{
		return FVector::ZeroVector;
	}
	return Start + InheritedVelocity * Seconds + FVector(0.0, 0.0, 0.5 * GravityZ * Seconds * Seconds);
}

bool FindWaterCrossing(const FVector& Start, const FVector& End,
	TFunctionRef<bool(const FVector&, float&)> QuerySurface, FVector& OutPosition)
{
	if (Start.ContainsNaN() || End.ContainsNaN() || Start.GetAbsMax() > 1.0e8 || End.GetAbsMax() > 1.0e8
		|| End.Z >= Start.Z)
	{
		return false;
	}
	const int32 Steps = FMath::Clamp(FMath::CeilToInt(FVector::Distance(Start, End) / 100.0), 1, 512);
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		const FVector A = FMath::Lerp(Start, End, static_cast<double>(Index) / Steps);
		const FVector B = FMath::Lerp(Start, End, static_cast<double>(Index + 1) / Steps);
		float SurfaceZ = 0.0f;
		if (!(QuerySurface((A + B) * 0.5, SurfaceZ) || QuerySurface(B, SurfaceZ) || QuerySurface(A, SurfaceZ))
			|| !FMath::IsFinite(SurfaceZ))
		{
			continue;
		}
		if (A.Z >= SurfaceZ && B.Z <= SurfaceZ && B.Z < A.Z)
		{
			const double Alpha = (A.Z - SurfaceZ) / (A.Z - B.Z);
			FVector Crossing = FMath::Lerp(A, B, Alpha);
			float ConfirmedZ = 0.0f;
			if (QuerySurface(Crossing, ConfirmedZ) && FMath::IsFinite(ConfirmedZ)
				&& FMath::Abs(ConfirmedZ - SurfaceZ) < 1.0f)
			{
				Crossing.Z = ConfirmedZ;
				OutPosition = Crossing;
				return true;
			}
		}
	}
	return false;
}

bool IsWeaponKey(const FKey& Key)
{
	return Key == EKeys::LeftMouseButton || Key == EKeys::RightMouseButton || Key == EKeys::B
		|| Key == EKeys::LeftBracket || Key == EKeys::RightBracket;
}

bool FCooldown::TryConsume(double Now, double Interval)
{
	if (!FMath::IsFinite(Now) || !FMath::IsFinite(Interval) || Now < 0.0 || Interval <= 0.0 || Now < NextAllowed)
	{
		return false;
	}
	NextAllowed = Now + FMath::Clamp(Interval, 0.05, 60.0);
	return true;
}

bool FImpactOnce::TryConsume(const void* HitActor, const void* Owner)
{
	if (bConsumed || (Owner && HitActor == Owner))
	{
		return false;
	}
	bConsumed = true;
	return true;
}
}
