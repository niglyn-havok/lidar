#include "Effects/DublinMaximumImpact.h"
#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Effects/DublinImpactEffectType.h"

#include "Materials/Material.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFloat.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"

bool DublinImpactFX::WantsMaximumPresentation(const FDublinImpact& Impact)
{
	return Route(Impact) == ERoute::Bomb && Impact.YieldTonsTNT >= MaximumTierYield;
}

bool DublinImpactFX::CanAdmitMaximum(int32 ActiveMaximum)
{
	return ActiveMaximum >= 0 && ActiveMaximum < MaxMaximumPresentations;
}

DublinImpactFX::FMaximumPresentation DublinImpactFX::MakeMaximumPresentation(
	const FDublinImpact& Impact, TConstArrayView<FDublinConfirmedDamageSample> Samples)
{
	FMaximumPresentation Result;
	if (!WantsMaximumPresentation(Impact) || Impact.RadiusCm > MaximumApprovedRadiusCm)
	{
		return Result;
	}
	Result.RadiusCm = Impact.RadiusCm;
	Result.DebrisTravelCm = FMath::Min(Impact.RadiusCm * 0.25f, 3000.0f);
	Result.DustRadiusCm = Impact.RadiusCm * 0.1f;
	Result.PlumeRadiusCm = Impact.RadiusCm * 0.15f;
	Result.PlumeHeightCm = Impact.RadiusCm;
	Result.SpriteRadiusCm = Impact.RadiusCm * 0.1f;
	const int32 Count = FMath::Min(Samples.Num(), MaxDamageSamples);
	Result.DamageOffsets.Reserve(Count);
	Result.DamageNormals.Reserve(Count);
	Result.DroppedSamples = Samples.Num() - Count;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FDublinConfirmedDamageSample& Sample = Samples[Index];
		const FVector Offset = Sample.PositionCm - Impact.PositionCm;
		if (Sample.PositionCm.ContainsNaN() || Sample.Normal.ContainsNaN()
			|| !FMath::IsFinite(Offset.SizeSquared()) || !FMath::IsFinite(Sample.Normal.SizeSquared())
			|| Sample.Normal.IsNearlyZero() || FMath::Abs(Sample.PositionCm.Z) > MaximumDamageSampleAbsZCm
			|| Offset.SizeSquared2D() > FMath::Square(static_cast<double>(Impact.RadiusCm)))
		{
			++Result.DroppedSamples;
			continue;
		}
		Result.DamageOffsets.Add(Offset);
		Result.DamageNormals.Add(Sample.Normal.GetSafeNormal());
	}
	return Result;
}

int32 DublinImpactFX::FMaximumPresentation::ParticleBudget() const
{
	return !IsEnabled() ? 0 : MaximumPlumeParticles + MaximumAccentParticles
		+ (HasStructuralSources() ? MaximumMeshParticles + MaximumFleckParticles + MaximumDustParticles : 0);
}

FBox DublinImpactFX::FMaximumPresentation::Bounds() const
{
	if (!IsEnabled()) { return FBox(ForceInit); }
	const float Width = PlumeRadiusCm + SpriteRadiusCm;
	FBox Result(FVector(-Width, -Width, -SpriteRadiusCm), FVector(Width, Width, PlumeHeightCm + SpriteRadiusCm));
	const FVector Margin(FMath::Max(DebrisTravelCm, DustRadiusCm) + SpriteRadiusCm);
	for (const FVector& Offset : DamageOffsets)
	{
		Result += Offset - Margin;
		Result += Offset + Margin;
	}
	return Result;
}

