#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

namespace DublinFlight
{
enum class EFlightMode : uint8
{
	Flight,
	God
};

enum class EControlKey : uint8
{
	Forward,
	Backward,
	Left,
	Right,
	YawLeft,
	YawRight,
	ThrottleUpLeft,
	ThrottleUpRight,
	ThrottleDownLeft,
	ThrottleDownRight,
	Up,
	Down,
	Count
};

struct FFlightTuning
{
	static constexpr double StartSpeedCmPerSecond = 4000.0;
	static constexpr double MinSpeedCmPerSecond = 500.0;
	static constexpr double MaxSpeedCmPerSecond = 10000.0;
	static constexpr double WheelSpeedStepCmPerSecond = 500.0;
	static constexpr double ThrottleAccelerationCmPerSecondSquared = 1000.0;
	static constexpr double PitchRateDegreesPerSecond = 40.0;
	static constexpr double RollRateDegreesPerSecond = 65.0;
	static constexpr double YawRateDegreesPerSecond = 25.0;
	static constexpr double BankTurnRateDegreesPerSecond = 30.0;
	static constexpr double MaxPitchDegrees = 80.0;
	static constexpr double GodSpeedCmPerSecond = 3000.0;
	static constexpr double MouseDegreesPerUnit = 0.12;
	static constexpr double MaxAimDegreesPerFrame = 90.0;
	static constexpr double MaxSubstepSeconds = 1.0 / 120.0;
	static constexpr double MaxFrameSeconds = 1.0;
	static constexpr double MaxPositionCm = 1.0e9;
};

struct FControlInput
{
	double Pitch = 0.0;
	double Roll = 0.0;
	double Yaw = 0.0;
	double Throttle = 0.0;
	double Forward = 0.0;
	double Right = 0.0;
	double Up = 0.0;
	// Mouse displacement in degrees for the whole frame, not an angular velocity.
	double AimYawDegrees = 0.0;
	double AimPitchDegrees = 0.0;
	// Wheel displacement for the whole frame; consumed once, only in flight.
	double SpeedSteps = 0.0;
};

class DUBLINFLIGHT_API FInputState
{
public:
	void SetKey(EControlKey Key, bool bPressed);
	bool IsHeld(EControlKey Key) const;
	void SuppressHeldKeys();
	void Clear();
	FControlInput BuildInput(EFlightMode Mode, const FVector2D& MouseDelta = FVector2D::ZeroVector,
		double SpeedSteps = 0.0) const;

private:
	uint32 HeldKeys = 0;
	uint32 SuppressedKeys = 0;
};

struct FFlightState
{
	FVector PositionCm = FVector::ZeroVector;
	FQuat Orientation = FQuat::Identity;
	FVector VelocityCmPerSecond = FVector::ForwardVector * FFlightTuning::StartSpeedCmPerSecond;
	double FlightSpeedCmPerSecond = FFlightTuning::StartSpeedCmPerSecond;
	EFlightMode Mode = EFlightMode::Flight;
};

struct FAdvanceResult
{
	int32 Substeps = 0;
	double SimulatedSeconds = 0.0;
	bool bRejectedDeltaTime = false;
	bool bClampedDeltaTime = false;
	bool bSanitizedInput = false;
	bool bRejectedMovement = false;
	bool bMovementBlocked = false;
};

// No UObject, world, input subsystem, or presentation dependency.
class DUBLINFLIGHT_API FFlightSimulation
{
public:
	// Invalid spawn data is replaced by a finite origin/identity and reported as false.
	bool Initialize(const FVector& PositionCm, const FQuat& Orientation);
	void SetGodMode(bool bGodMode);
	void ToggleGodMode();
	void Reset();

	const FFlightState& GetState() const { return State; }
	FTransform GetSpawnTransform() const { return FTransform(SpawnOrientation, SpawnPositionCm); }

	FAdvanceResult Advance(const FControlInput& Input, double DeltaSeconds);
	// The mover returns the swept position, and false to stop this frame on collision.
	FAdvanceResult Advance(const FControlInput& Input, double DeltaSeconds,
		TFunctionRef<bool(const FFlightState& Proposed, FVector& ActualPositionCm)> Move);

	static bool IsFinitePosition(const FVector& PositionCm);
	static bool IsFiniteOrientation(const FQuat& Orientation);
	// Dublin survey basis: +X east, +Y south, +Z up; north is yaw -90.
	static double HeadingFromYaw(double YawDegrees);

private:
	FFlightState State;
	FVector SpawnPositionCm = FVector::ZeroVector;
	FQuat SpawnOrientation = FQuat::Identity;

	static FControlInput SanitizeInput(const FControlInput& Input, bool& bChanged);
	static FFlightState Integrate(const FFlightState& Previous, const FControlInput& Input,
		double StepSeconds, double AimFraction);
};
}
