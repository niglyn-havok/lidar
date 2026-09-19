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
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "PipelineStateCache.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDublinFlightPerformance, Log, All);

namespace DublinFlight::Performance
{
	class FPSODiagnosticCapture
	{
	public:
		static constexpr int32 MaxEvents = 512;
		FPSODiagnosticCapture() { Events.Reserve(MaxEvents); }
		struct FState
		{
			uint32 Counters[5] = {};
			uint32 Pending = 0;
			TOptional<int32> ThresholdMs;
			bool bUnhealthy = false;
		};

		static FState Read()
		{
			const FPSORuntimeCreationStats Stats = PipelineStateCache::GetPSORuntimeCreationStats();
			FState State;
			State.Counters[0] = Stats.TotalPSOCreations;
			State.Counters[1] = Stats.ComputePSOHitches;
			State.Counters[2] = Stats.GraphicsPSOHitches;
			State.Counters[3] = Stats.PreviouslyPrecachedPSOHitches;
			State.Counters[4] = Stats.SuspectedUnhealthyDriverCachePSOHitches;
			State.bUnhealthy = Stats.bDriverCacheSuspectedUnhealthy;
			State.Pending = PipelineStateCache::NumActivePrecacheRequests();
			static const IConsoleVariable* Threshold = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSO.RuntimeCreationHitchThreshold"));
			if (Threshold) { State.ThresholdMs = Threshold->GetInt(); }
			return State;
		}

		void Observe(uint64 Frame, double WallSeconds, const FCaptureWindow& Window, bool bEnding, const FState& State)
		{
			++ObservedBoundaries;
			if (!FMath::IsFinite(WallSeconds) || (Previous.IsSet() &&
				(Frame <= Previous->Frame || WallSeconds <= Previous->WallSeconds)))
			{
				++InvalidBoundaries;
				return;
			}
			FBoundary Current{State, Frame, WallSeconds, Window.GetAccumulator().Num(),
				Window.GetAccumulator().GetDurationSeconds(), Window.IsMeasurementBoundary(), bEnding, TOptional<double>()};
			if (!First.IsSet()) { First = Current; }
			PeakPending = FMath::Max(PeakPending, State.Pending);
			bool bHitchChanged = false;
			bool bCounterDecreased = false;
			bool bDiagnosticChanged = false;
			if (Previous.IsSet())
			{
				for (int32 I = 0; I < 5; ++I)
				{
					if (State.Counters[I] >= Previous->State.Counters[I])
					{
						CounterDeltaTotals[I] += State.Counters[I] - Previous->State.Counters[I];
					}
					else { bCounterDecreased = true; }
					bHitchChanged |= I > 0 && State.Counters[I] != Previous->State.Counters[I];
				}
				if (bCounterDecreased) { ++CounterDecreaseBoundaries; }
				bDiagnosticChanged = State.ThresholdMs != Previous->State.ThresholdMs || State.bUnhealthy != Previous->State.bUnhealthy;
				if (Current.MeasuredFrames == Previous->MeasuredFrames + 1)
				{
					Current.CompletedFrameSeconds = Window.GetAccumulator().GetSamplesSeconds().Last();
				}
			}
			if (Current.bMeasurement)
			{
				if (!MeasurementStart.IsSet()) { MeasurementStart = Current; }
				MeasurementEnd = Current;
			}
			const bool bOverBudget = Current.CompletedFrameSeconds.IsSet() && Current.CompletedFrameSeconds.GetValue() > FrameBudgetSeconds;
			if (bOverBudget || bHitchChanged || bCounterDecreased || bDiagnosticChanged)
			{
				if (Events.Num() < MaxEvents) { Events.Add({Current, Previous}); }
				else { ++DroppedEvents; }
			}
			else
			{
				++SummarizedBoundaries;
				if (Previous.IsSet() && State.Counters[0] > Previous->State.Counters[0])
				{
					++SummarizedCreationChangeBoundaries;
					SummarizedCreationDelta += State.Counters[0] - Previous->State.Counters[0];
				}
			}
			Previous = Current;
		}

