#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "City/DublinCityGeometry.h"
#include "City/DublinCityDestruction.h"
#include "City/DublinCityFractureLibrary.h"
#include "DublinImpact.h"
#include "Effects/DublinMaximumImpact.h"
#if WITH_EDITOR
#include "Containers/Ticker.h"
#endif
#include "DublinCityWorld.generated.h"

class UMaterialInterface;
class USceneComponent;
class UGeometryCollectionComponent;
class UDublinWeaponComponent;
struct FStreamableHandle;

struct FDublinQueuedWorldImpact
{
	FDublinImpact Impact;
	TArray<int32> Buildings;
	int32 NextBuilding = 0;
	uint64 EventId = 0;
	double AcceptedWorldTime = 0;
	double FirstCommitWorldTime = -1;
	int32 RemainingCoreBuildings = 0;
	bool bDeferredPresentation = false;
	bool bPresentationRequested = false;
	bool bSurfaceCommitted = false;
	bool bCoreDamageCommitted = false;
	TWeakObjectPtr<UDublinWeaponComponent> Accounting;
	TArray<FDublinConfirmedDamageSample> DamageSamples;
};

struct FDublinPendingFractureAsset
{
	TSharedPtr<FStreamableHandle> Handle;
	FSoftObjectPath Path;
	double StartedWallTime = 0;
};

struct FDublinFractureActivity
{
	double LastImpactTime = 0;
	double LastMotionPoll = 0;
	int32 QuietPolls = 0;
	int32 Pieces = 0;
	int32 HullSlots = 0;
	int32 MovedPieces = 0;
	int32 SampledAwakeBodies = 0;
	int32 SampledAwakeLeaves = 0;
	bool bSleeping = false;
	TArray<FTransform> PreviousTransforms;
	TArray<FTransform> InitialTransforms;
	TArray<int32> LeafTransforms;
	TArray<FBox> MassLocalBounds;
};

UCLASS(Blueprintable)
class DUBLINFLIGHT_API ADublinCityWorld : public AActor
{
	GENERATED_BODY()

public:
	ADublinCityWorld();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Data")
	FString SourceDataRelativePath = TEXT("Data/dublin-city.json");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Data")
	bool bAutoBuild = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Materials")
	TObjectPtr<UMaterialInterface> RoofMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Materials")
	TObjectPtr<UMaterialInterface> WallMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Materials")
	TObjectPtr<UMaterialInterface> GroundMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Materials")
	TObjectPtr<UMaterialInterface> WaterMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Materials")
	TObjectPtr<UMaterialInterface> FoundationMaterial;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	int32 BuildingCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	int32 TerrainChunkCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	int32 BuildingChunkCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	int32 WaterTriangleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	bool bCityReady = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	FString LastBuildError;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Status")
	TArray<FString> Attribution;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Dublin|Data")
	void BuildCity();

