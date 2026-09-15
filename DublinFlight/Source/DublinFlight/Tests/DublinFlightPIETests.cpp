#include "DublinFlightPawn.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "DublinFlightHUD.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

namespace DublinFlight::Tests
{
bool CaptureFlightPIEViewport(UWorld& World, APlayerController& Player)
{
	UGameViewportClient* Client = World.GetGameViewport();
	if (!Client || !Client->Viewport)
	{
		return false;
	}
	if (!Client->Viewport->HasFocus() || !Client->Viewport->HasMouseCapture())
	{
		Player.SetInputMode(FInputModeGameOnly());
		Client->Viewport->SetUserFocus(true);
		Client->Viewport->CaptureMouse(true);
	}
	return Client->Viewport->HasFocus() && Client->Viewport->HasMouseCapture();
}

// The fixture creates PIE after the automation controller has restarted its
// test session; acceptance still uses only real input and elapsed world frames.
class FActualPlayerInputScenario final : public IAutomationLatentCommand
{
public:
	FActualPlayerInputScenario(FAutomationTestBase& InTest, UWorld& InWorld, APlayerController& InPlayer,
		ADublinFlightPawn& InPlane)
		: Test(InTest), World(&InWorld), Player(&InPlayer), Plane(&InPlane)
	{
		FEditorDelegates::PrePIEEnded.AddRaw(this, &FActualPlayerInputScenario::OnPrePIEEnded);
	}

	virtual ~FActualPlayerInputScenario() override
	{
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
		Cleanup();
	}

	virtual bool Update() override
	{
		if (!World.IsValid() || !Player.IsValid() || !Plane.IsValid()
			|| bSessionEnding || World->bIsTearingDown
			|| World->WorldType != EWorldType::PIE || Player->GetPawn() != Plane.Get())
		{
			Test.AddError(TEXT("PIE ended or lost the possessed DublinFlightPawn during actual-input acceptance."));
			Cleanup();
			return true;
		}
		if (!CaptureFlightPIEViewport(*World.Get(), *Player.Get()))
		{
			Test.AddError(TEXT("Native PIE viewport focus/mouse capture could not be maintained during actual-input acceptance."));
			Cleanup();
			return true;
		}
		if (!bStarted)
		{
			bStarted = true;
			StartedAt = FPlatformTime::Seconds();
			LastWorldTime = World->GetTimeSeconds();
			Setup();
			if (bAbort)
			{
				Cleanup();
				return true;
			}
			BuildSteps();
			BeginStep();
			return false;
		}
		if (FPlatformTime::Seconds() - StartedAt > 120.0)
		{
			Test.AddError(FString::Printf(TEXT("PIE input scenario timed out at '%s'. Keep PIE unpaused and the viewport focused."),
				*Steps[StepIndex].Name));
			Cleanup();
			return true;
		}

		const double Now = World->GetTimeSeconds();
		if (Now <= LastWorldTime)
		{
			return false;
		}
		const double FrameSeconds = Now - LastWorldTime;
		LastWorldTime = Now;
		++StepFrames;
		const FVector Position = Plane->GetActorLocation();
		if (!FFlightSimulation::IsFinitePosition(Position)
			|| !FFlightSimulation::IsFiniteOrientation(Plane->GetActorQuat()))
		{
			Test.AddError(TEXT("Actual PIE input produced a nonfinite aircraft transform."));
			Cleanup();
			return true;
		}
		if (!Steps[StepIndex].bAllowsReset)
		{
			const double MaximumTravel = FFlightTuning::MaxSpeedCmPerSecond * FrameSeconds + 2.0;
			if (FVector::Distance(Position, PreviousPosition) > MaximumTravel)
			{
				Test.AddError(FString::Printf(TEXT("Unexpected teleport/collision-reset during '%s': %.2f cm in %.4f s."),
					*Steps[StepIndex].Name, FVector::Distance(Position, PreviousPosition), FrameSeconds));
				Cleanup();
				return true;
			}
		}
		PreviousPosition = Position;
		FStep& Step = Steps[StepIndex];
		if (Step.Observe)
		{
			Step.Observe();
		}
		// Both elapsed game time and separate world frames are required. No Tick() or
		// ProcessPlayerInput() calls are used to manufacture a passing result.
		if (StepFrames < 2 || Now - StepStartedAt < Step.Seconds)
		{
			return false;
		}
		if (Step.Finish)
		{
			Step.Finish();
		}
		if (bAbort)
		{
			Cleanup();
			return true;
		}
		Test.AddInfo(FString::Printf(TEXT("PIE input phase completed: %s (%.3f s, %d world frames)"),
			*Step.Name, Now - StepStartedAt, StepFrames));
		++StepIndex;
		if (StepIndex == Steps.Num())
		{
			Cleanup();
			Test.AddInfo(TEXT("Actual-input sequence finished; keys released, aircraft left stationary in GOD. "
				"The automation controller may end PIE again when it stops the test session."));
			return true;
		}
		BeginStep();
		return false;
	}

private:
	struct FStep
	{
		FString Name;
		double Seconds = 0.0;
		TFunction<void()> Start;
		TFunction<void()> Observe;
		TFunction<void()> Finish;
		bool bAllowsReset = false;
	};

