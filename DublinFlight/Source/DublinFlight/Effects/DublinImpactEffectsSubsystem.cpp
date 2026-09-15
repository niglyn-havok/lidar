#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Effects/DublinImpactEffectType.h"

#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinImpactFX, Log, All);

DublinImpactFX::ERoute DublinImpactFX::Route(const FDublinImpact& Impact)
{
	if (!Impact.IsValid() || !FMath::IsFinite(Impact.PositionCm.SizeSquared())
		|| Impact.Normal.IsNearlyZero() || !FMath::IsFinite(Impact.Normal.SizeSquared())
		|| (Impact.Kind != EDublinImpactKind::Cannon && Impact.Kind != EDublinImpactKind::Bomb))
	{
		return ERoute::Invalid;
	}
	return Impact.bWater ? ERoute::Water
		: Impact.Kind == EDublinImpactKind::Bomb ? ERoute::Bomb : ERoute::Cannon;
}

float DublinImpactFX::VisualRadius(const FDublinImpact& Impact)
{
	return Route(Impact) == ERoute::Invalid ? 0.0f
		: FMath::Clamp(Impact.RadiusCm, 50.0f, Impact.Kind == EDublinImpactKind::Cannon ? 400.0f : 3000.0f);
}

float DublinImpactFX::VisualStrength(const FDublinImpact& Impact)
{
	return Route(Impact) == ERoute::Invalid ? 0.0f : FMath::Clamp(Impact.Strength, 0.1f, 4.0f);
}

bool DublinImpactFX::CanAdmit(int32 ActiveCount)
{
	return ActiveCount >= 0 && ActiveCount < MaxSystems;
}

void DublinImpactFX::PrepareSystem(UNiagaraSystem* System)
{
	if (!System)
	{
		return;
	}
	// Loading the package does not finish Niagara's lazy post-load or start on-demand compilation.
	System->EnsureFullyLoaded();
#if WITH_EDITOR
	if (System->NeedsRequestCompile())
	{
		System->RequestCompile(false);
	}
#endif
}

UDublinImpactEffectsSubsystem::UDublinImpactEffectsSubsystem()
{
	// Hard references on the native CDO retain these packages and their dependencies in cooks.
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> Cannon(TEXT("/Game/FX/NS_CannonImpact.NS_CannonImpact"));
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> Bomb(TEXT("/Game/FX/NS_BombExplosion.NS_BombExplosion"));
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> Water(TEXT("/Game/FX/NS_WaterImpact.NS_WaterImpact"));
	CannonSystem = Cannon.Object;
	BombSystem = Bomb.Object;
	WaterSystem = Water.Object;
#if WITH_EDITOR
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		// The coordinator saves these three assets after the first native build.
		UNiagaraEffectType* EffectType = GetMutableDefault<UDublinImpactEffectType>();
		for (UNiagaraSystem* System : {CannonSystem.Get(), BombSystem.Get(), WaterSystem.Get()})
		{
			if (System && System->GetEffectType() != EffectType)
			{
				System->SetEffectType(EffectType);
				System->MarkPackageDirty();
			}
		}
	}
#endif
}

bool UDublinImpactEffectsSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UDublinImpactEffectsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ActiveImpacts.Reserve(DublinImpactFX::MaxSystems);
	GetWorld()->GetTimerManager().SetTimer(CleanupTimer, this, &UDublinImpactEffectsSubsystem::ReapEffects, 0.2f, true);
	if (!CannonSystem || !BombSystem || !WaterSystem)
	{
		UE_LOG(LogDublinImpactFX, Error, TEXT("Impact effects incomplete: Cannon=%d Bomb=%d Water=%d. Required /Game/FX Niagara packages must be cooked."),
			CannonSystem != nullptr, BombSystem != nullptr, WaterSystem != nullptr);
	}
	for (UNiagaraSystem* System : {CannonSystem.Get(), BombSystem.Get(), WaterSystem.Get()})
	{
		DublinImpactFX::PrepareSystem(System);
		if (System && !System->GetEffectType())
		{
			UE_LOG(LogDublinImpactFX, Error, TEXT("%s has no EffectType. Resave the FX packages after the native build before cooking."), *System->GetPathName());
		}
	}
	if (GetWorld()->GetNetMode() != NM_DedicatedServer)
	{
		CannonPCM = DublinImpactFX::GenerateBoom(DublinImpactFX::ERoute::Cannon);
		BombPCM = DublinImpactFX::GenerateBoom(DublinImpactFX::ERoute::Bomb);
		WaterPCM = DublinImpactFX::GenerateBoom(DublinImpactFX::ERoute::Water);
		for (int32 Index = 0; Index < DublinImpactFX::MaxAudioVoices; ++Index)
		{
			USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this);
			Wave->SetSampleRate(DublinImpactFX::AudioSampleRate);
			Wave->NumChannels = 1;
			Wave->Duration = INDEFINITELY_LOOPING_DURATION;
			Wave->bLooping = false;
			UAudioComponent* Audio = NewObject<UAudioComponent>(GetWorld());
			Audio->bAutoDestroy = false;
			Audio->SetAutoActivate(false);
			Audio->bAllowSpatialization = true;
			Audio->SetSound(Wave);
			Audio->RegisterComponentWithWorld(GetWorld());
			AudioComponents.Add(Audio);
			AudioWaves.Add(Wave);
			AudioDeadlines.Add(0.0);
		}
	}
}

