#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DublinImpact.h"
#include "DublinImpactEffectsSubsystem.generated.h"

class UNiagaraSystem;
class UNiagaraComponent;
class UAudioComponent;
class USoundWaveProcedural;

namespace DublinImpactFX
{
	constexpr int32 MaxSystems = 16;
	constexpr int32 MaxAudioVoices = 6;
	constexpr int32 AudioSampleRate = 24000;
	enum class ERoute : uint8 { Invalid, Cannon, Bomb, Water };
	ERoute Route(const FDublinImpact& Impact);
	float VisualRadius(const FDublinImpact& Impact);
	float VisualStrength(const FDublinImpact& Impact);
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

private:
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> CannonSystem;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> BombSystem;
	UPROPERTY()
	TObjectPtr<UNiagaraSystem> WaterSystem;
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
	FTimerHandle CleanupTimer;
	void ReapEffects();
	void PlayImpactAudio(const FDublinImpact& Impact);
};
