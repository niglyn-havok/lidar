#include "DublinFlightPerformanceSubsystem.h"
#include "DublinFlightBenchmark.h"

#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinFlightPerformance, Log, All);

namespace
{
	using namespace DublinFlight::Performance;

	double GetSharedFrameBoundarySeconds()
	{
		// Every capturing world sees exactly the same timestamp for this engine frame.
		static TOptional<uint64> SampledEngineFrame;
		static double BoundarySeconds = 0.0;
		if (!SampledEngineFrame.IsSet() || SampledEngineFrame.GetValue() != GFrameCounter)
		{
			BoundarySeconds = FPlatformTime::Seconds();
			SampledEngineFrame = GFrameCounter;
		}
		return BoundarySeconds;
	}

	TSharedRef<FJsonObject> ReadScenario(UWorld& World)
	{
		const TSharedRef<FJsonObject> Scenario = MakeShared<FJsonObject>();
		Scenario->SetStringField(TEXT("worldName"), World.GetName());
		Scenario->SetStringField(TEXT("worldType"), World.WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Game"));
		Scenario->SetBoolField(TEXT("isPIE"), World.WorldType == EWorldType::PIE);
		Scenario->SetBoolField(TEXT("isDedicatedServer"), World.GetNetMode() == NM_DedicatedServer);
		Scenario->SetBoolField(TEXT("worldPaused"), World.IsPaused());
		Scenario->SetNumberField(TEXT("effectiveTimeDilation"), World.GetWorldSettings()->GetEffectiveTimeDilation());
		Scenario->SetStringField(TEXT("buildConfiguration"), LexToString(FApp::GetBuildConfiguration()));
		Scenario->SetBoolField(TEXT("requiresCookedData"), FPlatformProperties::RequiresCookedData());
		Scenario->SetBoolField(TEXT("isEditorProcess"), GIsEditor);
		Scenario->SetBoolField(TEXT("nullRHI"), GUsingNullRHI);
		Scenario->SetBoolField(TEXT("useFixedTimeStep"), FApp::UseFixedTimeStep());
		Scenario->SetNumberField(TEXT("fixedDeltaSeconds"), FApp::GetFixedDeltaTime());
		Scenario->SetBoolField(TEXT("engineFixedFrameRate"), GEngine && GEngine->bUseFixedFrameRate);
		Scenario->SetBoolField(TEXT("engineSmoothFrameRate"), GEngine && GEngine->bSmoothFrameRate);
		Scenario->SetNumberField(TEXT("engineFixedFrameRateValue"), GEngine ? GEngine->FixedFrameRate : 0);
		Scenario->SetBoolField(TEXT("runningOnBattery"), FPlatformMisc::IsRunningOnBattery());

		const TSharedRef<FJsonObject> Viewport = MakeShared<FJsonObject>();
		UGameViewportClient* ViewportClient = World.GetGameViewport();
		if (ViewportClient && ViewportClient->Viewport)
		{
			Scenario->SetStringField(TEXT("engineShowFlags"), ViewportClient->EngineShowFlags.ToString());
			const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
			const bool bHasDimensions = Size.X > 0 && Size.Y > 0;
			Viewport->SetBoolField(TEXT("available"), true);
			Viewport->SetBoolField(TEXT("dimensionsAvailable"), bHasDimensions);
			if (bHasDimensions)
			{
				Viewport->SetNumberField(TEXT("width"), Size.X);
				Viewport->SetNumberField(TEXT("height"), Size.Y);
			}
			else
			{
				Viewport->SetField(TEXT("width"), MakeShared<FJsonValueNull>());
				Viewport->SetField(TEXT("height"), MakeShared<FJsonValueNull>());
			}
			const TCHAR* WindowMode = TEXT("Unknown");
			switch (ViewportClient->Viewport->GetWindowMode())
			{
			case EWindowMode::Fullscreen: WindowMode = TEXT("Fullscreen"); break;
			case EWindowMode::WindowedFullscreen: WindowMode = TEXT("WindowedFullscreen"); break;
			case EWindowMode::Windowed: WindowMode = TEXT("Windowed"); break;
			default: break;
			}
			Viewport->SetStringField(TEXT("windowMode"), WindowMode);
			Viewport->SetBoolField(TEXT("hasFocus"), ViewportClient->Viewport->HasFocus());
			Viewport->SetBoolField(TEXT("isForegroundWindow"), ViewportClient->Viewport->IsForegroundWindow());
		}
		else
		{
			Viewport->SetBoolField(TEXT("available"), false);
			Viewport->SetBoolField(TEXT("dimensionsAvailable"), false);
			for (const TCHAR* Field : { TEXT("width"), TEXT("height"), TEXT("windowMode"),
				TEXT("hasFocus"), TEXT("isForegroundWindow") })
			{
				Viewport->SetField(Field, MakeShared<FJsonValueNull>());
			}
		}
		Viewport->SetStringField(TEXT("windowTitlePolicy"), TEXT("Not collected: titles can contain personal names or paths."));
		Scenario->SetObjectField(TEXT("viewport"), Viewport);

		const TSharedRef<FJsonObject> CVars = MakeShared<FJsonObject>();
		const TCHAR* CVarNames[] =
		{
			TEXT("sg.ResolutionQuality"), TEXT("sg.ViewDistanceQuality"), TEXT("sg.AntiAliasingQuality"),
			TEXT("sg.ShadowQuality"), TEXT("sg.GlobalIlluminationQuality"), TEXT("sg.ReflectionQuality"),
			TEXT("sg.PostProcessQuality"), TEXT("sg.TextureQuality"), TEXT("sg.EffectsQuality"),
			TEXT("sg.FoliageQuality"), TEXT("sg.ShadingQuality"), TEXT("sg.LandscapeQuality"),
			TEXT("r.ScreenPercentage"), TEXT("r.SecondaryScreenPercentage.GameViewport"),
			TEXT("r.DynamicRes.OperationMode"), TEXT("r.DynamicRes.MinScreenPercentage"),
			TEXT("r.DynamicRes.MaxScreenPercentage"), TEXT("r.VSync"), TEXT("r.VSyncEditor"),
			TEXT("t.MaxFPS"), TEXT("t.IdleWhenNotForeground"), TEXT("r.FullScreenMode"),
			TEXT("r.AntiAliasingMethod"), TEXT("r.OneFrameThreadLag"), TEXT("r.DontLimitOnBattery"),
			TEXT("rhi.SyncInterval"), TEXT("r.GTSyncType"), TEXT("Slate.bAllowThrottling"),
			TEXT("r.TemporalAA.Upsampling"), TEXT("r.Nanite"), TEXT("r.RayTracing"),
			TEXT("r.Lumen.DiffuseIndirect.Allow"), TEXT("r.Lumen.Reflections.Allow")
		};
		for (const TCHAR* Name : CVarNames)
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			const double Value = Variable ? static_cast<double>(Variable->GetFloat()) : 0.0;
			if (Variable && FMath::IsFinite(Value))
			{
				CVars->SetNumberField(Name, Value);
			}
			else
			{
				CVars->SetField(Name, MakeShared<FJsonValueNull>());
			}
		}
		Scenario->SetObjectField(TEXT("qualityAndTimingCVars"), CVars);
		Scenario->SetStringField(TEXT("cvarNullMeaning"), TEXT("CVar absent or its numeric value is non-finite."));
		Scenario->SetStringField(TEXT("resolutionPolicy"),
			TEXT("Viewport dimensions and configured resolution CVars, not verified per-frame internal render resolution."));

		const FString& AdapterName = GRHIGlobals.GpuInfo.AdapterName;
		if (AdapterName.IsEmpty())
		{
			Scenario->SetField(TEXT("gpuName"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Scenario->SetStringField(TEXT("gpuName"), AdapterName);
		}
		Scenario->SetStringField(TEXT("gpuNameSource"), TEXT("GRHIGlobals.GpuInfo.AdapterName; null when unavailable."));
		return Scenario;
	}

	bool WriteDurableReport(const TSharedRef<FJsonObject>& Report, const FString& FileName,
		const FString& CaptureId, FString& OutError, bool bBenchmark = false)
	{
		FString Serialized;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		if (!FJsonSerializer::Serialize(Report, Writer))
		{
			OutError = TEXT("Could not serialize performance JSON.");
			return false;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Directory = bBenchmark ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Performance"))
			: FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling"), TEXT("DublinFlight"));
		if (!PlatformFile.CreateDirectoryTree(*Directory))
		{
			OutError = TEXT("Could not create performance report directory.");
			return false;
		}
		const FString FinalPath = FPaths::Combine(Directory, FileName);
		const FString TemporaryPath = FinalPath + TEXT(".") + CaptureId + TEXT(".tmp");
		if (PlatformFile.FileExists(*FinalPath) || PlatformFile.FileExists(*TemporaryPath))
		{
			OutError = TEXT("Report filename collision; existing files were not overwritten.");
			return false;
		}

		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*TemporaryPath));
		if (!Handle)
		{
			OutError = TEXT("Could not open the temporary performance report for writing.");
			return false;
		}
		const FTCHARToUTF8 Utf8(*Serialized);
		if (!Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()) || !Handle->Flush(true))
		{
			OutError = TEXT("Report write/full flush failed; any temporary file is incomplete and must not be treated as a result.");
			return false;
		}
		Handle.Reset();
		if (!PlatformFile.MoveFile(*FinalPath, *TemporaryPath))
		{
			OutError = TEXT("Report publication failed; the flushed temporary JSON remains beside the intended result for recovery.");
			return false;
		}
		return true;
	}

