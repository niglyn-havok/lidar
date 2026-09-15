#include "DublinFlightPerformanceMetrics.h"

#include "String/LexFromString.h"

namespace DublinFlight::Performance
{
	namespace
	{
		bool IsAsciiDigit(TCHAR Character)
		{
			return Character >= TEXT('0') && Character <= TEXT('9');
		}

		bool IsAsciiAlphaNumeric(TCHAR Character)
		{
			return IsAsciiDigit(Character)
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('a') && Character <= TEXT('z'));
		}

		bool ParseSeconds(const FString& Text, double& OutSeconds)
		{
			if (Text.IsEmpty() || Text.Len() > 32)
			{
				return false;
			}

			bool bHasDigit = false;
			bool bHasDecimalPoint = false;
			for (TCHAR Character : Text)
			{
				if (IsAsciiDigit(Character))
				{
					bHasDigit = true;
				}
				else if (Character == TEXT('.') && !bHasDecimalPoint)
				{
					bHasDecimalPoint = true;
				}
				else
				{
					return false;
				}
			}

			if (!bHasDigit)
			{
				return false;
			}

			LexFromString(OutSeconds, FStringView(Text));
			return FMath::IsFinite(OutSeconds);
		}

		double QuantileMilliseconds(const TArray<double>& SortedSeconds, double Quantile)
		{
			const double Position = (SortedSeconds.Num() - 1) * Quantile;
			const int32 Lower = FMath::FloorToInt(Position);
			const int32 Upper = FMath::Min(Lower + 1, SortedSeconds.Num() - 1);
			return (SortedSeconds[Lower]
				+ (SortedSeconds[Upper] - SortedSeconds[Lower]) * (Position - Lower)) * 1000.0;
		}
	}

	bool ValidateLabel(const FString& Label, FString& OutError)
	{
		OutError.Reset();
		if (Label.IsEmpty() || Label.Len() > MaxLabelLength || !IsAsciiAlphaNumeric(Label[0]))
		{
			OutError = TEXT("Label must be 1..64 ASCII characters, starting with a letter or digit.");
			return false;
		}

		for (TCHAR Character : Label)
		{
			if (!IsAsciiAlphaNumeric(Character) && Character != TEXT('_') && Character != TEXT('-'))
			{
				OutError = TEXT("Label permits ASCII letters, digits, '_' and '-' only; no paths, spaces or control characters.");
				return false;
			}
		}

		const FString Upper = Label.ToUpper();
		const bool bNumberedDevice = Upper.Len() == 4
			&& (Upper.StartsWith(TEXT("COM")) || Upper.StartsWith(TEXT("LPT")))
			&& Upper[3] >= TEXT('1') && Upper[3] <= TEXT('9');
		if (Upper == TEXT("CON") || Upper == TEXT("PRN") || Upper == TEXT("AUX")
			|| Upper == TEXT("NUL") || bNumberedDevice)
		{
			OutError = TEXT("Label cannot be a reserved Windows device name.");
			return false;
		}

		return true;
	}

	bool ValidateOptions(const FOptions& Options, FString& OutError)
	{
		if (!ValidateLabel(Options.Label, OutError))
		{
			return false;
		}
		if (!FMath::IsFinite(Options.DurationSeconds)
			|| Options.DurationSeconds < 1.0 || Options.DurationSeconds > 300.0)
		{
			OutError = TEXT("Duration must be finite and within 1..300 seconds inclusive.");
			return false;
		}
		if (!FMath::IsFinite(Options.WarmupSeconds)
			|| Options.WarmupSeconds < 0.0 || Options.WarmupSeconds > 60.0)
		{
			OutError = TEXT("Warmup must be finite and within 0..60 seconds inclusive.");
			return false;
		}
		if (Options.ScenarioTargetMinimumFPS != 30 && Options.ScenarioTargetMinimumFPS != 60)
		{
			OutError = TEXT("Minimum FPS must be exactly 30 or 60.");
			return false;
		}
		return true;
	}

	bool ParseStartArguments(const TArray<FString>& Args, FOptions& OutOptions, FString& OutError)
	{
		OutError.Reset();
		if (Args.Num() > 4)
		{
			OutError = TEXT("Usage: DublinFlight.Profile.Start [label] [durationSeconds] [warmupSeconds] [minFPS=60]");
			return false;
		}

		FOptions Parsed;
		if (Args.Num() >= 1)
		{
			Parsed.Label = Args[0];
		}
		if (Args.Num() >= 2 && !ParseSeconds(Args[1], Parsed.DurationSeconds))
		{
			OutError = TEXT("Duration must be a non-negative decimal number (no exponent, sign or trailing text).");
			return false;
		}
		if (Args.Num() >= 3 && !ParseSeconds(Args[2], Parsed.WarmupSeconds))
		{
			OutError = TEXT("Warmup must be a non-negative decimal number (no exponent, sign or trailing text).");
			return false;
		}
		if (Args.Num() >= 4)
		{
			const FString& Target = Args[3];
			if (Target.Len() != 2 || Target[1] != TEXT('0')
				|| (Target[0] != TEXT('3') && Target[0] != TEXT('6')))
			{
				OutError = TEXT("Minimum FPS must be exactly 30 or 60.");
				return false;
			}
			Parsed.ScenarioTargetMinimumFPS = Target[0] == TEXT('3') ? 30 : 60;
		}
		if (!ValidateOptions(Parsed, OutError))
		{
			return false;
		}
		OutOptions = MoveTemp(Parsed);
		return true;
	}

	void FFrameAccumulator::Reset()
	{
		SamplesSeconds.Reset();
		SamplesSeconds.Reserve(MaxCapturedFrames);
		TotalSeconds = 0.0;
		SumCorrection = 0.0;
	}

	bool FFrameAccumulator::AddSample(double ElapsedSeconds, FString& OutError)
	{
		OutError.Reset();
		if (!FMath::IsFinite(ElapsedSeconds) || ElapsedSeconds <= 0.0
			|| !FMath::IsFinite(ElapsedSeconds * 1000.0) || !FMath::IsFinite(1.0 / ElapsedSeconds))
		{
			OutError = TEXT("Rejected frame period: seconds must be positive and finite with representable milliseconds and FPS.");
			return false;
		}
		if (SamplesSeconds.Num() >= MaxCapturedFrames)
		{
			OutError = TEXT("Capture failed: 120000-frame memory cap reached; no frames were dropped or replaced.");
			return false;
		}

		const double CorrectedSample = ElapsedSeconds - SumCorrection;
		const double NewTotal = TotalSeconds + CorrectedSample;
		if (!FMath::IsFinite(NewTotal) || !FMath::IsFinite((SamplesSeconds.Num() + 1) / NewTotal))
		{
			OutError = TEXT("Rejected frame period: accumulated duration or average FPS is not finite.");
			return false;
		}

		SumCorrection = (NewTotal - TotalSeconds) - CorrectedSample;
		TotalSeconds = NewTotal;
		SamplesSeconds.Add(ElapsedSeconds);
		return true;
	}

	TOptional<FMetrics> FFrameAccumulator::Calculate(FString& OutError) const
	{
		OutError.Reset();
		if (SamplesSeconds.IsEmpty())
		{
			OutError = TEXT("No measured frames; performance metrics are unavailable.");
			return {};
		}

		TArray<double> SortedSeconds = SamplesSeconds;
		SortedSeconds.Sort();

		FMetrics Metrics;
		Metrics.FrameCount = SamplesSeconds.Num();
		Metrics.DurationSeconds = TotalSeconds;
		Metrics.WorstFrameMilliseconds = SortedSeconds.Last() * 1000.0;
		Metrics.MinFPS = 1.0 / SortedSeconds.Last();
		Metrics.AverageFPS = Metrics.FrameCount / TotalSeconds;
		Metrics.P50FrameMilliseconds = QuantileMilliseconds(SortedSeconds, 0.50);
		Metrics.P95FrameMilliseconds = QuantileMilliseconds(SortedSeconds, 0.95);
		Metrics.P99FrameMilliseconds = QuantileMilliseconds(SortedSeconds, 0.99);
		for (double Seconds : SamplesSeconds)
		{
			Metrics.FramesOverBudget += Seconds > FrameBudgetSeconds ? 1 : 0;
			Metrics.FramesOverBudgetWithTolerance +=
				Seconds > FrameBudgetSeconds + ThresholdToleranceMilliseconds / 1000.0 ? 1 : 0;
			Metrics.FramesOver30FPSBudget += Seconds > FrameBudget30Seconds ? 1 : 0;
			Metrics.FramesOver30FPSBudgetWithTolerance +=
				Seconds > FrameBudget30Seconds + ThresholdToleranceMilliseconds / 1000.0 ? 1 : 0;
		}
		Metrics.bAllFramesAtLeast60 = Metrics.FramesOverBudgetWithTolerance == 0;
		Metrics.bAllFramesAtLeast30 = Metrics.FramesOver30FPSBudgetWithTolerance == 0;
		return Metrics;
	}

	bool FCaptureWindow::Start(const FOptions& InOptions, FString& OutError)
	{
		if (!ValidateOptions(InOptions, OutError))
		{
			return false;
		}
		Options = InOptions;
		Accumulator.Reset();
		PreviousBoundary.Reset();
		FirstBoundarySeconds = 0.0;
		ExcludedWarmupSeconds = 0.0;
		ExcludedWarmupFrames = 0;
		bStarted = true;
		bFinished = false;
		return true;
	}

	EBoundaryResult FCaptureWindow::AddBoundary(double BoundarySeconds, FString& OutError)
	{
		OutError.Reset();
		if (!bStarted || bFinished)
		{
			OutError = TEXT("Capture window is not active.");
			return EBoundaryResult::Error;
		}
		if (!FMath::IsFinite(BoundarySeconds) || BoundarySeconds < 0.0)
		{
			bFinished = true;
			OutError = TEXT("Frame clock produced a non-finite or negative boundary.");
			return EBoundaryResult::Error;
		}
		if (!PreviousBoundary.IsSet())
		{
			FirstBoundarySeconds = BoundarySeconds;
			PreviousBoundary = BoundarySeconds;
			return EBoundaryResult::Primed;
		}

		const double PeriodSeconds = BoundarySeconds - PreviousBoundary.GetValue();
		if (!FMath::IsFinite(PeriodSeconds) || PeriodSeconds <= 0.0)
		{
			bFinished = true;
			OutError = TEXT("Frame clock did not advance strictly; capture cannot silently skip this sample.");
			return EBoundaryResult::Error;
		}

		// A frame straddling the warmup deadline is excluded in full, never clipped.
		if (PreviousBoundary.GetValue() - FirstBoundarySeconds < Options.WarmupSeconds)
		{
			++ExcludedWarmupFrames;
			ExcludedWarmupSeconds = BoundarySeconds - FirstBoundarySeconds;
			PreviousBoundary = BoundarySeconds;
			return EBoundaryResult::Warmup;
		}

		if (!Accumulator.AddSample(PeriodSeconds, OutError))
		{
			bFinished = true;
			return EBoundaryResult::Error;
		}
		PreviousBoundary = BoundarySeconds;
		if (Accumulator.GetDurationSeconds() >= Options.DurationSeconds)
		{
			bFinished = true;
			return EBoundaryResult::Complete;
		}
		return EBoundaryResult::Recorded;
	}
}
