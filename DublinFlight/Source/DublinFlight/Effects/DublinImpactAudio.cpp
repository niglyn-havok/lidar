#include "Effects/DublinImpactEffectsSubsystem.h"

TArray<int16> DublinImpactFX::GenerateBoom(ERoute Route)
{
	if (Route == ERoute::Invalid)
	{
		return {};
	}
	const bool bCannon = Route == ERoute::Cannon;
	const bool bWater = Route == ERoute::Water;
	const float Duration = bCannon ? 0.45f : bWater ? 1.5f : 1.8f;
	const int32 Count = FMath::RoundToInt(Duration * AudioSampleRate);
	TArray<int16> PCM;
	PCM.SetNumUninitialized(Count);
	FRandomStream Random(bCannon ? 7301 : bWater ? 8303 : 9307);
	float LowNoise = 0.0f;
	float Phase = 0.0f;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Time = static_cast<float>(Index) / AudioSampleRate;
		const float Noise = Random.FRandRange(-1.0f, 1.0f);
		LowNoise += (Noise - LowNoise) * (bCannon ? 0.16f : 0.055f);
		const float Frequency = (bCannon ? 125.0f : bWater ? 72.0f : 52.0f) / (1.0f + Time * 2.5f);
		Phase += UE_TWO_PI * Frequency / AudioSampleRate;
		const float Attack = FMath::Min(1.0f, Time / 0.004f);
		const float Tail = FMath::Clamp((Duration - Time) / 0.1f, 0.0f, 1.0f);
		const float Body = FMath::Exp(-Time * (bCannon ? 12.0f : 3.5f));
		const float Crack = Noise * FMath::Exp(-Time * (bWater ? 9.0f : 75.0f));
		const float Rumble = FMath::Sin(Phase) * Body * (bWater ? 0.13f : 0.34f);
		const float Spray = bWater ? Noise * 0.16f * FMath::Exp(-Time * 2.8f) : 0.0f;
		const float Sample = Attack * Tail * (Rumble + LowNoise * Body * 0.8f + Crack * 0.24f + Spray);
		PCM[Index] = static_cast<int16>(FMath::Clamp(Sample, -0.65f, 0.65f) * 32767.0f);
	}
	PCM.Last() = 0;
	return PCM;
}
