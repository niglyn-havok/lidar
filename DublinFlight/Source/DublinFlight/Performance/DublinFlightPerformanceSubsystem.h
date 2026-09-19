#pragma once

#include "CoreMinimal.h"
#include "DublinFlightPerformanceMetrics.h"
#include "Subsystems/WorldSubsystem.h"
#include "DublinFlightPerformanceSubsystem.generated.h"

class FJsonObject;
namespace DublinFlight::Performance
{
	class FDublinRuntimeBenchmark;
	class FPSODiagnosticCapture;

	struct FStartupBenchmarkRequest
	{
		bool bRequested = false;
		FString Scenario;
		FString RunId;
		FString OutputDirectory;
		TArray<FString> Arguments;
	};

	DUBLINFLIGHT_API bool ParseStartupBenchmarkRequest(const FString& CommandLine,
		FStartupBenchmarkRequest& OutRequest, FString& OutError);
	DUBLINFLIGHT_API bool PrepareStartupBenchmarkDirectory(const FString& Directory,
		FString& OutDirectory, FString& OutError);
	DUBLINFLIGHT_API bool IsSafeBenchmarkReportName(const FString& Name);
	DUBLINFLIGHT_API bool TryClaimStartupBenchmarkRequest(bool bEligibleWorld, bool bHasBegunPlay,
		bool bWorldTearingDown, bool& bProcessClaimed);
	DUBLINFLIGHT_API uint8 StartupBenchmarkOutcomeCode(bool bCaptureStarted, bool bReportsPublished, bool bPassed);
	DUBLINFLIGHT_API bool PublishStartupBenchmarkResult(const FStartupBenchmarkRequest& Request,
		const FString& CaptureId, const FString& ProfileFile, const FString& ManifestFile,
		uint8 OutcomeCode, const FString& ErrorCode, const TArray<FString>& Errors, FString& OutError);
}

// OnBeginFrame supplies engine-frame wall periods even when the world is paused or time-dilated.
UCLASS()
class DUBLINFLIGHT_API UDublinFlightPerformanceSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	bool StartCapture(const DublinFlight::Performance::FOptions& Options);
	bool StopCapture();
	bool IsCapturing() const { return bCapturing; }
	bool StartBenchmark(const TArray<FString>& Args);
	bool StopBenchmark();

	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void OnWorldEndPlay(UWorld& InWorld) override;

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	void OnBeginFrame();
	void FinishCapture(const TCHAR* CompletionReason, const FString& Failure = FString());
	void AwaitBenchmarkWorld();
	void TryStartStartupBenchmark();
	void CancelPendingStartupBenchmark();
	bool QueueBenchmark(const TArray<FString>& Args, FString& OutError);
	void CompleteStartupBenchmark(uint8 OutcomeCode, const FString& ErrorCode, const TArray<FString>& Errors,
		const FString& ProfileFile = FString(), const FString& ManifestFile = FString());

	DublinFlight::Performance::FCaptureWindow CaptureWindow;
	FDelegateHandle BeginFrameHandle;
	FDelegateHandle StartupFrameHandle;
	TSharedPtr<FJsonObject> StartScenario;
	TSharedPtr<FJsonObject> StartForegroundDiagnostics;
	TSharedPtr<DublinFlight::Performance::FPSODiagnosticCapture> PSODiagnostics;
	FString StartedUtc;
	FString CaptureId;
	TOptional<uint64> PreviousEngineFrame;
	bool bCapturing = false;
	bool bStopRequested = false;
	bool bBenchmarkWaiting = false;
	double BenchmarkWaitStart = 0;
	TSharedPtr<DublinFlight::Performance::FDublinRuntimeBenchmark> Benchmark;
	TOptional<DublinFlight::Performance::FStartupBenchmarkRequest> StartupRequest;
	bool bStartupOutputReady = false;
	bool bStartupCaptureStarted = false;
	bool bStartupTerminalAttempted = false;
};
