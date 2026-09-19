#include "Weapons/DublinWeaponComponent.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Camera/CameraTypes.h"
#include "City/DublinCityWorld.h"
#include "CollisionQueryParams.h"
#include "Components/SphereComponent.h"
#include "Dom/JsonObject.h"
#include "GameFramework/SpringArmComponent.h"
#include "DublinFlightHUD.h"
#include "DublinFlightPawn.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "SceneView.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "Weapons/DublinProjectile.h"

#include <limits>

namespace DublinWeapons::PIETests
{
bool CaptureViewport(UWorld* World, APlayerController* Player)
{
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (!Player || !Viewport || !Viewport->Viewport) { return false; }
	if (!Viewport->Viewport->HasFocus() || !Viewport->Viewport->HasMouseCapture())
	{
		Player->SetInputMode(FInputModeGameOnly());
		Viewport->Viewport->SetUserFocus(true);
		Viewport->Viewport->CaptureMouse(true);
	}
	return Viewport->Viewport->HasFocus() && Viewport->Viewport->HasMouseCapture();
}

class FRunWeapons final : public IAutomationLatentCommand
{
public:
	explicit FRunWeapons(FAutomationTestBase& InTest, bool bInMultiBombOnly = false)
		: Test(InTest), bMultiBombOnly(bInMultiBombOnly) {}
	virtual ~FRunWeapons() override
	{
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
		Cleanup();
	}

	virtual bool Update() override
	{
		if (StartedAt == 0.0) { StartedAt = FPlatformTime::Seconds(); }
		if (!bStarted) { return WaitForReadyCity(); }
		if (bEnded || !World.IsValid() || World->bIsTearingDown || !Player.IsValid() || !Plane.IsValid()
			|| !Weapon.IsValid() || Player->GetPawn() != Plane.Get())
		{
			Test.AddError(TEXT("Weapons PIE lost its world, possession or component during actual-input acceptance."));
			Cleanup();
			return true;
		}
		if (!Capture())
		{
			Test.AddError(TEXT("Weapons PIE could not maintain native viewport focus/capture."));
			Cleanup();
			return true;
		}
		if (FPlatformTime::Seconds() - StartedAt > 180.0 || World->GetTimeSeconds() - PhaseStarted > 30.0)
		{
			Test.AddError(FString::Printf(TEXT("Weapons PIE timed out in '%s': %s"), *Steps[Phase].Name, *Weapon->LastFailure));
			Cleanup();
			return true;
		}
		const double Now = World->GetTimeSeconds();
		if (Now <= LastWorldTime) { return false; }
		LastWorldTime = Now;
		++Frames;
		FStep& Step = Steps[Phase];
		if (Step.Observe) { Step.Observe(); }
		if (Frames < 2 || Now - PhaseStarted < Step.Seconds || (Step.Until && !Step.Until())) { return false; }
		if (Step.Finish) { Step.Finish(); }
		if (bFailed) { Cleanup(); return true; }
		Test.AddInfo(FString::Printf(TEXT("Weapons actual-input phase: %s (%.3f s / %d frames)"),
			*Step.Name, Now - PhaseStarted, Frames));
		if (++Phase == Steps.Num())
		{
			Cleanup();
			Test.AddInfo(TEXT("Weapons input/trajectory/city-acceptance checks complete. Counters do NOT verify fracture or FX visuals. "
				"Surviving PIE is left in GOD; AutomationController may tear it down after the test."));
			return true;
		}
		BeginPhase();
		return false;
	}

private:
	struct FStep
	{
		FString Name;
		double Seconds = 0.0;
		TFunction<void()> Start;
		TFunction<void()> Finish;
		TFunction<void()> Observe;
		TFunction<bool()> Until;
	};
	FAutomationTestBase& Test;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<APlayerController> Player;
	TWeakObjectPtr<ADublinFlightPawn> Plane;
	TWeakObjectPtr<UDublinWeaponComponent> Weapon;
	TWeakObjectPtr<ADublinProjectile> FlightBomb;
	TWeakObjectPtr<ADublinProjectile> GodBomb;
	TArray<FStep> Steps;
	TArray<FKey> Held;
	FInputDeviceId Device = INPUTDEVICEID_NONE;
	FTransform Snapshot;
	FVector InheritedVelocity = FVector::ZeroVector;
	int32 BeforeShots = 0;
	int32 BeforeBombs = 0;
	int32 CannonImpactBaseline = 0;
	int32 BombImpactBaseline = 0;
	int32 Phase = 0;
	int32 Frames = 0;
	double StartedAt = 0.0;
	double PhaseStarted = 0.0;
	double LastWorldTime = 0.0;
	bool bStarted = false;
	bool bEnded = false;
	bool bCleaned = false;
	bool bFailed = false;
	bool bMultiBombOnly = false;

