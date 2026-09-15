#include "City/DublinCityFractureLibrary.h"

#if WITH_EDITOR
#include "City/DublinCityWorld.h"
#include "City/DublinCityDestruction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionProximityUtility.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Math/RandomStream.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlanarCut.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "Misc/AutomationTest.h"
#include <limits>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDublinFractureBake, Log, All);

namespace
{
	TArray<UMaterialInterface*> FractureMaterials(const ADublinCityWorld& City)
	{
		UMaterialInterface* Neutral = UMaterial::GetDefaultMaterial(MD_Surface);
		UMaterialInterface* Foundation = City.FoundationMaterial ? City.FoundationMaterial.Get() : Neutral;
		return {City.RoofMaterial ? City.RoofMaterial.Get() : Neutral,
			City.WallMaterial ? City.WallMaterial.Get() : Neutral, Foundation, Foundation};
	}

	bool SaveAsset(UObject& Asset, FString& Error)
	{
		UPackage* Package = Asset.GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (!Package->GetName().StartsWith(TEXT("/Game/Dublin/Fracture/")) ||
			!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
		{
			Error = TEXT("Cannot create the explicit Dublin fracture asset directory");
			return false;
		}
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Asset.MarkPackageDirty();
		if (!UPackage::SavePackage(Package, &Asset, *Filename, Args))
		{
			Error = TEXT("Failed saving fracture asset package: ") + Package->GetName();
			return false;
		}
		return true;
	}

	FTransform BoneGlobal(const FGeometryCollection& Geometry, int32 Bone)
	{
		FTransform Result(Geometry.Transform[Bone]);
		for (int32 Parent = Geometry.Parent[Bone], Guard = 0; Parent != INDEX_NONE && Guard < 256;
			Parent = Geometry.Parent[Parent], ++Guard)
		{
			Result = Result * FTransform(Geometry.Transform[Parent]);
		}
		return Result;
	}

	bool UsableFractureBounds(const FBox& Bounds)
	{
		if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN()) { return false; }
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float Min = static_cast<float>(Bounds.Min[Axis]);
			const float Max = static_cast<float>(Bounds.Max[Axis]);
			if (!FMath::IsFinite(Min) || !FMath::IsFinite(Max) || Max <= Min) { return false; }
		}
		const FVector Size = Bounds.GetSize();
		return !Size.ContainsNaN() && Size.GetMin() > 0;
	}

	FIntVector FractureGridCounts(const FVector& Size, int32 Attempt)
	{
		FIntVector Counts(FMath::CeilToInt(FMath::Clamp(Size.X / 400, 2.0, 6.0)),
			FMath::CeilToInt(FMath::Clamp(Size.Y / 400, 2.0, 6.0)),
			FMath::CeilToInt(FMath::Clamp(Size.Z / 400, 2.0, 12.0)));
		while (Counts.X * Counts.Y * Counts.Z > 64)
		{
			if (Counts.Z >= Counts.X && Counts.Z >= Counts.Y && Counts.Z > 2) { --Counts.Z; }
			else if (Counts.X >= Counts.Y && Counts.X > 2) { --Counts.X; }
			else { --Counts.Y; }
		}
		return Attempt == 1 ? FIntVector(2, 2, FMath::Min(4, Counts.Z)) : Counts;
	}
}

bool DublinFractureBake::HasCurrentMaterialBindings(const UGeometryCollection& Collection, const ADublinCityWorld& City)
{
	const TArray<UMaterialInterface*> Expected = FractureMaterials(City);
	if (Collection.Materials.Num() < Expected.Num()) { return false; }
	for (int32 I = 0; I < Expected.Num(); ++I)
	{
		if (Collection.Materials[I] != Expected[I]) { return false; }
	}
	return true;
}

