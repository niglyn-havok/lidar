#pragma once

#include "CoreMinimal.h"
#include "DublinImpact.h"

class UNiagaraComponent;
class UNiagaraSystem;

// The caller owns provenance: supply only surfaces from buildings actually activated/damaged
// by this accepted impact, within each building's validated SourceBounds. This is a read-only
// snapshot, not a request to damage or relocate those surfaces.
struct FDublinConfirmedDamageSample
{
	FVector PositionCm = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
};

namespace DublinImpactFX
{
	inline constexpr TCHAR MaximumSystemPath[] = TEXT("/Game/FX/NS_MaxBombExplosion.NS_MaxBombExplosion");
	constexpr int32 MaxMaximumPresentations = 2;
	constexpr int32 MaxDamageSamples = 32;
	constexpr float MaximumTierYield = 1000.0f;
	constexpr float MaximumApprovedRadiusCm = 12000.0f;
	constexpr double MaximumDamageSampleAbsZCm = 200000.0;
	constexpr float MaximumTransientSeconds = 12.0f;
	constexpr float MaximumLifetimeGuardSeconds = 16.0f;
	constexpr float MaximumAccentSeconds = 0.35f;
	constexpr float MaximumDustOpacity = 0.22f;
	constexpr float MaximumPlumeOpacity = 0.18f;
	constexpr int32 MaximumMeshParticles = 256;
	constexpr int32 MaximumFleckParticles = 2048;
	constexpr int32 MaximumDustParticles = 512;
	constexpr int32 MaximumPlumeParticles = 1024;
	constexpr int32 MaximumAccentParticles = 64;
	constexpr int32 MaximumParticleBudget = MaximumMeshParticles + MaximumFleckParticles
		+ MaximumDustParticles + MaximumPlumeParticles + MaximumAccentParticles;

	struct FMaximumPresentation
	{
		float RadiusCm = 0.0f;
		float DebrisTravelCm = 0.0f;
		float DustRadiusCm = 0.0f;
		float PlumeRadiusCm = 0.0f;
		float PlumeHeightCm = 0.0f;
		float SpriteRadiusCm = 0.0f;
		TArray<FVector> DamageOffsets;
		TArray<FVector> DamageNormals;
		int32 DroppedSamples = 0;

		bool IsEnabled() const { return RadiusCm > 0.0f; }
		bool HasStructuralSources() const { return !DamageOffsets.IsEmpty(); }
		int32 ParticleBudget() const;
		FBox Bounds() const;
	};

	// Proposed parent integration: call EmitImpact once, after accepted damage/activation,
	// with at most 32 samples. Do not call it again per fragment or to attach late samples.
	// Sample admission uses the approved XY footprint and absolute world Z +/-200000cm,
	// not a 3D blast sphere. Real roof/facade heights are preserved in offsets and bounds.
	// Empty samples intentionally disable chips, flecks and the structural dust front.
	bool WantsMaximumPresentation(const FDublinImpact& Impact);
	bool CanAdmitMaximum(int32 ActiveMaximum);
	FMaximumPresentation MakeMaximumPresentation(const FDublinImpact& Impact,
		TConstArrayView<FDublinConfirmedDamageSample> Samples);

	// Authored NS_MaxBombExplosion contract:
	// Five local-space GPU emitters, fixed allocations and deterministic seed base zero:
	// MasonryChips=256, DebrisFlecks=2048, DustFront=512, TransientPlume=1024, BriefAccents=64.
	// All particles stop by User.TransientSeconds (12); native timer kills the component then,
	// with a separate 16s cleanup failsafe. No persistent smoke in this expensive system.
	// An optional ordinary-system smoke-only component uses another of the SAME 16 slots,
	// one of the SAME four column slots and at most 96 particles, separately from the 3904.
	// User.ImpactRadius is the approved payload radius, NOT a radius reconstructed from yield.
	// User.DamageOffsets/Normals are float3 array DIs, maxElements=32, relative to the upright
	// component's impact origin. Structural emitters must sample only these confirmed sources.
	// Debris travel and dust patch radius are bounded per source; particles leaving that
	// envelope must die. SpriteRadius bounds half-size; plume height/radius bound its motion.
	// Grey/brown masonry dust, not a sustained fireball: accents end at User.AccentSeconds,
	// and User.DustOpacity/PlumeOpacity cap per-particle alpha. Sparse spatial coverage still
	// needs rendered verification: per-particle alpha alone cannot bound overlapping opacity.
	void SetMaximumParameters(UNiagaraComponent& Component, const FDublinImpact& Impact,
		const FMaximumPresentation& Presentation);
	void SetBombEmitterMask(UNiagaraComponent& Component, bool bSmokeOnly);
	bool ValidateMaximumAsset(const UNiagaraSystem* System, FString& Failure);
}
