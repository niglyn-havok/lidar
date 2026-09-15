#pragma once

#include "CoreMinimal.h"

namespace DublinFlight::Performance
{
	inline constexpr int32 MaxCapturedFrames = 120000;
	inline constexpr int32 MaxLabelLength = 64;
	inline constexpr double FrameBudgetSeconds = 1.0 / 60.0;
	inline constexpr double FrameBudgetMilliseconds = 1000.0 / 60.0;
	inline constexpr double FrameBudget30Seconds = 1.0 / 30.0;
	inline constexpr double FrameBudget30Milliseconds = 1000.0 / 30.0;
	inline constexpr double ThresholdToleranceMilliseconds = 0.000001;

	struct FOptions
	{
		FString Label = TEXT("capture");
		double DurationSeconds = 30.0;
		double WarmupSeconds = 5.0;
		int32 ScenarioTargetMinimumFPS = 60;
	};

	struct FMetrics
	{
		int32 FrameCount = 0;
		double DurationSeconds = 0.0;
		double MinFPS = 0.0;
		double AverageFPS = 0.0;
		double WorstFrameMilliseconds = 0.0;
		double P50FrameMilliseconds = 0.0;
		double P95FrameMilliseconds = 0.0;
		double P99FrameMilliseconds = 0.0;
		int32 FramesOverBudget = 0;
		int32 FramesOverBudgetWithTolerance = 0;
		bool bAllFramesAtLeast60 = false;
		int32 FramesOver30FPSBudget = 0;
		int32 FramesOver30FPSBudgetWithTolerance = 0;
		bool bAllFramesAtLeast30 = false;

		bool MeetsTargetMinimumFPS(int32 TargetMinimumFPS) const
		{
			return FrameCount > 0 && ((TargetMinimumFPS == 30 && bAllFramesAtLeast30)
				|| (TargetMinimumFPS == 60 && bAllFramesAtLeast60));
		}
	};

	DUBLINFLIGHT_API bool ValidateLabel(const FString& Label, FString& OutError);
	DUBLINFLIGHT_API bool ValidateOptions(const FOptions& Options, FString& OutError);
	DUBLINFLIGHT_API bool ParseStartArguments(const TArray<FString>& Args, FOptions& OutOptions, FString& OutError);

	class DUBLINFLIGHT_API FFrameAccumulator
	{
	public:
		void Reset();
		bool AddSample(double ElapsedSeconds, FString& OutError);
		TOptional<FMetrics> Calculate(FString& OutError) const;

		const TArray<double>& GetSamplesSeconds() const { return SamplesSeconds; }
		int32 Num() const { return SamplesSeconds.Num(); }
		double GetDurationSeconds() const { return TotalSeconds; }

	private:
		TArray<double> SamplesSeconds;
		double TotalSeconds = 0.0;
		double SumCorrection = 0.0;
	};

	enum class EBoundaryResult : uint8
	{
		Primed,
		Warmup,
		Recorded,
		Complete,
		Error
	};

	// Pure boundary/window logic: only full intervals after warmup enter the accumulator.
	class DUBLINFLIGHT_API FCaptureWindow
	{
	public:
		bool Start(const FOptions& InOptions, FString& OutError);
		EBoundaryResult AddBoundary(double BoundarySeconds, FString& OutError);

		const FOptions& GetOptions() const { return Options; }
		const FFrameAccumulator& GetAccumulator() const { return Accumulator; }
		uint64 GetExcludedWarmupFrames() const { return ExcludedWarmupFrames; }
		double GetExcludedWarmupSeconds() const { return ExcludedWarmupSeconds; }
		bool HasBoundary() const { return PreviousBoundary.IsSet(); }
		bool IsMeasurementBoundary() const
		{
			return PreviousBoundary.IsSet() && PreviousBoundary.GetValue() - FirstBoundarySeconds >= Options.WarmupSeconds;
		}

	private:
		FOptions Options;
		FFrameAccumulator Accumulator;
		TOptional<double> PreviousBoundary;
		double FirstBoundarySeconds = 0.0;
		double ExcludedWarmupSeconds = 0.0;
		uint64 ExcludedWarmupFrames = 0;
		bool bStarted = false;
		bool bFinished = false;
	};
}
