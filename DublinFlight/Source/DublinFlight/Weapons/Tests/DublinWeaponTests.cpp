#include "Weapons/DublinWeaponModel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DublinFlightSimulation.h"
#include "DublinFlightHUD.h"
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
			Impact.RadiusCm >= PreviousRadius && Impact.RadiusCm <= 12000.0f);
		TestTrue(TEXT("Depth increases monotonically and remains bounded"),
			Impact.CraterDepthCm >= PreviousDepth && Impact.CraterDepthCm <= 2000.0f);
		TestTrue(TEXT("Strength increases monotonically and remains bounded"),
			Impact.Strength >= PreviousStrength && Impact.Strength <= 50.0f);
		PreviousRadius = Impact.RadiusCm;
		PreviousDepth = Impact.CraterDepthCm;
		PreviousStrength = Impact.Strength;
	}
	TestNearlyEqual(TEXT("Maximum game yield reaches the 120 m radius"),
		MakeImpact(EDublinImpactKind::Bomb, 1000.0f, Curve).RadiusCm, 12000.0f);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBombMarkerVisibilityTest,
	"DublinFlight.Weapons.Unit.BombMarkerVisibility", DublinWeapons::Tests::Flags)
bool FDublinBombMarkerVisibilityTest::RunTest(const FString& Parameters)
{
	const FVector2D Viewport(1920, 1080);
	TestTrue(TEXT("No live bomb means no marker"), FDublinBombSight().Markers.IsEmpty());
	FDublinBombMarker Sight;
	TestTrue(TEXT("Missing world position cannot produce a centre marker"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::Unavailable);
	Sight.bHasPosition = true;
	Sight.WorldPoint = FVector(100, 200, 300);
	TestTrue(TEXT("Unprojectable or behind-camera bomb stays offscreen"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::Offscreen);
	Sight.bProjected = true;
	Sight.ScreenPoint = FVector2D(960, 600);
	TestTrue(TEXT("A real projected point inside the viewport can be marked"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::Visible);
	Sight.ScreenPoint.Y = 1080;
	TestTrue(TEXT("Actual bottom-edge crossing is labelled below view"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::BelowView);
	for (const FVector2D Point : { FVector2D(-1, 500), FVector2D(1920, 500), FVector2D(960, -1) })
	{
		Sight.ScreenPoint = Point;
		TestTrue(TEXT("Other offscreen directions are not labelled below"),
			ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::Offscreen);
	}
	Sight.ScreenPoint = FVector2D(std::numeric_limits<double>::quiet_NaN(), 500);
	TestTrue(TEXT("Nonfinite projection cannot be drawn"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::Unavailable);
	Sight.ScreenPoint = FVector2D(960, 600);
	Sight.WorldPoint.Z = std::numeric_limits<double>::infinity();
	TestTrue(TEXT("Nonfinite live position cannot be drawn"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, Viewport) == EDublinBombMarkerState::Unavailable);
	Sight.WorldPoint = FVector::ZeroVector;
	TestTrue(TEXT("An unavailable viewport cannot produce a marker"),
		ADublinFlightHUD::ClassifyBombMarker(Sight, FVector2D::ZeroVector) == EDublinBombMarkerState::Unavailable);

	FDublinBombSight Multiple;
	Multiple.Markers.Add(Sight);
	Multiple.Markers.Add(Sight);
	Multiple.Markers.Add(Sight);
	Multiple.Markers.Add(Sight);
	Multiple.Markers[1].ScreenPoint.Y = Viewport.Y;
	Multiple.Markers[2].bProjected = false;
	Multiple.Markers[3].bHasPosition = false;
	TestTrue(TEXT("An older visible bomb stays visible alongside offscreen and unavailable bombs"),
		ADublinFlightHUD::ClassifyBombMarker(Multiple.Markers[0], Viewport) == EDublinBombMarkerState::Visible);
	TestTrue(TEXT("A different bomb independently reports below view"),
		ADublinFlightHUD::ClassifyBombMarker(Multiple.Markers[1], Viewport) == EDublinBombMarkerState::BelowView);
	TestTrue(TEXT("A behind-camera bomb independently reports offscreen"),
		ADublinFlightHUD::ClassifyBombMarker(Multiple.Markers[2], Viewport) == EDublinBombMarkerState::Offscreen);
	TestTrue(TEXT("A newest unavailable bomb cannot hide the other marker states"),
		ADublinFlightHUD::ClassifyBombMarker(Multiple.Markers[3], Viewport) == EDublinBombMarkerState::Unavailable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBombLabelLayoutTest,
	"DublinFlight.Weapons.Unit.AllBombLabelsBoundedAndDistinct", DublinWeapons::Tests::Flags)
bool FDublinBombLabelLayoutTest::RunTest(const FString& Parameters)
{
	const FVector2D LabelSize(280.0, 24.0);
	for (const FVector2D Size : { FVector2D(1884, 900), FVector2D(1244, 504), FVector2D(620, 240), FVector2D(64, 64) })
	{
		const FBox2D Bounds(FVector2D(18, 126), FVector2D(18, 126) + Size);
		for (const int32 Count : { 0, 1, 2, 32, 64 })
		{
			const TArray<FBox2D> Labels = ADublinFlightHUD::LayoutBombLabels(Count, Bounds, LabelSize);
			TestEqual(TEXT("Every live bomb gets a label, including the full 64-projectile budget"), Labels.Num(), Count);
			for (int32 Index = 0; Index < Labels.Num(); ++Index)
			{
				const FBox2D& Label = Labels[Index];
				TestTrue(TEXT("Each entire label is finite and bounded, not just its anchor"),
					!Label.Min.ContainsNaN() && !Label.Max.ContainsNaN()
					&& Label.Min.X >= Bounds.Min.X - 0.001 && Label.Min.Y >= Bounds.Min.Y - 0.001
					&& Label.Max.X <= Bounds.Max.X + 0.001 && Label.Max.Y <= Bounds.Max.Y + 0.001
					&& Label.GetSize().X > 0.0 && Label.GetSize().Y > 0.0);
				TestNearlyEqual(TEXT("Fitting preserves text aspect ratio"),
					Label.GetSize().X / LabelSize.X, Label.GetSize().Y / LabelSize.Y, 0.000001);
				if (Size.X >= 1244.0)
				{
					TestNearlyEqual(TEXT("Standard view sizes fit all 64 labels at full readable text size"),
						Label.GetSize().Y, LabelSize.Y, 0.000001);
				}
				for (int32 Previous = 0; Previous < Index; ++Previous)
				{
					const FBox2D& Other = Labels[Previous];
					TestTrue(TEXT("Even coincident bomb projections have separate, nonoverlapping labels"),
						Label.Min.X >= Other.Max.X - 0.001 || Label.Max.X <= Other.Min.X + 0.001
						|| Label.Min.Y >= Other.Max.Y - 0.001 || Label.Max.Y <= Other.Min.Y + 0.001);
				}
			}
		}
	}
	return true;
}

#endif
