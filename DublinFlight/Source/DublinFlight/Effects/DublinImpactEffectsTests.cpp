#include "Effects/DublinImpactEffectsSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Effects/DublinImpactEffectType.h"
#include "Components/AudioComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionEyeAdaptationInverse.h"
#include "Materials/MaterialExpressionParticleColor.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/UObjectIterator.h"
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
			FString Failure;
			const bool bMaterialsValid = ValidateSpriteMaterials(System.Get(), Failure);
			Test.TestTrue(Path + TEXT(": authored sprite materials, not renderer fallback: ") + Failure,
				bMaterialsValid);
			if (System->GetFName() == FName(TEXT("NS_WaterImpact")))
			{
				int32 WaterSprites = 0;
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
					if (!Handle.GetIsEnabled() || !Data) { continue; }
					for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
					{
						const UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer);
						if (!Sprite || !Sprite->GetIsEnabled()) { continue; }
						++WaterSprites;
						const FString Label = Handle.GetName().ToString();
						Test.TestEqual(Label + TEXT(": water-only material cannot darken bomb smoke"),
							GetPathNameSafe(Sprite->Material.Get()), FString(TEXT("/Game/FX/M_WaterSpray.M_WaterSpray")));
						Test.TestTrue(Label + TEXT(": particle tint reaches the sprite renderer"),
							Sprite->ColorBinding.GetParamMapBindableVariable().GetName() == FName(TEXT("Particles.Color")));
						Test.TestTrue(Label + TEXT(": no user-material override bypasses the water material"),
							Sprite->MaterialUserParamBinding.Parameter.GetName().IsNone());
#if WITH_EDITORONLY_DATA
						const UMaterial* Material = Sprite->Material ? Sprite->Material->GetMaterial() : nullptr;
						const UMaterialEditorOnlyData* MaterialData = Material ? Material->GetEditorOnlyData() : nullptr;
						const UMaterialExpressionEyeAdaptationInverse* Exposure = MaterialData
							? Cast<UMaterialExpressionEyeAdaptationInverse>(MaterialData->EmissiveColor.Expression) : nullptr;
						if (Test.TestNotNull(Label + TEXT(": visible spray compensates daylight exposure"), Exposure))
						{
							Test.TestTrue(Label + TEXT(": emissive consumes actual particle RGB, not dark cloud texture RGB"),
								Cast<UMaterialExpressionParticleColor>(Exposure->LightValueInput.Expression) != nullptr
								&& Exposure->LightValueInput.OutputIndex == 0);
							const UMaterialExpressionConstant* Alpha = Cast<UMaterialExpressionConstant>(Exposure->AlphaInput.Expression);
							if (Test.TestNotNull(Label + TEXT(": exposure compensation is explicitly enabled"), Alpha))
							{
								Test.TestEqual(Label + TEXT(": full exposure compensation"), Alpha->R, 1.0f);
							}
						}
#endif
					}
				}
				Test.TestEqual(TEXT("Droplets, mist, jet and expanding foam all use the white-water contract"), WaterSprites, 4);
			}
			if (System->GetFName() == FName(TEXT("NS_BombExplosion")))
			{
				ValidateMasonryChips();
				const bool bParametersValid = ValidateSmokeColumnAsset(System.Get(), Failure);
				Test.TestTrue(Path + TEXT(": sustained smoke parameter contract: ") + Failure, bParametersValid);
				const FNiagaraEmitterHandle* Handle = System->GetEmitterHandles().FindByPredicate(
					[](const FNiagaraEmitterHandle& Emitter) { return Emitter.GetName() == FName(TEXT("SimpleSpriteBurst")); });
				const FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
				if (Test.TestNotNull(TEXT("Bomb retains the real smoke emitter"), Data))
				{
					Test.TestTrue(TEXT("Smoke emitter enabled"), Handle->GetIsEnabled());
					Test.TestTrue(TEXT("Native wall-normal conversion requires local-space particles"), Data->bLocalSpace);
					Test.TestTrue(TEXT("Smoke particle allocation stays fixed"), Data->AllocationMode == EParticleAllocationMode::FixedCount);
					Test.TestTrue(TEXT("Allocation covers initial burst plus 6/sec * 8s, bounded at 96"),
						Data->PreAllocationCount >= 84 && Data->PreAllocationCount <= 96);
#if WITH_EDITORONLY_DATA
					const UNiagaraScript* GPU = Data->GetGPUComputeScript();
					if (Test.TestNotNull(TEXT("Column has an actual GPU particle script"), GPU))
					{
						for (const TCHAR* Name : {TEXT("User.SmokeParticleLifetime"), TEXT("User.SmokeRadius"), TEXT("User.SmokeVelocity")})
						{
							Test.TestTrue(FString(Name) + TEXT(" is read by the particle script, not just exposed"),
								GPU->GetVMExecutableData().Parameters.Parameters.ContainsByPredicate(
									[Name](const FNiagaraVariable& Variable) { return Variable.GetName() == FName(Name); }));
						}
					}
					// System scripts read User.* through per-instance datasets, not uniform constants.
					Test.TestTrue(TEXT("System/emitter update actually consumes the smoke duration"),
						System->GetSystemCompiledData().UpdateInstanceParamsDataSetCompiledData.Variables.ContainsByPredicate(
							[](const FNiagaraVariableBase& Variable)
							{
								return Variable.GetName() == FName(TEXT("User.SmokeEmissionSeconds"))
									&& Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef();
							}));
#endif
				}
			}
			return true;
		}

	private:
#if WITH_EDITORONLY_DATA
		template<typename T>
		void CheckCompiledConstant(const UNiagaraScript* Script, const TCHAR* Name, const FNiagaraTypeDefinition& Type, T Expected)
		{
			if (!Test.TestNotNull(FString(Name) + TEXT(": compiled script"), Script)) { return; }
			const FNiagaraVariable* Value = Script->GetVMExecutableData().BakedRapidIterationParameters.FindByPredicate(
				[Name, &Type](const FNiagaraVariable& Variable)
				{
					return Variable.GetName() == FName(Name) && Variable.GetType() == Type;
				});
			if (Test.TestNotNull(FString(Name) + TEXT(": compiled input, not merely an exposed parameter"), Value)
				&& Test.TestTrue(FString(Name) + TEXT(": compiled value allocated"), Value->IsDataAllocated()))
			{
				Test.TestEqual(FString(Name) + TEXT(": bounded compiled value"), Value->GetValue<T>(), Expected);
			}
		}
#endif

		void ValidateMasonryChips()
		{
			Test.TestEqual(TEXT("Chips reuse the existing five-emitter bomb system"), System->GetNumEmitters(), 5);
			const FNiagaraEmitterHandle* Handle = System->GetEmitterHandles().FindByPredicate(
				[](const FNiagaraEmitterHandle& Emitter) { return Emitter.GetName() == FName(TEXT("UpwardMeshBurst")); });
			const FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
			if (!Test.TestNotNull(TEXT("Existing chip emitter retained"), Data)) { return; }
			Test.TestTrue(TEXT("Chip emitter enabled"), Handle->GetIsEnabled());
			Test.TestTrue(TEXT("Chips use GPU particles, not CPU bodies"), Data->SimTarget == ENiagaraSimTarget::GPUComputeSim);
			Test.TestTrue(TEXT("Chip sampling is deterministic"), Data->bDeterminism);
			Test.TestEqual(TEXT("Impact seed offset is not displaced by a baked seed"), Data->RandomSeed, 0);
			Test.TestTrue(TEXT("Chip allocation stays fixed"), Data->AllocationMode == EParticleAllocationMode::FixedCount);
			Test.TestEqual(TEXT("At most forty-eight chips per accepted land bomb"), Data->PreAllocationCount, 48);
			if (!Test.TestEqual(TEXT("One existing chip renderer, not an additional effect"), Data->GetRenderers().Num(), 1)) { return; }
			const UNiagaraMeshRendererProperties* Renderer = Cast<UNiagaraMeshRendererProperties>(Data->GetRenderers()[0]);
			if (!Test.TestNotNull(TEXT("Chips are actual mesh particles"), Renderer)) { return; }
			Test.TestTrue(TEXT("Chip renderer enabled"), Renderer->GetIsEnabled());
			Test.TestFalse(TEXT("Cosmetic chips never cast shadows"), Renderer->bCastShadows);
			if (!Test.TestEqual(TEXT("One shared low-poly chip mesh"), Renderer->Meshes.Num(), 1)) { return; }
			Test.TestTrue(TEXT("Authored flake proportions are not flattened again"), Renderer->Meshes[0].Scale.Equals(FVector::OneVector));
			const UStaticMesh* Mesh = Renderer->Meshes[0].Mesh.Get();
			if (!Test.TestNotNull(TEXT("Owned irregular chip mesh is loaded"), Mesh)) { return; }
			Test.TestEqual(TEXT("Masonry chips cannot regress to cube confetti"),
				Mesh->GetPathName(), FString(TEXT("/Game/FX/SM_MasonryChip.SM_MasonryChip")));
#if WITH_EDITORONLY_DATA
			Test.TestFalse(TEXT("Tiny chips do not request Nanite"), Mesh->GetNaniteSettings().bEnabled);
#endif
			Test.TestFalse(TEXT("Tiny chips have no Nanite render data"), Mesh->HasValidNaniteData());
			Test.TestEqual(TEXT("One low-poly LOD"), Mesh->GetNumLODs(), 1);
			if (Mesh->GetNumLODs() > 0)
			{
				Test.TestTrue(TEXT("Closed chip stays within sixteen triangles"),
					Mesh->GetNumTriangles(0) > 0 && Mesh->GetNumTriangles(0) <= 16);
			}
			Test.TestTrue(TEXT("Thin asymmetric source bounds retain centimetre scale"),
				Mesh->GetBoundingBox().GetSize().Equals(FVector(100.0, 70.0, 30.0), 0.1));
			const UBodySetup* Body = Mesh->GetBodySetup();
			Test.TestTrue(TEXT("Cosmetic mesh has no collision shapes"), !Body || Body->AggGeom.GetElementCount() == 0);
			if (Body)
			{
				Test.TestTrue(TEXT("Cosmetic mesh cannot create collision bodies"),
					Body->DefaultInstance.GetCollisionEnabled() == ECollisionEnabled::NoCollision);
				Test.TestTrue(TEXT("Cosmetic mesh never cooks convex or triangle collision"), Body->bNeverNeedsCookedCollisionData);
				Test.TestFalse(TEXT("Cosmetic mesh never requests rigid-body simulation"), Body->DefaultInstance.bSimulatePhysics);
			}
			Test.TestTrue(TEXT("Existing debris material override retained"), Renderer->bOverrideMaterials);
			if (Test.TestEqual(TEXT("Exactly one debris material override"), Renderer->OverrideMaterials.Num(), 1))
			{
				const FNiagaraMeshMaterialOverride& Override = Renderer->OverrideMaterials[0];
				Test.TestEqual(TEXT("Existing authored debris material, not renderer fallback"),
					GetPathNameSafe(Override.ExplicitMat.Get()), FString(TEXT("/Game/FX/M_ImpactDebris.M_ImpactDebris")));
				Test.TestTrue(TEXT("No user override bypasses the chip material"), Override.UserParamBinding.Parameter.GetName().IsNone());
				const UMaterial* Material = Override.ExplicitMat ? Override.ExplicitMat->GetMaterial() : nullptr;
				Test.TestTrue(TEXT("Debris material supports the real Niagara mesh renderer"),
					Material && !Material->IsDefaultMaterial() && Material->GetUsageByFlag(MATUSAGE_NiagaraMeshParticles));
			}
#if WITH_EDITORONLY_DATA
			const UNiagaraScript* GPU = Data->GetGPUComputeScript();
			if (Test.TestNotNull(TEXT("Compiled chip particle script"), GPU))
			{
				const FNiagaraVMExecutableData& Compiled = GPU->GetVMExecutableData();
				Test.TestTrue(TEXT("Renderer consumes the compiled mesh orientation attribute"),
					Compiled.Attributes.Contains(Renderer->MeshOrientationBinding.GetDataSetBindableVariable()));
				Test.TestTrue(TEXT("Compiled chip quaternion hashes the impact-derived seed and particle ID"),
					Compiled.LastHlslTranslationGPU.Contains(
						TEXT("rand3(Context.MapSpawn.Emitter.RandomSeed,Context.MapSpawn.Particles.UniqueID,104729,0)")));
				// Named initializer constants belong to the spawn VM; the GPU kernel folds them into literals.
				const UNiagaraScript* Spawn = Data->SpawnScriptProps.Script;
				CheckCompiledConstant(Spawn, TEXT("Constants.UpwardMeshBurst.InitializeParticle.Lifetime Min"),
					FNiagaraTypeDefinition::GetFloatDef(), 2.0f);
				CheckCompiledConstant(Spawn, TEXT("Constants.UpwardMeshBurst.InitializeParticle.Lifetime Max"),
					FNiagaraTypeDefinition::GetFloatDef(), 2.8f);
				CheckCompiledConstant(Spawn, TEXT("Constants.UpwardMeshBurst.InitializeParticle.Mesh Uniform Scale Min"),
					FNiagaraTypeDefinition::GetFloatDef(), 0.04f);
				CheckCompiledConstant(Spawn, TEXT("Constants.UpwardMeshBurst.InitializeParticle.Mesh Uniform Scale Max"),
					FNiagaraTypeDefinition::GetFloatDef(), 0.22f);
			}
			CheckCompiledConstant(System->GetSystemUpdateScript(), TEXT("Constants.UpwardMeshBurst.SpawnBurst_Instantaneous.Spawn Count"),
				FNiagaraTypeDefinition::GetIntDef(), 48);
#endif
		}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinSmokeClassificationTest,
	"DublinFlight.Effects.SmokeColumnClassificationAndYield", DublinImpactFX::Tests::Flags)

bool FDublinSmokeClassificationTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	FDublinImpact Impact;
	TestFalse(TEXT("Ordinary land cannon never starts a column"), MakeSmokeColumn(Impact, 0).IsEnabled());
	Impact.bWater = true;
	TestFalse(TEXT("Water cannon never starts a column"), MakeSmokeColumn(Impact, 0).IsEnabled());
	Impact.Kind = EDublinImpactKind::Bomb;
	TestFalse(TEXT("Underwater bomb never starts a column"), MakeSmokeColumn(Impact, 0).IsEnabled());
	Impact.bWater = false;
	for (const FVector& Normal : {FVector::UpVector, FVector::ForwardVector, -FVector::UpVector})
	{
		Impact.Normal = Normal;
		TestTrue(TEXT("Ground, wall and underside bomb impacts qualify"), MakeSmokeColumn(Impact, 0).IsEnabled());
	}
	Impact.Normal = FVector::ZeroVector;
	TestFalse(TEXT("Invalid normal cannot bypass classification"), MakeSmokeColumn(Impact, 0).IsEnabled());
	Impact.Normal = FVector::UpVector;
	TestFalse(TEXT("Invalid negative occupancy rejected"), MakeSmokeColumn(Impact, -1).IsEnabled());
	TestFalse(TEXT("Full column budget retains burst only"), MakeSmokeColumn(Impact, MaxSmokeColumns).IsEnabled());
	TestFalse(TEXT("Overfull column budget also rejected"), MakeSmokeColumn(Impact, MAX_int32).IsEnabled());
	float PreviousLifetime = 0.0f;
	double PreviousRise = 0.0;
	for (float Yield : {0.0f, 0.125f, 1.0f, 2.0f, 16.0f, 1000.0f, 1.0e30f})
	{
		Impact.YieldTonsTNT = Yield;
		const FSmokeColumn Smoke = MakeSmokeColumn(Impact, 0);
		const float Lifetime = EffectLifetime(Impact, Smoke);
		TestTrue(TEXT("Yield produces finite, explicitly bounded 45-55s lifetime"),
			FMath::IsFinite(Lifetime) && Lifetime >= 45.0f && Lifetime <= MaxSmokeLifetimeSeconds);
		TestTrue(TEXT("Larger yield never shortens the column"), Lifetime >= PreviousLifetime);
		TestTrue(TEXT("Larger yield never slows its rise"), Smoke.VelocityCmPerSecond.Z >= PreviousRise);
		TestTrue(TEXT("Vertical speed remains local, 2.1-5.4 m/s"),
			Smoke.VelocityCmPerSecond.Z >= 210.0 && Smoke.VelocityCmPerSecond.Z <= 540.0);
		TestTrue(TEXT("Seeded horizontal drift remains bounded"),
			Smoke.VelocityCmPerSecond.Size2D() >= 59.9 && Smoke.VelocityCmPerSecond.Size2D() <= 110.1);
		TestTrue(TEXT("Same impact gives identical wind"), Smoke.VelocityCmPerSecond.Equals(MakeSmokeColumn(Impact, 0).VelocityCmPerSecond));
		PreviousLifetime = Lifetime;
		PreviousRise = Smoke.VelocityCmPerSecond.Z;
	}
	for (float Radius : {0.01f, 200.0f, 600.0f, 3000.0f, 1.0e30f})
	{
		Impact.RadiusCm = Radius;
		const FSmokeColumn Smoke = MakeSmokeColumn(Impact, 0);
		TestTrue(TEXT("Column footprint never exceeds explicit bounds"), Smoke.RadiusCm >= 180.0f && Smoke.RadiusCm <= 1200.0f);
	}
	for (float Bad : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		Impact.YieldTonsTNT = Bad;
		TestFalse(TEXT("Nonfinite/negative yield cannot create smoke"), MakeSmokeColumn(Impact, 0).IsEnabled());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinSmokeLifecycleTest,
	"DublinFlight.Effects.SmokeColumnLifecycleAndBudget", DublinImpactFX::Tests::Flags)

