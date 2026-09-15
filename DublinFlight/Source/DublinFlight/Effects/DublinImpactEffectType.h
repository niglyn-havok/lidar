#pragma once

#include "CoreMinimal.h"
#include "NiagaraEffectType.h"
#include "DublinImpactEffectType.generated.h"

UCLASS()
class DUBLINFLIGHT_API UDublinImpactEffectType : public UNiagaraEffectType
{
	GENERATED_BODY()

public:
	UDublinImpactEffectType(const FObjectInitializer& ObjectInitializer);
};
