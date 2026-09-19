#include "Effects/DublinImpactEffectsSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFloat.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace DublinImpactFX::MaximumTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FDublinImpact Impact()
	{
		FDublinImpact Result;
		Result.Kind = EDublinImpactKind::Bomb;
		Result.YieldTonsTNT = 1000.0f;
		Result.RadiusCm = MaximumApprovedRadiusCm;
		Result.PositionCm = FVector(100000, -200000, 500);
		Result.Seed = 1847;
		return Result;
	}

	class FValidateMaximumAsset final : public IAutomationLatentCommand
	{
	public:
		FValidateMaximumAsset(FAutomationTestBase& InTest, UNiagaraSystem* InSystem)
			: Test(InTest), System(InSystem), Started(FPlatformTime::Seconds()) {}

		virtual bool Update() override
		{
#if WITH_EDITOR
			System->PollForCompilationComplete();
			if (System->HasOutstandingCompilationRequests(true) || !System->IsReadyToRun())
			{
				if (FPlatformTime::Seconds() - Started < 120.0) { return false; }
				Test.AddError(TEXT("Maximum Niagara asset did not become ready within 120 seconds."));
			}
#endif
			Test.TestTrue(TEXT("Maximum asset compiled and ready"), System->IsValid() && System->IsReadyToRun());
			FString Failure;
			const bool bValid = ValidateMaximumAsset(System.Get(), Failure);
			Test.TestTrue(TEXT("Maximum authored contract: ") + Failure, bValid);
			return true;
		}

	private:
		FAutomationTestBase& Test;
		TStrongObjectPtr<UNiagaraSystem> System;
		double Started;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumSelectionTest,
	"DublinFlight.Effects.MaximumTierSelectionAndBudget", DublinImpactFX::MaximumTests::Flags)

bool FDublinMaximumSelectionTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	FDublinImpact Impact = MaximumTests::Impact();
	TestTrue(TEXT("Maximum land bomb selects maximum presentation"), WantsMaximumPresentation(Impact));
	Impact.YieldTonsTNT = 999.0f;
	TestFalse(TEXT("Intermediate-yield curve is not silently redefined"), WantsMaximumPresentation(Impact));
	Impact.YieldTonsTNT = 1.0f;
	Impact.RadiusCm = 600.0f;
	TestEqual(TEXT("Normal one-unit bomb retains six-metre visual radius"), VisualRadius(Impact), 600.0f);
	TestFalse(TEXT("Ordinary bomb never allocates maximum recipe"), MakeMaximumPresentation(Impact, {}).IsEnabled());
	Impact = MaximumTests::Impact();
	Impact.bWater = true;
	TestFalse(TEXT("Maximum water bomb retains water route"), WantsMaximumPresentation(Impact));
	Impact.bWater = false;
	Impact.Kind = EDublinImpactKind::Cannon;
	TestFalse(TEXT("Cannon cannot acquire maximum tier"), WantsMaximumPresentation(Impact));
	Impact = MaximumTests::Impact();
	Impact.RadiusCm = 6000.0f;
	TestEqual(TEXT("Approved radius is not reconstructed as 120m from yield"), MakeMaximumPresentation(Impact, {}).RadiusCm, 6000.0f);
	Impact.RadiusCm = MaximumApprovedRadiusCm + 1.0f;
	TestFalse(TEXT("Out-of-contract radius cannot silently enlarge the recipe"), MakeMaximumPresentation(Impact, {}).IsEnabled());
	Impact.RadiusCm = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("Nonfinite maximum impact rejected"), WantsMaximumPresentation(Impact));
	TestEqual(TEXT("Explicit maximum GPU ceiling"), MaximumParticleBudget, 3904);
	TestEqual(TEXT("Shared system cap unchanged"), MaxSystems, 16);
	TestFalse(TEXT("Negative maximum occupancy rejected"), CanAdmitMaximum(-1));
	TestTrue(TEXT("Second maximum presentation fits"), CanAdmitMaximum(1));
	TestFalse(TEXT("Third maximum presentation is downgraded"), CanAdmitMaximum(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumDamageSamplesTest,
	"DublinFlight.Effects.MaximumConfirmedDamageSamplesAndBounds", DublinImpactFX::MaximumTests::Flags)

bool FDublinMaximumDamageSamplesTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	const FDublinImpact Impact = MaximumTests::Impact();
	const FMaximumPresentation Empty = MakeMaximumPresentation(Impact, {});
	TestTrue(TEXT("Missing damage samples still permit a centred plume/flash"), Empty.IsEnabled());
	TestFalse(TEXT("Missing samples cannot fabricate structural debris"), Empty.HasStructuralSources());
	TestEqual(TEXT("No-sample presentation enables only plume and accents"), Empty.ParticleBudget(), 1088);
	TestEqual(TEXT("Radius is the approved 120m radius, not diameter"), Empty.RadiusCm, 12000.0f);
	TestEqual(TEXT("Origin plume is bounded, not a 240m opaque blanket"), Empty.Bounds().Max.X, 3000.0);
	TestEqual(TEXT("Plume height includes sprite half-size"), Empty.Bounds().Max.Z, 13200.0);
	TArray<FDublinConfirmedDamageSample> Samples;
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Samples.Add({Impact.PositionCm + FVector(Impact.RadiusCm, 0, 0), FVector(0, 0, 2)});
	}
	const FMaximumPresentation Full = MakeMaximumPresentation(Impact, Samples);
	TestEqual(TEXT("At most thirty-two read-only source samples copied"), Full.DamageOffsets.Num(), MaxDamageSamples);
	TestEqual(TEXT("Excess samples explicitly accounted for"), Full.DroppedSamples, 68);
	TestEqual(TEXT("All maximum layers fit the 3904 ceiling"), Full.ParticleBudget(), 3904);
	TestEqual(TEXT("Boundary source is accepted as a local offset"), Full.DamageOffsets[0], FVector(12000, 0, 0));
	TestEqual(TEXT("Source normals normalized"), Full.DamageNormals[0], FVector::UpVector);
	TestEqual(TEXT("Bounds contain source + travel + billboard half-size"), Full.Bounds().Max.X, 16200.0);
	TestEqual(TEXT("Caller snapshot is not mutated"), Samples[0].Normal, FVector(0, 0, 2));
	for (const FVector& Direction : {FVector::ForwardVector, -FVector::ForwardVector, FVector::UpVector, -FVector::UpVector})
	{
		TArray<FDublinConfirmedDamageSample> Edge = {{Impact.PositionCm + Direction * Impact.RadiusCm, Direction}};
		const FMaximumPresentation Recipe = MakeMaximumPresentation(Impact, Edge);
		const FVector Corner = Recipe.DamageOffsets[0] + Direction * (Recipe.DebrisTravelCm + Recipe.SpriteRadiusCm);
		TestTrue(TEXT("Conservative bounds include full per-source visual travel"), Recipe.Bounds().IsInsideOrOn(Corner));
	}
	TArray<FDublinConfirmedDamageSample> Heights = {
		{Impact.PositionCm + FVector(Impact.RadiusCm, 0, Impact.RadiusCm * 2.0f), FVector::UpVector},
		{FVector(Impact.PositionCm.X, Impact.PositionCm.Y, MaximumDamageSampleAbsZCm), FVector::UpVector},
		{FVector(Impact.PositionCm.X, Impact.PositionCm.Y, -MaximumDamageSampleAbsZCm), FVector::UpVector}};
	const FMaximumPresentation Tall = MakeMaximumPresentation(Impact, Heights);
	if (TestEqual(TEXT("Roof/facade heights inside XY footprint and world Z limits are accepted"), Tall.DamageOffsets.Num(), 3))
	{
		TestTrue(TEXT("Legitimate high sample lies outside the old spherical filter"),
			Tall.DamageOffsets[0].SizeSquared() > FMath::Square(static_cast<double>(Impact.RadiusCm)));
		for (int32 Index = 0; Index < Heights.Num(); ++Index)
		{
			TestEqual(TEXT("Confirmed surface positions are not clamped or relocated"),
				Tall.DamageOffsets[Index] + Impact.PositionCm, Heights[Index].PositionCm);
			const FVector Margin(Tall.DebrisTravelCm + Tall.SpriteRadiusCm);
			TestTrue(TEXT("Dynamic bounds include actual elevated source and outward travel"),
				Tall.Bounds().IsInsideOrOn(Tall.DamageOffsets[Index] + Margin)
				&& Tall.Bounds().IsInsideOrOn(Tall.DamageOffsets[Index] - Margin));
		}
	}
	FDublinImpact HighImpact = Impact;
	HighImpact.PositionCm.Z = MaximumDamageSampleAbsZCm - 1000.0;
	TArray<FDublinConfirmedDamageSample> AboveMap = {{HighImpact.PositionCm + FVector(0, 0, 1001), FVector::UpVector}};
	TestFalse(TEXT("Z limit is absolute, even when the sample is close to an elevated impact"),
		MakeMaximumPresentation(HighImpact, AboveMap).HasStructuralSources());
	Samples.SetNum(8);
	Samples[0].PositionCm = Impact.PositionCm + FVector(12001, 0, 0);
	Samples[1].Normal = FVector::ZeroVector;
	Samples[2].PositionCm.X = std::numeric_limits<double>::quiet_NaN();
	Samples[3].Normal.Z = std::numeric_limits<double>::infinity();
	Samples[4].PositionCm = FVector(Impact.PositionCm.X, Impact.PositionCm.Y, MaximumDamageSampleAbsZCm + 1);
	Samples[5].PositionCm = FVector(Impact.PositionCm.X, Impact.PositionCm.Y, -MaximumDamageSampleAbsZCm - 1);
	Samples[6].PositionCm.Z = std::numeric_limits<double>::quiet_NaN();
	Samples[7].PositionCm.Z = std::numeric_limits<double>::infinity();
	const FMaximumPresentation Invalid = MakeMaximumPresentation(Impact, Samples);
	TestFalse(TEXT("Invalid/outside samples cannot enable structural layers"), Invalid.HasStructuralSources());
	TestEqual(TEXT("All rejected samples are reported"), Invalid.DroppedSamples, 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumParametersTest,
	"DublinFlight.Effects.MaximumParameterAndPooledSampleReset", DublinImpactFX::MaximumTests::Flags)

