#include "DublinFlightSimulation.h"

namespace DublinFlight
{
namespace
{
uint32 KeyBit(EControlKey Key)
{
	check(static_cast<uint32>(Key) < static_cast<uint32>(EControlKey::Count));
	return 1u << static_cast<uint32>(Key);
}

double AdjustLimitedAngle(double Angle, double Delta, double Limit)
{
	// An unusual captured spawn attitude must not snap merely because a mode changed.
	if (Angle > Limit)
	{
		return FMath::Clamp(Angle + FMath::Min(Delta, 0.0), -Limit, Angle);
	}
	if (Angle < -Limit)
	{
		return FMath::Clamp(Angle + FMath::Max(Delta, 0.0), Angle, Limit);
	}
	return FMath::Clamp(Angle + Delta, -Limit, Limit);
}
}

void FInputState::SetKey(EControlKey Key, bool bPressed)
{
	const uint32 Bit = KeyBit(Key);
	if (bPressed)
	{
		HeldKeys |= Bit;
	}
	else
	{
		HeldKeys &= ~Bit;
		SuppressedKeys &= ~Bit;
	}
}

bool FInputState::IsHeld(EControlKey Key) const
{
	return (HeldKeys & KeyBit(Key)) != 0;
}

void FInputState::SuppressHeldKeys()
{
	SuppressedKeys |= HeldKeys;
}

void FInputState::Clear()
{
	HeldKeys = 0;
	SuppressedKeys = 0;
}

FControlInput FInputState::BuildInput(EFlightMode Mode, const FVector2D& MouseDelta) const
{
	const uint32 ActiveKeys = HeldKeys & ~SuppressedKeys;
	const auto Down = [ActiveKeys](EControlKey Key) -> double
	{
		return (ActiveKeys & KeyBit(Key)) != 0 ? 1.0 : 0.0;
	};

	FControlInput Input;
	if (Mode == EFlightMode::Flight)
	{
		Input.Pitch = Down(EControlKey::Backward) - Down(EControlKey::Forward);
		Input.Roll = Down(EControlKey::Right) - Down(EControlKey::Left);
		Input.Yaw = Down(EControlKey::YawRight) - Down(EControlKey::YawLeft);
		Input.Throttle = FMath::Max(Down(EControlKey::ThrottleUpLeft), Down(EControlKey::ThrottleUpRight))
			- FMath::Max(Down(EControlKey::ThrottleDownLeft), Down(EControlKey::ThrottleDownRight));
	}
	else
	{
		Input.Forward = Down(EControlKey::Forward) - Down(EControlKey::Backward);
		Input.Right = Down(EControlKey::Right) - Down(EControlKey::Left);
		Input.Up = Down(EControlKey::Up) - Down(EControlKey::Down);
		Input.AimYawDegrees = MouseDelta.X * FFlightTuning::MouseDegreesPerUnit;
		Input.AimPitchDegrees = MouseDelta.Y * FFlightTuning::MouseDegreesPerUnit;
	}
	return Input;
}

bool FFlightSimulation::IsFinitePosition(const FVector& PositionCm)
{
	return FMath::IsFinite(PositionCm.X) && FMath::IsFinite(PositionCm.Y) && FMath::IsFinite(PositionCm.Z)
		&& PositionCm.GetAbsMax() <= FFlightTuning::MaxPositionCm;
}

bool FFlightSimulation::IsFiniteOrientation(const FQuat& Orientation)
{
	return FMath::IsFinite(Orientation.X) && FMath::IsFinite(Orientation.Y)
		&& FMath::IsFinite(Orientation.Z) && FMath::IsFinite(Orientation.W)
		&& FMath::IsFinite(Orientation.SizeSquared()) && Orientation.SizeSquared() > UE_SMALL_NUMBER;
}

double FFlightSimulation::HeadingFromYaw(double YawDegrees)
{
	if (!FMath::IsFinite(YawDegrees))
	{
		return 0.0;
	}
	return FMath::Fmod(FMath::Fmod(YawDegrees, 360.0) + 450.0, 360.0);
}

bool FFlightSimulation::Initialize(const FVector& PositionCm, const FQuat& Orientation)
{
	const bool bPositionValid = IsFinitePosition(PositionCm);
	const bool bOrientationValid = IsFiniteOrientation(Orientation);
	SpawnPositionCm = bPositionValid ? PositionCm : FVector::ZeroVector;
	SpawnOrientation = bOrientationValid ? Orientation.GetNormalized() : FQuat::Identity;
	Reset();
	return bPositionValid && bOrientationValid;
}

void FFlightSimulation::SetGodMode(bool bGodMode)
{
	State.Mode = bGodMode ? EFlightMode::God : EFlightMode::Flight;
	State.VelocityCmPerSecond = bGodMode ? FVector::ZeroVector
		: State.Orientation.GetForwardVector() * State.FlightSpeedCmPerSecond;
}

void FFlightSimulation::ToggleGodMode()
{
	SetGodMode(State.Mode != EFlightMode::God);
}

void FFlightSimulation::Reset()
{
	State = FFlightState();
	State.PositionCm = SpawnPositionCm;
	State.Orientation = SpawnOrientation;
	State.VelocityCmPerSecond = State.Orientation.GetForwardVector() * State.FlightSpeedCmPerSecond;
}

FControlInput FFlightSimulation::SanitizeInput(const FControlInput& Input, bool& bChanged)
{
	const auto Sanitize = [&bChanged](double Value, double Limit)
	{
		if (!FMath::IsFinite(Value))
		{
			bChanged = true;
			return 0.0;
		}
		const double Clamped = FMath::Clamp(Value, -Limit, Limit);
		bChanged |= Clamped != Value;
		return Clamped;
	};

	FControlInput Result;
	Result.Pitch = Sanitize(Input.Pitch, 1.0);
	Result.Roll = Sanitize(Input.Roll, 1.0);
	Result.Yaw = Sanitize(Input.Yaw, 1.0);
	Result.Throttle = Sanitize(Input.Throttle, 1.0);
	Result.Forward = Sanitize(Input.Forward, 1.0);
	Result.Right = Sanitize(Input.Right, 1.0);
	Result.Up = Sanitize(Input.Up, 1.0);
	Result.AimYawDegrees = Sanitize(Input.AimYawDegrees, FFlightTuning::MaxAimDegreesPerFrame);
	Result.AimPitchDegrees = Sanitize(Input.AimPitchDegrees, FFlightTuning::MaxAimDegreesPerFrame);
	return Result;
}

FFlightState FFlightSimulation::Integrate(const FFlightState& Previous, const FControlInput& Input,
	double StepSeconds, double AimFraction)
{
	FFlightState Next = Previous;
	const FRotator Rotation = Previous.Orientation.Rotator();

	if (Previous.Mode == EFlightMode::God)
	{
		const double YawDelta = Input.AimYawDegrees * AimFraction;
		const double Pitch = AdjustLimitedAngle(Rotation.Pitch, Input.AimPitchDegrees * AimFraction,
			FFlightTuning::MaxPitchDegrees);
		if (YawDelta != 0.0 || Pitch != Rotation.Pitch)
		{
			Next.Orientation = FRotator(Pitch, FMath::UnwindDegrees(Rotation.Yaw + YawDelta),
				Rotation.Roll).Quaternion();
		}

		// Yaw-relative horizontal motion: looking up or banking cannot change altitude.
		const FQuat Heading = FRotator(0.0, Rotation.Yaw + YawDelta * 0.5, 0.0).Quaternion();
		FVector Direction = Heading.GetForwardVector() * Input.Forward
			+ Heading.GetRightVector() * Input.Right + FVector::UpVector * Input.Up;
		if (Direction.SizeSquared() > 1.0)
		{
			Direction.Normalize();
		}
		Next.VelocityCmPerSecond = Direction * FFlightTuning::GodSpeedCmPerSecond;
		if (!Direction.IsZero())
		{
			Next.PositionCm += Next.VelocityCmPerSecond * StepSeconds;
		}
		return Next;
	}

	const double Pitch = AdjustLimitedAngle(Rotation.Pitch,
		Input.Pitch * FFlightTuning::PitchRateDegreesPerSecond * StepSeconds, FFlightTuning::MaxPitchDegrees);
	const double Roll = AdjustLimitedAngle(Rotation.Roll,
		Input.Roll * FFlightTuning::RollRateDegreesPerSecond * StepSeconds, FFlightTuning::MaxBankDegrees);
	const double YawDelta = (Input.Yaw * FFlightTuning::YawRateDegreesPerSecond
		+ FMath::Sin(FMath::DegreesToRadians((Rotation.Roll + Roll) * 0.5))
			* FFlightTuning::BankTurnRateDegreesPerSecond) * StepSeconds;
	Next.Orientation = FRotator(Pitch, FMath::UnwindDegrees(Rotation.Yaw + YawDelta), Roll).Quaternion();
	const FQuat MidOrientation = FRotator((Rotation.Pitch + Pitch) * 0.5,
		Rotation.Yaw + YawDelta * 0.5, (Rotation.Roll + Roll) * 0.5).Quaternion();

	const double Acceleration = Input.Throttle * FFlightTuning::ThrottleAccelerationCmPerSecondSquared;
	const double StartSpeed = Previous.FlightSpeedCmPerSecond;
	Next.FlightSpeedCmPerSecond = FMath::Clamp(StartSpeed + Acceleration * StepSeconds,
		FFlightTuning::MinSpeedCmPerSecond, FFlightTuning::MaxSpeedCmPerSecond);
	double AcceleratingSeconds = StepSeconds;
	if (Acceleration != 0.0)
	{
		const double Limit = Acceleration > 0.0 ? FFlightTuning::MaxSpeedCmPerSecond : FFlightTuning::MinSpeedCmPerSecond;
		AcceleratingSeconds = FMath::Clamp((Limit - StartSpeed) / Acceleration, 0.0, StepSeconds);
	}
	const double Distance = StartSpeed * AcceleratingSeconds
		+ 0.5 * Acceleration * FMath::Square(AcceleratingSeconds)
		+ Next.FlightSpeedCmPerSecond * (StepSeconds - AcceleratingSeconds);
	Next.PositionCm += MidOrientation.GetForwardVector() * Distance;
	Next.VelocityCmPerSecond = Next.Orientation.GetForwardVector() * Next.FlightSpeedCmPerSecond;
	return Next;
}

FAdvanceResult FFlightSimulation::Advance(const FControlInput& Input, double DeltaSeconds)
{
	return Advance(Input, DeltaSeconds, [](const FFlightState& Proposed, FVector& ActualPositionCm)
	{
		ActualPositionCm = Proposed.PositionCm;
		return true;
	});
}

FAdvanceResult FFlightSimulation::Advance(const FControlInput& Input, double DeltaSeconds,
	TFunctionRef<bool(const FFlightState& Proposed, FVector& ActualPositionCm)> Move)
{
	FAdvanceResult Result;
	const FControlInput SafeInput = SanitizeInput(Input, Result.bSanitizedInput);
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0)
	{
		Result.bRejectedDeltaTime = true;
		return Result;
	}
	if (DeltaSeconds == 0.0)
	{
		return Result;
	}
	Result.bClampedDeltaTime = DeltaSeconds > FFlightTuning::MaxFrameSeconds;
	const double FrameSeconds = FMath::Min(DeltaSeconds, FFlightTuning::MaxFrameSeconds);
	const int32 NumSteps = FMath::Max(1, FMath::CeilToInt(FrameSeconds / FFlightTuning::MaxSubstepSeconds));
	const double StepSeconds = FrameSeconds / NumSteps;

	for (int32 Index = 0; Index < NumSteps; ++Index)
	{
		FFlightState Next = Integrate(State, SafeInput, StepSeconds, 1.0 / NumSteps);
		if (!IsFinitePosition(Next.PositionCm) || !IsFiniteOrientation(Next.Orientation))
		{
			Result.bRejectedMovement = true;
			State.VelocityCmPerSecond = FVector::ZeroVector;
			break;
		}
		FVector ActualPositionCm = Next.PositionCm;
		const bool bContinue = Move(Next, ActualPositionCm);
		if (!IsFinitePosition(ActualPositionCm))
		{
			Result.bRejectedMovement = true;
			State.VelocityCmPerSecond = FVector::ZeroVector;
			break;
		}
		Next.PositionCm = ActualPositionCm;
		State = Next;
		++Result.Substeps;
		Result.SimulatedSeconds += StepSeconds;
		if (!bContinue)
		{
			Result.bMovementBlocked = true;
			State.VelocityCmPerSecond = FVector::ZeroVector;
			break;
		}
	}
	return Result;
}
}
