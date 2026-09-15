#include "City/DublinCityFractureLibrary.h"

#include "Chaos/ImplicitObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "GeometryCollectionProxyData.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

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
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Geometry = Collection.GetGeometryCollection();
	if (!Geometry || Record.PieceCount < 2 || Record.PieceCount > 128 ||
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
	Error.Reset();
	return true;
}
