#include "Effects/DublinImpactEffectType.h"
#include "Effects/DublinImpactEffectsSubsystem.h"

UDublinImpactEffectType::UDublinImpactEffectType(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAllowCullingForLocalPlayers = true;
	UpdateFrequency = ENiagaraScalabilityUpdateFrequency::High;
	CullReaction = ENiagaraCullReaction::DeactivateImmediate;
	SignificanceHandler = CreateDefaultSubobject<UNiagaraSignificanceHandlerDistance>(TEXT("DistanceSignificance"));
	FNiagaraSystemScalabilitySettings& Settings = SystemScalabilitySettings.Settings.AddDefaulted_GetRef();
	Settings.Platforms = FNiagaraPlatformSet(INDEX_NONE);
	Settings.bCullByDistance = true;
	Settings.MaxDistance = 180000.0f;
	Settings.bCullMaxInstanceCount = true;
	Settings.MaxInstances = DublinImpactFX::MaxSystems;
	Settings.bCullPerSystemMaxInstanceCount = true;
	Settings.MaxSystemInstances = DublinImpactFX::MaxSystems;
	Settings.VisibilityCulling.bCullByViewFrustum = true;
	Settings.VisibilityCulling.bAllowPreCullingByViewFrustum = false;
	Settings.VisibilityCulling.MaxTimeOutsideViewFrustum = 1.5f;
	Settings.VisibilityCulling.bCullWhenNotRendered = true;
	Settings.VisibilityCulling.MaxTimeWithoutRender = 2.0f;
	Settings.BudgetScaling.bScaleMaxInstanceCountByGlobalBudgetUse = true;
	Settings.BudgetScaling.MaxInstanceCountScaleByGlobalBudgetUse = FNiagaraLinearRamp(0.75f, 1.0f, 1.5f, 0.5f);
}