bool FDublinMaximumParametersTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	TStrongObjectPtr<UNiagaraComponent> Component(NewObject<UNiagaraComponent>());
	for (const TCHAR* Name : {TEXT("User.DamageOffsets"), TEXT("User.DamageNormals")})
	{
		const FNiagaraVariable Variable(FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass()), Name);
		Component->GetOverrideParameters().AddParameter(Variable);
		UNiagaraDataInterfaceArrayFloat3* Array = Cast<UNiagaraDataInterfaceArrayFloat3>(Component->GetOverrideParameters().GetDataInterface(Variable));
		if (!TestNotNull(TEXT("Typed array fixture initialized"), Array)) { return false; }
		Array->MaxElements = MaxDamageSamples;
	}
	const FDublinImpact Impact = MaximumTests::Impact();
	TArray<FDublinConfirmedDamageSample> Samples = {{Impact.PositionCm + FVector(100, 200, 300), FVector::UpVector}};
	SetMaximumParameters(*Component, Impact, MakeMaximumPresentation(Impact, Samples));
	bool bValid = false;
	TestEqual(TEXT("Approved radius reaches real Niagara override"), Component->GetVariableFloat(TEXT("User.ImpactRadius"), bValid), 12000.0f);
	TestTrue(TEXT("Radius override is typed"), bValid);
	TestEqual(TEXT("Maximum mesh count reaches Niagara"), Component->GetVariableInt(TEXT("User.MeshParticleCount"), bValid), 256);
	TestEqual(TEXT("Damage sample count reaches Niagara"), Component->GetVariableInt(TEXT("User.DamageSampleCount"), bValid), 1);
	const TArray<FVector> Offsets = UNiagaraDataInterfaceArrayFunctionLibrary::GetNiagaraArrayVector(Component.Get(), TEXT("User.DamageOffsets"));
	if (TestEqual(TEXT("Real Niagara sample data populated"), Offsets.Num(), 1))
	{
		TestEqual(TEXT("GPU receives bounded local positions, not large world coordinates"), Offsets[0], FVector(100, 200, 300));
	}
	SetMaximumParameters(*Component, Impact, MakeMaximumPresentation(Impact, {}));
	for (const TCHAR* Name : {TEXT("User.MeshParticleCount"), TEXT("User.FleckParticleCount"), TEXT("User.DustParticleCount"), TEXT("User.DamageSampleCount")})
	{
		TestEqual(FString(Name) + TEXT(" cleared on no-sample reuse"), Component->GetVariableInt(Name, bValid), 0);
	}
	TestTrue(TEXT("Pooled sample positions emptied"), UNiagaraDataInterfaceArrayFunctionLibrary::GetNiagaraArrayVector(Component.Get(), TEXT("User.DamageOffsets")).IsEmpty());
	TestTrue(TEXT("Pooled sample normals emptied"), UNiagaraDataInterfaceArrayFunctionLibrary::GetNiagaraArrayVector(Component.Get(), TEXT("User.DamageNormals")).IsEmpty());
	TestEqual(TEXT("Transient end bound reaches Niagara"), Component->GetVariableFloat(TEXT("User.TransientSeconds"), bValid), 12.0f);
	TestEqual(TEXT("Fire is a brief accent, not the full plume lifetime"),
		Component->GetVariableFloat(TEXT("User.AccentSeconds"), bValid), 0.35f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumLifecycleTest,
	"DublinFlight.Effects.MaximumLifecycleSharedBudgetAndTimerCleanup", DublinImpactFX::MaximumTests::Flags)

