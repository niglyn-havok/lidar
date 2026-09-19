#include "City/DublinCityWorld.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "DublinFlightPawn.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Effects/DublinImpactEffectsSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ImagePixelData.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "ImageWriteTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SceneView.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Weapons/DublinWeaponComponent.h"
#include "Weapons/DublinWeaponModel.h"

namespace DublinStructuralPilot
{
	constexpr double Interval = 0.125;
	constexpr double Duration = 10.0;
	constexpr double IntactSeconds = 1.0;
	constexpr double PostImpactSeconds = 9.0;
	constexpr int32 IntactSlots = 8;
	constexpr int32 PrimingCount = 2;
	constexpr double PrimingWorldLimit = 3.0;
	constexpr int32 FrameSlots = 81;
	constexpr int32 ImpactSeed = 20260918;
	const FIntPoint CaptureSize(1280, 720);
	constexpr double QueueDrainWorldLimit = 35.0;
	constexpr double QueueDrainWallLimit = 40.0;
	constexpr int32 MaxPendingWrites = 4;
	constexpr int32 MaxDroppedReadbacks = 4;
	constexpr double WriteTimeoutSeconds = 10.0;
	bool bFixtureActive = false;

	struct FCase { const TCHAR* Name; const TCHAR* SourceId; };
	const FCase Cases[] = {
		{TEXT("SmallRotated"), TEXT("osm/way/233804861")},
		{TEXT("IrregularFootprint"), TEXT("osm/way/233804877")},
		{TEXT("LargeBlock"), TEXT("osm/way/389853640")}
	};

	struct FPendingWrite
	{
		TFuture<bool> Result;
		TSharedPtr<FJsonObject> Frame;
		FString Path;
		double StartedWall = 0;
		bool bTimeoutReported = false;
	};

