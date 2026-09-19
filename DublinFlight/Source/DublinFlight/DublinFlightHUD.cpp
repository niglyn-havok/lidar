#include "DublinFlightHUD.h"

#include "City/DublinCityData.h"
#include "City/DublinCityWorld.h"
#include "CollisionQueryParams.h"
#include "DublinFlightPawn.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Weapons/DublinProjectile.h"

bool ADublinFlightHUD::ProjectSightPoint(const APlayerController* Player, const FVector& WorldPoint,
	FVector2D& OutScreenPoint)
{
	if (!Player || WorldPoint.ContainsNaN()) { return false; }
	FVector ViewLocation;
	FRotator ViewRotation;
	Player->GetPlayerViewPoint(ViewLocation, ViewRotation);
	if (ViewLocation.ContainsNaN() || ViewRotation.ContainsNaN()
		|| FVector::DotProduct(WorldPoint - ViewLocation, ViewRotation.Vector()) <= 0.0)
	{
		return false;
	}
	return Player->ProjectWorldLocationToScreen(WorldPoint, OutScreenPoint, true)
		&& FMath::IsFinite(OutScreenPoint.X) && FMath::IsFinite(OutScreenPoint.Y);
}

bool ADublinFlightHUD::GetCannonSight(FDublinCannonSight& OutSight) const
{
	const ADublinFlightPawn* Plane = PlayerOwner ? Cast<ADublinFlightPawn>(PlayerOwner->GetPawn()) : nullptr;
	UWorld* World = GetWorld();
	if (!Plane || !Plane->Weapons || !World) { return false; }
	FTransform LaunchTransform = FTransform::Identity;
	FVector LaunchVelocity = FVector::ZeroVector;
	if (!Plane->Weapons->GetCannonLaunch(LaunchTransform, LaunchVelocity) || LaunchVelocity.IsNearlyZero())
	{
		return false;
	}
	OutSight.Muzzle = LaunchTransform.GetLocation();
	OutSight.Direction = LaunchVelocity.GetSafeNormal();
	// A line-of-aim reference, not a bomb landing or swept-projectile impact prediction.
	OutSight.WorldPoint = OutSight.Muzzle + OutSight.Direction * 100000.0;
	FHitResult Hit;
	const FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinCannonSight), true, Plane);
	if (World->LineTraceSingleByChannel(Hit, OutSight.Muzzle, OutSight.WorldPoint, ECC_Visibility, Query))
	{
		OutSight.WorldPoint = Hit.ImpactPoint;
	}
	return ProjectSightPoint(PlayerOwner, OutSight.WorldPoint, OutSight.ScreenPoint);
}

FDublinBombSight ADublinFlightHUD::GetBombSight() const
{
	FDublinBombSight Sight;
	const ADublinFlightPawn* Plane = PlayerOwner ? Cast<ADublinFlightPawn>(PlayerOwner->GetPawn()) : nullptr;
	if (!Plane || !Plane->Weapons) { return Sight; }
	Sight.ReleasedCount = Plane->Weapons->BombsDropped;
	for (ADublinProjectile* Bomb : Plane->Weapons->GetActiveBombs())
	{
		FDublinBombMarker& Marker = Sight.Markers.AddDefaulted_GetRef();
		Marker.Projectile = Bomb;
		Marker.WorldPoint = Bomb->GetActorLocation();
		Marker.bHasPosition = !Marker.WorldPoint.ContainsNaN();
		if (Marker.bHasPosition)
		{
			Marker.bProjected = ProjectSightPoint(PlayerOwner, Marker.WorldPoint, Marker.ScreenPoint);
		}
	}
	return Sight;
}

EDublinBombMarkerState ADublinFlightHUD::ClassifyBombMarker(const FDublinBombMarker& Marker, const FVector2D& ViewportSize)
{
	if (!Marker.bHasPosition || Marker.WorldPoint.ContainsNaN()
		|| !FMath::IsFinite(ViewportSize.X) || !FMath::IsFinite(ViewportSize.Y)
		|| ViewportSize.X <= 0 || ViewportSize.Y <= 0
		|| !FMath::IsFinite(Marker.ScreenPoint.X) || !FMath::IsFinite(Marker.ScreenPoint.Y))
	{
		return EDublinBombMarkerState::Unavailable;
	}
	if (!Marker.bProjected) { return EDublinBombMarkerState::Offscreen; }
	if (Marker.ScreenPoint.Y >= ViewportSize.Y) { return EDublinBombMarkerState::BelowView; }
	if (Marker.ScreenPoint.X < 0 || Marker.ScreenPoint.X >= ViewportSize.X || Marker.ScreenPoint.Y < 0)
	{
		return EDublinBombMarkerState::Offscreen;
	}
	return EDublinBombMarkerState::Visible;
}