	FAutomationTestBase& Test;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<APlayerController> Player;
	TWeakObjectPtr<ADublinFlightPawn> Plane;
	TArray<FStep> Steps;
	TArray<FKey> HeldKeys;
	FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
	FTransform Spawn;
	FTransform Snapshot;
	FVector PreviousPosition = FVector::ZeroVector;
	FVector StationaryPosition = FVector::ZeroVector;
	double SnapshotSpeed = 0.0;
	double MaximumDriftCm = 0.0;
	double StartedAt = 0.0;
	double LastWorldTime = 0.0;
	double StepStartedAt = 0.0;
	int32 StepIndex = 0;
	int32 StepFrames = 0;
	bool bStarted = false;
	bool bCleanedUp = false;
	bool bAbort = false;
	bool bSessionEnding = false;

	void OnPrePIEEnded(bool bSimulating)
	{
		bSessionEnding = true;
		Cleanup();
	}

	void Send(const FKey& Key, EInputEvent Event, float Amount)
	{
		if (Player.IsValid())
		{
			FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, Event, Amount, 1, InputDevice);
			Args.DeltaTime = World.IsValid() ? World->GetDeltaSeconds() : 1.0f / 60.0f;
			Player->InputKey(Args);
		}
	}

	void Press(const FKey& Key)
	{
		HeldKeys.AddUnique(Key);
		Send(Key, IE_Pressed, 1.0f);
	}

	void Release(const FKey& Key)
	{
		Send(Key, IE_Released, 0.0f);
		HeldKeys.Remove(Key);
	}

	void ZeroMouse()
	{
		Send(EKeys::MouseX, IE_Axis, 0.0f);
		Send(EKeys::MouseY, IE_Axis, 0.0f);
	}

