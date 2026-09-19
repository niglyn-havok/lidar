#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Effects/DublinImpactEffectType.h"

#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
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
	if (Route(Impact) == ERoute::Invalid) { return 0.0f; }
	const float Radius = FMath::Clamp(Impact.RadiusCm, 50.0f, Impact.Kind == EDublinImpactKind::Cannon ? 400.0f : 3000.0f);
	// Water's authored HotCore, droplets, mist and ring all consume this same footprint.
	return Impact.bWater && Impact.Kind == EDublinImpactKind::Cannon ? FMath::Min(Radius * 2.0f, 400.0f) : Radius;
}

float DublinImpactFX::VisualStrength(const FDublinImpact& Impact)
{
	if (Route(Impact) == ERoute::Invalid) { return 0.0f; }
	const float Strength = FMath::Clamp(Impact.Strength, 0.1f, 4.0f);
	return Impact.bWater && Impact.Kind == EDublinImpactKind::Cannon ? FMath::Min(Strength * 3.0f, 4.0f) : Strength;
}

DublinImpactFX::FSmokeColumn DublinImpactFX::MakeSmokeColumn(const FDublinImpact& Impact, int32 ActiveColumns)
{
	FSmokeColumn Smoke;
	if (Route(Impact) != ERoute::Bomb || ActiveColumns < 0 || ActiveColumns >= MaxSmokeColumns)
	{
		return Smoke;
	}
	const float Yield = FMath::Clamp(Impact.YieldTonsTNT, 0.125f, 1000.0f);
	const float Growth = FMath::Pow(Yield, 1.0f / 3.0f);
	const float Lifetime = FMath::Clamp(45.0f + 2.0f * FMath::Log2(FMath::Max(Yield, 1.0f)), 45.0f, MaxSmokeLifetimeSeconds);
	Smoke.EmissionSeconds = Lifetime - SmokeParticleLifetimeSeconds;
	Smoke.RadiusCm = FMath::Clamp(VisualRadius(Impact) * 0.65f, 180.0f, 1200.0f);
	FRandomStream Random(Impact.Seed);
	const float Heading = Random.FRandRange(-PI, PI);
	const float Drift = Random.FRandRange(60.0f, 110.0f);
	Smoke.VelocityCmPerSecond = FVector(FMath::Cos(Heading) * Drift, FMath::Sin(Heading) * Drift,
		FMath::Clamp(180.0f + 60.0f * Growth, 210.0f, 540.0f));
	return Smoke;
}

int32 DublinImpactFX::CountSmokeColumns(TConstArrayView<FDublinActiveImpactEffect> ActiveImpacts)
{
	int32 Count = 0;
	for (const FDublinActiveImpactEffect& Active : ActiveImpacts)
	{
		Count += Active.bSmokeColumn ? 1 : 0;
	}
	return Count;
}

int32 DublinImpactFX::CountMaximumPresentations(TConstArrayView<FDublinActiveImpactEffect> ActiveImpacts)
{
	int32 Count = 0;
	for (const FDublinActiveImpactEffect& Active : ActiveImpacts)
	{
		Count += Active.bMaximumPresentation ? 1 : 0;
	}
	return Count;
}

float DublinImpactFX::EffectLifetime(const FDublinImpact& Impact, const FSmokeColumn& Smoke)
{
	if (Route(Impact) == ERoute::Invalid) { return 0.0f; }
	if (Route(Impact) == ERoute::Bomb && Smoke.IsEnabled())
	{
		return FMath::Min(Smoke.EmissionSeconds + SmokeParticleLifetimeSeconds, MaxSmokeLifetimeSeconds);
	}
	// NS_WaterImpact's mist lives 4.3s: the old cannon deadline killed it at 3s.
	return Impact.Kind == EDublinImpactKind::Bomb ? 9.0f : Impact.bWater ? 5.0f : 3.0f;
}