void UDublinImpactEffectsSubsystem::ReapEffects()
{
	const double Now = GetWorld()->GetTimeSeconds();
	for (int32 Index = ActiveImpacts.Num() - 1; Index >= 0; --Index)
	{
		UNiagaraComponent* Component = ActiveImpacts[Index].Component.Get();
		if (!IsValid(Component) || Component->IsComplete() || Now >= ActiveImpacts[Index].Deadline)
		{
			if (IsValid(Component))
			{
				Component->DeactivateImmediate();
				Component->ReleaseToPool();
			}
			ActiveImpacts.RemoveAtSwap(Index);
		}
	}
	for (int32 Index = 0; Index < AudioComponents.Num(); ++Index)
	{
		if (AudioDeadlines[Index] > 0.0 && Now >= AudioDeadlines[Index])
		{
			AudioComponents[Index]->Stop();
			AudioWaves[Index]->ResetAudio();
			AudioDeadlines[Index] = 0.0;
		}
	}
}

void UDublinImpactEffectsSubsystem::PlayImpactAudio(const FDublinImpact& Impact)
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (Impact.Kind == EDublinImpactKind::Cannon && Now < NextCannonSoundTime)
	{
		return;
	}
	const int32 Slot = AudioDeadlines.IndexOfByPredicate([](double Deadline) { return Deadline == 0.0; });
	if (Slot == INDEX_NONE)
	{
		return;
	}
	const TArray<int16>& PCM = Impact.bWater ? WaterPCM : Impact.Kind == EDublinImpactKind::Bomb ? BombPCM : CannonPCM;
	USoundWaveProcedural* Wave = AudioWaves[Slot];
	UAudioComponent* Audio = AudioComponents[Slot];
	Wave->ResetAudio();
	Wave->QueueAudio(reinterpret_cast<const uint8*>(PCM.GetData()), PCM.Num() * sizeof(int16));
	Audio->SetWorldLocation(Impact.PositionCm);
	FSoundAttenuationSettings Attenuation;
	Attenuation.bAttenuate = true;
	Attenuation.bSpatialize = true;
	Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
	Attenuation.AttenuationShapeExtents = FVector(100.0f, 0.0f, 0.0f);
	Attenuation.FalloffDistance = Impact.Kind == EDublinImpactKind::Cannon ? 18000.0f : 120000.0f;
	Audio->AdjustAttenuation(Attenuation);
	Audio->SetVolumeMultiplier(FMath::Clamp(0.18f * FMath::Sqrt(DublinImpactFX::VisualStrength(Impact)), 0.06f, 0.24f));
	Audio->SetPitchMultiplier(0.95f + 0.1f * static_cast<float>(static_cast<uint32>(Impact.Seed) % 17) / 16.0f);
	Audio->Play();
	AudioDeadlines[Slot] = Now + static_cast<double>(PCM.Num()) / (DublinImpactFX::AudioSampleRate * 0.95) + 0.1;
	if (Impact.Kind == EDublinImpactKind::Cannon)
	{
		NextCannonSoundTime = Now + 0.07;
	}
}

