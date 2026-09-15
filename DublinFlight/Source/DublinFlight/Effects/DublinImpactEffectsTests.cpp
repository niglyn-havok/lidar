#include "Effects/DublinImpactEffectsSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Effects/DublinImpactEffectType.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace DublinImpactFX::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	class FValidateAuthoredSystem final : public IAutomationLatentCommand
	{
	public:
		FValidateAuthoredSystem(FAutomationTestBase& InTest, UNiagaraSystem* InSystem)
			: Test(InTest), System(InSystem)
		{
		}

		virtual bool Update() override
		{
			if (StartTime == 0.0)
			{
				StartTime = FPlatformTime::Seconds();
			}
#if WITH_EDITOR
			System->PollForCompilationComplete();
			const bool bCompiling = System->HasOutstandingCompilationRequests(true);
			if (bCompiling || (System->IsValid() && !System->IsReadyToRun()))
			{
				if (FPlatformTime::Seconds() - StartTime < 120.0)
				{
					return false;
				}
				Test.AddError(FString::Printf(TEXT("%s readiness timed out: pending=%d valid=%d ready=%d"),
					*System->GetPathName(), bCompiling, System->IsValid(), System->IsReadyToRun()));
			}
#endif
			const FString Path = System->GetPathName();
			Test.TestTrue(Path + TEXT(": real compiled Niagara asset"), System->IsValid());
			Test.TestTrue(Path + TEXT(": ready to run including GPU compilation"), System->IsReadyToRun());
			Test.TestTrue(Path + TEXT(": compiled system spawn script"),
				System->GetSystemSpawnScript() && System->GetSystemSpawnScript()->DidScriptCompilationSucceed(false));
			Test.TestTrue(Path + TEXT(": compiled system update script"),
				System->GetSystemUpdateScript() && System->GetSystemUpdateScript()->DidScriptCompilationSucceed(false));
			Test.TestTrue(Path + TEXT(": concrete emitters"), System->GetNumEmitters() >= 4);
			Test.TestTrue(Path + TEXT(": shared native EffectType bound for packaging"),
				System->GetEffectType() == GetDefault<UDublinImpactEffectType>());
			const FNiagaraUserRedirectionParameterStore& User = System->GetExposedParameters();
			Test.TestTrue(Path + TEXT(": ImpactRadius float"), User.IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.ImpactRadius"))) != INDEX_NONE);
			Test.TestTrue(Path + TEXT(": ImpactStrength float"), User.IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.ImpactStrength"))) != INDEX_NONE);
			Test.TestTrue(Path + TEXT(": SurfaceNormal vector"), User.IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("User.SurfaceNormal"))) != INDEX_NONE);
			return true;
		}

	private:
		FAutomationTestBase& Test;
		TStrongObjectPtr<UNiagaraSystem> System;
		double StartTime = 0.0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinImpactRoutingTest,
	"DublinFlight.Effects.FinitePayloadRouting", DublinImpactFX::Tests::Flags)

bool FDublinImpactRoutingTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	FDublinImpact Impact;
	TestTrue(TEXT("Cannon routes to grit"), Route(Impact) == ERoute::Cannon);
	Impact.Kind = EDublinImpactKind::Bomb;
	TestTrue(TEXT("Bomb routes to plume"), Route(Impact) == ERoute::Bomb);
	Impact.bWater = true;
	TestTrue(TEXT("Water overrides bomb"), Route(Impact) == ERoute::Water);
	Impact.Kind = EDublinImpactKind::Cannon;
	TestTrue(TEXT("Water overrides cannon"), Route(Impact) == ERoute::Water);
	Impact.Normal = FVector::ZeroVector;
	TestTrue(TEXT("Degenerate normal rejected"), Route(Impact) == ERoute::Invalid);
	Impact = FDublinImpact();
	Impact.Normal.X = std::numeric_limits<double>::infinity();
	TestTrue(TEXT("Infinite normal rejected"), Route(Impact) == ERoute::Invalid);
	Impact = FDublinImpact();
	Impact.Normal = FVector(1.0e300, 0, 0);
	TestTrue(TEXT("Overflowing normal rejected"), Route(Impact) == ERoute::Invalid);
	Impact = FDublinImpact();
	Impact.PositionCm.Z = std::numeric_limits<double>::quiet_NaN();
	TestTrue(TEXT("NaN position rejected"), Route(Impact) == ERoute::Invalid);
	for (float Bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		Impact = FDublinImpact();
		Impact.Strength = Bad;
		TestTrue(TEXT("Invalid strength rejected"), Route(Impact) == ERoute::Invalid);
		Impact = FDublinImpact();
		Impact.RadiusCm = Bad;
		TestTrue(TEXT("Invalid radius rejected"), Route(Impact) == ERoute::Invalid);
	}
	Impact = FDublinImpact();
	Impact.Kind = static_cast<EDublinImpactKind>(255);
	TestTrue(TEXT("Unknown kind rejected"), Route(Impact) == ERoute::Invalid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinImpactBudgetTest,
	"DublinFlight.Effects.BoundedBudgetAndScale", DublinImpactFX::Tests::Flags)