TArray<FBox2D> ADublinFlightHUD::LayoutBombLabels(int32 Count, const FBox2D& Bounds, const FVector2D& LabelSize)
{
	TArray<FBox2D> Labels;
	if (Count == 0) { return Labels; }
	const FVector2D Available = Bounds.GetSize();
	if (!ensureMsgf(Count > 0 && Bounds.bIsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN()
		&& !LabelSize.ContainsNaN() && Available.X > 0 && Available.Y > 0 && LabelSize.X > 0 && LabelSize.Y > 0,
		TEXT("Bomb labels require a finite, positive layout area and cell size")))
	{
		return Labels;
	}
	int32 Columns = 1;
	double Fit = 0.0;
	for (int32 Candidate = 1; Candidate <= Count; ++Candidate)
	{
		const int32 Rows = FMath::DivideAndRoundUp(Count, Candidate);
		const double CandidateFit = FMath::Min(1.0, FMath::Min(
			Available.X / (Candidate * LabelSize.X), Available.Y / (Rows * LabelSize.Y)));
		if (CandidateFit > Fit)
		{
			Columns = Candidate;
			Fit = CandidateFit;
		}
	}
	const int32 Rows = FMath::DivideAndRoundUp(Count, Columns);
	const FVector2D Cell = LabelSize * Fit;
	const FVector2D Origin(Bounds.Max.X - Columns * Cell.X, Bounds.Min.Y);
	Labels.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D Position = Origin + FVector2D(Index / Rows * Cell.X, Index % Rows * Cell.Y);
		Labels.Emplace(Position, Position + Cell);
	}
	return Labels;
}

void ADublinFlightHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas || Canvas->ClipX <= 0.0f || Canvas->ClipY <= 0.0f)
	{
		return;
	}

	const ADublinFlightPawn* Plane = PlayerOwner ? Cast<ADublinFlightPawn>(PlayerOwner->GetPawn()) : nullptr;
	const float Scale = FMath::Clamp(FMath::Min(Canvas->ClipX / 1280.0f, Canvas->ClipY / 720.0f), 0.55f, 1.65f);
	const FLinearColor Panel(0.015f, 0.025f, 0.04f, 0.68f);
	const FLinearColor Cyan(0.2f, 0.88f, 1.0f);
	const FLinearColor White(0.9f, 0.95f, 0.98f);
	const FLinearColor Muted(0.56f, 0.67f, 0.73f);
	const FLinearColor BombColor(1.0f, 0.76f, 0.2f);
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;

	const auto Text = [this, Scale, Font](const FString& Value, const FLinearColor& Color, float X, float Y)
	{
		DrawText(Value, Color, X, Y, Font, Scale * 1.15f);
	};
	const float Margin = 18.0f * Scale;
	DrawRect(Panel, Margin, Margin, 500.0f * Scale, 96.0f * Scale);
	DrawRect(Cyan, Margin, Margin, 2.0f * Scale, 96.0f * Scale);
	if (!Plane)
	{
		Text(TEXT("DUBLIN FLIGHT | NO AIRCRAFT"), Cyan, Margin + 14.0f * Scale, Margin + 12.0f * Scale);
		Text(TEXT("Use DublinFlightGameMode and a PlayerStart."), White, Margin + 14.0f * Scale, Margin + 43.0f * Scale);
		return;
	}

	const float X = Margin + 14.0f * Scale;
	Text(FString::Printf(TEXT("DUBLIN | %s | %.1f m/s | %s %.1f m/s"), Plane->IsGodMode() ? TEXT("GOD") : TEXT("FLIGHT"),
		Plane->GetSpeedMetersPerSecond(), Plane->IsGodMode() ? TEXT("RESUME") : TEXT("SELECTED"),
		Plane->CruiseSpeedMetersPerSecond), Cyan, X, Margin + 10.0f * Scale);
	static const TCHAR* Directions[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW") };
	const int32 DirectionIndex = FMath::RoundToInt(Plane->GetHeadingDegrees() / 45.0f) % UE_ARRAY_COUNT(Directions);
	Text(FString::Printf(TEXT("ALT %.0f m (world Z) | HDG %03.0f deg %s"),
		Plane->GetAltitudeMeters(), Plane->GetHeadingDegrees(), Directions[DirectionIndex]),
		White, X, Margin + 36.0f * Scale);
	Text(Plane->IsGodMode() ? TEXT("Speed controls: flight only | G resume | Home 40 m/s")
		: TEXT("Wheel +/-5 m/s | Shift/Ctrl +/-10 m/s/s | 5-100 m/s"),
		White, X, Margin + 65.0f * Scale);

	const float FooterY = Canvas->ClipY - Margin - 48.0f * Scale;
	DrawRect(Panel, Margin, FooterY, FMath::Min(850.0f * Scale, Canvas->ClipX - 2.0f * Margin), 48.0f * Scale);
	Text(TEXT("G flight/god | Home reset aircraft + view | [ / ] yield | F1 controls + credits"),
		White, X, FooterY + 5.0f * Scale);
	Text(TEXT("(c) OpenStreetMap contributors; Survey Laefer et al., CC BY 4.0"),
		Muted, X, FooterY + 27.0f * Scale);

	const double FrameSeconds = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0;
	if (FMath::IsFinite(FrameSeconds) && FrameSeconds > 0.0)
	{
		SmoothedFrameSeconds = SmoothedFrameSeconds > 0.0
			? FMath::Lerp(SmoothedFrameSeconds, FrameSeconds, 1.0 - FMath::Exp(-FrameSeconds * 3.0))
			: FrameSeconds;
	}
	const FString FrameInfo = SmoothedFrameSeconds > 0.0
		? FString::Printf(TEXT("%.0f FPS | %.1f ms (frame delta)"), 1.0 / SmoothedFrameSeconds, SmoothedFrameSeconds * 1000.0)
		: TEXT("FPS --");
	const UDublinWeaponComponent* Weapon = Plane->Weapons;
	const FDublinBombSight BombSight = GetBombSight();
	const float WeaponX = Canvas->ClipX - Margin - 370.0f * Scale;
	DrawRect(Panel, WeaponX, Margin, 370.0f * Scale, 96.0f * Scale);
	if (Weapon)
	{
		Text(FString::Printf(TEXT("BOMB %.1f t TNT (game yield)"), Weapon->BombYieldTonsTNT),
			Cyan, WeaponX + 12.0f * Scale, Margin + 10.0f * Scale);
		Text(FString::Printf(TEXT("CITY %s | LMB cannon | B/RMB bomb"),
			Weapon->IsCityReady() ? TEXT("READY") : TEXT("WAIT")),
			White, WeaponX + 12.0f * Scale, Margin + 39.0f * Scale);
		Text(FString::Printf(TEXT("Released %d | In flight %d"), BombSight.ReleasedCount, BombSight.Markers.Num()),
			BombColor, WeaponX + 12.0f * Scale, Margin + 68.0f * Scale);
	}
	const auto DrawBombMarkers = [this, Scale, Font, Margin, Panel, BombColor, FooterY, &BombSight](float Top)
	{
		if (BombSight.Markers.IsEmpty()) { return; }
		const FVector2D Viewport(Canvas->ClipX, Canvas->ClipY);
		TArray<FString> Labels;
		TArray<EDublinBombMarkerState> States;
		FVector2D LabelSize(1.0, 18.0 * Scale);
		for (const FDublinBombMarker& Marker : BombSight.Markers)
		{
			const EDublinBombMarkerState State = ClassifyBombMarker(Marker, Viewport);
			States.Add(State);
			const TCHAR* Status = State == EDublinBombMarkerState::Visible ? TEXT("in view")
				: State == EDublinBombMarkerState::BelowView ? TEXT("below view")
				: State == EDublinBombMarkerState::Offscreen ? TEXT("off-screen") : TEXT("unavailable");
			Labels.Add(FString::Printf(TEXT("BOMB %u | %s"), Marker.Projectile->GetUniqueID(), Status));
			float Width = 0.0f;
			float Height = 0.0f;
			GetTextSize(Labels.Last(), Width, Height, Font, Scale * 1.15f);
			LabelSize.X = FMath::Max(LabelSize.X, Width + 12.0 * Scale);
			LabelSize.Y = FMath::Max(LabelSize.Y, Height + 6.0 * Scale);
		}
		const FBox2D Bounds(FVector2D(FMath::Min(Margin, Canvas->ClipX * 0.05f),
			FMath::Min(Top, Canvas->ClipY * 0.65f)),
			FVector2D(Canvas->ClipX - FMath::Min(Margin, Canvas->ClipX * 0.05f),
				FMath::Max(FooterY - 12.0f * Scale, Canvas->ClipY * 0.7f)));
		const TArray<FBox2D> Boxes = LayoutBombLabels(Labels.Num(), Bounds, LabelSize);
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			if (States[Index] != EDublinBombMarkerState::Visible) { continue; }
			const FVector2D Point = BombSight.Markers[Index].ScreenPoint;
			const float BombX = static_cast<float>(Point.X);
			const float BombY = static_cast<float>(Point.Y);
			const float Radius = 7.0f * Scale;
			DrawLine(BombX, FMath::Max(0.0f, BombY - Radius), FMath::Min(Canvas->ClipX, BombX + Radius), BombY,
				BombColor, 1.5f * Scale);
			DrawLine(FMath::Min(Canvas->ClipX, BombX + Radius), BombY, BombX, FMath::Min(Canvas->ClipY, BombY + Radius),
				BombColor, 1.5f * Scale);
			DrawLine(BombX, FMath::Min(Canvas->ClipY, BombY + Radius), FMath::Max(0.0f, BombX - Radius), BombY,
				BombColor, 1.5f * Scale);
			DrawLine(FMath::Max(0.0f, BombX - Radius), BombY, BombX, FMath::Max(0.0f, BombY - Radius),
				BombColor, 1.5f * Scale);
			DrawLine(BombX, BombY, Boxes[Index].Min.X, Boxes[Index].GetCenter().Y, BombColor, Scale);
		}
		// Draw labels after all leader lines so clustered/overlapping projections cannot obscure their text.
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			const FBox2D& Box = Boxes[Index];
			const float Fit = static_cast<float>(Box.GetSize().Y / LabelSize.Y);
			DrawRect(Panel, Box.Min.X, Box.Min.Y, Box.GetSize().X, Box.GetSize().Y);
			DrawText(Labels[Index], BombColor, Box.Min.X + 6.0f * Scale * Fit, Box.Min.Y + 3.0f * Scale * Fit,
				Font, Scale * 1.15f * Fit);
		}
	};
	float StatusY = Margin + 108.0f * Scale;
	const auto Warning = [this, Scale, Margin, X, Panel, &StatusY, &Text](const FString& Message)
	{
		DrawRect(Panel, Margin, StatusY, FMath::Min(1000.0f * Scale, Canvas->ClipX - 2.0f * Margin), 34.0f * Scale);
		Text(Message, FLinearColor(1.0f, 0.72f, 0.3f), X, StatusY + 8.0f * Scale);
		StatusY += 40.0f * Scale;
	};
	if (!Plane->FlightStatus.IsEmpty())
	{
		Warning(Plane->FlightStatus);
	}
	if (!Weapon)
	{
		Warning(TEXT("Weapon component unavailable."));
	}
	else if (!Weapon->LastFailure.IsEmpty())
	{
		Warning(Weapon->LastFailure);
	}
	if (const ADublinCityWorld* City = Weapon ? Weapon->GetCity() : nullptr)
	{
		if (const FDublinCityData* Data = City->GetSourceData())
		{
			const FVector LocalPosition = City->GetActorTransform().InverseTransformPosition(Plane->GetActorLocation());
			const double HalfExtentCm = Data->ExtentMeters * 50.0;
			if (FMath::Abs(LocalPosition.X) > HalfExtentCm || FMath::Abs(LocalPosition.Y) > HalfExtentCm)
			{
				Warning(TEXT("Outside surveyed area - Home returns to Dublin."));
			}
		}
	}
	FDublinCannonSight Sight;
	const float Gap = 4.0f * Scale;
	const float Arm = 10.0f * Scale;
	if (GetCannonSight(Sight) && Sight.ScreenPoint.X >= Arm && Sight.ScreenPoint.X <= Canvas->ClipX - Arm
		&& Sight.ScreenPoint.Y >= Arm && Sight.ScreenPoint.Y <= Canvas->ClipY - Arm)
	{
		const float AimX = static_cast<float>(Sight.ScreenPoint.X);
		const float AimY = static_cast<float>(Sight.ScreenPoint.Y);
		DrawLine(AimX - Arm, AimY, AimX - Gap, AimY, Cyan, 1.5f * Scale);
		DrawLine(AimX + Gap, AimY, AimX + Arm, AimY, Cyan, 1.5f * Scale);
		DrawLine(AimX, AimY - Arm, AimX, AimY - Gap, Cyan, 1.5f * Scale);
		DrawLine(AimX, AimY + Gap, AimX, AimY + Arm, Cyan, 1.5f * Scale);
		DrawRect(White, AimX - Scale, AimY - Scale, 2.0f * Scale, 2.0f * Scale);
	}
	else
	{
		Warning(TEXT("Cannon aim outside view or unavailable - no impact prediction."));
	}
	DrawBombMarkers(StatusY);
	if (Plane->bShowCredits)
	{
		const float CreditsWidth = FMath::Min(960.0f * Scale, Canvas->ClipX - 2.0f * Margin);
		const float CreditsX = (Canvas->ClipX - CreditsWidth) * 0.5f;
		const float CreditsY = Margin + 35.0f * Scale;
		DrawRect(FLinearColor(0.015f, 0.025f, 0.04f, 0.98f), CreditsX, CreditsY, CreditsWidth,
			Canvas->ClipY - CreditsY - Margin - 25.0f * Scale);
		Text(TEXT("CONTROLS, DIAGNOSTICS & DATA CREDITS | F1 close"), Cyan,
			CreditsX + 15.0f * Scale, CreditsY + 12.0f * Scale);
		TArray<FString> Lines;
		Lines.Add(Plane->IsGodMode() ? TEXT("W/S forward/back | A/D strafe | Space/C up/down | Mouse aim")
			: TEXT("Body axes: W/S nose down/up | A/D bank | Q/E yaw"));
		Lines.Add(Plane->IsGodMode()
			? FString::Printf(TEXT("Movement is horizontal; altitude is separate. Resume speed %.0f m/s; no inertia."),
				Plane->CruiseSpeedMetersPerSecond)
			: TEXT("Mouse: orbit camera, 0.36 deg/unit (no hold); no steering. Counter-bank to stop turn assist."));
		Lines.Add(TEXT("Flight speed 5-100 m/s: wheel +/-5 m/s; hold Shift/Ctrl +/-10 m/s each second. Home resets to 40."));
		Lines.Add(TEXT("Full body roll, including inverted flight. Vertical pitch guard: +/-80 deg; usable axes stay responsive."));
		Lines.Add(TEXT("LMB hold cannon | B/RMB drop bomb | [ / ] halve/double game yield"));
		Lines.Add(TEXT("G flight/god | Home restore aircraft + chase view | Release/re-press held controls after G/Home"));
		Lines.Add(TEXT("Crosshair: projected cannon line of aim, not a bomb landing or guaranteed impact point."));
		Lines.Add(TEXT("Amber BOMB labels track ALL live bombs; lines link in-view positions, not predicted landings."));
		Lines.Add(TEXT("Each bomb has its own below-view/off-screen/unavailable status; impact/expiry removes only that bomb."));
		Lines.Add(FrameInfo);
		if (Weapon)
		{
			Lines.Add(FString::Printf(TEXT("Cannon %d | Bombs %d | In flight %d | Last %s"),
				Weapon->CannonShots, Weapon->BombsDropped, Weapon->GetActiveProjectileCount(), *Weapon->CurrentWeapon));
			Lines.Add(FString::Printf(TEXT("Accepted %d | Rejected %d | City %s"),
				Weapon->AcceptedImpacts, Weapon->RejectedImpacts, Weapon->IsCityReady() ? TEXT("READY") : TEXT("WAIT")));
		}
		Lines.Add(TEXT(""));
		TArray<FString> CreditLines;
		Plane->CreditsText.ParseIntoArrayLines(CreditLines, false);
		Lines.Append(CreditLines);
		float LineY = CreditsY + 42.0f * Scale;
		const float AvailableHeight = FMath::Max(1.0f, Canvas->ClipY - Margin - 30.0f * Scale - LineY);
		const float LineStep = FMath::Min(16.0f * Scale, AvailableHeight / FMath::Max(1, Lines.Num()));
		const float DetailScale = FMath::Min(Scale, LineStep / 16.0f);
		for (const FString& Line : Lines)
		{
			DrawText(Line, White, CreditsX + 15.0f * Scale, LineY, Font, DetailScale);
			LineY += LineStep;
		}
	}
}
