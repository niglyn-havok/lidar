#include "Weapons/DublinWeaponModel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DublinFlightSimulation.h"
#include "Misc/AutomationTest.h"
#include <limits>

namespace DublinWeapons::Tests
{
constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponBallisticsTest,
	"DublinFlight.Weapons.Unit.BallisticGravityAndInheritedVelocity", DublinWeapons::Tests::Flags)
bool FDublinWeaponBallisticsTest::RunTest(const FString& Parameters)
{
	const FVector Start(-28000.0, 8000.0, 18000.0);
	const FVector AircraftVelocity(4000.0, 0.0, 0.0);
	const FVector AtTwo = DublinWeapons::BallisticPosition(Start, AircraftVelocity, -980.0, 2.0);
	TestNearlyEqual(TEXT("Bomb retains forward aircraft velocity"), AtTwo.X - Start.X, 8000.0);
	TestNearlyEqual(TEXT("Bomb falls under world gravity"), AtTwo.Z - Start.Z, -1960.0);
	TestNearlyEqual(TEXT("God-mode bomb has no inherited horizontal motion"),
		DublinWeapons::BallisticPosition(Start, FVector::ZeroVector, -980.0, 2.0).X, Start.X);
	TestTrue(TEXT("Bomb is not an immediate ground AOE"), AtTwo.Z > 0.0);
	TestTrue(TEXT("Invalid trajectory time rejected"),
		DublinWeapons::BallisticPosition(Start, AircraftVelocity, -980.0, -1.0).IsZero());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponYieldTest,
	"DublinFlight.Weapons.Unit.YieldFiniteClampAndMonotonicGameCurve", DublinWeapons::Tests::Flags)