bool FDublinSmokeLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	FDublinImpact Bomb;
	Bomb.Kind = EDublinImpactKind::Bomb;
	const double Now = 100.0;
	TArray<FDublinActiveImpactEffect> Active;
	for (int32 Attempt = 0; Attempt < 1000; ++Attempt)
	{
		if (!CanAdmit(Active.Num())) { continue; }
		const FSmokeColumn Smoke = MakeSmokeColumn(Bomb, CountSmokeColumns(Active));
		FDublinActiveImpactEffect& Entry = Active.AddDefaulted_GetRef();
		Entry.bSmokeColumn = Smoke.IsEnabled();
		Entry.Deadline = Now + EffectLifetime(Bomb, Smoke);
	}
	TestEqual(TEXT("Columns remain inside the existing global sixteen-system budget"), Active.Num(), MaxSystems);
	TestEqual(TEXT("Only four of those systems have sustained smoke"), CountSmokeColumns(Active), MaxSmokeColumns);
	TestEqual(TEXT("Fifth bomb still receives the ordinary nine-second burst"), Active[MaxSmokeColumns].Deadline, Now + 9.0);
	const FSmokeColumn Smoke = MakeSmokeColumn(Bomb, 0);
	TestFalse(TEXT("Stopping emission preserves the eight-second particle tail"),
		Active[0].ShouldRelease(Now + Smoke.EmissionSeconds, false));
	TestFalse(TEXT("Column stays alive immediately before its deadline"), Active[0].ShouldRelease(Active[0].Deadline - 0.001, false));
	TestTrue(TEXT("Column reclaimed exactly at its hard deadline"), Active[0].ShouldRelease(Active[0].Deadline, false));
	TestTrue(TEXT("A culled/completed system is reclaimed early, not respawned"), Active[0].ShouldRelease(Now + 1.0, true));
	Active.RemoveAt(0);
	TestTrue(TEXT("Eviction frees a global slot"), CanAdmit(Active.Num()));
	TestTrue(TEXT("Eviction also frees exactly one column slot"), MakeSmokeColumn(Bomb, CountSmokeColumns(Active)).IsEnabled());
	TestEqual(TEXT("Three columns remain after eviction"), CountSmokeColumns(Active), MaxSmokeColumns - 1);
	Active.Empty();
	TestEqual(TEXT("World teardown leaves no column occupancy"), CountSmokeColumns(Active), 0);
	FDublinActiveImpactEffect Corrupt;
	Corrupt.Deadline = std::numeric_limits<double>::infinity();
	TestTrue(TEXT("Nonfinite deadline cannot leak an effect"), Corrupt.ShouldRelease(Now, false));
	Corrupt.Deadline = Now + 10.0;
	TestTrue(TEXT("Nonfinite clock fails closed"), Corrupt.ShouldRelease(std::numeric_limits<double>::quiet_NaN(), false));
	Bomb.bWater = true;
	TestEqual(TEXT("Even a stale smoke profile cannot extend underwater bombs"), EffectLifetime(Bomb, Smoke), 9.0f);
	Bomb.Kind = EDublinImpactKind::Cannon;
	TestEqual(TEXT("Even a stale smoke profile cannot extend water cannon"), EffectLifetime(Bomb, Smoke), 5.0f);
	Bomb.bWater = false;
	TestEqual(TEXT("Land cannon keeps its original lifetime"), EffectLifetime(Bomb, Smoke), 3.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinSmokeBoundsTest,
	"DublinFlight.Effects.SmokeColumnBoundsOrientationAndPoolReset", DublinImpactFX::Tests::Flags)