		TSharedRef<FJsonObject> Report() const
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("schemaVersion"), 2);
			Out->SetNumberField(TEXT("maxEvents"), MaxEvents);
			Out->SetNumberField(TEXT("observedBoundaries"), ObservedBoundaries);
			Out->SetNumberField(TEXT("retainedEvents"), Events.Num());
			Out->SetNumberField(TEXT("droppedEvents"), DroppedEvents);
			Out->SetNumberField(TEXT("priorityEventsDropped"), DroppedEvents);
			Out->SetNumberField(TEXT("priorityEventsObserved"), Events.Num() + DroppedEvents);
			Out->SetNumberField(TEXT("summarizedBoundaries"), SummarizedBoundaries);
			Out->SetNumberField(TEXT("summarizedCreationChangeBoundaries"), SummarizedCreationChangeBoundaries);
			Out->SetNumberField(TEXT("summarizedCreationDelta"), static_cast<double>(SummarizedCreationDelta));
			Out->SetNumberField(TEXT("peakPendingPrecacheRequests"), PeakPending);
			Out->SetNumberField(TEXT("invalidBoundaries"), InvalidBoundaries);
			Out->SetNumberField(TEXT("counterDecreaseBoundaries"), CounterDecreaseBoundaries);
			Out->SetBoolField(TEXT("truncated"), DroppedEvents > 0);
			Out->SetStringField(TEXT("sampling"),
				TEXT("Observe every shared OnBeginFrame boundary. Pin first/last and measurementStart/measurementEnd outside the512-event array. Retain measured raw frames >1/60s, hitch-counter changes, counter decreases and diagnostic threshold/health changes. Creation-only, pending-only and quiet boundaries are intentionally summarized. Deltas reference the previous OBSERVED boundary."));
			Out->SetStringField(TEXT("accounting"),
				TEXT("observedBoundaries = invalidBoundaries + summarizedBoundaries + retainedEvents + priorityEventsDropped. droppedEvents aliases priorityEventsDropped; intentional summaries are not truncation. counterDeltaTotals sums nonnegative observed deltas, including summarized and overflow boundaries; decreases remain explicitly flagged."));
			Out->SetStringField(TEXT("retentionThresholdPolicy"),
				TEXT("Raw strict >1/60s selects diagnostic snapshots even for a30FPS scenario. This reads unchanged frame samples and does not alter either native FPS threshold or its acceptance tolerance."));
			Out->SetStringField(TEXT("attribution"),
				TEXT("Process-global relaxed atomic graphics/compute runtime-creation counters, not GPU times or exact hitch durations. Excludes background precaching and does not characterize all ray-tracing work. Counter decreases yield null deltas, never unsigned wrap. Pending count can decrease normally. No reset, threshold change, wait or benchmark gate modification."));
			Out->SetStringField(TEXT("coverage"),
				TEXT("Starts at capture clock priming, not engine startup. Missing per-frame rows do not imply unchanged total creations or pending counts. MeasurementEnd is the last observed measurement boundary; endingBoundary says whether it was terminal. No invented final partial frame."));
			const TSharedRef<FJsonObject> Totals = MakeShared<FJsonObject>();
			for (int32 I = 0; I < 5; ++I) { Totals->SetNumberField(CounterNames[I], static_cast<double>(CounterDeltaTotals[I])); }
			Out->SetObjectField(TEXT("counterDeltaTotals"), Totals);
			if (First.IsSet()) { Out->SetObjectField(TEXT("first"), WriteBoundary(First.GetValue())); }
			else { Out->SetField(TEXT("first"), MakeShared<FJsonValueNull>()); }
			if (Previous.IsSet()) { Out->SetObjectField(TEXT("last"), WriteBoundary(Previous.GetValue())); }
			else { Out->SetField(TEXT("last"), MakeShared<FJsonValueNull>()); }
			if (MeasurementStart.IsSet()) { Out->SetObjectField(TEXT("measurementStart"), WriteBoundary(MeasurementStart.GetValue())); }
			else { Out->SetField(TEXT("measurementStart"), MakeShared<FJsonValueNull>()); }
			if (MeasurementEnd.IsSet()) { Out->SetObjectField(TEXT("measurementEnd"), WriteBoundary(MeasurementEnd.GetValue())); }
			else { Out->SetField(TEXT("measurementEnd"), MakeShared<FJsonValueNull>()); }
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FEvent& Event : Events)
			{
				const TSharedRef<FJsonObject> Row = WriteBoundary(Event.Current);
				const TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
				bool bHitchChanged = false;
				bool bCounterDecreased = false;
				for (int32 I = 0; I < 5; ++I)
				{
					if (Event.Before.IsSet() && Event.Current.State.Counters[I] >= Event.Before->State.Counters[I])
					{
						Delta->SetNumberField(CounterNames[I], Event.Current.State.Counters[I] - Event.Before->State.Counters[I]);
					}
					else { Delta->SetField(CounterNames[I], MakeShared<FJsonValueNull>()); }
					if (Event.Before.IsSet())
					{
						bHitchChanged |= I > 0 && Event.Current.State.Counters[I] != Event.Before->State.Counters[I];
						bCounterDecreased |= Event.Current.State.Counters[I] < Event.Before->State.Counters[I];
					}
				}
				Row->SetObjectField(TEXT("counterDeltas"), Delta);
				Row->SetBoolField(TEXT("hitchCountersChanged"), bHitchChanged);
				Row->SetBoolField(TEXT("counterDecreased"), bCounterDecreased);
				Row->SetBoolField(TEXT("hitchThresholdChanged"), Event.Before.IsSet() && Event.Current.State.ThresholdMs != Event.Before->State.ThresholdMs);
				Row->SetBoolField(TEXT("driverCacheHealthChanged"), Event.Before.IsSet() && Event.Current.State.bUnhealthy != Event.Before->State.bUnhealthy);
				for (const TCHAR* Name : { TEXT("previousEngineFrame"), TEXT("previousSharedWallSeconds"),
					TEXT("pendingPrecacheDelta"), TEXT("completedMeasuredFrameIndex") })
				{
					Row->SetField(Name, MakeShared<FJsonValueNull>());
				}
				if (Event.Before.IsSet())
				{
					Row->SetNumberField(TEXT("previousEngineFrame"), static_cast<double>(Event.Before->Frame));
					Row->SetNumberField(TEXT("previousSharedWallSeconds"), Event.Before->WallSeconds);
					Row->SetNumberField(TEXT("pendingPrecacheDelta"),
						static_cast<double>(static_cast<int64>(Event.Current.State.Pending) - static_cast<int64>(Event.Before->State.Pending)));
					if (Event.Current.MeasuredFrames == Event.Before->MeasuredFrames + 1)
					{
						Row->SetNumberField(TEXT("completedMeasuredFrameIndex"), Event.Current.MeasuredFrames - 1);
					}
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Out->SetArrayField(TEXT("events"), Rows);
			return Out;
		}

	private:
		struct FBoundary
		{
			FState State;
			uint64 Frame;
			double WallSeconds;
			int32 MeasuredFrames;
			double MeasuredSeconds;
			bool bMeasurement;
			bool bEnding;
			TOptional<double> CompletedFrameSeconds;
		};
		struct FEvent { FBoundary Current; TOptional<FBoundary> Before; };
		inline static const TCHAR* const CounterNames[5] = { TEXT("totalPSOCreations"), TEXT("computePSOHitches"),
			TEXT("graphicsPSOHitches"), TEXT("previouslyPrecachedPSOHitches"), TEXT("suspectedUnhealthyDriverCachePSOHitches") };
		TOptional<FBoundary> First;
		TOptional<FBoundary> Previous;
		TOptional<FBoundary> MeasurementStart;
		TOptional<FBoundary> MeasurementEnd;
		TArray<FEvent> Events;
		int32 ObservedBoundaries = 0;
		int32 DroppedEvents = 0;
		int32 InvalidBoundaries = 0;
		int32 CounterDecreaseBoundaries = 0;
		int32 SummarizedBoundaries = 0;
		int32 SummarizedCreationChangeBoundaries = 0;
		uint64 SummarizedCreationDelta = 0;
		uint64 CounterDeltaTotals[5] = {};
		uint32 PeakPending = 0;

		static TSharedRef<FJsonObject> WriteBoundary(const FBoundary& Boundary)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("engineFrame"), static_cast<double>(Boundary.Frame));
			Out->SetNumberField(TEXT("sharedWallSeconds"), Boundary.WallSeconds);
			Out->SetNumberField(TEXT("measuredFrameCount"), Boundary.MeasuredFrames);
			Out->SetNumberField(TEXT("measuredSeconds"), Boundary.MeasuredSeconds);
			Out->SetBoolField(TEXT("measurementBoundary"), Boundary.bMeasurement);
			Out->SetBoolField(TEXT("endingBoundary"), Boundary.bEnding);
			if (Boundary.CompletedFrameSeconds.IsSet())
			{
				const double Seconds = Boundary.CompletedFrameSeconds.GetValue();
				Out->SetNumberField(TEXT("completedFrameSeconds"), Seconds);
				Out->SetBoolField(TEXT("over60FPSBudget"), Seconds > FrameBudgetSeconds);
				Out->SetBoolField(TEXT("over30FPSBudget"), Seconds > FrameBudget30Seconds);
			}
			else
			{
				Out->SetField(TEXT("completedFrameSeconds"), MakeShared<FJsonValueNull>());
				Out->SetField(TEXT("over60FPSBudget"), MakeShared<FJsonValueNull>());
				Out->SetField(TEXT("over30FPSBudget"), MakeShared<FJsonValueNull>());
			}
			const TSharedRef<FJsonObject> Counters = MakeShared<FJsonObject>();
			for (int32 I = 0; I < 5; ++I) { Counters->SetNumberField(CounterNames[I], Boundary.State.Counters[I]); }
			Out->SetObjectField(TEXT("counters"), Counters);
			Out->SetNumberField(TEXT("pendingPrecacheRequests"), Boundary.State.Pending);
			Out->SetBoolField(TEXT("driverCacheSuspectedUnhealthy"), Boundary.State.bUnhealthy);
			if (Boundary.State.ThresholdMs.IsSet())
			{
				Out->SetNumberField(TEXT("runtimeCreationHitchThresholdMs"), Boundary.State.ThresholdMs.GetValue());
			}
			else { Out->SetField(TEXT("runtimeCreationHitchThresholdMs"), MakeShared<FJsonValueNull>()); }
			return Out;
		}
	};