FBox DublinImpactFX::EffectBounds(const FDublinImpact& Impact, const FSmokeColumn& Smoke)
{
	if (Route(Impact) == ERoute::Invalid) { return FBox(ForceInit); }
	const float Radius = VisualRadius(Impact);
	FBox Bounds(FVector(-Radius * 5.0f, -Radius * 5.0f, -Radius), FVector(Radius * 5.0f, Radius * 5.0f, Radius * 9.0f));
	if (Impact.bWater)
	{
		// Conservative envelope for the authored 2.8s jets/droplets, even before drag.
		const float Travel = Radius * 1.9f * FMath::Sqrt(VisualStrength(Impact)) * 2.8f;
		Bounds += FVector(0.0f, 0.0f, Travel + Radius);
		Bounds += FVector(0.0f, 0.0f, -0.5f * 980.0f * 2.8f * 2.8f - Radius);
	}
	if (Route(Impact) == ERoute::Bomb && Smoke.IsEnabled())
	{
		const FVector Padding(Smoke.RadiusCm * 4.0f);
		FBox SmokeBounds(-Padding, Padding);
		const FVector Travel = Smoke.VelocityCmPerSecond * SmokeParticleLifetimeSeconds;
		SmokeBounds += Travel - Padding;
		SmokeBounds += Travel + Padding;
		const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Impact.Normal.GetSafeNormal());
		Bounds += SmokeBounds.TransformBy(FTransform(Rotation.Inverse()));
	}
	return Bounds;
}

void DublinImpactFX::SetSmokeParameters(UNiagaraComponent& Component, const FSmokeColumn& Smoke, const FQuat& Rotation)
{
	// Always reset all overrides, including on a pooled bomb denied a column slot.
	Component.SetVariableFloat(TEXT("User.SmokeEmissionSeconds"), Smoke.EmissionSeconds);
	Component.SetVariableFloat(TEXT("User.SmokeParticleLifetime"), Smoke.IsEnabled() ? SmokeParticleLifetimeSeconds : 0.0f);
	Component.SetVariableFloat(TEXT("User.SmokeRadius"), Smoke.RadiusCm);
	Component.SetVariableVec3(TEXT("User.SmokeVelocity"), Rotation.UnrotateVector(Smoke.VelocityCmPerSecond));
}

bool DublinImpactFX::ValidateSmokeColumnParameters(const UNiagaraSystem* System, FString& Failure)
{
	Failure.Reset();
	if (!System)
	{
		Failure = TEXT("Missing NS_BombExplosion.");
		return false;
	}
	const FNiagaraUserRedirectionParameterStore& Parameters = System->GetExposedParameters();
	for (const FNiagaraVariable& Variable : {
		FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SmokeEmissionSeconds")),
		FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SmokeParticleLifetime")),
		FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SmokeRadius")),
		FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("User.SmokeVelocity"))})
	{
		if (Parameters.IndexOf(Variable) == INDEX_NONE)
		{
			Failure = FString::Printf(TEXT("%s is missing correctly typed %s. Bind the SimpleSpriteBurst column contract and resave before cooking."),
				*System->GetPathName(), *Variable.GetName().ToString());
			return false;
		}
	}
	return true;
}

bool DublinImpactFX::ValidateSpriteMaterials(const UNiagaraSystem* System, FString& Failure)
{
	Failure.Reset();
	if (!System)
	{
		Failure = TEXT("Missing Niagara system; no sprite materials available.");
		return false;
	}
	int32 SpriteCount = 0;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (!Handle.GetIsEnabled()) { continue; }
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data) { continue; }
		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			const UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer);
			if (!Sprite || !Sprite->GetIsEnabled()) { continue; }
			++SpriteCount;
			const UMaterial* Material = Sprite->Material ? Sprite->Material->GetMaterial() : nullptr;
			if (!Material || Material->IsDefaultMaterial() || !Material->GetUsageByFlag(MATUSAGE_NiagaraSprites))
			{
				Failure = FString::Printf(TEXT("%s/%s has a missing/default/non-Niagara sprite material (%s); renderer fallback is not a valid impact effect."),
					*System->GetPathName(), *Handle.GetName().ToString(), *GetPathNameSafe(Sprite->Material.Get()));
				return false;
			}
		}
	}
	if (SpriteCount == 0)
	{
		Failure = FString::Printf(TEXT("%s has no enabled sprite renderers."), *System->GetPathName());
		return false;
	}
	return true;
}