bool FDublinImpactBudgetTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	TestFalse(TEXT("Negative occupancy rejected"), CanAdmit(-1));
	int32 Occupancy = 0;
	for (int32 Attempt = 0; Attempt < 10000; ++Attempt)
	{
		if (CanAdmit(Occupancy))
		{
			++Occupancy;
		}
	}
	TestEqual(TEXT("Burst admission never exceeds sixteen"), Occupancy, MaxSystems);
	TestFalse(TEXT("Full budget rejects admission"), CanAdmit(MaxSystems));
	TestTrue(TEXT("Reclaimed slot permits admission"), CanAdmit(MaxSystems - 1));
	FDublinImpact Impact;
	Impact.RadiusCm = 1.0e30f;
	Impact.Strength = 1.0e30f;
	TestEqual(TEXT("Cannon radius cap"), VisualRadius(Impact), 400.0f);
	TestEqual(TEXT("Strength cap"), VisualStrength(Impact), 4.0f);
	Impact.Kind = EDublinImpactKind::Bomb;
	TestEqual(TEXT("Bomb radius cap"), VisualRadius(Impact), 3000.0f);
	Impact.RadiusCm = 0.01f;
	TestEqual(TEXT("Readable minimum footprint"), VisualRadius(Impact), 50.0f);
	Impact.Strength = std::numeric_limits<float>::quiet_NaN();
	TestEqual(TEXT("Invalid payload has no visual scale"), VisualRadius(Impact), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinImpactAudioTest,
	"DublinFlight.Effects.OriginalAudioBoundedAndDeterministic", DublinImpactFX::Tests::Flags)

bool FDublinImpactAudioTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	TestTrue(TEXT("Invalid route produces no PCM"), GenerateBoom(ERoute::Invalid).IsEmpty());
	for (ERoute Route : {ERoute::Cannon, ERoute::Bomb, ERoute::Water})
	{
		const TArray<int16> PCM = GenerateBoom(Route);
		TestTrue(TEXT("Original synthesis is reproducible"), PCM == GenerateBoom(Route));
		TestTrue(TEXT("Bounded under two seconds"), PCM.Num() > 0 && PCM.Num() <= AudioSampleRate * 2);
		int32 Peak = 0;
		for (int16 Sample : PCM)
		{
			Peak = FMath::Max(Peak, FMath::Abs(static_cast<int32>(Sample)));
		}
		TestTrue(TEXT("Non-silent boom with conservative PCM headroom"), Peak > 1000 && Peak <= 21300);
		TestEqual(TEXT("Click-free opening"), PCM[0], static_cast<int16>(0));
		TestEqual(TEXT("Click-free tail"), PCM.Last(), static_cast<int16>(0));
	}
	TestEqual(TEXT("Fixed audio pool"), MaxAudioVoices, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinImpactAssetsTest,
	"DublinFlight.Effects.AuthoredAssetContract", DublinImpactFX::Tests::Flags)

bool FDublinImpactAssetsTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	for (const TCHAR* Path : {TEXT("/Game/FX/NS_CannonImpact.NS_CannonImpact"),
		TEXT("/Game/FX/NS_BombExplosion.NS_BombExplosion"), TEXT("/Game/FX/NS_WaterImpact.NS_WaterImpact")})
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, Path);
		if (!TestNotNull(Path, System))
		{
			continue;
		}
		PrepareSystem(System);
		ADD_LATENT_AUTOMATION_COMMAND(DublinImpactFX::Tests::FValidateAuthoredSystem(*this, System));
	}
	const UDublinImpactEffectType* Type = GetDefault<UDublinImpactEffectType>();
	TestNotNull(TEXT("Native distance significance"), Type->GetSignificanceHandler());
	if (TestEqual(TEXT("Single native scalability policy"), Type->SystemScalabilitySettings.Settings.Num(), 1))
	{
		TestEqual(TEXT("EffectType hard global ceiling"), Type->SystemScalabilitySettings.Settings[0].MaxInstances, MaxSystems);
	}
	return true;
}

#endif