void UDublinImpactEffectsSubsystem::EmitImpact(const FDublinImpact& Impact)
{
	const DublinImpactFX::ERoute Route = DublinImpactFX::Route(Impact);
	if (Route == DublinImpactFX::ERoute::Invalid)
	{
		UE_LOG(LogDublinImpactFX, Warning, TEXT("Rejected non-finite, degenerate or invalid impact payload."));
		return;
	}
	if (!GetWorld() || GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	UNiagaraSystem* System = Route == DublinImpactFX::ERoute::Water ? WaterSystem.Get()
		: Route == DublinImpactFX::ERoute::Bomb ? BombSystem.Get() : CannonSystem.Get();
#if WITH_EDITOR
	if (System && System->HasOutstandingCompilationRequests(true))
	{
		// A cold editor may still be compiling the systems prepared at world initialization.
		System->PollForCompilationComplete();
		return;
	}
#endif
	if (!System || !System->IsValid())
	{
		UE_LOG(LogDublinImpactFX, Error, TEXT("Cannot emit impact: missing or invalid Niagara system (route=%d)."), static_cast<int32>(Route));
		return;
	}

	FVector Listener = Impact.PositionCm;
	FRotator ViewRotation;
	if (APlayerController* Player = GetWorld()->GetFirstPlayerController())
	{
		Player->GetPlayerViewPoint(Listener, ViewRotation);
	}
	const float Radius = DublinImpactFX::VisualRadius(Impact);
	const double DistanceSquared = FVector::DistSquared(Listener, Impact.PositionCm);
	const float CullDistance = Impact.Kind == EDublinImpactKind::Cannon ? 35000.0f : 180000.0f;
	if (DistanceSquared > FMath::Square(CullDistance))
	{
		return;
	}
	ReapEffects();
	PlayImpactAudio(Impact);
	const float Significance = static_cast<float>(Radius / FMath::Max(100.0, FMath::Sqrt(DistanceSquared)));
	if (!DublinImpactFX::CanAdmit(ActiveImpacts.Num()))
	{
		int32 LeastSignificant = 0;
		for (int32 Index = 1; Index < ActiveImpacts.Num(); ++Index)
		{
			if (ActiveImpacts[Index].Significance < ActiveImpacts[LeastSignificant].Significance)
			{
				LeastSignificant = Index;
			}
		}
		if (Significance <= ActiveImpacts[LeastSignificant].Significance)
		{
			return;
		}
		if (UNiagaraComponent* Old = ActiveImpacts[LeastSignificant].Component.Get())
		{
			Old->DeactivateImmediate();
			Old->ReleaseToPool();
		}
		ActiveImpacts.RemoveAtSwap(LeastSignificant);
	}

	const FVector Normal = Impact.Normal.GetSafeNormal();
	UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		GetWorld(), System, Impact.PositionCm + Normal * 8.0f, FQuat::FindBetweenNormals(FVector::UpVector, Normal).Rotator(),
		FVector::OneVector, false, false, ENCPoolMethod::ManualRelease, true);
	if (!Component)
	{
		UE_LOG(LogDublinImpactFX, Verbose, TEXT("Impact instance not spawned (Niagara scalability/pre-cull)."));
		return;
	}
	Component->SetVariableFloat(TEXT("User.ImpactRadius"), Radius);
	Component->SetVariableFloat(TEXT("User.ImpactStrength"), DublinImpactFX::VisualStrength(Impact));
	Component->SetVariableVec3(TEXT("User.SurfaceNormal"), Normal);
	Component->SetRandomSeedOffset(Impact.Seed);
	Component->SetSystemFixedBounds(FBox(FVector(-Radius * 5.0f, -Radius * 5.0f, -Radius), FVector(Radius * 5.0f, Radius * 5.0f, Radius * 9.0f)));
	Component->SetCastShadow(false);
	Component->Activate(true);
	FDublinActiveImpactEffect& Entry = ActiveImpacts.AddDefaulted_GetRef();
	Entry.Component = Component;
	Entry.Significance = Significance;
	Entry.Deadline = GetWorld()->GetTimeSeconds() + (Impact.Kind == EDublinImpactKind::Cannon ? 3.0 : 9.0);
}

void UDublinImpactEffectsSubsystem::Deinitialize()
{
	GetWorld()->GetTimerManager().ClearTimer(CleanupTimer);
	for (const FDublinActiveImpactEffect& Entry : ActiveImpacts)
	{
		if (UNiagaraComponent* Component = Entry.Component.Get())
		{
			Component->DeactivateImmediate();
			Component->ReleaseToPool();
		}
	}
	ActiveImpacts.Empty();
	for (UAudioComponent* Audio : AudioComponents)
	{
		if (Audio)
		{
			Audio->Stop();
			Audio->DestroyComponent();
		}
	}
	AudioComponents.Empty();
	AudioWaves.Empty();
	AudioDeadlines.Empty();
	Super::Deinitialize();
}