	void Require(const TCHAR* What, bool Condition) { bFailed |= !Test.TestTrue(What, Condition); }
	void CheckBombSight(const ADublinProjectile* ExpectedBomb, int32 ExpectedCount = -1)
	{
		const ADublinFlightHUD* HUD = Cast<ADublinFlightHUD>(Player->GetHUD());
		Require(TEXT("Actual player has the native bomb-tracking HUD"), HUD != nullptr);
		if (!HUD) { return; }
		const FDublinBombSight Sight = HUD->GetBombSight();
		Require(TEXT("Default HUD release feedback matches actual successful releases"),
			Sight.ReleasedCount == Weapon->BombsDropped);
		const TArray<ADublinProjectile*> ActiveBombs = Weapon->GetActiveBombs();
		Require(TEXT("HUD in-flight count comes from the current native projectile list"),
			Sight.Markers.Num() == ActiveBombs.Num());
		if (ExpectedCount >= 0)
		{
			Require(TEXT("All expected live bombs have individual markers"), Sight.Markers.Num() == ExpectedCount);
		}
		if (!ExpectedBomb)
		{
			Require(TEXT("Cannon-only activity does not create a bomb marker"),
				ActiveBombs.IsEmpty() && Sight.Markers.IsEmpty());
			return;
		}
		Require(TEXT("Tracking includes the expected live bomb, never the last cannon"),
			Sight.Markers.ContainsByPredicate([ExpectedBomb](const FDublinBombMarker& Marker)
			{
				return Marker.Projectile.IsValid() && Marker.Projectile.Get() == ExpectedBomb;
			}));
		ULocalPlayer* LocalPlayer = Player->GetLocalPlayer();
		FSceneViewProjectionData Projection;
		const bool bHasProjection = LocalPlayer
			&& LocalPlayer->GetProjectionData(World->GetGameViewport()->Viewport, Projection, INDEX_NONE);
		Require(TEXT("Actual bomb projection has native viewport data"), bHasProjection);
		for (int32 Index = 0; Index < Sight.Markers.Num(); ++Index)
		{
			const FDublinBombMarker& Marker = Sight.Markers[Index];
			ADublinProjectile* Bomb = Marker.Projectile.Get();
			Require(TEXT("Every marker retains a live projectile"), IsValid(Bomb));
			if (!IsValid(Bomb)) { continue; }
			Require(TEXT("Markers cover the entire native list once in launch order"),
				ActiveBombs.IsValidIndex(Index) && ActiveBombs[Index] == Bomb);
			Require(TEXT("Bomb marker follows the actual live actor rather than a predicted landing"),
				Marker.bHasPosition && Marker.WorldPoint.Equals(Bomb->GetActorLocation(), 0.001));
			Require(TEXT("Tracked actor is an unresolved bomb"),
				Bomb->GetImpactKind() == EDublinImpactKind::Bomb && !Bomb->HasResolvedImpact() && !Bomb->IsActorBeingDestroyed());
			if (!bHasProjection) { continue; }
			const FIntRect Rect = Projection.GetConstrainedViewRect();
			FVector2D Expected = FVector2D::ZeroVector;
			const bool bProjected = FSceneView::ProjectWorldToScreen(
				Bomb->GetActorLocation(), Rect, Projection.ComputeViewProjectionMatrix(), Expected);
			Require(TEXT("Bomb projection availability matches the real engine projection"), Marker.bProjected == bProjected);
			if (bProjected)
			{
				Expected -= FVector2D(Rect.Min.X, Rect.Min.Y);
				Require(TEXT("Bomb marker matches the actual falling actor within one pixel"), Marker.ScreenPoint.Equals(Expected, 1.0));
			}
		}
	}
	void CheckCannonSight(const ADublinProjectile* Shell, bool bRequireOffCenter)
	{
		const ADublinFlightHUD* HUD = Cast<ADublinFlightHUD>(Player->GetHUD());
		Require(TEXT("Actual player uses the native flight HUD"), HUD != nullptr);
		if (!HUD) { return; }
		const FTransform Before = Plane->GetActorTransform();
		const FRotator BeforeAim = Plane->GetViewRotation();
		FDublinCannonSight Sight;
		const bool bHasSight = HUD->GetCannonSight(Sight);
		Require(TEXT("Actual cannon aim projects through the player viewport"), bHasSight);
		if (!bHasSight) { return; }
		Require(TEXT("Sight is on the forward muzzle ray"),
			FVector::DotProduct(Sight.WorldPoint - Sight.Muzzle, Sight.Direction) > 0.0
			&& FVector::CrossProduct(Sight.WorldPoint - Sight.Muzzle, Sight.Direction).Size() < 0.1);
		if (Shell)
		{
			Require(TEXT("HUD muzzle offset matches a genuinely launched cannon shell"),
				(Sight.Muzzle - Plane->GetActorLocation()).Equals(
					Shell->GetLaunchPosition() - Shell->GetLaunchOwnerPosition(), 0.1));
			Require(TEXT("HUD direction matches actual inherited cannon launch velocity"),
				Sight.Direction.Equals(Shell->GetLaunchVelocity().GetSafeNormal(), 0.0001));
		}
		if (Plane->IsGodMode())
		{
			Require(TEXT("Stationary god sight follows weapon aim, not the depressed chase camera"),
				Sight.Direction.Equals(Plane->GetViewRotation().Vector(), 0.0001));
		}
		ULocalPlayer* LocalPlayer = Player->GetLocalPlayer();
		FSceneViewProjectionData Projection;
		const bool bHasProjection = LocalPlayer
			&& LocalPlayer->GetProjectionData(World->GetGameViewport()->Viewport, Projection, INDEX_NONE);
		Require(TEXT("Real local-player projection data is available"), bHasProjection);
		if (bHasProjection)
		{
			const FIntRect Rect = Projection.GetConstrainedViewRect();
			FVector2D Expected = FVector2D::ZeroVector;
			const bool bProjected = FSceneView::ProjectWorldToScreen(
				Sight.WorldPoint, Rect, Projection.ComputeViewProjectionMatrix(), Expected);
			Require(TEXT("Independent engine scene-view projection succeeds"), bProjected);
			if (bProjected) { Expected -= FVector2D(Rect.Min.X, Rect.Min.Y); }
			Require(TEXT("HUD aim matches the real constrained viewport projection within one pixel"),
				bProjected && Sight.ScreenPoint.Equals(Expected, 1.0));
			if (bRequireOffCenter)
			{
				Require(TEXT("Depressed chase view does not draw a misleading screen-centre reticle"),
					FMath::Abs(Sight.ScreenPoint.Y - Rect.Height() * 0.5) > Rect.Height() * 0.1);
			}
		}
		FVector ViewLocation;
		FRotator ViewRotation;
		Player->GetPlayerViewPoint(ViewLocation, ViewRotation);
		FVector2D RejectedPoint;
		Require(TEXT("Behind-camera aim is rejected rather than mirrored onto the HUD"),
			!ADublinFlightHUD::ProjectSightPoint(Player.Get(),
				ViewLocation - ViewRotation.Vector() * 1000.0, RejectedPoint));
		Require(TEXT("Nonfinite aim cannot create a centre-reticle fallback"),
			!ADublinFlightHUD::ProjectSightPoint(Player.Get(),
				FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0), RejectedPoint));
		Require(TEXT("Sight calculation does not move or reorient the aircraft or weapon aim"),
			Plane->GetActorTransform().Equals(Before, 0.0001)
			&& Plane->GetViewRotation().Equals(BeforeAim, 0.0001));
		Require(TEXT("Chase-only framing uses the bounded 28-degree downward bias"),
			Plane->CameraBoom->GetComponentRotation().Equals(
				FRotator(FMath::Clamp(Before.Rotator().Pitch - 28.0, -85.0, 85.0),
					Before.Rotator().Yaw, 0.0), 0.001));
	}
	bool Capture()
	{
		return CaptureViewport(World.Get(), Player.Get());
	}
	void Send(const FKey& Key, EInputEvent Event, float Amount)
	{
		if (!Player.IsValid()) { return; }
		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, Event, Amount, 1, Device);
		Args.DeltaTime = World.IsValid() ? World->GetDeltaSeconds() : 1.0f / 60.0f;
		Player->InputKey(Args);
	}
	void Press(const FKey& Key) { Held.AddUnique(Key); Send(Key, IE_Pressed, 1.0f); }
	void Release(const FKey& Key) { Send(Key, IE_Released, 0.0f); Held.Remove(Key); }
	void OnPIEEnding(bool bSimulating) { bEnded = true; Cleanup(); }
	void Cleanup()
	{
		if (bCleaned) { return; }
		bCleaned = true;
		const TArray<FKey> Keys = Held;
		for (const FKey& Key : Keys) { Release(Key); }
		Send(EKeys::MouseX, IE_Axis, 0.0f);
		Send(EKeys::MouseY, IE_Axis, 0.0f);
		if (Player.IsValid()) { Player->FlushPressedKeys(); }
		if (Weapon.IsValid()) { Weapon->SuppressInput(); Weapon->ClearProjectiles(); }
		if (Plane.IsValid() && !Plane->IsGodMode()) { Plane->ToggleGodMode(); }
	}
	bool WaitForReadyCity()
	{
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (!Candidate || Candidate->WorldType != EWorldType::PIE || Candidate->bIsTearingDown) { continue; }
				if (UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) != TEXT("/Game/Maps/Dublin"))
				{
					Test.AddError(TEXT("Weapons acceptance requires actual /Game/Maps/Dublin PIE."));
					return true;
				}
				APlayerController* PC = Candidate->GetFirstPlayerController();
				ADublinFlightPawn* Pawn = PC ? Cast<ADublinFlightPawn>(PC->GetPawn()) : nullptr;
				if (!PC || !PC->IsLocalController() || !PC->PlayerInput || !Pawn || !Pawn->Weapons) { continue; }
				World = Candidate; Player = PC; Plane = Pawn; Weapon = Pawn->Weapons;
				if (!Pawn->bSpawnCaptured || !Weapon->IsCityReady() || !Capture()) { continue; }
				Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PC->GetPlatformUserId());
				if (Device == INPUTDEVICEID_NONE) { Test.AddError(TEXT("No native player input device for weapons PIE.")); return true; }
				FEditorDelegates::PrePIEEnded.AddRaw(this, &FRunWeapons::OnPIEEnding);
				PC->FlushPressedKeys();
				Weapon->SuppressInput();
				Weapon->ClearProjectiles();
				Pawn->ResetFlight();
				Require(TEXT("Fresh weapons fixture begins in flight"), !Pawn->IsGodMode());
				Require(TEXT("Real city contains buildings"), Weapon->GetCity()->BuildingCount > 0);
				bStarted = true;
				LastWorldTime = Candidate->GetTimeSeconds();
				BuildSteps();
				BeginPhase();
				return false;
			}
		}
		if (FPlatformTime::Seconds() - StartedAt > 60.0)
		{
			Test.AddError(TEXT("Native fixture failed to start possessed, focused Dublin PIE with a READY city within 60 seconds."));
			Cleanup();
			return true;
		}
		return false;
	}
	void Add(const TCHAR* Name, double Seconds, TFunction<void()> Start, TFunction<void()> Finish,
		TFunction<void()> Observe = {}, TFunction<bool()> Until = {})
	{
		FStep Step;
		Step.Name = Name; Step.Seconds = Seconds; Step.Start = MoveTemp(Start); Step.Finish = MoveTemp(Finish);
		Step.Observe = MoveTemp(Observe); Step.Until = MoveTemp(Until); Steps.Add(MoveTemp(Step));
	}
	void BeginPhase()
	{
		PhaseStarted = World->GetTimeSeconds(); Frames = 0;
		Snapshot = Plane->GetActorTransform(); BeforeShots = Weapon->CannonShots; BeforeBombs = Weapon->BombsDropped;
		if (Steps[Phase].Start) { Steps[Phase].Start(); }
	}
	void BuildSteps()
	{
		if (bMultiBombOnly)
		{
			BuildMultiBombSteps();
			return;
		}
		Add(TEXT("Flight LMB holds repeated cannon"), 0.65, [this]() { Press(EKeys::LeftMouseButton); }, [this]()
		{
			Test.AddInfo(FString::Printf(TEXT("Cannon input diagnostic: shots=%d elapsed=%.3f observedFrames=%d keyDown=%d focus=%d capture=%d failure='%s'"),
				Weapon->CannonShots - BeforeShots, World->GetTimeSeconds() - PhaseStarted, Frames,
				Player->IsInputKeyDown(EKeys::LeftMouseButton),
				World->GetGameViewport()->Viewport->HasFocus(), World->GetGameViewport()->Viewport->HasMouseCapture(),
				*Weapon->LastFailure));
			Require(TEXT("Actual held LMB launches multiple shells"), Weapon->CannonShots - BeforeShots >= 3);
			Require(TEXT("Cannon is rate bounded"), Weapon->CannonShots - BeforeShots <=
				FMath::CeilToInt((World->GetTimeSeconds() - PhaseStarted) * 8.0) + 1);
			ADublinProjectile* Shell = Weapon->GetLastProjectile();
			Require(TEXT("Cannon remains a real flying actor"), IsValid(Shell));
			if (Shell)
			{
				Require(TEXT("Cannon launch ignores its aircraft owner"), Shell->GetOwner() == Plane.Get() && !Shell->HasResolvedImpact());
				Require(TEXT("Muzzle is outside aircraft collision sphere"),
					FVector::Distance(Shell->GetLaunchPosition(), Shell->GetLaunchOwnerPosition()) > 470.0);
				Require(TEXT("Shell movement is swept"), Shell->Movement->bSweepCollision);
				CheckCannonSight(Shell, true);
				CheckBombSight(nullptr);
			}
		}, {}, [this]()
		{
			// Wait for real input results within the existing deadline; startup hitches must not require catch-up fire.
			return Weapon->CannonShots - BeforeShots >= 3 || !Weapon->LastFailure.IsEmpty();
		});
		Add(TEXT("LMB release stops fire"), 0.25, [this]() { Release(EKeys::LeftMouseButton); }, [this]()
		{
			Require(TEXT("Release stops repeated cannon"), Weapon->CannonShots == BeforeShots);
		});
		Add(TEXT("Flight B drops ballistic bomb"), 0.08, [this]()
		{
			InheritedVelocity = Plane->GetVelocity();
			Press(EKeys::B);
		}, [this]()
		{
			Require(TEXT("B launches exactly one bomb"), Weapon->BombsDropped == BeforeBombs + 1);
			FlightBomb = Weapon->GetLastProjectile();
			Require(TEXT("Bomb is a live projectile, not instant AOE"), FlightBomb.IsValid());
			if (FlightBomb.IsValid())
			{
				Require(TEXT("Bomb inherits flight velocity"), FlightBomb->GetLaunchVelocity().Equals(InheritedVelocity, 1.0));
				Test.TestNearlyEqual(TEXT("Bomb gravity scale is one"), FlightBomb->Movement->ProjectileGravityScale, 1.0f);
				CheckBombSight(FlightBomb.Get());
			}
			Release(EKeys::B);
		});
		Add(TEXT("Actual bomb continues under gravity"), 0.35, {}, [this]()
		{
			Require(TEXT("Bomb survives long enough for ballistic observation"), FlightBomb.IsValid());
			if (FlightBomb.IsValid())
			{
				Require(TEXT("Actual projectile Z falls"), FlightBomb->GetActorLocation().Z < FlightBomb->GetLaunchPosition().Z - 20.0);
				Require(TEXT("Actual bomb vertical velocity accelerates downward"), FlightBomb->Movement->Velocity.Z < -100.0);
				Test.TestNearlyEqual(TEXT("Actual bomb retains horizontal velocity"),
					FlightBomb->Movement->Velocity.X, InheritedVelocity.X, 1.0);
				CheckBombSight(FlightBomb.Get());
			}
		});
		Add(TEXT("RMB shares bomb cooldown"), 0.08, [this]() { Press(EKeys::RightMouseButton); }, [this]()
		{
			Require(TEXT("B then RMB cannot bypass bomb cooldown"), Weapon->BombsDropped == BeforeBombs);
			Release(EKeys::RightMouseButton);
		});
		Add(TEXT("Actual G enters stationary weapons mode"), 0.1, [this]() { Press(EKeys::G); }, [this]()
		{
			Require(TEXT("G enters GOD"), Plane->IsGodMode());
			Release(EKeys::G);
		});
		Add(TEXT("Actual mouse aims at the city"), 0.1, {}, [this]()
		{
			Send(EKeys::MouseY, IE_Axis, 0.0f);
			Require(TEXT("Mouse yaw/pitch aim is pawn view rotation"), Plane->GetViewRotation().Equals(Plane->GetActorRotation(), 0.001));
			FMinimalViewInfo View;
			const FRotator BeforeCamera = Plane->GetActorRotation();
			Plane->CalcCamera(0.0f, View);
			Require(TEXT("Downward chase camera does not change weapon aim"),
				Plane->GetActorRotation().Equals(BeforeCamera, 0.001)
				&& Plane->GetViewRotation().Equals(BeforeCamera, 0.001));
			CheckCannonSight(nullptr, false);
		}, [this]()
		{
			Send(EKeys::MouseY, IE_Axis, Plane->GetActorRotation().Pitch > -60.0 ? -500.0f : 0.0f);
		}, [this]() { return Plane->GetActorRotation().Pitch <= -60.0; });
		Add(TEXT("God LMB fires at READY city"), 0.65, [this]()
		{
			CannonImpactBaseline = Weapon->AcceptedCannonImpacts;
			Press(EKeys::LeftMouseButton);
		}, [this]()
		{
			Require(TEXT("Cannon works in GOD"), Weapon->CannonShots - BeforeShots >= 3);
			Require(TEXT("Firing does not translate stationary aircraft"),
				FVector::Distance(Plane->GetActorLocation(), Snapshot.GetLocation()) < 1.0);
			CheckBombSight(FlightBomb.IsValid() && !FlightBomb->HasResolvedImpact()
				&& !FlightBomb->IsActorBeingDestroyed() ? FlightBomb.Get() : nullptr);
			Release(EKeys::LeftMouseButton);
		}, {}, [this]() { return Weapon->CannonShots - BeforeShots >= 3 || !Weapon->LastFailure.IsEmpty(); });
		Add(TEXT("Cannon reaches real city collision"), 0.1, {}, [this]()
		{
			Require(TEXT("Real cannon payload accepted by city"), Weapon->AcceptedCannonImpacts > CannonImpactBaseline);
		}, {}, [this]() { return Weapon->AcceptedCannonImpacts > CannonImpactBaseline || Weapon->RejectedImpacts > 0; });
		Add(TEXT("Right bracket raises game yield"), 0.08, [this]() { Press(EKeys::RightBracket); }, [this]()
		{
			Test.TestNearlyEqual(TEXT("Bracket doubles default game yield"), Weapon->BombYieldTonsTNT, 2.0f);
			Release(EKeys::RightBracket);
		});
		Add(TEXT("Left bracket lowers game yield"), 0.08, [this]() { Press(EKeys::LeftBracket); }, [this]()
		{
			Test.TestNearlyEqual(TEXT("Left bracket returns yield to one"), Weapon->BombYieldTonsTNT, 1.0f);
			Release(EKeys::LeftBracket);
		});
		Add(TEXT("Space is still god climb, not bomb"), 0.2, [this]() { Press(EKeys::SpaceBar); }, [this]()
		{
			Require(TEXT("Space raises god altitude"), Plane->GetActorLocation().Z > Snapshot.GetLocation().Z + 100.0);
			Require(TEXT("Space cannot launch a bomb"), Weapon->BombsDropped == BeforeBombs);
			Release(EKeys::SpaceBar);
		});
		Add(TEXT("C is still god descent, not bomb"), 0.2, [this]() { Press(EKeys::C); }, [this]()
		{
			Require(TEXT("C lowers god altitude"), Plane->GetActorLocation().Z < Snapshot.GetLocation().Z - 100.0);
			Require(TEXT("C cannot launch a bomb"), Weapon->BombsDropped == BeforeBombs);
			Release(EKeys::C);
		});
		Add(TEXT("Wait for god movement release"), 0.1, {}, {});
		Add(TEXT("God RMB drops bomb without forward inertia"), 0.08, [this]()
		{
			BombImpactBaseline = Weapon->AcceptedBombImpacts;
			Press(EKeys::RightMouseButton);
		}, [this]()
		{
			Require(TEXT("RMB launches a god-mode bomb"), Weapon->BombsDropped == BeforeBombs + 1);
			ADublinProjectile* Bomb = Weapon->GetLastProjectile();
			GodBomb = Bomb;
			Require(TEXT("God bomb is a live actor"), IsValid(Bomb));
			if (Bomb)
			{
				Require(TEXT("God bomb inherits zero aircraft velocity"), Bomb->GetLaunchVelocity().IsNearlyZero(0.001));
				CheckBombSight(Bomb);
			}
			Release(EKeys::RightMouseButton);
		});
		Add(TEXT("God bomb falls to an accepted city impact"), 0.5, {}, [this]()
		{
			Require(TEXT("Ballistic bomb accepted by city"), Weapon->AcceptedBombImpacts > BombImpactBaseline);
			if (Weapon->RejectedImpacts > 0) { Test.AddError(FString::Printf(TEXT("City rejected weapon impacts: %s"), *Weapon->LastFailure)); }
			Require(TEXT("Active projectiles stay bounded"), Weapon->GetActiveProjectileCount() <= Weapon->MaximumActiveProjectiles);
			Require(TEXT("Effects only requested for accepted impacts"), Weapon->EffectsRequests <= Weapon->AcceptedImpacts);
		}, {}, [this]() { return Weapon->AcceptedBombImpacts > BombImpactBaseline || Weapon->RejectedImpacts > 0; });
		Add(TEXT("Resolved bomb is removed from tracking"), 0.1, {}, [this]()
		{
			const ADublinFlightHUD* HUD = Cast<ADublinFlightHUD>(Player->GetHUD());
			Require(TEXT("HUD remains available after the real bomb impact"), HUD != nullptr);
			if (HUD)
			{
				const FDublinBombSight Sight = HUD->GetBombSight();
				Require(TEXT("Resolved bomb no longer owns a marker"), Sight.Markers.IsEmpty()
					&& !GodBomb.IsValid() && !FlightBomb.IsValid());
				Require(TEXT("Release feedback remains after impact"), Sight.ReleasedCount == Weapon->BombsDropped
					&& Sight.ReleasedCount > 0);
				Require(TEXT("Completed flight/god bombs leave no stale in-flight marker"),
					Sight.Markers.IsEmpty());
			}
		}, {}, [this]()
		{
			const auto Ended = [](const TWeakObjectPtr<ADublinProjectile>& Bomb)
			{
				return !Bomb.IsValid() || Bomb->HasResolvedImpact() || Bomb->IsActorBeingDestroyed();
			};
			return Ended(GodBomb) && Ended(FlightBomb);
		});
		Add(TEXT("F1 opens inline data credits"), 0.08, [this]() { Press(EKeys::F1); }, [this]()
		{
			Require(TEXT("F1 opens credits"), Plane->bShowCredits);
			Require(TEXT("Data attribution loaded inline"), Plane->CreditsText.Contains(TEXT("OpenStreetMap")));
			Release(EKeys::F1);
		});
		Add(TEXT("Release F1 before closing"), 0.08, {}, {});
		Add(TEXT("F1 closes inline credits"), 0.08, [this]() { Press(EKeys::F1); }, [this]()
		{
			Require(TEXT("F1 closes credits"), !Plane->bShowCredits);
			Release(EKeys::F1);
		});
		Add(TEXT("Native expiry removes a released bomb from tracking"), 0.08, [this]() { Press(EKeys::B); }, [this]()
		{
			Release(EKeys::B);
			ADublinProjectile* Bomb = Weapon->GetLastProjectile();
			Require(TEXT("Expiry fixture is a genuinely released bomb"), Weapon->BombsDropped == BeforeBombs + 1
				&& IsValid(Bomb) && Bomb->GetImpactKind() == EDublinImpactKind::Bomb);
			if (!IsValid(Bomb) || Bomb->GetImpactKind() != EDublinImpactKind::Bomb) { return; }
			CheckBombSight(Bomb);
			// Exercise the engine's expiry lifecycle directly; production lifespan/trajectory remain unchanged.
			Bomb->LifeSpanExpired();
			const ADublinFlightHUD* HUD = Cast<ADublinFlightHUD>(Player->GetHUD());
			Require(TEXT("HUD remains available after expiry"), HUD != nullptr);
			if (HUD)
			{
				const FDublinBombSight Sight = HUD->GetBombSight();
				Require(TEXT("Release feedback remains after expiry"), Sight.ReleasedCount == Weapon->BombsDropped
					&& Sight.ReleasedCount == BeforeBombs + 1);
				Require(TEXT("Expired native actor cannot retain a marker"),
					Sight.Markers.IsEmpty());
			}
		});
	}

	void BuildMultiBombSteps()
	{
		Add(TEXT("Fresh multi-bomb fixture has no markers"), 0.1, {}, [this]() { CheckBombSight(nullptr, 0); });
		Add(TEXT("Actual B releases the oldest tracked bomb"), 0.08, [this]() { Press(EKeys::B); }, [this]()
		{
			Release(EKeys::B);
			Require(TEXT("Oldest bomb is a successful real B release"), Weapon->BombsDropped == BeforeBombs + 1);
			FlightBomb = Weapon->GetLastProjectile();
			Require(TEXT("Oldest bomb is live"), FlightBomb.IsValid());
			if (FlightBomb.IsValid()) { CheckBombSight(FlightBomb.Get(), 1); }
		});
		Add(TEXT("Wait for the unchanged shared bomb cooldown"), 1.6, {}, [this]()
		{
			Require(TEXT("Oldest bomb survives the cooldown under normal ballistics"), FlightBomb.IsValid());
			if (FlightBomb.IsValid()) { CheckBombSight(FlightBomb.Get(), 1); }
		});
		Add(TEXT("Actual RMB releases another bomb without replacing the oldest"), 0.08,
			[this]() { Press(EKeys::RightMouseButton); }, [this]()
		{
			Release(EKeys::RightMouseButton);
			Require(TEXT("Newest bomb is a successful real RMB release"), Weapon->BombsDropped == BeforeBombs + 1);
			GodBomb = Weapon->GetLastProjectile();
			Require(TEXT("Both oldest and newest are simultaneously live"), FlightBomb.IsValid() && GodBomb.IsValid());
			if (FlightBomb.IsValid() && GodBomb.IsValid())
			{
				CheckBombSight(FlightBomb.Get(), 2);
				CheckBombSight(GodBomb.Get(), 2);
				if (!bFailed)
				{
					const FDublinBombSight Sight = Cast<ADublinFlightHUD>(Player->GetHUD())->GetBombSight();
					int32 Width = 0;
					int32 Height = 0;
					Player->GetViewportSize(Width, Height);
					int32 ProjectedCount = 0;
					int32 VisibleCount = 0;
					for (const FDublinBombMarker& Marker : Sight.Markers)
					{
						ProjectedCount += Marker.bProjected ? 1 : 0;
						VisibleCount += ADublinFlightHUD::ClassifyBombMarker(Marker, FVector2D(Width, Height))
							== EDublinBombMarkerState::Visible ? 1 : 0;
					}
					const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
						TEXT("Screenshots"), TEXT("Feedback-20260918"));
					if (FScreenshotRequest::IsScreenshotRequested())
					{
						Test.AddWarning(TEXT("Two-bomb screenshot not queued: another viewport screenshot request is pending."));
					}
					else if (!IFileManager::Get().MakeDirectory(*Directory, true))
					{
						Test.AddWarning(FString::Printf(TEXT("Could not create screenshot directory: %s"), *Directory));
					}
					else
					{
						// Queue the next rendered viewport; the existing cannon phase precedes any bomb removal.
						FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory,
							TEXT("AllBombMarkersActualInputLifecycle-TwoLiveBombs.png")), false, true);
						Test.AddInfo(FString::Printf(TEXT("Queued viewport screenshot: %s | tracked=%d projected=%d visible=%d. "
							"Diagnostic request only, not capture completion or visual acceptance; offscreen bombs remain offscreen."),
							*FScreenshotRequest::GetFilename(), Sight.Markers.Num(), ProjectedCount, VisibleCount));
					}
				}
			}
		});
		Add(TEXT("A newer actual cannon round cannot hide either bomb"), 0.25,
			[this]() { Press(EKeys::LeftMouseButton); }, [this]()
		{
			Release(EKeys::LeftMouseButton);
			ADublinProjectile* Shell = Weapon->GetLastProjectile();
			Require(TEXT("Actual LMB launches a newer cannon shell"), Weapon->CannonShots > BeforeShots
				&& IsValid(Shell) && Shell->GetImpactKind() == EDublinImpactKind::Cannon);
			Require(TEXT("Both bombs remain live after cannon input"), FlightBomb.IsValid() && GodBomb.IsValid());
			if (FlightBomb.IsValid() && GodBomb.IsValid())
			{
				CheckBombSight(FlightBomb.Get(), 2);
				CheckBombSight(GodBomb.Get(), 2);
			}
		});
		Add(TEXT("Oldest bomb expiry removes only its marker"), 0.0, {}, [this]()
		{
			Require(TEXT("Both bombs are live before individual expiry"), FlightBomb.IsValid() && GodBomb.IsValid());
			if (!FlightBomb.IsValid() || !GodBomb.IsValid()) { return; }
			FlightBomb->LifeSpanExpired();
			Require(TEXT("Oldest weak reference becomes invalid after expiry"), !FlightBomb.IsValid());
			Require(TEXT("Newest survives oldest expiry"), GodBomb.IsValid());
			CheckBombSight(GodBomb.Get(), 1);
			Require(TEXT("Expiry cannot decrement successful release feedback"), Weapon->BombsDropped == BeforeBombs);
		});
		Add(TEXT("Newest bomb destruction removes its remaining marker"), 0.0, {}, [this]()
		{
			Require(TEXT("Newest bomb remains independently tracked"), GodBomb.IsValid());
			if (!GodBomb.IsValid()) { return; }
			Require(TEXT("Native bomb destruction succeeds"), GodBomb->Destroy());
			Require(TEXT("Newest weak reference becomes invalid after destruction"), !GodBomb.IsValid());
			CheckBombSight(nullptr, 0);
			Require(TEXT("Destruction cannot decrement successful release feedback"), Weapon->BombsDropped == BeforeBombs);
		});
	}
};