UDublinCityFractureLibrary* DublinFractureBake::CreateLibrary(FString& Error)
{
	if (UDublinCityFractureLibrary* Existing = FindLibrary()) { return Existing; }
	UPackage* Package = CreatePackage(TEXT("/Game/Dublin/Fracture/DA_DublinFractureLibrary"));
	UDublinCityFractureLibrary* Library = NewObject<UDublinCityFractureLibrary>(
		Package, TEXT("DA_DublinFractureLibrary"), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Library);
	return SaveAsset(*Library, Error) ? Library : nullptr;
}

bool DublinFractureBake::SaveLibrary(UDublinCityFractureLibrary& Library, FString& Error)
{
	Library.BakeVersion = UDublinCityFractureLibrary::CurrentBakeVersion;
	return SaveAsset(Library, Error);
}

bool DublinFractureBake::BuildConnectionGraph(UGeometryCollection& Collection, FString& Error)
{
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = Collection.GetGeometryCollection();
	if (!Geometry) { Error = TEXT("Cannot author connections without fracture geometry"); return false; }
	auto Properties = Geometry->GetProximityProperties();
	Properties.bUseAsConnectionGraph = true;
	Properties.Method = EProximityMethod::Precise;
	Properties.ContactAreaMethod = EConnectionContactMethod::None;
	Geometry->SetProximityProperties(Properties);
	FGeometryCollectionProximityUtility(Geometry.Get()).UpdateProximity();
	const GeometryCollection::Facades::FCollectionConnectionGraphFacade Graph(*Geometry);
	if (!Graph.IsValid() || !Graph.HasValidConnections() || Graph.NumConnections() == 0)
	{
		Error = TEXT("Fracture requires a valid authored contact-proximity connection graph");
		return false;
	}
	Collection.InvalidateCollection();
	Error.Reset();
	return true;
}

TArray<FVector> DublinFractureBake::MakeFractureSites(const FBox& Bounds, const FString& SourceId, int32 Attempt)
{
	TArray<FVector> Sites;
	if (!UsableFractureBounds(Bounds) || Attempt < 0 || Attempt >= 3) { return Sites; }
	const FVector Size = Bounds.GetSize();
	if (Attempt == 2)
	{
		for (double Height : {0.28, 0.74})
		{
			Sites.Add(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Min.Z + Size.Z * Height));
		}
		return Sites;
	}
	const FIntVector Counts = FractureGridCounts(Size, Attempt);
	FRandomStream Random(static_cast<int32>(GetTypeHash(SourceId)));
	TArray<double> Offsets[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (int32 I = 0; I < Counts[Axis]; ++I)
		{
			// Cartesian-product jitter retains irregular spacing without a fragile 3D tessellation.
			Offsets[Axis].Add(Attempt == 0 ? 0.0 : Random.FRandRange(-.13f, .13f));
		}
	}
	for (int32 Z = 0; Z < Counts.Z; ++Z)
		for (int32 Y = 0; Y < Counts.Y; ++Y)
			for (int32 X = 0; X < Counts.X; ++X)
			{
				const FVector Offset(Offsets[0][X], Offsets[1][Y], Offsets[2][Z]);
				Sites.Add(Bounds.Min + Size * FVector((X + .5 + Offset.X) / Counts.X,
					(Y + .5 + Offset.Y) / Counts.Y, (Z + .5 + Offset.Z) / Counts.Z));
			}
	return Sites;
}