bool DublinImpactFX::ValidateSmokeColumnAsset(const UNiagaraSystem* System, FString& Failure)
{
	if (!ValidateSmokeColumnParameters(System, Failure) || !ValidateSpriteMaterials(System, Failure))
	{
		return false;
	}
	const FNiagaraEmitterHandle* Handle = System->GetEmitterHandles().FindByPredicate(
		[](const FNiagaraEmitterHandle& Emitter) { return Emitter.GetName() == FName(TEXT("SimpleSpriteBurst")); });
	const FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
	if (!Data || !Handle->GetIsEnabled() || !Data->bLocalSpace
		|| Data->AllocationMode != EParticleAllocationMode::FixedCount
		|| Data->PreAllocationCount < 84 || Data->PreAllocationCount > 96)
	{
		Failure = FString::Printf(TEXT("%s requires enabled local-space SimpleSpriteBurst with fixed allocation 84-96 for the sustained column."),
			*System->GetPathName());
		return false;
	}
	bool bHasSmokeSprites = false;
	for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
	{
		bHasSmokeSprites |= IsValid(Renderer) && Renderer->GetIsEnabled() && Renderer->IsA<UNiagaraSpriteRendererProperties>();
	}
	if (!bHasSmokeSprites)
	{
		Failure = FString::Printf(TEXT("%s/SimpleSpriteBurst has no enabled cloud sprite renderer."), *System->GetPathName());
		return false;
	}
	return true;
}

bool FDublinActiveImpactEffect::ShouldRelease(double Now, bool bComplete) const
{
	return bComplete || !FMath::IsFinite(Now) || !FMath::IsFinite(Deadline) || Now >= Deadline;
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
	MaximumSystemAsset = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(DublinImpactFX::MaximumSystemPath));
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
		FString MaterialFailure;
		if (!DublinImpactFX::ValidateSpriteMaterials(System, MaterialFailure))
		{
			UE_LOG(LogDublinImpactFX, Error, TEXT("%s"), *MaterialFailure);
		}
	}
	bSmokeColumnContractReady = DublinImpactFX::ValidateSmokeColumnAsset(BombSystem, SmokeColumnFailure);
	// The source checkpoint can precede asset authoring. Missing maximum content is reported
	// explicitly on a maximum-tier request, without failing ordinary-world initialization.
	if (FPackageName::DoesPackageExist(MaximumSystemAsset.ToSoftObjectPath().GetLongPackageName()))
	{
		MaximumSystem = MaximumSystemAsset.LoadSynchronous();
		DublinImpactFX::PrepareSystem(MaximumSystem);
	}
	bMaximumContractReady = DublinImpactFX::ValidateMaximumAsset(MaximumSystem, MaximumFailure);
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
		if (!IsValid(Component) || ActiveImpacts[Index].ShouldRelease(Now, Component->IsComplete()))
		{
			ReleaseImpact(Index);
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

void UDublinImpactEffectsSubsystem::ReleaseImpact(int32 Index)
{
	check(ActiveImpacts.IsValidIndex(Index));
	GetWorld()->GetTimerManager().ClearTimer(ActiveImpacts[Index].StopTimer);
	if (UNiagaraComponent* Component = ActiveImpacts[Index].Component.Get(); IsValid(Component))
	{
		Component->DeactivateImmediate();
		Component->ReleaseToPool();
	}
	ActiveImpacts.RemoveAtSwap(Index);
}

void UDublinImpactEffectsSubsystem::StopMaximumPresentation(UNiagaraComponent* Component)
{
	for (int32 Index = 0; Index < ActiveImpacts.Num(); ++Index)
	{
		if (ActiveImpacts[Index].bMaximumPresentation && ActiveImpacts[Index].Component == Component)
		{
			ReleaseImpact(Index);
			return;
		}
	}
}

void UDublinImpactEffectsSubsystem::PlayImpactAudio(const FDublinImpact& Impact)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_ImpactAudio);
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
	Audio->SetVolumeMultiplier(FMath::Clamp(0.18f * FMath::Sqrt(FMath::Clamp(Impact.Strength, 0.1f, 4.0f)), 0.06f, 0.24f));
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
	EmitImpact(Impact, TConstArrayView<FDublinConfirmedDamageSample>());
}