#if WITH_DEV_AUTOMATION_TESTS
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinPSODiagnosticDeltaTest,
		"DublinFlight.Performance.Diagnostics.PSOBoundaryDeltas",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FDublinPSODiagnosticDeltaTest::RunTest(const FString& Parameters)
	{
		FOptions Options;
		Options.WarmupSeconds = 1;
		Options.DurationSeconds = 1;
		FCaptureWindow Window;
		FString Error;
		if (!TestTrue(TEXT("Diagnostic clock fixture"), Window.Start(Options, Error))) { return false; }
		FPSODiagnosticCapture Capture;
		FPSODiagnosticCapture::FState State;
		State.Counters[0] = 10;
		State.Pending = 5;
		State.ThresholdMs = 20;
		Window.AddBoundary(100, Error); Capture.Observe(10, 100, Window, false, State);
		State.Counters[0] = 12;
		State.Counters[1] = 1;
		State.Pending = 3;
		Window.AddBoundary(100.5, Error); Capture.Observe(11, 100.5, Window, false, State);
		Window.AddBoundary(101, Error); Capture.Observe(12, 101, Window, false, State);
		State.Counters[0] = 13;
		State.Counters[2] = 1;
		State.Pending = 1;
		Window.AddBoundary(101.005, Error); Capture.Observe(13, 101.005, Window, false, State);
		Window.AddBoundary(101.010, Error); Capture.Observe(14, 101.010, Window, false, State);
		Window.AddBoundary(102, Error); Capture.Observe(15, 102, Window, true, State);
		const TSharedRef<FJsonObject> Report = Capture.Report();
		const auto& Events = Report->GetArrayField(TEXT("events"));
		if (!TestEqual(TEXT("Only hitch changes and measured budget crossings consume events"), Events.Num(), 3)) { return false; }
		TestEqual(TEXT("Every frame boundary observed"), Report->GetNumberField(TEXT("observedBoundaries")), 6.0);
		TestEqual(TEXT("Existing hitch threshold is recorded"), Report->GetObjectField(TEXT("first"))->GetNumberField(TEXT("runtimeCreationHitchThresholdMs")), 20.0);
		TestEqual(TEXT("Warmup runtime-creation delta"), Events[0]->AsObject()->GetObjectField(TEXT("counterDeltas"))->GetNumberField(TEXT("totalPSOCreations")), 2.0);
		TestEqual(TEXT("Pending count may decrease"), Events[0]->AsObject()->GetNumberField(TEXT("pendingPrecacheDelta")), -2.0);
		TestEqual(TEXT("Measurement transition pinned outside event budget"), Report->GetObjectField(TEXT("measurementStart"))->GetNumberField(TEXT("measuredFrameCount")), 0.0);
		TestTrue(TEXT("Measurement end pinned independently"), Report->GetObjectField(TEXT("measurementEnd"))->GetBoolField(TEXT("endingBoundary")));
		TestTrue(TEXT("Warmup event is not measured frame zero"), Events[0]->AsObject()->GetField<EJson::Null>(TEXT("completedMeasuredFrameIndex")).IsValid());
		TestEqual(TEXT("Measured delta addresses raw frame zero"), Events[1]->AsObject()->GetNumberField(TEXT("completedMeasuredFrameIndex")), 0.0);
		TestEqual(TEXT("Measured graphics hitch delta is recorded"), Events[1]->AsObject()->GetObjectField(TEXT("counterDeltas"))->GetNumberField(TEXT("graphicsPSOHitches")), 1.0);
		TestFalse(TEXT("Hitch changes retained even on an under-budget frame"), Events[1]->AsObject()->GetBoolField(TEXT("over60FPSBudget")));
		TestEqual(TEXT("End delta uses last observed, not last retained boundary"), Events[2]->AsObject()->GetNumberField(TEXT("previousEngineFrame")), 14.0);
		TestEqual(TEXT("End addresses raw frame two"), Events[2]->AsObject()->GetNumberField(TEXT("completedMeasuredFrameIndex")), 2.0);
		TestEqual(TEXT("Non-priority boundaries are intentional summaries"), Report->GetNumberField(TEXT("summarizedBoundaries")), 3.0);
		TestEqual(TEXT("All boundaries contribute counter totals"), Report->GetObjectField(TEXT("counterDeltaTotals"))->GetNumberField(TEXT("totalPSOCreations")), 3.0);
		TestEqual(TEXT("Peak pending requests includes first boundary"), Report->GetNumberField(TEXT("peakPendingPrecacheRequests")), 5.0);
		TestEqual(TEXT("Raw frame count unchanged by diagnostics"), Window.GetAccumulator().Num(), 3);
		TestEqual(TEXT("Raw capture duration unchanged"), Window.GetAccumulator().GetDurationSeconds(), 1.0);
		TestFalse(TEXT("Small sample is not truncated"), Report->GetBoolField(TEXT("truncated")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinPSODiagnosticBoundTest,
		"DublinFlight.Performance.Diagnostics.PSOShapeBoundsAndCounterDecrease",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FDublinPSODiagnosticBoundTest::RunTest(const FString& Parameters)
	{
		FCaptureWindow Window;
		FOptions Options;
		Options.WarmupSeconds = 0;
		FString Error;
		if (!TestTrue(TEXT("Bounded diagnostic fixture"), Window.Start(Options, Error))) { return false; }
		FPSODiagnosticCapture Capture;
		FPSODiagnosticCapture::FState State;
		for (int32 I = 0; I < FPSODiagnosticCapture::MaxEvents + 6; ++I)
		{
			const double Now = 100 + I * 0.020;
			State.Counters[0] = I;
			Window.AddBoundary(Now, Error);
			Capture.Observe(I, Now, Window, I == FPSODiagnosticCapture::MaxEvents + 5, State);
		}
		const TSharedRef<FJsonObject> Report = Capture.Report();
		TestEqual(TEXT("Event storage has a fixed bound"), Report->GetArrayField(TEXT("events")).Num(), FPSODiagnosticCapture::MaxEvents);
		TestEqual(TEXT("Priority overflow is distinct from initial summary"), Report->GetNumberField(TEXT("priorityEventsDropped")), 5.0);
		TestEqual(TEXT("Legacy drop field also reports actual overflow"), Report->GetNumberField(TEXT("droppedEvents")), 5.0);
		TestEqual(TEXT("Priority event count includes overflow"), Report->GetNumberField(TEXT("priorityEventsObserved")), double(FPSODiagnosticCapture::MaxEvents + 5));
		TestEqual(TEXT("Initial boundary consumes no event slot"), Report->GetNumberField(TEXT("summarizedBoundaries")), 1.0);
		TestTrue(TEXT("Truncation is explicit"), Report->GetBoolField(TEXT("truncated")));
		TestEqual(TEXT("Last snapshot survives truncation"), Report->GetObjectField(TEXT("last"))->GetObjectField(TEXT("counters"))->GetNumberField(TEXT("totalPSOCreations")), double(FPSODiagnosticCapture::MaxEvents + 5));
		TestTrue(TEXT("Missing threshold is null, not invented"), Report->GetObjectField(TEXT("last"))->GetField<EJson::Null>(TEXT("runtimeCreationHitchThresholdMs")).IsValid());
		TestTrue(TEXT("Measurement end survives overflow"), Report->GetObjectField(TEXT("measurementEnd"))->GetBoolField(TEXT("endingBoundary")));
		TestEqual(TEXT("Overflow does not lose counter totals"), Report->GetObjectField(TEXT("counterDeltaTotals"))->GetNumberField(TEXT("totalPSOCreations")), double(FPSODiagnosticCapture::MaxEvents + 5));
		TestEqual(TEXT("Boundary accounting balances"), Report->GetNumberField(TEXT("observedBoundaries")),
			Report->GetNumberField(TEXT("invalidBoundaries")) + Report->GetNumberField(TEXT("summarizedBoundaries")) +
			Report->GetNumberField(TEXT("retainedEvents")) + Report->GetNumberField(TEXT("priorityEventsDropped")));

		FPSODiagnosticCapture Decrease;
		State.Counters[0] = 10;
		Decrease.Observe(1, 1, Window, false, State);
		State.Counters[0] = 2;
		Decrease.Observe(2, 2, Window, false, State);
		Decrease.Observe(2, 2, Window, false, State);
		const TSharedRef<FJsonObject> ResetReport = Decrease.Report();
		TestEqual(TEXT("Counter decrease is disclosed"), ResetReport->GetNumberField(TEXT("counterDecreaseBoundaries")), 1.0);
		TestEqual(TEXT("Duplicate boundary is disclosed"), ResetReport->GetNumberField(TEXT("invalidBoundaries")), 1.0);
		TestTrue(TEXT("Counter decrease never wraps unsigned"),
			ResetReport->GetArrayField(TEXT("events"))[0]->AsObject()->GetObjectField(TEXT("counterDeltas"))->GetField<EJson::Null>(TEXT("totalPSOCreations")).IsValid());
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinPSODiagnosticWarmupTest,
		"DublinFlight.Performance.Diagnostics.PSOWarmupChurnPreservesMeasurement",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FDublinPSODiagnosticWarmupTest::RunTest(const FString& Parameters)
	{
		FOptions Options;
		Options.WarmupSeconds = 6;
		Options.DurationSeconds = 1;
		Options.ScenarioTargetMinimumFPS = 30;
		FCaptureWindow Window;
		FString Error;
		if (!TestTrue(TEXT("Warmup churn fixture"), Window.Start(Options, Error))) { return false; }
		FPSODiagnosticCapture Capture;
		FPSODiagnosticCapture::FState State;
		State.ThresholdMs = 20;
		Window.AddBoundary(1000, Error); Capture.Observe(0, 1000, Window, false, State);
		constexpr int32 WarmupChanges = FPSODiagnosticCapture::MaxEvents + 100;
		for (int32 I = 1; I <= WarmupChanges; ++I)
		{
			State.Counters[0] = I;
			const double Now = 1000 + I * 0.005;
			Window.AddBoundary(Now, Error); Capture.Observe(I, Now, Window, false, State);
		}
		TestEqual(TEXT("More than512 warmup changes consume no event slots"), Capture.Report()->GetArrayField(TEXT("events")).Num(), 0);
		++State.Counters[0];
		Window.AddBoundary(1006, Error); Capture.Observe(WarmupChanges + 1, 1006, Window, false, State);
		++State.Counters[0];
		Window.AddBoundary(1006.025, Error); Capture.Observe(WarmupChanges + 2, 1006.025, Window, false, State);
		++State.Counters[0];
		Window.AddBoundary(1006.030, Error); Capture.Observe(WarmupChanges + 3, 1006.030, Window, false, State);
		++State.Counters[0];
		++State.Counters[1];
		Window.AddBoundary(1006.035, Error); Capture.Observe(WarmupChanges + 4, 1006.035, Window, false, State);
		Window.AddBoundary(1007, Error); Capture.Observe(WarmupChanges + 5, 1007, Window, true, State);
		const TSharedRef<FJsonObject> Report = Capture.Report();
		const auto& Events = Report->GetArrayField(TEXT("events"));
		if (!TestEqual(TEXT("Measured slow frame, hitch change and slow final frame retained"), Events.Num(), 3)) { return false; }
		TestEqual(TEXT("First measured over-budget frame retained"), Events[0]->AsObject()->GetNumberField(TEXT("completedMeasuredFrameIndex")), 0.0);
		TestTrue(TEXT("Raw60FPS miss retained even in30FPS scenario"), Events[0]->AsObject()->GetBoolField(TEXT("over60FPSBudget")));
		TestFalse(TEXT("Diagnostic retention does not redefine30FPS miss"), Events[0]->AsObject()->GetBoolField(TEXT("over30FPSBudget")));
		TestEqual(TEXT("Later measured hitch change retained"), Events[1]->AsObject()->GetNumberField(TEXT("completedMeasuredFrameIndex")), 2.0);
		TestEqual(TEXT("Hitch delta is local to previous observed frame"), Events[1]->AsObject()->GetObjectField(TEXT("counterDeltas"))->GetNumberField(TEXT("computePSOHitches")), 1.0);
		TestEqual(TEXT("Under-budget creation-only frame was summarized"), Events[1]->AsObject()->GetObjectField(TEXT("counterDeltas"))->GetNumberField(TEXT("totalPSOCreations")), 1.0);
		TestEqual(TEXT("Intentional summaries are counted"), Report->GetNumberField(TEXT("summarizedCreationChangeBoundaries")), double(WarmupChanges + 2));
		TestEqual(TEXT("All creation deltas survive summaries"), Report->GetObjectField(TEXT("counterDeltaTotals"))->GetNumberField(TEXT("totalPSOCreations")), double(WarmupChanges + 4));
		TestEqual(TEXT("No actual overflow occurred"), Report->GetNumberField(TEXT("priorityEventsDropped")), 0.0);
		TestFalse(TEXT("Summary is not truncation"), Report->GetBoolField(TEXT("truncated")));
		TestEqual(TEXT("Measurement start pinned"), Report->GetObjectField(TEXT("measurementStart"))->GetNumberField(TEXT("measuredFrameCount")), 0.0);
		TestTrue(TEXT("Measurement end pinned"), Report->GetObjectField(TEXT("measurementEnd"))->GetBoolField(TEXT("endingBoundary")));
		TestEqual(TEXT("Native raw frame count unchanged"), Window.GetAccumulator().Num(), 4);
		TestEqual(TEXT("Native measured duration unchanged"), Window.GetAccumulator().GetDurationSeconds(), 1.0);
		return true;
	}
#endif
}

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

	bool ValidStartupRunId(const FString& RunId)
	{
		if (RunId.Len() != 32) { return false; }
		for (TCHAR C : RunId)
		{
			if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'))
				|| (C >= TEXT('A') && C <= TEXT('F')))) { return false; }
		}
		FGuid Guid;
		return FGuid::ParseExact(RunId, EGuidFormats::Digits, Guid) && Guid.IsValid();
	}

	bool ValidStartupIdentity(const FStartupBenchmarkRequest& Request)
	{
		return Request.bRequested && ValidStartupRunId(Request.RunId)
			&& (Request.Scenario == TEXT("normal") || Request.Scenario == TEXT("light") || Request.Scenario == TEXT("maximal"));
	}

	bool SupportsStartupBenchmark(const UWorld& World)
	{
		const EBuildConfiguration Configuration = FApp::GetBuildConfiguration();
		return !GIsEditor && !IsRunningCommandlet() && !IsRunningDedicatedServer()
			&& FPlatformProperties::RequiresCookedData() && World.WorldType == EWorldType::Game
			&& World.GetNetMode() == NM_Standalone
			&& (Configuration == EBuildConfiguration::Development || Configuration == EBuildConfiguration::Shipping);
	}

	void AddStartupIdentity(const TSharedRef<FJsonObject>& Report, const FStartupBenchmarkRequest& Request)
	{
		Report->SetStringField(TEXT("runId"), Request.RunId.ToLower());
		Report->SetNumberField(TEXT("processId"), FPlatformProcess::GetCurrentProcessId());
		Report->SetStringField(TEXT("configuration"), LexToString(FApp::GetBuildConfiguration()));
	}

	bool ValidateStartupDirectoryPath(const FString& Directory, FString& OutDirectory, FString& OutError)
	{
		FString Path = Directory;
		OutDirectory.Reset();
		FPaths::NormalizeDirectoryName(Path);
		if (Path.IsEmpty() || FPaths::IsRelative(Path) || FPaths::GetCleanFilename(Path).IsEmpty())
		{
			OutError = TEXT("Benchmark output must name an absolute, non-root directory.");
			return false;
		}
#if PLATFORM_WINDOWS
		const bool bDriveAbsolute = Path.Len() > 3 && FChar::IsAlpha(Path[0])
			&& Path[1] == TEXT(':') && Path[2] == TEXT('/');
		if (!bDriveAbsolute && !Path.StartsWith(TEXT("//")))
		{
			OutError = TEXT("Benchmark output must be fully qualified, not drive-relative.");
			return false;
		}
#endif
		for (int32 Index = 0; Index < Path.Len(); ++Index)
		{
			const TCHAR C = Path[Index];
			if (C < 32 || C == TEXT('"') || C == TEXT('*') || C == TEXT('?') || C == TEXT('|')
				|| C == TEXT('<') || C == TEXT('>') || (C == TEXT(':') && Index != 1))
			{
				OutError = TEXT("Benchmark output contains a forbidden path character.");
				return false;
			}
		}
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"), true);
		for (const FString& Part : Parts)
		{
			if (Part == TEXT(".") || Part == TEXT("..") || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" ")))
			{
				OutError = TEXT("Benchmark output must not contain traversal or ambiguous path components.");
				return false;
			}
		}
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (FString Parent = Path; !Parent.IsEmpty();)
		{
			if (PlatformFile.FileExists(*Parent))
			{
				OutError = TEXT("Benchmark output or an ancestor is a file.");
				return false;
			}
			if (PlatformFile.DirectoryExists(*Parent) && PlatformFile.IsSymlink(*Parent) != ESymlinkResult::NonSymlink)
			{
				OutError = TEXT("Benchmark output must not traverse unverified links or reparse points.");
				return false;
			}
			FString Next = FPaths::GetPath(Parent);
#if PLATFORM_WINDOWS
			if (Next.Len() == 2 && Next[1] == TEXT(':')) { Next += TEXT("/"); }
#endif
			if (Next == Parent) { break; }
			Parent = Next;
		}
		OutDirectory = MoveTemp(Path);
		return true;
	}

	bool OwnsStartupDirectory(const FString& Directory, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString OwnerPath = FPaths::Combine(Directory, TEXT("owner.json"));
		const int64 Size = PlatformFile.FileSize(*OwnerPath);
		FString Text;
		TSharedPtr<FJsonObject> Owner;
		double ProcessId = 0;
		if (Size <= 0 || Size > 1024 || PlatformFile.IsSymlink(*OwnerPath) != ESymlinkResult::NonSymlink
			|| !FFileHelper::LoadFileToString(Text, *OwnerPath)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Owner) || !Owner.IsValid()
			|| !Owner->TryGetNumberField(TEXT("processId"), ProcessId)
			|| ProcessId != FPlatformProcess::GetCurrentProcessId())
		{
			OutError = TEXT("Benchmark output directory ownership is not valid for this process.");
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> ReadForegroundDiagnostics(UWorld* World)
	{
		const TSharedRef<FJsonObject> Diagnostics = MakeShared<FJsonObject>();
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		const FSceneViewport* Scene = Client ? Client->GetGameViewport() : nullptr;
		const bool bWidgetPresent = Scene && Scene->GetViewportWidget().IsValid();
		const TSharedPtr<SWindow> SceneWindow = Scene ? Scene->FindWindow() : TSharedPtr<SWindow>();
		const TSharedPtr<SWindow> ClientWindow = Client ? Client->GetWindow() : TSharedPtr<SWindow>();
		const TSharedPtr<FGenericWindow> SceneNative = SceneWindow.IsValid()
			? SceneWindow->GetNativeWindow() : TSharedPtr<FGenericWindow>();
		const TSharedPtr<FGenericWindow> ClientNative = ClientWindow.IsValid()
			? ClientWindow->GetNativeWindow() : TSharedPtr<FGenericWindow>();
		Diagnostics->SetBoolField(TEXT("applicationForeground"), FPlatformApplicationMisc::IsThisApplicationForeground());
		Diagnostics->SetBoolField(TEXT("sceneViewportPresent"), Scene != nullptr);
		Diagnostics->SetBoolField(TEXT("viewportWidgetPresent"), bWidgetPresent);
		Diagnostics->SetBoolField(TEXT("sceneNativeWindowPresent"), SceneNative.IsValid());
		Diagnostics->SetBoolField(TEXT("clientNativeWindowPresent"), ClientNative.IsValid());
		for (const TCHAR* Name : { TEXT("clientNativeWindowForeground"),
			TEXT("sceneNativeWindowEqualsClientNativeWindow"), TEXT("mouseCaptured") })
		{
			Diagnostics->SetField(Name, MakeShared<FJsonValueNull>());
		}
		if (ClientNative.IsValid())
		{
			Diagnostics->SetBoolField(TEXT("clientNativeWindowForeground"), ClientNative->IsForegroundWindow());
		}
		if (SceneNative.IsValid() && ClientNative.IsValid())
		{
			Diagnostics->SetBoolField(TEXT("sceneNativeWindowEqualsClientNativeWindow"), SceneNative == ClientNative);
		}
		if (bWidgetPresent) { Diagnostics->SetBoolField(TEXT("mouseCaptured"), Scene->HasMouseCapture()); }
		return Diagnostics;
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
		Scenario->SetBoolField(TEXT("reservedResourcesSupported"), GRHIGlobals.ReservedResources.Supported);

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
			TEXT("r.Lumen.DiffuseIndirect.Allow"), TEXT("r.Lumen.Reflections.Allow"),
			TEXT("r.Nanite.Streaming.ReservedResources"), TEXT("r.Nanite.Streaming.StreamingPoolSize"),
			TEXT("r.Nanite.Streaming.NumInitialRootPages"), TEXT("r.Nanite.Streaming.DynamicallyGrowAllocations"),
			TEXT("r.Nanite.Streaming.Debug.ReservedResourceIgnoreInitialRootAllocation"),
			TEXT("r.Nanite.Streaming.Debug.ReservedResourceRootPageGrowOnly"),
			TEXT("r.Shadow.Virtual.AllocatePagePoolAsReservedResource"), TEXT("r.Shadow.Virtual.MaxPhysicalPages"),
			TEXT("d3d12.ReservedResourceHeapSizeMB"), TEXT("r.NumBufferedOcclusionQueries")
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

#if WITH_DEV_AUTOMATION_TESTS
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinAllocatorAuditTest,
		"DublinFlight.Performance.Audit.AllocatorSettings",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FDublinAllocatorAuditTest::RunTest(const FString& Parameters)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Allocator audit fixture world"), World)) { return false; }
		ON_SCOPE_EXIT { World->DestroyWorld(false); };
		if (!TestNotNull(TEXT("Allocator audit world settings"), World->GetWorldSettings())) { return false; }
		const TSharedRef<FJsonObject> Scenario = ReadScenario(*World);
		const TSharedPtr<FJsonValue> Support = Scenario->TryGetField(TEXT("reservedResourcesSupported"));
		if (TestTrue(TEXT("RHI support field is a boolean"), Support.IsValid() && Support->Type == EJson::Boolean))
		{
			TestEqual(TEXT("RHI reserved-resource support is disclosed"),
				Support->AsBool(), GRHIGlobals.ReservedResources.Supported);
		}
		const TSharedPtr<FJsonObject> CVars = Scenario->GetObjectField(TEXT("qualityAndTimingCVars"));
		for (const TCHAR* Name : {
			TEXT("r.Nanite.Streaming.ReservedResources"), TEXT("r.Nanite.Streaming.StreamingPoolSize"),
			TEXT("r.Nanite.Streaming.NumInitialRootPages"), TEXT("r.Nanite.Streaming.DynamicallyGrowAllocations"),
			TEXT("r.Nanite.Streaming.Debug.ReservedResourceIgnoreInitialRootAllocation"),
			TEXT("r.Nanite.Streaming.Debug.ReservedResourceRootPageGrowOnly"),
			TEXT("r.Shadow.Virtual.AllocatePagePoolAsReservedResource"), TEXT("r.Shadow.Virtual.MaxPhysicalPages"),
			TEXT("d3d12.ReservedResourceHeapSizeMB"), TEXT("r.NumBufferedOcclusionQueries") })
		{
			const TSharedPtr<FJsonValue> Value = CVars->TryGetField(Name);
			if (!TestTrue(FString::Printf(TEXT("Allocator field present: %s"), Name), Value.IsValid())) { continue; }
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			if (!Variable || !FMath::IsFinite(Variable->GetFloat()))
			{
				TestTrue(FString::Printf(TEXT("Unavailable allocator value is null: %s"), Name), Value->Type == EJson::Null);
			}
			else if (TestTrue(FString::Printf(TEXT("Allocator value is numeric: %s"), Name), Value->Type == EJson::Number))
			{
				TestEqual(FString::Printf(TEXT("Actual allocator value recorded: %s"), Name),
					Value->AsNumber(), static_cast<double>(Variable->GetFloat()));
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinForegroundDiagnosticsTest,
		"DublinFlight.Performance.Audit.ForegroundDiagnostics",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FDublinForegroundDiagnosticsTest::RunTest(const FString& Parameters)
	{
		const TSharedRef<FJsonObject> Diagnostics = ReadForegroundDiagnostics(nullptr);
		TestEqual(TEXT("Only the bounded diagnostic fields are emitted"), Diagnostics->Values.Num(), 8);
		for (const auto& Field : Diagnostics->Values)
		{
			TestTrue(TEXT("Diagnostics cannot expose identifiers, handles, titles or nested data"),
				Field.Value.IsValid() && (Field.Value->Type == EJson::Boolean || Field.Value->Type == EJson::Null));
		}
		for (const TCHAR* Name : { TEXT("sceneViewportPresent"), TEXT("viewportWidgetPresent"),
			TEXT("sceneNativeWindowPresent"), TEXT("clientNativeWindowPresent") })
		{
			TestFalse(TEXT("Missing viewport/window infrastructure is reported absent"), Diagnostics->GetBoolField(Name));
		}
		for (const TCHAR* Name : { TEXT("clientNativeWindowForeground"),
			TEXT("sceneNativeWindowEqualsClientNativeWindow"), TEXT("mouseCaptured") })
		{
			const TSharedPtr<FJsonValue> Value = Diagnostics->TryGetField(Name);
			TestTrue(TEXT("Unavailable observations remain null, never fallback success"),
				Value.IsValid() && Value->Type == EJson::Null);
		}
		return true;
	}
#endif

	bool WriteDurableReport(const TSharedRef<FJsonObject>& Report, const FString& FileName,
		const FString& CaptureId, FString& OutError, bool bBenchmark = false,
		const FString& DirectoryOverride = FString())
	{
		FString Serialized;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		if (!FJsonSerializer::Serialize(Report, Writer))
		{
			OutError = TEXT("Could not serialize performance JSON.");
			return false;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		FString Directory = bBenchmark ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Performance"))
			: FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling"), TEXT("DublinFlight"));
		if (!DirectoryOverride.IsEmpty())
		{
			if (!IsSafeBenchmarkReportName(FileName)
				|| !ValidateStartupDirectoryPath(DirectoryOverride, Directory, OutError))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("Benchmark report must be a safe relative JSON basename."); }
				return false;
			}
			if (FileName != TEXT("owner.json") && !OwnsStartupDirectory(Directory, OutError)) { return false; }
		}
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

#if !UE_BUILD_SHIPPING
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

namespace DublinFlight::Performance
{
	bool ParseStartupBenchmarkRequest(const FString& CommandLine, FStartupBenchmarkRequest& OutRequest, FString& OutError)
	{
		OutRequest = FStartupBenchmarkRequest();
		OutError.Reset();
		TArray<FString> Tokens;
		TArray<FString> Switches;
		FCommandLine::Parse(*CommandLine, Tokens, Switches);
		TMap<FString, FString> Values;
		TSet<FString> Duplicates;
		for (const FString& Switch : Switches)
		{
			FString Name;
			FString Value;
			const bool bHasValue = Switch.Split(TEXT("="), &Name, &Value);
			if (!bHasValue) { Name = Switch; }
			Name.ToLowerInline();
			if (!Name.StartsWith(TEXT("dublinbenchmark"))) { continue; }
			OutRequest.bRequested = true;
			if (Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")) && Value.Len() >= 2)
			{
				Value = Value.Mid(1, Value.Len() - 2);
			}
			if (Value.Contains(TEXT("\""))) { OutError = TEXT("Benchmark switch has unmatched or embedded quotes."); }
			if (Values.Contains(Name)) { Duplicates.Add(Name); }
			Values.Add(Name, Value);
			if (Name != TEXT("dublinbenchmark") && Name != TEXT("dublinbenchmarkrunid")
				&& Name != TEXT("dublinbenchmarkoutput") && Name != TEXT("dublinbenchmarkduration")
				&& Name != TEXT("dublinbenchmarkwarmup"))
			{
				OutError = TEXT("Unknown DublinBenchmark-prefixed switch: ") + Name;
			}
			else if (!bHasValue || Value.IsEmpty())
			{
				OutError = TEXT("Benchmark switches require nonempty '=value' arguments.");
			}
		}
		// Any custom-prefixed option is an attempted request, including incomplete requests.
		if (!OutRequest.bRequested) { return OutError.IsEmpty(); }
		for (const FString& Name : Duplicates) { Values.Remove(Name); }
		OutRequest.Scenario = Values.FindRef(TEXT("dublinbenchmark")).ToLower();
		OutRequest.RunId = Values.FindRef(TEXT("dublinbenchmarkrunid")).ToLower();
		OutRequest.OutputDirectory = Values.FindRef(TEXT("dublinbenchmarkoutput"));
		if (!Duplicates.IsEmpty()) { OutError = TEXT("Duplicate benchmark switches are not permitted."); }
		for (TCHAR C : CommandLine)
		{
			if (C == 0 || C == TEXT('\r') || C == TEXT('\n'))
			{
				OutError = TEXT("Benchmark command line contains embedded control characters.");
			}
		}
		if (!OutError.IsEmpty()) { return false; }
		if (!ValidStartupRunId(OutRequest.RunId))
		{
			OutError = TEXT("DublinBenchmarkRunId must be a nonzero GUID in 32-hex-digit format.");
			return false;
		}
		if (OutRequest.OutputDirectory.IsEmpty() || FPaths::IsRelative(OutRequest.OutputDirectory))
		{
			OutError = TEXT("DublinBenchmarkOutput requires an absolute directory.");
			return false;
		}
		const FString* Duration = Values.Find(TEXT("dublinbenchmarkduration"));
		const FString* Warmup = Values.Find(TEXT("dublinbenchmarkwarmup"));
		OutRequest.Arguments = { OutRequest.Scenario, Duration ? *Duration : TEXT("30"), Warmup ? *Warmup : TEXT("5") };
		FOptions Options;
		EBenchmark Kind;
		return ParseBenchmarkArguments(OutRequest.Arguments, Kind, Options, OutError);
	}

	bool IsSafeBenchmarkReportName(const FString& Name)
	{
		FString Error;
		return Name.EndsWith(TEXT(".json"), ESearchCase::CaseSensitive)
			&& FPaths::GetCleanFilename(Name) == Name && FPaths::IsRelative(Name)
			&& ValidateLabel(Name.LeftChop(5), Error);
	}

	bool PrepareStartupBenchmarkDirectory(const FString& Directory, FString& OutDirectory, FString& OutError)
	{
		OutError.Reset();
		if (!ValidateStartupDirectoryPath(Directory, OutDirectory, OutError)) { return false; }
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.CreateDirectoryTree(*OutDirectory))
		{
			OutError = TEXT("Could not create benchmark output directory.");
			return false;
		}
		struct FEmptyVisitor final : IPlatformFile::FDirectoryVisitor
		{
			bool bEmpty = true;
			virtual bool Visit(const TCHAR*, bool) override { bEmpty = false; return false; }
		} Visitor;
		const bool bEnumerated = PlatformFile.IterateDirectory(*OutDirectory, Visitor);
		if (!Visitor.bEmpty || !bEnumerated)
		{
			OutError = !Visitor.bEmpty ? TEXT("Benchmark output directory is not empty; existing contents were preserved.")
				: TEXT("Could not verify that the benchmark output directory is empty.");
			return false;
		}
		// A durable claim keeps a running/failed directory nonempty and prevents replay.
		const TSharedRef<FJsonObject> Owner = MakeShared<FJsonObject>();
		Owner->SetNumberField(TEXT("processId"), FPlatformProcess::GetCurrentProcessId());
		return WriteDurableReport(Owner, TEXT("owner.json"), FGuid::NewGuid().ToString(EGuidFormats::Digits),
			OutError, true, OutDirectory);
	}

	uint8 StartupBenchmarkOutcomeCode(bool bCaptureStarted, bool bReportsPublished, bool bPassed)
	{
		if (!bCaptureStarted || !bReportsPublished) { return 2; }
		return bPassed ? 0 : 1;
	}

	bool TryClaimStartupBenchmarkRequest(bool bEligibleWorld, bool bHasBegunPlay,
		bool bWorldTearingDown, bool& bProcessClaimed)
	{
		if (!bEligibleWorld || !bHasBegunPlay || bWorldTearingDown || bProcessClaimed) { return false; }
		bProcessClaimed = true;
		return true;
	}

	bool PublishStartupBenchmarkResult(const FStartupBenchmarkRequest& Request, const FString& CaptureId,
		const FString& ProfileFile, const FString& ManifestFile, uint8 OutcomeCode, const FString& ErrorCode,
		const TArray<FString>& Errors, FString& OutError)
	{
		OutError.Reset();
		if (!ValidStartupIdentity(Request) || OutcomeCode > 2 || (OutcomeCode != 0 && (ErrorCode.IsEmpty() || Errors.IsEmpty()))
			|| (OutcomeCode == 0 && (!ErrorCode.IsEmpty() || !Errors.IsEmpty()))
			|| (!CaptureId.IsEmpty() && !ValidStartupRunId(CaptureId)))
		{
			OutError = TEXT("Invalid terminal benchmark identity or outcome.");
			return false;
		}
		if (OutcomeCode != 2 && (CaptureId.IsEmpty() || ProfileFile.IsEmpty() || ManifestFile.IsEmpty()))
		{
			OutError = TEXT("A native pass/failure requires both published reports and a capture ID.");
			return false;
		}
		if (!ProfileFile.IsEmpty() && ProfileFile.Equals(ManifestFile, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Profile and manifest must be different files.");
			return false;
		}
		FString Directory;
		if (!ValidateStartupDirectoryPath(Request.OutputDirectory, Directory, OutError)) { return false; }
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& File : { ProfileFile, ManifestFile })
		{
			if (!File.IsEmpty() && (!IsSafeBenchmarkReportName(File)
				|| File.Equals(TEXT("result.json"), ESearchCase::IgnoreCase) || File.Equals(TEXT("owner.json"), ESearchCase::IgnoreCase)
				|| !PlatformFile.FileExists(*FPaths::Combine(Directory, File))
				|| PlatformFile.IsSymlink(*FPaths::Combine(Directory, File)) != ESymlinkResult::NonSymlink))
			{
				OutError = TEXT("Terminal status must only reference existing, safe, relative report basenames.");
				return false;
			}
		}
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("schemaVersion"), 2);
		AddStartupIdentity(Result, Request);
		Result->SetStringField(TEXT("scenario"), Request.Scenario);
		Result->SetStringField(TEXT("status"), OutcomeCode == 0 ? TEXT("passed") : OutcomeCode == 1 ? TEXT("failed") : TEXT("error"));
		Result->SetNumberField(TEXT("outcomeCode"), OutcomeCode);
		Result->SetStringField(TEXT("processExitPolicy"), TEXT("graceful-zero"));
		const auto OptionalString = [&Result](const TCHAR* Name, const FString& Value)
		{
			if (Value.IsEmpty()) { Result->SetField(Name, MakeShared<FJsonValueNull>()); }
			else { Result->SetStringField(Name, Value); }
		};
		OptionalString(TEXT("captureId"), CaptureId);
		OptionalString(TEXT("profileFile"), ProfileFile);
		OptionalString(TEXT("manifestFile"), ManifestFile);
		OptionalString(TEXT("errorCode"), ErrorCode);
		TArray<TSharedPtr<FJsonValue>> ErrorValues;
		for (const FString& Error : Errors) { ErrorValues.Add(MakeShared<FJsonValueString>(Error)); }
		Result->SetArrayField(TEXT("errors"), ErrorValues);
		return WriteDurableReport(Result, TEXT("result.json"), FGuid::NewGuid().ToString(EGuidFormats::Digits),
			OutError, true, Directory);
	}
}

bool UDublinFlightPerformanceSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::PIE || WorldType == EWorldType::Game;
}

void UDublinFlightPerformanceSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!SupportsStartupBenchmark(InWorld)) { return; }
	DublinFlight::Performance::FStartupBenchmarkRequest Pending;
	FString Error;
	DublinFlight::Performance::ParseStartupBenchmarkRequest(FCommandLine::Get(), Pending, Error);
	if (!Pending.bRequested || StartupFrameHandle.IsValid()) { return; }
	// This callback precedes actor BeginPlay; no process/output ownership or error publication yet.
	StartupFrameHandle = FCoreDelegates::OnBeginFrame.AddUObject(this,
		&UDublinFlightPerformanceSubsystem::TryStartStartupBenchmark);
}

