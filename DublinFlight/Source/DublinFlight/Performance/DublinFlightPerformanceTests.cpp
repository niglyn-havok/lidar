#include "DublinFlightPerformanceMetrics.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include <cmath>
#include <limits>

namespace
{
	using namespace DublinFlight::Performance;

	bool TestClose(FAutomationTestBase& Test, const TCHAR* Description, double Actual, double Expected)
	{
		return Test.TestTrue(FString::Printf(TEXT("%s: actual=%.17g, expected=%.17g"), Description, Actual, Expected),
			FMath::IsFinite(Actual) && FMath::Abs(Actual - Expected) <= 1.0e-10);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceQuantilesTest,
	"DublinFlight.Performance.Metrics.QuantilesAndOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceQuantilesTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FFrameAccumulator Accumulator;
	Accumulator.Reset();
	FString Error;
	for (double Sample : { 0.050, 0.010, 0.040, 0.020, 0.030 })
	{
		TestTrue(TEXT("Accept finite positive sample"), Accumulator.AddSample(Sample, Error));
	}
	const TArray<double> OriginalOrder = Accumulator.GetSamplesSeconds();
	const TOptional<FMetrics> Metrics = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Metrics exist"), Metrics.IsSet()))
	{
		return false;
	}
	const FMetrics& Value = Metrics.GetValue();
	TestEqual(TEXT("Count"), Value.FrameCount, 5);
	TestClose(*this, TEXT("Total duration"), Value.DurationSeconds, 0.150);
	TestClose(*this, TEXT("Average is count / total seconds"), Value.AverageFPS, 5.0 / 0.150);
	TestClose(*this, TEXT("Minimum is reciprocal worst frame"), Value.MinFPS, 20.0);
	TestClose(*this, TEXT("Worst milliseconds"), Value.WorstFrameMilliseconds, 50.0);
	TestClose(*this, TEXT("p50 type 7"), Value.P50FrameMilliseconds, 30.0);
	TestClose(*this, TEXT("p95 type 7"), Value.P95FrameMilliseconds, 48.0);
	TestClose(*this, TEXT("p99 type 7"), Value.P99FrameMilliseconds, 49.6);
	TestEqual(TEXT("Strict over-budget frames"), Value.FramesOverBudget, 4);
	TestFalse(TEXT("Not all frames reached 60"), Value.bAllFramesAtLeast60);
	TestTrue(TEXT("Quantiles do not reorder the captured trace"), OriginalOrder == Accumulator.GetSamplesSeconds());

	FFrameAccumulator Reversed;
	Reversed.Reset();
	for (int32 Index = OriginalOrder.Num() - 1; Index >= 0; --Index)
	{
		TestTrue(TEXT("Accept reversed sample"), Reversed.AddSample(OriginalOrder[Index], Error));
	}
	const TOptional<FMetrics> ReversedMetrics = Reversed.Calculate(Error);
	if (!TestTrue(TEXT("Reversed metrics exist"), ReversedMetrics.IsSet()))
	{
		return false;
	}
	TestClose(*this, TEXT("Order-independent p95"), ReversedMetrics.GetValue().P95FrameMilliseconds, Value.P95FrameMilliseconds);
	TestClose(*this, TEXT("Order-independent average"), ReversedMetrics.GetValue().AverageFPS, Value.AverageFPS);

	Accumulator.Reset();
	TestTrue(TEXT("Singleton sample accepted"), Accumulator.AddSample(0.012, Error));
	const TOptional<FMetrics> Singleton = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Singleton metrics exist"), Singleton.IsSet()))
	{
		return false;
	}
	TestClose(*this, TEXT("Singleton p50"), Singleton.GetValue().P50FrameMilliseconds, 12.0);
	TestClose(*this, TEXT("Singleton p95"), Singleton.GetValue().P95FrameMilliseconds, 12.0);
	TestClose(*this, TEXT("Singleton p99"), Singleton.GetValue().P99FrameMilliseconds, 12.0);
	TestEqual(TEXT("Reset discards old frames"), Singleton.GetValue().FrameCount, 1);

	Accumulator.Reset();
	TestTrue(TEXT("First pair sample"), Accumulator.AddSample(0.010, Error));
	TestTrue(TEXT("Second pair sample"), Accumulator.AddSample(0.020, Error));
	const TOptional<FMetrics> Pair = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Pair metrics exist"), Pair.IsSet()))
	{
		return false;
	}
	TestClose(*this, TEXT("Even-sized interpolated median"), Pair.GetValue().P50FrameMilliseconds, 15.0);
	TestClose(*this, TEXT("Two-value p95 interpolation"), Pair.GetValue().P95FrameMilliseconds, 19.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceSpikeTest,
	"DublinFlight.Performance.Metrics.SpikeDespiteGoodAverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceSpikeTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FFrameAccumulator Accumulator;
	Accumulator.Reset();
	FString Error;
	for (int32 Index = 0; Index < 999; ++Index)
	{
		if (!TestTrue(TEXT("Fast sample accepted"), Accumulator.AddSample(0.010, Error)))
		{
			return false;
		}
	}
	TestTrue(TEXT("Unfiltered hitch accepted"), Accumulator.AddSample(0.250, Error));
	const TOptional<FMetrics> Metrics = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Metrics exist"), Metrics.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("Average exceeds 60 FPS"), Metrics.GetValue().AverageFPS > 60.0);
	TestClose(*this, TEXT("Worst-frame minimum still 4 FPS"), Metrics.GetValue().MinFPS, 4.0);
	TestClose(*this, TEXT("250 ms hitch is preserved"), Metrics.GetValue().WorstFrameMilliseconds, 250.0);
	TestClose(*this, TEXT("p99 alone also misses this rare hitch"), Metrics.GetValue().P99FrameMilliseconds, 10.0);
	TestEqual(TEXT("One slow frame"), Metrics.GetValue().FramesOverBudget, 1);
	TestFalse(TEXT("Good average cannot imply minimum 60"), Metrics.GetValue().bAllFramesAtLeast60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceInvalidSamplesTest,
	"DublinFlight.Performance.Metrics.RejectInvalidSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceInvalidSamplesTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FFrameAccumulator Accumulator;
	Accumulator.Reset();
	FString Error;
	TestFalse(TEXT("Empty capture has no metrics"), Accumulator.Calculate(Error).IsSet());
	TestFalse(TEXT("Empty capture explains missing metrics"), Error.IsEmpty());
	TestTrue(TEXT("Seed sample"), Accumulator.AddSample(0.010, Error));

	const double InvalidSamples[] =
	{
		0.0, -0.0, -0.001, std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min()
	};
	for (double Sample : InvalidSamples)
	{
		TestFalse(TEXT("Invalid or unrepresentable sample rejected"), Accumulator.AddSample(Sample, Error));
		TestFalse(TEXT("Rejection explains failure"), Error.IsEmpty());
		TestEqual(TEXT("Rejected sample cannot change count"), Accumulator.Num(), 1);
		TestClose(*this, TEXT("Rejected sample cannot change total time"), Accumulator.GetDurationSeconds(), 0.010);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceThresholdTest,
	"DublinFlight.Performance.Metrics.Exact60FPSThreshold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceThresholdTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FFrameAccumulator Accumulator;
	Accumulator.Reset();
	FString Error;
	TestTrue(TEXT("Rounded 16.666 ms sample"), Accumulator.AddSample(16.666 / 1000.0, Error));
	TestTrue(TEXT("Exact 1000/60 ms sample"), Accumulator.AddSample(FrameBudgetSeconds, Error));
	const TOptional<FMetrics> Exact = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Exact threshold metrics"), Exact.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Exactly equal is not strictly greater"), Exact.GetValue().FramesOverBudget, 0);
	TestTrue(TEXT("All exact-budget frames reach 60"), Exact.GetValue().bAllFramesAtLeast60);
	TestClose(*this, TEXT("Minimum at the exact budget"), Exact.GetValue().MinFPS, 60.0);

	Accumulator.Reset();
	const double OneUlpOver = std::nextafter(FrameBudgetSeconds, std::numeric_limits<double>::infinity());
	TestTrue(TEXT("One ULP over budget sample"), Accumulator.AddSample(OneUlpOver, Error));
	const TOptional<FMetrics> Tiny = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Tiny overrun metrics"), Tiny.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Strict count catches even an ULP"), Tiny.GetValue().FramesOverBudget, 1);
	TestEqual(TEXT("Numerical tolerance count stays zero"), Tiny.GetValue().FramesOverBudgetWithTolerance, 0);
	TestTrue(TEXT("Only tiny numerical error is tolerated"), Tiny.GetValue().bAllFramesAtLeast60);