void UDublinImpactEffectsSubsystem::EmitImpact(const FDublinImpact& Impact,
	TConstArrayView<FDublinConfirmedDamageSample> ConfirmedDamageSamples)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_EmitImpactFX);
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
	// Visual water amplification must not change the existing impact-admission ordering.
	const float AdmissionRadius = FMath::Clamp(Impact.RadiusCm, 50.0f, Impact.Kind == EDublinImpactKind::Cannon ? 400.0f : 3000.0f);
	const float Significance = static_cast<float>(AdmissionRadius / FMath::Max(100.0, FMath::Sqrt(DistanceSquared)));
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
		ReleaseImpact(LeastSignificant);
	}

	const bool bWantsMaximum = DublinImpactFX::WantsMaximumPresentation(Impact);
	const DublinImpactFX::FMaximumPresentation Maximum = DublinImpactFX::MakeMaximumPresentation(Impact, ConfirmedDamageSamples);
	if (Maximum.DroppedSamples > 0)
	{
		UE_LOG(LogDublinImpactFX, Warning, TEXT("Ignored %d excess, invalid or out-of-XY/Z-bounds confirmed damage samples; maximum is %d."),
			Maximum.DroppedSamples, DublinImpactFX::MaxDamageSamples);
	}
	if (bWantsMaximum && TryEmitMaximum(Impact, Maximum, Significance))
	{
		return;
	}

	DublinImpactFX::FSmokeColumn Smoke;
	if (Route == DublinImpactFX::ERoute::Bomb)
	{
		if (bSmokeColumnContractReady)
		{
			Smoke = DublinImpactFX::MakeSmokeColumn(Impact, DublinImpactFX::CountSmokeColumns(ActiveImpacts));
		}
		else if (!bReportedSmokeColumnFailure)
		{
			UE_LOG(LogDublinImpactFX, Error, TEXT("Sustained bomb smoke unavailable; retaining the original short burst only. %s"), *SmokeColumnFailure);
			bReportedSmokeColumnFailure = true;
		}
	}
	const FVector Normal = Impact.bWater ? FVector::UpVector : Impact.Normal.GetSafeNormal();
	const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
	UNiagaraComponent* Component = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Niagara_SpawnImpact);
		Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			GetWorld(), System, Impact.PositionCm + Normal * 8.0f, Rotation.Rotator(),
			FVector::OneVector, false, false, ENCPoolMethod::ManualRelease, true);
	}
	if (!Component)
	{
		UE_LOG(LogDublinImpactFX, Verbose, TEXT("Impact instance not spawned (Niagara scalability/pre-cull)."));
		return;
	}
	Component->SetVariableFloat(TEXT("User.ImpactRadius"), Radius);
	Component->SetVariableFloat(TEXT("User.ImpactStrength"), DublinImpactFX::VisualStrength(Impact));
	Component->SetVariableVec3(TEXT("User.SurfaceNormal"), Normal);
	if (System == BombSystem.Get() && bSmokeColumnContractReady)
	{
		DublinImpactFX::SetSmokeParameters(*Component, Smoke, Rotation);
	}
	if (System == BombSystem.Get())
	{
		DublinImpactFX::SetBombEmitterMask(*Component, false);
		if (bMaximumContractReady && bWantsMaximum && !Maximum.HasStructuralSources())
		{
			Component->SetEmitterEnable(TEXT("UpwardMeshBurst"), false);
			Component->SetEmitterEnable(TEXT("GroundRing"), false);
		}
	}
	Component->SetRandomSeedOffset(Impact.Seed);
	Component->SetSystemFixedBounds(DublinImpactFX::EffectBounds(Impact, Smoke));
	Component->SetCastShadow(false);
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Niagara_ActivateImpact);
		Component->Activate(true);
	}
	FDublinActiveImpactEffect& Entry = ActiveImpacts.AddDefaulted_GetRef();
	Entry.Component = Component;
	Entry.Significance = Significance;
	Entry.bSmokeColumn = Smoke.IsEnabled();
	Entry.Deadline = GetWorld()->GetTimeSeconds() + DublinImpactFX::EffectLifetime(Impact, Smoke);
}