static bool MakeFractureCells(const FBox& Bounds, const FString& SourceId, int32 Attempt,
	FPlanarCells& Cells, int32& SiteCount, FString& Error)
{
	const TArray<FVector> Sites = DublinFractureBake::MakeFractureSites(Bounds, SourceId, Attempt);
	SiteCount = Sites.Num();
	if (SiteCount < 2)
	{
		Error = FString::Printf(TEXT("Unusable fracture bounds/attempt: min=%s max=%s attempt=%d"),
			*Bounds.Min.ToString(), *Bounds.Max.ToString(), Attempt + 1);
		return false;
	}
	if (Attempt == 2)
	{
		const double Height = Sites[0].Z + (Sites[1].Z - Sites[0].Z) * .5;
		if (static_cast<float>(Height) <= static_cast<float>(Bounds.Min.Z) ||
			static_cast<float>(Height) >= static_cast<float>(Bounds.Max.Z))
		{
			Error = TEXT("Horizontal fracture plane collapses at source-mesh float precision");
			return false;
		}
		Cells = FPlanarCells(FPlane(FVector(0, 0, Height), FVector::UpVector));
		return true;
	}

	const FIntVector Counts = FractureGridCounts(Bounds.GetSize(), Attempt);
	const FIntVector Strides(1, Counts.X, Counts.X * Counts.Y);
	TArray<double> Edges[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// Equal-grid Voronoi cells are boxes. Keep the original bisectors and the original
		// three-centimetre exterior margin, using doubles throughout (not the float grid ctor).
		Edges[Axis].Add(Bounds.Min[Axis] - 3.0);
		for (int32 I = 1; I < Counts[Axis]; ++I)
		{
			const double A = Sites[(I - 1) * Strides[Axis]][Axis];
			const double B = Sites[I * Strides[Axis]][Axis];
			Edges[Axis].Add(A + (B - A) * .5);
		}
		Edges[Axis].Add(Bounds.Max[Axis] + 3.0);
		for (int32 I = 1; I < Edges[Axis].Num(); ++I)
		{
			if (Edges[Axis][I] - Edges[Axis][I - 1] <= 2 * UE_DOUBLE_KINDA_SMALL_NUMBER ||
				static_cast<float>(Edges[Axis][I]) <= static_cast<float>(Edges[Axis][I - 1]))
			{
				Error = FString::Printf(TEXT("Fracture cell collapses at native weld/mesh precision: axis=%d edge=%d"), Axis, I);
				return false;
			}
		}
	}
	TArray<FBox> Boxes;
	for (int32 Z = 0; Z < Counts.Z; ++Z)
		for (int32 Y = 0; Y < Counts.Y; ++Y)
			for (int32 X = 0; X < Counts.X; ++X)
			{
				Boxes.Emplace(FVector(Edges[0][X], Edges[1][Y], Edges[2][Z]),
					FVector(Edges[0][X + 1], Edges[1][Y + 1], Edges[2][Z + 1]));
			}
	Cells = FPlanarCells(MakeArrayView(Boxes), true);
	const int32 ExpectedPlanes = (Counts.X + 1) * Counts.Y * Counts.Z +
		Counts.X * (Counts.Y + 1) * Counts.Z + Counts.X * Counts.Y * (Counts.Z + 1);
	if (Cells.NumCells != SiteCount || Cells.Planes.Num() != ExpectedPlanes || !Cells.HasValidPlaneBoundaryOrientations())
	{
		Error = TEXT("Native fracture boxes did not produce a complete shared-face partition");
		return false;
	}
	return true;
}

