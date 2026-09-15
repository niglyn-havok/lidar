#include "DublinFlightSimulation.h"

#if WITH_DEV_AUTOMATION_TESTS

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
	Tests::AdvanceFor(Simulation, Input, 4.0);
	TestNearlyEqual(TEXT("Throttle saturates at 65 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 6500.0);
	TestNearlyEqual(TEXT("Integrates acceleration then cruise without distance overshoot"),
		Simulation.GetState().PositionCm.X, 22875.0, 0.001);
	Input.Throttle = -1.0;
	Tests::AdvanceFor(Simulation, Input, 6.0);
	TestNearlyEqual(TEXT("Throttle saturates at 20 m/s"), Simulation.GetState().FlightSpeedCmPerSecond, 2000.0);
	TestNearlyEqual(TEXT("Velocity matches speed floor"), Simulation.GetState().VelocityCmPerSecond.Size(), 2000.0);
	Tests::AdvanceFor(Simulation, FControlInput(), 1.0);
	TestNearlyEqual(TEXT("Released throttle holds selected cruise"), Simulation.GetState().FlightSpeedCmPerSecond, 2000.0);
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
	Tests::AdvanceFor(Coarse, Input, 10.0);
	TestNearlyEqual(TEXT("Pitch is limited"), Coarse.GetState().Orientation.Rotator().Pitch,
		FFlightTuning::MaxPitchDegrees, 0.001);
	TestNearlyEqual(TEXT("Bank is limited"), Coarse.GetState().Orientation.Rotator().Roll,
		FFlightTuning::MaxBankDegrees, 0.001);
	TestTrue(TEXT("Orientation remains normalized"), Coarse.GetState().Orientation.IsNormalized());
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

#endif