void DublinImpactFX::SetMaximumParameters(UNiagaraComponent& Component, const FDublinImpact& Impact,
	const FMaximumPresentation& Presentation)
{
	Component.SetVariableFloat(TEXT("User.ImpactRadius"), Presentation.RadiusCm);
	Component.SetVariableFloat(TEXT("User.ImpactStrength"), VisualStrength(Impact));
	Component.SetVariableVec3(TEXT("User.SurfaceNormal"), Impact.Normal.GetSafeNormal());
	Component.SetVariableFloat(TEXT("User.TransientSeconds"), MaximumTransientSeconds);
	Component.SetVariableFloat(TEXT("User.AccentSeconds"), MaximumAccentSeconds);
	Component.SetVariableFloat(TEXT("User.DustOpacity"), MaximumDustOpacity);
	Component.SetVariableFloat(TEXT("User.PlumeOpacity"), MaximumPlumeOpacity);
	Component.SetVariableFloat(TEXT("User.DebrisTravel"), Presentation.DebrisTravelCm);
	Component.SetVariableFloat(TEXT("User.DustRadius"), Presentation.DustRadiusCm);
	Component.SetVariableFloat(TEXT("User.PlumeRadius"), Presentation.PlumeRadiusCm);
	Component.SetVariableFloat(TEXT("User.PlumeHeight"), Presentation.PlumeHeightCm);
	Component.SetVariableFloat(TEXT("User.SpriteRadius"), Presentation.SpriteRadiusCm);
	const bool bStructural = Presentation.HasStructuralSources();
	Component.SetVariableInt(TEXT("User.DamageSampleCount"), Presentation.DamageOffsets.Num());
	Component.SetVariableInt(TEXT("User.MeshParticleCount"), bStructural ? MaximumMeshParticles : 0);
	Component.SetVariableInt(TEXT("User.FleckParticleCount"), bStructural ? MaximumFleckParticles : 0);
	Component.SetVariableInt(TEXT("User.DustParticleCount"), bStructural ? MaximumDustParticles : 0);
	Component.SetVariableInt(TEXT("User.PlumeParticleCount"), MaximumPlumeParticles);
	Component.SetVariableInt(TEXT("User.AccentParticleCount"), MaximumAccentParticles);
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector(&Component, TEXT("User.DamageOffsets"), Presentation.DamageOffsets);
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector(&Component, TEXT("User.DamageNormals"), Presentation.DamageNormals);
	for (const TCHAR* Name : {TEXT("MasonryChips"), TEXT("DebrisFlecks"), TEXT("DustFront")})
	{
		Component.SetEmitterEnable(Name, bStructural);
	}
	Component.SetEmitterEnable(TEXT("TransientPlume"), true);
	Component.SetEmitterEnable(TEXT("BriefAccents"), true);
	Component.SetRandomSeedOffset(Impact.Seed);
	Component.SetSystemFixedBounds(Presentation.Bounds());
	for (const TCHAR* Name : {TEXT("MasonryChips"), TEXT("DebrisFlecks"), TEXT("DustFront"), TEXT("TransientPlume"), TEXT("BriefAccents")})
	{
		Component.SetEmitterFixedBounds(Name, Presentation.Bounds());
	}
}

void DublinImpactFX::SetBombEmitterMask(UNiagaraComponent& Component, bool bSmokeOnly)
{
	for (const TCHAR* Name : {TEXT("OmnidirectionalBurst"), TEXT("UpwardMeshBurst"), TEXT("HotCore"), TEXT("GroundRing")})
	{
		Component.SetEmitterEnable(Name, !bSmokeOnly);
	}
	Component.SetEmitterEnable(TEXT("SimpleSpriteBurst"), true);
}

bool DublinImpactFX::ValidateMaximumAsset(const UNiagaraSystem* System, FString& Failure)
{
	Failure.Reset();
	if (!System)
	{
		Failure = FString::Printf(TEXT("Required maximum-tier asset %s is missing; maximum presentation is unavailable."), MaximumSystemPath);
		return false;
	}
	const FNiagaraUserRedirectionParameterStore& Parameters = System->GetExposedParameters();
	auto Require = [&Parameters, &Failure, System](const TCHAR* Name, const FNiagaraTypeDefinition& Type)
	{
		if (Parameters.IndexOf(FNiagaraVariable(Type, Name)) != INDEX_NONE) { return true; }
		Failure = FString::Printf(TEXT("%s is missing correctly typed %s."), *System->GetPathName(), Name);
		return false;
	};
	for (const TCHAR* Name : {TEXT("User.ImpactRadius"), TEXT("User.ImpactStrength"), TEXT("User.TransientSeconds"),
		TEXT("User.AccentSeconds"), TEXT("User.DustOpacity"), TEXT("User.PlumeOpacity"),
		TEXT("User.DebrisTravel"), TEXT("User.DustRadius"), TEXT("User.PlumeRadius"), TEXT("User.PlumeHeight"), TEXT("User.SpriteRadius")})
	{
		if (!Require(Name, FNiagaraTypeDefinition::GetFloatDef())) { return false; }
	}
	for (const TCHAR* Name : {TEXT("User.DamageSampleCount"), TEXT("User.MeshParticleCount"), TEXT("User.FleckParticleCount"),
		TEXT("User.DustParticleCount"), TEXT("User.PlumeParticleCount"), TEXT("User.AccentParticleCount")})
	{
		if (!Require(Name, FNiagaraTypeDefinition::GetIntDef())) { return false; }
	}
	if (!Require(TEXT("User.SurfaceNormal"), FNiagaraTypeDefinition::GetVec3Def())
		|| !Require(TEXT("User.DamageOffsets"), FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass()))
		|| !Require(TEXT("User.DamageNormals"), FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass())))
	{
		return false;
	}
	for (const TCHAR* Name : {TEXT("User.DamageOffsets"), TEXT("User.DamageNormals")})
	{
		const UNiagaraDataInterfaceArrayFloat3* Array = Cast<UNiagaraDataInterfaceArrayFloat3>(
			Parameters.GetDataInterface(FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass()), Name)));
		if (!Array || Array->MaxElements != MaxDamageSamples)
		{
			Failure = FString::Printf(TEXT("%s requires an initialized float3 array with maxElements=%d."), Name, MaxDamageSamples);
			return false;
		}
	}
	if (System->GetEffectType() != GetDefault<UDublinImpactEffectType>() || System->GetNumEmitters() != 5)
	{
		Failure = TEXT("Maximum tier requires exactly five emitters and the existing shared sixteen-system EffectType.");
		return false;
	}
