#include "City/DublinCityFractureLibrary.h"
#include "City/DublinCityDestruction.h"
#include "City/DublinCityStructural.h"

#include "Chaos/ImplicitObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "GeometryCollectionProxyData.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/UObjectGlobals.h"

bool DublinFractureBake::IsSupportedRecipe(EDublinFractureRecipe Recipe)
{
	return Recipe == EDublinFractureRecipe::SolidGrid || Recipe == EDublinFractureRecipe::StructuralPilot || IsDetailedRecipe(Recipe);
}

bool DublinFractureBake::IsDetailedRecipe(EDublinFractureRecipe Recipe)
{
	return Recipe == EDublinFractureRecipe::StructuralCity || Recipe == EDublinFractureRecipe::LayeredMasonry;
}

bool DublinFractureBake::HasValidPieceBudget(const FDublinFractureRecord& Record)
{
	if (!IsSupportedRecipe(Record.Recipe) || Record.PieceCount < 2 ||
		Record.CollisionStorageVersion < 0 || Record.CollisionStorageVersion > 1 ||
		(Record.CollisionStorageVersion != 0 && !IsDetailedRecipe(Record.Recipe))) { return false; }
	if (!IsDetailedRecipe(Record.Recipe)) { return Record.PieceCount <= UDublinCityFractureLibrary::MaxPiecesPerBuilding; }
	return (Record.DetailTier == 128 || Record.DetailTier == 384 || Record.DetailTier == 768 ||
		(Record.DetailTier == 1536 && (Record.BroadSurfaceAreaM2 > 6000 ||
			(Record.bDenseForOversize && Record.LargestMemberAreaM2 > 16)))) &&
		Record.PieceCount <= Record.DetailTier && Record.HullCount == Record.PieceCount &&
		Record.MemberCount > 0 && !Record.PlanReason.IsEmpty();
}

bool DublinFractureBake::IsCurrentRecord(const FDublinCityBuilding& Building, const FDublinFractureRecord& Record)
{
	return IsSupportedRecipe(Record.Recipe) && Record.SourceId == Building.Id &&
		(Record.Recipe != EDublinFractureRecipe::StructuralPilot || DublinStructural::IsPilotId(Building.Id)) &&
		!Record.SourceDigest.IsEmpty() && Record.SourceDigest == DublinDestruction::BuildingDigest(Building, Record.Recipe);
}

bool DublinFractureBake::HasCompleteDetailCoverage(const TArray<FDublinCityBuilding>& Buildings,
	const UDublinCityFractureLibrary& Library, TArray<FString>& PendingIds)
{
	PendingIds.Reset();
	for (const auto& Building : Buildings)
	{
		const FDublinFractureRecord* R = Library.Find(Building.Id);
		if (!R || (!IsDetailedRecipe(R->Recipe) && R->Recipe != EDublinFractureRecipe::StructuralPilot) ||
			!R->bReady || !R->bNaniteReady || !HasValidPieceBudget(*R) ||
			!IsCurrentRecord(Building, *R) || R->Collection.IsNull() ||
			!FPackageName::DoesPackageExist(R->Collection.ToSoftObjectPath().GetLongPackageName()))
		{
			PendingIds.Add(Building.Id);
		}
	}
	return !Buildings.IsEmpty() && PendingIds.IsEmpty() && Library.BakeVersion == UDublinCityFractureLibrary::CurrentBakeVersion;
}

bool DublinFractureBake::ValidateBakeRequest(EDublinFractureRecipe Recipe, const TArray<FString>& Ids,
	bool bBakeAll, FString& Error)
{
	if (!IsSupportedRecipe(Recipe))
	{
		Error = TEXT("Unsupported fracture bake recipe");
		return false;
	}
	if (Recipe == EDublinFractureRecipe::StructuralPilot)
	{
		if (bBakeAll || Ids.IsEmpty() || Ids.Num() > 3)
		{
			Error = TEXT("StructuralPilot requires 1..3 explicit approved IDs and forbids BakeAll");
			return false;
		}
		TSet<FString> Unique;
		for (const FString& Id : Ids)
		{
			if (!DublinStructural::IsPilotId(Id) || Unique.Contains(Id))
			{
				Error = TEXT("StructuralPilot ID is not approved or is duplicated: ") + Id;
				return false;
			}
			Unique.Add(Id);
		}
	}
	Error.Reset();
	return true;
}

