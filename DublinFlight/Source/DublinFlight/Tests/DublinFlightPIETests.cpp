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
#include "GameFramework/SpringArmComponent.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "Weapons/DublinWeaponComponent.h"

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
		ADublinFlightPawn& InPlane, bool bInSpeedAndBodyAxesOnly = false)
		: Test(InTest), World(&InWorld), Player(&InPlayer), Plane(&InPlane), bSpeedAndBodyAxesOnly(bInSpeedAndBodyAxesOnly)
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
		if (!bIntentionallyUncaptured && !bWaitingForCaptureRestore && !CaptureFlightPIEViewport(*World.Get(), *Player.Get()))
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
		if ((bIntentionallyUncaptured || bWaitingForCaptureRestore) && FPlatformTime::Seconds() - CaptureScopeStartedWall > 5.0)
		{
			Test.AddError(TEXT("Scoped capture-loss phase did not complete within five wall seconds. ") + CaptureDiagnostics());
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
		if (bAbort)
		{
			Cleanup();
			return true;
		}
		// Both elapsed game time and separate world frames are required. No Tick() or
		// ProcessPlayerInput() calls are used to manufacture a passing result.
		if (StepFrames < 2 || Now - StepStartedAt < Step.Seconds || (Step.Until && !Step.Until()))
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
		TFunction<bool()> Until;
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
	FTransform CameraSnapshot;
	FVector CameraOffsetSnapshot = FVector::ZeroVector;
	FVector OrbitVelocitySnapshot = FVector::ZeroVector;
	FVector OrbitCannonDirection = FVector::ZeroVector;
	FVector2D OrbitDisplacement = FVector2D::ZeroVector;
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
	bool bSpeedAndBodyAxesOnly = false;
	bool bIgnoringLookForTest = false;
	bool bIgnoringMoveForTest = false;
	bool bIntentionallyUncaptured = false;
	bool bWaitingForCaptureRestore = false;
	TWeakObjectPtr<UGameViewportClient> CaptureScopeViewport;
	EMouseCaptureMode SavedMouseCaptureMode = EMouseCaptureMode::NoCapture;
	EMouseLockMode SavedMouseLockMode = EMouseLockMode::DoNotLock;
	bool bSavedHideCursorDuringCapture = false;
	bool bSavedShowMouseCursor = false;
	double CaptureScopeStartedWall = 0.0;
	int32 UncapturedWorldFrames = 0;
	int32 RecapturedWorldFrames = 0;
	FQuat BodyPreviousOrientation = FQuat::Identity;
	double BodyPreviousTime = 0.0;
	double BodyMaximumErrorRatio = 0.0;
	double BodyMaximumErrorDegrees = 0.0;
	int32 BodySamples = 0;

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
		Send(EKeys::MouseWheelAxis, IE_Axis, 0.0f);
	}

	void Setup()
	{
		InputDevice = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Player->GetPlatformUserId());
		Require(TEXT("Local PIE player has a mapped input device"), InputDevice != INPUTDEVICEID_NONE);
		Require(TEXT("Orbit acceptance has the native camera and weapon components"),
			Plane->CameraBoom && Plane->ChaseCamera && Plane->Weapons);
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
			if (bIgnoringLookForTest) { Player->SetIgnoreLookInput(false); bIgnoringLookForTest = false; }
			if (bIgnoringMoveForTest) { Player->SetIgnoreMoveInput(false); bIgnoringMoveForTest = false; }
			Player->FlushPressedKeys();
		}
		RestoreCaptureScope();
		bWaitingForCaptureRestore = false;
		if (Plane.IsValid() && !Plane->IsGodMode())
		{
			Plane->ToggleGodMode();
		}
	}

	void AddStep(const TCHAR* Name, double Seconds, TFunction<void()> Start,
		TFunction<void()> Finish, TFunction<void()> Observe = {}, bool bAllowsReset = false,
		TFunction<bool()> Until = {})
	{
		FStep Step;
		Step.Name = Name;
		Step.Seconds = Seconds;
		Step.Start = MoveTemp(Start);
		Step.Observe = MoveTemp(Observe);
		Step.Finish = MoveTemp(Finish);
		Step.Until = MoveTemp(Until);
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

	FTransform ReadPlayerCamera() const
	{
		FVector Location;
		FRotator Rotation;
		Player->GetPlayerViewPoint(Location, Rotation);
		return FTransform(Rotation, Location);
	}

	void RequireChaseView(const TCHAR* Description)
	{
		const FRotator Aircraft = Plane->GetActorRotation();
		const FRotator Expected(FMath::Clamp(Aircraft.Pitch - 28.0, -85.0, 85.0), Aircraft.Yaw, 0.0);
		Require(Description, Plane->CameraBoom->GetComponentRotation().Equals(Expected, 0.01)
			&& ReadPlayerCamera().Rotator().Equals(Expected, 0.01));
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
		if (bSpeedAndBodyAxesOnly)
		{
			BuildSpeedAndBodyAxisSteps();
			return;
		}
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
			RequireChaseView(TEXT("Initial player camera retains the default chase view"));
		});

		AddStep(TEXT("Flight mouse orbits without a held button"), 0.25, [this]()
		{
			CameraSnapshot = ReadPlayerCamera();
			CameraOffsetSnapshot = CameraSnapshot.GetLocation() - Plane->GetActorLocation();
			OrbitVelocitySnapshot = Plane->GetVelocity();
			FTransform Launch;
			FVector Velocity;
			const bool bHasLaunch = Plane->Weapons->GetCannonLaunch(Launch, Velocity);
			Require(TEXT("Native flight cannon launch is available before orbit"), bHasLaunch);
			if (!bHasLaunch) { return; }
			OrbitCannonDirection = Velocity.GetSafeNormal();
			Send(EKeys::MouseX, IE_Axis, 120.0f);
			Send(EKeys::MouseY, IE_Axis, 80.0f);
		}, [this]()
		{
			const FTransform Camera = ReadPlayerCamera();
			OrbitDisplacement = FVector2D(
				FMath::FindDeltaAngleDegrees(CameraSnapshot.Rotator().Yaw, Camera.Rotator().Yaw),
				Camera.Rotator().Pitch - CameraSnapshot.Rotator().Pitch);
			Require(TEXT("Mouse orbit requires neither mouse button"),
				!Player->IsInputKeyDown(EKeys::LeftMouseButton) && !Player->IsInputKeyDown(EKeys::RightMouseButton));
			Require(TEXT("Flight MouseX changes actual player-camera yaw"),
				FMath::Abs(FMath::FindDeltaAngleDegrees(CameraSnapshot.Rotator().Yaw, Camera.Rotator().Yaw)) > 0.1);
			Require(TEXT("Flight MouseY changes actual player-camera pitch"),
				FMath::Abs(Camera.Rotator().Pitch - CameraSnapshot.Rotator().Pitch) > 0.1);
			Require(TEXT("Orbit moves the camera around the aircraft, not just its look direction"),
				FVector::Distance(Camera.GetLocation() - Plane->GetActorLocation(), CameraOffsetSnapshot) > 5.0);
			Require(TEXT("Flight mouse cannot steer aircraft attitude"),
				Plane->GetActorQuat().Equals(Snapshot.GetRotation(), 0.00001));
			Test.TestNearlyEqual(TEXT("Flight mouse preserves velocity magnitude"),
				Plane->GetVelocity().Size(), OrbitVelocitySnapshot.Size(), 0.01);
			Require(TEXT("Flight mouse preserves velocity direction"),
				Plane->GetVelocity().Equals(OrbitVelocitySnapshot, 0.01));
			Require(TEXT("Weapon view rotation remains aircraft attitude, not orbit"),
				Plane->GetViewRotation().Equals(Snapshot.Rotator(), 0.00001));
			FTransform Launch;
			FVector Velocity;
			const bool bHasLaunch = Plane->Weapons->GetCannonLaunch(Launch, Velocity);
			Require(TEXT("Native cannon launch remains available while orbited"), bHasLaunch);
			if (bHasLaunch)
			{
				Require(TEXT("Orbit cannot redirect the cannon launch"),
					Velocity.GetSafeNormal().Equals(OrbitCannonDirection, 0.00001));
			}
			Require(TEXT("Aircraft continues flying forward while the camera orbits"),
				FVector::DotProduct(Plane->GetActorLocation() - Snapshot.GetLocation(),
					Snapshot.GetRotation().GetForwardVector()) > 500.0);
		});
		AddStep(TEXT("Flight orbit holds without more mouse motion"), 0.25, [this]()
		{
			CameraSnapshot = ReadPlayerCamera();
			ZeroMouse();
		}, [this]()
		{
			Require(TEXT("A consumed mouse displacement does not repeat on later frames"),
				ReadPlayerCamera().GetRotation().Equals(CameraSnapshot.GetRotation(), 0.0001));
			Require(TEXT("Retaining the orbit does not stop flight"),
				FVector::Distance(Plane->GetActorLocation(), Snapshot.GetLocation()) > 500.0);
		}, [this]() { ZeroMouse(); });
		AddStep(TEXT("G clears flight orbit and queued mouse"), 0.15, [this]()
		{
			// Key actions dispatch before summed axes, even when the mouse was queued first.
			Send(EKeys::MouseX, IE_Axis, 240.0f);
			Send(EKeys::MouseY, IE_Axis, 160.0f);
			Press(EKeys::G);
		}, [this]()
		{
			Require(TEXT("Orbited flight enters GOD through actual G input"), Plane->IsGodMode());
			Require(TEXT("Queued flight mouse cannot become GOD aim on the transition frame"),
				Plane->GetActorQuat().Equals(Snapshot.GetRotation(), 0.00001));
			Require(TEXT("Orbit-to-GOD transition stops flight velocity"), Plane->GetVelocity().IsNearlyZero(0.001));
			RequireChaseView(TEXT("G removes the flight orbit from boom and actual player view"));
		});
		AddStep(TEXT("Release orbit-test G"), 0.15, [this]() { Release(EKeys::G); }, [this]()
		{
			Require(TEXT("Orbit-test G is released"), !Player->IsInputKeyDown(EKeys::G));
			Require(TEXT("No stale flight mouse aims GOD on later frames"),
				Plane->GetActorQuat().Equals(Snapshot.GetRotation(), 0.00001));
			RequireChaseView(TEXT("Cleared orbit stays centered in GOD"));
		});
		AddStep(TEXT("Same actual mouse displacement retains one-third GOD sensitivity"), 0.25, [this]()
		{
			Send(EKeys::MouseX, IE_Axis, 120.0f);
			Send(EKeys::MouseY, IE_Axis, 80.0f);
		}, [this]()
		{
			const FRotator Aim = Plane->GetActorRotation();
			const double YawDelta = FMath::FindDeltaAngleDegrees(Snapshot.Rotator().Yaw, Aim.Yaw);
			const double PitchDelta = Aim.Pitch - Snapshot.Rotator().Pitch;
			Test.TestNearlyEqual(TEXT("Real flight mouse yaw is three times GOD aim for the same input"),
				OrbitDisplacement.X, YawDelta * 3.0, 0.01);
			Test.TestNearlyEqual(TEXT("Real flight mouse pitch is three times GOD aim for the same input"),
				OrbitDisplacement.Y, PitchDelta * 3.0, 0.01);
			Require(TEXT("Sensitivity comparison cannot translate GOD aircraft"),
				Plane->GetActorLocation().Equals(Snapshot.GetLocation(), 0.001));
			RequireChaseView(TEXT("GOD sensitivity comparison still has no separate orbit"));
		});
		AddStep(TEXT("G resumes without queued GOD mouse orbit"), 0.15, [this]()
		{
			Send(EKeys::MouseX, IE_Axis, -240.0f);
			Send(EKeys::MouseY, IE_Axis, -160.0f);
			Press(EKeys::G);
		}, [this]()
		{
			Require(TEXT("Orbit-test G resumes FLIGHT"), !Plane->IsGodMode());
			Require(TEXT("Queued GOD mouse cannot steer resumed flight"),
				Plane->GetActorQuat().Equals(Snapshot.GetRotation(), 0.00001));
			RequireChaseView(TEXT("Queued GOD mouse cannot create a flight orbit on transition"));
			Require(TEXT("Clearing mouse input still permits forward flight"),
				FVector::DotProduct(Plane->GetActorLocation() - Snapshot.GetLocation(),
					Snapshot.GetRotation().GetForwardVector()) > 100.0);
		});
		AddStep(TEXT("Release orbit-test resume G"), 0.1, [this]() { Release(EKeys::G); }, [this]()
		{
			Require(TEXT("Orbit-test resume G is released"), !Player->IsInputKeyDown(EKeys::G));
			RequireChaseView(TEXT("No stale mouse orbit appears after resumed world frames"));
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
			Require(TEXT("Throttle remains at or below 100 m/s"), Plane->CruiseSpeedMetersPerSecond <= 100.01f);
		});
		HoldControl(TEXT("Ctrl throttle down"), EKeys::LeftControl, 0.4, [this]()
		{
			Require(TEXT("Held Ctrl decreases throttle"), Plane->CruiseSpeedMetersPerSecond < SnapshotSpeed - 2.0);
			Require(TEXT("Throttle remains at or above 5 m/s"), Plane->CruiseSpeedMetersPerSecond >= 4.99f);
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
			Require(TEXT("GOD mouse still aims the aircraft weapon view"),
				Plane->GetViewRotation().Equals(Plane->GetActorRotation(), 0.00001));
			FTransform Launch;
			FVector Velocity;
			const bool bHasLaunch = Plane->Weapons->GetCannonLaunch(Launch, Velocity);
			Require(TEXT("GOD cannon launch remains available after mouse aim"), bHasLaunch);
			if (bHasLaunch)
			{
				Require(TEXT("Stationary GOD cannon follows the aimed aircraft"),
					Velocity.GetSafeNormal().Equals(Plane->GetActorForwardVector(), 0.00001));
			}
			ZeroMouse();
		}, [this]()
		{
			WatchStationary();
			Send(EKeys::MouseX, IE_Axis, 8.0f);
			Send(EKeys::MouseY, IE_Axis, 6.0f);
		});
		AddStep(TEXT("Mouse axes returned to zero"), 0.25, [this]() { ZeroMouse(); },
			[this]()
			{
				Require(TEXT("Aiming release remains stationary"), MaximumDriftCm < 1.0);
				RequireChaseView(TEXT("GOD mouse aim does not accumulate a separate camera orbit"));
			},
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

		AddStep(TEXT("Flight mouse creates an orbit before Home"), 0.25, [this]()
		{
			Send(EKeys::MouseX, IE_Axis, 120.0f);
			Send(EKeys::MouseY, IE_Axis, 80.0f);
		}, [this]()
		{
			const FRotator Camera = ReadPlayerCamera().Rotator();
			const FRotator Aircraft = Plane->GetActorRotation();
			Require(TEXT("Home trial starts with a real nonzero yaw orbit"),
				FMath::Abs(FMath::FindDeltaAngleDegrees(Aircraft.Yaw, Camera.Yaw)) > 0.1);
			Require(TEXT("Home trial starts with a real nonzero pitch orbit"),
				FMath::Abs(Camera.Pitch - FMath::Clamp(Aircraft.Pitch - 28.0, -85.0, 85.0)) > 0.1);
		});
		AddStep(TEXT("Home restores the original mid-air spawn"), 0.0, [this]()
		{
			Send(EKeys::MouseX, IE_Axis, 240.0f);
			Send(EKeys::MouseY, IE_Axis, 160.0f);
			Press(EKeys::Home);
		}, [this]()
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
			RequireChaseView(TEXT("Home recenters the chase view and discards queued mouse"));
		}, {}, true);
		AddStep(TEXT("Release Home"), 0.08, [this]() { Release(EKeys::Home); },
			[this]()
			{
				Require(TEXT("Home released"), !Player->IsInputKeyDown(EKeys::Home));
				RequireChaseView(TEXT("No stale mouse changes the chase view after Home"));
			});
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

	void RequireSelectedSpeed(float Expected)
	{
		const FString CurrentStepName = Steps.IsValidIndex(StepIndex) ? Steps[StepIndex].Name : TEXT("<no current step>");
		Require(*FString::Printf(TEXT("[%s] HUD selected speed reflects the one simulation speed: "
			"expected=%.3f m/s, HUD=%.3f m/s, simulation=%.3f m/s"),
			*CurrentStepName, Expected, Plane->CruiseSpeedMetersPerSecond, Plane->GetFlightState().FlightSpeedCmPerSecond / 100.0),
			FMath::IsNearlyEqual(Plane->CruiseSpeedMetersPerSecond, Expected, 0.01f)
			&& FMath::IsNearlyEqual(Plane->GetFlightState().FlightSpeedCmPerSecond, Expected * 100.0, 0.01));
		Require(*FString::Printf(TEXT("[%s] Actual velocity matches selected flight speed, or zero in stationary GOD: "
			"expected=%.3f m/s, actual=%.3f m/s, mode=%s"),
			*CurrentStepName, Plane->IsGodMode() ? 0.0f : Expected, Plane->GetSpeedMetersPerSecond(),
			Plane->IsGodMode() ? TEXT("GOD") : TEXT("FLIGHT")),
			FMath::IsNearlyEqual(Plane->GetSpeedMetersPerSecond(), Plane->IsGodMode() ? 0.0f : Expected, 0.01f));
	}

	FString PendingWheelDiagnostics() const
	{
		const FKeyState* Wheel = Player.IsValid() && Player->PlayerInput
			? Player->PlayerInput->GetKeyState(EKeys::MouseWheelAxis) : nullptr;
		return FString::Printf(TEXT("wheelState=%d accumulated=%.3f samples=%d raw=%.3f processed=%.3f "
			"selected=%.3f actual=%.3f worldSeconds=%.3f"),
			Wheel != nullptr, Wheel ? Wheel->RawValueAccumulator.X : 0.0,
			Wheel ? static_cast<int32>(Wheel->SampleCountAccumulator) : 0,
			Wheel ? Wheel->RawValue.X : 0.0, Wheel ? Wheel->Value.X : 0.0,
			Plane.IsValid() ? Plane->CruiseSpeedMetersPerSecond : -1.0f,
			Plane.IsValid() ? Plane->GetSpeedMetersPerSecond() : -1.0f,
			World.IsValid() ? World->GetTimeSeconds() : -1.0);
	}

	FString CaptureDiagnostics() const
	{
		UGameViewportClient* Client = CaptureScopeViewport.Get();
		const FViewport* Viewport = Client ? Client->Viewport : nullptr;
		return FString::Printf(TEXT("captureMode=%d lockMode=%d focus=%d captured=%d viewportIgnored=%d "
			"moveIgnored=%d lookIgnored=%d uncapturedFrames=%d restoring=%d recapturedFrames=%d selected=%.3f actual=%.3f"),
			Client ? static_cast<int32>(Client->GetMouseCaptureMode()) : -1,
			Client ? static_cast<int32>(Client->GetMouseLockMode()) : -1,
			Viewport && Viewport->HasFocus(), Viewport && Viewport->HasMouseCapture(), Client && Client->IgnoreInput(),
			Player.IsValid() && Player->IsMoveInputIgnored(), Player.IsValid() && Player->IsLookInputIgnored(),
			UncapturedWorldFrames, bWaitingForCaptureRestore, RecapturedWorldFrames,
			Plane.IsValid() ? Plane->CruiseSpeedMetersPerSecond : -1.0f,
			Plane.IsValid() ? Plane->GetSpeedMetersPerSecond() : -1.0f);
	}

	bool HasUncapturedInputViewport() const
	{
		UGameViewportClient* Client = CaptureScopeViewport.Get();
		return Client && World.IsValid() && Client == World->GetGameViewport() && Client->Viewport
			&& Client->GetMouseCaptureMode() == EMouseCaptureMode::NoCapture
			&& Client->GetMouseLockMode() == EMouseLockMode::DoNotLock
			&& Client->Viewport->HasFocus() && !Client->Viewport->HasMouseCapture()
			&& !Client->IgnoreInput() && Player.IsValid() && !Player->IsMoveInputIgnored() && !Player->IsLookInputIgnored();
	}

	bool HasRestoredCapture() const
	{
		const UGameViewportClient* Client = CaptureScopeViewport.Get();
		return Client && Client->Viewport && Client->Viewport->HasFocus() && Client->Viewport->HasMouseCapture()
			&& Client->GetMouseCaptureMode() == SavedMouseCaptureMode && Client->GetMouseLockMode() == SavedMouseLockMode
			&& Client->HideCursorDuringCapture() == bSavedHideCursorDuringCapture
			&& Player.IsValid() && Player->bShowMouseCursor == bSavedShowMouseCursor;
	}

	void BeginCaptureScope()
	{
		UGameViewportClient* Client = World->GetGameViewport();
		Require(TEXT("Capture-loss fixture has its live game viewport"), Client && Client->Viewport);
		if (bAbort) { return; }
		CaptureScopeViewport = Client;
		SavedMouseCaptureMode = Client->GetMouseCaptureMode();
		SavedMouseLockMode = Client->GetMouseLockMode();
		bSavedHideCursorDuringCapture = Client->HideCursorDuringCapture();
		bSavedShowMouseCursor = Player->bShowMouseCursor;
		bIntentionallyUncaptured = true;
		CaptureScopeStartedWall = FPlatformTime::Seconds();
		UncapturedWorldFrames = 0;
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		Player->bShowMouseCursor = true;
		// SetInputMode queues release through the local player's Slate operations, not a transient viewport reply.
		Player->SetInputMode(InputMode);
		Client->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
		Test.AddInfo(TEXT("Requested scoped NoCapture; wheel not yet sent. ") + CaptureDiagnostics());
	}

	void RestoreCaptureScope()
	{
		if (!bIntentionallyUncaptured && !bWaitingForCaptureRestore) { return; }
		if (Player.IsValid())
		{
			Player->bShowMouseCursor = bSavedShowMouseCursor;
			if (World.IsValid() && !World->bIsTearingDown && !bSessionEnding)
			{
				// The fixture started in GameOnly. Restore its Slate capture operations before the saved viewport settings.
				Player->SetInputMode(FInputModeGameOnly());
			}
		}
		if (UGameViewportClient* Client = CaptureScopeViewport.Get())
		{
			Client->SetMouseCaptureMode(SavedMouseCaptureMode);
			Client->SetMouseLockMode(SavedMouseLockMode);
			Client->SetHideCursorDuringCapture(bSavedHideCursorDuringCapture);
		}
		bIntentionallyUncaptured = false;
	}

	void WheelStep(const TCHAR* Name, float WheelSteps, float Expected)
	{
		AddStep(Name, 0.2, [this, WheelSteps]()
		{
			CameraSnapshot = ReadPlayerCamera();
			Send(EKeys::MouseWheelAxis, IE_Axis, WheelSteps);
		}, [this, Expected]()
		{
			RequireSelectedSpeed(Expected);
			Require(TEXT("Wheel speed input does not steer the aircraft or weapon view"),
				Plane->GetActorQuat().Equals(Snapshot.GetRotation(), 0.00001)
				&& Plane->GetViewRotation().Equals(Snapshot.Rotator(), 0.00001));
			Require(TEXT("Wheel speed input does not orbit or aim the actual camera"),
				ReadPlayerCamera().GetRotation().Equals(CameraSnapshot.GetRotation(), 0.00001));
			if (Plane->IsGodMode())
			{
				Require(TEXT("Wheel input cannot translate stationary GOD aircraft"),
					Plane->GetActorLocation().Equals(Snapshot.GetLocation(), 0.001));
			}
		});
		AddStep(TEXT("Wheel displacement is consumed, not held/repeated"), 0.15, [this]() { ZeroMouse(); },
			[this, Expected]() { RequireSelectedSpeed(Expected); });
	}

	void BodyAxisStep(const TCHAR* Name, const FKey& Key, const FRotator& LocalRate)
	{
		AddStep(Name, 0.35, [this, Key]()
		{
			const FRotator Attitude = Plane->GetActorRotation();
			Require(TEXT("Actual body-axis trial starts banked, pitched and yawed, clear of envelope limits"),
				FMath::Abs(Attitude.Roll) > 20.0 && FMath::Abs(Attitude.Roll) < 60.0
				&& FMath::Abs(Attitude.Pitch) > 5.0 && FMath::Abs(Attitude.Pitch) < 50.0
				&& FMath::Abs(Attitude.Yaw) > 10.0);
			BodyPreviousOrientation = Plane->GetActorQuat();
			BodyPreviousTime = World->GetTimeSeconds();
			BodySamples = 0;
			BodyMaximumErrorRatio = BodyMaximumErrorDegrees = 0.0;
			Press(Key);
			Send(EKeys::MouseX, IE_Axis, 40.0f);
			Send(EKeys::MouseY, IE_Axis, 20.0f);
		}, [this, Key]()
		{
			Release(Key);
			Require(TEXT("Body-axis assertion observed at least three actual held-input world frames"), BodySamples >= 3);
			Require(*FString::Printf(TEXT("Actual banked body-axis input matches local rotation despite camera orbit "
				"(samples=%d, worst error=%.6f deg, error/tolerance=%.3f)"),
				BodySamples, BodyMaximumErrorDegrees, BodyMaximumErrorRatio), BodyMaximumErrorRatio <= 1.0);
			Require(TEXT("Actual mouse moved the camera away from aircraft heading during body steering"),
				FMath::Abs(FMath::FindDeltaAngleDegrees(Plane->GetActorRotation().Yaw, ReadPlayerCamera().Rotator().Yaw)) > 0.1);
			Require(TEXT("Weapon view remains the real aircraft attitude"), Plane->GetViewRotation().Equals(Plane->GetActorRotation(), 0.001));
			FTransform Launch;
			FVector Velocity;
			Require(TEXT("Camera orbit cannot redirect flight cannon launch"),
				Plane->Weapons->GetCannonLaunch(Launch, Velocity)
				&& Velocity.GetSafeNormal().Equals(Plane->GetActorForwardVector(), 0.00001));
		}, [this, Key, LocalRate]()
		{
			const double Now = World->GetTimeSeconds();
			const double Seconds = Now - BodyPreviousTime;
			const FQuat Actual = Plane->GetActorQuat();
			if (StepFrames > 2 && Seconds > 0.0 && Seconds <= 1.0 / 30.0)
			{
				Require(TEXT("Body-axis sample uses a genuinely held PlayerController key"), Player->IsInputKeyDown(Key));
				FFlightSimulation Idle;
				Idle.Initialize(Plane->GetActorLocation(), BodyPreviousOrientation);
				Idle.Advance(FControlInput(), Seconds);
				const FQuat Local = (Idle.GetState().Orientation.Inverse() * Actual).GetNormalized();
				const double Error = FMath::RadiansToDegrees(Local.AngularDistance((LocalRate * Seconds).Quaternion()));
				const double Rate = FMath::Max(FMath::Abs(LocalRate.Pitch), FMath::Abs(LocalRate.Yaw));
				const double Tolerance = 0.003
					+ FMath::DegreesToRadians(2.0 * Rate * FFlightTuning::BankTurnRateDegreesPerSecond * Seconds * Seconds);
				BodyMaximumErrorDegrees = FMath::Max(BodyMaximumErrorDegrees, Error);
				BodyMaximumErrorRatio = FMath::Max(BodyMaximumErrorRatio, Error / Tolerance);
				++BodySamples;
			}
			BodyPreviousOrientation = Actual;
			BodyPreviousTime = Now;
		});
		AddStep(TEXT("Release body-axis control"), 0.1, [this]() { ZeroMouse(); },
			[this, Key]() { Require(TEXT("Body-axis control released"), !Player->IsInputKeyDown(Key)); });
	}

	void BuildSpeedAndBodyAxisSteps()
	{
		AddStep(TEXT("Initial speed control fixture settles"), 0.2, {}, [this]()
		{
			Require(TEXT("Speed control fixture starts in FLIGHT"), !Plane->IsGodMode());
			RequireSelectedSpeed(40.0f);
		});
		WheelStep(TEXT("Actual wheel up selects 45 m/s"), 1.0f, 45.0f);
		WheelStep(TEXT("Actual wheel down returns to 40 m/s"), -1.0f, 40.0f);
		WheelStep(TEXT("Three actual wheel steps select 55 m/s"), 3.0f, 55.0f);
		HoldControl(TEXT("Shift adjusts the wheel-selected speed"), EKeys::LeftShift, 0.3, [this]()
		{
			Require(TEXT("Held Shift raises the same selected speed"), Plane->CruiseSpeedMetersPerSecond > SnapshotSpeed + 2.0);
		});
		HoldControl(TEXT("Ctrl adjusts the same selected speed"), EKeys::LeftControl, 0.3, [this]()
		{
			Require(TEXT("Held Ctrl lowers the same selected speed"), Plane->CruiseSpeedMetersPerSecond < SnapshotSpeed - 2.0);
		});
		WheelStep(TEXT("Actual wheel input reaches the 100 m/s ceiling"), 19.0f, 100.0f);
		WheelStep(TEXT("Actual wheel input reaches the 5 m/s floor"), -19.0f, 5.0f);
		AddStep(TEXT("Home discards same-frame wheel and restores 40 m/s"), 0.0, [this]()
		{
			Send(EKeys::MouseWheelAxis, IE_Axis, 3.0f);
			Press(EKeys::Home);
		}, [this]()
		{
			RequireSelectedSpeed(40.0f);
			RequireChaseView(TEXT("Home also restores the chase camera"));
		}, {}, true);
		AddStep(TEXT("Release speed-test Home"), 0.15, [this]() { Release(EKeys::Home); ZeroMouse(); },
			[this]() { RequireSelectedSpeed(40.0f); });
		WheelStep(TEXT("Select resume speed before G"), 1.0f, 45.0f);
		AddStep(TEXT("G to GOD discards transition-frame wheel"), 0.15, [this]()
		{
			Send(EKeys::MouseWheelAxis, IE_Axis, 3.0f);
			Press(EKeys::G);
		}, [this]()
		{
			Require(TEXT("Actual G enters GOD"), Plane->IsGodMode());
			RequireSelectedSpeed(45.0f);
			Release(EKeys::G);
		});
		WheelStep(TEXT("God wheel cannot change resume speed or aim"), -19.0f, 45.0f);
		AddStep(TEXT("G to FLIGHT discards transition-frame wheel"), 0.15, [this]()
		{
			Send(EKeys::MouseWheelAxis, IE_Axis, 3.0f);
			Press(EKeys::G);
		}, [this]()
		{
			Require(TEXT("Actual G resumes flight"), !Plane->IsGodMode());
			RequireSelectedSpeed(45.0f);
			Release(EKeys::G);
		});
		AddStep(TEXT("Ignored look input rejects wheel"), 0.15, [this]()
		{
			Player->SetIgnoreLookInput(true);
			bIgnoringLookForTest = true;
			Send(EKeys::MouseWheelAxis, IE_Axis, 2.0f);
		}, [this]() { RequireSelectedSpeed(45.0f); });
		AddStep(TEXT("Look input restoration cannot replay discarded wheel"), 0.15, [this]()
		{
			Player->SetIgnoreLookInput(false);
			bIgnoringLookForTest = false;
			ZeroMouse();
		}, [this]() { RequireSelectedSpeed(45.0f); });
		AddStep(TEXT("Ignored movement input rejects wheel"), 0.15, [this]()
		{
			Player->SetIgnoreMoveInput(true);
			bIgnoringMoveForTest = true;
			Send(EKeys::MouseWheelAxis, IE_Axis, -2.0f);
		}, [this]() { RequireSelectedSpeed(45.0f); });
		AddStep(TEXT("Movement input restoration cannot replay discarded wheel"), 0.15, [this]()
		{
			Player->SetIgnoreMoveInput(false);
			bIgnoringMoveForTest = false;
			ZeroMouse();
		}, [this]() { RequireSelectedSpeed(45.0f); });
		AddStep(TEXT("Wait for stable native NoCapture before sending wheel"), 0.0,
			[this]() { BeginCaptureScope(); }, [this]()
		{
			Require(TEXT("Capture loss is confirmed across at least two focused world frames"),
				UncapturedWorldFrames >= 2 && HasUncapturedInputViewport());
			RequireSelectedSpeed(45.0f);
			Test.AddInfo(TEXT("Native NoCapture is stable; gameplay input remains enabled. ") + CaptureDiagnostics());
		}, [this]()
		{
			UncapturedWorldFrames = HasUncapturedInputViewport() ? UncapturedWorldFrames + 1 : 0;
		}, false, [this]() { return UncapturedWorldFrames >= 2; });
		AddStep(TEXT("Lost native viewport capture rejects wheel"), 0.15, [this]()
		{
			Require(*FString::Printf(TEXT("Wheel is injected only while native capture is absent. %s"), *CaptureDiagnostics()),
				HasUncapturedInputViewport());
			if (!bAbort)
			{
				Test.AddInfo(TEXT("Sending two actual wheel steps while uncaptured. ") + CaptureDiagnostics());
				Send(EKeys::MouseWheelAxis, IE_Axis, 2.0f);
			}
		}, [this]()
		{
			Require(TEXT("Wheel rejection was observed while native capture was actually absent"),
				HasUncapturedInputViewport());
			Test.AddInfo(TEXT("Observed post-wheel state before capture restoration. ") + CaptureDiagnostics());
			RequireSelectedSpeed(45.0f);
		}, [this]()
		{
			Require(*FString::Printf(TEXT("Native capture must remain absent throughout wheel processing. %s"), *CaptureDiagnostics()),
				HasUncapturedInputViewport());
		});
		AddStep(TEXT("Restored capture cannot replay old wheel"), 0.15, [this]()
		{
			// Do not flush or zero axes: this phase must expose any buffered wheel replay.
			bWaitingForCaptureRestore = true;
			RecapturedWorldFrames = 0;
			RestoreCaptureScope();
		}, [this]()
		{
			const UGameViewportClient* Client = CaptureScopeViewport.Get();
			Require(TEXT("Native viewport capture can be restored"), Client && Client->Viewport
				&& Client->Viewport->HasFocus() && Client->Viewport->HasMouseCapture());
			Require(TEXT("Capture scope restores the original viewport modes and cursor policy"),
				Client && Client->GetMouseCaptureMode() == SavedMouseCaptureMode
				&& Client->GetMouseLockMode() == SavedMouseLockMode
				&& Client->HideCursorDuringCapture() == bSavedHideCursorDuringCapture
				&& Player->bShowMouseCursor == bSavedShowMouseCursor);
			RequireSelectedSpeed(45.0f);
			bWaitingForCaptureRestore = false;
			Test.AddInfo(TEXT("Original native capture and viewport modes restored without flushing wheel input. ") + CaptureDiagnostics());
		}, [this]()
		{
			RecapturedWorldFrames = HasRestoredCapture() ? RecapturedWorldFrames + 1 : 0;
		}, false, [this]() { return RecapturedWorldFrames >= 2; });
		WheelStep(TEXT("Wheel binding still works after capture restoration"), 1.0f, 50.0f);
		WheelStep(TEXT("Restore 45 m/s using actual wheel input"), -1.0f, 45.0f);
		AddStep(TEXT("Native possession restart discards queued wheel"), 0.2, [this]()
		{
			RequireSelectedSpeed(45.0f);
			Send(EKeys::MouseWheelAxis, IE_Axis, 3.0f);
			const FKeyState* PendingBefore = Player->PlayerInput
				? Player->PlayerInput->GetKeyState(EKeys::MouseWheelAxis) : nullptr;
			Test.AddInfo(TEXT("Before native possession restart: ") + PendingWheelDiagnostics());
			Require(TEXT("Possession trial really queues three unprocessed wheel steps"),
				PendingBefore && FMath::IsNearlyEqual(PendingBefore->RawValueAccumulator.X, 3.0, 0.000001)
				&& PendingBefore->SampleCountAccumulator > 0);
			if (bAbort) { return; }
			// Exercise actual possession hooks without setting any simulation state or manually ticking input.
			Player->UnPossess();
			Player->Possess(Plane.Get());
			const FKeyState* PendingAfter = Player->PlayerInput
				? Player->PlayerInput->GetKeyState(EKeys::MouseWheelAxis) : nullptr;
			Test.AddInfo(TEXT("After native possession restart, before another input frame: ") + PendingWheelDiagnostics());
			Require(TEXT("Production possession restart discards the controller's pending wheel accumulator"),
				Player->PlayerInput && (!PendingAfter || (PendingAfter->RawValueAccumulator.IsNearlyZero(0.000001)
					&& PendingAfter->SampleCountAccumulator == 0)));
		}, [this]()
		{
			Test.AddInfo(TEXT("Possession restart after elapsed world frames: ") + PendingWheelDiagnostics());
			Require(TEXT("Original aircraft is possessed again"), Player->GetPawn() == Plane.Get());
			RequireSelectedSpeed(45.0f);
			RequireChaseView(TEXT("Possession restart resets the independent orbit"));
		});
		WheelStep(TEXT("Fresh wheel input works after possession restart"), 1.0f, 50.0f);
		WheelStep(TEXT("Restore 45 m/s after the possession positive control"), -1.0f, 45.0f);
		AddStep(TEXT("Home prepares body-axis input trial"), 0.0, [this]() { Press(EKeys::Home); }, [this]()
		{
			RequireSelectedSpeed(40.0f);
			Release(EKeys::Home);
		}, {}, true);
		AddStep(TEXT("Body-axis trial input settles after Home"), 0.15, [this]() { ZeroMouse(); }, {});
		HoldControl(TEXT("Actual S establishes pitch"), EKeys::S, 0.25, [this]()
		{
			Require(TEXT("Actual input pitched the aircraft"), Plane->GetActorRotation().Pitch > 5.0);
		});
		HoldControl(TEXT("Actual E establishes heading"), EKeys::E, 0.8, [this]()
		{
			Require(TEXT("Actual input yawed the aircraft"), Plane->GetActorRotation().Yaw > 10.0);
		});
		HoldControl(TEXT("Actual D establishes bank"), EKeys::D, 0.5, [this]()
		{
			Require(TEXT("Actual input banked the aircraft"), Plane->GetActorRotation().Roll > 20.0);
		});
		BodyAxisStep(TEXT("Actual banked S uses aircraft-local pitch with camera orbit"), EKeys::S,
			FRotator(FFlightTuning::PitchRateDegreesPerSecond, 0.0, 0.0));
		BodyAxisStep(TEXT("Actual banked E uses aircraft-local yaw with camera orbit"), EKeys::E,
			FRotator(0.0, FFlightTuning::YawRateDegreesPerSecond, 0.0));
		AddStep(TEXT("Final G parks speed/body-axis fixture"), 0.15, [this]() { Press(EKeys::G); }, [this]()
		{
			Require(TEXT("Final G parks the aircraft"), Plane->IsGodMode());
			RequireSelectedSpeed(40.0f);
			Release(EKeys::G);
		});
	}
};

class FWaitForPossessedDublinPIE final : public IAutomationLatentCommand
{
public:
	explicit FWaitForPossessedDublinPIE(FAutomationTestBase& InTest, bool bInSpeedAndBodyAxesOnly = false)
		: Test(InTest), bSpeedAndBodyAxesOnly(bInSpeedAndBodyAxesOnly)
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
			Scenario = MakeUnique<FActualPlayerInputScenario>(Test, *PIEWorld, *Player, *Plane, bSpeedAndBodyAxesOnly);
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
	bool bSpeedAndBodyAxesOnly = false;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightSpeedBodyAxesPIETest,
	"DublinFlight.PIE.ActualSpeedWheelAndBodyAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightSpeedBodyAxesPIETest::RunTest(const FString& Parameters)
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || EditorWorld->WorldType != EWorldType::Editor
		|| EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin"))
	{
		AddError(TEXT("Open /Game/Maps/Dublin and run this test separately; it starts its own actual-input PIE fixture."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinFlight::Tests::FWaitForPossessedDublinPIE(*this, true));
	return true;
}

#endif
