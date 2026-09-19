#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "City/DublinCityData.h"
#include "City/DublinCityMeshComponent.h"
#include "City/DublinCityWorld.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "LegacyScreenPercentageDriver.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityResolutionPolicyTest,
	"DublinFlight.City.Visual.ResolutionPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityResolutionPolicyTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Resolution policy is inspected on the game thread"), IsInGameThread())) { return false; }
	IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	if (!TestNotNull(TEXT("Engine screen-percentage CVar exists"), ScreenPercentage)) { return false; }
	const FString OriginalValue = ScreenPercentage->GetString();
	const EConsoleVariableFlags OriginalFlags = ScreenPercentage->GetFlags();
	bool bPassed = true;

	for (const TCHAR* Name : {
		TEXT("r.ScreenPercentage"), TEXT("r.ScreenPercentage.Default"),
		TEXT("r.ScreenPercentage.Default.Desktop.Mode"), TEXT("r.ScreenPercentage.Auto.PixelCountMultiplier"),
		TEXT("r.ScreenPercentage.MinResolution"), TEXT("r.ScreenPercentage.MaxResolution"),
		TEXT("r.SecondaryScreenPercentage.GameViewport"), TEXT("r.DynamicRes.OperationMode"),
		TEXT("r.Editor.Viewport.OverridePIEScreenPercentage"), TEXT("r.Editor.Viewport.ScreenPercentage"),
		TEXT("r.Editor.Viewport.ScreenPercentageMode.RealTime") })
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!TestNotNull(FString::Printf(TEXT("Required CVar %s exists"), Name), Variable))
		{
			bPassed = false;
			continue;
		}
		AddInfo(FString::Printf(TEXT("%s=%s flags=0x%08x"), Name, *Variable->GetString(),
			static_cast<uint32>(Variable->GetFlags())));
	}
	AddInfo(TEXT("Measured fractions below come from UE FStaticResolutionFractionHeuristic, not a duplicated formula. "
		"Fixed probes use one desktop view, secondary fraction=1 and DPI=1. Targets are unrounded policy results, "
		"not captured GPU view rectangles; dynamic resolution and renderer alignment are not measured."));
	AddInfo(FString::Printf(TEXT("Editor PIE override enabled (positive runtime screen percentage takes precedence): %s"),
		FStaticResolutionFractionHeuristic::FUserSettings::EditorOverridePIESettings() ? TEXT("yes") : TEXT("no")));

	auto Measure = [this, &bPassed](const TCHAR* Label, const FStaticResolutionFractionHeuristic& Heuristic,
		const FIntPoint& OutputSize)
	{
		const float Fraction = Heuristic.ResolveResolutionFraction();
		bPassed &= TestTrue(FString::Printf(TEXT("%s returns a finite positive fraction"), Label),
			FMath::IsFinite(Fraction) && Fraction > 0.0f);
		AddInfo(FString::Printf(
			TEXT("%s: output=%dx%d mode=%d primary=%.6f%% unroundedTarget=%.3fx%.3f min=%.3f max=%.3f"),
			Label, OutputSize.X, OutputSize.Y, static_cast<int32>(Heuristic.Settings.Mode),
			Fraction * 100.0f, OutputSize.X * Fraction, OutputSize.Y * Fraction,
			Heuristic.Settings.MinRenderingResolution, Heuristic.Settings.MaxRenderingResolution));
		return Fraction;
	};

	const FIntPoint ProbeSize(1920, 1080);
	FStaticResolutionFractionHeuristic Runtime;
	Runtime.Settings.PullRunTimeRenderingSettings(EViewStatusForScreenPercentage::Desktop);
	Runtime.TotalDisplayedPixelCount = ProbeSize.X * ProbeSize.Y;
	Measure(TEXT("Live runtime/PIE policy"), Runtime, ProbeSize);

	FStaticResolutionFractionHeuristic Editor;
	Editor.Settings.PullEditorRenderingSettings(EViewStatusForScreenPercentage::Desktop);
	Editor.TotalDisplayedPixelCount = Runtime.TotalDisplayedPixelCount;
	Measure(TEXT("Live editor desktop default policy"), Editor, ProbeSize);

	// Isolate the primary mode with identical resolved clamps, not a live CVar/render A/B.
	// No viewport, user setting or CVar is changed.
	FStaticResolutionFractionHeuristic Automatic = Runtime;
	Automatic.Settings.Mode = EScreenPercentageMode::BasedOnDisplayResolution;
	const float AutomaticFraction = Measure(TEXT("Automatic control (live clamps retained)"), Automatic, ProbeSize);
	FStaticResolutionFractionHeuristic Native = Runtime;
	Native.Settings.Mode = EScreenPercentageMode::Manual;
	Native.Settings.GlobalResolutionFraction = 1.0f;
	const float NativeFraction = Measure(TEXT("Native100 control (live clamps retained)"), Native, ProbeSize);
	AddInfo(FString::Printf(TEXT("Automatic versus native100: primary %.6f%% versus %.6f%%; "
		"automatic is %s native100. No visual sharpness or performance acceptance is implied."),
		AutomaticFraction * 100.0f, NativeFraction * 100.0f,
		AutomaticFraction < NativeFraction ? TEXT("below") : TEXT("not below")));

	FStaticResolutionFractionHeuristic Contract;
	Contract.TotalDisplayedPixelCount = Runtime.TotalDisplayedPixelCount;
	bPassed &= TestEqual(TEXT("Unclamped manual 100 resolves to one"), Contract.ResolveResolutionFraction(), 1.0f);
	Contract.Settings.GlobalResolutionFraction = 0.5f;
	bPassed &= TestEqual(TEXT("Unclamped manual 50 resolves to one half"), Contract.ResolveResolutionFraction(), 0.5f);
	Contract.Settings.GlobalResolutionFraction = 1.0f;
	Contract.Settings.MaxRenderingResolution = 540.0f;
	bPassed &= TestTrue(TEXT("A 540p cap still limits manual100 at 1080p"),
		FMath::IsNearlyEqual(Contract.ResolveResolutionFraction(), 0.5f, 0.0001f));

	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		AddInfo(FString::Printf(TEXT("Current game viewport output=%dx%d; internal view rectangles not sampled."),
			Size.X, Size.Y));
	}
	else
	{
		AddInfo(TEXT("Current game viewport unavailable; fixed policy probes remain valid."));
	}

	bool bFoundEditorViewport = false;
	if (GEditor)
	{
		FViewport* ActiveViewport = GEditor->GetActiveViewport();
		for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (!Client || !ActiveViewport || Client->Viewport != ActiveViewport) { continue; }
			bFoundEditorViewport = true;
			const FIntPoint Size = ActiveViewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
			{
				AddInfo(TEXT("Active editor viewport has no drawable output; live viewport policy probe skipped."));
				break;
			}
			const float Fraction = Client->GetDefaultPrimaryResolutionFractionTarget();
			bPassed &= TestTrue(TEXT("Active editor default target is finite and positive"),
				FMath::IsFinite(Fraction) && Fraction > 0.0f);
			AddInfo(FString::Printf(TEXT("Active editor viewport: output=%dx%d viewStatus=%d "
				"defaultPrimary=%.6f%% previewEnabled=%d previewPercentage=%d lowDPIPreview=%d. "
				"Default target uses the real viewport heuristic including DPI; preview is a UI value, "
				"not proof of the rendered internal view dimensions."),
				Size.X, Size.Y, static_cast<int32>(Client->GetViewStatusForScreenPercentage()),
				Fraction * 100.0f, Client->IsPreviewingScreenPercentage(), Client->GetPreviewScreenPercentage(),
				Client->IsLowDPIPreview()));
			break;
		}
	}
	if (!bFoundEditorViewport)
	{
		AddInfo(TEXT("No active editor viewport client available; no live internal-resolution claim is made."));
	}
	bPassed &= TestEqual(TEXT("Diagnostic preserves screen-percentage value"), ScreenPercentage->GetString(), OriginalValue);
	bPassed &= TestEqual(TEXT("Diagnostic preserves screen-percentage priority and flags"), ScreenPercentage->GetFlags(), OriginalFlags);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityOrthophotoResidencyTest,
	"DublinFlight.City.Visual.OrthophotoResidency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityOrthophotoResidencyTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Texture residency is inspected on the game thread"), IsInGameThread())) { return false; }
	UTexture2D* Texture = FindObject<UTexture2D>(nullptr, TEXT("/Game/Textures/T_DublinOrtho.T_DublinOrtho"));
	if (!TestNotNull(TEXT("Orthophoto must already be loaded: run after rendering the real city in PIE"), Texture))
	{
		return false;
	}
	AddInfo(TEXT("Passive game-thread residency snapshot: no asset loading, compilation waits, streaming flushes, "
		"residency requests or CVar changes. Resident dimensions are availability, not the shader-selected mip."));
	int32 BegunPIEWorlds = 0;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World() && Context.World()->HasBegunPlay())
			{
				++BegunPIEWorlds;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("BegunPIEWorlds=%d; asset is shared with editor views. "
		"If zero, this snapshot does not establish that gameplay has rendered it."), BegunPIEWorlds));
	bool bPassed = true;
	for (const TCHAR* Name : {
		TEXT("r.TextureStreaming"), TEXT("r.Streaming.UseNewMetrics"), TEXT("r.Streaming.Boost"),
		TEXT("r.Streaming.MipBias"), TEXT("r.Streaming.UsePerTextureBias"), TEXT("r.Streaming.PoolSize"),
		TEXT("r.Streaming.FullyLoadUsedTextures"), TEXT("r.Streaming.UseAllMips"), TEXT("r.MipMapLODBias") })
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!TestNotNull(FString::Printf(TEXT("Required CVar %s exists"), Name), Variable))
		{
			bPassed = false;
			continue;
		}
		AddInfo(FString::Printf(TEXT("%s=%s"), Name, *Variable->GetString()));
	}
	if (!TestFalse(TEXT("No placeholder/compiling texture may be reported as orthophoto residency"), Texture->IsCompiling()) ||
		!TestTrue(TEXT("Platform data must already be ready; diagnostic will not wait"), Texture->IsAsyncCacheComplete()))
	{
		return false;
	}
	if (!TestFalse(TEXT("Diagnostic requires conventional mips, not virtual texture tile residency"),
		Texture->IsCurrentlyVirtualTextured())) { return false; }
	const FTexturePlatformData* Platform = Texture->GetPlatformData();
	if (!TestNotNull(TEXT("Real orthophoto platform data exists"), Platform)) { return false; }
	bPassed &= TestTrue(TEXT("Orthophoto source metadata is valid"), Texture->Source.IsValid());
	AddInfo(FString::Printf(TEXT("Orthophoto source=%lldx%lld sourceMips=%d platform=%dx%d platformMips=%d "
		"pixelFormat=%d LODBias=%d combinedLODBias=%d MaxTextureSize=%d LODGroup=%d"),
		static_cast<long long>(Texture->Source.GetSizeX()), static_cast<long long>(Texture->Source.GetSizeY()),
		Texture->Source.GetNumMips(), Platform->SizeX, Platform->SizeY, Texture->GetNumMips(),
		static_cast<int32>(Platform->PixelFormat), Texture->LODBias, Texture->GetCachedLODBias(),
		Texture->MaxTextureSize, static_cast<int32>(Texture->LODGroup)));
	AddInfo(FString::Printf(TEXT("NeverStream=%d virtualStreamingSetting=%d supportsStreaming=%d "
		"linkedToStreamer=%d forceGlobal=%d forceTransient=%d forceEffective=%d ignoreStreamingMipBias=%d "
		"pendingInitOrStreaming=%d streamingUpdatePending=%d"),
		static_cast<int32>(Texture->NeverStream), static_cast<int32>(Texture->VirtualTextureStreaming),
		Texture->RenderResourceSupportsStreaming(), Texture->IsStreamable(),
		static_cast<int32>(Texture->bGlobalForceMipLevelsToBeResident),
		static_cast<int32>(Texture->bForceMiplevelsToBeResident), Texture->ShouldMipLevelsBeForcedResident(),
		static_cast<int32>(Texture->bIgnoreStreamingMipBias), Texture->HasPendingInitOrStreaming(),
		static_cast<int32>(Texture->bHasStreamingUpdatePending)));
	const FStreamableRenderResourceState State = Texture->GetStreamableResourceState();
	if (!TestTrue(TEXT("Game-thread render-resource residency state is valid"), State.IsValid())) { return false; }
	const int32 Resident = State.NumResidentLODs;
	const int32 Requested = State.NumRequestedLODs;
	AddInfo(FString::Printf(TEXT("Resource maxMips=%d assetLODBias=%d LODBiasModifier=%d "
		"nonStreamingMips=%d nonOptionalMips=%d residentMips=%d requestedMips=%d"),
		static_cast<int32>(State.MaxNumLODs), static_cast<int32>(State.AssetLODBias),
		static_cast<int32>(State.LODBiasModifier), static_cast<int32>(State.NumNonStreamingLODs),
		static_cast<int32>(State.NumNonOptionalLODs), Resident, Requested));