bool FDublinSmokeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	TStrongObjectPtr<UNiagaraComponent> Component(NewObject<UNiagaraComponent>());
	FDublinImpact Bomb;
	Bomb.Kind = EDublinImpactKind::Bomb;
	Bomb.YieldTonsTNT = 1000.0f;
	Bomb.RadiusCm = 3000.0f;
	Bomb.Seed = 17;
	for (const FVector& Normal : {FVector::UpVector, FVector::ForwardVector, FVector(1, 2, 3).GetSafeNormal(), -FVector::UpVector})
	{
		Bomb.Normal = Normal;
		const FSmokeColumn Smoke = MakeSmokeColumn(Bomb, 0);
		const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
		SetSmokeParameters(*Component, Smoke, Rotation);
		bool bValid = false;
		const FVector LocalVelocity = Component->GetVariableVec3(TEXT("User.SmokeVelocity"), bValid);
		TestTrue(TEXT("Native code supplies a real Niagara vector override"), bValid);
		TestTrue(TEXT("Wall and underside smoke still rises in world space"),
			Rotation.RotateVector(LocalVelocity).Equals(Smoke.VelocityCmPerSecond, 0.001));
		TestEqual(TEXT("Emission duration reaches the component"),
			Component->GetVariableFloat(TEXT("User.SmokeEmissionSeconds"), bValid), Smoke.EmissionSeconds);
		TestTrue(TEXT("Emission duration has the correct Niagara type"), bValid);
		const FBox Bounds = EffectBounds(Bomb, Smoke);
		for (float Age : {0.0f, SmokeParticleLifetimeSeconds})
		{
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Padding = FVector((Corner & 1) ? 1 : -1, (Corner & 2) ? 1 : -1, (Corner & 4) ? 1 : -1) * Smoke.RadiusCm * 4.0f;
				const FVector Position = Rotation.UnrotateVector(Smoke.VelocityCmPerSecond * Age + Padding);
				TestTrue(TEXT("Local system bounds contain rising/drifting sprite envelope"),
					Bounds.ExpandBy(0.01).IsInsideOrOn(Position));
			}
		}
		SetSmokeParameters(*Component, FSmokeColumn(), Rotation);
		for (const TCHAR* Name : {TEXT("User.SmokeEmissionSeconds"), TEXT("User.SmokeParticleLifetime"), TEXT("User.SmokeRadius")})
		{
			TestEqual(FString(Name) + TEXT(" reset on pooled burst-only reuse"), Component->GetVariableFloat(Name, bValid), 0.0f);
			TestTrue(TEXT("Reset preserves the float parameter type"), bValid);
		}
		TestTrue(TEXT("Pooled velocity is also cleared"), Component->GetVariableVec3(TEXT("User.SmokeVelocity"), bValid).IsZero());
	}
	Bomb.Normal = FVector::ZeroVector;
	TestFalse(TEXT("Invalid payload has no valid visual bounds"), EffectBounds(Bomb, FSmokeColumn()).IsValid != 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinWaterColumnTest,
	"DublinFlight.Effects.CannonWaterColumnScaleBoundsAndLifetime", DublinImpactFX::Tests::Flags)