class FBombCollisionWitness final : public IAutomationLatentCommand
{
public:
	FBombCollisionWitness(FAutomationTestBase& InTest, bool bInLowRelease, bool bInInputCoverage = true)
		: Test(InTest), bLowRelease(bInLowRelease), bInputCoverage(bInInputCoverage), StartedWall(FPlatformTime::Seconds()) {}
	virtual ~FBombCollisionWitness() override { Cleanup(); }

	virtual bool Update() override
	{
		if (bFinished) { return true; }
		if (FPlatformTime::Seconds() - StartedWall > 150.0)
		{
			Test.AddError(FString::Printf(TEXT("Bomb collision fixture exceeded 150 wall seconds; no missing case is a collision pass. bootstrap=%s case=%d phase=%d frames=%d"),
				*BootstrapStatus, CaseIndex, Phase, CaseFrames));
			return Finish(true);
		}
		if (!bReady) { return Bootstrap(); }
		if (bSessionEnding || !World.IsValid() || World->bIsTearingDown || !City.IsValid() || !Player.IsValid()
			|| (bCaseActive && !FixturePawn.IsValid()))
		{
			Test.AddError(TEXT("Owned Dublin PIE ended during bomb collision observation."));
			return Finish(true);
		}
		const double Now = World->GetTimeSeconds();
		if (Now <= LastWorldTime) { return false; }
		LastWorldTime = Now;
		if (!bCaseActive)
		{
			if (CaseIndex == Cases.Num()) { return Finish(); }
			if (Now < NextCaseAfterWorldTime) { return false; }
			if (!BeginCase()) { return Finish(true); }
			return false;
		}
		++CaseFrames;
		if (Cases[CaseIndex].Key.IsValid())
		{
			CaptureViewport(World.Get(), Player.Get());
			UGameViewportClient* Client = World->GetGameViewport();
			const bool bFocused = Client && Client->Viewport && Client->Viewport->HasFocus();
			const bool bCaptured = Client && Client->Viewport && Client->Viewport->HasMouseCapture();
			const bool bPossessed = Player->GetPawn() == FixturePawn.Get() && FixturePawn->GetController() == Player.Get();
			CaseEvidence->SetBoolField(TEXT("input_focus_verified"), bFocused);
			CaseEvidence->SetBoolField(TEXT("input_capture_verified"), bCaptured);
			CaseEvidence->SetBoolField(TEXT("input_possession_verified"), bPossessed);
			CaseEvidence->SetBoolField(TEXT("move_input_ignored"), Player->IsMoveInputIgnored());
			CaseEvidence->SetBoolField(TEXT("look_input_ignored"), Player->IsLookInputIgnored());
			if (!bFocused || !bCaptured || !bPossessed || !Player->IsLocalController()
				|| Player->IsMoveInputIgnored() || Player->IsLookInputIgnored())
			{
				if (Now - CaseStarted > 5.0)
				{
					Test.AddInfo(FString::Printf(TEXT("%s input prerequisite timeout: focus=%d capture=%d possession=%d moveIgnored=%d lookIgnored=%d"),
						*Cases[CaseIndex].Name, bFocused, bCaptured, bPossessed,
						Player->IsMoveInputIgnored(), Player->IsLookInputIgnored()));
					EndCase(false, TEXT("fixture_input_capture_unavailable"));
				}
				return false;
			}
		}
		if (Phase == 0)
		{
			if (CaseFrames < 2 || Now - CaseStarted < (Cases[CaseIndex].Key.IsValid() ? 1.6 : 0.1)) { return false; }
			Phase = 1;
			DropStarted = Now;
			LastMovementTime = Now;
			if (Cases[CaseIndex].Key.IsValid())
			{
				Send(Cases[CaseIndex].Key, IE_Pressed);
				bKeyHeld = true;
			}
			else
			{
				const FTransform Transform(LaunchVelocity.IsNearlyZero() ? FRotator::ZeroRotator : LaunchVelocity.Rotation(), LaunchPosition);
				ADublinProjectile* Projectile = World->SpawnActorDeferred<ADublinProjectile>(ADublinProjectile::StaticClass(),
					Transform, FixturePawn.Get(), FixturePawn.Get(), ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				if (!Projectile) { EndCase(false, TEXT("fixture_projectile_spawn_failed")); return false; }
				Watch(Projectile);
				FDublinImpact Payload = MakeImpact(EDublinImpactKind::Bomb, 1.0f, FBombCurve());
				Payload.Seed = 183000 + CaseIndex;
				Projectile->Initialize(Payload, LaunchVelocity, City.Get(), FixturePawn->Weapons);
				// A native tick interval supplies an actual long movement tick, not a manual Tick or substitute mover.
				if (Cases[CaseIndex].MovementInterval > 0.0f)
				{
					Projectile->Movement->SetComponentTickInterval(Cases[CaseIndex].MovementInterval);
				}
				Projectile->FinishSpawning(Transform);
			}
			return false;
		}
		if (bKeyHeld && FixturePawn->Weapons->BombsDropped > BombsBeforeCase)
		{
			Send(Cases[CaseIndex].Key, IE_Released);
			bKeyHeld = false;
		}
		if (bDestroyed)
		{
			const int32 Dispatches = FixturePawn->Weapons->AcceptedImpacts + FixturePawn->Weapons->RejectedImpacts - DispatchesBeforeCase;
			const bool bBombDispatched = DestroyedImpact.Kind == EDublinImpactKind::Bomb && Dispatches == 1
				&& (Cases[CaseIndex].Key.IsValid() || DestroyedImpact.Seed == 183000 + CaseIndex);
			const bool bReleaseContactCorrect = !Cases[CaseIndex].bLow
				|| (bReleaseBlocked && !DestroyedImpact.bWater
					&& DestroyedImpact.PositionCm.Equals(ReleaseHit.ImpactPoint, 100.0));
			const bool bSolidWitness = DestroyedImpact.bWater || ContactEvidence.IsValid()
				|| (Cases[CaseIndex].bLow && bReleaseBlocked);
			const bool bLongTickObserved = Cases[CaseIndex].MovementInterval <= 0.0f || MaxMovementGap >= 0.3;
			CaseEvidence->SetNumberField(TEXT("maximum_observed_movement_gap_seconds"), MaxMovementGap);
			CaseEvidence->SetNumberField(TEXT("actual_motion_segments"), MotionSegments);
			CaseEvidence->SetBoolField(TEXT("release_contact_matches_live_blocker"), bReleaseContactCorrect);
			CaseEvidence->SetBoolField(TEXT("long_native_movement_tick_observed"), bLongTickObserved);
			if (ContactEvidence) { CaseEvidence->SetObjectField(TEXT("live_pre_resolution_contact"), ContactEvidence); }
			EndCase(bResolvedAtDestroy && bBombDispatched && bReleaseContactCorrect && bSolidWitness && bLongTickObserved,
				!bResolvedAtDestroy ? TEXT("destroyed_without_resolve")
				: !bBombDispatched ? TEXT("resolved_without_matching_bomb_dispatch")
				: !bReleaseContactCorrect ? TEXT("resolved_beyond_release_blocker")
				: !bSolidWitness ? TEXT("solid_resolution_without_observed_live_contact")
				: !bLongTickObserved ? TEXT("long_tick_stimulus_not_observed")
				: DestroyedImpact.bWater ? TEXT("resolved_water") : TEXT("resolved_solid"));
			return false;
		}
		if (!Bomb.IsValid())
		{
			if (Now - DropStarted > 2.0) { EndCase(false, TEXT("no_observed_native_bomb")); }
			return false;
		}
		CaseEvidence->SetObjectField(TEXT("latest_live_actor"), Snapshot(*Bomb.Get()));
		if (Bomb->GetImpactKind() != EDublinImpactKind::Bomb || Bomb->Movement->bSimulationUseScopedMovement)
		{
			EndCase(false, TEXT("fixture_requires_single_bomb_and_observable_native_substeps"));
			return false;
		}
		if (Cases[CaseIndex].bLow && Now - DropStarted >= 0.05)
		{
			FHitResult StillBlocking;
			const bool bStillBlocking = Probe(ReleaseOwnerPosition, LaunchPosition, nullptr, StillBlocking);
			CaseEvidence->SetBoolField(TEXT("release_segment_still_blocked"), bStillBlocking);
			if (bStillBlocking) { CaseEvidence->SetObjectField(TEXT("live_release_blocker_after_drop"), DescribeHit(StillBlocking)); }
			const double Radius = Bomb->CollisionRoot->GetScaledSphereRadius();
			const bool bBornBeyond = Bomb->GetLaunchPosition().Z + Radius < ReleaseHit.ImpactPoint.Z - 1.0;
			const bool bStillBeyond = Bomb->GetActorLocation().Z + Radius < ReleaseHit.ImpactPoint.Z - 1.0;
			CaseEvidence->SetBoolField(TEXT("whole_bomb_spawned_below_release_blocker"), bBornBeyond);
			CaseEvidence->SetBoolField(TEXT("whole_live_bomb_still_below_release_blocker"), bStillBeyond);
			if (bReleaseBlocked && bBornBeyond && bStillBeyond && !Bomb->HasResolvedImpact())
			{
				EndCase(false, bStillBlocking ? TEXT("live_unresolved_bomb_beyond_solid_release_segment")
					: TEXT("release_collider_changed_or_disappeared"));
				return false;
			}
		}
		if (MissEvidence)
		{
			CaseEvidence->SetObjectField(TEXT("exact_native_motion_crossed_live_blocker"), MissEvidence);
			EndCase(false, TEXT("live_unresolved_bomb_crossed_blocking_motion_segment"));
			return false;
		}
		if (!Bomb->HasResolvedImpact() && (!Bomb->Movement->UpdatedComponent || !Bomb->Movement->IsActive()))
		{
			EndCase(false, TEXT("movement_stopped_or_updated_component_lost_without_resolve"));
			return false;
		}
		if (Now - DropStarted > 6.0)
		{
			FHitResult RemainingSurface;
			const bool bSurfaceStillPresent = SurfaceAt(Bomb->GetActorLocation(), RemainingSurface);
			CaseEvidence->SetBoolField(TEXT("live_vertical_surface_present_at_final_xy"), bSurfaceStillPresent);
			if (bSurfaceStillPresent)
			{
				CaseEvidence->SetObjectField(TEXT("live_vertical_surface_at_final_xy"), DescribeHit(RemainingSurface));
				const FVector Above = RemainingSurface.ImpactPoint + FVector(0, 0, 100);
				const FVector Below = RemainingSurface.ImpactPoint - FVector(0, 0, 100);
				FHitResult WorldSweep, LocalSweep, LocalRay;
				CaseEvidence->SetBoolField(TEXT("short_world_sphere_hits"), Probe(Above, Below, Bomb.Get(), WorldSweep));
				if (UPrimitiveComponent* SurfaceComponent = RemainingSurface.GetComponent())
				{
					FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinCraterCollisionParity), false);
					CaseEvidence->SetBoolField(TEXT("short_component_sphere_hits"),
						SurfaceComponent->SweepComponent(LocalSweep, Above, Below, FQuat::Identity,
							FCollisionShape::MakeSphere(20.0f), false));
					CaseEvidence->SetBoolField(TEXT("short_component_ray_hits"),
						SurfaceComponent->LineTraceComponent(LocalRay, Above, Below, Query));
					Point(*CaseEvidence, TEXT("component_bounds_origin_cm"), SurfaceComponent->Bounds.Origin);
					Point(*CaseEvidence, TEXT("component_bounds_extent_cm"), SurfaceComponent->Bounds.BoxExtent);
				}
			}
			EndCase(false, bSurfaceStillPresent ? TEXT("unresolved_after_six_seconds_no_solid_miss_witness")
				: TEXT("unresolved_over_source_or_collision_hole_not_proven_sweep_miss"));
		}
		return false;
	}