bool UDublinImpactEffectsSubsystem::TryEmitMaximum(const FDublinImpact& Impact,
	const DublinImpactFX::FMaximumPresentation& Presentation, float Significance)
{
	if (!MaximumSystem)
	{
		if (!bReportedMaximumFailure)
		{
			UE_LOG(LogDublinImpactFX, Display, TEXT("Maximum VFX authoring pending: %s. Using unchanged legacy bomb VFX; maximum-tier visual acceptance remains pending."),
				DublinImpactFX::MaximumSystemPath);
			bReportedMaximumFailure = true;
		}
		return false;
	}
	if (!Presentation.IsEnabled())
	{
		UE_LOG(LogDublinImpactFX, Error, TEXT("Maximum presentation rejected: approved radius %.1f cm exceeds the supported %.1f cm recipe. Using ordinary VFX, not changing damage."),
			Impact.RadiusCm, DublinImpactFX::MaximumApprovedRadiusCm);
		return false;
	}
	if (!DublinImpactFX::CanAdmitMaximum(DublinImpactFX::CountMaximumPresentations(ActiveImpacts)))
	{
		UE_LOG(LogDublinImpactFX, Display, TEXT("Maximum presentation budget 2/2 occupied; using ordinary VFX for this accepted impact."));
		return false;
	}
	bool bReady = bMaximumContractReady && MaximumSystem && MaximumSystem->IsValid() && MaximumSystem->IsReadyToRun();
#if WITH_EDITOR
	if (MaximumSystem && MaximumSystem->HasOutstandingCompilationRequests(true))
	{
		MaximumSystem->PollForCompilationComplete();
		bReady = false;
	}
#endif
	if (!bReady)
	{
		if (!bReportedMaximumFailure)
		{
			UE_LOG(LogDublinImpactFX, Error, TEXT("Maximum presentation unavailable: %s %s Ordinary VFX fallback is NOT maximum-tier completion."),
				*MaximumFailure, bMaximumContractReady ? TEXT("Niagara compilation/readiness is pending or invalid.") : TEXT(""));
			bReportedMaximumFailure = true;
		}
		return false;
	}
	UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), MaximumSystem,
		Impact.PositionCm, FRotator::ZeroRotator, FVector::OneVector, false, false, ENCPoolMethod::ManualRelease, true);
	if (!Component)
	{
		UE_LOG(LogDublinImpactFX, Verbose, TEXT("Maximum impact pre-culled by the existing Niagara scalability policy; not retrying as another system."));
		return true;
	}
	DublinImpactFX::SetMaximumParameters(*Component, Impact, Presentation);
	Component->SetCastShadow(false);
	Component->Activate(true);
	FDublinActiveImpactEffect& Entry = ActiveImpacts.AddDefaulted_GetRef();
	Entry.Component = Component;
	Entry.Significance = Significance;
	Entry.bMaximumPresentation = true;
	Entry.Deadline = GetWorld()->GetTimeSeconds() + DublinImpactFX::MaximumLifetimeGuardSeconds;
	const TWeakObjectPtr<UNiagaraComponent> WeakComponent(Component);
	GetWorld()->GetTimerManager().SetTimer(Entry.StopTimer,
		FTimerDelegate::CreateWeakLambda(this, [this, WeakComponent]()
		{
			if (WeakComponent.IsValid()) { StopMaximumPresentation(WeakComponent.Get()); }
		}), DublinImpactFX::MaximumTransientSeconds, false);
	if (!Presentation.HasStructuralSources())
	{
		UE_LOG(LogDublinImpactFX, Display, TEXT("Maximum impact has no confirmed damage samples: structural chips, flecks and dust front disabled; origin plume/flash only."));
	}
	EmitSmokeOnly(Impact, Significance);
	return true;
}

