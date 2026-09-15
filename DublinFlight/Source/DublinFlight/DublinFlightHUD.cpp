#include "DublinFlightHUD.h"

#include "DublinFlightPawn.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Weapons/DublinWeaponComponent.h"

void ADublinFlightHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas || Canvas->ClipX <= 0.0f || Canvas->ClipY <= 0.0f)
	{
		return;
	}

	const ADublinFlightPawn* Plane = PlayerOwner ? Cast<ADublinFlightPawn>(PlayerOwner->GetPawn()) : nullptr;
	const float Scale = FMath::Clamp(FMath::Min(Canvas->ClipX / 1280.0f, Canvas->ClipY / 720.0f), 0.55f, 1.65f);
	const FLinearColor Panel(0.015f, 0.025f, 0.04f, 0.82f);
	const FLinearColor Cyan(0.2f, 0.88f, 1.0f);
	const FLinearColor White(0.9f, 0.95f, 0.98f);
	const FLinearColor Muted(0.56f, 0.67f, 0.73f);
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;

	const auto Text = [this, Scale, Font](const FString& Value, const FLinearColor& Color, float X, float Y)
	{
		DrawText(Value, Color, X, Y, Font, Scale * 1.15f);
	};
	const float Margin = 18.0f * Scale;
	DrawRect(Panel, Margin, Margin, 355.0f * Scale, 157.0f * Scale);
	DrawRect(Cyan, Margin, Margin, 3.0f * Scale, 157.0f * Scale);
	if (!Plane)
	{
		Text(TEXT("DUBLIN FLIGHT | NO AIRCRAFT"), Cyan, Margin + 14.0f * Scale, Margin + 12.0f * Scale);
		Text(TEXT("Use DublinFlightGameMode and a PlayerStart."), White, Margin + 14.0f * Scale, Margin + 43.0f * Scale);
		return;
	}

	const float X = Margin + 14.0f * Scale;
	Text(Plane->IsGodMode() ? TEXT("DUBLIN FLIGHT | GOD") : TEXT("DUBLIN FLIGHT | FLIGHT"), Cyan, X, Margin + 12.0f * Scale);
	Text(FString::Printf(TEXT("SPEED  %5.1f m/s  |  %5.0f km/h"),
		Plane->GetSpeedMetersPerSecond(), Plane->GetSpeedMetersPerSecond() * 3.6f), White, X, Margin + 42.0f * Scale);
	Text(FString::Printf(TEXT("ALT    %7.1f m  (world Z)"), Plane->GetAltitudeMeters()), White, X, Margin + 67.0f * Scale);
	static const TCHAR* Directions[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW") };
	const int32 DirectionIndex = FMath::RoundToInt(Plane->GetHeadingDegrees() / 45.0f) % UE_ARRAY_COUNT(Directions);
	Text(FString::Printf(TEXT("HDG    %03.0f deg  %s"), Plane->GetHeadingDegrees(), Directions[DirectionIndex]),
		White, X, Margin + 92.0f * Scale);
	Text(Plane->IsGodMode()
		? FString::Printf(TEXT("Resume speed %.0f m/s | no inertia"), Plane->CruiseSpeedMetersPerSecond)
		: TEXT("Speed envelope 20-65 m/s | arcade flight"), Muted, X, Margin + 124.0f * Scale);

	const float HelpHeight = 108.0f * Scale;
	const float HelpY = Canvas->ClipY - Margin - HelpHeight - 22.0f * Scale;
	DrawRect(Panel, Margin, HelpY, 590.0f * Scale, HelpHeight);
	Text(Plane->IsGodMode() ? TEXT("W/S forward/back | A/D strafe | Space/C up/down")
		: TEXT("W/S nose down/up | A/D bank | Q/E yaw"), White, X, HelpY + 10.0f * Scale);
	Text(Plane->IsGodMode() ? TEXT("Mouse aim | movement is horizontal, altitude is separate")
		: TEXT("Shift/Ctrl throttle up/down | counter-bank to level"), White, X, HelpY + 33.0f * Scale);
	Text(TEXT("LMB hold cannon | B/RMB bomb | [ / ] bomb yield"), White, X, HelpY + 56.0f * Scale);
	Text(TEXT("G flight/god | Home reset | release/re-press after G/Home"), Muted, X, HelpY + 79.0f * Scale);
	DrawRect(Panel, Margin - 4.0f * Scale, Canvas->ClipY - Margin - 18.0f * Scale,
		FMath::Min(850.0f * Scale, Canvas->ClipX - 2.0f * Margin), 24.0f * Scale);
	Text(TEXT("(c) OpenStreetMap contributors; Survey Laefer et al., CC BY 4.0 | F1 credits"),
		Muted, Margin, Canvas->ClipY - Margin - 13.0f * Scale);

	const double FrameSeconds = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0;
	if (FMath::IsFinite(FrameSeconds) && FrameSeconds > 0.0)
	{
		SmoothedFrameSeconds = SmoothedFrameSeconds > 0.0
			? FMath::Lerp(SmoothedFrameSeconds, FrameSeconds, 1.0 - FMath::Exp(-FrameSeconds * 3.0))
			: FrameSeconds;
	}
	const float FpsX = Canvas->ClipX - Margin - 290.0f * Scale;
	DrawRect(Panel, FpsX, Margin, 290.0f * Scale, 34.0f * Scale);
	Text(SmoothedFrameSeconds > 0.0
		? FString::Printf(TEXT("%.0f FPS | %.1f ms (frame delta)"), 1.0 / SmoothedFrameSeconds, SmoothedFrameSeconds * 1000.0)
		: TEXT("FPS --"), White, FpsX + 12.0f * Scale, Margin + 9.0f * Scale);
	if (const UDublinWeaponComponent* Weapon = Plane->Weapons)
	{
		const float WeaponX = Canvas->ClipX - Margin - 390.0f * Scale;
		const float WeaponY = Margin + 46.0f * Scale;
		DrawRect(Panel, WeaponX, WeaponY, 390.0f * Scale, 124.0f * Scale);
		Text(FString::Printf(TEXT("%s | BOMB %.1f t TNT (game yield)"), *Weapon->CurrentWeapon,
			Weapon->BombYieldTonsTNT), Cyan, WeaponX + 12.0f * Scale, WeaponY + 9.0f * Scale);
		Text(FString::Printf(TEXT("Cannon %d | Bombs %d | In flight %d"), Weapon->CannonShots,
			Weapon->BombsDropped, Weapon->GetActiveProjectileCount()), White,
			WeaponX + 12.0f * Scale, WeaponY + 34.0f * Scale);
		Text(FString::Printf(TEXT("Accepted %d | Rejected %d | City %s"), Weapon->AcceptedImpacts,
			Weapon->RejectedImpacts, Weapon->IsCityReady() ? TEXT("READY") : TEXT("WAIT")),
			Muted, WeaponX + 12.0f * Scale, WeaponY + 59.0f * Scale);
		if (!Weapon->LastFailure.IsEmpty())
		{
			Text(Weapon->LastFailure.Left(56), FLinearColor(1.0f, 0.72f, 0.3f),
				WeaponX + 12.0f * Scale, WeaponY + 86.0f * Scale);
			Text(Weapon->LastFailure.Mid(56, 56), FLinearColor(1.0f, 0.72f, 0.3f),
				WeaponX + 12.0f * Scale, WeaponY + 103.0f * Scale);
		}
	}

	const float CenterX = Canvas->ClipX * 0.5f;
	const float CenterY = Canvas->ClipY * 0.5f;
	const float Gap = 5.0f * Scale;
	const float Arm = 14.0f * Scale;
	DrawLine(CenterX - Arm, CenterY, CenterX - Gap, CenterY, Cyan, 1.5f * Scale);
	DrawLine(CenterX + Gap, CenterY, CenterX + Arm, CenterY, Cyan, 1.5f * Scale);
	DrawLine(CenterX, CenterY - Arm, CenterX, CenterY - Gap, Cyan, 1.5f * Scale);
	DrawLine(CenterX, CenterY + Gap, CenterX, CenterY + Arm, Cyan, 1.5f * Scale);
	DrawRect(White, CenterX - Scale, CenterY - Scale, 2.0f * Scale, 2.0f * Scale);

	if (!Plane->FlightStatus.IsEmpty())
	{
		const float StatusY = Margin + 172.0f * Scale;
		DrawRect(Panel, Margin, StatusY, FMath::Min(900.0f * Scale, Canvas->ClipX - 2.0f * Margin), 37.0f * Scale);
		Text(Plane->FlightStatus, FLinearColor(1.0f, 0.72f, 0.3f), X, StatusY + 9.0f * Scale);
	}
	if (Plane->bShowCredits)
	{
		const float CreditsWidth = FMath::Min(960.0f * Scale, Canvas->ClipX - 2.0f * Margin);
		const float CreditsX = (Canvas->ClipX - CreditsWidth) * 0.5f;
		const float CreditsY = Margin + 35.0f * Scale;
		DrawRect(FLinearColor(0.015f, 0.025f, 0.04f, 0.98f), CreditsX, CreditsY, CreditsWidth,
			Canvas->ClipY - CreditsY - Margin - 25.0f * Scale);
		Text(TEXT("DATA CREDITS & LIMITATIONS | F1 close"), Cyan, CreditsX + 15.0f * Scale, CreditsY + 12.0f * Scale);
		TArray<FString> Lines;
		Plane->CreditsText.ParseIntoArrayLines(Lines, false);
		float LineY = CreditsY + 42.0f * Scale;
		for (const FString& Line : Lines)
		{
			DrawText(Line, White, CreditsX + 15.0f * Scale, LineY, Font, Scale);
			LineY += 16.0f * Scale;
		}
	}
}
