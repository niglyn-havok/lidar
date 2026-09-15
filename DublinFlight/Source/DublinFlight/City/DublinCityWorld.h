#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "City/DublinCityGeometry.h"
#include "City/DublinCityDestruction.h"
#include "City/DublinCityFractureLibrary.h"
#include "DublinImpact.h"
#include "DublinCityWorld.generated.h"

class UMaterialInterface;
class USceneComponent;
class UGeometryCollectionComponent;

struct FDublinQueuedWorldImpact
{
	FDublinImpact Impact;
	TArray<int32> Buildings;
	int32 NextBuilding = 0;
};

struct FDublinFractureActivity
{
	double LastImpactTime = 0;
	double LastMotionPoll = 0;
	int32 QuietPolls = 0;
	int32 Pieces = 0;
	int32 MovedPieces = 0;
	bool bSleeping = false;
	TArray<FTransform> PreviousTransforms;
	TArray<FTransform> InitialTransforms;
	TArray<int32> LeafTransforms;
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
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 ActiveFractureCollections = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 SleepingFractureCollections = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Dublin|Destruction") int32 ActiveFracturePieces = 0;
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
#if WITH_EDITOR
	virtual bool ShouldTickIfViewportsOnly() const override { return bBakeInProgress; }
#endif

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostLoad() override;
	virtual void PostRegisterAllComponents() override;

	// Immutable parsed records and source-to-render mappings are retained for a later targeted rebuild system.
	const FDublinCityData* GetSourceData() const { return SourceData.Get(); }
	const TArray<FDublinCityChunk>& GetTerrainChunks() const { return TerrainChunks; }
	const TArray<FDublinCityChunk>& GetBuildingChunks() const { return BuildingChunks; }

protected:
	virtual void BeginPlay() override;

private:
	friend class FDublinCityLifecycleTest;
	friend class FDublinDestructionReadinessTest;

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
	TArray<int32> BakeQueue;
	int32 BakeCursor = 0;
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
	bool RewriteBuildingChunk(int32 ChunkIndex, const TSet<int32>& Removed);
	void ApplyCollectionImpact(int32 BuildingIndex, const FDublinImpact& Impact);
	bool ApplyGroundCrater(const FDublinImpact& Impact);
	bool IsMarkedGeneratedComponent(const UProceduralMeshComponent* Component) const;
	void FailBuild(const FString& Error);
	UProceduralMeshComponent* CreateChunkComponent(const FString& Name, const FVector& Origin,
		const TArray<FDublinCitySection>& Sections, const TArray<UMaterialInterface*>& Materials, bool Collision);
};