	UDublinFlightPerformanceSubsystem* GetCaptureSubsystem(UWorld* World)
	{
		if (!IsInGameThread())
		{
			UE_LOG(LogDublinFlightPerformance, Error, TEXT("Profile commands must run on the game thread."));
			return nullptr;
		}
		if (!World || (World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game))
		{
			UE_LOG(LogDublinFlightPerformance, Error,
				TEXT("Profile commands require an explicit PIE/game world. Use that world's console after starting PIE; editor-world fallback is not allowed."));
			return nullptr;
		}
		UDublinFlightPerformanceSubsystem* Subsystem = World->GetSubsystem<UDublinFlightPerformanceSubsystem>();
		if (!Subsystem)
		{
			UE_LOG(LogDublinFlightPerformance, Error, TEXT("Performance subsystem is unavailable in the supplied world."));
		}
		return Subsystem;
	}

	void StartCommand(const TArray<FString>& Args, UWorld* World)
	{
		UDublinFlightPerformanceSubsystem* Subsystem = GetCaptureSubsystem(World);
		if (!Subsystem)
		{
			return;
		}
		FOptions Options;
		FString Error;
		if (!ParseStartArguments(Args, Options, Error))
		{
			UE_LOG(LogDublinFlightPerformance, Error, TEXT("%s"), *Error);
			return;
		}
		Subsystem->StartCapture(Options);
	}