static bool BakeBuildingAttempt(const FDublinCityBuilding& Building, ADublinCityWorld& City,
	FDublinFractureRecord& Record, FString& Error, int32 Attempt)
{
	Record = FDublinFractureRecord();
	Record.SourceId = Building.Id;
	Record.GeometryDigest = DublinDestruction::BuildingGeometryDigest(Building);
	Record.SourceDigest = DublinDestruction::BuildingDigest(Building);
	Record.BakeAttempts = Attempt + 1;
	FBox Bounds(ForceInit);
	for (const FVector& Position : Building.Mesh.VerticesCm)
	{
		if (Position.ContainsNaN() || !FMath::IsFinite(static_cast<float>(Position.X)) ||
			!FMath::IsFinite(static_cast<float>(Position.Y)) || !FMath::IsFinite(static_cast<float>(Position.Z)))
		{
			Error = TEXT("Source contains a nonfinite or float-overflowing mesh position");
			return false;
		}
		Bounds += Position;
	}
	if (!UsableFractureBounds(Bounds))
	{
		Error = TEXT("Source bounds collapse at source-mesh float precision or have no finite positive extent");
		return false;
	}
	const FString AssetName = TEXT("GC_") + Record.SourceDigest;
	const FString PackageName = TEXT("/Game/Dublin/Fracture/") + AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	UGeometryCollection* Asset = FindObject<UGeometryCollection>(Package, *AssetName);
	if (!Asset) { Asset = NewObject<UGeometryCollection>(Package, *AssetName, RF_Public | RF_Standalone); FAssetRegistryModule::AssetCreated(Asset); }
	Asset->Reset();
	Asset->Materials.Reset();
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
	Asset->SetGeometryCollection(Geometry);
	const TArray<UMaterialInterface*> Materials = FractureMaterials(City);
	const int32 StartMaterial = FGeometryCollectionEngineConversion::AppendMaterials(Materials, Asset, false);

	FMeshDescription Mesh;
	FStaticMeshAttributes Attributes(Mesh);
	Attributes.Register();
	auto Positions = Attributes.GetVertexPositions();
	auto Normals = Attributes.GetVertexInstanceNormals();
	auto Tangents = Attributes.GetVertexInstanceTangents();
	auto Signs = Attributes.GetVertexInstanceBinormalSigns();
	auto UVs = Attributes.GetVertexInstanceUVs();
	auto Colors = Attributes.GetVertexInstanceColors();
	UVs.SetNumChannels(2);
	FPolygonGroupID Groups[3];
	for (int32 I = 0; I < 3; ++I)
	{
		Groups[I] = Mesh.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Groups[I]] = FName(*FString::Printf(TEXT("Material%d"), I));
	}
	TMap<FVector3f, TMap<uint32, FVertexID>> Welded;
	TArray<FVertexID> SourceVertices;
	for (int32 Source = 0; Source < Building.Mesh.VerticesCm.Num(); ++Source)
	{
		const FVector& Position = Building.Mesh.VerticesCm[Source];
		const FVector3f P(Position);
		const FColor Color = Building.Mesh.ColorsRGBA.IsEmpty() ? FColor::White : Building.Mesh.ColorsRGBA[Source];
		const uint32 ColorKey = (static_cast<uint32>(Color.R) << 24) | (static_cast<uint32>(Color.G) << 16) |
			(static_cast<uint32>(Color.B) << 8) | Color.A;
		// The engine conversion splits normals/UVs, but not colors: retain color-only seams here.
		TMap<uint32, FVertexID>& Variants = Welded.FindOrAdd(P);
		FVertexID* Existing = Variants.Find(ColorKey);
		if (Existing) { SourceVertices.Add(*Existing); }
		else
		{
			const FVertexID Vertex = Mesh.CreateVertex();
			Positions[Vertex] = P;
			Variants.Add(ColorKey, Vertex);
			SourceVertices.Add(Vertex);
		}
	}
	for (int32 Offset = 0; Offset < Building.Mesh.Triangles.Num(); Offset += 3)
	{
		const int32 Material = Building.MaterialIds[Offset / 3];
		const FVector Normal = DublinCity::ClockwiseNormal(
			Building.Mesh.VerticesCm[Building.Mesh.Triangles[Offset]],
			Building.Mesh.VerticesCm[Building.Mesh.Triangles[Offset + 1]],
			Building.Mesh.VerticesCm[Building.Mesh.Triangles[Offset + 2]]);
		TArray<FVertexInstanceID> Corners;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 Source = Building.Mesh.Triangles[Offset + Corner];
			const FVertexInstanceID Instance = Mesh.CreateVertexInstance(SourceVertices[Source]);
			Corners.Add(Instance);
			Normals[Instance] = FVector3f(Normal);
			const FVector Tangent = FMath::Abs(Normal.Z) < .9 ? FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal() : FVector::ForwardVector;
			Tangents[Instance] = FVector3f(Tangent);
			Signs[Instance] = 1;
			Colors[Instance] = FVector4f(DublinDestruction::SourceVertexLinearColor(Building.Mesh, Source));
			const FVector2D SourceUV = Building.Mesh.UV[Source];
			UVs.Set(Instance, 0, FVector2f(Material == 0
				? DublinCity::AerialUV(Building.Mesh.VerticesCm[Source] + Building.PivotCm) : SourceUV));
			UVs.Set(Instance, 1, FVector2f(SourceUV));
		}
		Mesh.CreatePolygon(Groups[Material], Corners);
	}
	FGeometryCollectionEngineConversion::AppendMeshDescription(&Mesh, Building.Id, StartMaterial, FTransform::Identity,
		Geometry.Get(), nullptr, true, false, false);
	if (Geometry->NumElements(FGeometryCollection::GeometryGroup) < 1)
	{
		Error = TEXT("Source MeshDescription conversion produced no geometry");
		Record.Error = Error;
		return false;
	}
	TArray<int32> SourceBones{0};
	TArray<double> SourceVolumes;
	FindBoneVolumes(*Geometry, MakeArrayView(SourceBones), SourceVolumes, 1.0);
	Record.SourceVolumeCm3 = SourceVolumes.IsEmpty() ? 0 : SourceVolumes[0];
	if (!FMath::IsFinite(Record.SourceVolumeCm3) || Record.SourceVolumeCm3 <= 0)
	{
		Error = TEXT("Source conversion has no finite positive closed-mesh volume");
		return false;
	}
	const FVector Size = Bounds.GetSize();
	FPlanarCells Cells;
	UE_LOG(LogDublinFractureBake, Display, TEXT("Fracture partition source=%s attempt=%d bounds=%s"),
		*Building.Id, Attempt + 1, *Bounds.ToString());
	if (!MakeFractureCells(Bounds, Building.Id, Attempt, Cells, Record.FractureSiteCount, Error)) { return false; }
	Cells.InternalSurfaceMaterials.GlobalMaterialID = StartMaterial + 3;
	UE_LOG(LogDublinFractureBake, Display, TEXT("Fracture cut source=%s attempt=%d route=%s cells=%d bounds=%s"),
		*Building.Id, Attempt + 1, Attempt == 2 ? TEXT("horizontal-plane") : TEXT("native-shared-boxes"),
		Cells.NumCells, *Bounds.ToString());
	TArray<int32> Selection{0};
	const int32 NewGeometry = CutMultipleWithPlanarCells(Cells, *Geometry, MakeArrayView(Selection),
		0, 50, static_cast<int32>(GetTypeHash(Building.Id)), TOptional<FTransform>(), true, false,
		nullptr, FVector::ZeroVector, Attempt == 0 ? FIslandSplitSettings() : FIslandSplitSettings(Attempt != 2, .01, .1));
	if (NewGeometry < 0)
	{
		Error = TEXT("Planar cutter produced no new fracture geometry");
		Record.Error = Error;
		return false;
	}
	if (Attempt > 0)
	{
		TArray<int32> Bones;
		TArray<double> Volumes;
		FindBoneVolumes(*Geometry, MakeArrayView(Bones), Volumes, 1.0);
		double TotalVolume = 0;
		for (double Volume : Volumes) { TotalVolume += FMath::Max(0.0, Volume); }
		const double MinVolume = FMath::Max(1.0, TotalVolume * 1.e-6);
		TArray<int32> Small;
		for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
		{
			const int32 G = Geometry->TransformToGeometryIndex[Bone];
			if (G != INDEX_NONE && Geometry->IsRigid(Bone) && Geometry->Children[Bone].IsEmpty() &&
				(Volumes[Bone] < MinVolume || Geometry->BoundingBox[G].GetExtent().GetMin() < .1))
			{
				Small.Add(Bone);
			}
		}
		const int32 Before = Geometry->Transform.Num();
		if (!Small.IsEmpty())
		{
			// Append the tiny piece's complete mesh to a contact neighbor; no triangle/attribute deletion.
			MergeBones(*Geometry, MakeArrayView(Bones), MakeArrayView(Volumes), MinVolume, MakeArrayView(Small),
				false, UE::PlanarCut::LargestNeighbor, false, true);
		}
		Record.MergedMicroShards = Before - Geometry->Transform.Num();
		auto Convex = Geometry->GetConvexProperties();
		Convex.SimplificationThreshold = .1f;
		Geometry->SetConvexProperties(Convex);
	}
	TArray<int32> AllBones;
	TArray<double> RetainedVolumes;
	FindBoneVolumes(*Geometry, MakeArrayView(AllBones), RetainedVolumes, 1.0);
	for (double Volume : RetainedVolumes) { Record.RetainedVolumeCm3 += Volume; }
	if (!FMath::IsFinite(Record.RetainedVolumeCm3) ||
		FMath::Abs(Record.RetainedVolumeCm3 - Record.SourceVolumeCm3) > FMath::Max(1.0, Record.SourceVolumeCm3 * 1.e-4))
	{
		Error = FString::Printf(TEXT("Fracture changed closed-mesh volume: source=%.9g retained=%.9g cm3"),
			Record.SourceVolumeCm3, Record.RetainedVolumeCm3);
		return false;
	}
	TArray<int32> Roots;
	FGeometryCollectionClusteringUtility::GetRootBones(Geometry.Get(), Roots);
	if (Roots.Num() != 1) { FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(Geometry.Get()); }
	Roots.Reset();
	FGeometryCollectionClusteringUtility::GetRootBones(Geometry.Get(), Roots);
	if (Roots.Num() != 1) { Error = TEXT("Fracture hierarchy has no unique root"); Record.Error = Error; return false; }
	Record.RootTransform = Roots[0];
	if (Geometry->Children[Roots[0]].Num() < 2)
	{
		Error = TEXT("Planar output has no real clustered child hierarchy");
		Record.Error = Error;
		return false;
	}
	Geometry->SimulationType[Roots[0]] = FGeometryCollection::ESimulationTypes::FST_Clustered;
	FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Geometry.Get(), Roots[0]);
	if (!DublinFractureBake::BuildConnectionGraph(*Asset, Error)) { Record.Error = Error; return false; }
	if (!Geometry->HasAttribute(TEXT("InitialDynamicState"), FGeometryCollection::TransformGroup))
	{
		TManagedArray<int32>& States = Geometry->AddAttribute<int32>(TEXT("InitialDynamicState"), FGeometryCollection::TransformGroup);
		for (int32 I = 0; I < States.Num(); ++I) { States[I] = static_cast<int32>(Chaos::EObjectStateType::Dynamic); }
	}
	Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Geometry);
	Anchoring.AddAnchoredAttribute();
	for (int32 Bone = 0; Bone < Geometry->NumElements(FGeometryCollection::TransformGroup); ++Bone)
	{
		const int32 G = Geometry->TransformToGeometryIndex[Bone];
		if (G == INDEX_NONE || !Geometry->Children[Bone].IsEmpty() ||
			Geometry->SimulationType[Bone] != FGeometryCollection::ESimulationTypes::FST_Rigid) { continue; }
		++Record.PieceCount;
		Record.LeafTransforms.Add(Bone);
		const auto& LocalBox = Geometry->BoundingBox[G];
		const FBox GlobalBox = FBox(FVector(LocalBox.Min), FVector(LocalBox.Max)).TransformBy(BoneGlobal(*Geometry, Bone));
		if (GlobalBox.Min.Z <= Bounds.Min.Z + FMath::Min(40.0, Size.Z * 0.15))
		{
			Record.Anchors.Add(Bone);
			Anchoring.SetAnchored(Bone, true);
			Anchoring.SetInitialDynamicState(Bone, Chaos::EObjectStateType::Kinematic);
		}
	}
	if (Record.PieceCount < 2 || Record.PieceCount > 128 || Record.Anchors.IsEmpty())
	{
		Error = TEXT("Fracture requires 2..128 real pieces and at least one ground-support anchor");
		Record.Error = Error;
		return false;
	}
	Asset->EnableClustering = true;
	Asset->DamageModel = EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
	Asset->DamageThreshold = {100, 75, 50};
	Asset->bUseSizeSpecificDamageThreshold = false;
	Asset->bRemoveOnMaxSleep = false;
	Asset->bAutomaticCrumblePartialClusters = false;
	Asset->DamagePropagationData.bEnabled = false;
	Asset->DamagePropagationData.BreakDamagePropagationFactor = 0;
	Asset->DamagePropagationData.ShockDamagePropagationFactor = 0;
	Asset->bEnableNaniteFallback = true;
	Asset->SetEnableNanite(true);
	Asset->InvalidateCollection();
	Asset->UpdateGeometryDependentProperties();
	if (Attempt > 0)
	{
		// A concave root's single hull may be rejected by CanExceedFraction. Keep its child hulls
		// instead of filling the courtyard with a larger convex or accepting a missing root shape.
		FGeometryCollectionConvexUtility::FClusterConvexHullSettings Settings(Record.PieceCount, 0, false);
		const TArray<int32> ClusterRoots{Record.RootTransform};
		FGeometryCollectionConvexUtility::GenerateClusterConvexHullsFromChildrenHulls(*Geometry, Settings, MakeArrayView(ClusterRoots));
		Asset->InvalidateCollection();
	}
	Asset->CreateSimulationData();
	Asset->SetConvertVertexColorsToSRGB(true);
	Asset->RebuildRenderData();
	Record.bNaniteReady = Asset->HasNaniteData();
	if (!Record.bNaniteReady)
	{
		Asset->SetEnableNanite(false);
		Asset->CreateSimulationData();
		Asset->RebuildRenderData();
		Record.Error = TEXT("Nanite data unavailable; valid non-Nanite fallback retained");
	}
	if (!Asset->HasVisibleGeometry() || (!Asset->HasMeshData() && !Asset->HasNaniteData()) || Asset->IsSimulationDataDirty())
	{
		Error = TEXT("Fracture render or simulation data failed validation");
		Record.Error = Error;
		return false;
	}
	if (!DublinFractureBake::ValidateCollisionData(*Asset, Record, Error)) { Record.Error = Error; return false; }
	if (!SaveAsset(*Asset, Error)) { Record.Error = Error; return false; }
	Record.Collection = Asset;
	Record.bReady = true;
	return true;
}

