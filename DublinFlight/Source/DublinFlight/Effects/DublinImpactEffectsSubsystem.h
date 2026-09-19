#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DublinImpact.h"
#include "Effects/DublinMaximumImpact.h"
#include "DublinImpactEffectsSubsystem.generated.h"

class UNiagaraSystem;
class UNiagaraComponent;
class UAudioComponent;
class USoundWaveProcedural;
struct FDublinActiveImpactEffect;

namespace DublinImpactFX
{
	constexpr int32 MaxSystems = 16;
	constexpr int32 MaxSmokeColumns = 4;
	constexpr float SmokeParticleLifetimeSeconds = 8.0f;
	constexpr float MaxSmokeLifetimeSeconds = 55.0f;
	constexpr int32 MaxAudioVoices = 6;
	constexpr int32 AudioSampleRate = 24000;
	enum class ERoute : uint8 { Invalid, Cannon, Bomb, Water };
	struct FSmokeColumn
	{
		float EmissionSeconds = 0.0f;
		float RadiusCm = 0.0f;
		FVector VelocityCmPerSecond = FVector::ZeroVector;
		bool IsEnabled() const { return EmissionSeconds > 0.0f; }
	};

	// NS_BombExplosion.SimpleSpriteBurst asset contract (the parent owns asset edits):
	// Add float User.SmokeEmissionSeconds, User.SmokeParticleLifetime, User.SmokeRadius
	// and vec3 User.SmokeVelocity, all defaulting to zero. Zero emission keeps the old burst.
	// Keep the initial 36 particles; add 6/sec only while Emitter.Age < SmokeEmissionSeconds.
	// For columns: lifetime = SmokeParticleLifetime; radius = SmokeRadius; initial velocity
	// = SmokeVelocity; GravityForce = SmokeVelocity * 0.4; drag = 0.4; curl <= radius * 0.12.
	// Radius must also replace ImpactRadius in ShapeLocation and the CURRENT update override:
	// SetVariables_28DB0063473A2BBA9BF9978359415CE2 / Particles.SpriteSize (not just InitializeParticle).
	// Write initial SmokeVelocity after AddVelocity; retain all old expressions when emission is zero.
	// Retain cloud sprite growth/fade, with fixed allocation 96 (36 + 6 * 8 <= 96).
	// System and smoke emitter: Once, duration max(1, SmokeEmissionSeconds), inactive Complete.
	// Other emitters MUST remain Self/Once/1s, not inherit the extended system duration or loop.
	// SmokeVelocity is component-local so wall hits still rise/drift in world space.
	// Exposing parameters alone is insufficient: bind them in the actual spawn/update stacks.
	ERoute Route(const FDublinImpact& Impact);
	float VisualRadius(const FDublinImpact& Impact);
	float VisualStrength(const FDublinImpact& Impact);
	FSmokeColumn MakeSmokeColumn(const FDublinImpact& Impact, int32 ActiveColumns);
	int32 CountSmokeColumns(TConstArrayView<FDublinActiveImpactEffect> ActiveImpacts);
	int32 CountMaximumPresentations(TConstArrayView<FDublinActiveImpactEffect> ActiveImpacts);
	float EffectLifetime(const FDublinImpact& Impact, const FSmokeColumn& Smoke);
	FBox EffectBounds(const FDublinImpact& Impact, const FSmokeColumn& Smoke);
	void SetSmokeParameters(UNiagaraComponent& Component, const FSmokeColumn& Smoke, const FQuat& Rotation);
	bool ValidateSmokeColumnParameters(const UNiagaraSystem* System, FString& Failure);
	bool ValidateSmokeColumnAsset(const UNiagaraSystem* System, FString& Failure);
	bool ValidateSpriteMaterials(const UNiagaraSystem* System, FString& Failure);
	bool CanAdmit(int32 ActiveCount);
	void PrepareSystem(UNiagaraSystem* System);
	TArray<int16> GenerateBoom(ERoute Route);
}

USTRUCT()
struct FDublinActiveImpactEffect
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Component;
	double Deadline = 0.0;
	float Significance = 0.0f;
	bool bSmokeColumn = false;
	bool bMaximumPresentation = false;
	FTimerHandle StopTimer;

	bool ShouldRelease(double Now, bool bComplete) const;
};

UCLASS()
class DUBLINFLIGHT_API UDublinImpactEffectsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UDublinImpactEffectsSubsystem();
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	void EmitImpact(const FDublinImpact& Impact);
	void EmitImpact(const FDublinImpact& Impact, TConstArrayView<FDublinConfirmedDamageSample> ConfirmedDamageSamples);

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FDublinMaximumLifecycleTest;
#endif
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> CannonSystem;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> BombSystem;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> WaterSystem;
	UPROPERTY()
	TSoftObjectPtr<UNiagaraSystem> MaximumSystemAsset;
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> MaximumSystem;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> AudioComponents;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundWaveProcedural>> AudioWaves;

	UPROPERTY(Transient)
	TArray<FDublinActiveImpactEffect> ActiveImpacts;
	TArray<double> AudioDeadlines;
	TArray<int16> CannonPCM;
	TArray<int16> BombPCM;
	TArray<int16> WaterPCM;
	double NextCannonSoundTime = 0.0;
	bool bSmokeColumnContractReady = false;
	bool bReportedSmokeColumnFailure = false;
	FString SmokeColumnFailure;
	bool bMaximumContractReady = false;
	bool bReportedMaximumFailure = false;
	FString MaximumFailure;
	FTimerHandle CleanupTimer;
	void ReapEffects();
	void PlayImpactAudio(const FDublinImpact& Impact);
	void ReleaseImpact(int32 Index);
	void StopMaximumPresentation(UNiagaraComponent* Component);
	bool TryEmitMaximum(const FDublinImpact& Impact, const DublinImpactFX::FMaximumPresentation& Presentation, float Significance);
	void EmitSmokeOnly(const FDublinImpact& Impact, float Significance);
};