	void VectorField(FJsonObject& Object, const TCHAR* Name, const FVector& Value)
	{
		Object.SetArrayField(Name, {MakeShared<FJsonValueNumber>(Value.X),
			MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)});
	}

	TArray<TSharedPtr<FJsonValue>> LeafJson(const TArray<FTransform>& Transforms, const TArray<int32>& Leaves)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (int32 Index = 0; Index < Transforms.Num(); ++Index)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("leaf"), Leaves[Index]);
			VectorField(*Item, TEXT("position_cm"), Transforms[Index].GetLocation());
			VectorField(*Item, TEXT("scale"), Transforms[Index].GetScale3D());
			const FQuat Rotation = Transforms[Index].GetRotation();
			Item->SetArrayField(TEXT("rotation_xyzw"), {MakeShared<FJsonValueNumber>(Rotation.X),
				MakeShared<FJsonValueNumber>(Rotation.Y), MakeShared<FJsonValueNumber>(Rotation.Z), MakeShared<FJsonValueNumber>(Rotation.W)});
			Result.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Result;
	}

	class FCapture final : public IAutomationLatentCommand
	{
	public:
		FCapture(FAutomationTestBase& InTest, const FCase& InCase)
			: Test(InTest), Case(InCase), StartedWall(FPlatformTime::Seconds())
		{
			RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Directory = MakeDirectory(TEXT("unresolved"));
			Manifest->SetStringField(TEXT("test"), FString(TEXT("DublinFlight.Destruction.PIE.StructuralPilot.")) + Case.Name);
			Manifest->SetStringField(TEXT("source_id"), Case.SourceId);
			Manifest->SetStringField(TEXT("run_guid"), RunId);
			Manifest->SetNumberField(TEXT("manifest_version"), 3);
			Manifest->SetNumberField(TEXT("outcome_policy_version"), 2);
			Manifest->SetNumberField(TEXT("screenshot_request_policy_version"), 3);
			Manifest->SetStringField(TEXT("screenshot_request_policy"), TEXT("Later-engine-frame requests only. Cleared requests without owned pixels are explicit drops, never successes. Primes wait for each previous attempt to be verified or dropped."));
			Manifest->SetNumberField(TEXT("max_dropped_readbacks"), MaxDroppedReadbacks);
			Manifest->SetStringField(TEXT("motion_gate_contract"), TEXT("Travel, drop and independent separation must each exceed 25cm; at least two descending world frames."));
			Manifest->SetStringField(TEXT("capture_profile"), TEXT("1280x720-fixed-async-png-primed2-no-background-throttle-v2"));
			Manifest->SetStringField(TEXT("frame_timing_basis"), TEXT("captured_world_seconds is the game-thread readback event, not asynchronous file completion."));
			Manifest->SetBoolField(TEXT("png_compression_and_write_async"), true);
			Manifest->SetBoolField(TEXT("gpu_readback_async"), false);
			Manifest->SetNumberField(TEXT("max_pending_pixel_copies"), MaxPendingWrites);
			Manifest->SetNumberField(TEXT("max_pending_pixel_bytes"), double(MaxPendingWrites) * CaptureSize.X * CaptureSize.Y * sizeof(FColor));
			Manifest->SetObjectField(TEXT("capture_environment"), Environment);
			Manifest->SetStringField(TEXT("started_utc"), FDateTime::UtcNow().ToIso8601());
			Manifest->SetStringField(TEXT("route"), TEXT("City.ApplyImpact then Effects.EmitImpact: accepted bomb physics; NOT B-key input or a projectile drop."));
			Manifest->SetBoolField(TEXT("visual_inspection_required"), true);
			Manifest->SetBoolField(TEXT("performance_claim"), false);
			Manifest->SetBoolField(TEXT("deterministic_physics_claim"), false);
			Manifest->SetBoolField(TEXT("hud_left_unchanged"), true);
			Manifest->SetBoolField(TEXT("neighbor_damage_allowed"), true);
			Manifest->SetNumberField(TEXT("camera_recipe_version"), 1);
			Manifest->SetNumberField(TEXT("target_frames_per_world_second"), 8);
			Manifest->SetNumberField(TEXT("scheduled_frame_slots"), FrameSlots);
			Manifest->SetNumberField(TEXT("capture_world_seconds"), Duration);
			Manifest->SetStringField(TEXT("capture_duration_policy"), TEXT("Ten seconds nominal: one-second intact pre-roll, then nine seconds scheduled from the actual impact. Use captured timestamps for actual durations."));
			Manifest->SetNumberField(TEXT("post_impact_capture_seconds"), PostImpactSeconds);
			Manifest->SetNumberField(TEXT("priming_capture_count"), PrimingCount);
			Manifest->SetNumberField(TEXT("priming_world_limit_seconds"), PrimingWorldLimit);
			Manifest->SetBoolField(TEXT("priming_excluded_from_evidence"), true);
			Manifest->SetField(TEXT("impact_world_seconds"), MakeShared<FJsonValueNull>());
			Manifest->SetField(TEXT("target_activation_world_seconds"), MakeShared<FJsonValueNull>());
			Manifest->SetBoolField(TEXT("queue_drain_complete"), false);
			ProcessedHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddRaw(this, &FCapture::ScreenshotProcessed);
			PIEEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FCapture::OnPIEEnding);
			ScopedEditor = GEditor;
			PerformanceSettings = GetMutableDefault<UEditorPerformanceSettings>();
			bOriginalThrottleCPU = PerformanceSettings->bThrottleCPUWhenNotForeground != 0;
			Environment->SetBoolField(TEXT("original_background_cpu_throttle"), bOriginalThrottleCPU);
			Environment->SetBoolField(TEXT("original_effective_cpu_throttle"), GEditor && GEditor->ShouldThrottleCPUUsage());
			PerformanceSettings->bThrottleCPUWhenNotForeground = false;
			if (ScopedEditor.IsValid())
			{
				UEditorEngine::FShouldDisableCPUThrottling Delegate =
					UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([] { return true; });
				CPUThrottleHandle = Delegate.GetHandle();
				ScopedEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
			}
			Environment->SetBoolField(TEXT("effective_cpu_throttle_during_fixture"), GEditor && GEditor->ShouldThrottleCPUUsage());
			Environment->SetStringField(TEXT("scope"), TEXT("In-memory editor throttle flag and scoped CPU-throttling delegate; owned PIE fixed viewport only. No config, window-size or quality changes."));
		}

		virtual ~FCapture() override
		{
			FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ProcessedHandle);
			FEditorDelegates::PrePIEEnded.Remove(PIEEndedHandle);
			Cleanup();
			bFixtureActive = false;
		}

		virtual bool Update() override
		{
			if (bFinishing) { return Finalize(); }
			PollWrites();
			if (FPlatformTime::Seconds() - StartedWall > 85.0)
			{
				Fail(TEXT("Pilot exceeded 85 seconds wall time; five seconds remain for bounded PIE teardown."));
				return Finish();
			}
			if (bFailed) { return Finish(); }
			if (!bStarted) { return bPrepared ? AwaitCapturePresentation() : Bootstrap(); }
			if (!World.IsValid() || World->bIsTearingDown || !City.IsValid() || !Player.IsValid() || !Camera.IsValid())
			{
				Fail(TEXT("Owned PIE or observer ended during capture."));
				return Finish();
			}
			if (!City->bAllBuildingsFractureReady || City->bAllowPartialBakePreview || City->bImpactQueueBlocked ||
				City->ActiveFractureCollections > DublinDestruction::MaxActiveCollections ||
				City->ActiveFracturePieces > DublinDestruction::MaxActivePieces ||
				City->QueuedImpactCount > DublinDestruction::MaxQueuedImpacts)
			{
				Fail(TEXT("Full-city readiness, queue or 24-collection/1536-piece budget invariant failed: ") + City->LastDestructionError);
				return Finish();
			}
			if (bDraining) { return DrainQueue(); }
			if (bCaptureComplete) { return CompleteCapture(); }
			PollScreenshot();
			if (bFailed) { return Finish(); }
			const double Now = World->GetTimeSeconds();
			if (Now <= LastWorld) { return false; }
			LastWorld = Now;
			if (!ValidateView()) { return Finish(); }
			const double Elapsed = Now - CaptureStart;
			if (!bImpact && Elapsed > 2.0)
			{
				Fail(TEXT("Screenshot throughput prevented impact within the intact pre-roll bound.")); return Finish();
			}
			if (!bImpact && Elapsed >= IntactSeconds && !Pending.IsValid())
			{
				if (CapturedBeforeFrames < 1) { Fail(TEXT("No captured intact frame exists before the bomb.")); return Finish(); }
				UDublinImpactEffectsSubsystem* Effects = World->GetSubsystem<UDublinImpactEffectsSubsystem>();
				if (!Effects || City->AcceptedImpactCount != 0 || !City->ApplyImpact(Impact))
				{
					Fail(TEXT("The exact pilot's fixed roof-centroid bomb was rejected; impact was not relocated."));
					return Finish();
				}
				Effects->EmitImpact(Impact);
				bImpact = true;
				ImpactWorld = Now;
				Manifest->SetNumberField(TEXT("impact_world_seconds"), Now);
				Manifest->SetNumberField(TEXT("impact_elapsed_seconds"), Elapsed);
				Manifest->SetNumberField(TEXT("intact_captured_frames_at_impact"), CapturedBeforeFrames);
			}
			if (bImpact)
			{
				if (City->AcceptedImpactCount != 1) { Fail(TEXT("Unexpected extra impact contaminated the single-bomb comparison.")); }
				ObserveLeaves();
				if (!Collection.IsValid() && Now - ImpactWorld > 3.0) { Fail(TEXT("Exact pilot collection did not activate within three world seconds.")); }
			}
			if (bFailed) { return Finish(); }
			if (!Pending.IsValid() && NextSlot < FrameSlots && Elapsed >= ScheduledElapsed(NextSlot) && CanIssueScreenshotThisFrame())
			{
				if (FScreenshotRequest::IsScreenshotRequested())
				{
					if (ScreenshotBusyWall == 0) { ScreenshotBusyWall = FPlatformTime::Seconds(); }
					if (FPlatformTime::Seconds() - ScreenshotBusyWall > 5.0) { Fail(TEXT("Another screenshot request blocked capture; it was not overwritten.")); }
				}
				else if (PendingWrites.Num() >= MaxPendingWrites)
				{
					++WriterBackpressureObservations;
				}
				else
				{
					ScreenshotBusyWall = 0;
					while (NextSlot < FrameSlots - 1 && ScheduledElapsed(NextSlot + 1) <=
						FMath::Min(Elapsed, ScheduledElapsed(FrameSlots - 1)))
					{
						MissedSlots.Add(MakeShared<FJsonValueNumber>(NextSlot++));
					}
					RequestFrame(Now);
				}
			}
			if (bFailed) { return Finish(); }
			if (bImpact && Now - ImpactWorld >= PostImpactSeconds && NextSlot == FrameSlots && !Pending.IsValid())
			{
				bCaptureComplete = true;
				CaptureEndWorld = Now;
				CaptureEndWall = FPlatformTime::Seconds();
				Manifest->SetNumberField(TEXT("capture_end_world_seconds"), Now);
				Manifest->SetNumberField(TEXT("capture_end_queued_impacts"), City->QueuedImpactCount);
				Manifest->SetNumberField(TEXT("capture_end_queued_building_hits"), City->QueuedBuildingHitCount);
				SaveManifest();
				return bFailed ? Finish() : CompleteCapture();
			}
			return false;
		}

	private:
		FAutomationTestBase& Test;
		FCase Case;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ADublinCityWorld> City;
		TWeakObjectPtr<APlayerController> Player;
		TWeakObjectPtr<ADublinFlightPawn> Pawn;
		TWeakObjectPtr<AActor> PreviousViewTarget;
		TWeakObjectPtr<ACameraActor> Camera;
		TWeakObjectPtr<UGeometryCollectionComponent> Collection;
		TWeakObjectPtr<UEditorEngine> ScopedEditor;
		TWeakObjectPtr<UEditorPerformanceSettings> PerformanceSettings;
		TWeakObjectPtr<UGameViewportClient> CaptureViewportClient;
		FSceneViewport* ModifiedViewport = nullptr;
		FIntPoint OriginalViewportSize = FIntPoint::ZeroValue;
		FDublinFractureRecord Record;
		FDublinImpact Impact;
		FBox SourceBounds = FBox(ForceInit);
		FBox FrameBounds = FBox(ForceInit);
		FVector SourcePivot = FVector::ZeroVector;
		FVector CameraPosition = FVector::ZeroVector;
		FRotator CameraRotation = FRotator::ZeroRotator;
		FVector FacadeProbe = FVector::ZeroVector;
		TArray<FTransform> InitialLeaves, PreviousLeaves, LatestLeaves;
		TArray<TSharedPtr<FJsonValue>> Frames, PrimingFrames, MissedSlots, Errors;
		TArray<FPendingWrite> PendingWrites;
		IImageWriteQueue* ImageQueue = nullptr;
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Environment = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Pending;
		FDelegateHandle ProcessedHandle, CapturedHandle;
		FDelegateHandle PIEEndedHandle, CPUThrottleHandle;
		FString RunId, Directory, PendingPath;
		double StartedWall, CaptureStart = 0, ImpactWorld = 0, LastWorld = -1;
		double PendingWall = 0, ScreenshotBusyWall = 0, FinishWall = 0;
		double PrepareWorld = 0, PrepareWall = 0, DrainStartWorld = 0, DrainStartWall = 0;
		double CaptureEndWorld = 0, CaptureEndWall = 0;
		double PrimingStartWorld = -1;
		uint64 PrepareEngineFrame = 0;
		uint64 LastScreenshotServiceFrame = 0;
		uint64 PendingRequestEngineFrame = 0;
		double LastProcessedWorld = -1, MaxProcessedGap = 0, MaxTravel = 0, MaxDrop = 0, MaxSeparationChange = 0;
		int32 NextSlot = 0, WrittenFrames = 0, BeforeFrames = 0, AfterFrames = 0, DescendingFrames = 0;
		int32 CapturedFrames = 0, CapturedBeforeFrames = 0, PeakPendingWrites = 0, WriterBackpressureObservations = 0;
		int32 FirstIntactSlot = INDEX_NONE, LastAfterSlot = INDEX_NONE;
		int32 PrimingRequested = 0, PrimingCaptured = 0, PrimingWritten = 0;
		int32 UnmatchedProcessedCallbacks = 0;
		int32 DroppedReadbacks = 0, PrimingDropped = 0;
		bool bStarted = false, bImpact = false, bFailed = false, bFinishing = false, bCleaned = false;
		bool bProcessed = false, bPawnTickWasEnabled = false, bInputSuppressed = false;
		bool bScreenMessagesBeforeRequest = true;
		bool bPrepared = false, bDraining = false, bOriginalThrottleCPU = false;
		bool bOriginalViewportFixed = false, bEnvironmentRestored = false;
		bool bCaptured = false, bCaptureComplete = false, bFinalTimeoutReported = false;
		bool bLegacyBaseline = false, bStructuralAcceptanceAssessed = false, bMotionGateEvaluated = false;
		bool bObservedScreenshotService = false;
		bool bPendingDropped = false;

		FString MakeDirectory(const FString& Digest) const
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
				TEXT("StructuralPilot-20260918"), Case.Name, Digest + TEXT("-") + RunId));
		}
		void Fail(const FString& Reason)
		{
			bFailed = true;
			Test.AddError(Reason);
			Errors.Add(MakeShared<FJsonValueString>(Reason));
		}
		double ScheduledElapsed(int32 Slot) const
		{
			return Slot * Interval + (bImpact && Slot >= IntactSlots ? ImpactWorld - CaptureStart - IntactSeconds : 0.0);
		}
		bool MotionGatePassed() const
		{
			return MaxTravel > 25.0 && MaxDrop > 25.0 && MaxSeparationChange > 25.0 && DescendingFrames >= 2;
		}
		bool CanIssueScreenshotThisFrame() const
		{
			return !bObservedScreenshotService || GFrameCounter > LastScreenshotServiceFrame;
		}
		void ObserveScreenshotService()
		{
			bObservedScreenshotService = true;
			LastScreenshotServiceFrame = GFrameCounter;
		}
		bool SetCapturePresentation(UGameViewportClient& Client)
		{
			FSceneViewport* Scene = Client.GetGameViewport();
			if (!Scene || Scene != Client.Viewport || Scene->GetSizeXY().GetMin() <= 0)
			{
				Fail(TEXT("Owned PIE has no live scene viewport for a scoped fixed render size.")); return false;
			}
			CaptureViewportClient = &Client;
			ModifiedViewport = Scene;
			OriginalViewportSize = Scene->GetSizeXY();
			bOriginalViewportFixed = Scene->HasFixedSize();
			Environment->SetNumberField(TEXT("original_viewport_width"), OriginalViewportSize.X);
			Environment->SetNumberField(TEXT("original_viewport_height"), OriginalViewportSize.Y);
			Environment->SetBoolField(TEXT("original_viewport_fixed"), bOriginalViewportFixed);
			Environment->SetNumberField(TEXT("requested_viewport_width"), CaptureSize.X);
			Environment->SetNumberField(TEXT("requested_viewport_height"), CaptureSize.Y);
			Environment->SetStringField(TEXT("resize_api"), TEXT("FSceneViewport::SetFixedViewportSize"));
			Scene->SetFixedViewportSize(static_cast<uint32>(CaptureSize.X), static_cast<uint32>(CaptureSize.Y));
			PrepareWorld = World->GetTimeSeconds();
			PrepareWall = FPlatformTime::Seconds();
			PrepareEngineFrame = GFrameCounter;
			return true;
		}
		bool HasCaptureSize() const
		{
			const FSceneViewport* Scene = CaptureViewportClient.IsValid() ? CaptureViewportClient->GetGameViewport() : nullptr;
			return Scene && Scene == ModifiedViewport && Scene->HasFixedSize() &&
				Scene->GetSizeXY() == CaptureSize && Scene->GetRenderTargetTextureSizeXY() == CaptureSize;
		}
		bool AwaitCapturePresentation()
		{
			if (!World.IsValid() || World->bIsTearingDown || !Player.IsValid() || !Camera.IsValid())
			{
				Fail(TEXT("Owned PIE ended while preparing the fixed capture viewport.")); return Finish();
			}
			if (FPlatformTime::Seconds() - PrepareWall > 10.0)
			{
				Fail(TEXT("Fixed render target, screenshot availability or priming did not complete within ten wall seconds.")); return Finish();
			}
			PollScreenshot();
			if (bFailed) { return Finish(); }
			if (PrimingStartWorld >= 0 && World->GetTimeSeconds() - PrimingStartWorld > PrimingWorldLimit)
			{
				Fail(TEXT("Two real priming captures did not finish within three world seconds; evidence capture was not started.")); return Finish();
			}
			if (Pending.IsValid() || !HasCaptureSize() || GFrameCounter - PrepareEngineFrame < 3 ||
				World->GetTimeSeconds() - PrepareWorld < 0.25 || FScreenshotRequest::IsScreenshotRequested() ||
				!CanIssueScreenshotThisFrame()) { return false; }
			if (!ValidateView()) { return Finish(); }
			if (PrimingRequested < PrimingCount + PrimingDropped)
			{
				if (PrimingWritten + PrimingDropped != PrimingRequested) { return false; }
				if (PrimingStartWorld < 0)
				{
					PrimingStartWorld = World->GetTimeSeconds();
					Manifest->SetNumberField(TEXT("priming_start_world_seconds"), PrimingStartWorld);
				}
				RequestFrame(World->GetTimeSeconds(), true);
				SaveManifest();
				return bFailed ? Finish() : false;
			}
			if (Pending.IsValid() || !PendingWrites.IsEmpty() || PrimingWritten != PrimingCount) { return false; }
			Environment->SetNumberField(TEXT("verified_viewport_width"), CaptureSize.X);
			Environment->SetNumberField(TEXT("verified_viewport_height"), CaptureSize.Y);
			Environment->SetNumberField(TEXT("preparation_world_seconds"), World->GetTimeSeconds() - PrepareWorld);
			Environment->SetNumberField(TEXT("preparation_wall_seconds"), FPlatformTime::Seconds() - PrepareWall);
			CaptureStart = World->GetTimeSeconds();
			LastWorld = CaptureStart;
			LastProcessedWorld = -1;
			MaxProcessedGap = 0;
			Manifest->SetNumberField(TEXT("priming_complete_world_seconds"), CaptureStart);
			Manifest->SetNumberField(TEXT("capture_start_world_seconds"), CaptureStart);
			bStarted = true;
			SaveManifest();
			if (bFailed) { return Finish(); }
			RequestFrame(CaptureStart);
			return bFailed ? Finish() : false;
		}
		bool CompleteCapture()
		{
			if (!PendingWrites.IsEmpty())
			{
				if (FPlatformTime::Seconds() - CaptureEndWall > WriteTimeoutSeconds)
				{
					Fail(TEXT("Asynchronous PNG writes did not finish within the bounded post-capture wait.")); return Finish();
				}
				return false;
			}
			Manifest->SetNumberField(TEXT("post_capture_write_wait_wall_seconds"), FPlatformTime::Seconds() - CaptureEndWall);
			if (!bImpact || CaptureEndWorld - ImpactWorld < PostImpactSeconds)
			{
				Fail(TEXT("Recording lacks the accepted fixed bomb or its full post-impact observation window."));
			}
			bMotionGateEvaluated = true;
			if (bStructuralAcceptanceAssessed && !MotionGatePassed())
			{
				Fail(TEXT("Insufficient actual leaf translation, falling or relative separation; counters alone do not establish structural breakup."));
			}
			else if (bLegacyBaseline && !MotionGatePassed())
			{
				const FString Warning = FString::Printf(TEXT("SolidGrid recording-only baseline has insufficient motion: "
					"travel=%.3fcm drop=%.3fcm separation=%.3fcm descendingFrames=%d. Structural acceptance is NOT assessed."),
					MaxTravel, MaxDrop, MaxSeparationChange, DescendingFrames);
				Manifest->SetStringField(TEXT("motion_gate_warning"), Warning);
				Test.AddWarning(Warning);
			}
			if (WrittenFrames < 64 || BeforeFrames < 6 || AfterFrames < 56 || MaxProcessedGap > 0.5)
			{
				Fail(TEXT("Insufficient temporal coverage: require 64 written frames, six intact, 56 after impact and no processed-frame gap over 0.5 world seconds."));
			}
			if (WrittenFrames != CapturedFrames) { Fail(TEXT("Not every captured image produced a verified PNG.")); }
			if (!MissedSlots.IsEmpty()) { Test.AddWarning(TEXT("Capture missed scheduled slots; encode from captured timestamps, not an assumed constant frame rate.")); }
			if (bFailed) { return Finish(); }
			bDraining = true;
			DrainStartWorld = World->GetTimeSeconds();
			DrainStartWall = FPlatformTime::Seconds();
			Manifest->SetNumberField(TEXT("queue_drain_limit_world_seconds"), QueueDrainWorldLimit);
			Manifest->SetNumberField(TEXT("queue_drain_limit_wall_seconds"), QueueDrainWallLimit);
			SaveManifest();
			Test.AddInfo(TEXT("Footage and asynchronous writes complete; waiting separately for neighboring-building queue work."));
			return bFailed ? Finish() : DrainQueue();
		}
		bool DrainQueue()
		{
			const double WorldElapsed = World->GetTimeSeconds() - DrainStartWorld;
			const double WallElapsed = FPlatformTime::Seconds() - DrainStartWall;
			Manifest->SetNumberField(TEXT("queue_drain_elapsed_world_seconds"), WorldElapsed);
			Manifest->SetNumberField(TEXT("queue_drain_elapsed_wall_seconds"), WallElapsed);
			if (City->AcceptedImpactCount != 1)
			{
				Fail(TEXT("An extra impact contaminated the post-capture queue-drain phase.")); return Finish();
			}
			if (WorldElapsed >= QueueDrainWorldLimit || WallElapsed >= QueueDrainWallLimit)
			{
				Manifest->SetBoolField(TEXT("queue_drain_complete"), false);
				Fail(TEXT("Healthy-looking neighbor queue still did not drain within its additional 35-world/40-wall-second bound."));
				return Finish();
			}
			if (City->QueuedImpactCount == 0 && City->QueuedBuildingHitCount == 0)
			{
				Manifest->SetBoolField(TEXT("queue_drain_complete"), true);
				return Finish();
			}
			return false;
		}
		void RestoreCapturePresentation()
		{
			if (bEnvironmentRestored) { return; }
			bEnvironmentRestored = true;
			FSceneViewport* Scene = CaptureViewportClient.IsValid() ? CaptureViewportClient->GetGameViewport() : nullptr;
			if (Scene && Scene == ModifiedViewport)
			{
				Scene->SetFixedViewportSize(bOriginalViewportFixed ? static_cast<uint32>(OriginalViewportSize.X) : 0u,
					bOriginalViewportFixed ? static_cast<uint32>(OriginalViewportSize.Y) : 0u);
				const bool bRestored = Scene->HasFixedSize() == bOriginalViewportFixed &&
					(!bOriginalViewportFixed || Scene->GetSizeXY() == OriginalViewportSize);
				Environment->SetBoolField(TEXT("viewport_restored"), bRestored);
				if (!bRestored) { Fail(TEXT("Could not restore the owned viewport's original fixed-size policy.")); }
			}
			else
			{
				Environment->SetStringField(TEXT("viewport_restore_status"), TEXT("Owned PIE viewport already released or never overridden."));
			}
			ModifiedViewport = nullptr;
			if (PerformanceSettings.IsValid())
			{
				PerformanceSettings->bThrottleCPUWhenNotForeground = bOriginalThrottleCPU;
				Environment->SetBoolField(TEXT("background_cpu_setting_restored"),
					PerformanceSettings->bThrottleCPUWhenNotForeground == bOriginalThrottleCPU);
			}
			if (ScopedEditor.IsValid())
			{
				ScopedEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
					[this](const UEditorEngine::FShouldDisableCPUThrottling& Delegate) { return Delegate.GetHandle() == CPUThrottleHandle; });
				Environment->SetBoolField(TEXT("cpu_throttle_delegate_removed"), true);
			}
		}
		void OnPIEEnding(bool bSimulating)
		{
			if (!bFinishing) { Fail(TEXT("Owned PIE was ended before pilot verification completed.")); }
			RestoreCapturePresentation();
		}
		FCollisionQueryParams TraceParams() const
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(DublinStructuralPilot), true);
			if (Pawn.IsValid()) { Params.AddIgnoredActor(Pawn.Get()); }
			if (Camera.IsValid()) { Params.AddIgnoredActor(Camera.Get()); }
			return Params;
		}
		bool ClearSight(const FVector& Start, const FVector& End) const
		{
			FHitResult Hit;
			return !World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, TraceParams()) ||
				FVector::Dist(Hit.ImpactPoint, End) < 10.0;
		}
		bool SelectSourceView(const FDublinCityBuilding& Building)
		{
			SourcePivot = Building.PivotCm;
			TArray<TArray<FVector>> Roofs;
			TArray<TPair<FVector, FVector>> Walls;
			FVector RoofSum = FVector::ZeroVector;
			double AreaSum = 0;
			for (const FVector& Vertex : Building.Mesh.VerticesCm) { SourceBounds += Vertex + SourcePivot; }
			for (int32 Triangle = 0; Triangle < Building.MaterialIds.Num(); ++Triangle)
			{
				const FVector A = Building.Mesh.VerticesCm[Building.Mesh.Triangles[Triangle * 3]] + SourcePivot;
				const FVector B = Building.Mesh.VerticesCm[Building.Mesh.Triangles[Triangle * 3 + 1]] + SourcePivot;
				const FVector C = Building.Mesh.VerticesCm[Building.Mesh.Triangles[Triangle * 3 + 2]] + SourcePivot;
				if (Building.MaterialIds[Triangle] == 0)
				{
					const double Area = FMath::Abs(FVector::CrossProduct(B - A, C - A).Z);
					RoofSum += (A + B + C) * (Area / 3.0);
					AreaSum += Area;
					Roofs.Add({A, B, C});
				}
				else if (Building.MaterialIds[Triangle] == 1)
				{
					Walls.Emplace((A + B + C) / 3.0, DublinCity::ClockwiseNormal(A, B, C));
				}
			}
			if (!SourceBounds.IsValid || AreaSum <= 0) { Fail(TEXT("Pilot source has no valid roof footprint.")); return false; }
			FVector RoofPoint = RoofSum / AreaSum;
			bool bInside = false;
			double RoofZ = -DBL_MAX;
			for (const TArray<FVector>& Triangle : Roofs)
			{
				const FVector& A = Triangle[0]; const FVector& B = Triangle[1]; const FVector& C = Triangle[2];
				const double Den = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
				if (FMath::Abs(Den) < 0.000001) { continue; }
				const double U = ((B.X - RoofPoint.X) * (C.Y - RoofPoint.Y) - (B.Y - RoofPoint.Y) * (C.X - RoofPoint.X)) / Den;
				const double V = ((C.X - RoofPoint.X) * (A.Y - RoofPoint.Y) - (C.Y - RoofPoint.Y) * (A.X - RoofPoint.X)) / Den;
				const double W = 1.0 - U - V;
				if (U >= -0.000001 && V >= -0.000001 && W >= -0.000001)
				{
					bInside = true;
					RoofZ = FMath::Max(RoofZ, A.Z * U + B.Z * V + C.Z * W);
				}
			}
			if (!bInside) { Fail(TEXT("Area-weighted source roof centroid is outside its footprint; no substitute impact selected.")); return false; }
			RoofPoint.Z = RoofZ;
			Impact = DublinWeapons::MakeImpact(EDublinImpactKind::Bomb, 4.0f, DublinWeapons::FBombCurve());
			Impact.PositionCm = RoofPoint;
			Impact.Seed = ImpactSeed;
			FHitResult RoofHit;
			if (!World->LineTraceSingleByChannel(RoofHit, RoofPoint + FVector(0, 0, 100), RoofPoint - FVector(0, 0, 100),
				ECC_Visibility, TraceParams()) || RoofHit.GetActor() != City.Get() || FVector::Dist(RoofHit.ImpactPoint, RoofPoint) > 10.0)
			{
				Fail(TEXT("Exact source roof centroid does not match intact city collision.")); return false;
			}
			FrameBounds = SourceBounds.ExpandBy(FVector(350, 350, 400));
			const FVector Aim = FrameBounds.GetCenter();
			const double TanHorizontal = FMath::Tan(FMath::DegreesToRadians(30.0));
			const double TanVertical = TanHorizontal / (16.0 / 9.0);
			const double BaseDistance = FrameBounds.GetExtent().Size() / FMath::Sin(FMath::Atan(TanVertical)) * 1.1;
			const double HeadingOffset = FCrc::StrCrc32(Case.SourceId) % 360;
			for (double Multiplier : {1.0, 1.25, 1.5})
			{
				for (int32 Direction = 0; Direction < 16; ++Direction)
				{
					const double Angle = FMath::DegreesToRadians(HeadingOffset + Direction * 22.5);
					const double Down = FMath::DegreesToRadians(32.0);
					const double Distance = BaseDistance * Multiplier;
					const FVector Position = Aim + FVector(FMath::Cos(Angle) * FMath::Cos(Down),
						FMath::Sin(Angle) * FMath::Cos(Down), FMath::Sin(Down)) * Distance;
					if (FVector::Dist(Position, RoofPoint) < FMath::Max(Impact.RadiusCm * 3.0, DublinImpactFX::VisualRadius(Impact) * 2.0)) { continue; }
					bool bOutside = true;
					for (const FDublinCityBuilding& Other : City->GetSourceData()->Buildings)
					{
						FBox Box(ForceInit);
						for (const FVector& Vertex : Other.Mesh.VerticesCm) { Box += Vertex + Other.PivotCm; }
						if (Box.ExpandBy(80.0).IsInside(Position)) { bOutside = false; break; }
					}
					if (!bOutside || World->OverlapBlockingTestByChannel(Position, FQuat::Identity, ECC_Visibility,
						FCollisionShape::MakeSphere(80.0f), TraceParams()) || !ClearSight(Position, RoofPoint + FVector(0, 0, 20))) { continue; }
					int32 ClearRoofs = 0;
					for (const TArray<FVector>& Triangle : Roofs)
					{
						if (ClearSight(Position, (Triangle[0] + Triangle[1] + Triangle[2]) / 3.0 + FVector(0, 0, 20))) { ++ClearRoofs; }
					}
					if (ClearRoofs * 2 < Roofs.Num()) { continue; }
					for (const TPair<FVector, FVector>& Wall : Walls)
					{
						const FVector Probe = Wall.Key + Wall.Value * 20.0;
						if (FVector::DotProduct(Wall.Value, Position - Wall.Key) <= 0 || !ClearSight(Position, Probe)) { continue; }
						CameraPosition = Position;
						CameraRotation = (Aim - Position).Rotation();
						FacadeProbe = Probe;
						Manifest->SetNumberField(TEXT("source_roof_max_z_cm"), SourceBounds.Max.Z);
						Manifest->SetNumberField(TEXT("source_roof_footprint_area_m2"), AreaSum / 20000.0);
						Manifest->SetNumberField(TEXT("camera_candidate_direction"), Direction);
						Manifest->SetNumberField(TEXT("camera_distance_multiplier"), Multiplier);
						return true;
					}
				}
			}
			Fail(TEXT("No exterior 32-degree source-based view has clear roof and facade sightlines. No alternate pilot, nadir view or relocated bomb used."));
			return false;
		}
		bool ValidateView()
		{
			if (bPrepared && (!HasCaptureSize() || !PerformanceSettings.IsValid() ||
				PerformanceSettings->bThrottleCPUWhenNotForeground || (ScopedEditor.IsValid() && ScopedEditor->ShouldThrottleCPUUsage())))
			{
				Fail(TEXT("Scoped capture size or no-background-throttle policy changed during footage.")); return false;
			}
			FVector Position; FRotator Rotation;
			Player->GetPlayerViewPoint(Position, Rotation);
			if (!Position.Equals(CameraPosition, 1.0) || !Rotation.Equals(CameraRotation, 0.05))
			{
				Fail(TEXT("Actual player view diverged from the fixed owned observer.")); return false;
			}
			FSceneViewProjectionData Projection;
			ULocalPlayer* Local = Player->GetLocalPlayer();
			UGameViewportClient* Client = World->GetGameViewport();
			if (!Local || !Client || !Client->Viewport || !Local->GetProjectionData(Client->Viewport, Projection, INDEX_NONE))
			{
				Fail(TEXT("No actual rendered viewport projection for pilot capture.")); return false;
			}
			const FIntRect Rect = Projection.GetConstrainedViewRect();
			if (Rect.Width() <= 0 || Rect.Height() <= 0) { Fail(TEXT("Empty constrained viewport.")); return false; }
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Point(Corner & 1 ? FrameBounds.Max.X : FrameBounds.Min.X,
					Corner & 2 ? FrameBounds.Max.Y : FrameBounds.Min.Y, Corner & 4 ? FrameBounds.Max.Z : FrameBounds.Min.Z);
				FVector2D Pixel;
				if (!FSceneView::ProjectWorldToScreen(Point, Rect, Projection.ComputeViewProjectionMatrix(), Pixel) ||
					Pixel.ContainsNaN() || Pixel.X < Rect.Min.X + Rect.Width() * 0.03 || Pixel.X > Rect.Max.X - Rect.Width() * 0.03 ||
					Pixel.Y < Rect.Min.Y + Rect.Height() * 0.03 || Pixel.Y > Rect.Max.Y - Rect.Height() * 0.03)
				{
					Fail(TEXT("Actual camera does not frame the source building and debris margin.")); return false;
				}
			}
			Manifest->SetNumberField(TEXT("constrained_view_width"), Rect.Width());
			Manifest->SetNumberField(TEXT("constrained_view_height"), Rect.Height());
			return true;
		}
		bool Bootstrap()
		{
			if (!GEngine) { Fail(TEXT("Engine unavailable.")); return Finish(); }
			if (!ImageQueue)
			{
				if (!FPlatformProcess::SupportsMultithreading())
				{
					Fail(TEXT("Asynchronous PNG capture requires a multithreaded platform; no synchronous fallback.")); return Finish();
				}
				IImageWriteQueueModule* Module = FModuleManager::LoadModulePtr<IImageWriteQueueModule>(TEXT("ImageWriteQueue"));
				if (!Module) { Fail(TEXT("Editor ImageWriteQueue module is unavailable.")); return Finish(); }
				ImageQueue = &Module->GetWriteQueue();
				Manifest->SetNumberField(TEXT("shared_image_queue_tasks_at_entry"), ImageQueue->GetNumPendingTasks());
			}
			int32 PIEWorlds = 0;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->WorldType == EWorldType::PIE && !Context.World()->bIsTearingDown) { ++PIEWorlds; }
			}
			if (PIEWorlds > 1)
			{
				World = GEditor ? GEditor->PlayWorld : nullptr;
				Fail(TEXT("Pilot requires a single-player PIE session, not multiple PIE worlds.")); return Finish();
			}
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (!Candidate || Candidate->WorldType != EWorldType::PIE || Candidate->bIsTearingDown) { continue; }
				if (World.IsValid() && World.Get() != Candidate) { Fail(TEXT("Multiple PIE worlds are not supported.")); return Finish(); }
				World = Candidate;
				if (UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) != TEXT("/Game/Maps/Dublin"))
				{
					Fail(TEXT("Fixture did not start saved Dublin PIE.")); return Finish();
				}
				for (TActorIterator<ADublinCityWorld> It(Candidate); It; ++It)
				{
					if (City.IsValid() && City.Get() != *It) { Fail(TEXT("PIE contains multiple city actors.")); return Finish(); }
					City = *It;
				}
				Player = Candidate->GetFirstPlayerController();
				Pawn = Player.IsValid() ? Cast<ADublinFlightPawn>(Player->GetPawn()) : nullptr;
				UGameViewportClient* Client = Candidate->GetGameViewport();
				if (City.IsValid() && City->bAllowPartialBakePreview)
				{
					Fail(TEXT("Partial-bake preview must be off for structural pilot comparison.")); return Finish();
				}
				if (!Candidate->HasBegunPlay() || Candidate->IsPaused() || !City.IsValid() || !City->bCityReady ||
					!City->bDestructionReady || !City->bAllBuildingsFractureReady || !City->GetSourceData() || !City->FractureLibrary ||
					!Player.IsValid() || !Player->IsLocalController() || !Player->PlayerCameraManager || !Pawn.IsValid() ||
					!Pawn->bSpawnCaptured || !Pawn->Weapons || !Client || !Client->Viewport || Client->Viewport->GetSizeXY().GetMin() <= 0) { continue; }
				if (City->bAllowPartialBakePreview || City->AcceptedImpactCount || City->FracturedBuildingCount ||
					City->QueuedImpactCount || City->QueuedBuildingHitCount || City->CraterChangedNodeCount ||
					City->WaterActiveNodeCount || Pawn->Weapons->CannonShots || Pawn->Weapons->BombsDropped)
				{
					Fail(TEXT("Pilot requires a fresh, fully ready, undamaged city with partial preview off.")); return Finish();
				}
				TInlineComponentArray<UGeometryCollectionComponent*> ExistingCollections;
				City->GetComponents(ExistingCollections);
				for (UGeometryCollectionComponent* Existing : ExistingCollections)
				{
					if (IsValid(Existing) && Existing->IsRegistered() && Existing->ComponentHasTag(TEXT("DublinFlight.City.Fracture.v1")))
					{
						Fail(TEXT("Fresh pilot contains an existing fracture component despite its counters.")); return Finish();
					}
				}
				const FDublinCityBuilding* Building = City->GetSourceData()->Buildings.FindByPredicate(
					[this](const FDublinCityBuilding& Item) { return Item.Id == Case.SourceId; });
				const FDublinFractureRecord* Found = City->FractureLibrary->Find(Case.SourceId);
				if (!Building || !Found || !Found->bReady || Found->SourceId != Case.SourceId || Found->PieceCount < 2 ||
					Found->PieceCount > UDublinCityFractureLibrary::MaxPiecesPerBuilding || Found->LeafTransforms.Num() != Found->PieceCount ||
					Found->Collection.IsNull() || !FPackageName::DoesPackageExist(Found->Collection.ToSoftObjectPath().GetLongPackageName()))
				{
					Fail(TEXT("Exact pilot source/ready saved fracture record is missing or invalid; no alternative selected.")); return Finish();
				}
				Record = *Found;
				if (Record.Recipe != EDublinFractureRecipe::SolidGrid && Record.Recipe != EDublinFractureRecipe::StructuralPilot)
				{
					Fail(TEXT("Unsupported fracture recipe: no implicit legacy baseline classification.")); return Finish();
				}
				bLegacyBaseline = Record.Recipe == EDublinFractureRecipe::SolidGrid;
				bStructuralAcceptanceAssessed = Record.Recipe == EDublinFractureRecipe::StructuralPilot;
				if (bLegacyBaseline)
				{
					Test.AddInfo(TEXT("SolidGrid baseline: this run assesses recording quality and safety guards, not StructuralPilot physics acceptance."));
				}
				if (City->FractureLibrary->GetOutermost()->IsDirty() ||
					(Record.Collection.Get() && Record.Collection.Get()->GetOutermost()->IsDirty()))
				{
					Fail(TEXT("Save the fracture library and pilot collection before starting a comparison.")); return Finish();
				}
				if (Record.SourceDigest.IsEmpty() || FPaths::MakeValidFileName(Record.SourceDigest) != Record.SourceDigest)
				{
					Fail(TEXT("Pilot SourceDigest is not a valid artifact-directory key.")); return Finish();
				}
				Directory = MakeDirectory(Record.SourceDigest);
				Manifest->SetStringField(TEXT("source_digest"), Record.SourceDigest);
				Manifest->SetStringField(TEXT("record_geometry_digest"), Record.GeometryDigest);
				Manifest->SetStringField(TEXT("source_geometry_digest"), DublinDestruction::BuildingGeometryDigest(*Building));
				Manifest->SetStringField(TEXT("collection"), Record.Collection.ToSoftObjectPath().ToString());
				Manifest->SetNumberField(TEXT("piece_count"), Record.PieceCount);
				Manifest->SetNumberField(TEXT("library_bake_version"), City->FractureLibrary->BakeVersion);
				Manifest->SetNumberField(TEXT("source_volume_cm3"), Record.SourceVolumeCm3);
				Manifest->SetNumberField(TEXT("retained_volume_cm3"), Record.RetainedVolumeCm3);
				Manifest->SetField(TEXT("record_recipe"), MakeShared<FJsonValueNull>());
				if (const FProperty* Recipe = FindFProperty<FProperty>(FDublinFractureRecord::StaticStruct(), TEXT("Recipe")))
				{
					FString RecipeText;
					Recipe->ExportTextItem_Direct(RecipeText, Recipe->ContainerPtrToValuePtr<void>(&Record), nullptr, nullptr, PPF_None);
					Manifest->SetStringField(TEXT("record_recipe"), RecipeText);
				}
				Manifest->SetField(TEXT("structural_volume_cm3"), MakeShared<FJsonValueNull>());
				if (const FDoubleProperty* Volume = FindFProperty<FDoubleProperty>(FDublinFractureRecord::StaticStruct(), TEXT("StructuralVolumeCm3")))
				{
					const double Value = Volume->GetPropertyValue_InContainer(&Record);
					if (!FMath::IsFinite(Value)) { Fail(TEXT("Record structural volume is non-finite.")); return Finish(); }
					Manifest->SetNumberField(TEXT("structural_volume_cm3"), Value);
				}
				TArray<TSharedPtr<FJsonValue>> Attribution;
				for (const FString& Credit : City->GetSourceData()->Attribution) { Attribution.Add(MakeShared<FJsonValueString>(Credit)); }
				Manifest->SetArrayField(TEXT("attribution"), Attribution);
				if (!City->GetActorTransform().Equals(FTransform::Identity, 0.01))
				{
					Fail(TEXT("Source-based pilot coordinates require the city at its authored identity transform.")); return Finish();
				}
				if (!SelectSourceView(*Building)) { return Finish(); }
				PreviousViewTarget = Player->GetViewTarget();
				if (!PreviousViewTarget.IsValid()) { Fail(TEXT("No previous view target to restore.")); return Finish(); }
				bPawnTickWasEnabled = Pawn->IsActorTickEnabled();
				Pawn->Weapons->SuppressInput();
				Pawn->SetActorTickEnabled(false);
				Player->SetIgnoreMoveInput(true);
				Player->SetIgnoreLookInput(true);
				bInputSuppressed = true;
				Camera = Candidate->SpawnActor<ACameraActor>();
				if (!Camera.IsValid()) { Fail(TEXT("Could not spawn owned pilot observer.")); return Finish(); }
				Camera->SetActorLocationAndRotation(CameraPosition, CameraRotation);
				Camera->GetCameraComponent()->SetFieldOfView(60.0f);
				Camera->GetCameraComponent()->SetAspectRatio(16.0f / 9.0f);
				Camera->GetCameraComponent()->SetConstraintAspectRatio(true);
				Player->SetViewTarget(Camera.Get());
				Player->PlayerCameraManager->UpdateCamera(0.0f);
				if (!SetCapturePresentation(*Client)) { return Finish(); }
				VectorField(*Manifest, TEXT("source_bounds_min_cm"), SourceBounds.Min);
				VectorField(*Manifest, TEXT("source_bounds_max_cm"), SourceBounds.Max);
				VectorField(*Manifest, TEXT("roof_centroid_impact_cm"), Impact.PositionCm);
				VectorField(*Manifest, TEXT("camera_position_cm"), CameraPosition);
				VectorField(*Manifest, TEXT("camera_rotation_pitch_yaw_roll"), FVector(CameraRotation.Pitch, CameraRotation.Yaw, CameraRotation.Roll));
				VectorField(*Manifest, TEXT("clear_facade_probe_cm"), FacadeProbe);
				Manifest->SetNumberField(TEXT("camera_fov_degrees"), 60);
				Manifest->SetNumberField(TEXT("impact_seed"), Impact.Seed);
				Manifest->SetNumberField(TEXT("yield_game_tons"), Impact.YieldTonsTNT);
				Manifest->SetNumberField(TEXT("impact_radius_cm"), Impact.RadiusCm);
				Manifest->SetNumberField(TEXT("impact_strength"), Impact.Strength);
				bPrepared = true;
				SaveManifest();
				Test.AddInfo(TEXT("Structural pilot capture: ") + Directory + TEXT(". Accepted native bomb route; visual inspection required, not B-key/drop or FPS evidence."));
				return bFailed ? Finish() : false;
			}
			return false;
		}
		void ObserveLeaves()
		{
			if (!Collection.IsValid())
			{
				TInlineComponentArray<UGeometryCollectionComponent*> Components;
				City->GetComponents(Components);
				for (UGeometryCollectionComponent* Component : Components)
				{
					if (!IsValid(Component) || Component->GetOwner() != City.Get() ||
						!Component->ComponentHasTag(TEXT("DublinFlight.City.Fracture.v1")) || !Component->GetRestCollection() ||
						Component->GetRestCollection()->GetPathName() != Record.Collection.ToSoftObjectPath().ToString() ||
						!Component->GetRelativeLocation().Equals(SourcePivot, 0.1)) { continue; }
					if (Collection.IsValid()) { Fail(TEXT("Multiple components match the exact pilot collection.")); return; }
					Collection = Component;
					Manifest->SetNumberField(TEXT("target_activation_world_seconds"), World->GetTimeSeconds());
				}
			}
			if (!Collection.IsValid()) { return; }
			if (!Collection->IsRegistered() || !Collection->IsVisible() || !Collection->GetPhysicsProxy() ||
				!Collection->IsPhysicsStateCreated() || !Collection->IsRenderStateCreated() ||
				Collection->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics)
			{
				Fail(TEXT("Exact pilot collection lacks live visible physics/render/collision state.")); return;
			}
			const TArray<FTransform> Current = Collection->GetCurrentTransforms();
			const TArray<FTransform> Rest = Collection->GetInitialLocalRestTransforms();
			TArray<FTransform> ThisFrame;
			for (int32 Leaf : Record.LeafTransforms)
			{
				if (!Current.IsValidIndex(Leaf) || !Rest.IsValidIndex(Leaf) || Current[Leaf].ContainsNaN() ||
					Rest[Leaf].ContainsNaN() || Current[Leaf].GetScale3D().GetAbsMin() < 0.01)
				{
					Fail(TEXT("Pilot leaf transform is missing, invalid or invisibly scaled.")); return;
				}
				ThisFrame.Add(Current[Leaf] * Collection->GetComponentTransform());
				if (InitialLeaves.Num() < Record.PieceCount) { InitialLeaves.Add(Rest[Leaf] * Collection->GetComponentTransform()); }
			}
			bool bDescending = false;
			for (int32 Index = 0; Index < ThisFrame.Num(); ++Index)
			{
				const FVector Position = ThisFrame[Index].GetLocation();
				MaxTravel = FMath::Max(MaxTravel, FVector::Dist(Position, InitialLeaves[Index].GetLocation()));
				MaxDrop = FMath::Max(MaxDrop, InitialLeaves[Index].GetLocation().Z - Position.Z);
				MaxSeparationChange = FMath::Max(MaxSeparationChange, FMath::Abs(
					FVector::Dist(Position, ThisFrame[0].GetLocation()) -
					FVector::Dist(InitialLeaves[Index].GetLocation(), InitialLeaves[0].GetLocation())));
				if (PreviousLeaves.IsValidIndex(Index) && PreviousLeaves[Index].GetLocation().Z - Position.Z > 1.0) { bDescending = true; }
			}
			if (bDescending) { ++DescendingFrames; }
			PreviousLeaves = ThisFrame;
			LatestLeaves = MoveTemp(ThisFrame);
		}
		void RequestFrame(double Now, bool bPrime = false)
		{
			if (!CanIssueScreenshotThisFrame())
			{
				Fail(TEXT("Screenshot request attempted in a frame still servicing the preceding screenshot."));
				return;
			}
			const IConsoleVariable* UseDelegate = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenshotDelegate"));
			if (!ImageQueue || !UseDelegate || UseDelegate->GetInt() == 0 || UGameViewportClient::OnScreenshotCaptured().IsBound() ||
				Pending.IsValid() || FScreenshotRequest::IsScreenshotRequested() || PendingWrites.Num() >= MaxPendingWrites)
			{
				Fail(FString::Printf(TEXT("Async screenshot unavailable: writer=%d delegateCVar=%d listenerBound=%d requestPending=%d copies=%d/%d. Nothing overwritten."),
					ImageQueue != nullptr, UseDelegate ? UseDelegate->GetInt() : -1, UGameViewportClient::OnScreenshotCaptured().IsBound(),
					FScreenshotRequest::IsScreenshotRequested(), PendingWrites.Num(), MaxPendingWrites));
				return;
			}
			PendingPath = FPaths::Combine(Directory, bPrime ? FString::Printf(TEXT("priming-%02d.png"), PrimingRequested) :
				FString::Printf(TEXT("frame-%04d.png"), NextSlot));
			if (IFileManager::Get().FileExists(*PendingPath)) { Fail(TEXT("Refusing to overwrite an existing capture: ") + PendingPath); return; }
			Pending = MakeShared<FJsonObject>();
			Pending->SetNumberField(TEXT("slot"), bPrime ? PrimingRequested : NextSlot);
			Pending->SetStringField(TEXT("file"), FPaths::GetCleanFilename(PendingPath));
			if (bPrime) { Pending->SetField(TEXT("scheduled_elapsed_seconds"), MakeShared<FJsonValueNull>()); }
			else { Pending->SetNumberField(TEXT("scheduled_elapsed_seconds"), ScheduledElapsed(NextSlot)); }
			Pending->SetNumberField(TEXT("requested_world_seconds"), Now);
			if (bPrime) { Pending->SetField(TEXT("requested_elapsed_seconds"), MakeShared<FJsonValueNull>()); }
			else { Pending->SetNumberField(TEXT("requested_elapsed_seconds"), Now - CaptureStart); }
			Pending->SetNumberField(TEXT("requested_wall_elapsed_seconds"), FPlatformTime::Seconds() - StartedWall);
			Pending->SetNumberField(TEXT("requested_engine_frame"), static_cast<double>(GFrameCounter));
			if (bObservedScreenshotService)
			{
				Pending->SetNumberField(TEXT("previous_screenshot_service_engine_frame"), static_cast<double>(LastScreenshotServiceFrame));
			}
			else { Pending->SetField(TEXT("previous_screenshot_service_engine_frame"), MakeShared<FJsonValueNull>()); }
			Pending->SetNumberField(TEXT("unmatched_processed_callbacks"), 0);
			Pending->SetStringField(TEXT("phase"), bPrime ? TEXT("warmup") : (bImpact ? TEXT("after_impact") : TEXT("intact")));
			Pending->SetNumberField(TEXT("max_leaf_travel_cm"), MaxTravel);
			Pending->SetNumberField(TEXT("max_leaf_drop_cm"), MaxDrop);
			Pending->SetNumberField(TEXT("active_collections"), City->ActiveFractureCollections);
			Pending->SetNumberField(TEXT("active_pieces"), City->ActiveFracturePieces);
			Pending->SetBoolField(TEXT("target_collection_observed"), Collection.IsValid());
			Pending->SetBoolField(TEXT("processed"), false);
			Pending->SetBoolField(TEXT("captured"), false);
			Pending->SetBoolField(TEXT("dropped"), false);
			Pending->SetBoolField(TEXT("file_written"), false);
			Pending->SetBoolField(TEXT("write_queued"), false);
			Pending->SetField(TEXT("captured_world_seconds"), MakeShared<FJsonValueNull>());
			Pending->SetField(TEXT("processed_world_seconds"), MakeShared<FJsonValueNull>());
			Pending->SetField(TEXT("processed_elapsed_seconds"), MakeShared<FJsonValueNull>());
			if (bPrime)
			{
				PrimingFrames.Add(MakeShared<FJsonValueObject>(Pending));
				++PrimingRequested;
			}
			else
			{
				Frames.Add(MakeShared<FJsonValueObject>(Pending));
				++NextSlot;
			}
			PendingWall = FPlatformTime::Seconds();
			PendingRequestEngineFrame = GFrameCounter;
			bProcessed = false;
			bCaptured = false;
			bPendingDropped = false;
			bScreenMessagesBeforeRequest = GAreScreenMessagesEnabled;
			CapturedHandle = UGameViewportClient::OnScreenshotCaptured().AddRaw(this, &FCapture::ScreenshotCaptured);
			FScreenshotRequest::RequestScreenshot(PendingPath, false, false, false, FIntRect(), true);
		}
		void ScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Bitmap)
		{
			check(IsInGameThread());
			if (bCleaned) { return; }
			ObserveScreenshotService();
			if (!Pending.IsValid() || bCaptured || bPendingDropped) { return; }
			Pending->SetNumberField(TEXT("readback_width"), Width);
			Pending->SetNumberField(TEXT("readback_height"), Height);
			Pending->SetNumberField(TEXT("readback_pixel_count"), Bitmap.Num());
			if (!World.IsValid() || FPaths::ConvertRelativePathToFull(FScreenshotRequest::GetFilename()) != PendingPath ||
				FIntPoint(Width, Height) != CaptureSize || Bitmap.Num() != int64(Width) * Height ||
				PendingWrites.Num() >= MaxPendingWrites)
			{
				Fail(TEXT("Screenshot readback ownership, dimensions or bounded-copy contract failed.")); return;
			}
			bCaptured = true;
			const bool bPrime = Pending->GetStringField(TEXT("phase")) == TEXT("warmup");
			if (bPrime) { ++PrimingCaptured; }
			else
			{
				++CapturedFrames;
				if (Pending->GetStringField(TEXT("phase")) == TEXT("intact")) { ++CapturedBeforeFrames; }
			}
			Pending->SetBoolField(TEXT("captured"), true);
			Pending->SetNumberField(TEXT("captured_world_seconds"), World->GetTimeSeconds());
			if (bPrime) { Pending->SetField(TEXT("captured_elapsed_seconds"), MakeShared<FJsonValueNull>()); }
			else { Pending->SetNumberField(TEXT("captured_elapsed_seconds"), World->GetTimeSeconds() - CaptureStart); }
			Pending->SetNumberField(TEXT("captured_wall_elapsed_seconds"), FPlatformTime::Seconds() - StartedWall);
			Pending->SetNumberField(TEXT("captured_engine_frame"), static_cast<double>(GFrameCounter));
			TArray64<FColor> Pixels;
			Pixels.Append(Bitmap.GetData(), Bitmap.Num());
			TUniquePtr<FImageWriteTask> Task = MakeUnique<FImageWriteTask>();
			Task->Filename = PendingPath;
			Task->Format = ImageFormatFromDesired(EDesiredImageFormat::PNG);
			Task->bOverwriteFile = false;
			Task->PixelData = MakeUnique<TImagePixelData<FColor>>(CaptureSize, MoveTemp(Pixels));
			// The task owns only pixels and its unique filename. No UObject/test/JSON callbacks run on the worker.
			TFuture<bool> Result = ImageQueue->Enqueue(MoveTemp(Task), false);
			if (!Result.IsValid()) { Fail(TEXT("Shared ImageWriteQueue rejected a nonblocking enqueue.")); return; }
			FPendingWrite& Write = PendingWrites.AddDefaulted_GetRef();
			Write.Result = MoveTemp(Result);
			Write.Frame = Pending;
			Write.Path = PendingPath;
			Write.StartedWall = FPlatformTime::Seconds();
			Pending->SetBoolField(TEXT("write_queued"), true);
			Pending->SetNumberField(TEXT("write_enqueued_wall_elapsed_seconds"), Write.StartedWall - StartedWall);
			PeakPendingWrites = FMath::Max(PeakPendingWrites, PendingWrites.Num());
		}
		void ScreenshotProcessed()
		{
			check(IsInGameThread());
			if (bCleaned) { return; }
			ObserveScreenshotService();
			if (!Pending.IsValid() || bProcessed || bPendingDropped) { return; }
			// UE resets the global filename before this identity-free notification.
			// Only our matching color readback can establish which request was serviced.
			if (!bCaptured)
			{
				++UnmatchedProcessedCallbacks;
				Pending->SetNumberField(TEXT("unmatched_processed_callbacks"), Pending->GetNumberField(TEXT("unmatched_processed_callbacks")) + 1);
				Pending->SetNumberField(TEXT("last_unmatched_processed_engine_frame"), static_cast<double>(GFrameCounter));
				const bool bRequestPending = FScreenshotRequest::IsScreenshotRequested();
				const FString RequestFilename = FScreenshotRequest::GetFilename();
				Pending->SetBoolField(TEXT("request_pending_at_unmatched_callback"), bRequestPending);
				Pending->SetStringField(TEXT("request_filename_at_unmatched_callback"), RequestFilename);
				if (!bRequestPending && RequestFilename.IsEmpty())
				{
					DropClearedReadback(TEXT("processed_notification_cleared_request_without_owned_pixels"));
				}
				return;
			}
			UGameViewportClient::OnScreenshotCaptured().Remove(CapturedHandle);
			CapturedHandle.Reset();
			bProcessed = true;
			Pending->SetBoolField(TEXT("processed"), true);
			Pending->SetNumberField(TEXT("processed_engine_frame"), static_cast<double>(GFrameCounter));
			Pending->SetNumberField(TEXT("processed_wall_elapsed_seconds"), FPlatformTime::Seconds() - StartedWall);
			if (World.IsValid())
			{
				const double Now = World->GetTimeSeconds();
				Pending->SetNumberField(TEXT("processed_world_seconds"), Now);
				if (Pending->GetStringField(TEXT("phase")) != TEXT("warmup"))
				{
					Pending->SetNumberField(TEXT("processed_elapsed_seconds"), Now - CaptureStart);
					if (LastProcessedWorld >= 0) { MaxProcessedGap = FMath::Max(MaxProcessedGap, Now - LastProcessedWorld); }
					LastProcessedWorld = Now;
				}
			}
		}
		void PollScreenshot()
		{
			if (!Pending.IsValid()) { return; }
			if (!bPendingDropped && !bCaptured && !bProcessed && GFrameCounter > PendingRequestEngineFrame &&
				!FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename().IsEmpty())
			{
				DropClearedReadback(TEXT("poll_observed_cleared_request_without_owned_pixels"));
			}
			if (bPendingDropped)
			{
				Pending.Reset();
				return;
			}
			if (!bProcessed && FScreenshotRequest::IsScreenshotRequested() &&
				FPaths::ConvertRelativePathToFull(FScreenshotRequest::GetFilename()) != PendingPath)
			{
				Fail(TEXT("Another caller replaced this fixture's screenshot request.")); return;
			}
			if (bProcessed)
			{
				if (!bCaptured) { Fail(TEXT("Screenshot request was processed without the owned color readback; synchronous/HDR fallback is not accepted.")); }
				Pending.Reset();
			}
			else if (FPlatformTime::Seconds() - PendingWall > 5.0)
			{
				Fail(TEXT("Requested screenshot did not deliver a readback within five seconds: ") + PendingPath);
			}
		}
		void DropClearedReadback(const TCHAR* Reason)
		{
			if (!Pending.IsValid() || bPendingDropped || bCaptured || bFailed) { return; }
			if (FScreenshotRequest::IsScreenshotRequested() || !FScreenshotRequest::GetFilename().IsEmpty()) { return; }
			bPendingDropped = true;
			ObserveScreenshotService();
			UGameViewportClient::OnScreenshotCaptured().Remove(CapturedHandle);
			CapturedHandle.Reset();
			GAreScreenMessagesEnabled = bScreenMessagesBeforeRequest;
			++DroppedReadbacks;
			Pending->SetBoolField(TEXT("dropped"), true);
			Pending->SetStringField(TEXT("drop_reason"), Reason);
			Pending->SetBoolField(TEXT("request_pending_at_drop"), FScreenshotRequest::IsScreenshotRequested());
			Pending->SetStringField(TEXT("request_filename_at_drop"), FScreenshotRequest::GetFilename());
			Pending->SetNumberField(TEXT("dropped_engine_frame"), static_cast<double>(GFrameCounter));
			Pending->SetNumberField(TEXT("dropped_wall_elapsed_seconds"), FPlatformTime::Seconds() - StartedWall);
			if (World.IsValid()) { Pending->SetNumberField(TEXT("dropped_world_seconds"), World->GetTimeSeconds()); }
			if (Pending->GetStringField(TEXT("phase")) == TEXT("warmup")) { ++PrimingDropped; }
			else { MissedSlots.Add(MakeShared<FJsonValueNumber>(Pending->GetNumberField(TEXT("slot")))); }
			// Do not update captured/written counts or the last successful timestamp: the gap remains real.
			Test.AddWarning(TEXT("Dropped screenshot without owned pixels: ") + PendingPath + TEXT("; ") + Reason);
			if (DroppedReadbacks > MaxDroppedReadbacks)
			{
				Fail(TEXT("Cleared-request readback drops exceeded the four-drop budget; no further retries."));
			}
		}
		bool VerifyWrittenFrame(FPendingWrite& Write)
		{
			const int64 Bytes = IFileManager::Get().FileSize(*Write.Path);
			TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Write.Path));
			uint8 Header[24] = {};
			if (Bytes <= 0 || !Reader || Reader->TotalSize() < static_cast<int64>(sizeof(Header)))
			{
				Fail(TEXT("Asynchronous writer reported success without a complete PNG: ") + Write.Path); return false;
			}
			Reader->Serialize(Header, sizeof(Header));
			const uint8 Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
			const uint32 Width = (uint32(Header[16]) << 24) | (uint32(Header[17]) << 16) | (uint32(Header[18]) << 8) | Header[19];
			const uint32 Height = (uint32(Header[20]) << 24) | (uint32(Header[21]) << 16) | (uint32(Header[22]) << 8) | Header[23];
			if (Reader->IsError() || FMemory::Memcmp(Header, Signature, sizeof(Signature)) != 0 ||
				FMemory::Memcmp(Header + 12, "IHDR", 4) != 0 || Width != uint32(CaptureSize.X) || Height != uint32(CaptureSize.Y))
			{
				Fail(TEXT("Actual PNG dimensions/format differ from the fixed 1280x720 capture contract: ") + Write.Path); return false;
			}
			Write.Frame->SetNumberField(TEXT("png_width"), Width);
			Write.Frame->SetNumberField(TEXT("png_height"), Height);
			Write.Frame->SetBoolField(TEXT("file_written"), true);
			Write.Frame->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
			if (World.IsValid()) { Write.Frame->SetNumberField(TEXT("file_observed_world_seconds"), World->GetTimeSeconds()); }
			if (Write.Frame->GetStringField(TEXT("phase")) == TEXT("warmup"))
			{
				++PrimingWritten;
				return true;
			}
			++WrittenFrames;
			const int32 Slot = static_cast<int32>(Write.Frame->GetNumberField(TEXT("slot")));
			if (Write.Frame->GetStringField(TEXT("phase")) == TEXT("intact"))
			{
				if (FirstIntactSlot == INDEX_NONE || Slot < FirstIntactSlot)
				{
					FirstIntactSlot = Slot;
					Manifest->SetStringField(TEXT("intact_keyframe_file"), FPaths::GetCleanFilename(Write.Path));
				}
				++BeforeFrames;
			}
			else
			{
				if (Slot > LastAfterSlot)
				{
					LastAfterSlot = Slot;
					Manifest->SetStringField(TEXT("aftermath_keyframe_file"), FPaths::GetCleanFilename(Write.Path));
				}
				++AfterFrames;
			}
			return true;
		}
		void PollWrites()
		{
			bool bChanged = false;
			for (int32 Index = PendingWrites.Num() - 1; Index >= 0; --Index)
			{
				FPendingWrite& Write = PendingWrites[Index];
				if (!Write.Result.IsReady())
				{
					if (!Write.bTimeoutReported && FPlatformTime::Seconds() - Write.StartedWall > WriteTimeoutSeconds)
					{
						Write.bTimeoutReported = true;
						Write.Frame->SetBoolField(TEXT("write_timed_out"), true);
						Fail(TEXT("Asynchronous PNG task exceeded its ten-second wall bound: ") + Write.Path);
					}
					continue;
				}
				const bool bSuccess = Write.Result.Get();
				Write.Frame->SetBoolField(TEXT("write_future_succeeded"), bSuccess);
				Write.Frame->SetNumberField(TEXT("write_completion_observed_wall_elapsed_seconds"), FPlatformTime::Seconds() - StartedWall);
				if (!bSuccess) { Fail(TEXT("Asynchronous PNG compression/write failed: ") + Write.Path); }
				else { VerifyWrittenFrame(Write); }
				PendingWrites.RemoveAt(Index);
				bChanged = true;
			}
			if (bChanged && !bFinishing && WrittenFrames % 16 == 0) { SaveManifest(); }
		}
		void SaveManifest()
		{
			const bool bAutomationErrors = Test.HasAnyErrors();
			const TCHAR* SuccessStatus = bLegacyBaseline ? TEXT("baseline_captured_no_structural_acceptance") :
				TEXT("native_checks_passed_visual_inspection_required");
			Manifest->SetStringField(TEXT("status"), (bFailed || bAutomationErrors) ? TEXT("failed") :
				(bFinishing ? SuccessStatus :
					(bDraining ? TEXT("draining_queue") : (bCaptureComplete ? TEXT("flushing_images") :
						(bStarted ? TEXT("capturing") : TEXT("preparing"))))));
			Manifest->SetBoolField(TEXT("structural_acceptance_assessed"), bStructuralAcceptanceAssessed);
			Manifest->SetBoolField(TEXT("motion_gate_evaluated"), bMotionGateEvaluated);
			Manifest->SetBoolField(TEXT("motion_gate_passed"), MotionGatePassed());
			Manifest->SetBoolField(TEXT("automation_errors_present"), bAutomationErrors);
			Manifest->SetNumberField(TEXT("wall_elapsed_seconds"), FPlatformTime::Seconds() - StartedWall);
			Manifest->SetNumberField(TEXT("written_frames"), WrittenFrames);
			Manifest->SetNumberField(TEXT("captured_frames"), CapturedFrames);
			Manifest->SetNumberField(TEXT("captured_intact_frames"), CapturedBeforeFrames);
			Manifest->SetNumberField(TEXT("priming_captured_frames"), PrimingCaptured);
			Manifest->SetNumberField(TEXT("priming_written_frames"), PrimingWritten);
			Manifest->SetNumberField(TEXT("priming_requested_frames"), PrimingRequested);
			Manifest->SetNumberField(TEXT("priming_dropped_frames"), PrimingDropped);
			Manifest->SetNumberField(TEXT("pending_async_writes"), PendingWrites.Num());
			Manifest->SetNumberField(TEXT("peak_pending_async_writes"), PeakPendingWrites);
			Manifest->SetNumberField(TEXT("writer_backpressure_observations"), WriterBackpressureObservations);
			Manifest->SetNumberField(TEXT("unmatched_processed_callbacks"), UnmatchedProcessedCallbacks);
			Manifest->SetNumberField(TEXT("dropped_readbacks"), DroppedReadbacks);
			Manifest->SetNumberField(TEXT("intact_frames"), BeforeFrames);
			Manifest->SetNumberField(TEXT("after_impact_frames"), AfterFrames);
			Manifest->SetNumberField(TEXT("max_processed_gap_world_seconds"), MaxProcessedGap);
			Manifest->SetNumberField(TEXT("max_leaf_travel_cm"), MaxTravel);
			Manifest->SetNumberField(TEXT("max_leaf_drop_cm"), MaxDrop);
			Manifest->SetNumberField(TEXT("max_leaf_separation_change_cm"), MaxSeparationChange);
			Manifest->SetNumberField(TEXT("descending_world_frames"), DescendingFrames);
			Manifest->SetArrayField(TEXT("initial_leaf_world_transforms"), LeafJson(InitialLeaves, Record.LeafTransforms));
			Manifest->SetArrayField(TEXT("last_leaf_world_transforms"), LeafJson(LatestLeaves, Record.LeafTransforms));
			Manifest->SetArrayField(TEXT("frames"), Frames);
			Manifest->SetArrayField(TEXT("priming_frames"), PrimingFrames);
			Manifest->SetArrayField(TEXT("missed_slots"), MissedSlots);
			Manifest->SetArrayField(TEXT("errors"), Errors);
			if (City.IsValid())
			{
				Manifest->SetNumberField(TEXT("accepted_impacts"), City->AcceptedImpactCount);
				Manifest->SetNumberField(TEXT("queued_impacts"), City->QueuedImpactCount);
				Manifest->SetNumberField(TEXT("queued_building_hits"), City->QueuedBuildingHitCount);
				Manifest->SetNumberField(TEXT("active_collections"), City->ActiveFractureCollections);
				Manifest->SetNumberField(TEXT("active_pieces"), City->ActiveFracturePieces);
				Manifest->SetNumberField(TEXT("fractured_buildings"), City->FracturedBuildingCount);
				Manifest->SetBoolField(TEXT("queue_blocked"), City->bImpactQueueBlocked);
			}
			FString Json;
			if (!FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Json)) ||
				!IFileManager::Get().MakeDirectory(*Directory, true) ||
				!FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Directory, TEXT("manifest.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Fail(TEXT("Could not persist pilot capture manifest: ") + Directory);
			}
		}
		void Cleanup()
		{
			if (bCleaned) { return; }
			bCleaned = true;
			if (Player.IsValid())
			{
				if (PreviousViewTarget.IsValid()) { Player->SetViewTarget(PreviousViewTarget.Get()); }
				if (bInputSuppressed) { Player->SetIgnoreMoveInput(false); Player->SetIgnoreLookInput(false); }
			}
			if (Pawn.IsValid() && bInputSuppressed) { Pawn->SetActorTickEnabled(bPawnTickWasEnabled); }
			if (Camera.IsValid()) { Camera->Destroy(); }
			UGameViewportClient::OnScreenshotCaptured().Remove(CapturedHandle);
			CapturedHandle.Reset();
			if (Pending.IsValid() && FScreenshotRequest::IsScreenshotRequested() &&
				FPaths::ConvertRelativePathToFull(FScreenshotRequest::GetFilename()) == PendingPath)
			{
				Pending->SetBoolField(TEXT("cancelled_during_cleanup"), true);
				FScreenshotRequest::Reset();
				GAreScreenMessagesEnabled = bScreenMessagesBeforeRequest;
			}
			RestoreCapturePresentation();
			if (GEditor && World.IsValid() && GEditor->PlayWorld == World.Get()) { GEditor->RequestEndPlayMap(); }
		}
		bool Finish()
		{
			if (!bFinishing)
			{
				bFinishing = true;
				FinishWall = FPlatformTime::Seconds();
				Cleanup();
				SaveManifest();
				Test.AddInfo(TEXT("Pilot manifest (visual inspection still required): ") + FPaths::Combine(Directory, TEXT("manifest.json")));
			}
			return Finalize();
		}
		bool Finalize()
		{
			PollWrites();
			const double Now = FPlatformTime::Seconds();
			const bool bDeadline = Now - FinishWall >= 5.0 || Now - StartedWall >= 90.0;
			const bool bWorldEnded = !GEditor || !World.IsValid() || GEditor->PlayWorld != World.Get();
			if (bDeadline && !bFinalTimeoutReported)
			{
				bFinalTimeoutReported = true;
				if (!bWorldEnded) { Fail(TEXT("Owned PIE did not end within its bounded teardown interval.")); }
				if (!PendingWrites.IsEmpty())
				{
					for (FPendingWrite& Write : PendingWrites) { Write.Frame->SetBoolField(TEXT("write_unfinished_at_exit"), true); }
					Fail(TEXT("Asynchronous PNG tasks remain incomplete at bounded teardown; their eventual files are not accepted evidence."));
				}
			}
			if ((bWorldEnded && PendingWrites.IsEmpty()) || bDeadline)
			{
				SaveManifest();
				return true;
			}
			return false;
		}
	};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDublinStructuralPilotPIETest,
	"DublinFlight.Destruction.PIE.StructuralPilot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FDublinStructuralPilotPIETest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const DublinStructuralPilot::FCase& Case : DublinStructuralPilot::Cases)
	{
		OutBeautifiedNames.Add(Case.Name);
		OutTestCommands.Add(Case.SourceId);
	}
}

bool FDublinStructuralPilotPIETest::RunTest(const FString& Parameters)
{
	const DublinStructuralPilot::FCase* Selected = nullptr;
	for (const DublinStructuralPilot::FCase& Case : DublinStructuralPilot::Cases)
	{
		if (Parameters == Case.SourceId) { Selected = &Case; break; }
	}
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!Selected || DublinStructuralPilot::bFixtureActive || !EditorWorld || GEditor->PlayWorld ||
		EditorWorld->GetPackage()->GetName() != TEXT("/Game/Maps/Dublin") || EditorWorld->GetOutermost()->IsDirty() ||
		!FPackageName::DoesPackageExist(TEXT("/Game/Maps/Dublin")) || FScreenshotRequest::IsScreenshotRequested() ||
		UGameViewportClient::OnScreenshotCaptured().IsBound())
	{
		AddError(TEXT("Select one exact pilot case, save /Game/Maps/Dublin, end existing PIE and finish other screenshots/readback listeners. Run this fixture alone."));
		return false;
	}
	DublinStructuralPilot::bFixtureActive = true;
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(DublinStructuralPilot::FCapture(*this, *Selected));
	return true;
}

#endif
