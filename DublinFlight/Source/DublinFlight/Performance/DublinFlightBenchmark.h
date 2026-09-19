#pragma once

#include "CoreMinimal.h"
#include "DublinFlightPerformanceMetrics.h"
#include "DublinImpact.h"
#include "InputCoreTypes.h"

class UWorld;
class ADublinCityWorld;
class ADublinFlightPawn;
class APawn;
class ACameraActor;
class FJsonObject;
class FJsonValue;

namespace DublinFlight::Performance
{
	enum class EBenchmark : uint8 { Normal, Light, Maximal };
	DUBLINFLIGHT_API bool ParseBenchmarkArguments(const TArray<FString>& Args, EBenchmark& Kind, FOptions& Options, FString& Error);
	DUBLINFLIGHT_API int32 PlannedCannonShots(double Duration);
	DUBLINFLIGHT_API bool LightBurstActive(double Elapsed, double Duration);
	DUBLINFLIGHT_API bool BenchmarkPassed(bool Completed, bool Valid, bool Admitted, bool FrameTargetMet,
		bool FrameTargetRequired = true);
	DUBLINFLIGHT_API bool StressQueueHasCapacity(int32 QueuedImpacts);
	DUBLINFLIGHT_API bool StressImpactUnoccluded(const FDublinImpact& Impact, bool bBlockingHit, const FVector& HitPositionCm);

	// Absolute wall-clock slots: one operation per boundary, missed slots are counted, never replayed.
	struct DUBLINFLIGHT_API FBenchmarkSlots
	{
		int32 Next = 0;
		int32 Skipped = 0;
		double LastAdmission = -1;
		int32 Take(double Elapsed, double Duration, double Period);
	};

	inline constexpr int32 MaximalSchedulePolicyVersion = 3;
	inline constexpr double MaximalSlotPeriodSeconds = 0.5;
	inline constexpr int32 MaximalAttemptsPerBoundary = 8;

	// Historical V2 policy, retained for regression coverage of its absolute-slot contract.
	struct DUBLINFLIGHT_API FDeferredBenchmarkSlots
	{
		int32 Next = 0;
		int32 Due = 0;
		int32 PeakPending = 0;
		int32 PeakBatch = 0;
		int32 TakenThisBoundary = 0;
		double ObservedElapsed = -1;
		bool bCanAdmit = false;
		bool Observe(double Elapsed, double Duration, bool Ending = false);
		int32 Peek() const;
		int32 Take();
		int32 Pending() const { return Due - Next; }
	};

	// V3 saturation workload: release the entire unchanged operation count at measurement start.
	struct DUBLINFLIGHT_API FSaturationBenchmarkSlots
	{
		int32 Next = 0;
		int32 Due = 0;
		int32 PeakPending = 0;
		int32 PeakBatch = 0;
		int32 TakenThisBoundary = 0;
		double ObservedElapsed = -1;
		bool bCanAdmit = false;
		bool Observe(double Elapsed, double Duration, bool Ending = false);
		int32 Peek() const;
		int32 Take();
		int32 Pending() const { return Due - Next; }
	};

	class FDublinRuntimeBenchmark
	{
	public:
		explicit FDublinRuntimeBenchmark(EBenchmark InKind, const FOptions& InOptions);
		~FDublinRuntimeBenchmark();
		bool Prepare(UWorld& World, FString& Error);
		void Boundary(UWorld& World, double WallSeconds, const FCaptureWindow& Window, bool Ending,
			const TSharedRef<FJsonObject>& Settings);
		void Invalidate(const FString& Reason);
		void Finish(const TCHAR* Reason);
		TSharedRef<FJsonObject> Report() const;
		bool IsValid() const { return Failures.IsEmpty(); }
		bool WorkloadAdmitted() const;
		bool IsFrameTargetRequired() const { return Kind != EBenchmark::Maximal; }
		const FOptions& GetOptions() const { return Options; }

	private:
		EBenchmark Kind;
		FOptions Options;
		TWeakObjectPtr<UWorld> ActiveWorld;
		TWeakObjectPtr<ADublinCityWorld> City;
		TWeakObjectPtr<ADublinFlightPawn> Plane;
		TWeakObjectPtr<APawn> PreviousPawn;
		TWeakObjectPtr<ACameraActor> Observer;
		bool bPreviousHidden = false;
		bool bPreviousCollision = false;
		bool bPreviousTick = false;
		bool bMeasured = false;
		bool bFinished = false;
		bool bCannonHeld = false;
		bool bBomb5 = false;
		bool bBomb20 = false;
		double MeasurementStart = 0;
		double LastElapsed = 0;
		FVector RouteStart = FVector::ZeroVector;
		FVector LastPosition = FVector::ZeroVector;
		double DistanceCm = 0;
		FString BaselineSettings;
		FString Completion;
		TArray<FString> Failures;
		TArray<FString> SelectedIds;
		TArray<FDublinImpact> Sequence;
		TArray<FString> SequenceIds;
		TArray<TSharedPtr<FJsonValue>> Events;
		TArray<TSharedPtr<FJsonValue>> RouteSamples;
		TSharedPtr<FJsonObject> StartSettings;
		TSharedPtr<FJsonObject> EndSettings;
		FSaturationBenchmarkSlots StressSlots;
		int32 PeakStressAdmissionsPerBoundary = 0;
		int32 StressBackpressureBoundaries = 0;
		double LastStressAdmissionSeconds = -1;
		double MaxStressAdmissionDelaySeconds = 0;
		int32 Attempted = 0;
		int32 Accepted = 0;
		int32 Rejected = 0;
		int32 BackpressureSkipped = 0;
		int32 CannonShots = 0;
		int32 BombShots = 0;
		int32 BombImpacts = 0;
		int32 CannonImpacts = 0;
		int32 InputRequests = 0;
		int32 EndProjectiles = 0;
		int32 HeldBursts = 0;
		int32 LastBurstIndex = INDEX_NONE;
		int32 BurstStartShots = 0;
		int32 ImpactCount = 0;
		int32 RejectedImpactCount = 0;
		int32 EffectsRequests = 0;
		int32 MaxCollections = 0;
		int32 MaxPieces = 0;
		int32 MaxAllocatedCollections = 0;
		int32 MaxAllocatedPieces = 0;
		int64 MaxAllocatedHullSlots = 0;
		int32 MaxPendingAssetLoads = 0;
		int32 MaxRegistrationsPerFrame = 0;
		int32 MaxQueued = 0;
		int32 MaxQueuedBuildings = 0;
		int32 MaxFX = 0;
		int32 MaxProjectiles = 0;
		int32 Fractured = 0;
		int32 Moved = 0;
		int32 CraterNodes = 0;
		int32 CityImpacts = 0;
		float MaxWater = 0;
		FDelegateHandle DeactivateHandle;
		FDelegateHandle ActivateHandle;
		TSharedPtr<FJsonObject> WeaponSettings;
		TSharedPtr<FJsonObject> EndWeaponSettings;
		FString WeaponSettingsKey;
		void OnFocusTransition();
		void Key(const FKey& Key, bool Pressed);
		bool SelectStress(UWorld& World, FString& Error);
		void Sample(UWorld& World);
		void ObserveEnvironment(UWorld& World, const TSharedRef<FJsonObject>& Settings);
	};
}