	void StopCommand(const TArray<FString>& Args, UWorld* World)
	{
		UDublinFlightPerformanceSubsystem* Subsystem = GetCaptureSubsystem(World);
		if (!Subsystem)
		{
			return;
		}
		if (!Args.IsEmpty())
		{
			UE_LOG(LogDublinFlightPerformance, Error, TEXT("Usage: DublinFlight.Profile.Stop (no arguments)"));
			return;
		}
		Subsystem->StopCapture();
	}

	FAutoConsoleCommandWithWorldAndArgs StartProfileCommand(
		TEXT("DublinFlight.Profile.Start"),
		TEXT("Start explicit wall-clock capture in the supplied PIE/game world: [label=capture] [durationSeconds=30] [warmupSeconds=5] [minFPS=60]. "
			"Duration 1..300, warmup 0..60; unsigned decimal seconds. minFPS must be 30 or 60 and affects reporting only, not FPS caps or gameplay. "
			"Labels: 1..64 ASCII letters/digits/_/-, starting alphanumeric."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StartCommand));

	FAutoConsoleCommandWithWorldAndArgs StopProfileCommand(
		TEXT("DublinFlight.Profile.Stop"),
		TEXT("Stop this world's active capture at the next engine-frame boundary, preserving the full in-flight frame."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StopCommand));

#if !UE_BUILD_SHIPPING
	FAutoConsoleCommandWithWorldAndArgs StartBenchmarkCommand(
		TEXT("DublinFlight.Benchmark.Start"),
		TEXT("normal|light|maximal [durationSeconds=30] [warmupSeconds=5]. Visible 1920x1080, fresh full 1044 READY city; "
			"maximal means downstream destruction stress. Waits up to 60 shared wall seconds for BeginPlay. "
			"Does not alter graphics/pacing. Reports Saved\\Performance. Use a fresh map for each run."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UDublinFlightPerformanceSubsystem* Subsystem = GetCaptureSubsystem(World)) { Subsystem->StartBenchmark(Args); }
		}));
	FAutoConsoleCommandWithWorldAndArgs StopBenchmarkCommand(
		TEXT("DublinFlight.Benchmark.Stop"),
		TEXT("Stop benchmark at the next shared frame boundary; manual stop is explicitly invalid."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!Args.IsEmpty()) { UE_LOG(LogDublinFlightPerformance, Error, TEXT("Benchmark.Stop takes no arguments.")); return; }
			if (UDublinFlightPerformanceSubsystem* Subsystem = GetCaptureSubsystem(World)) { Subsystem->StopBenchmark(); }
		}));
#endif
}

bool UDublinFlightPerformanceSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::PIE || WorldType == EWorldType::Game;
}