void UDublinImpactEffectsSubsystem::EmitSmokeOnly(const FDublinImpact& Impact, float Significance)
{
	if (!bSmokeColumnContractReady || !DublinImpactFX::CanAdmit(ActiveImpacts.Num()))
	{
		UE_LOG(LogDublinImpactFX, Warning, TEXT("Maximum impact has no persistent smoke tail: %s"),
			bSmokeColumnContractReady ? TEXT("shared sixteen-system budget is full.") : *SmokeColumnFailure);
		return;
	}
	const DublinImpactFX::FSmokeColumn Smoke = DublinImpactFX::MakeSmokeColumn(Impact, DublinImpactFX::CountSmokeColumns(ActiveImpacts));
	if (!Smoke.IsEnabled())
	{
		UE_LOG(LogDublinImpactFX, Display, TEXT("Maximum impact has no persistent smoke tail: global four-column budget is full."));
		return;
	}
	const FVector Normal = Impact.Normal.GetSafeNormal();
	const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
	UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), BombSystem,
		Impact.PositionCm + Normal * 8.0f, Rotation.Rotator(), FVector::OneVector, false, false, ENCPoolMethod::ManualRelease, true);
	if (!Component)
	{
		UE_LOG(LogDublinImpactFX, Verbose, TEXT("Smoke-only tail pre-culled by Niagara."));
		return;
	}
	Component->SetVariableFloat(TEXT("User.ImpactRadius"), DublinImpactFX::VisualRadius(Impact));
	Component->SetVariableFloat(TEXT("User.ImpactStrength"), DublinImpactFX::VisualStrength(Impact));
	Component->SetVariableVec3(TEXT("User.SurfaceNormal"), Normal);
	DublinImpactFX::SetSmokeParameters(*Component, Smoke, Rotation);
	DublinImpactFX::SetBombEmitterMask(*Component, true);
	Component->SetRandomSeedOffset(Impact.Seed);
	Component->SetSystemFixedBounds(DublinImpactFX::EffectBounds(Impact, Smoke));
	Component->SetCastShadow(false);
	Component->Activate(true);
	FDublinActiveImpactEffect& Entry = ActiveImpacts.AddDefaulted_GetRef();
	Entry.Component = Component;
	Entry.Significance = Significance;
	Entry.bSmokeColumn = true;
	Entry.Deadline = GetWorld()->GetTimeSeconds() + DublinImpactFX::EffectLifetime(Impact, Smoke);
}

void UDublinImpactEffectsSubsystem::Deinitialize()
{
	GetWorld()->GetTimerManager().ClearTimer(CleanupTimer);
	while (!ActiveImpacts.IsEmpty())
	{
		ReleaseImpact(ActiveImpacts.Num() - 1);
	}
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
