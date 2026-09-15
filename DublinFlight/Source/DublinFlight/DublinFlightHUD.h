#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DublinFlightHUD.generated.h"

UCLASS()
class DUBLINFLIGHT_API ADublinFlightHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	double SmoothedFrameSeconds = 0.0;
};