void UDublinFlightPerformanceSubsystem::CancelPendingStartupBenchmark()
{
	FCoreDelegates::OnBeginFrame.Remove(StartupFrameHandle);
	StartupFrameHandle.Reset();
}

void UDublinFlightPerformanceSubsystem::TryStartStartupBenchmark()
{
	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown || !SupportsStartupBenchmark(*World))
	{
		CancelPendingStartupBenchmark();
		return;
	}
	if (!World->HasBegunPlay()) { return; }
	CancelPendingStartupBenchmark();
	DublinFlight::Performance::FStartupBenchmarkRequest Request;
	FString Error;
	const bool bParsed = DublinFlight::Performance::ParseStartupBenchmarkRequest(FCommandLine::Get(), Request, Error);
	if (!Request.bRequested) { return; }
	static bool bProcessRequestClaimed = false;
	if (!DublinFlight::Performance::TryClaimStartupBenchmarkRequest(
		SupportsStartupBenchmark(*World), World->HasBegunPlay(), World->bIsTearingDown, bProcessRequestClaimed)) { return; }
	StartupRequest = MoveTemp(Request);
	if (!ValidStartupIdentity(StartupRequest.GetValue()))
	{
		CompleteStartupBenchmark(2, TEXT("invalid_request"),
			{ Error.IsEmpty() ? TEXT("Required benchmark identity is missing or invalid.") : Error });
		return;
	}
	FString Directory;
	FString DirectoryError;
	if (!DublinFlight::Performance::PrepareStartupBenchmarkDirectory(
		StartupRequest->OutputDirectory, Directory, DirectoryError))
	{
		CompleteStartupBenchmark(2, TEXT("invalid_output_directory"), { DirectoryError });
		return;
	}
	StartupRequest->OutputDirectory = MoveTemp(Directory);
	bStartupOutputReady = true;
	if (!bParsed)
	{
		CompleteStartupBenchmark(2, TEXT("invalid_request"), { Error });
		return;
	}
	if (!QueueBenchmark(StartupRequest->Arguments, Error))
	{
		CompleteStartupBenchmark(2, TEXT("startup_failed"), { Error });
	}
}