#if !UE_BUILD_SHIPPING
	AddInfo(FString::Printf(TEXT("Streamer cachedWantedMips=%d (debug cache; zero may be unpopulated, "
		"requestedMips is the resource's current request target)"), static_cast<int32>(Texture->GetCachedNumWantedLODs())));
#endif
	bPassed &= TestEqual(TEXT("Resident mip accessor agrees with game-thread state"), Texture->GetNumResidentMips(), Resident);
	bPassed &= TestEqual(TEXT("Platform mip accessor agrees with the actual array"), Texture->GetNumMips(), Platform->Mips.Num());
	if (!TestTrue(TEXT("Resident mip count is within resource limits"),
		Resident > 0 && Resident >= State.NumNonStreamingLODs && Resident <= State.MaxNumLODs) ||
		!TestTrue(TEXT("Requested mip count is within resource limits"),
			Requested > 0 && Requested >= State.NumNonStreamingLODs && Requested <= State.MaxNumLODs))
	{
		return false;
	}
	// Resource mip zero can differ from asset mip zero; use UE's mapping, not totalMips-resident.
	const int32 ResidentIndex = State.LODCountToAssetFirstLODIdx(Resident);
	const int32 RequestedIndex = State.LODCountToAssetFirstLODIdx(Requested);
	if (!TestTrue(TEXT("Resident top mip maps into platform data"), Platform->Mips.IsValidIndex(ResidentIndex)) ||
		!TestTrue(TEXT("Requested top mip maps into platform data"), Platform->Mips.IsValidIndex(RequestedIndex)))
	{
		return false;
	}
	const FTexture2DMipMap& ResidentMip = Platform->Mips[ResidentIndex];
	const FTexture2DMipMap& RequestedMip = Platform->Mips[RequestedIndex];
	if (!TestTrue(TEXT("Resident and requested mip dimensions are nonzero"),
		ResidentMip.SizeX > 0 && ResidentMip.SizeY > 0 && RequestedMip.SizeX > 0 && RequestedMip.SizeY > 0))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("TopResident assetMip=%d size=%dx%d fullCoreMetersPerTexel=%.6fx%.6f; "
		"TopRequested assetMip=%d size=%dx%d. Core extent=%.1fm. "
		"Low residency is evidence to investigate demand/budget/latency, not proof of the root cause."),
		ResidentIndex, static_cast<int32>(ResidentMip.SizeX), static_cast<int32>(ResidentMip.SizeY),
		DublinCity::ExtentMeters / ResidentMip.SizeX, DublinCity::ExtentMeters / ResidentMip.SizeY,
		RequestedIndex, static_cast<int32>(RequestedMip.SizeX), static_cast<int32>(RequestedMip.SizeY), DublinCity::ExtentMeters));
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityAerialStreamingDensityTest,
	"DublinFlight.City.Visual.AerialStreamingDensity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityAerialStreamingDensityTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_DublinAerial.M_DublinAerial"));
	UTexture2D* Ortho = LoadObject<UTexture2D>(nullptr, TEXT("/Game/Textures/T_DublinOrtho.T_DublinOrtho"));
	UTexture2D* Coverage = LoadObject<UTexture2D>(nullptr, TEXT("/Game/Textures/T_DublinCoverage.T_DublinCoverage"));
	if (!TestNotNull(TEXT("Real aerial material"), Material) || !TestNotNull(TEXT("Real orthophoto"), Ortho) ||
		!TestNotNull(TEXT("Real coverage mask"), Coverage)) { return false; }
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Isolated streaming fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Fixture owner"), Owner)) { return false; }
	UProceduralMeshComponent* Stock = NewObject<UProceduralMeshComponent>(Owner);
	UDublinCityMeshComponent* City = NewObject<UDublinCityMeshComponent>(Owner);
	for (UProceduralMeshComponent* Mesh : {Stock, static_cast<UProceduralMeshComponent*>(City)})
	{
		Owner->AddInstanceComponent(Mesh);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->RegisterComponent();
		Mesh->SetMaterial(0, Material);
	}
	auto SetSquare = [](UProceduralMeshComponent* Mesh, double Side)
	{
		const TArray<FVector> Vertices = {FVector(0, 0, 0), FVector(0, Side, 0), FVector(Side, 0, 0), FVector(Side, Side, 0)};
		const TArray<FVector2D> UVs = {DublinCity::AerialUV(Vertices[0]), DublinCity::AerialUV(Vertices[1]),
			DublinCity::AerialUV(Vertices[2]), DublinCity::AerialUV(Vertices[3])};
		Mesh->CreateMeshSection(0, Vertices, {0, 1, 2, 2, 1, 3}, TArray<FVector>(), UVs,
			TArray<FColor>(), TArray<FProcMeshTangent>(), false);
	};
	auto Gather = [](const UProceduralMeshComponent* Mesh)
	{
		FStreamingTextureLevelContext Context(EMaterialQualityLevel::High, GMaxRHIFeatureLevel, true);
		TArray<FStreamingRenderAssetPrimitiveInfo> Result;
		Mesh->GetStreamingRenderAssetInfo(Context, Result);
		return Result;
	};
	auto Find = [](const TArray<FStreamingRenderAssetPrimitiveInfo>& Infos, const UTexture* Texture)
	{
		return Infos.FindByPredicate([Texture](const FStreamingRenderAssetPrimitiveInfo& Info) { return Info.RenderAsset == Texture; });
	};
	bool bPassed = true;
	for (double Side : {6400.0, 12800.0})
	{
		for (double Scale : {1.0, 2.0})
		{
			const FTransform Transform(FRotator(0, 37, 0), FVector(12000, -4000, 500), FVector(Scale, -Scale, Scale * 0.5));
			for (UProceduralMeshComponent* Mesh : {Stock, static_cast<UProceduralMeshComponent*>(City)})
			{
				SetSquare(Mesh, Side);
				Mesh->SetWorldTransform(Transform);
			}
			const TArray<FStreamingRenderAssetPrimitiveInfo> StockInfos = Gather(Stock);
			const TArray<FStreamingRenderAssetPrimitiveInfo> CityInfos = Gather(City);
			bPassed &= TestEqual(TEXT("No extra or removed streaming records"), CityInfos.Num(), StockInfos.Num());
			bPassed &= TestEqual(TEXT("PMC streamer applies no second transform scale"), City->GetStreamingScale(), 1.0f);
			for (const UTexture* Texture : {static_cast<UTexture*>(Ortho), static_cast<UTexture*>(Coverage)})
			{
				const FStreamingRenderAssetPrimitiveInfo* Before = Find(StockInfos, Texture);
				const FStreamingRenderAssetPrimitiveInfo* After = Find(CityInfos, Texture);
				// A non-streamable mask can legitimately be absent; never manufacture a streaming entry for it.
				if (Texture == Coverage && !Before)
				{
					bPassed &= TestNull(TEXT("Non-streamable coverage remains absent"), After);
					continue;
				}
				if (!TestNotNull(TEXT("Stock material reports aerial texture"), Before) ||
					!TestNotNull(TEXT("City material retains aerial texture"), After)) { return false; }
				bPassed &= TestTrue(TEXT("Full-core density is independent of chunk extent and scaled once"),
					FMath::IsNearlyEqual(After->TexelFactor, static_cast<float>(76800.0 * Scale), 1.0f));
				bPassed &= TestTrue(TEXT("Corrected bounds retain the component's world position"),
					After->Bounds.Origin.Equals(Before->Bounds.Origin));
				bPassed &= TestTrue(TEXT("Corrected bounds retain the component extent"),
					After->Bounds.BoxExtent.Equals(Before->Bounds.BoxExtent));
				bPassed &= TestEqual(TEXT("Bounds radius is not expanded to survey extent"), After->Bounds.SphereRadius, Before->Bounds.SphereRadius);
				bPassed &= TestEqual(TEXT("Relative-box metadata preserved"), After->PackedRelativeBox, Before->PackedRelativeBox);
				bPassed &= TestEqual(TEXT("Scale policy preserved"), static_cast<bool>(After->bAffectedByComponentScale),
					static_cast<bool>(Before->bAffectedByComponentScale));
				AddInfo(FString::Printf(TEXT("Aerial fixture side=%.0f scale=%.1f texture=%s stockFactor=%.3f correctedFactor=%.3f"),
					Side, Scale, *Texture->GetName(), Before->TexelFactor, After->TexelFactor));
			}
		}
	}
	SetSquare(City, 6400.0);
	const FProcMeshSection* Section = City->GetProcMeshSection(0);
	bPassed &= TestTrue(TEXT("64m fixture actually spans 1/12 of the aerial UV"),
		Section && FMath::IsNearlyEqual(Section->ProcVertexBuffer[2].UV0.X - Section->ProcVertexBuffer[0].UV0.X, 1.0 / 12.0, 0.000001));

	UTexture2D* Unrelated = DuplicateObject<UTexture2D>(Ortho, GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("UnrelatedAerialFixture")));
	if (!TestNotNull(TEXT("Unrelated texture fixture"), Unrelated)) { return false; }
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, Owner);
	if (!TestNotNull(TEXT("Material-parameter fixture"), Instance)) { return false; }
	Instance->SetTextureParameterValue(TEXT("AerialTexture"), Unrelated);
	Stock->SetMaterial(0, Instance);
	City->SetMaterial(0, Instance);
	SetSquare(Stock, 6400.0);
	const TArray<FStreamingRenderAssetPrimitiveInfo> StockOther = Gather(Stock);
	const TArray<FStreamingRenderAssetPrimitiveInfo> CityOther = Gather(City);
	const FStreamingRenderAssetPrimitiveInfo* BeforeOther = Find(StockOther, Unrelated);
	const FStreamingRenderAssetPrimitiveInfo* AfterOther = Find(CityOther, Unrelated);
	if (!TestNotNull(TEXT("Unrelated texture is exercised through real material streaming"), BeforeOther) ||
		!TestNotNull(TEXT("Unrelated texture retained"), AfterOther)) { return false; }
	bPassed &= TestEqual(TEXT("Non-city texture density is unchanged"), AfterOther->TexelFactor, BeforeOther->TexelFactor);
	bPassed &= TestTrue(TEXT("Non-city texture bounds unchanged"), AfterOther->Bounds.Origin.Equals(BeforeOther->Bounds.Origin) &&
		AfterOther->Bounds.BoxExtent.Equals(BeforeOther->Bounds.BoxExtent));

	FStreamingTextureLevelContext Context(EMaterialQualityLevel::High, GMaxRHIFeatureLevel, true);
	TArray<FStreamingRenderAssetPrimitiveInfo> Prefilled;
	Prefilled.Emplace(Ortho, City->Bounds, 123.0f);
	City->GetStreamingRenderAssetInfo(Context, Prefilled);
	bPassed &= TestEqual(TEXT("Caller-owned prefix is not corrected"), Prefilled[0].TexelFactor, 123.0f);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityGeneratedMeshCompatibilityTest,
	"DublinFlight.City.Visual.GeneratedMeshCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityGeneratedMeshCompatibilityTest::RunTest(const FString& Parameters)
{
	bool bPassed = TestTrue(TEXT("New exact generated class accepted"),
		UDublinCityMeshComponent::IsGeneratedCityMeshClass(UDublinCityMeshComponent::StaticClass()));
	bPassed &= TestTrue(TEXT("Exact legacy generated class accepted"),
		UDublinCityMeshComponent::IsGeneratedCityMeshClass(UProceduralMeshComponent::StaticClass()));
	UClass* ForeignClass = NewObject<UClass>();
	ForeignClass->SetSuperStruct(UDublinCityMeshComponent::StaticClass());
	bPassed &= TestTrue(TEXT("Fixture represents a foreign subclass"), ForeignClass->IsChildOf(UDublinCityMeshComponent::StaticClass()));
	bPassed &= TestFalse(TEXT("Foreign subclasses are not accepted by the lifecycle gate"),
		UDublinCityMeshComponent::IsGeneratedCityMeshClass(ForeignClass));
	ForeignClass->SetSuperStruct(UProceduralMeshComponent::StaticClass());
	bPassed &= TestFalse(TEXT("Foreign stock-PMC subclasses are not accepted either"),
		UDublinCityMeshComponent::IsGeneratedCityMeshClass(ForeignClass));
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Isolated lifecycle fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	ADublinCityWorld* City = World->SpawnActorDeferred<ADublinCityWorld>(
		ADublinCityWorld::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("City fixture"), City)) { return false; }
	City->bAutoBuild = false;
	City->FinishSpawning(FTransform::Identity);
	auto Attach = [City](UProceduralMeshComponent* Mesh, bool bTag)
	{
		City->AddInstanceComponent(Mesh);
		Mesh->SetupAttachment(City->GetRootComponent());
		if (bTag) { Mesh->ComponentTags.Add(TEXT("DublinFlight.City.GeneratedMesh.v1")); }
		Mesh->RegisterComponent();
	};
	UProceduralMeshComponent* Legacy = NewObject<UProceduralMeshComponent>(City, TEXT("DublinTerrain_Legacy"));
	UDublinCityMeshComponent* Generated = NewObject<UDublinCityMeshComponent>(City, TEXT("DublinBuildings_New"));
	UDublinCityMeshComponent* Untagged = NewObject<UDublinCityMeshComponent>(City, TEXT("DublinBuildings_Manual"));
	Attach(Legacy, true);
	Attach(Generated, true);
	Attach(Untagged, false);
	AActor* OtherActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Foreign fixture owner"), OtherActor)) { return false; }
	UProceduralMeshComponent* ForeignOwner = NewObject<UProceduralMeshComponent>(OtherActor, TEXT("DublinTerrain_Foreign"));
	Attach(ForeignOwner, true);
	City->SourceDataRelativePath = TEXT("../diagnostic-invalid.json");
	AddExpectedError(TEXT("city build failed:"), EAutomationExpectedErrorFlags::Contains, 1);
	City->BuildCity();
	TInlineComponentArray<UProceduralMeshComponent*> Remaining;
	City->GetComponents(Remaining);
	bPassed &= TestFalse(TEXT("Untracked legacy generated mesh reclaimed"), Remaining.Contains(Legacy));
	bPassed &= TestFalse(TEXT("Untracked new generated mesh reclaimed"), Remaining.Contains(Generated));
	bPassed &= TestTrue(TEXT("Untagged component retained"), Remaining.Contains(Untagged));
	bPassed &= TestTrue(TEXT("Foreign-owned component retained"), IsValid(ForeignOwner) && ForeignOwner->IsRegistered());
	return bPassed;
}

#endif