	Accumulator.Reset();
	TestTrue(TEXT("Meaningful 16.667 ms overrun"), Accumulator.AddSample(16.667 / 1000.0, Error));
	const TOptional<FMetrics> Slow = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Slow metrics"), Slow.IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("16.667 ms must fail minimum 60"), Slow.GetValue().bAllFramesAtLeast60);
	TestEqual(TEXT("Strict count"), Slow.GetValue().FramesOverBudget, 1);
	TestEqual(TEXT("Beyond-tolerance count"), Slow.GetValue().FramesOverBudgetWithTolerance, 1);

	Accumulator.Reset();
	TestTrue(TEXT("Two-nanosecond overrun"), Accumulator.AddSample(
		FrameBudgetSeconds + 2.0 * ThresholdToleranceMilliseconds / 1000.0, Error));
	const TOptional<FMetrics> BeyondTolerance = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Beyond-tolerance metrics"), BeyondTolerance.IsSet()))
	{
		return false;
	}
	TestFalse(TEXT("Tolerance cannot hide a two-nanosecond overrun"), BeyondTolerance.GetValue().bAllFramesAtLeast60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceInputsTest,
	"DublinFlight.Performance.Input.LabelsAndDurations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceInputsTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FString Error;
	for (const TCHAR* Label : { TEXT("capture"), TEXT("Route-02_high"), TEXT("0"), TEXT("COM10") })
	{
		TestTrue(TEXT("Safe label accepted"), ValidateLabel(Label, Error));
	}
	TestTrue(TEXT("64 characters accepted"), ValidateLabel(FString::ChrN(64, TEXT('a')), Error));
	for (const TCHAR* Label : { TEXT(""), TEXT("../escape"), TEXT("..\\escape"), TEXT("a/b"), TEXT("a\\b"),
		TEXT("C:\\capture"), TEXT("a:b"), TEXT("."), TEXT(".."), TEXT("white space"), TEXT("a\nb"),
		TEXT("a\rb"), TEXT("a\tb"), TEXT("a."), TEXT("a "), TEXT("_prefix"), TEXT("-prefix"),
		TEXT("CON"), TEXT("con"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"), TEXT("com1"), TEXT("LPT9"),
		TEXT("a*"), TEXT("a?"), TEXT("a\"b"), TEXT("a|b"), TEXT("<a>") })
	{
		TestFalse(TEXT("Unsafe label rejected"), ValidateLabel(Label, Error));
		TestFalse(TEXT("Unsafe label supplies reason"), Error.IsEmpty());
	}
	// Mutate allocated storage so string construction cannot discard the embedded NUL.
	FString ControlLabel = TEXT("a_b");
	ControlLabel[1] = static_cast<TCHAR>(0);
	TestEqual(TEXT("Embedded NUL fixture retains its length"), ControlLabel.Len(), 3);
	TestTrue(TEXT("Embedded NUL fixture contains a NUL"), ControlLabel[1] == static_cast<TCHAR>(0));
	TestTrue(TEXT("Embedded NUL fixture retains its suffix"), ControlLabel[2] == TEXT('b'));
	TestFalse(TEXT("Embedded NUL rejected"), ValidateLabel(ControlLabel, Error));
	TestFalse(TEXT("Embedded NUL rejection supplies reason"), Error.IsEmpty());
	TestFalse(TEXT("Non-ASCII rejected"), ValidateLabel(FString::ChrN(1, static_cast<TCHAR>(0x00e9)), Error));
	TestFalse(TEXT("Overlong label rejected"), ValidateLabel(FString::ChrN(65, TEXT('a')), Error));

	FOptions Options;
	TestTrue(TEXT("All arguments optional"), ParseStartArguments({}, Options, Error));
	TestClose(*this, TEXT("Default duration"), Options.DurationSeconds, 30.0);
	TestClose(*this, TEXT("Default warmup"), Options.WarmupSeconds, 5.0);
	TestTrue(TEXT("Inclusive lower bounds"), ParseStartArguments({ TEXT("route"), TEXT("1"), TEXT("0") }, Options, Error));
	TestTrue(TEXT("Inclusive upper bounds"), ParseStartArguments({ TEXT("route"), TEXT("300"), TEXT("60") }, Options, Error));
	TestTrue(TEXT("Fractional seconds"), ParseStartArguments({ TEXT("route"), TEXT("1.25"), TEXT(".5") }, Options, Error));
	for (const TCHAR* Number : { TEXT("0"), TEXT("0.999"), TEXT("300.001"), TEXT("-1"), TEXT("nan"),
		TEXT("inf"), TEXT("1e309"), TEXT("1e2"), TEXT("30garbage"), TEXT("1.2.3"), TEXT(""),
		TEXT("."), TEXT(" 30"), TEXT("30 "), TEXT("1,5"), TEXT("+30") })
	{
		TestFalse(TEXT("Bad duration rejected"), ParseStartArguments({ TEXT("route"), Number }, Options, Error));
		TestFalse(TEXT("Bad duration explains failure"), Error.IsEmpty());
	}
	for (const TCHAR* Number : { TEXT("-0.1"), TEXT("60.001"), TEXT("nan"), TEXT("infinity"), TEXT("5x") })
	{
		TestFalse(TEXT("Bad warmup rejected"), ParseStartArguments({ TEXT("route"), TEXT("30"), Number }, Options, Error));
	}
	TestFalse(TEXT("Extra arguments rejected"), ParseStartArguments(
		{ TEXT("route"), TEXT("30"), TEXT("5"), TEXT("extra") }, Options, Error));

	for (double Invalid : { -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
	{
		FOptions Direct;
		Direct.DurationSeconds = Invalid;
		TestFalse(TEXT("Native duration rejects invalid finite/range values"), ValidateOptions(Direct, Error));
		Direct = FOptions();
		Direct.WarmupSeconds = Invalid;
		TestFalse(TEXT("Native warmup rejects invalid finite/range values"), ValidateOptions(Direct, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceWindowTest,
	"DublinFlight.Performance.Window.WarmupAndFullFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceWindowTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FCaptureWindow Window;
	FOptions Options;
	Options.DurationSeconds = 1.0;
	Options.WarmupSeconds = 0.5;
	FString Error;
	TestTrue(TEXT("Configure window"), Window.Start(Options, Error));
	TestTrue(TEXT("First boundary only primes"), Window.AddBoundary(10.0, Error) == EBoundaryResult::Primed);
	TestEqual(TEXT("Prime is not a frame"), Window.GetAccumulator().Num(), 0);
	TestTrue(TEXT("Before deadline excluded"), Window.AddBoundary(10.25, Error) == EBoundaryResult::Warmup);
	TestTrue(TEXT("Crossing warmup deadline excluded whole"), Window.AddBoundary(10.75, Error) == EBoundaryResult::Warmup);
	TestEqual(TEXT("Explicit excluded-frame count"), Window.GetExcludedWarmupFrames(), uint64(2));
	TestClose(*this, TEXT("Actual warmup includes boundary overshoot"), Window.GetExcludedWarmupSeconds(), 0.75);
	TestEqual(TEXT("Warmup is not measured"), Window.GetAccumulator().Num(), 0);
	TestTrue(TEXT("First full post-warmup frame measured"), Window.AddBoundary(11.0, Error) == EBoundaryResult::Recorded);
	TestTrue(TEXT("End-crossing full frame completes"), Window.AddBoundary(12.0, Error) == EBoundaryResult::Complete);
	const TOptional<FMetrics> Metrics = Window.GetAccumulator().Calculate(Error);
	if (!TestTrue(TEXT("Window metrics exist"), Metrics.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("No warmup frames leaked into capture"), Metrics.GetValue().FrameCount, 2);
	TestClose(*this, TEXT("Last frame is not clipped to target"), Metrics.GetValue().DurationSeconds, 1.25);
	TestClose(*this, TEXT("Measured hitch is never filtered"), Metrics.GetValue().WorstFrameMilliseconds, 1000.0);
	TestTrue(TEXT("Cannot append after completion"), Window.AddBoundary(12.25, Error) == EBoundaryResult::Error);

	Options.WarmupSeconds = 0.0;
	TestTrue(TEXT("Restart with zero warmup"), Window.Start(Options, Error));
	TestTrue(TEXT("Zero-warmup prime"), Window.AddBoundary(20.0, Error) == EBoundaryResult::Primed);
	TestTrue(TEXT("First whole frame measured at zero warmup"), Window.AddBoundary(20.25, Error) == EBoundaryResult::Recorded);
	TestEqual(TEXT("No extra startup exclusions"), Window.GetExcludedWarmupFrames(), uint64(0));
	TestEqual(TEXT("Measured prefix available for manual stop"), Window.GetAccumulator().Num(), 1);
	TestClose(*this, TEXT("Manual-stop prefix contains complete intervals"), Window.GetAccumulator().GetDurationSeconds(), 0.25);

	Options.WarmupSeconds = 0.5;
	TestTrue(TEXT("Exact-deadline restart"), Window.Start(Options, Error));
	TestTrue(TEXT("Exact-deadline prime"), Window.AddBoundary(30.0, Error) == EBoundaryResult::Primed);
	TestTrue(TEXT("Frame ending at deadline excluded"), Window.AddBoundary(30.5, Error) == EBoundaryResult::Warmup);
	TestTrue(TEXT("Frame starting at deadline measured"), Window.AddBoundary(30.75, Error) == EBoundaryResult::Recorded);
	TestEqual(TEXT("Only one exact warmup frame excluded"), Window.GetExcludedWarmupFrames(), uint64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceClockTest,
	"DublinFlight.Performance.Window.RejectInvalidClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceClockTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FCaptureWindow Window;
	FOptions Options;
	Options.WarmupSeconds = 0.0;
	FString Error;
	TestTrue(TEXT("Unconfigured clock rejected"), Window.AddBoundary(10.0, Error) == EBoundaryResult::Error);
	for (double InvalidBoundary : { 10.0, 9.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity() })
	{
		TestTrue(TEXT("Clock setup"), Window.Start(Options, Error));
		TestTrue(TEXT("Clock prime"), Window.AddBoundary(10.0, Error) == EBoundaryResult::Primed);
		TestTrue(TEXT("Non-advancing/non-finite boundary fails capture"), Window.AddBoundary(InvalidBoundary, Error) == EBoundaryResult::Error);
		TestFalse(TEXT("Invalid clock explains failure"), Error.IsEmpty());
		TestEqual(TEXT("Bad boundary cannot become a frame"), Window.GetAccumulator().Num(), 0);
		TestTrue(TEXT("Failed window cannot silently recover"), Window.AddBoundary(11.0, Error) == EBoundaryResult::Error);
	}
	TestTrue(TEXT("First-boundary setup"), Window.Start(Options, Error));
	TestTrue(TEXT("Non-finite initial boundary rejected"),
		Window.AddBoundary(std::numeric_limits<double>::quiet_NaN(), Error) == EBoundaryResult::Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceCapTest,
	"DublinFlight.Performance.Metrics.MemoryCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceCapTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FFrameAccumulator Accumulator;
	Accumulator.Reset();
	FString Error;
	for (int32 Index = 0; Index < MaxCapturedFrames; ++Index)
	{
		if (!Accumulator.AddSample(0.001, Error))
		{
			AddError(FString::Printf(TEXT("Unexpected failure before cap at frame %d: %s"), Index, *Error));
			return false;
		}
	}
	const double BeforeFailure = Accumulator.GetDurationSeconds();
	TestFalse(TEXT("First sample exceeding cap fails explicitly"), Accumulator.AddSample(0.5, Error));
	TestTrue(TEXT("Error identifies cap"), Error.Contains(TEXT("120000-frame memory cap")));
	TestEqual(TEXT("No overwrite or ring-buffer drop"), Accumulator.Num(), MaxCapturedFrames);
	TestClose(*this, TEXT("Cap failure preserves total"), Accumulator.GetDurationSeconds(), BeforeFailure);
	TestClose(*this, TEXT("Cap failure preserves first sample"), Accumulator.GetSamplesSeconds()[0], 0.001);
	TestClose(*this, TEXT("Cap failure preserves final accepted sample"), Accumulator.GetSamplesSeconds().Last(), 0.001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformance30ThresholdTest,
	"DublinFlight.Performance.Metrics.Exact30FPSThreshold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformance30ThresholdTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	struct FCase
	{
		double Seconds;
		int32 StrictCount;
		int32 TolerantCount;
		bool bPass;
	};
	const FCase Cases[] =
	{
		{ 0.025, 0, 0, true },
		{ 33.333 / 1000.0, 0, 0, true },
		{ FrameBudget30Seconds, 0, 0, true },
		{ std::nextafter(FrameBudget30Seconds, std::numeric_limits<double>::infinity()), 1, 0, true },
		{ 33.334 / 1000.0, 1, 1, false },
		{ FrameBudget30Seconds + 2.0 * ThresholdToleranceMilliseconds / 1000.0, 1, 1, false }
	};
	TestClose(*this, TEXT("30 FPS budget is not rounded to 33.333 ms"), FrameBudget30Milliseconds, 1000.0 / 30.0);
	FFrameAccumulator Accumulator;
	FString Error;
	for (const FCase& Case : Cases)
	{
		Accumulator.Reset();
		if (!TestTrue(TEXT("30 FPS threshold sample accepted"), Accumulator.AddSample(Case.Seconds, Error)))
		{
			return false;
		}
		const TOptional<FMetrics> Metrics = Accumulator.Calculate(Error);
		if (!TestTrue(TEXT("30 FPS threshold metrics exist"), Metrics.IsSet()))
		{
			return false;
		}
		const FMetrics& Value = Metrics.GetValue();
		TestEqual(TEXT("Strict 30 FPS over-budget count"), Value.FramesOver30FPSBudget, Case.StrictCount);
		TestEqual(TEXT("Tolerant 30 FPS over-budget count"), Value.FramesOver30FPSBudgetWithTolerance, Case.TolerantCount);
		TestEqual(TEXT("All frames at least 30 with disclosed tolerance"), Value.bAllFramesAtLeast30, Case.bPass);
		TestEqual(TEXT("30 FPS target verdict uses frame predicate"), Value.MeetsTargetMinimumFPS(30), Case.bPass);
		TestFalse(TEXT("These samples still fail the unchanged 60 FPS predicate"), Value.bAllFramesAtLeast60);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceScenarioSpikeTest,
	"DublinFlight.Performance.Metrics.ScenarioTargetsAndSpike",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceScenarioSpikeTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	TestFalse(TEXT("Empty metrics cannot meet 30 FPS"), FMetrics().MeetsTargetMinimumFPS(30));
	TestFalse(TEXT("Empty metrics cannot meet 60 FPS"), FMetrics().MeetsTargetMinimumFPS(60));
	FFrameAccumulator Accumulator;
	Accumulator.Reset();
	FString Error;
	TestTrue(TEXT("100 FPS frame accepted"), Accumulator.AddSample(0.010, Error));
	const TOptional<FMetrics> Fast = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("100 FPS metrics exist"), Fast.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("100 FPS meets the default 60 target"), Fast.GetValue().MeetsTargetMinimumFPS(60));
	TestTrue(TEXT("100 FPS also meets the 30 target"), Fast.GetValue().MeetsTargetMinimumFPS(30));
	Accumulator.Reset();
	TestTrue(TEXT("40 FPS frame accepted"), Accumulator.AddSample(0.025, Error));
	const TOptional<FMetrics> FortyFPS = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("40 FPS metrics exist"), FortyFPS.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("40 FPS meets destruction target"), FortyFPS.GetValue().MeetsTargetMinimumFPS(30));
	TestFalse(TEXT("40 FPS does not meet normal-flight target"), FortyFPS.GetValue().MeetsTargetMinimumFPS(60));
	TestFalse(TEXT("Unsupported native target cannot pass"), FortyFPS.GetValue().MeetsTargetMinimumFPS(40));
	TestEqual(TEXT("60 FPS count is independent of selected target"), FortyFPS.GetValue().FramesOverBudget, 1);

	Accumulator.Reset();
	for (int32 Index = 0; Index < 999; ++Index)
	{
		if (!TestTrue(TEXT("Fast frame accepted"), Accumulator.AddSample(0.010, Error)))
		{
			return false;
		}
	}
	TestTrue(TEXT("One 50 ms hitch accepted"), Accumulator.AddSample(0.050, Error));
	const TOptional<FMetrics> Spiked = Accumulator.Calculate(Error);
	if (!TestTrue(TEXT("Hitch metrics exist"), Spiked.IsSet()))
	{
		return false;
	}
	const FMetrics& Value = Spiked.GetValue();
	TestTrue(TEXT("Average still exceeds both targets"), Value.AverageFPS > 60.0);
	TestClose(*this, TEXT("One bad frame lowers minimum to 20 FPS"), Value.MinFPS, 20.0);
	TestEqual(TEXT("Exactly one strict 30 FPS failure"), Value.FramesOver30FPSBudget, 1);
	TestEqual(TEXT("Exactly one tolerant 30 FPS failure"), Value.FramesOver30FPSBudgetWithTolerance, 1);
	TestFalse(TEXT("Good average cannot meet the 30 FPS target"), Value.MeetsTargetMinimumFPS(30));
	TestFalse(TEXT("Good average cannot meet the 60 FPS target"), Value.MeetsTargetMinimumFPS(60));
	TestFalse(TEXT("One bad frame defeats all-frames-30"), Value.bAllFramesAtLeast30);
	TestFalse(TEXT("Existing all-frames-60 remains false"), Value.bAllFramesAtLeast60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFlightPerformanceTargetInputTest,
	"DublinFlight.Performance.Input.ScenarioTargetMinimumFPS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFlightPerformanceTargetInputTest::RunTest(const FString& Parameters)
{
	using namespace DublinFlight::Performance;
	FOptions Options;
	FString Error;
	TestEqual(TEXT("Native default target remains 60"), Options.ScenarioTargetMinimumFPS, 60);
	TestTrue(TEXT("Explicit destruction target accepted"), ParseStartArguments(
		{ TEXT("destruction"), TEXT("30"), TEXT("5"), TEXT("30") }, Options, Error));
	TestEqual(TEXT("Explicit 30 target retained"), Options.ScenarioTargetMinimumFPS, 30);
	TestTrue(TEXT("Explicit normal-flight target accepted"), ParseStartArguments(
		{ TEXT("flight"), TEXT("30"), TEXT("5"), TEXT("60") }, Options, Error));
	TestEqual(TEXT("Explicit 60 target retained"), Options.ScenarioTargetMinimumFPS, 60);

	TArray<FString> LegacyArgs;
	const TCHAR* LegacyValues[] = { TEXT("route"), TEXT("30"), TEXT("5") };
	for (int32 Count = 0; Count <= 3; ++Count)
	{
		Options.ScenarioTargetMinimumFPS = 30;
		TestTrue(TEXT("Legacy invocation accepted"), ParseStartArguments(LegacyArgs, Options, Error));
		TestEqual(TEXT("Legacy invocation resets target to 60"), Options.ScenarioTargetMinimumFPS, 60);
		if (Count < 3)
		{
			LegacyArgs.Add(LegacyValues[Count]);
		}
	}
	for (const TCHAR* Target : { TEXT(""), TEXT("0"), TEXT("29"), TEXT("31"), TEXT("59"), TEXT("61"),
		TEXT("120"), TEXT("30.0"), TEXT("030"), TEXT("+30"), TEXT("-30"), TEXT("3e1"),
		TEXT(" 30"), TEXT("30 "), TEXT("nan"), TEXT("inf"), TEXT("30garbage") })
	{
		TestFalse(TEXT("Invalid target rejected"), ParseStartArguments(
			{ TEXT("route"), TEXT("30"), TEXT("5"), Target }, Options, Error));
		TestTrue(TEXT("Target error states allowed values"), Error.Contains(TEXT("30 or 60")));
		TestEqual(TEXT("Invalid target cannot partially replace output options"), Options.ScenarioTargetMinimumFPS, 60);
	}
	FString EmbeddedNulTarget = TEXT("30x");
	EmbeddedNulTarget[2] = static_cast<TCHAR>(0);
	TestEqual(TEXT("Malformed target fixture retains hidden character"), EmbeddedNulTarget.Len(), 3);
	TestFalse(TEXT("Target cannot hide a trailing embedded NUL"), ParseStartArguments(
		{ TEXT("route"), TEXT("30"), TEXT("5"), EmbeddedNulTarget }, Options, Error));
	TestFalse(TEXT("Fifth argument rejected"), ParseStartArguments(
		{ TEXT("route"), TEXT("30"), TEXT("5"), TEXT("30"), TEXT("extra") }, Options, Error));
	TestTrue(TEXT("Extra argument error supplies usage"), Error.Contains(TEXT("Usage:")));
	for (int32 Target : { 0, 29, 31, 59, 61, 120 })
	{
		FOptions Invalid;
		Invalid.ScenarioTargetMinimumFPS = Target;
		TestFalse(TEXT("Invalid native target rejected"), ValidateOptions(Invalid, Error));
		TestTrue(TEXT("Native target error states allowed values"), Error.Contains(TEXT("30 or 60")));
	}
	return true;
}

#endif