bool FDublinMaximumLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Lifecycle fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	UDublinImpactEffectsSubsystem* Effects = World->GetSubsystem<UDublinImpactEffectsSubsystem>();
	if (!TestNotNull(TEXT("Effects fixture"), Effects)) { return false; }
	{
		TStrongObjectPtr<UNiagaraSystem> ExistingMaximum(Effects->MaximumSystem.Get());
		Effects->MaximumSystem = nullptr;
		const FDublinImpact Impact = MaximumTests::Impact();
		TestFalse(TEXT("Pending maximum asset leaves legacy presentation available without an error"),
			Effects->TryEmitMaximum(Impact, MakeMaximumPresentation(Impact, {}), 1.0f));
		TestEqual(TEXT("Pending authoring allocates no maximum component"), Effects->ActiveImpacts.Num(), 0);
		Effects->MaximumSystem = ExistingMaximum.Get();
	}
	Effects->ActiveImpacts.SetNum(MaxSystems);
	for (int32 Index = 0; Index < MaxSystems; ++Index)
	{
		FDublinActiveImpactEffect& Entry = Effects->ActiveImpacts[Index];
		Entry.bMaximumPresentation = Index < MaxMaximumPresentations;
		Entry.bSmokeColumn = Index >= MaxMaximumPresentations && Index < MaxMaximumPresentations + MaxSmokeColumns;
		Entry.Deadline = MaximumLifetimeGuardSeconds;
	}
	TestEqual(TEXT("Two maximum systems count inside the existing sixteen"), CountMaximumPresentations(Effects->ActiveImpacts), 2);
	TestEqual(TEXT("Ordinary and maximum tails share exactly four columns"), CountSmokeColumns(Effects->ActiveImpacts), 4);
	TestFalse(TEXT("A tail cannot bypass the shared full budget"), CanAdmit(Effects->ActiveImpacts.Num()));
	bool bStaleTimerRan = false;
	World->GetTimerManager().SetTimer(Effects->ActiveImpacts[0].StopTimer, FTimerDelegate::CreateLambda([&bStaleTimerRan]()
	{
		bStaleTimerRan = true;
	}), MaximumTransientSeconds, false);
	const FTimerHandle OldTimer = Effects->ActiveImpacts[0].StopTimer;
	Effects->StopMaximumPresentation(nullptr);
	TestFalse(TEXT("Releasing a maximum effect cancels its lease timer"), World->GetTimerManager().TimerExists(OldTimer));
	TestFalse(TEXT("Cancellation never executes the stale timer"), bStaleTimerRan);
	TestEqual(TEXT("Maximum release frees its presentation budget"), CountMaximumPresentations(Effects->ActiveImpacts), 1);
	TestEqual(TEXT("Maximum release does not terminate independent smoke tails"), CountSmokeColumns(Effects->ActiveImpacts), 4);
	TestTrue(TEXT("Maximum release frees one shared slot"), CanAdmit(Effects->ActiveImpacts.Num()));
	FDublinActiveImpactEffect Guard;
	Guard.Deadline = MaximumLifetimeGuardSeconds;
	TestFalse(TEXT("Separate hard guard is not premature"), Guard.ShouldRelease(15.999, false));
	TestTrue(TEXT("Hard cleanup cannot outlive sixteen seconds"), Guard.ShouldRelease(16.0, false));
	TestTrue(TEXT("Scalability completion can release earlier"), Guard.ShouldRelease(1.0, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumAssetFailuresTest,
	"DublinFlight.Effects.MaximumMissingAssetIsExplicit", DublinImpactFX::MaximumTests::Flags)

bool FDublinMaximumAssetFailuresTest::RunTest(const FString& Parameters)
{
	FString Failure;
	TestFalse(TEXT("No maximum asset cannot masquerade as ready"), DublinImpactFX::ValidateMaximumAsset(nullptr, Failure));
	TestTrue(TEXT("Missing asset error names the required package"), Failure.Contains(TEXT("NS_MaxBombExplosion")));
	TStrongObjectPtr<UNiagaraSystem> Empty(NewObject<UNiagaraSystem>());
	TestFalse(TEXT("Empty asset cannot satisfy the contract"), DublinImpactFX::ValidateMaximumAsset(Empty.Get(), Failure));
	TestTrue(TEXT("Contract failure identifies its missing typed parameter"), Failure.Contains(TEXT("User.ImpactRadius")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximumAuthoredAssetTest,
	"DublinFlight.Effects.MaximumAuthoredAssetContract", DublinImpactFX::MaximumTests::Flags)

bool FDublinMaximumAuthoredAssetTest::RunTest(const FString& Parameters)
{
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, DublinImpactFX::MaximumSystemPath, nullptr, LOAD_NoWarn);
	if (!TestNotNull(TEXT("Required NS_MaxBombExplosion asset must be authored before maximum-tier acceptance"), System)) { return false; }
	DublinImpactFX::PrepareSystem(System);
	ADD_LATENT_AUTOMATION_COMMAND(DublinImpactFX::MaximumTests::FValidateMaximumAsset(*this, System));
	return true;
}

#endif
