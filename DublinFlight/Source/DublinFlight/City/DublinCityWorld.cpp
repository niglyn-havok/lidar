#include "City/DublinCityWorld.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinCity, Log, All);

namespace
{
	const FName GeneratedMeshTag(TEXT("DublinFlight.City.GeneratedMesh.v1"));
}

ADublinCityWorld::ADublinCityWorld()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickInterval = 1.0f / 30;
	CityRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CityRoot"));
	SetRootComponent(CityRoot);
	CityRoot->SetMobility(EComponentMobility::Static);
	UMaterialInterface* Neutral = UMaterial::GetDefaultMaterial(MD_Surface);
	RoofMaterial = Neutral;
	WallMaterial = Neutral;
	GroundMaterial = Neutral;
	WaterMaterial = Neutral;
	FoundationMaterial = Neutral;
}

void ADublinCityWorld::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (bBakeRequested) { BeginFractureBake(); return; }
	if (bBakeInProgress) { return; }
	if (bAutoBuild && !IsTemplate()) { BuildCity(); }
}

void ADublinCityWorld::PostLoad()
{
	Super::PostLoad();
	bNeedsEditorLoadBuild = true;
	// Serialized status cannot stand in for native data or transient generated geometry.
	bCityReady = false;
}

void ADublinCityWorld::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (bNeedsEditorLoadBuild && bAutoBuild && GetWorld() && GetWorld()->WorldType == EWorldType::Editor)
	{
		bNeedsEditorLoadBuild = false;
		BuildCity();
	}
}

void ADublinCityWorld::BeginPlay()
{
	Super::BeginPlay();
	// Native source/mutable records are intentionally not serialized or copied into PIE.
	if (bAutoBuild && (!bCityReady || !SourceData.IsValid() || GeneratedComponents.IsEmpty()))
	{
		BuildCity();
	}
}

void ADublinCityWorld::ClearGeneratedCity()
{
	ResetDestructionState();
	bCityReady = false;
	BuildingCount = 0;
	TerrainChunkCount = 0;
	BuildingChunkCount = 0;
	WaterTriangleCount = 0;
	Attribution.Reset();
	TSet<UProceduralMeshComponent*> ComponentsToDestroy;
	for (UProceduralMeshComponent* Component : GeneratedComponents)
	{
		if (IsValid(Component) && Component->GetOwner() == this && Component->GetOuter() == this)
		{
			ComponentsToDestroy.Add(Component);
		}
	}
	// PIE can duplicate instance components while resetting the transient tracking array.
	TInlineComponentArray<UProceduralMeshComponent*> OwnedMeshes;
	GetComponents(OwnedMeshes);
	for (UProceduralMeshComponent* Component : OwnedMeshes)
	{
		if (IsMarkedGeneratedComponent(Component)) { ComponentsToDestroy.Add(Component); }
	}
	GeneratedComponents.Reset();
	TerrainComponents.Reset();
	BuildingComponents.Reset();
	WaterComponent = nullptr;
	for (UProceduralMeshComponent* Component : ComponentsToDestroy)
	{
		RemoveInstanceComponent(Component);
		Component->DestroyComponent();
	}
	SourceData.Reset();
	TerrainChunks.Reset();
	BuildingChunks.Reset();
}

bool ADublinCityWorld::IsMarkedGeneratedComponent(const UProceduralMeshComponent* Component) const
{
	if (!IsValid(Component) || Component->GetOwner() != this || Component->GetOuter() != this ||
		Component->GetClass() != UProceduralMeshComponent::StaticClass() ||
		Component->CreationMethod != EComponentCreationMethod::Instance ||
		Component->GetAttachParent() != CityRoot || !Component->ComponentHasTag(GeneratedMeshTag))
	{
		return false;
	}
	const FString Name = Component->GetName();
	return Name.StartsWith(TEXT("DublinTerrain_")) || Name.StartsWith(TEXT("DublinBuildings_")) ||
		Name == TEXT("DublinWater") || Name.StartsWith(TEXT("DublinWater_"));
}

void ADublinCityWorld::FailBuild(const FString& Error)
{
	ClearGeneratedCity();
	LastBuildError = Error;
	UE_LOG(LogDublinCity, Error, TEXT("%s city build failed: %s"), *GetName(), *LastBuildError);
}

