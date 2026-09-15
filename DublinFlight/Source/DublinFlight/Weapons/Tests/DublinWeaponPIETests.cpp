#include "Weapons/DublinWeaponComponent.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Camera/CameraTypes.h"
#include "City/DublinCityWorld.h"
#include "Components/SphereComponent.h"
#include "DublinFlightPawn.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "Weapons/DublinProjectile.h"

namespace DublinWeapons::PIETests
{
class FRunWeapons final : public IAutomationLatentCommand
{
public:
	explicit FRunWeapons(FAutomationTestBase& InTest) : Test(InTest) {}
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

	void Require(const TCHAR* What, bool Condition) { bFailed |= !Test.TestTrue(What, Condition); }
	bool Capture()
	{
		UGameViewportClient* Viewport = World.IsValid() ? World->GetGameViewport() : nullptr;
		if (!Player.IsValid() || !Viewport || !Viewport->Viewport) { return false; }
		if (!Viewport->Viewport->HasFocus() || !Viewport->Viewport->HasMouseCapture())
		{
			Player->SetInputMode(FInputModeGameOnly());
			Viewport->Viewport->SetUserFocus(true);
			Viewport->Viewport->CaptureMouse(true);
		}
		return Viewport->Viewport->HasFocus() && Viewport->Viewport->HasMouseCapture();
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
			Require(TEXT("God bomb is a live actor"), IsValid(Bomb));
			if (Bomb) { Require(TEXT("God bomb inherits zero aircraft velocity"), Bomb->GetLaunchVelocity().IsNearlyZero(0.001)); }
			Release(EKeys::RightMouseButton);
		});
		Add(TEXT("God bomb falls to an accepted city impact"), 0.5, {}, [this]()
		{
			Require(TEXT("Ballistic bomb accepted by city"), Weapon->AcceptedBombImpacts > BombImpactBaseline);
			if (Weapon->RejectedImpacts > 0) { Test.AddError(FString::Printf(TEXT("City rejected weapon impacts: %s"), *Weapon->LastFailure)); }
			Require(TEXT("Active projectiles stay bounded"), Weapon->GetActiveProjectileCount() <= Weapon->MaximumActiveProjectiles);
			Require(TEXT("Effects only requested for accepted impacts"), Weapon->EffectsRequests <= Weapon->AcceptedImpacts);
		}, {}, [this]() { return Weapon->AcceptedBombImpacts > BombImpactBaseline || Weapon->RejectedImpacts > 0; });
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

#endif
