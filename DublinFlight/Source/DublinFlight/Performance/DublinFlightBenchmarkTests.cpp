#include "DublinFlightBenchmark.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "City/DublinCityDestruction.h"
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
	TestEqual(TEXT("Maximal reference target stays measurable"), Options.ScenarioTargetMinimumFPS, 30);
	TestFalse(TEXT("No arbitrary runner"), ParseBenchmarkArguments({ TEXT("exec") }, Kind, Options, Error));
	TestFalse(TEXT("Scenario required"), ParseBenchmarkArguments({}, Kind, Options, Error));
	TestFalse(TEXT("No fourth target override"), ParseBenchmarkArguments(
		{ TEXT("light"), TEXT("30"), TEXT("5"), TEXT("30") }, Kind, Options, Error));
	TestFalse(TEXT("Finite duration"), ParseBenchmarkArguments({ TEXT("normal"), TEXT("nan") }, Kind, Options, Error));
	for (int32 Mask = 0; Mask < 16; ++Mask)
	{
		TestEqual(TEXT("Completion, valid environment, admitted workload AND frame floor all required"),
			BenchmarkPassed((Mask & 1) != 0, (Mask & 2) != 0, (Mask & 4) != 0, (Mask & 8) != 0), Mask == 15);
		TestEqual(TEXT("Spectacle may miss the frame floor, never duration, validity or workload"),
			BenchmarkPassed((Mask & 1) != 0, (Mask & 2) != 0, (Mask & 4) != 0, (Mask & 8) != 0, false),
			(Mask & 7) == 7);
	}
	TestTrue(TEXT("Normal retains hard frame floor"),
		FDublinRuntimeBenchmark(EBenchmark::Normal, Options).IsFrameTargetRequired());
	TestTrue(TEXT("Light retains hard frame floor"),
		FDublinRuntimeBenchmark(EBenchmark::Light, Options).IsFrameTargetRequired());
	TestFalse(TEXT("Maximal declares spectacle policy rather than forging a frame-floor pass"),
		FDublinRuntimeBenchmark(EBenchmark::Maximal, Options).IsFrameTargetRequired());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximalDeferredScheduleTest,
	"DublinFlight.Performance.Benchmark.MaximalRetainsSlowFrameRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinMaximalDeferredScheduleTest::RunTest(const FString& Parameters)
{
	FDeferredBenchmarkSlots Slots;
	TestFalse(TEXT("No request before the first measurement boundary"), Slots.Peek() != INDEX_NONE);
	TestTrue(TEXT("Initial measured boundary"), Slots.Observe(0, 30));
	TestEqual(TEXT("Only the already due first slot is released"), Slots.Due, 1);
	TestEqual(TEXT("First slot is zero"), Slots.Take(), 0);
	TestEqual(TEXT("No future work queued at start"), Slots.Take(), INDEX_NONE);
	Slots.Observe(2.1, 30);
	TestEqual(TEXT("Four delayed slots are retained"), Slots.Pending(), 4);
	for (int32 Expected = 1; Expected <= 4; ++Expected)
	{
		TestEqual(TEXT("Late requests keep every original absolute slot in order"), Slots.Take(), Expected);
	}
	TestEqual(TEXT("Deferred batch fully consumed"), Slots.Pending(), 0);
	TestEqual(TEXT("Actual catch-up batch is disclosed"), Slots.PeakBatch, 4);
	Slots.Observe(2.5, 30);
	TestEqual(TEXT("No artificial half-second cooldown after a late batch"), Slots.Take(), 5);

	FDeferredBenchmarkSlots Slow;
	TArray<int32> Attempts;
	for (int32 Second = 0; Second < 30; ++Second)
	{
		Slow.Observe(Second, 30);
		for (int32 Slot = Slow.Take(); Slot != INDEX_NONE; Slot = Slow.Take()) { Attempts.Add(Slot); }
	}
	TestEqual(TEXT("One FPS retains 59 requests through the 29-second boundary"), Attempts.Num(), 59);
	Slow.Observe(29.5, 30);
	Attempts.Add(Slow.Take());
	Slow.Observe(30.64, 30, true);
	TestEqual(TEXT("All sixty planned requests remain achievable without a two-FPS assumption"), Attempts.Num(), 60);
	for (int32 Slot = 0; Slot < Attempts.Num(); ++Slot) { TestEqual(TEXT("No skip or duplicate"), Attempts[Slot], Slot); }
	TestEqual(TEXT("No end backlog after actual in-window admissions"), Slow.Pending(), 0);
	TestEqual(TEXT("One-FPS catch-up uses batches of two, not fictional spacing"), Slow.PeakBatch, 2);
	TestEqual(TEXT("Ending boundary cannot add destruction after measurement"), Slow.Take(), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximalDeferredBoundaryTest,
	"DublinFlight.Performance.Benchmark.MaximalCatchUpBoundAndDeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinMaximalDeferredBoundaryTest::RunTest(const FString& Parameters)
{
	FDeferredBenchmarkSlots Slots;
	Slots.Observe(0, 30);
	Slots.Take();
	Slots.Observe(20, 30);
	TestEqual(TEXT("Forty due requests survive a very slow frame"), Slots.Pending(), 40);
	for (int32 I = 0; I < MaximalAttemptsPerBoundary; ++I) { TestEqual(TEXT("Bounded ordered catch-up"), Slots.Take(), I + 1); }
	TestEqual(TEXT("Ninth attempt waits for a later real boundary"), Slots.Take(), INDEX_NONE);
	TestEqual(TEXT("Excess due requests remain explicitly pending"), Slots.Pending(), 32);
	Slots.Observe(20, 30);
	TestEqual(TEXT("Duplicate observation cannot reset a boundary's work limit"), Slots.Take(), INDEX_NONE);
	Slots.Observe(20.1, 30);
	TestEqual(TEXT("Next boundary resumes, rather than replacing, the oldest due request"), Slots.Take(), 9);
	TestEqual(TEXT("Eight-attempt cap is independent of FPS"), Slots.PeakBatch, 8);
	TestFalse(TEXT("Backward wall clock cannot fabricate another batch"), Slots.Observe(19, 30));
	TestFalse(TEXT("Nonfinite boundary rejected"), Slots.Observe(std::numeric_limits<double>::quiet_NaN(), 30));
	TestFalse(TEXT("Unbounded duration rejected"), Slots.Observe(21, 301));

	FDeferredBenchmarkSlots Backpressure;
	Backpressure.Observe(0, 30);
	Backpressure.Observe(1.1, 30);
	TestEqual(TEXT("Not taking a request under native backpressure loses nothing"), Backpressure.Pending(), 3);
	TestEqual(TEXT("Backpressure retry still starts with slot zero"), Backpressure.Take(), 0);
	Backpressure.Observe(30, 30, true);
	TestEqual(TEXT("End reports all unattempted planned slots"), Backpressure.Pending(), 59);
	TestEqual(TEXT("End backlog is never flushed outside the original window"), Backpressure.Take(), INDEX_NONE);
	TestEqual(TEXT("Even the ending boundary contributes truthful peak pending work"), Backpressure.PeakPending, 59);

	FDeferredBenchmarkSlots LastHalfSecond;
	for (int32 Second = 0; Second < 30; ++Second)
	{
		LastHalfSecond.Observe(Second, 30);
		while (LastHalfSecond.Take() != INDEX_NONE) {}
	}
	LastHalfSecond.Observe(30.64, 30, true);
	TestEqual(TEXT("A final half-second slot without an in-window boundary is genuinely unadmitted, not waived"),
		LastHalfSecond.Pending(), 1);
	FDeferredBenchmarkSlots Fractional;
	Fractional.Observe(1.25, 1.25, true);
	TestEqual(TEXT("Fractional durations preserve ceil(duration/period) planned requests"), Fractional.Due, 3);
	TestEqual(TEXT("No workload admitted at the exact deadline"), Fractional.Take(), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBenchmarkNativeQueueTest,
	"DublinFlight.Performance.Benchmark.MaximalUsesNativeQueueCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBenchmarkNativeQueueTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Captured backlog of 16 is below the real finite impact capacity"), StressQueueHasCapacity(16));
	for (int32 Queued = 0; Queued < 60; ++Queued)
	{
		TestTrue(TEXT("All 60 planned operations can be submitted without a synthetic quarter-cap"),
			StressQueueHasCapacity(Queued));
	}
	TestTrue(TEXT("Last native queue slot remains available"), StressQueueHasCapacity(DublinDestruction::MaxQueuedImpacts - 1));
	TestFalse(TEXT("Full native queue still applies backpressure"), StressQueueHasCapacity(DublinDestruction::MaxQueuedImpacts));
	TestFalse(TEXT("Invalid queue occupancy is not admissible"), StressQueueHasCapacity(-1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximalSaturationReleaseTest,
	"DublinFlight.Performance.Benchmark.MaximalSaturationReleasesAtMeasurementStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinMaximalSaturationReleaseTest::RunTest(const FString& Parameters)
{
	FSaturationBenchmarkSlots Slots;
	TestEqual(TEXT("No release or admission during warmup"), Slots.Take(), INDEX_NONE);
	TestEqual(TEXT("No queued requests before measurement"), Slots.Due, 0);
	TestFalse(TEXT("Negative elapsed time cannot start destruction"), Slots.Observe(-1, 30));
	TestTrue(TEXT("First measured boundary releases the saturation workload"), Slots.Observe(0, 30));
	TestEqual(TEXT("All sixty original operations are due immediately"), Slots.Due, 60);
	TArray<int32> Attempts;
	for (int32 I = 0; I < 8; ++I) { Attempts.Add(Slots.Take()); }
	TestEqual(TEXT("First frame is bounded to eight attempts"), Slots.Take(), INDEX_NONE);
	TestEqual(TEXT("Remaining fifty-two requests are explicit backlog"), Slots.Pending(), 52);
	Slots.Observe(0, 30);
	TestEqual(TEXT("Repeated observation cannot create a ninth same-boundary attempt"), Slots.Take(), INDEX_NONE);
	for (int32 Boundary = 1; Boundary <= 7; ++Boundary)
	{
		Slots.Observe(Boundary * 2.0, 30);
		for (int32 Slot = Slots.Take(); Slot != INDEX_NONE; Slot = Slots.Take()) { Attempts.Add(Slot); }
	}
	TestEqual(TEXT("Half-FPS boundaries preserve all sixty operations in the unchanged window"), Attempts.Num(), 60);
	for (int32 I = 0; I < Attempts.Num(); ++I) { TestEqual(TEXT("Original target-sequence index preserved exactly once"), Attempts[I], I); }
	Slots.Observe(28.914, 30);
	Slots.Observe(31.091, 30, true);
	TestEqual(TEXT("Captured V2 end-boundary gap creates no new V3 release dependency"), Slots.Pending(), 0);
	TestEqual(TEXT("Peak backlog truthfully discloses the whole released workload"), Slots.PeakPending, 60);
	TestEqual(TEXT("Peak batch remains eight"), Slots.PeakBatch, 8);
	TestEqual(TEXT("No admission after capture"), Slots.Take(), INDEX_NONE);

	FOptions Options;
	const TSharedRef<FJsonObject> Report = FDublinRuntimeBenchmark(EBenchmark::Maximal, Options).Report();
	TestEqual(TEXT("New reports explicitly opt into policy three"), Report->GetNumberField(TEXT("maximalSchedulePolicyVersion")), 3.0);
	const TSharedPtr<FJsonObject> Schedule = Report->GetObjectField(TEXT("maximalSchedule"));
	TestEqual(TEXT("Saturation release contract is explicit"),
		Schedule->GetStringField(TEXT("releasePolicy")), FString(TEXT("allAtFirstMeasuredBoundary")));
	TestFalse(TEXT("V3 does not claim a half-second release period"), Schedule->HasField(TEXT("periodSeconds")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinMaximalSaturationDeadlineTest,
	"DublinFlight.Performance.Benchmark.MaximalSaturationBackpressureAndDeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinMaximalSaturationDeadlineTest::RunTest(const FString& Parameters)
{
	FSaturationBenchmarkSlots Slots;
	Slots.Observe(0, 30);
	Slots.Observe(28.914, 30);
	TestEqual(TEXT("Native backpressure retains every released request"), Slots.Pending(), 60);
	for (int32 I = 0; I < 8; ++I) { TestEqual(TEXT("Deferred saturation retries oldest request without skipping"), Slots.Take(), I); }
	Slots.Observe(31.091, 30, true);
	TestEqual(TEXT("Unadmitted requests remain visible at the real ending boundary"), Slots.Pending(), 52);
	TestEqual(TEXT("Deadline never triggers an out-of-measurement flush"), Slots.Take(), INDEX_NONE);
	TestFalse(TEXT("Spectacle never waives unadmitted workload"), BenchmarkPassed(true, true, false, false, false));
	TestFalse(TEXT("Nonfinite time cannot change admission state"), Slots.Observe(std::numeric_limits<double>::quiet_NaN(), 30));
	TestFalse(TEXT("Backward time cannot reset the batch cap"), Slots.Observe(28, 30));
	FSaturationBenchmarkSlots Fractional;
	Fractional.Observe(0, 31.5);
	TestEqual(TEXT("Longer captures retain the pre-existing operation-count formula"), Fractional.Due, 63);
	Fractional.Observe(31.5, 31.5, true);
	TestEqual(TEXT("Exact deadline also forbids admission"), Fractional.Take(), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinBenchmarkStressSightlineTest,
	"DublinFlight.Performance.Benchmark.MaximalPreflightUsesRuntimeSightline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinBenchmarkStressSightlineTest::RunTest(const FString& Parameters)
{
	FDublinImpact Impact;
	Impact.PositionCm = FVector(2571.3, 20344.3, 427.88);
	Impact.RadiusCm = 3374.0478515625f;
	// Captured target osm/way/111845358 is occluded by intact neighbor osm/way/268550898.
	const FVector BlockingHit(2485.176829088405, 18055.174721061994, 2925.6);
	TestFalse(TEXT("The captured obstructed target cannot enter the preflight stress sequence"),
		StressImpactUnoccluded(Impact, true, BlockingHit));
	TestTrue(TEXT("An unobstructed sightline remains eligible"), StressImpactUnoccluded(Impact, false, BlockingHit));
	Impact.PositionCm = FVector::ZeroVector;
	TestTrue(TEXT("A first hit on the impact footprint boundary remains visible"),
		StressImpactUnoccluded(Impact, true, FVector(Impact.RadiusCm, 0, 0)));
	TestFalse(TEXT("Occlusion tolerance is not expanded to hide the captured failure"),
		StressImpactUnoccluded(Impact, true, FVector(Impact.RadiusCm + 1, 0, 0)));
	return true;
}
#endif