void UDublinFlightPerformanceSubsystem::CompleteStartupBenchmark(uint8 OutcomeCode, const FString& ErrorCode,
	const TArray<FString>& Errors, const FString& ProfileFile, const FString& ManifestFile)
{
	if (!StartupRequest.IsSet() || bStartupTerminalAttempted) { return; }
	bStartupTerminalAttempted = true;
	FString PublicationError;
	if (!bStartupOutputReady || !DublinFlight::Performance::PublishStartupBenchmarkResult(
		StartupRequest.GetValue(), CaptureId, ProfileFile, ManifestFile, OutcomeCode, ErrorCode, Errors, PublicationError))
	{
		UE_LOG(LogDublinFlightPerformance, Error,
			TEXT("No terminal benchmark result was published; consumers must fail the run. %s"), *PublicationError);
	}
	// Semantic outcome is in result.json; a missing result fails even after graceful OS exit zero.
	FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("DublinFlight startup benchmark completed"));
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
	StartForegroundDiagnostics = ReadForegroundDiagnostics(World);
	PSODiagnostics = MakeShared<DublinFlight::Performance::FPSODiagnosticCapture>();
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
	FString Error;
	const bool bStarted = QueueBenchmark(Args, Error);
	if (!bStarted) { UE_LOG(LogDublinFlightPerformance, Error, TEXT("%s"), *Error); }
	return bStarted;
