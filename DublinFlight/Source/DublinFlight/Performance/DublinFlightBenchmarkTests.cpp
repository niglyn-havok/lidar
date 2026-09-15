#include "DublinFlightBenchmark.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include <limits>

using namespace DublinFlight::Performance;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBenchmarkContractTest,
	"DublinFlight.Performance.Benchmark.ConsoleAndStrictTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBenchmarkContractTest::RunTest(const FString& Parameters)
{
	FOptions Options;
	EBenchmark Kind;
	FString Error;
	TestTrue(TEXT("Normal defaults"), ParseBenchmarkArguments({ TEXT("normal") }, Kind, Options, Error));
	TestEqual(TEXT("Duration default"), Options.DurationSeconds, 30.0);
	TestEqual(TEXT("Warmup default"), Options.WarmupSeconds, 5.0);
	TestEqual(TEXT("Normal strict target"), Options.ScenarioTargetMinimumFPS, 60);
	TestTrue(TEXT("Light"), ParseBenchmarkArguments({ TEXT("light"), TEXT("1"), TEXT("0") }, Kind, Options, Error));
	TestEqual(TEXT("Light strict target"), Options.ScenarioTargetMinimumFPS, 60);
	TestTrue(TEXT("Maximal"), ParseBenchmarkArguments({ TEXT("maximal") }, Kind, Options, Error));
	TestEqual(TEXT("Maximal strict target"), Options.ScenarioTargetMinimumFPS, 30);
	TestFalse(TEXT("No arbitrary runner"), ParseBenchmarkArguments({ TEXT("exec") }, Kind, Options, Error));
	TestFalse(TEXT("Scenario required"), ParseBenchmarkArguments({}, Kind, Options, Error));
	TestFalse(TEXT("No fourth target override"), ParseBenchmarkArguments(
		{ TEXT("light"), TEXT("30"), TEXT("5"), TEXT("30") }, Kind, Options, Error));
	TestFalse(TEXT("Finite duration"), ParseBenchmarkArguments({ TEXT("normal"), TEXT("nan") }, Kind, Options, Error));
	for (int32 Mask = 0; Mask < 16; ++Mask)
	{
		TestEqual(TEXT("Completion, valid environment, admitted workload AND frame floor all required"),
			BenchmarkPassed((Mask & 1) != 0, (Mask & 2) != 0, (Mask & 4) != 0, (Mask & 8) != 0), Mask == 15);
	}
	FDublinRuntimeBenchmark Manual(Kind, Options);
	Manual.Finish(TEXT("manualStop"));
	const TSharedRef<FJsonObject> Report = Manual.Report();
	TestFalse(TEXT("Manual stop invalid"), Report->GetBoolField(TEXT("benchmarkValid")));
	TestFalse(TEXT("No fabricated admitted workload"), Report->GetBoolField(TEXT("workloadAdmitted")));
	TestEqual(TEXT("Stop reason retained"), Report->GetStringField(TEXT("completionReason")), FString(TEXT("manualStop")));
	TestTrue(TEXT("Invalid reasons persisted"), Report->GetArrayField(TEXT("invalidReasons")).Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBenchmarkBoundaryTest,
	"DublinFlight.Performance.Benchmark.SharedWarmupAndEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBenchmarkBoundaryTest::RunTest(const FString& Parameters)
{
	FCaptureWindow Window;
	FOptions Options;
	Options.DurationSeconds = 1;
	Options.WarmupSeconds = 0.5;
	FString Error;
	TestTrue(TEXT("Start shared window"), Window.Start(Options, Error));
	TestFalse(TEXT("No workload before prime"), Window.IsMeasurementBoundary());
	Window.AddBoundary(10, Error);
	TestFalse(TEXT("No warmup destruction"), Window.IsMeasurementBoundary());
	Window.AddBoundary(10.25, Error);
	TestFalse(TEXT("Warmup still excludes operations"), Window.IsMeasurementBoundary());
	Window.AddBoundary(10.75, Error);
	TestTrue(TEXT("Workload starts at crossing boundary, BEFORE first measured frame"), Window.IsMeasurementBoundary());
	TestEqual(TEXT("Crossing interval remains excluded"), Window.GetAccumulator().Num(), 0);
	TestTrue(TEXT("First complete workload interval"), Window.AddBoundary(11.25, Error) == EBoundaryResult::Recorded);
	TestTrue(TEXT("End reached before more workload dispatched"), Window.AddBoundary(11.75, Error) == EBoundaryResult::Complete);
	TestEqual(TEXT("Every measured frame retained"), Window.GetAccumulator().Num(), 2);
	Options.WarmupSeconds = 0;
	Window.Start(Options, Error);
	Window.AddBoundary(20, Error);
	TestTrue(TEXT("Zero warmup workload starts at prime"), Window.IsMeasurementBoundary());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBenchmarkScheduleTest,
	"DublinFlight.Performance.Benchmark.WallScheduleAndMissedAdmissions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBenchmarkScheduleTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Six fourteen-round bursts"), PlannedCannonShots(30), 84);
	TestEqual(TEXT("One-second bounded test"), PlannedCannonShots(1), 7);
	TestFalse(TEXT("No premeasurement cannon"), LightBurstActive(-1, 30));
	TestTrue(TEXT("First destructive event at capture start"), LightBurstActive(0, 30));
	TestFalse(TEXT("Burst ends at 2s"), LightBurstActive(2, 30));
	TestTrue(TEXT("Next deterministic burst"), LightBurstActive(5, 30));
	TestFalse(TEXT("No workload at shared end"), LightBurstActive(30, 30));
	FBenchmarkSlots Slots;
	TestEqual(TEXT("First stress operation"), Slots.Take(0, 30, 0.5), 0);
	TestEqual(TEXT("One operation per callback, no duplicate"), Slots.Take(0, 30, 0.5), INDEX_NONE);
	TestEqual(TEXT("2Hz ceiling"), Slots.Take(0.49, 30, 0.5), INDEX_NONE);
	TestEqual(TEXT("Next operation"), Slots.Take(0.5, 30, 0.5), 1);
	TestEqual(TEXT("Slow frames skip, never replay backlog"), Slots.Take(2.1, 30, 0.5), 4);
	TestEqual(TEXT("Both missed operations counted"), Slots.Skipped, 2);
	TestEqual(TEXT("No close catch-up after late callback"), Slots.Take(2.5, 30, 0.5), INDEX_NONE);
	TestEqual(TEXT("No operation at capture end"), Slots.Take(30, 30, 0.5), INDEX_NONE);
	TestEqual(TEXT("Nonfinite clock cannot admit work"), Slots.Take(std::numeric_limits<double>::quiet_NaN(), 30, 0.5), INDEX_NONE);
	TestEqual(TEXT("Invalid period cannot admit work"), Slots.Take(3, 30, 0), INDEX_NONE);
	return true;
}
#endif