UProceduralMeshComponent* ADublinCityWorld::CreateChunkComponent(const FString& Name, const FVector& Origin,
	const TArray<FDublinCitySection>& Sections, const TArray<UMaterialInterface*>& Materials, bool Collision)
{
	UProceduralMeshComponent* Component = NewObject<UProceduralMeshComponent>(this,
		MakeUniqueObjectName(this, UProceduralMeshComponent::StaticClass(), FName(*Name)), RF_Transient);
	if (!Component) { return nullptr; }
	Component->ComponentTags.AddUnique(GeneratedMeshTag);
	GeneratedComponents.Add(Component);
	AddInstanceComponent(Component);
	Component->SetupAttachment(CityRoot);
	Component->SetRelativeLocation(Origin);
	Component->SetMobility(EComponentMobility::Static);
	Component->SetCanEverAffectNavigation(false);
	Component->SetGenerateOverlapEvents(false);
	Component->bUseAsyncCooking = false;
	Component->bUseComplexAsSimpleCollision = true;
	Component->SetCollisionObjectType(ECC_WorldStatic);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	Component->SetCollisionEnabled(Collision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	int32 LastSection = INDEX_NONE;
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		const FDublinCitySection& Section = Sections[Index];
		if (Section.Triangles.IsEmpty()) { continue; }
		Component->CreateMeshSection(Index, Section.Vertices, Section.Triangles, Section.Normals,
			Section.UV0, Section.UV1, TArray<FVector2D>(), TArray<FVector2D>(), Section.Colors, Section.Tangents, false);
		Component->SetMaterial(Index, Materials.IsValidIndex(Index) && Materials[Index] ?
			Materials[Index] : UMaterial::GetDefaultMaterial(MD_Surface));
		LastSection = Index;
	}
	if (Collision && LastSection != INDEX_NONE)
	{
		// Enable every section before one final synchronous cook; do not recook accumulated buildings three times.
		for (int32 Index = 0; Index < Component->GetNumSections(); ++Index)
		{
			if (FProcMeshSection* Section = Component->GetProcMeshSection(Index))
			{
				Section->bEnableCollision = !Section->ProcIndexBuffer.IsEmpty();
			}
		}
		const FProcMeshSection LastCopy = *Component->GetProcMeshSection(LastSection);
		Component->SetProcMeshSection(LastSection, LastCopy);
		UBodySetup* BodySetup = Component->GetBodySetup();
		if (!BodySetup || !BodySetup->bCreatedPhysicsMeshes || BodySetup->bFailedToCreatePhysicsMeshes ||
			BodySetup->TriMeshGeometries.IsEmpty())
		{
			UE_LOG(LogDublinCity, Error, TEXT("Synchronous collision cook produced no triangle mesh for %s"), *Name);
			return nullptr;
		}
	}
	Component->RegisterComponent();
	return Component->IsRegistered() && (!Collision || Component->IsPhysicsStateCreated()) ? Component : nullptr;
}