bool DublinFractureBake::BakeBuilding(const FDublinCityBuilding& Building, ADublinCityWorld& City,
	FDublinFractureRecord& Record, FString& Error)
{
	TArray<FString> Diagnostics;
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		Error.Reset();
		const bool Ready = BakeBuildingAttempt(Building, City, Record, Error, Attempt);
		Diagnostics.Add(FString::Printf(TEXT("attempt=%d sites=%d pieces=%d anchors=%d merged=%d volumeCm3=%.6g->%.6g: %s"),
			Attempt + 1, Record.FractureSiteCount, Record.PieceCount, Record.Anchors.Num(), Record.MergedMicroShards,
			Record.SourceVolumeCm3, Record.RetainedVolumeCm3,
			Ready ? TEXT("ready") : *Error));
		UE_LOG(LogDublinFractureBake, Display, TEXT("Fracture bake source=%s %s"), *Building.Id, *Diagnostics.Last());
		Record.BakeDiagnostics = Diagnostics;
		if (Ready) { return true; }
	}
	Error = FString::Join(Diagnostics, TEXT(" | "));
	Record.Error = Error;
	Record.bReady = false;
	return false;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinFractureNumericalCellsTest,
	"DublinFlight.Destruction.Fracture.NumericalCellCutsConserveGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinFractureNumericalCellsTest::RunTest(const FString& Parameters)
{
	// First fixture is the exact bounds of the source saved immediately after the 233 ensure.
	const TArray<FBox> Fixtures{
		FBox(FVector(-237.1, -223.8, 0), FVector(237.0, 223.7, 3440.14)),
		FBox(FVector(-11095.75, -4724.25, 0), FVector(11095.75, 4724.25, 70))};
	for (const FBox& Bounds : Fixtures)
	{
		for (int32 Attempt = 0; Attempt < 3; ++Attempt)
		{
			FPlanarCells Cells;
			FString Error;
			int32 Sites = 0;
			if (!TestTrue(TEXT("Numerical fixture has a complete native partition"),
				MakeFractureCells(Bounds, TEXT("numerical-cut-fixture"), Attempt, Cells, Sites, Error)))
			{
				AddError(Error);
				return false;
			}
			TestEqual(TEXT("Only last retry uses the native infinite plane"), Cells.IsInfinitePlane(), Attempt == 2);
			auto Geometry = GeometryCollection::MakeCubeElement(FTransform(Bounds.GetCenter()), Bounds.GetSize());
			TArray<int32> Selection{0};
			TArray<double> Before;
			FindBoneVolumes(*Geometry, MakeArrayView(Selection), Before, 1.0);
			if (!TestEqual(TEXT("Closed numerical source has one measured volume"), Before.Num(), 1)) { return false; }
			const int32 Cut = CutMultipleWithPlanarCells(Cells, *Geometry, MakeArrayView(Selection),
				0, 50, 0, TOptional<FTransform>(), true, false, nullptr, FVector::ZeroVector,
				FIslandSplitSettings(Attempt != 2, .01, .1));
			if (!TestTrue(TEXT("Native partition actually cuts the numerical mesh"), Cut >= 0)) { return false; }
			TArray<int32> All;
			TArray<double> After;
			FindBoneVolumes(*Geometry, MakeArrayView(All), After, 1.0);
			double Retained = 0;
			int32 Pieces = 0;
			for (int32 Bone = 0; Bone < Geometry->Transform.Num(); ++Bone)
			{
				if (Geometry->IsRigid(Bone) && Geometry->Children[Bone].IsEmpty()) { ++Pieces; }
			}
			for (double Volume : After) { Retained += Volume; }
			TestEqual(TEXT("No numerical cell or source piece is silently lost"), Pieces, Sites);
			TestTrue(TEXT("Actual cut conserves volume at the production tolerance"),
				FMath::IsFinite(Retained) && Before[0] > 0 &&
				FMath::Abs(Retained - Before[0]) <= FMath::Max(1.0, Before[0] * 1.e-4));
		}
	}
	const double Inf = std::numeric_limits<double>::infinity();
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	TArray<FBox> Invalid{
		FBox(FVector::ZeroVector, FVector(100, 100, 0)),
		FBox(FVector(1.e12), FVector(1.e12 + 100))};
	Invalid.Add(FBox(FVector::ZeroVector, FVector(100)));
	Invalid.Last().Max.X = Inf;
	Invalid.Add(FBox(FVector::ZeroVector, FVector(100)));
	Invalid.Last().Max.X = NaN;
	for (const FBox& Bounds : Invalid)
	{
		for (int32 Attempt = 0; Attempt < 3; ++Attempt)
		{
			FPlanarCells Cells;
			FString Error;
			int32 Sites = 0;
			TestFalse(TEXT("Nonfinite, flat or float-collapsed bounds never reach a cutter"),
				MakeFractureCells(Bounds, TEXT("invalid-cut-fixture"), Attempt, Cells, Sites, Error));
			TestFalse(TEXT("Rejected numerical inputs explain the failure"), Error.IsEmpty());
		}
	}
	return true;
}
#endif
#endif