bool UDublinFlightPerformanceSubsystem::StartCapture(const DublinFlight::Performance::FOptions& Options)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("StartCapture requires the game thread."));
		return false;
	}
	UWorld* World = GetWorld();
	if (!World || !DoesSupportWorldType(World->WorldType) || !World->HasBegunPlay() || IsRunningCommandlet())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("StartCapture requires a begun-play PIE/game world, not a commandlet."));
		return false;
	}
	if (bCapturing || bBenchmarkWaiting)
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("This world already has an active capture; stop it before starting another."));
		return false;
	}
	FString Error;
	if (!CaptureWindow.Start(Options, Error))
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("%s"), *Error);
		return false;
	}

	StartScenario = ReadScenario(*World);
	StartedUtc = FDateTime::UtcNow().ToIso8601();
	CaptureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	PreviousEngineFrame.Reset();
	bStopRequested = false;
	bCapturing = true;
	BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddUObject(this, &UDublinFlightPerformanceSubsystem::OnBeginFrame);
	UE_LOG(LogDublinFlightPerformance, Display,
		TEXT("Profile '%s' armed: %.9g measured seconds after %.9g warmup seconds; scenario target %d FPS. First following engine boundary primes the clock; no partial starting frame is measured."),
		*Options.Label, Options.DurationSeconds, Options.WarmupSeconds, Options.ScenarioTargetMinimumFPS);
	return true;
}

bool UDublinFlightPerformanceSubsystem::StartBenchmark(const TArray<FString>& Args)
{
#if UE_BUILD_SHIPPING
	return false;
#else
	using namespace DublinFlight::Performance;
	if (!IsInGameThread())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("StartBenchmark requires the game thread."));
		return false;
	}
	if (bCapturing || bBenchmarkWaiting || Benchmark.IsValid())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("This world already has an active capture or benchmark."));
		return false;
	}
	FOptions Options;
	EBenchmark Kind;
	FString Error;
	if (!ParseBenchmarkArguments(Args, Kind, Options, Error))
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("%s"), *Error);
		return false;
	}
	if (!GetWorld() || !DoesSupportWorldType(GetWorld()->WorldType) || IsRunningCommandlet())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("StartBenchmark requires a PIE/game world, not a commandlet."));
		return false;
	}
	if (!CaptureWindow.Start(Options, Error))
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("Could not initialize benchmark capture: %s"), *Error);
		return false;
	}
	Benchmark = MakeShared<FDublinRuntimeBenchmark>(Kind, Options);
	BenchmarkWaitStart = GetSharedFrameBoundarySeconds();
	bBenchmarkWaiting = true;
	bStopRequested = false;
	StartScenario = ReadScenario(*GetWorld());
	StartedUtc = FDateTime::UtcNow().ToIso8601();
	CaptureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddUObject(this, &UDublinFlightPerformanceSubsystem::AwaitBenchmarkWorld);
	UE_LOG(LogDublinFlightPerformance, Display, TEXT("Benchmark %s queued; bounded 60-wall-second BeginPlay wait. Reports: Saved\\Performance."),
		*Options.Label);
	return true;
#endif
}

void UDublinFlightPerformanceSubsystem::AwaitBenchmarkWorld()
{
	if (!bBenchmarkWaiting || !Benchmark.IsValid()) { return; }
	if (bStopRequested) { FinishCapture(TEXT("manualStop")); return; }
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay())
	{
		if (GetSharedFrameBoundarySeconds() - BenchmarkWaitStart >= 60)
		{
			FinishCapture(TEXT("notReady"), TEXT("BeginPlay did not become ready within 60 shared wall seconds."));
		}
		return;
	}
	FString Error;
	if (!Benchmark->Prepare(*World, Error))
	{
		FinishCapture(TEXT("notReady"), Error);
		return;
	}
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	BeginFrameHandle.Reset();
	bBenchmarkWaiting = false;
	if (!StartCapture(Benchmark->GetOptions())) { FinishCapture(TEXT("failed"), TEXT("Could not start shared benchmark capture.")); }
}

bool UDublinFlightPerformanceSubsystem::StopBenchmark()
{
	if (!IsInGameThread() || !Benchmark.IsValid())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("StopBenchmark requires an active benchmark on the game thread."));
		return false;
	}
	if (bBenchmarkWaiting) { bStopRequested = true; return true; }
	return StopCapture();
}

bool UDublinFlightPerformanceSubsystem::StopCapture()
{
	if (!IsInGameThread())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("StopCapture requires the game thread."));
		return false;
	}
	if (!bCapturing)
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("No active performance capture exists in this world."));
		return false;
	}
	if (bStopRequested)
	{
		UE_LOG(LogDublinFlightPerformance, Warning, TEXT("Stop already requested; waiting for the next engine-frame boundary."));
		return false;
	}
	bStopRequested = true;
	UE_LOG(LogDublinFlightPerformance, Display, TEXT("Profile stop requested; the current full frame will be included if it is after warmup."));
	return true;
}

