#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DublinFlightHUD.generated.h"

class APlayerController;
class ADublinProjectile;

struct FDublinCannonSight
{
	FVector Muzzle = FVector::ZeroVector;
	FVector Direction = FVector::ZeroVector;
	FVector WorldPoint = FVector::ZeroVector;
	FVector2D ScreenPoint = FVector2D::ZeroVector;
};

enum class EDublinBombMarkerState : uint8 { Visible, BelowView, Offscreen, Unavailable };

struct FDublinBombMarker
{
	TWeakObjectPtr<ADublinProjectile> Projectile;
	FVector WorldPoint = FVector::ZeroVector;
	FVector2D ScreenPoint = FVector2D::ZeroVector;
	bool bHasPosition = false;
	bool bProjected = false;
};

struct FDublinBombSight
{
	int32 ReleasedCount = 0;
	TArray<FDublinBombMarker> Markers;
};

UCLASS()
class DUBLINFLIGHT_API ADublinFlightHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
	bool GetCannonSight(FDublinCannonSight& OutSight) const;
	FDublinBombSight GetBombSight() const;
	static EDublinBombMarkerState ClassifyBombMarker(const FDublinBombMarker& Marker, const FVector2D& ViewportSize);
	static TArray<FBox2D> LayoutBombLabels(int32 Count, const FBox2D& Bounds, const FVector2D& LabelSize);
	static bool ProjectSightPoint(const APlayerController* Player, const FVector& WorldPoint,
		FVector2D& OutScreenPoint);

private:
	double SmoothedFrameSeconds = 0.0;
};