const FDublinFractureRecord* UDublinCityFractureLibrary::Find(const FString& Id) const
{
	return Records.FindByPredicate([&Id](const FDublinFractureRecord& Record) { return Record.SourceId == Id; });
}

UDublinCityFractureLibrary* DublinFractureBake::FindLibrary()
{
	const TCHAR* ObjectPath = TEXT("/Game/Dublin/Fracture/DA_DublinFractureLibrary.DA_DublinFractureLibrary");
	if (UDublinCityFractureLibrary* Existing = FindObject<UDublinCityFractureLibrary>(nullptr, ObjectPath)) { return Existing; }
	return FPackageName::DoesPackageExist(TEXT("/Game/Dublin/Fracture/DA_DublinFractureLibrary"))
		? LoadObject<UDublinCityFractureLibrary>(nullptr, ObjectPath) : nullptr;
}

bool DublinFractureBake::ValidateCollisionData(const UGeometryCollection& Collection,
	const FDublinFractureRecord& Record, FString& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_FullStaticProof);
	if (!IsSupportedRecipe(Record.Recipe))
	{
		Error = TEXT("Unsupported fracture collision recipe");
		return false;
	}
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = Collection.GetGeometryCollection();
	if (!Geometry || !HasValidPieceBudget(Record) ||
		Record.LeafTransforms.Num() != Record.PieceCount || Record.Anchors.IsEmpty() ||
		Record.Anchors.Num() >= Record.PieceCount ||
		Record.RootTransform != Collection.GetRootIndex() || !Geometry->Children.IsValidIndex(Record.RootTransform) ||
		!Geometry->IsClustered(Record.RootTransform) || Geometry->Children[Record.RootTransform].IsEmpty())
	{
		Error = TEXT("Fracture collision requires a valid root and complete authored leaf mapping");
		return false;
	}
	const TManagedArray<bool>* Simulatable = Geometry->FindAttribute<bool>(
		FGeometryCollection::SimulatableParticlesAttribute, FGeometryCollection::TransformGroup);
	const TManagedArray<Chaos::FImplicitObjectPtr>* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(
		FGeometryDynamicCollection::ImplicitsAttribute, FGeometryCollection::TransformGroup);
	const TManagedArray<float>* Mass = Geometry->FindAttribute<float>(TEXT("Mass"), FGeometryCollection::TransformGroup);
	const TManagedArray<FTransform>* MassToLocal = Geometry->FindAttribute<FTransform>(
		TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
	const TManagedArray<TSet<int32>>* ConvexIndices = Geometry->FindAttribute<TSet<int32>>(
		TEXT("TransformToConvexIndices"), FGeometryCollection::TransformGroup);
	auto DescribeCollision = [&](int32 Bone)
	{
		const bool ValidBone = Geometry->Transform.IsValidIndex(Bone);
		const int32 G = ValidBone ? Geometry->TransformToGeometryIndex[Bone] : INDEX_NONE;
		const bool ValidGeometry = Geometry->BoundingBox.IsValidIndex(G);
		const bool HasImplicit = Implicits && Implicits->IsValidIndex(Bone) && (*Implicits)[Bone];
		return FString::Printf(TEXT("bone=%d geometry=%d simType=%d simulatable=%d implicit=%d bounded=%d mass=%.9g massTransformFinite=%d hulls=%d vertices=%d faces=%d geometrySizeCm=%s"),
			Bone, G, ValidBone ? Geometry->SimulationType[Bone] : INDEX_NONE,
			Simulatable && Simulatable->IsValidIndex(Bone) ? static_cast<int32>((*Simulatable)[Bone]) : -1,
			HasImplicit, HasImplicit && (*Implicits)[Bone]->HasBoundingBox(),
			Mass && Mass->IsValidIndex(Bone) ? (*Mass)[Bone] : -1.0f,
			MassToLocal && MassToLocal->IsValidIndex(Bone) && !(*MassToLocal)[Bone].ContainsNaN(),
			ConvexIndices && ConvexIndices->IsValidIndex(Bone) ? (*ConvexIndices)[Bone].Num() : -1,
			ValidGeometry ? Geometry->VertexCount[G] : 0, ValidGeometry ? Geometry->FaceCount[G] : 0,
			ValidGeometry ? *Geometry->BoundingBox[G].GetSize().ToCompactString() : TEXT("none"));
	};
	const GeometryCollection::Facades::FCollectionConnectionGraphFacade Graph(*Geometry);
	if (!Graph.IsValid() || !Graph.HasValidConnections() || Graph.NumConnections() == 0)
	{
		Error = TEXT("Fracture has no valid authored connection graph; rebake this source volume");
		return false;
	}
	if (!Simulatable || !Implicits || !Mass || !MassToLocal ||
		!Simulatable->IsValidIndex(Record.RootTransform) || !Implicits->IsValidIndex(Record.RootTransform) ||
		!Mass->IsValidIndex(Record.RootTransform) || !MassToLocal->IsValidIndex(Record.RootTransform) ||
		!(*Implicits)[Record.RootTransform] || !(*Implicits)[Record.RootTransform]->HasBoundingBox())
	{
		Error = FString::Printf(TEXT("Fracture is missing cooked leaf or clustered-root collision data (attributes sim=%d implicit=%d mass=%d massTransform=%d): %s"),
			Simulatable != nullptr, Implicits != nullptr, Mass != nullptr, MassToLocal != nullptr,
			*DescribeCollision(Record.RootTransform));
		return false;
	}
	TSet<int32> Leaves;
	for (int32 Leaf : Record.LeafTransforms)
	{
		if (!Geometry->Children.IsValidIndex(Leaf) || Leaves.Contains(Leaf) ||
			!Simulatable->IsValidIndex(Leaf) || !Implicits->IsValidIndex(Leaf) || !Mass->IsValidIndex(Leaf) ||
			!MassToLocal->IsValidIndex(Leaf) ||
			!Geometry->Children[Leaf].IsEmpty() || !Geometry->IsRigid(Leaf) || !(*Simulatable)[Leaf] ||
			!(*Implicits)[Leaf] || !(*Implicits)[Leaf]->HasBoundingBox() ||
			!FMath::IsFinite((*Mass)[Leaf]) || (*Mass)[Leaf] <= 0 || (*MassToLocal)[Leaf].ContainsNaN())
		{
			Error = FString::Printf(TEXT("Authored fracture leaf %d has no valid simulatable collision/mass: %s"),
				Leaf, *DescribeCollision(Leaf));
			return false;
		}
		const auto Bounds = (*Implicits)[Leaf]->BoundingBox();
		if (FVector(Bounds.Min()).ContainsNaN() || FVector(Bounds.Max()).ContainsNaN() ||
			Bounds.Extents().GetMin() <= 0)
		{
			Error = FString::Printf(TEXT("Authored fracture leaf %d has invalid collision bounds: %s"),
				Leaf, *DescribeCollision(Leaf));
			return false;
		}
		Leaves.Add(Leaf);
	}
	for (int32 Anchor : Record.Anchors)
	{
		if (!Leaves.Contains(Anchor))
		{
			Error = TEXT("Fracture anchor is not an authored collision leaf");
			return false;
		}
	}
	if (Record.Recipe == EDublinFractureRecipe::StructuralPilot || IsDetailedRecipe(Record.Recipe))
	{
		if (!Record.bNaniteReady || !Collection.HasNaniteData())
		{
			Error = TEXT("StructuralPilot requires valid Nanite data; render fallback is not a valid bake");
			return false;
		}
		if (!DublinStructural::ValidateCookedCollision(Collection, Record, Error)) { return false; }
	}
	Error.Reset();
	return true;
}