void UDublinFlightPerformanceSubsystem::OnBeginFrame()
{
	if (!bCapturing)
	{
		return;
	}
	if (PreviousEngineFrame.IsSet() && GFrameCounter != PreviousEngineFrame.GetValue() + 1)
	{
		FinishCapture(TEXT("failed"), TEXT("Engine frame sequence is not contiguous; refusing to combine or skip frames."));
		return;
	}
	PreviousEngineFrame = GFrameCounter;
	FString Error;
	const double Boundary = GetSharedFrameBoundarySeconds();
	const DublinFlight::Performance::EBoundaryResult Result = CaptureWindow.AddBoundary(Boundary, Error);
	if (Benchmark.IsValid() && GetWorld())
	{
		Benchmark->Boundary(*GetWorld(), Boundary, CaptureWindow,
			Result == DublinFlight::Performance::EBoundaryResult::Complete
				|| Result == DublinFlight::Performance::EBoundaryResult::Error || bStopRequested, ReadScenario(*GetWorld()));
	}
	if (Result == DublinFlight::Performance::EBoundaryResult::Error)
	{
		FinishCapture(TEXT("failed"), Error);
	}
	else if (bStopRequested && Benchmark.IsValid())
	{
		FinishCapture(TEXT("manualStop"));
	}
	else if (Result == DublinFlight::Performance::EBoundaryResult::Complete)
	{
		FinishCapture(TEXT("durationReached"));
	}
	else if (bStopRequested)
	{
		FinishCapture(TEXT("manualStop"));
	}
}

