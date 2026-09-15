#include "DublinFlightGameMode.h"

#include "DublinFlightHUD.h"
#include "DublinFlightPawn.h"
#include "GameFramework/PlayerController.h"

ADublinFlightGameMode::ADublinFlightGameMode()
{
	DefaultPawnClass = ADublinFlightPawn::StaticClass();
	HUDClass = ADublinFlightHUD::StaticClass();
	PlayerControllerClass = APlayerController::StaticClass();
	bStartPlayersAsSpectators = false;
}