#if WITH_EDITORONLY_DATA
	const UNiagaraScript* UpdateScript = System->GetSystemUpdateScript();
	bool bConsumesTransient = false, bConsumesLegacySmoke = false;
	if (UpdateScript)
	{
		auto InspectCompiledRead = [&bConsumesTransient, &bConsumesLegacySmoke](const FNiagaraVariable& Variable)
		{
			bConsumesTransient |= Variable.GetName() == TEXT("User.TransientSeconds")
				&& Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef();
			bConsumesLegacySmoke |= Variable.GetName().ToString().StartsWith(TEXT("User.Smoke"));
		};
		const FNiagaraVMExecutableData& Compiled = UpdateScript->GetVMExecutableData();
		for (const FNiagaraVariable& Variable : Compiled.Parameters.Parameters) { InspectCompiledRead(Variable); }
		// Batched system scripts read per-instance user values from compiled input datasets, not just uniforms.
		for (const auto& DataSet : Compiled.DataSetToParameters)
		{
			for (const FNiagaraVariable& Variable : DataSet.Value.Parameters) { InspectCompiledRead(Variable); }
		}
	}
	if (!bConsumesTransient || bConsumesLegacySmoke)
	{
		Failure = TEXT("Maximum system compiled lifecycle must consume User.TransientSeconds, not the removed ordinary smoke parameter.");
		return false;
	}
#endif
	const TCHAR* Names[] = {TEXT("MasonryChips"), TEXT("DebrisFlecks"), TEXT("DustFront"), TEXT("TransientPlume"), TEXT("BriefAccents")};
	const int32 Budgets[] = {MaximumMeshParticles, MaximumFleckParticles, MaximumDustParticles, MaximumPlumeParticles, MaximumAccentParticles};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		const FNiagaraEmitterHandle* Handle = System->GetEmitterHandles().FindByPredicate(
			[&Names, Index](const FNiagaraEmitterHandle& Candidate) { return Candidate.GetName() == FName(Names[Index]); });
		const FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
		if (!Data || !Handle->GetIsEnabled() || !Data->bLocalSpace || !Data->bDeterminism || Data->RandomSeed != 0
			|| Data->SimTarget != ENiagaraSimTarget::GPUComputeSim
			|| Data->AllocationMode != EParticleAllocationMode::FixedCount || Data->PreAllocationCount != Budgets[Index])
		{
			Failure = FString::Printf(TEXT("%s requires local-space deterministic GPU %s with fixed allocation %d and seed base zero."),
				*System->GetPathName(), Names[Index], Budgets[Index]);
			return false;
		}
		if (Data->GetRenderers().Num() != 1 || !IsValid(Data->GetRenderers()[0]) || !Data->GetRenderers()[0]->GetIsEnabled())
		{
			Failure = FString::Printf(TEXT("%s requires exactly one enabled renderer."), Names[Index]);
			return false;
		}
		if (Index == 0)
		{
			const UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(Data->GetRenderers()[0]);
			if (!Mesh || Mesh->Meshes.IsEmpty() || Mesh->Meshes.Num() > 3 || Mesh->bCastShadows)
			{
				Failure = TEXT("MasonryChips requires a real, shadowless mesh renderer.");
				return false;
			}
			for (const FNiagaraMeshRendererMeshProperties& Entry : Mesh->Meshes)
			{
				if (!Entry.Mesh)
				{
					Failure = TEXT("MasonryChips contains a missing mesh.");
					return false;
				}
			}
			TArray<UMaterialInterface*> Materials;
			Mesh->GetUsedMaterials(nullptr, Materials);
			if (Materials.IsEmpty())
			{
				Failure = TEXT("MasonryChips has no authored material.");
				return false;
			}
			for (UMaterialInterface* Interface : Materials)
			{
				const UMaterial* Material = Interface ? Interface->GetMaterial() : nullptr;
				if (!Material || Material->IsDefaultMaterial() || !Material->GetUsageByFlag(MATUSAGE_NiagaraMeshParticles))
				{
					Failure = TEXT("MasonryChips has a missing/default/non-Niagara mesh material.");
					return false;
				}
			}
		}
		else
		{
			const UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Data->GetRenderers()[0]);
			if (!Sprite || Sprite->bCastShadows)
			{
				Failure = FString::Printf(TEXT("%s requires a shadowless sprite renderer."), Names[Index]);
				return false;
			}
		}
	}
	return ValidateSpriteMaterials(System, Failure);
}