	void Setup()
	{
		InputDevice = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Player->GetPlatformUserId());
		Require(TEXT("Local PIE player has a mapped input device"), InputDevice != INPUTDEVICEID_NONE);
		Player->FlushPressedKeys();
		Player->SetInputMode(FInputModeGameOnly());
		if (UGameViewportClient* Viewport = World->GetGameViewport(); Viewport && Viewport->Viewport)
		{
			Viewport->Viewport->SetUserFocus(true);
			Viewport->Viewport->CaptureMouse(true);
		}
		// State methods are restricted to fixture setup/cleanup, never acceptance steps.
		Plane->ResetFlight();
		Spawn = Plane->GetInitialSpawnTransform();
		PreviousPosition = Plane->GetActorLocation();
	}

	void Cleanup()
	{
		if (bCleanedUp || !bStarted)
		{
			return;
		}
		bCleanedUp = true;
		const TArray<FKey> KeysToRelease = HeldKeys;
		for (const FKey& Key : KeysToRelease)
		{
			Release(Key);
		}
		ZeroMouse();
		if (Player.IsValid())
		{
			Player->FlushPressedKeys();
		}
		if (Plane.IsValid() && !Plane->IsGodMode())
		{
			Plane->ToggleGodMode();
		}
	}

	void AddStep(const TCHAR* Name, double Seconds, TFunction<void()> Start,
		TFunction<void()> Finish, TFunction<void()> Observe = {}, bool bAllowsReset = false)
	{
		FStep Step;
		Step.Name = Name;
		Step.Seconds = Seconds;
		Step.Start = MoveTemp(Start);
		Step.Observe = MoveTemp(Observe);
		Step.Finish = MoveTemp(Finish);
		Step.bAllowsReset = bAllowsReset;
		Steps.Add(MoveTemp(Step));
	}

	void BeginStep()
	{
		StepStartedAt = World->GetTimeSeconds();
		StepFrames = 0;
		Snapshot = Plane->GetActorTransform();
		SnapshotSpeed = Plane->CruiseSpeedMetersPerSecond;
		if (Steps[StepIndex].Start)
		{
			Steps[StepIndex].Start();
		}
	}

	void Require(const TCHAR* Description, bool bCondition)
	{
		bAbort |= !Test.TestTrue(Description, bCondition);
	}

	void WatchStationary()
	{
		MaximumDriftCm = FMath::Max(MaximumDriftCm, FVector::Distance(Plane->GetActorLocation(), StationaryPosition));
	}

	void HoldControl(const TCHAR* Name, const FKey& Key, double Seconds, TFunction<void()> Verify)
	{
		AddStep(Name, Seconds, [this, Key]() { Press(Key); }, MoveTemp(Verify));
		const FString ReleaseName = FString::Printf(TEXT("%s release"), Name);
		AddStep(*ReleaseName, 0.08, [this, Key]() { Release(Key); },
			[this, Key]() { Require(TEXT("PlayerController sees the key released"), !Player->IsInputKeyDown(Key)); });
	}

	void BuildSteps()
	{
		AddStep(TEXT("Initial possessed flight"), 0.25, {}, [this]()
		{
			Require(TEXT("Initial mode is FLIGHT"), !Plane->IsGodMode());
			Require(TEXT("Mid-air Dublin PlayerStart is captured"),
				Spawn.GetLocation().Equals(FVector(-28000.0, 8000.0, 18000.0), 1.0));
			Require(TEXT("Dublin spawn points east at yaw zero"), Spawn.GetRotation().Equals(FQuat::Identity, 0.0001));
			Require(TEXT("Readable native HUD is installed"), Cast<ADublinFlightHUD>(Player->GetHUD()) != nullptr);
			Require(TEXT("Pawn is the active view target"), Player->GetViewTarget() == Plane.Get());
			Test.TestNearlyEqual(TEXT("Initial speed is 40 m/s"), Plane->GetSpeedMetersPerSecond(), 40.0f, 0.01f);
			Test.TestNearlyEqual(TEXT("Initial compass is east, not north"), Plane->GetHeadingDegrees(), 90.0f, 0.01f);
			Require(TEXT("Real PIE frames advance the aircraft"),
				FVector::Distance(Plane->GetActorLocation(), Snapshot.GetLocation()) > 500.0);
			UGameViewportClient* Viewport = World->GetGameViewport();
			Require(TEXT("PIE viewport has focus and mouse capture for actual input"),
				Viewport && Viewport->Viewport && Viewport->Viewport->HasFocus() && Viewport->Viewport->HasMouseCapture());
		});

		HoldControl(TEXT("D bank right"), EKeys::D, 0.25, [this]()
		{
			Require(TEXT("Held D changes bank over elapsed frames"),
				Plane->GetActorRotation().Roll - Snapshot.Rotator().Roll > 5.0);
		});
		HoldControl(TEXT("A bank left"), EKeys::A, 0.25, [this]()
		{
			Require(TEXT("Held A changes bank in the opposite direction"),
				Plane->GetActorRotation().Roll - Snapshot.Rotator().Roll < -5.0);
		});
		HoldControl(TEXT("S pitch up"), EKeys::S, 0.25, [this]()
		{
			Require(TEXT("Held S raises the nose"), Plane->GetActorRotation().Pitch - Snapshot.Rotator().Pitch > 3.0);
			Require(TEXT("Pitch input causes a climb"), Plane->GetActorLocation().Z > Snapshot.GetLocation().Z + 5.0);
		});
		HoldControl(TEXT("W pitch down"), EKeys::W, 0.25, [this]()
		{
			Require(TEXT("Held W lowers the nose"), Plane->GetActorRotation().Pitch - Snapshot.Rotator().Pitch < -3.0);
		});
		HoldControl(TEXT("E yaw right"), EKeys::E, 0.25, [this]()
		{
			Require(TEXT("Held E turns right"),
				FMath::FindDeltaAngleDegrees(Snapshot.Rotator().Yaw, Plane->GetActorRotation().Yaw) > 2.0);
		});
		HoldControl(TEXT("Q yaw left"), EKeys::Q, 0.25, [this]()
		{
			Require(TEXT("Held Q turns left"),
				FMath::FindDeltaAngleDegrees(Snapshot.Rotator().Yaw, Plane->GetActorRotation().Yaw) < -2.0);
		});
		HoldControl(TEXT("Shift throttle up"), EKeys::LeftShift, 0.4, [this]()
		{
			Require(TEXT("Held Shift increases throttle"), Plane->CruiseSpeedMetersPerSecond > SnapshotSpeed + 2.0);
			Require(TEXT("Throttle remains at or below 65 m/s"), Plane->CruiseSpeedMetersPerSecond <= 65.01f);
		});
		HoldControl(TEXT("Ctrl throttle down"), EKeys::LeftControl, 0.4, [this]()
		{
			Require(TEXT("Held Ctrl decreases throttle"), Plane->CruiseSpeedMetersPerSecond < SnapshotSpeed - 2.0);
			Require(TEXT("Throttle remains at or above 20 m/s"), Plane->CruiseSpeedMetersPerSecond >= 19.99f);
		});

		AddStep(TEXT("Hold W before switching"), 0.15, [this]() { Press(EKeys::W); }, [this]()
		{
			Require(TEXT("W is physically held before G"), Player->IsInputKeyDown(EKeys::W));
		});
		AddStep(TEXT("G enters GOD with W still held"), 0.1, [this]() { Press(EKeys::G); }, [this]()
		{
			Require(TEXT("G key entered GOD"), Plane->IsGodMode());
			Require(TEXT("God entry stops velocity"), Plane->GetVelocity().IsNearlyZero(0.001));
			Require(TEXT("Toggle preserves attitude rather than snapping level"),
				Plane->GetActorQuat().AngularDistance(Snapshot.GetRotation()) < FMath::DegreesToRadians(2.0));
			StationaryPosition = Plane->GetActorLocation();
			MaximumDriftCm = 0.0;
		});
		AddStep(TEXT("G held five seconds, carried W suppressed"), 5.0, {}, [this]()
		{
			Require(TEXT("Held/repeated G does not toggle repeatedly"), Plane->IsGodMode());
			Require(TEXT("G and W remain held during the five-second trial"),
				Player->IsInputKeyDown(EKeys::G) && Player->IsInputKeyDown(EKeys::W));
			Test.TestTrue(FString::Printf(TEXT("Maximum five-second god drift %.6f cm is under 1 cm"), MaximumDriftCm),
				MaximumDriftCm < 1.0);
			Require(TEXT("Stationary GOD speed is zero"), Plane->GetVelocity().IsNearlyZero(0.001));
		}, [this]()
		{
			Send(EKeys::G, IE_Repeat, 1.0f);
			WatchStationary();
		});
		AddStep(TEXT("Release G and carried W"), 0.15, [this]()
		{
			Release(EKeys::G);
			Release(EKeys::W);
		}, [this]()
		{
			Require(TEXT("G released"), !Player->IsInputKeyDown(EKeys::G));
			Require(TEXT("W released"), !Player->IsInputKeyDown(EKeys::W));
			Require(TEXT("No-input GOD still has no drift"), MaximumDriftCm < 1.0);
		}, [this]() { WatchStationary(); });

		HoldControl(TEXT("God W re-pressed forward"), EKeys::W, 0.3, [this]()
		{
			const FVector Delta = Plane->GetActorLocation() - Snapshot.GetLocation();
			const FVector Forward = FRotator(0.0, Snapshot.Rotator().Yaw, 0.0).Vector();
			Require(TEXT("Released/re-pressed W deliberately repositions"), FVector::DotProduct(Delta, Forward) > 400.0);
			Test.TestNearlyEqual(TEXT("God forward stays horizontal"), Delta.Z, 0.0, 0.5);
		});
		HoldControl(TEXT("God D strafe"), EKeys::D, 0.3, [this]()
		{
			const FVector Delta = Plane->GetActorLocation() - Snapshot.GetLocation();
			const FVector Right = FRotator(0.0, Snapshot.Rotator().Yaw, 0.0).Quaternion().GetRightVector();
			Require(TEXT("God D strafes in aimed heading"), FVector::DotProduct(Delta, Right) > 400.0);
			Test.TestNearlyEqual(TEXT("God strafe stays horizontal"), Delta.Z, 0.0, 0.5);
		});
		HoldControl(TEXT("God Space climbs"), EKeys::SpaceBar, 0.3, [this]()
		{
			Require(TEXT("Space deliberately gains altitude"), Plane->GetActorLocation().Z > Snapshot.GetLocation().Z + 400.0);
		});
		HoldControl(TEXT("God C descends"), EKeys::C, 0.2, [this]()
		{
			Require(TEXT("C deliberately loses altitude"), Plane->GetActorLocation().Z < Snapshot.GetLocation().Z - 250.0);
		});
		AddStep(TEXT("God released movement stops immediately"), 0.25, [this]()
		{
			StationaryPosition = Plane->GetActorLocation();
			MaximumDriftCm = 0.0;
		}, [this]()
		{
			Require(TEXT("Released god controls do not coast"), MaximumDriftCm < 1.0);
			Require(TEXT("Released god movement velocity is zero"), Plane->GetVelocity().IsNearlyZero(0.001));
		}, [this]() { WatchStationary(); });

		AddStep(TEXT("Mouse axes aim without translation"), 0.5, [this]()
		{
			StationaryPosition = Plane->GetActorLocation();
			MaximumDriftCm = 0.0;
		}, [this]()
		{
			Require(TEXT("PlayerController MouseX changes yaw"),
				FMath::Abs(FMath::FindDeltaAngleDegrees(Snapshot.Rotator().Yaw, Plane->GetActorRotation().Yaw)) > 0.1);
			Require(TEXT("PlayerController MouseY changes pitch"),
				FMath::Abs(Plane->GetActorRotation().Pitch - Snapshot.Rotator().Pitch) > 0.1);
			Require(TEXT("Mouse aiming cannot translate GOD aircraft"), MaximumDriftCm < 1.0);
			ZeroMouse();
		}, [this]()
		{
			WatchStationary();
			Send(EKeys::MouseX, IE_Axis, 8.0f);
			Send(EKeys::MouseY, IE_Axis, 6.0f);
		});
		AddStep(TEXT("Mouse axes returned to zero"), 0.25, [this]() { ZeroMouse(); },
			[this]() { Require(TEXT("Aiming release remains stationary"), MaximumDriftCm < 1.0); },
			[this]() { ZeroMouse(); WatchStationary(); });

		AddStep(TEXT("G resumes without teleport"), 0.15, [this]() { Press(EKeys::G); }, [this]()
		{
			Require(TEXT("G resumed flight"), !Plane->IsGodMode());
			const FVector Delta = Plane->GetActorLocation() - Snapshot.GetLocation();
			Require(TEXT("Resume advances from the repositioned pose"),
				FVector::DotProduct(Delta, Snapshot.GetRotation().GetForwardVector()) > 100.0);
			Require(TEXT("Resume cannot return to original spawn"),
				Delta.Size() <= SnapshotSpeed * 100.0 * (World->GetTimeSeconds() - StepStartedAt) + 2.0);
			Test.TestNearlyEqual(TEXT("Resume preserves selected cruise speed"),
				static_cast<double>(Plane->CruiseSpeedMetersPerSecond), SnapshotSpeed, 0.01);
			Require(TEXT("Resume preserves aimed orientation"),
				Plane->GetActorQuat().AngularDistance(Snapshot.GetRotation()) < FMath::DegreesToRadians(3.0));
		});
		AddStep(TEXT("Release resume G"), 0.1, [this]() { Release(EKeys::G); },
			[this]() { Require(TEXT("Resume G released"), !Player->IsInputKeyDown(EKeys::G)); });

		AddStep(TEXT("Home restores the original mid-air spawn"), 0.0, [this]() { Press(EKeys::Home); }, [this]()
		{
			Require(TEXT("Home returns to FLIGHT"), !Plane->IsGodMode());
			Require(TEXT("Home is not blocked"), !Plane->bResetBlocked);
			const FVector Delta = Plane->GetActorLocation() - Spawn.GetLocation();
			const FVector Forward = Spawn.GetRotation().GetForwardVector();
			const double Along = FVector::DotProduct(Delta, Forward);
			const FVector Lateral = Delta - Forward * Along;
			Require(TEXT("Home restores original position then only advances along spawn heading"), Lateral.Size() < 1.0);
			Require(TEXT("Home movement is bounded by elapsed post-reset frames"),
				Along >= -1.0 && Along <= 4000.0 * (World->GetTimeSeconds() - StepStartedAt) + 2.0);
			Require(TEXT("Home restores exact original orientation"), Plane->GetActorQuat().Equals(Spawn.GetRotation(), 0.00001));
			Test.TestNearlyEqual(TEXT("Home resets throttle to 40 m/s"), Plane->CruiseSpeedMetersPerSecond, 40.0f, 0.01f);
			Test.TestNearlyEqual(TEXT("Home HUD compass is east"), Plane->GetHeadingDegrees(), 90.0f, 0.01f);
			Test.TestNearlyEqual(TEXT("Home HUD altitude is 180 m"), Plane->GetAltitudeMeters(), 180.0f, 0.01f);
		}, {}, true);
		AddStep(TEXT("Release Home"), 0.08, [this]() { Release(EKeys::Home); },
			[this]() { Require(TEXT("Home released"), !Player->IsInputKeyDown(EKeys::Home)); });
		AddStep(TEXT("Final G parks for screenshot"), 0.1, [this]() { Press(EKeys::G); }, [this]()
		{
			Require(TEXT("Final G parks in GOD"), Plane->IsGodMode());
			StationaryPosition = Plane->GetActorLocation();
			MaximumDriftCm = 0.0;
		});
		AddStep(TEXT("Release final G and verify parked"), 0.25, [this]() { Release(EKeys::G); }, [this]()
		{
			Require(TEXT("Final G released"), !Player->IsInputKeyDown(EKeys::G));
			Require(TEXT("Final stationary screenshot state"), Plane->IsGodMode() && MaximumDriftCm < 1.0);
		}, [this]() { WatchStationary(); });
	}
};