void ADublinCityWorld::BuildCity()
{
	if (bBuilding || IsTemplate()) { return; }
	if (GetWorld() && GetWorld()->IsGameWorld() && AcceptedImpactCount > 0)
	{
		DestructionFailure(TEXT("BuildCity cannot erase persistent destruction during this play session"));
		return;
	}
	TGuardValue<bool> Guard(bBuilding, true);
	bNeedsEditorLoadBuild = false;
	ClearGeneratedCity();
	LastBuildError.Reset();
	if (!GetWorld())
	{
		FailBuild(TEXT("BuildCity requires an actor in an editor or game world"));
		return;
	}
	// Keeping the actor at survey origin prevents a visually plausible but geographically wrong city.
	if (!GetActorTransform().Equals(FTransform::Identity, 0.001))
	{
		FailBuild(TEXT("City actor transform must be identity: source already uses the fixed survey origin and centimeters"));
		return;
	}
	TUniquePtr<FDublinCityData> Parsed = MakeUnique<FDublinCityData>();
	FString Error;
	if (!DublinCity::LoadCityJson(SourceDataRelativePath, *Parsed, Error) ||
		!DublinCity::MakeTerrainChunks(Parsed->Terrain, TerrainChunks, Error) ||
		!DublinCity::MakeBuildingChunks(Parsed->Buildings, BuildingChunks, Error))
	{
		FailBuild(Error);
		return;
	}
	if (Parsed->Buildings.IsEmpty())
	{
		FailBuild(TEXT("The real Dublin city slice requires source building records; no fallback city will be generated"));
		return;
	}
	FDublinCitySection Water;
	if (!DublinCity::MakeWaterSection(Parsed->Water, Water, Error))
	{
		FailBuild(Error);
		return;
	}
	int64 RenderedVertices = Water.Vertices.Num();
	for (const FDublinCityChunk& Chunk : TerrainChunks)
	{
		for (const FDublinCitySection& Section : Chunk.Sections) { RenderedVertices += Section.Vertices.Num(); }
	}
	for (const FDublinCityChunk& Chunk : BuildingChunks)
	{
		int64 ChunkVertices = 0;
		for (const FDublinCitySection& Section : Chunk.Sections)
		{
			RenderedVertices += Section.Vertices.Num();
			ChunkVertices += Section.Vertices.Num();
			if (ChunkVertices > 200000 || Section.Triangles.Num() > 600000)
			{
				FailBuild(TEXT("One building chunk exceeds bounded geometry/collision budget"));
				return;
			}
		}
	}
	if (RenderedVertices > DublinCity::MaxRenderedVertices)
	{
		FailBuild(TEXT("Total generated vertex budget exceeded (4 million)"));
		return;
	}
	for (int32 Index = 0; Index < TerrainChunks.Num(); ++Index)
	{
		const FDublinCityChunk& Chunk = TerrainChunks[Index];
		UProceduralMeshComponent* Component = CreateChunkComponent(FString::Printf(TEXT("DublinTerrain_%02d_%02d"), Chunk.Cell.X, Chunk.Cell.Y),
			Chunk.OriginCm, Chunk.Sections, {GroundMaterial.Get()}, true);
		if (!Component)
		{
			FailBuild(TEXT("Terrain component registration or synchronous collision cook failed"));
			return;
		}
		TerrainComponents.Add(Component);
	}
	for (const FDublinCityChunk& Chunk : BuildingChunks)
	{
		UProceduralMeshComponent* Component = CreateChunkComponent(FString::Printf(TEXT("DublinBuildings_%02d_%02d"), Chunk.Cell.X, Chunk.Cell.Y),
			Chunk.OriginCm, Chunk.Sections, {RoofMaterial.Get(), WallMaterial.Get(), FoundationMaterial.Get()}, true);
		if (!Component)
		{
			FailBuild(TEXT("Building component registration or synchronous collision cook failed"));
			return;
		}
		BuildingComponents.Add(Component);
	}
	if (!Water.Triangles.IsEmpty())
	{
		TArray<FDublinCitySection> WaterSections;
		WaterSections.Add(MoveTemp(Water));
		WaterComponent = CreateChunkComponent(TEXT("DublinWater"), FVector::ZeroVector, WaterSections, {WaterMaterial.Get()}, false);
		if (!WaterComponent)
		{
			FailBuild(TEXT("Water component registration failed"));
			return;
		}
	}
	BuildingCount = Parsed->Buildings.Num();
	TerrainChunkCount = TerrainChunks.Num();
	BuildingChunkCount = BuildingChunks.Num();
	WaterTriangleCount = Parsed->Water.Triangles.Num() / 3;
	Attribution = Parsed->Attribution;
	SourceData = MoveTemp(Parsed);
	bCityReady = true;
	InitializeDestructionState();
	UE_LOG(LogDublinCity, Display, TEXT("Dublin ready: %d buildings, %d building chunks, %d terrain chunks, %d water triangles. Initial collision cooked synchronously."),
		BuildingCount, BuildingChunkCount, TerrainChunkCount, WaterTriangleCount);
	if (!RoofMaterial || !GroundMaterial || RoofMaterial == UMaterial::GetDefaultMaterial(MD_Surface) ||
		GroundMaterial == UMaterial::GetDefaultMaterial(MD_Surface))
	{
		UE_LOG(LogDublinCity, Warning, TEXT("Neutral provisional materials active: imagery is NOT yet faithfully textured. Assign aerial ground/roof materials using /Game/Textures/T_DublinOrtho; walls remain generic."));
	}
}