private:
	enum class ESurface : uint8 { Terrain, Roof, Bridge, Shore };
	struct FCase
	{
		FString Name;
		ESurface Surface;
		FKey Key;
		double SpeedCm;
		bool bLow;
		bool bNeedsCrater;
		float MovementInterval;
	};
	FAutomationTestBase& Test;
	bool bLowRelease;
	bool bInputCoverage;
	double StartedWall;
	FString BootstrapStatus = TEXT("No PIE world discovered");
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ADublinCityWorld> City;
	TWeakObjectPtr<APlayerController> Player;
	TWeakObjectPtr<ADublinFlightPawn> OriginalPawn;
	TWeakObjectPtr<ADublinFlightPawn> FixturePawn;
	TWeakObjectPtr<ADublinProjectile> Bomb;
	TArray<FVector> UsedTerrainSurfaces;
	TArray<TSharedPtr<FJsonValue>> NearSurfaceSegments;
	double DropSurfaceZ = 0;
	bool bBorrowedOriginalPawn = false;
	bool bOriginalPawnTickEnabled = false;
	FTransform OriginalPawnTransform = FTransform::Identity;
	int32 BombsBeforeCase = 0;
	int32 DispatchesBeforeCase = 0;
	double NextCaseAfterWorldTime = 0;
	TWeakObjectPtr<USceneComponent> ObservedRoot;
	FDelegateHandle SpawnHandle;
	FDelegateHandle DestroyHandle;
	FDelegateHandle TransformHandle;
	TArray<FCase> Cases;
	TArray<TSharedPtr<FJsonValue>> Results;
	TSharedPtr<FJsonObject> CaseEvidence;
	TSharedPtr<FJsonObject> ContactEvidence;
	TSharedPtr<FJsonObject> MissEvidence;
	FInputDeviceId Device = INPUTDEVICEID_NONE;
	FVector TerrainAnchor = FVector::ZeroVector;
	FVector LaunchPosition = FVector::ZeroVector;
	FVector LaunchVelocity = FVector::ZeroVector;
	FVector ReleaseOwnerPosition = FVector::ZeroVector;
	FVector LastMotionPosition = FVector::ZeroVector;
	FHitResult ReleaseHit;
	FDublinImpact DestroyedImpact;
	double LastWorldTime = 0.0;
	double CaseStarted = 0.0;
	double DropStarted = 0.0;
	double LastMovementTime = 0.0;
	double MaxMovementGap = 0.0;
	int32 CaseIndex = 0;
	int32 CaseFrames = 0;
	int32 Phase = 0;
	int32 MotionSegments = 0;
	int32 FailedCases = 0;
	bool bReady = false;
	bool bCaseActive = false;
	bool bDestroyed = false;
	bool bResolvedAtDestroy = false;
	bool bReleaseBlocked = false;
	bool bKeyHeld = false;
	bool bHasTerrainAnchor = false;
	bool bHaveMotionPosition = false;
	bool bFinished = false;
	bool bCleaned = false;
	bool bSessionEnding = false;

	static void Point(FJsonObject& Object, const TCHAR* Name, const FVector& Value)
	{
		if (Value.ContainsNaN()) { Object.SetStringField(Name, TEXT("nonfinite")); return; }
		TArray<TSharedPtr<FJsonValue>> Values;
		for (double Coordinate : { Value.X, Value.Y, Value.Z }) { Values.Add(MakeShared<FJsonValueNumber>(Coordinate)); }
		Object.SetArrayField(Name, Values);
	}
	static TSharedRef<FJsonObject> DescribeHit(const FHitResult& Hit)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		UPrimitiveComponent* Component = Hit.GetComponent();
		Out->SetStringField(TEXT("actor"), GetNameSafe(Hit.GetActor()));
		Out->SetStringField(TEXT("component"), GetNameSafe(Component));
		Out->SetBoolField(TEXT("blocking"), Hit.bBlockingHit);
		Out->SetBoolField(TEXT("start_penetrating"), Hit.bStartPenetrating);
		Out->SetNumberField(TEXT("face_index"), Hit.FaceIndex);
		Out->SetNumberField(TEXT("hit_fraction"), Hit.Time);
		Point(*Out, TEXT("impact_cm"), Hit.ImpactPoint);
		Point(*Out, TEXT("normal"), Hit.ImpactNormal);
		if (Component)
		{
			Out->SetNumberField(TEXT("collision_enabled"), static_cast<int32>(Component->GetCollisionEnabled()));
			Out->SetNumberField(TEXT("object_channel"), static_cast<int32>(Component->GetCollisionObjectType()));
			Out->SetNumberField(TEXT("response_to_bomb"), static_cast<int32>(Component->GetCollisionResponseToChannel(ECC_WorldDynamic)));
			Out->SetBoolField(TEXT("registered"), Component->IsRegistered());
			Out->SetBoolField(TEXT("physics_state_created"), Component->IsPhysicsStateCreated());
			if (const UBodySetup* Body = Component->GetBodySetup())
			{
				Out->SetBoolField(TEXT("double_sided_geometry"), Body->bDoubleSidedGeometry);
				Out->SetBoolField(TEXT("physics_meshes_created"), Body->bCreatedPhysicsMeshes);
				Out->SetBoolField(TEXT("physics_mesh_cook_failed"), Body->bFailedToCreatePhysicsMeshes);
			}
			if (const UProceduralMeshComponent* Mesh = Cast<UProceduralMeshComponent>(Component))
			{
				Out->SetBoolField(TEXT("async_cooking"), Mesh->bUseAsyncCooking);
				Out->SetBoolField(TEXT("complex_as_simple"), Mesh->bUseComplexAsSimpleCollision);
			}
		}
		return Out;
	}
	TSharedRef<FJsonObject> Snapshot(const ADublinProjectile& Projectile) const
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Point(*Out, TEXT("position_cm"), Projectile.GetActorLocation());
		Point(*Out, TEXT("launch_cm"), Projectile.GetLaunchPosition());
		Point(*Out, TEXT("launch_owner_cm"), Projectile.GetLaunchOwnerPosition());
		Point(*Out, TEXT("velocity_cm_s"), Projectile.Movement->Velocity);
		Out->SetStringField(TEXT("owner"), GetNameSafe(Projectile.GetOwner()));
		Out->SetStringField(TEXT("updated_component"), GetNameSafe(Projectile.Movement->UpdatedComponent.Get()));
		Out->SetNumberField(TEXT("impact_kind"), static_cast<int32>(Projectile.GetImpactKind()));
		Out->SetNumberField(TEXT("collision_enabled"), static_cast<int32>(Projectile.CollisionRoot->GetCollisionEnabled()));
		Out->SetNumberField(TEXT("response_world_static"), static_cast<int32>(Projectile.CollisionRoot->GetCollisionResponseToChannel(ECC_WorldStatic)));
		Out->SetNumberField(TEXT("response_world_dynamic"), static_cast<int32>(Projectile.CollisionRoot->GetCollisionResponseToChannel(ECC_WorldDynamic)));
		Out->SetNumberField(TEXT("response_physics_body"), static_cast<int32>(Projectile.CollisionRoot->GetCollisionResponseToChannel(ECC_PhysicsBody)));
		Out->SetBoolField(TEXT("resolved"), Projectile.HasResolvedImpact());
		Out->SetBoolField(TEXT("movement_active"), Projectile.Movement->IsActive());
		Out->SetBoolField(TEXT("stop_delegate_bound"), Projectile.Movement->OnProjectileStop.IsBound());
		Out->SetBoolField(TEXT("swept"), Projectile.Movement->bSweepCollision);
		Out->SetBoolField(TEXT("forced_substeps"), Projectile.Movement->bForceSubStepping);
		Out->SetBoolField(TEXT("sphere_simulates_physics"), Projectile.CollisionRoot->IsSimulatingPhysics());
		Out->SetBoolField(TEXT("movement_tick_enabled"), Projectile.Movement->IsComponentTickEnabled());
		Out->SetNumberField(TEXT("movement_tick_interval_seconds"), Projectile.Movement->GetComponentTickInterval());
		Out->SetBoolField(TEXT("scoped_simulation"), Projectile.Movement->bSimulationUseScopedMovement);
		Out->SetNumberField(TEXT("radius_cm"), Projectile.CollisionRoot->GetScaledSphereRadius());
		Out->SetNumberField(TEXT("max_step_seconds"), Projectile.Movement->MaxSimulationTimeStep);
		Out->SetNumberField(TEXT("max_iterations"), Projectile.Movement->MaxSimulationIterations);
		Out->SetNumberField(TEXT("world_delta_seconds"), World.IsValid() ? World->GetDeltaSeconds() : 0.0);
		Out->SetNumberField(TEXT("lifespan_remaining_seconds"), Projectile.GetLifeSpan());
		float WaterZ = 0.0f;
		Out->SetBoolField(TEXT("water_query_available"), City.IsValid());
		const bool bWater = City.IsValid() && City->GetWaterSurfaceZ(Projectile.GetActorLocation(), WaterZ);
		Out->SetBoolField(TEXT("source_water_at_xy"), bWater);
		if (bWater) { Out->SetNumberField(TEXT("water_surface_z_cm"), WaterZ); }
		return Out;
	}
	bool Probe(const FVector& From, const FVector& To, const ADublinProjectile* Projectile, FHitResult& Hit) const
	{
		const USphereComponent* Shape = Projectile ? Projectile->CollisionRoot.Get()
			: GetDefault<ADublinProjectile>()->CollisionRoot.Get();
		FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBombCollisionWitness), Shape->bTraceComplexOnMove, Projectile);
		Query.bReturnFaceIndex = true;
		if (FixturePawn.IsValid()) { Query.AddIgnoredActor(FixturePawn.Get()); }
		return World->SweepSingleByChannel(Hit, From, To, FQuat::Identity, Shape->GetCollisionObjectType(),
			FCollisionShape::MakeSphere(Projectile ? Shape->GetScaledSphereRadius() : 20.0f), Query,
			FCollisionResponseParams(Shape->GetCollisionResponseToChannels()));
	}
	bool SurfaceAt(const FVector& PointToProbe, FHitResult& Hit) const
	{
		FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBombSurfaceSelection), false, OriginalPawn.Get());
		Query.bReturnFaceIndex = true;
		if (FixturePawn.IsValid()) { Query.AddIgnoredActor(FixturePawn.Get()); }
		if (Bomb.IsValid()) { Query.AddIgnoredActor(Bomb.Get()); }
		return World->LineTraceSingleByChannel(Hit, PointToProbe + FVector(0, 0, 20000),
			PointToProbe - FVector(0, 0, 10000), ECC_Visibility, Query);
	}
	bool ConfigureSurface(const FVector& PointToProbe, ESurface Kind, FHitResult& OutHit)
	{
		if (!SurfaceAt(PointToProbe, OutHit) || OutHit.GetActor() != City.Get() || !OutHit.GetComponent()) { return false; }
		const bool bTerrain = Kind == ESurface::Terrain || Kind == ESurface::Shore;
		if (!OutHit.GetComponent()->GetName().StartsWith(bTerrain ? TEXT("DublinTerrain_") : TEXT("DublinBuildings_"))) { return false; }
		float WaterZ = 0.0f;
		if (bTerrain && City->GetWaterSurfaceZ(OutHit.ImpactPoint, WaterZ)) { return false; }
		const FCase& Spec = Cases[CaseIndex];
		const double Height = Spec.bLow ? -40.0 : 1000.0;
		TArray<FVector> Directions = { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) };
		if (Kind == ESurface::Shore)
		{
			Directions.RemoveAll([&](const FVector& Direction)
			{
				return !City->GetWaterSurfaceZ(OutHit.ImpactPoint + Direction * 200.0, WaterZ)
					|| !City->GetWaterSurfaceZ(OutHit.ImpactPoint + Direction * 800.0, WaterZ);
			});
		}
		for (const FVector& Direction : Directions)
		{
			LaunchVelocity = Direction * Spec.SpeedCm;
			const double FallSeconds = Spec.bLow || Kind == ESurface::Shore ? 0.0
				: FMath::Sqrt(2.0 * (Height - 20.0) / -World->GetGravityZ());
			LaunchPosition = OutHit.ImpactPoint + FVector(0, 0, Height) - LaunchVelocity * FallSeconds;
			ReleaseOwnerPosition = LaunchPosition + FVector(0, 0, 540.0);
			FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBombOwnerClearance), false, OriginalPawn.Get());
			FHitResult Above;
			if (World->OverlapBlockingTestByChannel(ReleaseOwnerPosition, FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeSphere(472.0f), Query)
				|| World->LineTraceSingleByChannel(Above, ReleaseOwnerPosition + FVector(0, 0, 30000),
					ReleaseOwnerPosition, ECC_Visibility, Query)) { continue; }
			return true;
		}
		return false;
	}
	bool SelectSurface(FHitResult& Hit)
	{
		const FCase& Spec = Cases[CaseIndex];
		if (Spec.Surface == ESurface::Terrain && Spec.bNeedsCrater && bHasTerrainAnchor)
		{
			if (Spec.bLow && Spec.bNeedsCrater)
			{
				const double SearchRadius = MakeGroundImpact(MakeImpact(EDublinImpactKind::Bomb, 1.0f, FBombCurve())).RadiusCm;
				for (int32 Ring = 0; Ring <= FMath::CeilToInt(SearchRadius / 25.0); ++Ring)
				{
					const double Radius = Ring * 25.0;
					for (int32 Direction = 0; Direction < 16; ++Direction)
					{
						const double Angle = Direction * PI / 8.0;
						const FVector PointToProbe = TerrainAnchor + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;
						if (ConfigureSurface(PointToProbe, Spec.Surface, Hit) && Hit.ImpactPoint.Z < TerrainAnchor.Z - 20.0)
						{
							return true;
						}
					}
				}
				return false;
			}
			return ConfigureSurface(TerrainAnchor, Spec.Surface, Hit);
		}
		if (Spec.Surface == ESurface::Terrain || Spec.Surface == ESurface::Shore)
		{
			for (const FDublinCityChunk& Chunk : City->GetTerrainChunks())
			{
				if (Chunk.Sections.IsEmpty()) { continue; }
				const FDublinCitySection& Section = Chunk.Sections[0];
				for (int32 Index = 0; Index + 2 < Section.Triangles.Num(); Index += 12)
				{
					const FVector Position = Chunk.OriginCm + (Section.Vertices[Section.Triangles[Index]]
						+ Section.Vertices[Section.Triangles[Index + 1]] + Section.Vertices[Section.Triangles[Index + 2]]) / 3.0;
					if (FMath::Abs(Position.X) > 24000 || FMath::Abs(Position.Y) > 24000) { continue; }
					if (Spec.Surface == ESurface::Terrain && UsedTerrainSurfaces.ContainsByPredicate([&](const FVector& Used)
						{ return FVector::DistSquaredXY(Position, Used) < FMath::Square(2000.0); })) { continue; }
					if (ConfigureSurface(Position, Spec.Surface, Hit))
					{
						if (Spec.Surface == ESurface::Terrain && !bHasTerrainAnchor && !bLowRelease)
						{
							// Later cases need a low aircraft pose, not just clearance above a nearby roof.
							const double Radius = MakeGroundImpact(MakeImpact(EDublinImpactKind::Bomb, 1.0f, FBombCurve())).RadiusCm + 472.0;
							TArray<FOverlapResult> Nearby;
							FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinCraterFixtureClearance), false, OriginalPawn.Get());
							World->OverlapMultiByChannel(Nearby, Hit.ImpactPoint + FVector(0, 0, 1000),
								FQuat::Identity, ECC_Pawn, FCollisionShape::MakeBox(FVector(Radius, Radius, 2500)), Query);
							if (Nearby.ContainsByPredicate([](const FOverlapResult& Overlap)
								{
									const UPrimitiveComponent* Component = Overlap.GetComponent();
									return Overlap.bBlockingHit && (!Component || !Component->GetName().StartsWith(TEXT("DublinTerrain_")));
								})) { continue; }
						}
						if (Spec.Surface == ESurface::Terrain)
						{
							if (!bHasTerrainAnchor)
							{
								TerrainAnchor = Hit.ImpactPoint;
								bHasTerrainAnchor = true;
							}
							UsedTerrainSurfaces.Add(Hit.ImpactPoint);
						}
						return true;
					}
				}
			}
		}
		else
		{
			for (const FDublinCityBuilding& Building : City->GetSourceData()->Buildings)
			{
				const bool bBridge = Building.Id.StartsWith(TEXT("bridge/")) || Building.Name.Contains(TEXT("bridge"), ESearchCase::IgnoreCase);
				if (bBridge != (Spec.Surface == ESurface::Bridge)) { continue; }
				for (int32 Index = 0; Index < Building.MaterialIds.Num(); ++Index)
				{
					if (Building.MaterialIds[Index] != 0) { continue; }
					const FVector Position = Building.PivotCm + (Building.Mesh.VerticesCm[Building.Mesh.Triangles[Index * 3]]
						+ Building.Mesh.VerticesCm[Building.Mesh.Triangles[Index * 3 + 1]]
						+ Building.Mesh.VerticesCm[Building.Mesh.Triangles[Index * 3 + 2]]) / 3.0;
					if (ConfigureSurface(Position, Spec.Surface, Hit) && Hit.ImpactPoint.Equals(Position, 10.0)) { return true; }
				}
			}
		}
		return false;
	}
	void Watch(ADublinProjectile* Projectile)
	{
		if (Bomb.IsValid() || bDestroyed) { return; }
		Bomb = Projectile;
		ObservedRoot = Projectile->CollisionRoot;
		TransformHandle = ObservedRoot->TransformUpdated.AddRaw(this, &FBombCollisionWitness::Moved);
	}
	void Spawned(AActor* Actor)
	{
		if (bCaseActive && FixturePawn.IsValid() && Actor->GetOwner() == FixturePawn.Get())
		{
			if (ADublinProjectile* Projectile = Cast<ADublinProjectile>(Actor)) { Watch(Projectile); }
		}
	}
	void Moved(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport)
	{
		if (!bCaseActive || !Bomb.IsValid() || !Bomb->HasActorBegunPlay() || Bomb->GetImpactKind() != EDublinImpactKind::Bomb) { return; }
		const FVector From = bHaveMotionPosition ? LastMotionPosition : Bomb->GetLaunchPosition();
		const FVector To = Component->GetComponentLocation();
		LastMotionPosition = To;
		bHaveMotionPosition = true;
		if (From.Equals(To, 0.000001)) { return; }
		++MotionSegments;
		const double Now = World->GetTimeSeconds();
		if (Now > LastMovementTime) { MaxMovementGap = FMath::Max(MaxMovementGap, Now - LastMovementTime); LastMovementTime = Now; }
		const FVector Direction = (To - From).GetSafeNormal();
		FHitResult Hit;
		if (NearSurfaceSegments.Num() < 64 && FMath::Min(From.Z, To.Z) <= DropSurfaceZ + 100
			&& FMath::Max(From.Z, To.Z) >= DropSurfaceZ - 100)
		{
			TSharedRef<FJsonObject> Segment = MakeShared<FJsonObject>();
			Point(*Segment, TEXT("from_cm"), From);
			Point(*Segment, TEXT("to_cm"), To);
			Segment->SetBoolField(TEXT("sphere_hit"), Probe(From, To, Bomb.Get(), Hit));
			FCollisionQueryParams Query(SCENE_QUERY_STAT(DublinBombCenterTrajectoryWitness), false, Bomb.Get());
			Query.AddIgnoredActor(FixturePawn.Get());
			Segment->SetBoolField(TEXT("center_ray_hit"), World->LineTraceSingleByChannel(Hit, From, To,
				ECC_WorldDynamic, Query, FCollisionResponseParams(Bomb->CollisionRoot->GetCollisionResponseToChannels())));
			NearSurfaceSegments.Add(MakeShared<FJsonValueObject>(Segment));
		}
		// Witness the actual native substep segment; these queries never move or resolve the projectile.
		if (Probe(From, To, Bomb.Get(), Hit) && !Hit.bStartPenetrating
			&& FVector::DotProduct(To - FVector(Hit.Location), Direction) > 1.0 && !MissEvidence)
		{
			MissEvidence = DescribeHit(Hit);
			Point(*MissEvidence, TEXT("native_segment_from_cm"), From);
			Point(*MissEvidence, TEXT("native_segment_to_cm"), To);
		}
		// Native sweeps pull back from contact slightly; this extended query is evidence only on actual resolution.
		if (Probe(From, To + Direction * 2.0, Bomb.Get(), Hit))
		{
			ContactEvidence = DescribeHit(Hit);
			Point(*ContactEvidence, TEXT("native_segment_from_cm"), From);
			Point(*ContactEvidence, TEXT("native_segment_to_cm"), To);
		}
	}
	void Destroyed(AActor* Actor)
	{
		ADublinProjectile* Projectile = Cast<ADublinProjectile>(Actor);
		if (!bCaseActive || !FixturePawn.IsValid() || !Projectile || Projectile->GetOwner() != FixturePawn.Get()) { return; }
		bDestroyed = true;
		bResolvedAtDestroy = Projectile->HasResolvedImpact();
		DestroyedImpact = FixturePawn->Weapons->GetLastImpact();
		CaseEvidence->SetObjectField(TEXT("at_native_destroy_event"), Snapshot(*Projectile));
		CaseEvidence->SetBoolField(TEXT("resolved_before_destroy"), bResolvedAtDestroy);
		CaseEvidence->SetBoolField(TEXT("impact_water"), DestroyedImpact.bWater);
		CaseEvidence->SetNumberField(TEXT("impact_seed"), DestroyedImpact.Seed);
		Point(*CaseEvidence, TEXT("dispatched_impact_cm"), DestroyedImpact.PositionCm);
		Projectile->CollisionRoot->TransformUpdated.Remove(TransformHandle);
		TransformHandle.Reset();
	}
	void Send(const FKey& Key, EInputEvent Event)
	{
		if (!World.IsValid() || !Player.IsValid() || !Key.IsValid()) { return; }
		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, Event, Event == IE_Pressed ? 1.0f : 0.0f, 1, Device);
		Args.DeltaTime = World->GetDeltaSeconds();
		Player->InputKey(Args);
	}
	bool Bootstrap()
	{
		if (!GEngine) { Test.AddError(TEXT("No engine for native bomb fixture.")); return Finish(true); }
		int32 PIEWorlds = 0;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() && Context.World()->WorldType == EWorldType::PIE && !Context.World()->bIsTearingDown) { ++PIEWorlds; }
		}
		if (PIEWorlds > 1)
		{
			Test.AddError(TEXT("Bomb collision fixture requires one standalone PIE world, not a multi-client session."));
			return Finish(true);
		}
		BootstrapStatus = FString::Printf(TEXT("PIE worlds=%d"), PIEWorlds);
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (!Candidate || Candidate->WorldType != EWorldType::PIE || Candidate->bIsTearingDown) { continue; }
			if (UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) != TEXT("/Game/Maps/Dublin"))
			{
				Test.AddError(TEXT("Bomb collision fixture requires actual Dublin PIE.")); return Finish(true);
			}
			APlayerController* PC = Candidate->GetFirstPlayerController();
			ADublinFlightPawn* Pawn = PC ? Cast<ADublinFlightPawn>(PC->GetPawn()) : nullptr;
			BootstrapStatus = FString::Printf(TEXT("player=%d local=%d input=%d pawn=%d weapons=%d spawnCaptured=%d worldSeconds=%.3f"),
				PC != nullptr, PC && PC->IsLocalController(), PC && PC->PlayerInput,
				Pawn != nullptr, Pawn && Pawn->Weapons, Pawn && Pawn->bSpawnCaptured, Candidate->GetTimeSeconds());
			if (!Pawn || !Pawn->Weapons || !Pawn->bSpawnCaptured || !PC->IsLocalController() || !PC->PlayerInput) { continue; }
			ADublinCityWorld* Found = Pawn->Weapons->GetCity();
			BootstrapStatus = FString::Printf(TEXT("city=%d ready=%d destructionReady=%d source=%d"),
				Found != nullptr, Found && Found->bCityReady, Found && Found->bDestructionReady, Found && Found->GetSourceData());
			if (!Found || !Found->bCityReady || !Found->bDestructionReady || !Found->GetSourceData()) { continue; }
			UGameViewportClient* Viewport = Candidate->GetGameViewport();
			BootstrapStatus = FString::Printf(TEXT("viewportClient=%d viewport=%d"), Viewport != nullptr, Viewport && Viewport->Viewport);
			if (!Viewport || !Viewport->Viewport) { continue; }
			if (bInputCoverage && !CaptureViewport(Candidate, PC))
			{
				BootstrapStatus = FString::Printf(TEXT("native input focus=%d mouseCapture=%d local=%d"),
					Viewport->Viewport->HasFocus(), Viewport->Viewport->HasMouseCapture(), PC->IsLocalController());
				continue;
			}
			World = Candidate; Player = PC; OriginalPawn = Pawn; City = Found;
			if (City->AcceptedImpactCount != 0 || Pawn->Weapons->GetActiveProjectileCount() != 0
				|| !City->GetActorTransform().Equals(FTransform::Identity, 0.01) || World->GetGravityZ() >= 0)
			{
				Test.AddError(TEXT("Use fresh, gravity-enabled Dublin PIE with an unmodified identity-transform city and no prior projectiles/impacts."));
				return Finish(true);
			}
			Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PC->GetPlatformUserId());
			if (Device == INPUTDEVICEID_NONE) { Test.AddError(TEXT("Bomb fixture has no native input device.")); return Finish(true); }
			PC->FlushPressedKeys();
			Pawn->Weapons->SuppressInput();
			if (!Pawn->IsGodMode()) { Pawn->ToggleGodMode(); }
			SpawnHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateRaw(this, &FBombCollisionWitness::Spawned));
			DestroyHandle = World->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateRaw(this, &FBombCollisionWitness::Destroyed));
			FEditorDelegates::PrePIEEnded.AddRaw(this, &FBombCollisionWitness::PIEEnding);
			if (bLowRelease)
			{
				Cases = {
					{TEXT("actual_B_low_dry_land"), ESurface::Terrain, EKeys::B, 0.0, true, false, 0.0f},
					{TEXT("actual_RMB_low_dry_land"), ESurface::Terrain, EKeys::RightMouseButton, 0.0, true, false, 0.0f},
					{TEXT("native_low_land_5mps"), ESurface::Terrain, FKey(), 500.0, true, false, 0.0f},
					{TEXT("native_low_land_100mps"), ESurface::Terrain, FKey(), 10000.0, true, false, 0.0f}
				};
			}
			else
			{
				Cases = {
					{TEXT("first_ready_terrain_drop"), ESurface::Terrain, FKey(), 0.0, false, false, 0.0f},
					{TEXT("actual_RMB_low_changed_crater"), ESurface::Terrain, EKeys::RightMouseButton, 0.0, true, true, 0.0f},
					{TEXT("changed_crater_drop"), ESurface::Terrain, FKey(), 0.0, false, true, 0.0f},
					{TEXT("terrain_5mps"), ESurface::Terrain, FKey(), 500.0, false, true, 0.0f},
					{TEXT("terrain_40mps"), ESurface::Terrain, FKey(), 4000.0, false, false, 0.0f},
					{TEXT("terrain_100mps"), ESurface::Terrain, FKey(), 10000.0, false, false, 0.0f},
					{TEXT("terrain_100mps_native_400ms_tick"), ESurface::Terrain, FKey(), 10000.0, false, false, 0.4f},
					{TEXT("real_building_roof_drop"), ESurface::Roof, FKey(), 0.0, false, false, 0.0f},
					{TEXT("real_bridge_deck_drop"), ESurface::Bridge, FKey(), 0.0, false, false, 0.0f},
					{TEXT("dry_to_water_shore_5mps"), ESurface::Shore, FKey(), 500.0, false, false, 0.0f}
				};
			}
			if (!bInputCoverage)
			{
				Cases.RemoveAll([](const FCase& Spec) { return Spec.Key.IsValid(); });
			}
			bReady = true;
			BootstrapStatus = TEXT("Ready");
			LastWorldTime = World->GetTimeSeconds();
			Test.AddInfo(TEXT("Collision proof uses genuine B/RMB releases or public native Initialize and elapsed world ticks. "
				"Queries only observe real colliders/actual movement segments; no manual movement Tick, Resolve, DispatchImpact or synthetic floor."));
			return false;
		}
		return false;
	}
	bool BeginCase()
	{
		const FCase& Spec = Cases[CaseIndex];
		CaseEvidence = MakeShared<FJsonObject>();
		CaseEvidence->SetStringField(TEXT("case"), Spec.Name);
		CaseEvidence->SetStringField(TEXT("launch_route"), Spec.Key.IsValid() ? Spec.Key.ToString() : TEXT("public_native_Initialize_velocity_fixture"));
		CaseEvidence->SetBoolField(TEXT("requires_input_capture"), Spec.Key.IsValid());
		CaseEvidence->SetNumberField(TEXT("movement_tick_interval_seconds"), Spec.MovementInterval);
		ContactEvidence.Reset(); MissEvidence.Reset();
		NearSurfaceSegments.Reset();
		bDestroyed = bResolvedAtDestroy = bHaveMotionPosition = false;
		MaxMovementGap = 0; MotionSegments = 0; CaseFrames = 0; Phase = 0;
		FHitResult Surface;
		if (!SelectSurface(Surface))
		{
			Test.AddError(Spec.Name + TEXT(": fixture has no live supported surface/clear owner pose; this is NOT a proven projectile miss."));
			CaseEvidence->SetStringField(TEXT("result"), TEXT("fixture_source_or_collision_surface_unavailable"));
			CaseEvidence->SetBoolField(TEXT("passed"), false);
			Results.Add(MakeShared<FJsonValueObject>(CaseEvidence));
			++FailedCases;
			++CaseIndex;
			return true;
		}
		if (Spec.bNeedsCrater && (City->CraterChangedNodeCount <= 0 || Surface.ImpactPoint.Z >= TerrainAnchor.Z - 20.0))
		{
			Test.AddError(Spec.Name + TEXT(": prior real projectile did not produce a live collision-backed changed crater."));
			CaseEvidence->SetStringField(TEXT("result"), TEXT("fixture_crater_not_proven"));
			CaseEvidence->SetBoolField(TEXT("passed"), false);
			Results.Add(MakeShared<FJsonValueObject>(CaseEvidence));
			++FailedCases;
			++CaseIndex;
			return true;
		}
		CaseEvidence->SetObjectField(TEXT("live_surface_before_drop"), DescribeHit(Surface));
		DropSurfaceZ = Surface.ImpactPoint.Z;
		FCollisionQueryParams ReverseQuery(SCENE_QUERY_STAT(DublinBombReverseSurfaceProbe), false, OriginalPawn.Get());
		FHitResult ReverseHit;
		const bool bReverseBlocks = World->LineTraceSingleByChannel(ReverseHit, Surface.ImpactPoint - FVector(0, 0, 100),
			Surface.ImpactPoint + FVector(0, 0, 100), ECC_Visibility, ReverseQuery);
		CaseEvidence->SetBoolField(TEXT("reverse_surface_trace_blocks"), bReverseBlocks);
		if (bReverseBlocks) { CaseEvidence->SetObjectField(TEXT("reverse_surface_trace"), DescribeHit(ReverseHit)); }
		Point(*CaseEvidence, TEXT("requested_launch_cm"), LaunchPosition);
		Point(*CaseEvidence, TEXT("requested_initial_velocity_cm_s"), LaunchVelocity);
		Point(*CaseEvidence, TEXT("owner_cm"), ReleaseOwnerPosition);
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		bBorrowedOriginalPawn = Spec.Key.IsValid();
		if (bBorrowedOriginalPawn)
		{
			FixturePawn = OriginalPawn;
			OriginalPawnTransform = FixturePawn->GetActorTransform();
			bOriginalPawnTickEnabled = FixturePawn->IsActorTickEnabled();
			// Aircraft positioning is fixture setup, not the projectile's movement or collision path.
			FixturePawn->SetActorTickEnabled(false);
			FixturePawn->SetActorLocationAndRotation(ReleaseOwnerPosition, FRotator::ZeroRotator,
				false, nullptr, ETeleportType::TeleportPhysics);
		}
		else
		{
			FixturePawn = World->SpawnActor<ADublinFlightPawn>(ADublinFlightPawn::StaticClass(), ReleaseOwnerPosition,
				FRotator::ZeroRotator, Params);
		}
		if (!FixturePawn.IsValid() || FixturePawn->bResetBlocked)
		{
			Test.AddError(Spec.Name + TEXT(": native aircraft fixture does not have a clear real collision pose."));
			CaseEvidence->SetStringField(TEXT("result"), TEXT("fixture_native_owner_pose_blocked"));
			CaseEvidence->SetBoolField(TEXT("passed"), false);
			Results.Add(MakeShared<FJsonValueObject>(CaseEvidence));
			++FailedCases;
			return false;
		}
		if (!FixturePawn->IsGodMode()) { FixturePawn->ToggleGodMode(); }
		BombsBeforeCase = FixturePawn->Weapons->BombsDropped;
		DispatchesBeforeCase = FixturePawn->Weapons->AcceptedImpacts + FixturePawn->Weapons->RejectedImpacts;
		bReleaseBlocked = Probe(ReleaseOwnerPosition, LaunchPosition, nullptr, ReleaseHit);
		CaseEvidence->SetBoolField(TEXT("release_segment_blocked_before_drop"), bReleaseBlocked);
		if (bReleaseBlocked) { CaseEvidence->SetObjectField(TEXT("live_release_blocker_before_drop"), DescribeHit(ReleaseHit)); }
		CaseEvidence->SetBoolField(TEXT("native_owner_pose_clear"), !FixturePawn->bResetBlocked);
		bCaseActive = true;
		CaseStarted = LastMovementTime = World->GetTimeSeconds();
		if (Spec.bLow && !bReleaseBlocked) { EndCase(false, TEXT("fixture_low_release_has_no_live_blocker")); }
		return true;
	}
	void EndCase(bool bPassed, const TCHAR* Classification)
	{
		CaseEvidence->SetStringField(TEXT("result"), Classification);
		CaseEvidence->SetBoolField(TEXT("passed"), bPassed);
		CaseEvidence->SetNumberField(TEXT("observed_world_frames"), CaseFrames);
		CaseEvidence->SetNumberField(TEXT("actual_motion_segments"), MotionSegments);
		CaseEvidence->SetArrayField(TEXT("near_surface_native_segments"), NearSurfaceSegments);
		CaseEvidence->SetNumberField(TEXT("maximum_observed_movement_gap_seconds"), MaxMovementGap);
		CaseEvidence->SetNumberField(TEXT("elapsed_drop_world_seconds"), World->GetTimeSeconds() - DropStarted);
		if (MissEvidence) { CaseEvidence->SetObjectField(TEXT("exact_native_motion_crossed_live_blocker"), MissEvidence); }
		if (FixturePawn.IsValid())
		{
			CaseEvidence->SetNumberField(TEXT("released_bombs"), FixturePawn->Weapons->BombsDropped);
			CaseEvidence->SetNumberField(TEXT("registered_live_bombs"), FixturePawn->Weapons->GetActiveBombs().Num());
			CaseEvidence->SetNumberField(TEXT("accepted_impacts"), FixturePawn->Weapons->AcceptedImpacts);
			CaseEvidence->SetNumberField(TEXT("rejected_impacts"), FixturePawn->Weapons->RejectedImpacts);
			CaseEvidence->SetStringField(TEXT("dispatch_status"), FixturePawn->Weapons->LastFailure);
		}
		Results.Add(MakeShared<FJsonValueObject>(CaseEvidence));
		if (!bPassed) { ++FailedCases; Test.AddError(Cases[CaseIndex].Name + TEXT(": ") + Classification); }
		else { Test.AddInfo(Cases[CaseIndex].Name + TEXT(": ") + Classification + TEXT(" (collision/lifecycle only, not visual or destruction acceptance)")); }
		ClearCase();
		NextCaseAfterWorldTime = World->GetTimeSeconds() + 0.25;
		++CaseIndex;
	}
	void ClearCase()
	{
		bCaseActive = false;
		if (bKeyHeld) { Send(Cases[CaseIndex].Key, IE_Released); bKeyHeld = false; }
		if (ObservedRoot.IsValid()) { ObservedRoot->TransformUpdated.Remove(TransformHandle); }
		TransformHandle.Reset(); ObservedRoot.Reset();
		if (Bomb.IsValid()) { Bomb->Destroy(); }
		Bomb.Reset();
		if (FixturePawn.IsValid())
		{
			if (bBorrowedOriginalPawn)
			{
				FixturePawn->SetActorTransform(OriginalPawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
				FixturePawn->SetActorTickEnabled(bOriginalPawnTickEnabled);
				FixturePawn.Reset();
				bBorrowedOriginalPawn = false;
				return;
			}
			if (!bSessionEnding && World.IsValid() && !World->bIsTearingDown && Player.IsValid() && OriginalPawn.IsValid()
				&& Player->GetPawn() == FixturePawn.Get())
			{
				Player->Possess(OriginalPawn.Get());
			}
			FixturePawn->Destroy();
			FixturePawn.Reset();
		}
	}
	void Cleanup()
	{
		if (bCleaned) { return; }
		bCleaned = true;
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
		if (World.IsValid())
		{
			World->RemoveOnActorSpawnedHandler(SpawnHandle);
			World->RemoveOnActorDestroyedHandler(DestroyHandle);
		}
		ClearCase();
		if (!bSessionEnding && World.IsValid() && !World->bIsTearingDown && GEditor && GEditor->PlayWorld == World.Get())
		{
			GEditor->RequestEndPlayMap();
		}
	}
	void PIEEnding(bool bSimulating)
	{
		bSessionEnding = true;
		if (!bFinished)
		{
			Test.AddError(TEXT("Owned PIE ended before bomb collision cases completed."));
			Finish(true);
		}
	}
	bool Finish(bool bAborted = false)
	{
		if (bFinished) { return true; }
		bFinished = true;
		if (bCaseActive && CaseEvidence)
		{
			CaseEvidence->SetStringField(TEXT("result"), TEXT("fixture_interrupted_or_timed_out"));
			CaseEvidence->SetBoolField(TEXT("passed"), false);
			Results.Add(MakeShared<FJsonValueObject>(CaseEvidence));
			++FailedCases;
		}
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("status"), !bAborted && bReady && CaseIndex == Cases.Num() && FailedCases == 0
			? TEXT("collision_lifecycle_checks_passed") : TEXT("failed_or_incomplete"));
		Report->SetNumberField(TEXT("planned_cases"), Cases.Num());
		Report->SetNumberField(TEXT("completed_cases"), CaseIndex);
		Report->SetNumberField(TEXT("failed_cases"), FailedCases);
		Report->SetBoolField(TEXT("actual_button_input_in_scope"), bInputCoverage);
		Report->SetStringField(TEXT("bootstrap_status"), BootstrapStatus);
		Report->SetStringField(TEXT("suite"), bLowRelease ? TEXT("BombReleaseBelowLiveLand") : TEXT("BombLiveCollisionMatrix"));
		if (!bInputCoverage)
		{
			Report->SetStringField(TEXT("suite"), bLowRelease ? TEXT("BombNativeReleaseBelowLiveLand") : TEXT("BombNativeCollisionMatrix"));
		}
		Report->SetStringField(TEXT("scope"), TEXT("Actual native collision/lifecycle witnesses. River/source holes are not assumed to be land sweep failures."));
		Report->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedWall);
		Report->SetArrayField(TEXT("cases"), Results);
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("StructuralPilot-20260918"));
		const FString Path = FPaths::Combine(Directory, TEXT("bomb-collision-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
		FString Json;
		FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
		if (!IFileManager::Get().MakeDirectory(*Directory, true)
			|| !FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(TEXT("Could not save collision witness evidence: ") + Path);
		}
		else { Test.AddInfo(TEXT("Native collision witness evidence: ") + Path); }
		Cleanup();
		return true;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponActualPIETest, "DublinFlight.Weapons.PIE.ActualCannonBombInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinWeaponActualPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open real /Game/Maps/Dublin in the editor; this native fixture starts PIE after automation-session teardown."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinWeapons::PIETests::FRunWeapons(*this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponMultiBombPIETest, "DublinFlight.Weapons.PIE.AllBombMarkersActualInputLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinWeaponMultiBombPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open real /Game/Maps/Dublin in the editor; run this test separately so it starts its own fresh PIE session."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinWeapons::PIETests::FRunWeapons(*this, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBombLowReleaseCollisionPIETest, "DublinFlight.Weapons.PIE.BombReleaseBelowLiveLand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinBombLowReleaseCollisionPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin") || GEditor->PlayWorld)
	{
		AddError(TEXT("Open saved Dublin and run this collision test alone; it owns a fresh native PIE world."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinWeapons::PIETests::FBombCollisionWitness(*this, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBombLiveCollisionMatrixPIETest, "DublinFlight.Weapons.PIE.BombLiveCollisionMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinBombLiveCollisionMatrixPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin") || GEditor->PlayWorld)
	{
		AddError(TEXT("Open saved Dublin and run this collision matrix separately; existing destructive PIE cannot be reused."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinWeapons::PIETests::FBombCollisionWitness(*this, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBombNativeLowReleasePIETest, "DublinFlight.Weapons.PIE.BombNativeReleaseBelowLiveLand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinBombNativeLowReleasePIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin") || GEditor->PlayWorld)
	{
		AddError(TEXT("Open saved Dublin and run the native projectile test alone."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinWeapons::PIETests::FBombCollisionWitness(*this, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBombNativeMatrixPIETest, "DublinFlight.Weapons.PIE.BombNativeCollisionMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDublinBombNativeMatrixPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin") || GEditor->PlayWorld)
	{
		AddError(TEXT("Open saved Dublin and run the native collision matrix alone."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinWeapons::PIETests::FBombCollisionWitness(*this, false, false));
	return true;
}

#endif