class FWaitForPossessedDublinPIE final : public IAutomationLatentCommand
{
public:
	explicit FWaitForPossessedDublinPIE(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}

	virtual ~FWaitForPossessedDublinPIE() override
	{
		Scenario.Reset();
		if (StartupPlayer.IsValid())
		{
			StartupPlayer->FlushPressedKeys();
		}
		if (StartupPlane.IsValid() && !StartupPlane->IsGodMode())
		{
			StartupPlane->ToggleGodMode();
		}
	}

	virtual bool Update() override
	{
		if (Scenario)
		{
			return Scenario->Update();
		}
		if (WaitStartedAt == 0.0)
		{
			WaitStartedAt = FPlatformTime::Seconds();
		}
		if (!GEditor || !GEngine)
		{
			Test.AddError(TEXT("Editor became unavailable while waiting for fixture-owned PIE."));
			return true;
		}

		UWorld* PIEWorld = nullptr;
		APlayerController* Player = nullptr;
		ADublinFlightPawn* Plane = nullptr;
		bool bHasPIEWorld = false;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (!Candidate || Candidate->WorldType != EWorldType::PIE || Candidate->bIsTearingDown)
			{
				continue;
			}
			bHasPIEWorld = true;
			if (UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) != TEXT("/Game/Maps/Dublin"))
			{
				Test.AddError(TEXT("Fixture started the wrong PIE map; /Game/Maps/Dublin is required."));
				return true;
			}
			APlayerController* CandidatePlayer = Candidate->GetFirstPlayerController();
			ADublinFlightPawn* CandidatePlane = CandidatePlayer ? Cast<ADublinFlightPawn>(CandidatePlayer->GetPawn()) : nullptr;
			if (!CandidatePlayer || !CandidatePlayer->IsLocalController() || !CandidatePlane)
			{
				continue;
			}
			if (PIEWorld)
			{
				Test.AddError(TEXT("Run DublinFlight.PIE with exactly one locally possessed PIE world, not multi-client PIE."));
				return true;
			}
			PIEWorld = Candidate;
			Player = CandidatePlayer;
			Plane = CandidatePlane;
			StartupPlayer = Player;
			StartupPlane = Plane;
		}
		if (bSawPIEWorld && !bHasPIEWorld)
		{
			Test.AddError(TEXT("Fixture-owned PIE ended before a possessed DublinFlightPawn and viewport became ready."));
			return true;
		}
		bSawPIEWorld |= bHasPIEWorld;
		if (PIEWorld && PIEWorld->AreActorsInitialized() && Player->PlayerInput && Plane->bSpawnCaptured
			&& Player->GetHUD() && CaptureFlightPIEViewport(*PIEWorld, *Player))
		{
			Test.AddInfo(TEXT("Fixture-owned Dublin PIE is initialized, possessed and natively focused; starting actual-input acceptance."));
			Scenario = MakeUnique<FActualPlayerInputScenario>(Test, *PIEWorld, *Player, *Plane);
			return Scenario->Update();
		}
		if (FPlatformTime::Seconds() - WaitStartedAt > 60.0)
		{
			Test.AddError(TEXT("Fixture PIE startup timed out after 60 seconds: FStartPIECommand(false) must produce "
				"a locally possessed DublinFlightPawn, initialized player input/HUD, and focused/captured viewport. "
				"No PIE or failed possession/capture is a test failure, never a skipped input check."));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase& Test;
	TUniquePtr<FActualPlayerInputScenario> Scenario;
	TWeakObjectPtr<APlayerController> StartupPlayer;
	TWeakObjectPtr<ADublinFlightPawn> StartupPlane;
	double WaitStartedAt = 0.0;
	bool bSawPIEWorld = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightActualPIEInputTest,
	"DublinFlight.PIE.ActualPlayerControllerInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightActualPIEInputTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddError(TEXT("DublinFlight.PIE requires the editor. The fixture starts its own PIE session after automation-session setup."));
		return false;
	}
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld || EditorWorld->WorldType != EWorldType::Editor
		|| EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open /Game/Maps/Dublin in the current editor before running this test. "
			"The fixture will start Play, wait for possession, and exercise actual input; it does not load or substitute a test map."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinFlight::Tests::FWaitForPossessedDublinPIE(*this));
	return true;
}

#endif
