#pragma once

#include "CoreMinimal.h"
#include "DublinFlightPerformanceMetrics.h"
#include "Subsystems/WorldSubsystem.h"
#include "DublinFlightPerformanceSubsystem.generated.h"

class FJsonObject;
namespace DublinFlight::Performance { class FDublinRuntimeBenchmark; }

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

	virtual void Deinitialize() override;
	virtual void OnWorldEndPlay(UWorld& InWorld) override;

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	void OnBeginFrame();
	void FinishCapture(const TCHAR* CompletionReason, const FString& Failure = FString());
	void AwaitBenchmarkWorld();

	DublinFlight::Performance::FCaptureWindow CaptureWindow;
	FDelegateHandle BeginFrameHandle;
	TSharedPtr<FJsonObject> StartScenario;
	FString StartedUtc;
	FString CaptureId;
	TOptional<uint64> PreviousEngineFrame;
	bool bCapturing = false;
	bool bStopRequested = false;
	bool bBenchmarkWaiting = false;
	double BenchmarkWaitStart = 0;
	TSharedPtr<DublinFlight::Performance::FDublinRuntimeBenchmark> Benchmark;
};
