#include "DublinFlightSimulation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DublinFlightPawn.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace DublinFlight::Tests
{
constexpr EAutomationTestFlags PureFlags = EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

void AdvanceFor(FFlightSimulation& Simulation, const FControlInput& Input, double Seconds, int32 Hz = 60)
{
	const int32 Frames = FMath::RoundToInt(Seconds * Hz);
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		Simulation.Advance(Input, 1.0 / Hz);
	}
}

void CheckBodyLocalControl(FAutomationTestBase& Test, const TCHAR* ControlName, EControlKey Key,
	const FRotator& LocalDegreesPerSecond)
{
	const double StepSeconds = FFlightTuning::MaxSubstepSeconds;
	const FQuat ExpectedLocalDelta = (LocalDegreesPerSecond * StepSeconds).Quaternion();
	for (const FRotator Attitude : { FRotator(0.0, 67.0, 0.0), FRotator(0.0, 67.0, 45.0),
		FRotator(25.0, 123.0, -50.0), FRotator(-30.0, -142.0, 55.0) })
	{
		const FString Context = FString::Printf(TEXT("%s at %s"), ControlName, *Attitude.ToString());
		FFlightSimulation Idle;
		FFlightSimulation Controlled;
		const FVector Spawn(0.0, 0.0, 18000.0);
		Test.TestTrue(Context + TEXT(": idle spawn accepted"), Idle.Initialize(Spawn, Attitude.Quaternion()));
		Test.TestTrue(Context + TEXT(": controlled spawn accepted"), Controlled.Initialize(Spawn, Attitude.Quaternion()));
		FInputState Keys;
		Keys.SetKey(Key, true);
		const FAdvanceResult IdleResult = Idle.Advance(FControlInput(), StepSeconds);
		const FAdvanceResult ControlledResult = Controlled.Advance(Keys.BuildInput(EFlightMode::Flight), StepSeconds);
		Test.TestEqual(Context + TEXT(": idle uses exactly one real integration substep"), IdleResult.Substeps, 1);
		Test.TestEqual(Context + TEXT(": control uses exactly one real integration substep"), ControlledResult.Substeps, 1);
		Test.TestFalse(Context + TEXT(": valid keyboard input is not sanitized"), ControlledResult.bSanitizedInput);

		// Cancel the common bank-turn assist with the idle result, not a hardcoded assist formula.
		// One substep stays clear of Euler limits; 0.005 degrees permits second-order assist/order effects.
		const FQuat LocalDelta = (Idle.GetState().Orientation.Inverse() * Controlled.GetState().Orientation).GetNormalized();
		const double ErrorDegrees = FMath::RadiansToDegrees(LocalDelta.AngularDistance(ExpectedLocalDelta));
		Test.TestTrue(FString::Printf(TEXT("%s: control rotates about the aircraft body axis (error %.6f deg <= 0.005)"),
			*Context, ErrorDegrees), ErrorDegrees <= 0.005);
		Test.TestNearlyEqual(Context + TEXT(": local angular speed retains the configured control rate"),
			FMath::RadiansToDegrees(LocalDelta.AngularDistance(FQuat::Identity)),
			FMath::RadiansToDegrees(ExpectedLocalDelta.AngularDistance(FQuat::Identity)), 0.005);
		Test.TestTrue(Context + TEXT(": controlled attitude remains normalized"), Controlled.GetState().Orientation.IsNormalized());
		Test.TestNearlyEqual(Context + TEXT(": steering preserves selected speed"),
			Controlled.GetState().FlightSpeedCmPerSecond, Idle.GetState().FlightSpeedCmPerSecond, 0.000001);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightInitialStateTest,
	"DublinFlight.Flight.InitialStateUnitsAndCompass", DublinFlight::Tests::PureFlags)

bool FDublinFlightInitialStateTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FFlightSimulation Simulation;
	const FVector Spawn(-28000.0, 8000.0, 18000.0);
	TestTrue(TEXT("Dublin mid-air spawn is valid"), Simulation.Initialize(Spawn, FQuat::Identity));
	TestFalse(TEXT("Initial mode is flight"), Simulation.GetState().Mode == EFlightMode::God);
	TestNearlyEqual(TEXT("Initial speed is 40 m/s"), Simulation.GetState().FlightSpeedCmPerSecond / 100.0, 40.0);
	TestNearlyEqual(TEXT("Initial altitude is 180 m"), Simulation.GetState().PositionCm.Z / 100.0, 180.0);
	Simulation.Advance(FControlInput(), 0.5);
	TestNearlyEqual(TEXT("Half-second cruise travels 20 metres east"),
		Simulation.GetState().PositionCm, Spawn + FVector(2000.0, 0.0, 0.0), 0.001f);
	TestNearlyEqual(TEXT("Flight velocity uses centimetres"), Simulation.GetState().VelocityCmPerSecond,
		FVector(4000.0, 0.0, 0.0), 0.001f);
	TestNearlyEqual(TEXT("+X east"), FFlightSimulation::HeadingFromYaw(0.0), 90.0);
	TestNearlyEqual(TEXT("+Y south"), FFlightSimulation::HeadingFromYaw(90.0), 180.0);
	TestNearlyEqual(TEXT("-X west"), FFlightSimulation::HeadingFromYaw(180.0), 270.0);
	TestNearlyEqual(TEXT("-Y north"), FFlightSimulation::HeadingFromYaw(-90.0), 0.0);
	TestNearlyEqual(TEXT("Wrapped north"), FFlightSimulation::HeadingFromYaw(270.0), 0.0);
	TestNearlyEqual(TEXT("Negative turns wrap"), FFlightSimulation::HeadingFromYaw(-720.0), 90.0);
	TestNearlyEqual(TEXT("Positive turns wrap"), FFlightSimulation::HeadingFromYaw(1080.0), 90.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightThrottleTest,
	"DublinFlight.Flight.ThrottleBoundsAndDistance", DublinFlight::Tests::PureFlags)

bool FDublinFlightThrottleTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FFlightSimulation Simulation;
	Simulation.Initialize(FVector::ZeroVector, FQuat::Identity);
	FControlInput Input;
	Input.Throttle = 1.0;
	Tests::AdvanceFor(Simulation, Input, 8.0);
	TestNearlyEqual(TEXT("Throttle saturates at 100 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 10000.0);
	TestNearlyEqual(TEXT("Integrates acceleration then cruise without distance overshoot"),
		Simulation.GetState().PositionCm.X, 62000.0, 0.001);
	Input.Throttle = -1.0;
	Tests::AdvanceFor(Simulation, Input, 12.0);
	TestNearlyEqual(TEXT("Throttle saturates at 5 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 500.0);
	TestNearlyEqual(TEXT("Velocity matches speed floor"), Simulation.GetState().VelocityCmPerSecond.Size(), 500.0);
	Tests::AdvanceFor(Simulation, FControlInput(), 1.0);
	TestNearlyEqual(TEXT("Released throttle holds selected cruise"), Simulation.GetState().FlightSpeedCmPerSecond, 500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightAttitudeTest,
	"DublinFlight.Flight.AttitudeAndFramePartition", DublinFlight::Tests::PureFlags)

bool FDublinFlightAttitudeTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FControlInput Input;
	Input.Pitch = 0.4;
	Input.Roll = 0.5;
	Input.Yaw = 0.3;
	Input.Throttle = 0.25;
	FFlightSimulation Coarse;
	FFlightSimulation Fine;
	Coarse.Initialize(FVector(0.0, 0.0, 18000.0), FQuat::Identity);
	Fine.Initialize(FVector(0.0, 0.0, 18000.0), FQuat::Identity);
	Tests::AdvanceFor(Coarse, Input, 1.0, 30);
	Tests::AdvanceFor(Fine, Input, 1.0, 120);
	TestNearlyEqual(TEXT("30 and 120 Hz integrate the same distance"),
		Coarse.GetState().PositionCm, Fine.GetState().PositionCm, 0.01f);
	TestTrue(TEXT("30 and 120 Hz integrate the same attitude"),
		Coarse.GetState().Orientation.Equals(Fine.GetState().Orientation, 0.00001));
	TestTrue(TEXT("Pitch raises the nose"), Coarse.GetState().Orientation.Rotator().Pitch > 10.0);
	TestTrue(TEXT("Roll banks"), Coarse.GetState().Orientation.Rotator().Roll > 20.0);
	TestTrue(TEXT("Yaw and bank turn heading"), Coarse.GetState().Orientation.Rotator().Yaw > 5.0);
	TestTrue(TEXT("Pitch produces climb"), Coarse.GetState().PositionCm.Z > 18000.0);
	Input.Pitch = 1.0;
	Input.Roll = 1.0;
	bool bStayedInsidePitchGuard = true;
	bool bRolledBeyondFormerLimit = false;
	for (int32 Frame = 0; Frame < 1200; ++Frame)
	{
		Coarse.Advance(Input, 1.0 / 120.0);
		const FRotator Attitude = Coarse.GetState().Orientation.Rotator();
		bStayedInsidePitchGuard &= FMath::Abs(Attitude.Pitch) <= FFlightTuning::MaxPitchDegrees + 0.001;
		bRolledBeyondFormerLimit |= FMath::Abs(Attitude.Roll) > 90.0;
	}
	TestTrue(TEXT("Combined body-axis input respects the pitch envelope"),
		bStayedInsidePitchGuard);
	TestTrue(TEXT("Combined steering cannot freeze roll at the former bank limit"), bRolledBeyondFormerLimit);
	TestTrue(TEXT("Orientation remains normalized"), Coarse.GetState().Orientation.IsNormalized());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightBodyPitchTest,
	"DublinFlight.Flight.BodyAxes.PitchAfterBankAndHeading", DublinFlight::Tests::PureFlags)

bool FDublinFlightBodyPitchTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	Tests::CheckBodyLocalControl(*this, TEXT("S nose up"), EControlKey::Backward,
		FRotator(FFlightTuning::PitchRateDegreesPerSecond, 0.0, 0.0));
	Tests::CheckBodyLocalControl(*this, TEXT("W nose down"), EControlKey::Forward,
		FRotator(-FFlightTuning::PitchRateDegreesPerSecond, 0.0, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightBodyYawTest,
	"DublinFlight.Flight.BodyAxes.YawAfterBankPitchAndHeading", DublinFlight::Tests::PureFlags)

bool FDublinFlightBodyYawTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	Tests::CheckBodyLocalControl(*this, TEXT("E yaw right"), EControlKey::YawRight,
		FRotator(0.0, FFlightTuning::YawRateDegreesPerSecond, 0.0));
	Tests::CheckBodyLocalControl(*this, TEXT("Q yaw left"), EControlKey::YawLeft,
		FRotator(0.0, -FFlightTuning::YawRateDegreesPerSecond, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightBodyRollControlTest,
	"DublinFlight.Flight.BodyAxes.RollPositiveControl", DublinFlight::Tests::PureFlags)

bool FDublinFlightBodyRollControlTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	Tests::CheckBodyLocalControl(*this, TEXT("D bank right"), EControlKey::Right,
		FRotator(0.0, 0.0, FFlightTuning::RollRateDegreesPerSecond));
	Tests::CheckBodyLocalControl(*this, TEXT("A bank left"), EControlKey::Left,
		FRotator(0.0, 0.0, -FFlightTuning::RollRateDegreesPerSecond));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightBodyAxesOrbitIndependenceTest,
	"DublinFlight.Flight.BodyAxes.CameraOrbitDoesNotChangeFlightControls", DublinFlight::Tests::PureFlags)

bool FDublinFlightBodyAxesOrbitIndependenceTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	const FVector Spawn(0.0, 0.0, 18000.0);
	const FQuat Orientation = FRotator(25.0, 123.0, -50.0).Quaternion();
	FFlightSimulation Chase;
	FFlightSimulation Orbited;
	TestTrue(TEXT("Chase fixture accepts banked, pitched, yawed spawn"), Chase.Initialize(Spawn, Orientation));
	TestTrue(TEXT("Orbit fixture accepts the same aircraft pose"), Orbited.Initialize(Spawn, Orientation));
	FInputState Keys;
	Keys.SetKey(EControlKey::Forward, true);
	Keys.SetKey(EControlKey::Right, true);
	Keys.SetKey(EControlKey::YawLeft, true);
	FCameraOrbit Orbit;
	const FCameraOrbit DefaultChase;
	for (int32 Frame = 0; Frame < 12; ++Frame)
	{
		const FVector2D Mouse(30.0, 20.0);
		TestTrue(TEXT("Independent camera accepts mouse while the aircraft is banked"),
			Orbit.ApplyMouseDelta(Mouse, Orbited.GetState().Orientation.Rotator()));
		const FControlInput WithMouse = Keys.BuildInput(EFlightMode::Flight, Mouse);
		TestNearlyEqual(TEXT("Flight camera mouse cannot become aircraft pitch aim"), WithMouse.AimPitchDegrees, 0.0);
		TestNearlyEqual(TEXT("Flight camera mouse cannot become aircraft yaw aim"), WithMouse.AimYawDegrees, 0.0);
		Chase.Advance(Keys.BuildInput(EFlightMode::Flight), FFlightTuning::MaxSubstepSeconds);
		Orbited.Advance(WithMouse, FFlightTuning::MaxSubstepSeconds);
		TestTrue(TEXT("Same keyboard controls produce identical aircraft orientation with either camera"),
			Chase.GetState().Orientation.Equals(Orbited.GetState().Orientation, 0.000001));
		TestNearlyEqual(TEXT("Orbit does not redirect aircraft translation"),
			Chase.GetState().PositionCm, Orbited.GetState().PositionCm, 0.000001f);
		TestNearlyEqual(TEXT("Orbit does not redirect aircraft velocity"),
			Chase.GetState().VelocityCmPerSecond, Orbited.GetState().VelocityCmPerSecond, 0.000001f);
	}
	TestFalse(TEXT("Camera independence trial really changed the view, not just the mouse input"),
		Orbit.GetRotation(Orbited.GetState().Orientation.Rotator()).Equals(
			DefaultChase.GetRotation(Chase.GetState().Orientation.Rotator()), 0.01));
	TestFalse(TEXT("Keyboard trial really changed aircraft attitude"), Chase.GetState().Orientation.Equals(Orientation, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightFormerBankLimitPitchTest,
	"DublinFlight.Flight.BodyAxes.PitchAtFormerBankLimit", DublinFlight::Tests::PureFlags)

bool FDublinFlightFormerBankLimitPitchTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double Bank : { -70.0, 70.0 })
	{
		for (const double Sign : { -1.0, 1.0 })
		{
			FFlightSimulation Idle;
			FFlightSimulation Controlled;
			const FQuat Start = FRotator(0.0, 63.0, Bank).Quaternion();
			Idle.Initialize(FVector::ZeroVector, Start);
			Controlled.Initialize(FVector::ZeroVector, Start);
			FInputState Keys;
			Keys.SetKey(Sign > 0.0 ? EControlKey::Backward : EControlKey::Forward, true);
			Idle.Advance(FControlInput(), FFlightTuning::MaxSubstepSeconds);
			Controlled.Advance(Keys.BuildInput(EFlightMode::Flight), FFlightTuning::MaxSubstepSeconds);
			const FQuat Local = (Idle.GetState().Orientation.Inverse() * Controlled.GetState().Orientation).GetNormalized();
			const double Degrees = Sign * FFlightTuning::PitchRateDegreesPerSecond * FFlightTuning::MaxSubstepSeconds;
			const double Error = FMath::RadiansToDegrees(Local.AngularDistance(FRotator(Degrees, 0.0, 0.0).Quaternion()));
			TestTrue(FString::Printf(TEXT("Bank %.0f: %s applies full correct body-local pitch, not a bank-limit freeze (error %.6f deg)"),
				Bank, Sign > 0.0 ? TEXT("S") : TEXT("W"), Error), Error <= 0.005);
			TestTrue(TEXT("Pitch command is measurably nonzero at the former maximum bank"),
				FMath::RadiansToDegrees(Local.AngularDistance(FQuat::Identity)) >= FMath::Abs(Degrees) * 0.99);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightMixedBankControlsTest,
	"DublinFlight.Flight.BodyAxes.MixedBankControlsRemainResponsive", DublinFlight::Tests::PureFlags)

bool FDublinFlightMixedBankControlsTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double BankSign : { -1.0, 1.0 })
	{
		for (const FRotator Rate : { FRotator(40.0, 0.0, 0.0), FRotator(-40.0, 0.0, 0.0),
			FRotator(0.0, 25.0, 0.0), FRotator(0.0, -25.0, 0.0) })
		{
			FFlightSimulation RollOnly;
			FFlightSimulation Mixed;
			const FQuat Start = FRotator(0.0, 63.0, BankSign * 70.0).Quaternion();
			RollOnly.Initialize(FVector::ZeroVector, Start);
			Mixed.Initialize(FVector::ZeroVector, Start);
			FControlInput Input;
			Input.Roll = BankSign;
			RollOnly.Advance(Input, FFlightTuning::MaxSubstepSeconds);
			Input.Pitch = Rate.Pitch / FFlightTuning::PitchRateDegreesPerSecond;
			Input.Yaw = Rate.Yaw / FFlightTuning::YawRateDegreesPerSecond;
			Mixed.Advance(Input, FFlightTuning::MaxSubstepSeconds);
			const FQuat Local = (RollOnly.GetState().Orientation.Inverse() * Mixed.GetState().Orientation).GetNormalized();
			const FQuat Expected = (Rate * FFlightTuning::MaxSubstepSeconds).Quaternion();
			const double Error = FMath::RadiansToDegrees(Local.AngularDistance(Expected));
			TestTrue(FString::Printf(TEXT("Held %s at bank %.0f cannot freeze valid pitch/yaw %s (error %.6f deg)"),
				BankSign > 0.0 ? TEXT("D") : TEXT("A"), BankSign * 70.0, *Rate.ToString(), Error), Error <= 0.005);
			TestTrue(TEXT("Mixed control retains its full usable-axis displacement"),
				Local.AngularDistance(FQuat::Identity) >= Expected.AngularDistance(FQuat::Identity) * 0.99);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightBodyEnvelopeTest,
	"DublinFlight.Flight.BodyAxes.QuaternionEnvelopeAndReversal", DublinFlight::Tests::PureFlags)

bool FDublinFlightBodyEnvelopeTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double Sign : { -1.0, 1.0 })
	{
		FFlightSimulation Simulation;
		Simulation.Initialize(FVector::ZeroVector, FRotator(Sign * 79.9, 37.0, 0.0).Quaternion());
		FControlInput Input;
		Input.Pitch = Sign;
		Tests::AdvanceFor(Simulation, Input, 0.5);
		TestNearlyEqual(TEXT("Single-axis pitch reaches and holds the vertical guard"),
			Simulation.GetState().Orientation.Rotator().Pitch, Sign * 80.0, 0.001);
		Input.Pitch = -Sign;
		Simulation.Advance(Input, FFlightTuning::MaxSubstepSeconds);
		TestTrue(TEXT("Countersteering responds immediately at the vertical guard"),
			Sign * Simulation.GetState().Orientation.Rotator().Pitch < 79.9);
		for (const bool bRoll : { false, true })
		{
			FFlightSimulation FreeAxis;
			FFlightSimulation Mixed;
			const FQuat Start = FRotator(Sign * 80.0, 35.0, 0.0).Quaternion();
			FreeAxis.Initialize(FVector::ZeroVector, Start);
			Mixed.Initialize(FVector::ZeroVector, Start);
			FControlInput Turn;
			Turn.Roll = bRoll ? 1.0 : 0.0;
			Turn.Yaw = bRoll ? 0.0 : 1.0;
			FreeAxis.Advance(Turn, FFlightTuning::MaxSubstepSeconds);
			Turn.Pitch = Sign;
			Mixed.Advance(Turn, FFlightTuning::MaxSubstepSeconds);
			const double Rate = bRoll ? FFlightTuning::RollRateDegreesPerSecond : FFlightTuning::YawRateDegreesPerSecond;
			TestTrue(TEXT("A usable roll/yaw command really moves the aircraft at the pitch guard"),
				FMath::RadiansToDegrees(Start.AngularDistance(FreeAxis.GetState().Orientation))
					> Rate * FFlightTuning::MaxSubstepSeconds * 0.99);
			TestTrue(TEXT("Blocked outward pitch cannot reduce an independent usable roll/yaw command"),
				FMath::RadiansToDegrees(FreeAxis.GetState().Orientation.AngularDistance(Mixed.GetState().Orientation)) < 0.005);
			TestTrue(TEXT("Independent commands still respect the vertical pitch guard"),
				FMath::Abs(Mixed.GetState().Orientation.Rotator().Pitch) <= 80.001);
		}
	}
	FFlightSimulation Unusual;
	Unusual.Initialize(FVector::ZeroVector, FRotator(85.0, 20.0, 0.0).Quaternion());
	Unusual.Advance(FControlInput(), 0.1);
	TestNearlyEqual(TEXT("An unusual captured spawn does not snap to the envelope"),
		Unusual.GetState().Orientation.Rotator().Pitch, 85.0, 0.001);
	FControlInput Turn;
	Turn.Pitch = -1.0;
	Unusual.Advance(Turn, 0.1);
	TestNearlyEqual(TEXT("An out-of-envelope spawn can steer back toward the allowed attitude"),
		Unusual.GetState().Orientation.Rotator().Pitch, 81.0, 0.001);
	Turn.Pitch = 1.0;
	Unusual.Advance(Turn, 0.1);
	TestNearlyEqual(TEXT("Out-of-envelope steering cannot increase the existing excess"),
		Unusual.GetState().Orientation.Rotator().Pitch, 81.0, 0.001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightInvertedBodyAxesTest,
	"DublinFlight.Flight.BodyAxes.InvertedBodyAxes", DublinFlight::Tests::PureFlags)

bool FDublinFlightInvertedBodyAxesTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const FRotator Attitude : { FRotator(20.0, 123.0, 135.0), FRotator(-25.0, -73.0, -150.0), FRotator(0.0, 67.0, 180.0) })
	{
		for (const FRotator Rate : { FRotator(40.0, 0.0, 0.0), FRotator(-40.0, 0.0, 0.0),
			FRotator(0.0, 25.0, 0.0), FRotator(0.0, -25.0, 0.0), FRotator(0.0, 0.0, 65.0), FRotator(0.0, 0.0, -65.0) })
		{
			FFlightSimulation Idle;
			FFlightSimulation Controlled;
			Idle.Initialize(FVector::ZeroVector, Attitude.Quaternion());
			Controlled.Initialize(FVector::ZeroVector, Attitude.Quaternion());
			FControlInput Input;
			Input.Pitch = Rate.Pitch / FFlightTuning::PitchRateDegreesPerSecond;
			Input.Yaw = Rate.Yaw / FFlightTuning::YawRateDegreesPerSecond;
			Input.Roll = Rate.Roll / FFlightTuning::RollRateDegreesPerSecond;
			Idle.Advance(FControlInput(), FFlightTuning::MaxSubstepSeconds);
			Controlled.Advance(Input, FFlightTuning::MaxSubstepSeconds);
			const FQuat Local = (Idle.GetState().Orientation.Inverse() * Controlled.GetState().Orientation).GetNormalized();
			const double Error = FMath::RadiansToDegrees(Local.AngularDistance((Rate * FFlightTuning::MaxSubstepSeconds).Quaternion()));
			TestTrue(FString::Printf(TEXT("Inverted attitude %s retains signed body-local rate %s (error %.6f deg)"),
				*Attitude.ToString(), *Rate.ToString(), Error), Error <= 0.005);
			TestTrue(TEXT("Inverted controls remain finite and normalized"),
				FFlightSimulation::IsFinitePosition(Controlled.GetState().PositionCm) && Controlled.GetState().Orientation.IsNormalized());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightFullRollPartitionTest,
	"DublinFlight.Flight.BodyAxes.FullRollFramePartitionAndFiniteCamera", DublinFlight::Tests::PureFlags)

bool FDublinFlightFullRollPartitionTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double Sign : { -1.0, 1.0 })
	{
		FFlightSimulation Coarse;
		FFlightSimulation Fine;
		const FRotator Start(20.0, 43.0, 30.0);
		Coarse.Initialize(FVector(0, 0, 18000), Start.Quaternion());
		Fine.Initialize(FVector(0, 0, 18000), Start.Quaternion());
		FControlInput Input;
		Input.Roll = Sign;
		FCameraOrbit Orbit;
		Orbit.ApplyMouseDelta(FVector2D(20, 10), Start);
		FRotator PreviousCamera = Orbit.GetRotation(Start);
		bool bSawInverted = false;
		bool bFiniteAndContinuous = true;
		for (int32 Frame = 0; Frame < 720; ++Frame)
		{
			Fine.Advance(Input, 1.0 / 120.0);
			const FFlightState& State = Fine.GetState();
			const FRotator Camera = Orbit.GetRotation(State.Orientation.Rotator());
			bSawInverted |= FVector::DotProduct(State.Orientation.GetUpVector(), FVector::UpVector) < -0.5;
			bFiniteAndContinuous &= FFlightSimulation::IsFinitePosition(State.PositionCm) && State.Orientation.IsNormalized()
				&& !Camera.ContainsNaN() && FMath::Abs(State.Orientation.Rotator().Pitch) <= 80.001
				&& FMath::Abs(FMath::FindDeltaAngleDegrees(PreviousCamera.Yaw, Camera.Yaw)) < 0.26;
			PreviousCamera = Camera;
		}
		Tests::AdvanceFor(Coarse, Input, 6.0, 30);
		TestTrue(TEXT("Full roll actually passes through inverted flight"), bSawInverted);
		TestTrue(TEXT("Full roll stays finite without a chase-camera heading-pole jump"), bFiniteAndContinuous);
		TestNearlyEqual(TEXT("Roll completes 390 degrees rather than stopping at a bank limit"),
			Fine.GetState().Orientation.Rotator().Roll, FRotator::NormalizeAxis(30.0 + Sign * 390.0), 0.001);
		TestNearlyEqual(TEXT("Body roll alone preserves nose elevation"), Fine.GetState().Orientation.Rotator().Pitch, 20.0, 0.001);
		TestTrue(TEXT("30/120 Hz partitions produce the same full-roll attitude"),
			Coarse.GetState().Orientation.Equals(Fine.GetState().Orientation, 0.00001));
		TestNearlyEqual(TEXT("30/120 Hz partitions produce the same full-roll flight path"),
			Coarse.GetState().PositionCm, Fine.GetState().PositionCm, 0.01f);
		const double BeforeReverse = Fine.GetState().Orientation.Rotator().Roll;
		Input.Roll = -Sign;
		Fine.Advance(Input, 0.2);
		TestNearlyEqual(TEXT("Roll reverses immediately after a complete revolution"),
			FMath::FindDeltaAngleDegrees(BeforeReverse, Fine.GetState().Orientation.Rotator().Roll), -Sign * 13.0, 0.001);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightBankAssistTest,
	"DublinFlight.Flight.BodyAxes.BankAssistRemainsSeparate", DublinFlight::Tests::PureFlags)

bool FDublinFlightBankAssistTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double Bank : { -45.0, 0.0, 45.0 })
	{
		FFlightSimulation Simulation;
		const FRotator Start(20.0, 35.0, Bank);
		Simulation.Initialize(FVector::ZeroVector, Start.Quaternion());
		Simulation.Advance(FControlInput(), 0.5);
		const FRotator End = Simulation.GetState().Orientation.Rotator();
		TestNearlyEqual(TEXT("Idle bank still creates the intended world-heading assist"),
			FMath::FindDeltaAngleDegrees(Start.Yaw, End.Yaw),
			FMath::Sin(FMath::DegreesToRadians(Bank)) * FFlightTuning::BankTurnRateDegreesPerSecond * 0.5, 0.001);
		TestNearlyEqual(TEXT("Assist alone does not pitch the aircraft"), End.Pitch, Start.Pitch, 0.001);
		TestNearlyEqual(TEXT("Assist alone does not level the bank"), End.Roll, Start.Roll, 0.001);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightWheelDisplacementTest,
	"DublinFlight.Flight.Speed.WheelDisplacementOncePerFrame", DublinFlight::Tests::PureFlags)

bool FDublinFlightWheelDisplacementTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double Seconds : { 1.0 / 240.0, 1.0 / 30.0, 0.5, 1.0 })
	{
		FFlightSimulation Simulation;
		Simulation.Initialize(FVector::ZeroVector, FQuat::Identity);
		FControlInput Input;
		Input.SpeedSteps = 3.0;
		Simulation.Advance(Input, Seconds);
		TestNearlyEqual(TEXT("Three wheel steps add 15 m/s regardless of frame duration/substeps"),
			Simulation.GetState().FlightSpeedCmPerSecond, 5500.0, 0.000001);
		TestNearlyEqual(TEXT("Wheel-selected speed applies before the first movement substep"),
			Simulation.GetState().PositionCm.X, 5500.0 * Seconds, 0.001);
		TestNearlyEqual(TEXT("Actual velocity immediately matches selected speed"),
			Simulation.GetState().VelocityCmPerSecond.Size(), 5500.0, 0.001);
		Simulation.Advance(FControlInput(), Seconds);
		TestNearlyEqual(TEXT("A consumed wheel displacement never repeats"), Simulation.GetState().FlightSpeedCmPerSecond, 5500.0);
	}
	FFlightSimulation Combined;
	Combined.Initialize(FVector::ZeroVector, FQuat::Identity);
	FControlInput Input;
	Input.SpeedSteps = 1.0;
	Input.Throttle = 1.0;
	Combined.Advance(Input, 0.5);
	TestNearlyEqual(TEXT("Wheel and held throttle modify one selected speed"), Combined.GetState().FlightSpeedCmPerSecond, 5000.0);
	TestNearlyEqual(TEXT("Wheel is immediate and held throttle retains 10 m/s each second"), Combined.GetState().PositionCm.X, 2375.0, 0.001);
	Input.SpeedSteps = 19.0;
	Combined.Advance(Input, 0.5);
	TestNearlyEqual(TEXT("Wheel/throttle cannot exceed 100 m/s"), Combined.GetState().FlightSpeedCmPerSecond, 10000.0);
	Input.SpeedSteps = -19.0;
	Input.Throttle = -1.0;
	Combined.Advance(Input, 0.5);
	TestNearlyEqual(TEXT("Wheel/throttle cannot fall below 5 m/s"), Combined.GetState().FlightSpeedCmPerSecond, 500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightWheelGuardsTest,
	"DublinFlight.Flight.Speed.WheelGuardsGodModeAndReset", DublinFlight::Tests::PureFlags)

bool FDublinFlightWheelGuardsTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FFlightSimulation Simulation;
	Simulation.Initialize(FVector::ZeroVector, FQuat::Identity);
	FControlInput Wheel;
	Wheel.SpeedSteps = 1.0;
	for (const double InvalidTime : { -0.1, 0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
	{
		Simulation.Advance(Wheel, InvalidTime);
		TestNearlyEqual(TEXT("Rejected/zero frame time cannot apply a wheel command"), Simulation.GetState().FlightSpeedCmPerSecond, 4000.0);
	}
	for (const double Invalid : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
	{
		Wheel.SpeedSteps = Invalid;
		TestTrue(TEXT("Nonfinite wheel input is reported"), Simulation.Advance(Wheel, 0.01).bSanitizedInput);
		TestNearlyEqual(TEXT("Nonfinite wheel cannot poison speed"), Simulation.GetState().FlightSpeedCmPerSecond, 4000.0);
	}
	Wheel.SpeedSteps = std::numeric_limits<double>::max();
	TestTrue(TEXT("Excessive finite wheel displacement is bounded and reported"), Simulation.Advance(Wheel, 0.01).bSanitizedInput);
	TestNearlyEqual(TEXT("Huge wheel input stays at the finite speed ceiling"), Simulation.GetState().FlightSpeedCmPerSecond, 10000.0);
	Wheel.SpeedSteps = -std::numeric_limits<double>::max();
	TestTrue(TEXT("Excessive reverse wheel displacement is bounded and reported"), Simulation.Advance(Wheel, 0.01).bSanitizedInput);
	TestNearlyEqual(TEXT("Huge reverse wheel input stays at the positive speed floor"), Simulation.GetState().FlightSpeedCmPerSecond, 500.0);
	Simulation.Reset();
	Wheel.SpeedSteps = 1.0;
	Simulation.Advance(Wheel, 0.01);
	TestNearlyEqual(TEXT("One wheel step selects 45 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 4500.0);
	Simulation.SetGodMode(true);
	Wheel.SpeedSteps = 19.0;
	Wheel.Throttle = 1.0;
	Wheel.Forward = 1.0;
	Simulation.Advance(Wheel, 0.5);
	TestNearlyEqual(TEXT("God ignores wheel/throttle and retains resume speed"), Simulation.GetState().FlightSpeedCmPerSecond, 4500.0);
	TestNearlyEqual(TEXT("God translation retains its own 30 m/s movement"), Simulation.GetState().VelocityCmPerSecond.Size(), 3000.0);
	Simulation.ToggleGodMode();
	Simulation.Advance(FControlInput(), 0.01);
	TestNearlyEqual(TEXT("No wheel command carries across a mode transition"), Simulation.GetState().FlightSpeedCmPerSecond, 4500.0);
	Simulation.Reset();
	TestNearlyEqual(TEXT("Home restores selected speed to 40 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 4000.0);
	Wheel = FControlInput();
	Wheel.SpeedSteps = 5.0;
	const FAdvanceResult Rejected = Simulation.Advance(Wheel, 0.1,
		[](const FFlightState& Proposed, FVector& Actual)
		{
			Actual.X = std::numeric_limits<double>::quiet_NaN();
			return true;
		});
	TestTrue(TEXT("Wheel-selected movement still passes through the existing finite mover guard"), Rejected.bRejectedMovement);
	TestNearlyEqual(TEXT("Rejected proposed movement cannot commit a wheel speed"), Simulation.GetState().FlightSpeedCmPerSecond, 4000.0);
	const FInputState Keys;
	TestNearlyEqual(TEXT("Flight maps wheel displacement, not elapsed time"),
		Keys.BuildInput(EFlightMode::Flight, FVector2D::ZeroVector, -2.0).SpeedSteps, -2.0);
	TestNearlyEqual(TEXT("God input mapping discards wheel before simulation"),
		Keys.BuildInput(EFlightMode::God, FVector2D::ZeroVector, 19.0).SpeedSteps, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightModeResetTest,
	"DublinFlight.Flight.ModeTransitionsAndReset", DublinFlight::Tests::PureFlags)

bool FDublinFlightModeResetTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FFlightSimulation Simulation;
	const FVector Spawn(-28000.0, 8000.0, 18000.0);
	const FQuat SpawnOrientation = FRotator(4.0, 30.0, -10.0).Quaternion();
	Simulation.Initialize(Spawn, SpawnOrientation);
	FControlInput Throttle;
	Throttle.Throttle = 1.0;
	Tests::AdvanceFor(Simulation, Throttle, 1.0);
	const FFlightState BeforeGod = Simulation.GetState();
	Simulation.ToggleGodMode();
	TestTrue(TEXT("G enters god mode"), Simulation.GetState().Mode == EFlightMode::God);
	TestTrue(TEXT("Toggle does not teleport"), Simulation.GetState().PositionCm == BeforeGod.PositionCm);
	TestTrue(TEXT("Toggle preserves orientation"), Simulation.GetState().Orientation == BeforeGod.Orientation);
	TestTrue(TEXT("Toggle stops all inertia immediately"), Simulation.GetState().VelocityCmPerSecond.IsZero());
	FControlInput Reposition;
	Reposition.Right = 1.0;
	Reposition.AimYawDegrees = 20.0;
	Simulation.Advance(Reposition, 0.5);
	const FFlightState BeforeResume = Simulation.GetState();
	Simulation.ToggleGodMode();
	TestTrue(TEXT("Resume preserves repositioned location"), Simulation.GetState().PositionCm == BeforeResume.PositionCm);
	TestTrue(TEXT("Resume preserves aimed attitude"), Simulation.GetState().Orientation == BeforeResume.Orientation);
	TestNearlyEqual(TEXT("Resume retains throttle"), Simulation.GetState().FlightSpeedCmPerSecond, 5000.0);
	Simulation.Advance(FControlInput(), 0.1);
	TestTrue(TEXT("Resume flies forward"), FVector::DotProduct(
		Simulation.GetState().PositionCm - BeforeResume.PositionCm, BeforeResume.Orientation.GetForwardVector()) > 450.0);
	Simulation.Reset();
	TestTrue(TEXT("Home restores captured position exactly"), Simulation.GetState().PositionCm == Spawn);
	TestTrue(TEXT("Home restores captured orientation"), Simulation.GetState().Orientation.Equals(SpawnOrientation, 0.000001));
	TestTrue(TEXT("Home returns to flight"), Simulation.GetState().Mode == EFlightMode::Flight);
	TestNearlyEqual(TEXT("Home restores 40 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 4000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightGodModeTest,
	"DublinFlight.Flight.GodStationaryAimingAndTranslation", DublinFlight::Tests::PureFlags)

bool FDublinFlightGodModeTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FFlightSimulation Simulation;
	const FVector Spawn(-28000.0, 8000.0, 18000.0);
	Simulation.Initialize(Spawn, FRotator(30.0, 0.0, 25.0).Quaternion());
	Simulation.SetGodMode(true);
	Tests::AdvanceFor(Simulation, FControlInput(), 5.0);
	TestTrue(TEXT("Five-second god drift is exactly zero"), Simulation.GetState().PositionCm == Spawn);
	TestTrue(TEXT("God has zero velocity"), Simulation.GetState().VelocityCmPerSecond.IsZero());
	FControlInput Aim;
	Aim.AimYawDegrees = 30.0;
	Aim.AimPitchDegrees = 15.0;
	Simulation.Advance(Aim, 0.5);
	TestTrue(TEXT("Aiming cannot move the aircraft"), Simulation.GetState().PositionCm == Spawn);
	TestNearlyEqual(TEXT("Mouse yaw displacement applied once, not per substep"),
		Simulation.GetState().Orientation.Rotator().Yaw, 30.0, 0.001);
	TestNearlyEqual(TEXT("Mouse pitch displacement applied once"),
		Simulation.GetState().Orientation.Rotator().Pitch, 45.0, 0.001);
	FControlInput Move;
	Move.Forward = 1.0;
	Move.Right = 1.0;
	Simulation.Advance(Move, 1.0);
	TestNearlyEqual(TEXT("Horizontal reposition is independent of pitch/bank"), Simulation.GetState().PositionCm.Z, Spawn.Z);
	TestNearlyEqual(TEXT("Diagonal movement is speed limited"),
		FVector::Distance(Simulation.GetState().PositionCm, Spawn), FFlightTuning::GodSpeedCmPerSecond, 0.001);
	const FVector BeforeStop = Simulation.GetState().PositionCm;
	Simulation.Advance(FControlInput(), 0.5);
	TestTrue(TEXT("Release stops without coast"), Simulation.GetState().PositionCm == BeforeStop);
	Move = FControlInput();
	Move.Up = 1.0;
	Simulation.Advance(Move, 0.5);
	TestNearlyEqual(TEXT("Vertical reposition uses world Z"), Simulation.GetState().PositionCm,
		BeforeStop + FVector(0.0, 0.0, 1500.0), 0.001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightInputStateTest,
	"DublinFlight.Flight.InputMappingAndHeldKeySuppression", DublinFlight::Tests::PureFlags)

bool FDublinFlightInputStateTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FInputState Keys;
	Keys.SetKey(EControlKey::Forward, true);
	Keys.SetKey(EControlKey::Right, true);
	Keys.SetKey(EControlKey::YawRight, true);
	Keys.SetKey(EControlKey::ThrottleUpLeft, true);
	FControlInput Input = Keys.BuildInput(EFlightMode::Flight);
	TestNearlyEqual(TEXT("W pitches down"), Input.Pitch, -1.0);
	TestNearlyEqual(TEXT("D banks right"), Input.Roll, 1.0);
	TestNearlyEqual(TEXT("E yaws right"), Input.Yaw, 1.0);
	TestNearlyEqual(TEXT("Shift increases throttle"), Input.Throttle, 1.0);
	Keys.SetKey(EControlKey::ThrottleUpRight, true);
	TestNearlyEqual(TEXT("Two Shift keys do not double throttle"), Keys.BuildInput(EFlightMode::Flight).Throttle, 1.0);
	Keys.SuppressHeldKeys();
	Input = Keys.BuildInput(EFlightMode::God);
	TestNearlyEqual(TEXT("Held W cannot carry into god translation"), Input.Forward, 0.0);
	TestNearlyEqual(TEXT("Held D cannot carry into god strafe"), Input.Right, 0.0);
	Keys.SetKey(EControlKey::Forward, true);
	TestNearlyEqual(TEXT("Repeated press cannot unsuppress"), Keys.BuildInput(EFlightMode::God).Forward, 0.0);
	Keys.SetKey(EControlKey::Forward, false);
	Keys.SetKey(EControlKey::Forward, true);
	TestNearlyEqual(TEXT("Release then press re-arms"), Keys.BuildInput(EFlightMode::God).Forward, 1.0);
	Keys.SetKey(EControlKey::Backward, true);
	TestNearlyEqual(TEXT("Opposing inputs cancel"), Keys.BuildInput(EFlightMode::God).Forward, 0.0);
	Keys.Clear();
	Keys.SetKey(EControlKey::Up, true);
	Keys.SetKey(EControlKey::Down, true);
	Input = Keys.BuildInput(EFlightMode::God, FVector2D(10.0, -5.0));
	TestNearlyEqual(TEXT("Opposing vertical inputs cancel"), Input.Up, 0.0);
	TestNearlyEqual(TEXT("Mouse X maps to yaw degrees"), Input.AimYawDegrees, 1.2);
	TestNearlyEqual(TEXT("Mouse Y maps to pitch degrees"), Input.AimPitchDegrees, -0.6);
	Input = Keys.BuildInput(EFlightMode::Flight, FVector2D(10.0, -5.0));
	TestNearlyEqual(TEXT("Flight mouse cannot become aircraft yaw input"), Input.AimYawDegrees, 0.0);
	TestNearlyEqual(TEXT("Flight mouse cannot become aircraft pitch input"), Input.AimPitchDegrees, 0.0);
	Keys.Clear();
	TestFalse(TEXT("Focus loss clears held state"), Keys.IsHeld(EControlKey::Up));
	TestNearlyEqual(TEXT("Clear removes throttle"), Keys.BuildInput(EFlightMode::Flight).Throttle, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightGuardsTest,
	"DublinFlight.Flight.FiniteGuardsAndBoundedSubsteps", DublinFlight::Tests::PureFlags)

bool FDublinFlightGuardsTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Infinity = std::numeric_limits<double>::infinity();
	FFlightSimulation Simulation;
	// Assign corrupt components directly so diagnostic constructors do not assert
	// before the application's finite-input guard can be exercised.
	FVector InvalidPosition = FVector::ZeroVector;
	InvalidPosition.X = NaN;
	FQuat InvalidOrientation = FQuat::Identity;
	InvalidOrientation.X = Infinity;
	TestFalse(TEXT("Invalid spawn is reported"), Simulation.Initialize(InvalidPosition, FQuat(0.0, 0.0, 0.0, 0.0)));
	TestTrue(TEXT("Invalid spawn fallback is finite"), FFlightSimulation::IsFinitePosition(Simulation.GetState().PositionCm));
	TestTrue(TEXT("Invalid orientation fallback is normalized"), Simulation.GetState().Orientation.IsNormalized());
	TestFalse(TEXT("Infinite quaternion rejected"), FFlightSimulation::IsFiniteOrientation(InvalidOrientation));
	TestFalse(TEXT("Position range bounded"), FFlightSimulation::IsFinitePosition(FVector(1.1e9, 0.0, 0.0)));
	TestNearlyEqual(TEXT("Invalid heading has finite fallback"), FFlightSimulation::HeadingFromYaw(NaN), 0.0);
	const FVector Before = Simulation.GetState().PositionCm;
	for (double InvalidDelta : { -0.1, NaN, Infinity })
	{
		TestTrue(TEXT("Invalid frame time rejected"), Simulation.Advance(FControlInput(), InvalidDelta).bRejectedDeltaTime);
	}
	TestTrue(TEXT("Invalid delta never moves"), Simulation.GetState().PositionCm == Before);
	TestEqual(TEXT("Zero frame time never integrates"), Simulation.Advance(FControlInput(), 0.0).Substeps, 0);
	FControlInput InvalidInput;
	InvalidInput.Pitch = NaN;
	InvalidInput.Roll = Infinity;
	InvalidInput.Throttle = 100.0;
	TestTrue(TEXT("Nonfinite and excessive inputs sanitized"), Simulation.Advance(InvalidInput, 0.01).bSanitizedInput);
	TestTrue(TEXT("Sanitized movement remains finite"), FFlightSimulation::IsFinitePosition(Simulation.GetState().PositionCm));
	Simulation.Reset();
	int32 MoveCount = 0;
	double LargestMove = 0.0;
	FVector LastPosition = Simulation.GetState().PositionCm;
	const FAdvanceResult Normal = Simulation.Advance(FControlInput(), 0.25,
		[&](const FFlightState& Proposed, FVector& Actual)
		{
			++MoveCount;
			LargestMove = FMath::Max(LargestMove, FVector::Distance(Proposed.PositionCm, LastPosition));
			Actual = Proposed.PositionCm;
			LastPosition = Actual;
			return true;
		});
	TestNearlyEqual(TEXT("Normal long frame time is not discarded"), Normal.SimulatedSeconds, 0.25, 0.000001);
	TestFalse(TEXT("Normal frame is not capped"), Normal.bClampedDeltaTime);
	TestEqual(TEXT("Every substep invokes swept movement"), MoveCount, Normal.Substeps);
	TestTrue(TEXT("Sweep steps bounded to 1/120 second"), LargestMove <= 4000.0 / 120.0 + 0.001);
	const FAdvanceResult Hitch = Simulation.Advance(FControlInput(), 10.0);
	TestTrue(TEXT("Pathological hitch capped"), Hitch.bClampedDeltaTime);
	TestNearlyEqual(TEXT("Hitch simulates bounded time"), Hitch.SimulatedSeconds, FFlightTuning::MaxFrameSeconds);
	TestTrue(TEXT("Hitch substeps bounded"), Hitch.Substeps <= 120);
	Simulation.Initialize(FVector(FFlightTuning::MaxPositionCm, 0.0, 0.0), FQuat::Identity);
	TestTrue(TEXT("Out-of-range proposed movement rejected"), Simulation.Advance(FControlInput(), 0.1).bRejectedMovement);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightCollisionContractTest,
	"DublinFlight.Flight.SweptMovementCallbackContract", DublinFlight::Tests::PureFlags)

bool FDublinFlightCollisionContractTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FFlightSimulation Simulation;
	Simulation.Initialize(FVector::ZeroVector, FQuat::Identity);
	int32 Calls = 0;
	const FVector Contact(10.0, 0.0, 0.0);
	const FAdvanceResult Blocked = Simulation.Advance(FControlInput(), 0.5,
		[&](const FFlightState& Proposed, FVector& Actual)
		{
			++Calls;
			Actual = Contact;
			return false;
		});
	TestTrue(TEXT("Collision reported"), Blocked.bMovementBlocked);
	TestEqual(TEXT("Collision terminates remaining substeps"), Calls, 1);
	TestTrue(TEXT("State uses swept contact, not requested endpoint"), Simulation.GetState().PositionCm == Contact);
	TestTrue(TEXT("Collision removes velocity"), Simulation.GetState().VelocityCmPerSecond.IsZero());
	const FAdvanceResult InvalidMove = Simulation.Advance(FControlInput(), 0.1,
		[](const FFlightState& Proposed, FVector& Actual)
		{
			Actual.X = std::numeric_limits<double>::quiet_NaN();
			return true;
		});
	TestTrue(TEXT("Nonfinite mover result rejected"), InvalidMove.bRejectedMovement);
	TestTrue(TEXT("Bad mover cannot poison last valid position"), Simulation.GetState().PositionCm == Contact);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitDefaultsTest,
	"DublinFlight.Flight.CameraOrbit.DefaultChaseAndReset", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FCameraOrbit Orbit;
	for (const FRotator Aircraft : { FRotator::ZeroRotator, FRotator(20.0, 45.0, 35.0),
		FRotator(-80.0, -170.0, -70.0), FRotator(80.0, 170.0, 70.0) })
	{
		const FRotator Expected(FMath::Clamp(Aircraft.Pitch - 28.0, -85.0, 85.0), Aircraft.Yaw, 0.0);
		TestTrue(TEXT("Zero orbit preserves the bounded chase bias and ignores bank"),
			Orbit.GetRotation(Aircraft).Equals(Expected, 0.000001));
		TestTrue(TEXT("Zero mouse is accepted"), Orbit.ApplyMouseDelta(FVector2D::ZeroVector, Aircraft));
		TestTrue(TEXT("Zero mouse preserves the default view"), Orbit.GetRotation(Aircraft).Equals(Expected, 0.000001));
		TestTrue(TEXT("Reset fixture accumulates orbit"), Orbit.ApplyMouseDelta(FVector2D(100.0, 100.0), Aircraft));
		TestFalse(TEXT("Reset fixture is not already centered"), Orbit.GetRotation(Aircraft).Equals(Expected, 0.000001));
		Orbit.Reset();
		TestTrue(TEXT("Reset removes both offsets"), Orbit.GetRotation(Aircraft).Equals(Expected, 0.000001));
		Orbit.Reset();
		TestTrue(TEXT("Reset is idempotent"), Orbit.GetRotation(Aircraft).Equals(Expected, 0.000001));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitAccumulationTest,
	"DublinFlight.Flight.CameraOrbit.AccumulationAndHeading", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitAccumulationTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FCameraOrbit Orbit;
	const FRotator Aircraft(10.0, 30.0, 40.0);
	TestTrue(TEXT("Finite mouse displacement accepted"), Orbit.ApplyMouseDelta(FVector2D(100.0, 50.0), Aircraft));
	TestTrue(TEXT("Positive X/Y add yaw/pitch at 0.36 degrees per unit"),
		Orbit.GetRotation(Aircraft).Equals(FRotator(0.0, 66.0, 0.0), 0.000001));
	TestTrue(TEXT("Second displacement accepted"), Orbit.ApplyMouseDelta(FVector2D(-25.0, -100.0), Aircraft));
	TestTrue(TEXT("Offsets accumulate with signed displacement"),
		Orbit.GetRotation(Aircraft).Equals(FRotator(-36.0, 57.0, 0.0), 0.000001));
	const FRotator TurnedAircraft(25.0, -175.0, -60.0);
	TestTrue(TEXT("Orbit follows aircraft heading and pitch, never bank"),
		Orbit.GetRotation(TurnedAircraft).Equals(FRotator(-21.0, -148.0, 0.0), 0.000001));
	TestTrue(TEXT("Turning the aircraft does not consume the stored orbit"),
		Orbit.GetRotation(Aircraft).Equals(FRotator(-36.0, 57.0, 0.0), 0.000001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitSensitivityTest,
	"DublinFlight.Flight.CameraOrbit.ThreeTimesSensitivityPreservesGodAim", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitSensitivityTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	TestNearlyEqual(TEXT("Flight orbit has an independently named 0.36 degree tuning"),
		FCameraOrbit::OrbitMouseDegreesPerUnit, 0.36, 0.000001);
	TestNearlyEqual(TEXT("God aim tuning remains 0.12 degrees per unit"),
		FFlightTuning::MouseDegreesPerUnit, 0.12, 0.000001);
	const FVector2D Mouse(100.0, 50.0);
	const FInputState Keys;
	const FControlInput Aim = Keys.BuildInput(EFlightMode::God, Mouse);
	TestNearlyEqual(TEXT("Unchanged God mouse yaw mapping"), Aim.AimYawDegrees, 12.0, 0.000001);
	TestNearlyEqual(TEXT("Unchanged God mouse pitch mapping"), Aim.AimPitchDegrees, 6.0, 0.000001);
	for (const double DeltaSeconds : { 1.0 / 30.0, 1.0 / 120.0 })
	{
		FCameraOrbit Orbit;
		TestTrue(TEXT("Flight displacement accepted"), Orbit.ApplyMouseDelta(Mouse, FRotator::ZeroRotator));
		const FRotator View = Orbit.GetRotation(FRotator::ZeroRotator);
		TestNearlyEqual(TEXT("Flight yaw is exactly three times unchanged God input"), View.Yaw, Aim.AimYawDegrees * 3.0, 0.000001);
		TestNearlyEqual(TEXT("Flight pitch is exactly three times unchanged God input"), View.Pitch + 28.0, Aim.AimPitchDegrees * 3.0, 0.000001);
		FFlightSimulation God;
		God.Initialize(FVector::ZeroVector, FQuat::Identity);
		God.SetGodMode(true);
		God.Advance(Aim, DeltaSeconds);
		TestNearlyEqual(TEXT("God yaw is not scaled by orbit tuning or frame time"), God.GetState().Orientation.Rotator().Yaw, 12.0, 0.000001);
		TestNearlyEqual(TEXT("God pitch is not scaled by orbit tuning or frame time"), God.GetState().Orientation.Rotator().Pitch, 6.0, 0.000001);
		God.Advance(FControlInput(), DeltaSeconds);
		TestNearlyEqual(TEXT("God displacement is consumed once"), God.GetState().Orientation.Rotator().Yaw, 12.0, 0.000001);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitYawTest,
	"DublinFlight.Flight.CameraOrbit.YawWrapAndFrameLimit", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitYawTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FCameraOrbit Orbit;
	const FRotator Aircraft(0.0, 175.0, 0.0);
	TestTrue(TEXT("Positive yaw crosses the wrap"), Orbit.ApplyMouseDelta(FVector2D(100.0, 0.0), Aircraft));
	TestNearlyEqual(TEXT("Yaw is normalized across +180"), Orbit.GetRotation(Aircraft).Yaw, -149.0, 0.000001);
	TestTrue(TEXT("Negative yaw crosses back"), Orbit.ApplyMouseDelta(FVector2D(-200.0, 0.0), Aircraft));
	TestNearlyEqual(TEXT("Yaw is normalized across -180"),
		Orbit.GetRotation(FRotator(0.0, -175.0, 0.0)).Yaw, 149.0, 0.000001);
	Orbit.Reset();
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		TestTrue(TEXT("Repeated turns accepted"), Orbit.ApplyMouseDelta(FVector2D(100.0, 0.0), Aircraft));
	}
	TestNearlyEqual(TEXT("Accumulated yaw wraps through complete turns"), Orbit.GetRotation(Aircraft).Yaw, 175.0, 0.000001);
	Orbit.Reset();
	const double LargestFinite = std::numeric_limits<double>::max();
	TestTrue(TEXT("Huge finite displacement is bounded, not rejected"),
		Orbit.ApplyMouseDelta(FVector2D(LargestFinite, LargestFinite), FRotator::ZeroRotator));
	TestNearlyEqual(TEXT("Yaw displacement is limited to 90 degrees per frame"),
		Orbit.GetRotation(FRotator::ZeroRotator).Yaw, 90.0, 0.000001);
	TestNearlyEqual(TEXT("Pitch displacement is limited to 90 degrees per frame"),
		Orbit.GetRotation(FRotator::ZeroRotator).Pitch, 62.0, 0.000001);
	TestTrue(TEXT("Negative huge displacement accepted"),
		Orbit.ApplyMouseDelta(FVector2D(-LargestFinite, -LargestFinite), FRotator::ZeroRotator));
	TestTrue(TEXT("Negative frame limits reverse without overflow"),
		Orbit.GetRotation(FRotator::ZeroRotator).Equals(FRotator(-28.0, 0.0, 0.0), 0.000001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitPitchTest,
	"DublinFlight.Flight.CameraOrbit.PitchLimitsAndImmediateReversal", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitPitchTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	for (const double Sign : { -1.0, 1.0 })
	{
		FCameraOrbit Orbit;
		for (int32 Frame = 0; Frame < 8; ++Frame)
		{
			TestTrue(TEXT("Pitch limit accepts repeated mouse input"),
				Orbit.ApplyMouseDelta(FVector2D(0.0, Sign * 10000.0), FRotator::ZeroRotator));
		}
		TestNearlyEqual(TEXT("World pitch clamps at the requested pole"),
			Orbit.GetRotation(FRotator::ZeroRotator).Pitch, Sign * 85.0, 0.000001);
		TestTrue(TEXT("Small reverse input accepted"),
			Orbit.ApplyMouseDelta(FVector2D(0.0, -Sign * 10.0), FRotator::ZeroRotator));
		TestNearlyEqual(TEXT("Saturation never accumulates a reversal dead zone"),
			Orbit.GetRotation(FRotator::ZeroRotator).Pitch, Sign * 81.4, 0.000001);
		const FRotator PitchedAircraft(Sign * 80.0, 35.0, 60.0);
		TestNearlyEqual(TEXT("Aircraft pitch changes cannot push the camera beyond its world limit"),
			Orbit.GetRotation(PitchedAircraft).Pitch, Sign * 85.0, 0.000001);
		TestTrue(TEXT("Yaw-only input at the pitch limit is accepted"),
			Orbit.ApplyMouseDelta(FVector2D(10.0, 0.0), PitchedAircraft));
		TestNearlyEqual(TEXT("Yaw-only input cannot rebase the stored pitch offset"),
			Orbit.GetRotation(FRotator::ZeroRotator).Pitch, Sign * 81.4, 0.000001);
		TestTrue(TEXT("Reversal after aircraft pitch changes accepted"),
			Orbit.ApplyMouseDelta(FVector2D(0.0, -Sign * 10.0), PitchedAircraft));
		TestNearlyEqual(TEXT("Reversal starts from the visible pitch, not a hidden overshoot"),
			Orbit.GetRotation(PitchedAircraft).Pitch, Sign * 81.4, 0.000001);
	}
	FCameraOrbit Descending;
	const FRotator NoseDown(-80.0, 0.0, 0.0);
	TestTrue(TEXT("Default clamped chase accepts upward mouse"),
		Descending.ApplyMouseDelta(FVector2D(0.0, 10.0), NoseDown));
	TestNearlyEqual(TEXT("Default lower clamp reverses immediately too"),
		Descending.GetRotation(NoseDown).Pitch, -81.4, 0.000001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitPartitionTest,
	"DublinFlight.Flight.CameraOrbit.FramePartitionDisplacement", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitPartitionTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	const FVector2D Displacement(180.0, 120.0);
	for (const int32 Frames : { 1, 2, 30, 60, 120 })
	{
		FCameraOrbit Orbit;
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			TestTrue(TEXT("Unsaturated partition accepted"),
				Orbit.ApplyMouseDelta(Displacement / Frames, FRotator::ZeroRotator));
		}
		TestTrue(FString::Printf(TEXT("Same displacement over %d frames gives the same orbit"), Frames),
			Orbit.GetRotation(FRotator::ZeroRotator).Equals(FRotator(15.2, 64.8, 0.0), 0.000001));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightOrbitFiniteTest,
	"DublinFlight.Flight.CameraOrbit.NonfiniteDoesNotMutate", DublinFlight::Tests::PureFlags)

bool FDublinFlightOrbitFiniteTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight;
	FCameraOrbit Orbit;
	const FRotator Aircraft(15.0, 20.0, 40.0);
	TestTrue(TEXT("Finite guard fixture has nonzero orbit"), Orbit.ApplyMouseDelta(FVector2D(80.0, -60.0), Aircraft));
	const FRotator Before = Orbit.GetRotation(Aircraft);
	for (const double Invalid : { std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() })
	{
		for (int32 Component = 0; Component < 5; ++Component)
		{
			FVector2D Mouse(100.0, 100.0);
			FRotator Base = Aircraft;
			// Avoid diagnostic constructor checks before the finite-input guard.
			switch (Component)
			{
			case 0: Mouse.X = Invalid; break;
			case 1: Mouse.Y = Invalid; break;
			case 2: Base.Pitch = Invalid; break;
			case 3: Base.Yaw = Invalid; break;
			case 4: Base.Roll = Invalid; break;
			}
			TestFalse(TEXT("Nonfinite mouse or aircraft component rejected"), Orbit.ApplyMouseDelta(Mouse, Base));
			TestTrue(TEXT("Rejected input changes neither accumulated axis"), Orbit.GetRotation(Aircraft).Equals(Before, 0.000001));
		}
	}
	TestTrue(TEXT("Valid input still works after rejection"), Orbit.ApplyMouseDelta(FVector2D(-80.0, 60.0), Aircraft));
	TestTrue(TEXT("Rejected samples cannot poison later accumulation"),
		Orbit.GetRotation(Aircraft).Equals(FRotator(-13.0, 20.0, 0.0), 0.000001));
	return true;
}

#endif