#endif
}

bool UDublinFlightPerformanceSubsystem::QueueBenchmark(const TArray<FString>& Args, FString& OutError)
{
	using namespace DublinFlight::Performance;
	if (!IsInGameThread())
	{
		OutError = TEXT("StartBenchmark requires the game thread.");
		return false;
	}
	if (bCapturing || bBenchmarkWaiting || Benchmark.IsValid())
	{
		OutError = TEXT("This world already has an active capture or benchmark.");
		return false;
	}
	FOptions Options;
	EBenchmark Kind;
	FString Error;
	if (!ParseBenchmarkArguments(Args, Kind, Options, Error))
	{
		OutError = Error;
		return false;
	}
	if (!GetWorld() || !DoesSupportWorldType(GetWorld()->WorldType) || IsRunningCommandlet())
	{
		OutError = TEXT("StartBenchmark requires a PIE/game world, not a commandlet.");
		return false;
	}
	if (!CaptureWindow.Start(Options, Error))
	{
		OutError = TEXT("Could not initialize benchmark capture: ") + Error;
		return false;
	}
	Benchmark = MakeShared<FDublinRuntimeBenchmark>(Kind, Options);
	BenchmarkWaitStart = GetSharedFrameBoundarySeconds();
	bBenchmarkWaiting = true;
	bStopRequested = false;
	StartScenario = ReadScenario(*GetWorld());
	StartForegroundDiagnostics = ReadForegroundDiagnostics(GetWorld());
	StartedUtc = FDateTime::UtcNow().ToIso8601();
	CaptureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddUObject(this, &UDublinFlightPerformanceSubsystem::AwaitBenchmarkWorld);
	UE_LOG(LogDublinFlightPerformance, Display, TEXT("Benchmark %s queued; bounded 60-wall-second BeginPlay wait. Reports: %s."),
		*Options.Label, StartupRequest.IsSet() ? TEXT("explicit startup output directory") : TEXT("Saved\\Performance"));
	return true;
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
	else if (StartupRequest.IsSet()) { bStartupCaptureStarted = true; }
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
	if (PSODiagnostics.IsValid() && Result != DublinFlight::Performance::EBoundaryResult::Error)
	{
		PSODiagnostics->Observe(GFrameCounter, Boundary, CaptureWindow,
			Result == DublinFlight::Performance::EBoundaryResult::Complete || bStopRequested,
			DublinFlight::Performance::FPSODiagnosticCapture::Read());
	}
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
	const FString OutputDirectory = StartupRequest.IsSet() ? StartupRequest->OutputDirectory : FString();
	FString PublishedManifest;
	FString PublicationError;
	bool bNativePassed = false;
	bCapturing = false;
	bBenchmarkWaiting = false;
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	BeginFrameHandle.Reset();
	if (StartupRequest.IsSet() && bStartupTerminalAttempted)
	{
		if (Benchmark.IsValid()) { Benchmark->Finish(CompletionReason); }
		StartScenario.Reset();
		StartForegroundDiagnostics.Reset();
		PSODiagnostics.Reset();
		Benchmark.Reset();
		return;
	}

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
	if (StartForegroundDiagnostics.IsValid())
	{
		Report->SetObjectField(TEXT("foregroundDiagnosticsStart"), StartForegroundDiagnostics.ToSharedRef());
	}
	else
	{
		Report->SetField(TEXT("foregroundDiagnosticsStart"), MakeShared<FJsonValueNull>());
	}
	Report->SetObjectField(TEXT("foregroundDiagnosticsEnd"), ReadForegroundDiagnostics(bWorldEnding ? nullptr : GetWorld()));
	if (PSODiagnostics.IsValid()) { Report->SetObjectField(TEXT("psoDiagnostics"), PSODiagnostics->Report()); }
	else { Report->SetField(TEXT("psoDiagnostics"), MakeShared<FJsonValueNull>()); }
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
	if (StartupRequest.IsSet()) { AddStartupIdentity(Report, StartupRequest.GetValue()); }

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
		const bool bFrameTargetRequired = Benchmark->IsFrameTargetRequired();
		const TCHAR* PerformancePolicy = bFrameTargetRequired
			? TEXT("strict-frame-floor-v1") : TEXT("maximal-spectacle-v1");
		Report->SetBoolField(TEXT("benchmarkValid"), bValid);
		Report->SetBoolField(TEXT("workloadAdmitted"), bAdmitted);
		Report->SetBoolField(TEXT("frameTargetMet"), bFrameTarget);
		Report->SetBoolField(TEXT("frameTargetRequired"), bFrameTargetRequired);
		Report->SetStringField(TEXT("performancePolicy"), PerformancePolicy);
		Report->SetBoolField(TEXT("scenarioTargetMet"), bFrameTarget && bValid && bAdmitted);
		bNativePassed = BenchmarkPassed(bDurationReached && Metrics.IsSet(), bValid, bAdmitted,
			bFrameTarget, bFrameTargetRequired);
		Report->SetBoolField(TEXT("benchmarkPassed"), bNativePassed);
		if (!bValid || !bAdmitted) { Report->SetStringField(TEXT("status"), TEXT("invalid")); }
		Report->SetStringField(TEXT("settingsSampling"), TEXT("Benchmark environment sampled at every measured shared boundary; all frames retained on invalidity."));
		Report->SetObjectField(TEXT("benchmark"), Benchmark->Report());
		const FString ManifestName = FPaths::GetBaseFilename(FileName) + TEXT("-manifest.json");
		const TSharedRef<FJsonObject> Manifest = Benchmark->Report();
		if (StartupRequest.IsSet()) { AddStartupIdentity(Manifest, StartupRequest.GetValue()); }
		Manifest->SetStringField(TEXT("captureId"), CaptureId);
		Manifest->SetStringField(TEXT("profileFile"), FileName);
		Manifest->SetStringField(TEXT("status"), Report->GetStringField(TEXT("status")));
		Manifest->SetBoolField(TEXT("completedRequestedDuration"), bDurationReached && Metrics.IsSet());
		Manifest->SetBoolField(TEXT("scenarioTargetMet"), bFrameTarget && bValid && bAdmitted);
		Manifest->SetBoolField(TEXT("frameTargetMet"), bFrameTarget);
		Manifest->SetBoolField(TEXT("frameTargetRequired"), bFrameTargetRequired);
		Manifest->SetStringField(TEXT("performancePolicy"), PerformancePolicy);
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
		if (!WriteDurableReport(Manifest, ManifestName, CaptureId, ManifestError, true, OutputDirectory))
		{
			PublicationError = ManifestError;
			Report->SetStringField(TEXT("status"), TEXT("failed"));
			Report->SetBoolField(TEXT("benchmarkPassed"), false);
			Report->SetBoolField(TEXT("scenarioTargetMet"), false);
			Report->SetStringField(TEXT("manifestError"), ManifestError);
			UE_LOG(LogDublinFlightPerformance, Error, TEXT("Benchmark manifest NOT published: %s"), *ManifestError);
		}
		else
		{
			PublishedManifest = ManifestName;
			const TCHAR* Location = StartupRequest.IsSet() ? TEXT("<startup output>") : TEXT("Saved\\Performance");
			UE_LOG(LogDublinFlightPerformance, Display, TEXT("Benchmark status=%s completedRequestedDuration=%s scenarioTargetMet=%s profile=%s\\%s manifest=%s\\%s"),
				*Report->GetStringField(TEXT("status")), bDurationReached ? TEXT("true") : TEXT("false"),
				Report->GetBoolField(TEXT("scenarioTargetMet")) ? TEXT("true") : TEXT("false"), Location, *FileName, Location, *ManifestName);
		}
	}
	FString WriteError;
	const bool bProfilePublished = WriteDurableReport(Report, FileName, CaptureId, WriteError, Benchmark.IsValid(), OutputDirectory);
	if (!bProfilePublished)
	{
		PublicationError += PublicationError.IsEmpty() ? WriteError : TEXT("; ") + WriteError;
		UE_LOG(LogDublinFlightPerformance, Error, TEXT("Profile '%s' was NOT published: %s Intended filename: %s; temporary suffix: .%s.tmp"),
			*Options.Label, *WriteError, *FileName, *CaptureId);
	}
	else if (Benchmark.IsValid())
	{
		UE_LOG(LogDublinFlightPerformance, Display, TEXT("Benchmark profile published: status=%s, %s\\%s"),
			*Report->GetStringField(TEXT("status")),
			StartupRequest.IsSet() ? TEXT("<startup output>") : TEXT("Saved\\Performance"), *FileName);
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
	if (StartupRequest.IsSet() && !bStartupTerminalAttempted)
	{
		const uint8 OutcomeCode = StartupBenchmarkOutcomeCode(bStartupCaptureStarted,
			bProfilePublished && !PublishedManifest.IsEmpty(), bNativePassed);
		TArray<FString> Errors;
		if (!Failure.IsEmpty()) { Errors.Add(Failure); }
		if (!MetricsError.IsEmpty()) { Errors.AddUnique(MetricsError); }
		if (!PublicationError.IsEmpty()) { Errors.Add(PublicationError); }
		if (Benchmark.IsValid())
		{
			const TSharedRef<FJsonObject> BenchmarkReport = Benchmark->Report();
			for (const TSharedPtr<FJsonValue>& Reason : BenchmarkReport->GetArrayField(TEXT("invalidReasons")))
			{
				Errors.AddUnique(Reason->AsString());
			}
			if (!Benchmark->WorkloadAdmitted()) { Errors.AddUnique(TEXT("Native workload was not admitted.")); }
		}
		if (!bDurationReached) { Errors.AddUnique(FString(TEXT("Capture ended: ")) + CompletionReason); }
		if ((!Benchmark.IsValid() || Benchmark->IsFrameTargetRequired()) &&
			Metrics.IsSet() && !Metrics->MeetsTargetMinimumFPS(Options.ScenarioTargetMinimumFPS))
		{
			Errors.AddUnique(TEXT("The required native per-frame FPS floor was not met."));
		}
		if (OutcomeCode != 0 && Errors.IsEmpty()) { Errors.Add(TEXT("Native benchmark did not satisfy all required gates.")); }
		const FString Code = OutcomeCode == 0 ? FString() : OutcomeCode == 1 ? TEXT("benchmark_failed")
			: bStartupCaptureStarted ? TEXT("publication_failed") : TEXT("startup_failed");
		CompleteStartupBenchmark(OutcomeCode, Code, Errors, bProfilePublished ? FileName : FString(), PublishedManifest);
	}
	StartScenario.Reset();
	StartForegroundDiagnostics.Reset();
	PSODiagnostics.Reset();
	Benchmark.Reset();
}

void UDublinFlightPerformanceSubsystem::OnWorldEndPlay(UWorld& InWorld)
{
	CancelPendingStartupBenchmark();
	if (bCapturing || bBenchmarkWaiting)
	{
		FinishCapture(TEXT("worldEnded"));
	}
	Super::OnWorldEndPlay(InWorld);
}

void UDublinFlightPerformanceSubsystem::Deinitialize()
{
	CancelPendingStartupBenchmark();
	if (bCapturing || bBenchmarkWaiting)
	{
		FinishCapture(TEXT("worldDeinitialized"));
	}
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	BeginFrameHandle.Reset();
	Super::Deinitialize();
}