bool FDublinWeaponYieldTest::RunTest(const FString& Parameters)
{
	using namespace DublinWeapons;
	TestNearlyEqual(TEXT("Yield floor"), ClampYield(-20.0f), 0.1f);
	TestNearlyEqual(TEXT("Yield ceiling"), ClampYield(100000.0f), 1000.0f);
	TestNearlyEqual(TEXT("NaN yield restored to default"), ClampYield(std::numeric_limits<float>::quiet_NaN()), 1.0f);
	TestNearlyEqual(TEXT("Infinite yield restored to default"), ClampYield(std::numeric_limits<float>::infinity()), 1.0f);
	FBombCurve Curve;
	float PreviousRadius = 0.0f;
	float PreviousDepth = 0.0f;
	float PreviousStrength = 0.0f;
	for (float Yield : { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 10.0f, 100.0f, 1000.0f })
	{
		const FDublinImpact Impact = MakeImpact(EDublinImpactKind::Bomb, Yield, Curve);
		TestTrue(TEXT("Finite valid bomb payload"), Impact.IsValid());
		TestTrue(TEXT("Radius increases monotonically and remains bounded"),
			Impact.RadiusCm >= PreviousRadius && Impact.RadiusCm <= 5000.0f);
		TestTrue(TEXT("Depth increases monotonically and remains bounded"),
			Impact.CraterDepthCm >= PreviousDepth && Impact.CraterDepthCm <= 2000.0f);
		TestTrue(TEXT("Strength increases monotonically and remains bounded"),
			Impact.Strength >= PreviousStrength && Impact.Strength <= 50.0f);
		PreviousRadius = Impact.RadiusCm;
		PreviousDepth = Impact.CraterDepthCm;
		PreviousStrength = Impact.Strength;
	}
	Curve.Exponent = std::numeric_limits<float>::quiet_NaN();
	Curve.RadiusAtOneCm = std::numeric_limits<float>::infinity();
	Curve.MaxDepthCm = -100.0f;
	TestTrue(TEXT("Corrupt editor tuning still produces finite bounds"), MakeImpact(EDublinImpactKind::Bomb, 1.0f, Curve).IsValid());
	const FDublinImpact Cannon = MakeImpact(EDublinImpactKind::Cannon, 1000.0f, Curve);
	TestNearlyEqual(TEXT("Cannon stays a local 2 m impact"), Cannon.RadiusCm, 200.0f);
	TestNearlyEqual(TEXT("Cannon crater stays 0.5 m"), Cannon.CraterDepthCm, 50.0f);
	TestNearlyEqual(TEXT("Bomb yield does not leak into cannon"), Cannon.YieldTonsTNT, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponCooldownTest,
	"DublinFlight.Weapons.Unit.CooldownAndNoCatchupBurst", DublinWeapons::Tests::Flags)
bool FDublinWeaponCooldownTest::RunTest(const FString& Parameters)
{
	DublinWeapons::FCooldown Bomb;
	TestTrue(TEXT("First bomb permitted"), Bomb.TryConsume(0.0, 1.5));
	TestFalse(TEXT("Immediate B/RMB duplicate refused"), Bomb.TryConsume(0.0, 1.5));
	TestFalse(TEXT("Bomb spam during cooldown refused"), Bomb.TryConsume(1.49, 1.5));
	TestTrue(TEXT("Bomb re-arms at cooldown boundary"), Bomb.TryConsume(1.5, 1.5));
	TestFalse(TEXT("Nonfinite clock cannot bypass cooldown"), Bomb.TryConsume(std::numeric_limits<double>::quiet_NaN(), 1.5));
	DublinWeapons::FCooldown Cannon;
	TestTrue(TEXT("Cannon starts"), Cannon.TryConsume(0.0, 1.0 / 7.0));
	TestTrue(TEXT("One round after long hitch"), Cannon.TryConsume(10.0, 1.0 / 7.0));
	TestFalse(TEXT("No catchup burst in same elapsed frame"), Cannon.TryConsume(10.0, 1.0 / 7.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponImpactGateTest,
	"DublinFlight.Weapons.Unit.OwnerImmunityAndExactlyOnceImpact", DublinWeapons::Tests::Flags)
bool FDublinWeaponImpactGateTest::RunTest(const FString& Parameters)
{
	int32 Owner = 0;
	int32 Wall = 1;
	DublinWeapons::FImpactOnce Gate;
	TestFalse(TEXT("Owner collision never consumes or damages"), Gate.TryConsume(&Owner, &Owner));
	TestFalse(TEXT("Owner immunity leaves projectile armed"), Gate.IsConsumed());
	TestTrue(TEXT("First foreign hit accepted"), Gate.TryConsume(&Wall, &Owner));
	TestFalse(TEXT("OnHit duplicate suppressed"), Gate.TryConsume(&Wall, &Owner));
	TestFalse(TEXT("Tick water crossing duplicate suppressed"), Gate.TryConsume(nullptr, &Owner));
	DublinWeapons::FImpactOnce Water;
	TestTrue(TEXT("Nonblocking water can own first impact"), Water.TryConsume(nullptr, &Owner));
	TestFalse(TEXT("Ground stop after water cannot duplicate"), Water.TryConsume(&Wall, &Owner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponWaterTest,
	"DublinFlight.Weapons.Unit.NonblockingWaterTrajectoryCrossing", DublinWeapons::Tests::Flags)
bool FDublinWeaponWaterTest::RunTest(const FString& Parameters)
{
	FVector Contact;
	const auto River = [](const FVector& Position, float& Z)
	{
		Z = 25.0f;
		return FMath::Abs(Position.Y) < 1000.0 && FMath::Abs(Position.X) < 10000.0;
	};
	TestTrue(TEXT("Falling projectile crosses actual water surface"),
		DublinWeapons::FindWaterCrossing(FVector(0.0, 0.0, 1000.0), FVector(1000.0, 0.0, -1000.0), River, Contact));
	TestNearlyEqual(TEXT("Intersection is on water, not the bed"), Contact.Z, 25.0);
	TestNearlyEqual(TEXT("Intersection respects trajectory XY"), Contact.X, 487.5, 0.01);
	TestFalse(TEXT("Passing above water is not an impact"),
		DublinWeapons::FindWaterCrossing(FVector(0.0, 0.0, 1000.0), FVector(1000.0, 0.0, 500.0), River, Contact));
	TestFalse(TEXT("Dry land does not turn into water"),
		DublinWeapons::FindWaterCrossing(FVector(0.0, 2000.0, 1000.0), FVector(1000.0, 2000.0, -1000.0), River, Contact));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWeaponInputConflictTest,
	"DublinFlight.Weapons.Unit.GodMovementInputHasNoWeaponConflicts", DublinWeapons::Tests::Flags)
bool FDublinWeaponInputConflictTest::RunTest(const FString& Parameters)
{
	for (const FKey& Key : { EKeys::SpaceBar, EKeys::C, EKeys::W, EKeys::S, EKeys::A, EKeys::D, EKeys::G, EKeys::Home })
	{
		TestFalse(TEXT("Existing flight/god key is not a weapon key"), DublinWeapons::IsWeaponKey(Key));
	}
	TestTrue(TEXT("LMB is cannon"), DublinWeapons::IsWeaponKey(EKeys::LeftMouseButton));
	TestTrue(TEXT("B is bomb"), DublinWeapons::IsWeaponKey(EKeys::B));
	TestTrue(TEXT("RMB is alternate bomb"), DublinWeapons::IsWeaponKey(EKeys::RightMouseButton));
	DublinFlight::FInputState Input;
	Input.SetKey(DublinFlight::EControlKey::Up, true);
	TestNearlyEqual(TEXT("Space remains god vertical motion"), Input.BuildInput(DublinFlight::EFlightMode::God).Up, 1.0);
	return true;
}

#endif