void UDublinFlightPerformanceSubsystem::FinishCapture(const TCHAR* CompletionReason, const FString& Failure)
{
	using namespace DublinFlight::Performance;
	bCapturing = false;
	bBenchmarkWaiting = false;
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	BeginFrameHandle.Reset();

	const bool bWorldEnding = FCString::Strcmp(CompletionReason, TEXT("worldEnded")) == 0
		|| FCString::Strcmp(CompletionReason, TEXT("worldDeinitialized")) == 0;
	const bool bDurationReached = FCString::Strcmp(CompletionReason, TEXT("durationReached")) == 0;
	const FFrameAccumulator& Accumulator = CaptureWindow.GetAccumulator();
	FString MetricsError;
	const TOptional<FMetrics> Metrics = Failure.IsEmpty() ? Accumulator.Calculate(MetricsError) : TOptional<FMetrics>();
	const FDateTime CompletedUtc = FDateTime::UtcNow();
	const FOptions& Options = CaptureWindow.GetOptions();
	const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	const TCHAR* Status = !Failure.IsEmpty() ? TEXT("failed")
		: bWorldEnding ? TEXT("interrupted")
		: !Metrics.IsSet() ? TEXT("noSamples")
		: bDurationReached ? TEXT("completed") : TEXT("stopped");

	Report->SetNumberField(TEXT("schemaVersion"), 1);
	Report->SetStringField(TEXT("captureId"), CaptureId);
	Report->SetStringField(TEXT("label"), Options.Label);
	Report->SetStringField(TEXT("status"), Status);
	Report->SetStringField(TEXT("completionReason"), CompletionReason);
	Report->SetBoolField(TEXT("completedRequestedDuration"), bDurationReached && Metrics.IsSet());
	Report->SetBoolField(TEXT("metricsAvailable"), Metrics.IsSet());
	Report->SetStringField(TEXT("startedUtc"), StartedUtc);
	Report->SetStringField(TEXT("finishedUtc"), CompletedUtc.ToIso8601());
	Report->SetStringField(TEXT("clockMethod"),
		TEXT("FPlatformTime::Seconds sampled once per GFrameCounter at FCoreDelegates::OnBeginFrame; consecutive unsmoothed shared engine-frame wall-clock deltas."));
	Report->SetStringField(TEXT("measurementScope"),
		TEXT("Engine game-thread frame cadence, not presentation intervals or isolated world/CPU/GPU execution time. "
			"Includes pacing sleeps, stalls, GC, focus/background throttling, paused-world and PIE/editor overhead after warmup."));
	Report->SetStringField(TEXT("scenarioLimitation"),
		TEXT("Results apply only to the recorded scenario and measured frames, not other routes, future frames, machines or a general minimum-FPS guarantee."));
	Report->SetStringField(TEXT("settingsSampling"),
		TEXT("Read-only start/end snapshots; transient setting, viewport or focus changes between snapshots are not tracked."));
	Report->SetStringField(TEXT("gpuAttribution"),
		TEXT("Unverified: no GPU timing was collected. Wall-frame periods and GPU name do not establish a GPU bottleneck."));
	Report->SetField(TEXT("gpuFrameTimeMilliseconds"), MakeShared<FJsonValueNull>());
	Report->SetField(TEXT("gpuBound"), MakeShared<FJsonValueNull>());
	Report->SetNumberField(TEXT("requestedDurationSeconds"), Options.DurationSeconds);
	Report->SetNumberField(TEXT("requestedWarmupSeconds"), Options.WarmupSeconds);
	Report->SetNumberField(TEXT("scenarioTargetMinimumFPS"), Options.ScenarioTargetMinimumFPS);
	Report->SetStringField(TEXT("scenarioTargetPolicy"),
		TEXT("scenarioTargetMet selects allFramesAtLeast30 or allFramesAtLeast60 for the explicit target using the disclosed numerical tolerance, never average FPS. "
			"It describes only measured frames; it does not imply completed duration, presentation FPS, or unmeasured workloads. Target selection changes no gameplay or timing settings."));
	Report->SetNumberField(TEXT("excludedWarmupFrames"), static_cast<double>(CaptureWindow.GetExcludedWarmupFrames()));
	Report->SetNumberField(TEXT("excludedWarmupSeconds"), CaptureWindow.GetExcludedWarmupSeconds());
	Report->SetBoolField(TEXT("clockPrimed"), CaptureWindow.HasBoundary());
	Report->SetStringField(TEXT("boundaryPolicy"),
		TEXT("Prime at the first boundary after Start. Exclude every whole frame whose start is before the warmup deadline, "
			"including the warmup-crossing frame. Record every later frame, without filtering or clipping, "
			"through the first boundary reaching the requested measured duration or the next boundary after Stop. "
			"World teardown interrupts immediately; its unfinished final interval is not a complete frame."));
	Report->SetNumberField(TEXT("maxCapturedFrames"), MaxCapturedFrames);
	Report->SetNumberField(TEXT("frameCount"), Accumulator.Num());
	Report->SetNumberField(TEXT("frameBudgetMilliseconds"), FrameBudgetMilliseconds);
	Report->SetNumberField(TEXT("frameBudget30Milliseconds"), FrameBudget30Milliseconds);
	Report->SetNumberField(TEXT("thresholdToleranceMilliseconds"), ThresholdToleranceMilliseconds);
	Report->SetStringField(TEXT("thresholdPolicy"),
		TEXT("framesOver60FPSBudget uses strict seconds > 1/60. allFramesAtLeast60 allows only 0.000001 ms (1 ns) numerical tolerance; no frame times are rounded or adjusted. "
			"framesOver30FPSBudget uses strict seconds > 1/30; its WithTolerance count and allFramesAtLeast30 use the same 1 ns tolerance. "
			"Both budgets are evaluated independently on the same trace, regardless of scenario target."));
	Report->SetStringField(TEXT("quantileMethod"), TEXT("Sorted linear interpolation: index=(N-1)*q (type 7), q=0.50,0.95,0.99."));

	const FEngineVersion& Version = FEngineVersion::Current();
	Report->SetStringField(TEXT("engineVersion"), FString::Printf(TEXT("%u.%u.%u"),
		static_cast<uint32>(Version.GetMajor()), static_cast<uint32>(Version.GetMinor()), static_cast<uint32>(Version.GetPatch())));
	Report->SetNumberField(TEXT("engineChangelist"), Version.GetChangelist());
	Report->SetObjectField(TEXT("startScenario"), StartScenario.ToSharedRef());
	if (!bWorldEnding && GetWorld())
	{
		Report->SetObjectField(TEXT("endScenario"), ReadScenario(*GetWorld()));
	}
	else
	{
		Report->SetField(TEXT("endScenario"), MakeShared<FJsonValueNull>());
	}
	Report->SetStringField(TEXT("privacyPolicy"),
		TEXT("No machine/user identity, command line, window title, engine branch or project/asset absolute path is collected. Use a non-personal scenario label."));

	if (Metrics.IsSet())
	{
		const FMetrics& Value = Metrics.GetValue();
		Report->SetNumberField(TEXT("durationSeconds"), Value.DurationSeconds);
		Report->SetNumberField(TEXT("durationOvershootSeconds"), FMath::Max(0.0, Value.DurationSeconds - Options.DurationSeconds));
		Report->SetNumberField(TEXT("minFPS"), Value.MinFPS);
		Report->SetNumberField(TEXT("averageFPS"), Value.AverageFPS);
		Report->SetStringField(TEXT("fpsMethod"), TEXT("minFPS=1/max(frameSeconds); averageFPS=frameCount/sum(frameSeconds), not the mean of per-frame FPS."));
		Report->SetNumberField(TEXT("worstFrameMilliseconds"), Value.WorstFrameMilliseconds);
		Report->SetNumberField(TEXT("p50FrameMilliseconds"), Value.P50FrameMilliseconds);
		Report->SetNumberField(TEXT("p95FrameMilliseconds"), Value.P95FrameMilliseconds);
		Report->SetNumberField(TEXT("p99FrameMilliseconds"), Value.P99FrameMilliseconds);
		Report->SetNumberField(TEXT("framesOver60FPSBudget"), Value.FramesOverBudget);
		Report->SetNumberField(TEXT("framesOver60FPSBudgetWithTolerance"), Value.FramesOverBudgetWithTolerance);
		Report->SetBoolField(TEXT("allFramesAtLeast60"), Value.bAllFramesAtLeast60);
		Report->SetNumberField(TEXT("framesOver30FPSBudget"), Value.FramesOver30FPSBudget);
		Report->SetNumberField(TEXT("framesOver30FPSBudgetWithTolerance"), Value.FramesOver30FPSBudgetWithTolerance);
		Report->SetBoolField(TEXT("allFramesAtLeast30"), Value.bAllFramesAtLeast30);
		Report->SetBoolField(TEXT("scenarioTargetMet"), Value.MeetsTargetMinimumFPS(Options.ScenarioTargetMinimumFPS));
	}
	else
	{
		for (const TCHAR* Field : { TEXT("durationSeconds"), TEXT("durationOvershootSeconds"), TEXT("minFPS"),
			TEXT("averageFPS"), TEXT("worstFrameMilliseconds"), TEXT("p50FrameMilliseconds"),
			TEXT("p95FrameMilliseconds"), TEXT("p99FrameMilliseconds"), TEXT("framesOver60FPSBudget"),
			TEXT("framesOver60FPSBudgetWithTolerance"), TEXT("allFramesAtLeast60"),
			TEXT("framesOver30FPSBudget"), TEXT("framesOver30FPSBudgetWithTolerance"),
			TEXT("allFramesAtLeast30"), TEXT("scenarioTargetMet") })
		{
			Report->SetField(Field, MakeShared<FJsonValueNull>());
		}
	}
	if (!Failure.IsEmpty() || !MetricsError.IsEmpty())
	{
		Report->SetStringField(TEXT("error"), Failure.IsEmpty() ? MetricsError : Failure);
	}
	else
	{
		Report->SetField(TEXT("error"), MakeShared<FJsonValueNull>());
	}

	TArray<TSharedPtr<FJsonValue>> FrameTimes;
	FrameTimes.Reserve(Accumulator.Num());
	for (double Seconds : Accumulator.GetSamplesSeconds())
	{
		FrameTimes.Add(MakeShared<FJsonValueNumber>(Seconds));
	}
	Report->SetArrayField(TEXT("frameTimesSeconds"), FrameTimes);

	const FString FileName = FString::Printf(TEXT("%s-%s-%lld.json"), *Options.Label,
		*CompletedUtc.ToString(TEXT("%Y%m%dT%H%M%SZ")), static_cast<long long>(CompletedUtc.GetTicks()));
	if (Benchmark.IsValid())
	{
		if (!Failure.IsEmpty()) { Benchmark->Invalidate(Failure); }
		Benchmark->Finish(CompletionReason);
		const bool bValid = Benchmark->IsValid();
		const bool bAdmitted = Benchmark->WorkloadAdmitted();
		const bool bFrameTarget = Metrics.IsSet() && Metrics->MeetsTargetMinimumFPS(Options.ScenarioTargetMinimumFPS);
		Report->SetBoolField(TEXT("benchmarkValid"), bValid);
		Report->SetBoolField(TEXT("workloadAdmitted"), bAdmitted);
		Report->SetBoolField(TEXT("frameTargetMet"), bFrameTarget);
		Report->SetBoolField(TEXT("scenarioTargetMet"), bFrameTarget && bValid && bAdmitted);
		Report->SetBoolField(TEXT("benchmarkPassed"), BenchmarkPassed(bDurationReached, bValid, bAdmitted, bFrameTarget));
		if (!bValid || !bAdmitted) { Report->SetStringField(TEXT("status"), TEXT("invalid")); }
		Report->SetStringField(TEXT("settingsSampling"), TEXT("Benchmark environment sampled at every measured shared boundary; all frames retained on invalidity."));
		Report->SetObjectField(TEXT("benchmark"), Benchmark->Report());
		const FString ManifestName = FPaths::GetBaseFilename(FileName) + TEXT("-manifest.json");
		const TSharedRef<FJsonObject> Manifest = Benchmark->Report();
		Manifest->SetStringField(TEXT("captureId"), CaptureId);
		Manifest->SetStringField(TEXT("profileFile"), FileName);
		Manifest->SetStringField(TEXT("status"), Report->GetStringField(TEXT("status")));
		Manifest->SetBoolField(TEXT("completedRequestedDuration"), bDurationReached && Metrics.IsSet());
		Manifest->SetBoolField(TEXT("scenarioTargetMet"), bFrameTarget && bValid && bAdmitted);
		if (Metrics.IsSet())
		{
			Manifest->SetNumberField(TEXT("minFPS"), Metrics->MinFPS);
			Manifest->SetNumberField(TEXT("averageFPS"), Metrics->AverageFPS);
			Manifest->SetNumberField(TEXT("worstFrameMilliseconds"), Metrics->WorstFrameMilliseconds);
			Manifest->SetNumberField(TEXT("p95FrameMilliseconds"), Metrics->P95FrameMilliseconds);
			Manifest->SetNumberField(TEXT("p99FrameMilliseconds"), Metrics->P99FrameMilliseconds);
			Manifest->SetNumberField(TEXT("framesOver60FPSBudget"), Metrics->FramesOverBudget);
			Manifest->SetNumberField(TEXT("framesOver30FPSBudget"), Metrics->FramesOver30FPSBudget);
			Manifest->SetNumberField(TEXT("frameCount"), Metrics->FrameCount);
		}
		FString ManifestError;
		if (!WriteDurableReport(Manifest, ManifestName, CaptureId, ManifestError, true))
		{
			Report->SetStringField(TEXT("status"), TEXT("failed"));
			Report->SetBoolField(TEXT("benchmarkPassed"), false);
			Report->SetBoolField(TEXT("scenarioTargetMet"), false);
			Report->SetStringField(TEXT("manifestError"), ManifestError);
			UE_LOG(LogDublinFlightPerformance, Error, TEXT("Benchmark manifest NOT published: %s"), *ManifestError);
		}
		else
		{
			UE_LOG(LogDublinFlightPerformance, Display, TEXT("Benchmark status=%s completedRequestedDuration=%s scenarioTargetMet=%s profile=Saved\\Performance\\%s manifest=Saved\\Performance\\%s"),
				*Report->GetStringField(TEXT("status")), bDurationReached ? TEXT("true") : TEXT("false"),
				Report->GetBoolField(TEXT("scenarioTargetMet")) ? TEXT("true") : TEXT("false"), *FileName, *ManifestName);
		}
	}
	FString WriteError;
	if (!WriteDurableReport(Report, FileName, CaptureId, WriteError, Benchmark.IsValid()))
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("Profile '%s' was NOT published: %s Intended filename: %s; temporary suffix: .%s.tmp"),
			*Options.Label, *WriteError, *FileName, *CaptureId);
	}
	else if (Benchmark.IsValid())
	{
		UE_LOG(LogDublinFlightPerformance, Display, TEXT("Benchmark profile published: status=%s, Saved\\Performance\\%s"),
			*Report->GetStringField(TEXT("status")), *FileName);
	}
	else if (!Failure.IsEmpty())
	{
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("Profile FAILED: %s Failure report: Saved\\Profiling\\DublinFlight\\%s"),
			*Failure, *FileName);
	}
	else if (!Metrics.IsSet())
	{
		UE_LOG(LogDublinFlightPerformance, Warning, TEXT("Profile has no measured frames: %s Report: Saved\\Profiling\\DublinFlight\\%s"),
			*MetricsError, *FileName);
	}
	else
	{
		const FMetrics& Value = Metrics.GetValue();
		UE_LOG(LogDublinFlightPerformance, Display,
			TEXT("Profile %s: minFPS=%.17g (from worstFrameMs=%.17g), averageFPS=%.17g, frames=%d, durationSeconds=%.17g, framesOver60FPSBudget=%d, allFramesAtLeast60=%s, framesOver30FPSBudget=%d, allFramesAtLeast30=%s, scenarioTargetMinimumFPS=%d, scenarioTargetMet=%s (1e-6 ms tolerance). Saved\\Profiling\\DublinFlight\\%s"),
			Status, Value.MinFPS, Value.WorstFrameMilliseconds, Value.AverageFPS, Value.FrameCount,
			Value.DurationSeconds, Value.FramesOverBudget, Value.bAllFramesAtLeast60 ? TEXT("true") : TEXT("false"),
			Value.FramesOver30FPSBudget, Value.bAllFramesAtLeast30 ? TEXT("true") : TEXT("false"),
			Options.ScenarioTargetMinimumFPS, Value.MeetsTargetMinimumFPS(Options.ScenarioTargetMinimumFPS) ? TEXT("true") : TEXT("false"), *FileName);
	}
	StartScenario.Reset();
	Benchmark.Reset();
}

void UDublinFlightPerformanceSubsystem::OnWorldEndPlay(UWorld& InWorld)
{
	if (bCapturing || bBenchmarkWaiting)
	{
		FinishCapture(TEXT("worldEnded"));
	}
	Super::OnWorldEndPlay(InWorld);
}

void UDublinFlightPerformanceSubsystem::Deinitialize()
{
	if (bCapturing || bBenchmarkWaiting)
	{
		FinishCapture(TEXT("worldDeinitialized"));
	}
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	BeginFrameHandle.Reset();
	Super::Deinitialize();
}