bool FDublinWaterColumnTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	FDublinImpact Impact;
	const FDublinImpact Original = Impact;
	TestEqual(TEXT("Land cannon radius unchanged"), VisualRadius(Impact), 200.0f);
	TestEqual(TEXT("Land cannon strength unchanged"), VisualStrength(Impact), 1.0f);
	Impact.bWater = true;
	TestEqual(TEXT("Cannon water footprint uses the authored radius binding"), VisualRadius(Impact), 400.0f);
	TestEqual(TEXT("Cannon water velocity uses the authored strength binding"), VisualStrength(Impact), 3.0f);
	TestEqual(TEXT("Water mist is no longer killed at three seconds"), EffectLifetime(Impact, FSmokeColumn()), 5.0f);
	TestEqual(TEXT("Visual calculations never mutate physics radius"), Impact.RadiusCm, Original.RadiusCm);
	TestEqual(TEXT("Visual calculations never mutate damage strength"), Impact.Strength, Original.Strength);
	TestEqual(TEXT("Visual calculations never mutate crater depth"), Impact.CraterDepthCm, Original.CraterDepthCm);
	const float Radius = VisualRadius(Impact);
	const float JetSpeed = Radius * 1.75f * FMath::Sqrt(VisualStrength(Impact));
	const float ApexCm = JetSpeed * JetSpeed / (2.0f * 980.0f);
	TestTrue(TEXT("Authored HotCore ballistic estimate is a readable 7-8m, not a nuclear column"),
		ApexCm > 700.0f && ApexCm < 800.0f);
	const float RingRadius = Radius * (0.3f + 0.7f * 2.5f);
	const FBox Bounds = EffectBounds(Impact, FSmokeColumn());
	TestTrue(TEXT("Expanded ring remains within the same system envelope"), Bounds.IsInsideOrOn(FVector(RingRadius, 0, 20)));
	TestTrue(TEXT("Conservative jet apex remains in bounds"), Bounds.IsInsideOrOn(FVector(0, 0, ApexCm)));
	TestTrue(TEXT("Ballistic droplet tail cannot escape lower bounds"),
		Bounds.IsInsideOrOn(FVector(0, 0, -0.5f * 980.0f * 2.8f * 2.8f)));
	Impact.RadiusCm = 1.0e30f;
	Impact.Strength = 1.0e30f;
	TestEqual(TEXT("Water cannon retains the four-metre visual radius cap"), VisualRadius(Impact), 400.0f);
	TestEqual(TEXT("Water cannon retains the fourfold strength cap"), VisualStrength(Impact), 4.0f);
	Impact.Kind = EDublinImpactKind::Bomb;
	TestEqual(TEXT("Water bombs retain their existing radius scaling"), VisualRadius(Impact), 3000.0f);
	Impact.RadiusCm = Original.RadiusCm;
	Impact.Strength = Original.Strength;
	TestEqual(TEXT("Water bomb strength is not multiplied like cannon"), VisualStrength(Impact), 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinSmokeParameterContractTest,
	"DublinFlight.Effects.SmokeColumnParameterContractFailures", DublinImpactFX::Tests::Flags)

bool FDublinSmokeParameterContractTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	FString Failure;
	TestFalse(TEXT("Missing system explicitly rejected"), ValidateSmokeColumnParameters(nullptr, Failure));
	TestFalse(TEXT("Missing system includes diagnostics"), Failure.IsEmpty());
	TestFalse(TEXT("Missing material system explicitly rejected"), ValidateSpriteMaterials(nullptr, Failure));
	TStrongObjectPtr<UNiagaraSystem> System(NewObject<UNiagaraSystem>());
	TestFalse(TEXT("No sprites cannot silently pass material validation"), ValidateSpriteMaterials(System.Get(), Failure));
	TestTrue(TEXT("No-renderer failure is actionable"), Failure.Contains(TEXT("no enabled sprite renderers")));
	FNiagaraUserRedirectionParameterStore& User = System->GetExposedParameters();
	for (const TCHAR* Name : {TEXT("User.SmokeEmissionSeconds"), TEXT("User.SmokeParticleLifetime"), TEXT("User.SmokeRadius")})
	{
		TestFalse(TEXT("Partial contract cannot silently enable smoke"), ValidateSmokeColumnParameters(System.Get(), Failure));
		User.AddParameter(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Name));
	}
	User.AddParameter(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SmokeVelocity")));
	TestFalse(TEXT("Velocity with the wrong Niagara type is rejected"), ValidateSmokeColumnParameters(System.Get(), Failure));
	TestTrue(TEXT("Failure names the broken parameter"), Failure.Contains(TEXT("User.SmokeVelocity")));
	User.RemoveParameter(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SmokeVelocity")));
	User.AddParameter(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("User.SmokeVelocity")));
	TestTrue(TEXT("Complete typed contract is accepted"), ValidateSmokeColumnParameters(System.Get(), Failure));
	TestTrue(TEXT("Successful validation clears stale diagnostics"), Failure.IsEmpty());
	TestFalse(TEXT("Exposing parameters without a real emitter cannot enable a column"), ValidateSmokeColumnAsset(System.Get(), Failure));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinImpactAssetsTest,
	"DublinFlight.Effects.AuthoredAssetContract", DublinImpactFX::Tests::Flags)

bool FDublinImpactAssetsTest::RunTest(const FString& Parameters)
{
	using namespace DublinImpactFX;
	UWorld* InitializationWorld = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Effects initialization fixture world"), InitializationWorld)) { return false; }
	ON_SCOPE_EXIT { InitializationWorld->DestroyWorld(false); };
	TestNotNull(TEXT("Native effects subsystem initialized without gameplay"),
		InitializationWorld->GetSubsystem<UDublinImpactEffectsSubsystem>());
	int32 NiagaraComponents = 0;
	int32 PlayingAudioComponents = 0;
	for (TObjectIterator<UNiagaraComponent> It; It; ++It)
	{
		if (!It->IsTemplate() && It->GetWorld() == InitializationWorld) { ++NiagaraComponents; }
	}
	for (TObjectIterator<UAudioComponent> It; It; ++It)
	{
		if (!It->IsTemplate() && It->GetWorld() == InitializationWorld && It->IsPlaying()) { ++PlayingAudioComponents; }
	}
	TestEqual(TEXT("Initialization spawns no Niagara gameplay components"), NiagaraComponents, 0);
	TestEqual(TEXT("Initialization plays no impact audio"), PlayingAudioComponents, 0);
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