	bool ApplyImpact(const FDublinImpact& Impact);
	bool ApplyImpactWithPresentation(const FDublinImpact& Impact, UDublinWeaponComponent* Accounting, uint64& OutEventId);
	bool HasWeaponImpactCapacity() const;
	const FGuid& GetImpactEpoch() const { return ImpactEpoch; }
	bool GetWaterSurfaceZ(const FVector& PositionCm, float& OutSurfaceZCm) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Destruction")
	TObjectPtr<UDublinCityFractureLibrary> FractureLibrary;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Destruction")
	bool bAllowPartialBakePreview = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Fracture Bake")
	bool bBakeRequested = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Fracture Bake")
	bool bCancelBake = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Fracture Bake")
	bool bBakeAllBuildings = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Fracture Bake")
	EDublinFractureRecipe BakeRecipe = EDublinFractureRecipe::SolidGrid;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Fracture Bake", meta = (ClampMin = "1", ClampMax = "5000"))
	int32 BakeLimit = 12;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dublin|Fracture Bake")
	TArray<FString> BakeBuildingIds;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") bool bDestructionReady = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") bool bAllBuildingsFractureReady = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") bool bImpactQueueBlocked = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") FString LastDestructionError;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 FractureReadyBuildingCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 AcceptedImpactCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 QueuedImpactCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 QueuedBuildingHitCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 QueuedCoreBuildingHitCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 ActiveFractureCollections = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 SleepingFractureCollections = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 ActiveFracturePieces = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 CatalogFractureCollections = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 CatalogFractureLeafSlots = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 CatalogFractureHullSlots = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") bool bCatalogBudgetValid = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 AllocatedFractureCollections = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 AllocatedFracturePieces = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 AllocatedFractureHullSlots = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 SampledAwakeLeafBodies = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 SampledSleepingLeafBodies = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 PendingFractureAssetLoads = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 LastFrameFractureRegistrations = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 LastFrameOrdinaryFractureRegistrations = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 PeakFrameFractureRegistrations = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 DeferredImpactEffectsRequests = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") float MaxFirstDamageLatencySeconds = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") float MaxCoreDamageLatencySeconds = 0;
	uint64 LastAcceptedImpactEventId = 0;
	uint64 LastPresentedImpactEventId = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 FracturedBuildingCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 MovedFragmentCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 ForcedSleepCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 CraterChangedNodeCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 LastCraterChunkCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") float WaterMaxDisplacementCm = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 WaterSimulationTriangleCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 WaterActiveNodeCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Fracture Bake") bool bBakeInProgress = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Fracture Bake") int32 BakeCompleted = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Fracture Bake") int32 BakeTotal = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Fracture Bake") int32 BakeErrorCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Fracture Bake") FString BakeCurrentSourceId;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Fracture Bake") TArray<FString> BakeErrors;
	UFUNCTION(CallInEditor, Category = "Dublin|Fracture Bake") void BeginFractureBake();
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Dublin|Destruction") void RefreshDestructionReadiness();
	UFUNCTION(BlueprintCallable, Category = "Dublin|Destruction") void RetryDestructionQueue();
	virtual void Tick(float DeltaSeconds) override;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostLoad() override;
	virtual void PostRegisterAllComponents() override;

	// Immutable parsed records and source-to-render mappings are retained for a later targeted rebuild system.
	const FDublinCityData* GetSourceData() const { return SourceData.Get(); }
	const TArray<FDublinCityChunk>& GetTerrainChunks() const { return TerrainChunks; }
	const TArray<FDublinCityChunk>& GetBuildingChunks() const { return BuildingChunks; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend class FDublinCityLifecycleTest;
	friend class FDublinDestructionReadinessTest;
	friend class FDublinDestructionAsyncLoadTest;
	friend class FDublinBlastCatalogAdmissionTest;
	friend class FDublinBlastOutboxTest;
	friend class FDublinBlastDuplicateAllocationTest;
	friend class FDublinCityActivationPacingNativeTest;

	UPROPERTY(VisibleAnywhere, Category = "Dublin")
	TObjectPtr<USceneComponent> CityRoot;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> GeneratedComponents;
	UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> TerrainComponents;
	UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> BuildingComponents;
	UPROPERTY(Transient) TObjectPtr<UProceduralMeshComponent> WaterComponent;
	UPROPERTY(Transient) TMap<int32, TObjectPtr<UGeometryCollectionComponent>> FractureComponents;

	TUniquePtr<FDublinCityData> SourceData;
	TArray<FDublinCityChunk> TerrainChunks;
	TArray<FDublinCityChunk> BuildingChunks;
	bool bBuilding = false;
	bool bNeedsEditorLoadBuild = false;
	bool bDestructionInitialized = false;
	FDublinGroundMutation GroundMutation;
	FDublinWaterWaves WaterWaves;
	TArray<FBox> BuildingBounds;
	TArray<int32> BuildingToChunk;
	TMap<FIntPoint, TArray<int32>> BuildingSpatialIndex;
	TMap<int32, FDublinFractureActivity> FractureActivity;
	TSet<int32> RemovedIntactBuildings;
	TArray<FDublinQueuedWorldImpact> PendingImpacts;
	TSharedPtr<FStreamableHandle> PendingFractureLoad;
	FSoftObjectPath PendingFracturePath;
	int32 PendingFractureBuilding = INDEX_NONE;
	double PendingFractureStartedWall = 0;
	TMap<int32, FDublinPendingFractureAsset> AdditionalFractureLoads;
	FGuid ImpactEpoch = FGuid::NewGuid();
	uint64 NextImpactEventId = 1;
	uint64 LastImpactProcessingFrame = MAX_uint64;
	int32 ImpactQueueCursor = 0;
	double LastNativeBodyPoll = -1;
	double LastDestructionTickWorldTime = -1;
	TArray<int32> BakeQueue;
	TArray<EDublinFractureRecipe> BakeQueueRecipes;
	EDublinFractureRecipe ActiveBakeRecipe = EDublinFractureRecipe::SolidGrid;
	int32 BakeCursor = 0;
#if WITH_EDITOR
	FTSTicker::FDelegateHandle BakeTickerHandle;
#endif
	double WaterAccumulator = 0;

	void ClearGeneratedCity();
	void ResetDestructionState();
	void InitializeDestructionState();
	void TickFractureBake();
	void UpdateDestructionTickEnabled();
	void UpdateDestructionCounters();
	bool DestructionFailure(const FString& Error);
	TArray<int32> SelectImpactedBuildings(const FDublinImpact& Impact) const;
	bool ActivateBuilding(int32 BuildingIndex);
	bool PrepareFractureAsset(int32 BuildingIndex, bool& bReady);
	void CancelPendingFractureLoad();
	void ReleaseFractureLoad(int32 BuildingIndex);
	void PrefetchFractureAssets();
	void ProcessImpactQueue();
	void RequestCommittedPresentations();
	bool ApplyImpactInternal(const FDublinImpact& Impact, bool bDeferredPresentation,
		UDublinWeaponComponent* Accounting, uint64& OutEventId);
	void AddCommittedDamageSamples(FDublinQueuedWorldImpact& Event, int32 BuildingIndex,
		const TArray<int32>& HitLeaves);
	bool RewriteBuildingChunk(int32 ChunkIndex, const TSet<int32>& Removed);
	bool ApplyCollectionImpact(int32 BuildingIndex, const FDublinImpact& Impact, TArray<int32>& OutHitLeaves);
	bool ApplyGroundCrater(const FDublinImpact& Impact);
	bool IsMarkedGeneratedComponent(const UProceduralMeshComponent* Component) const;
	void FailBuild(const FString& Error);
	UProceduralMeshComponent* CreateChunkComponent(const FString& Name, const FVector& Origin,
		const TArray<FDublinCitySection>& Sections, const TArray<UMaterialInterface*>& Materials, bool Collision);
};
