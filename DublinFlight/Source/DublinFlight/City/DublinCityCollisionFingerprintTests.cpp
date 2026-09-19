#include "City/DublinCityFractureLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "City/DublinCityData.h"
#include "City/DublinCityDestruction.h"
#include "City/DublinCityStructural.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/Convex.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Engine/World.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	bool LoadFixture(FAutomationTestBase& Test, FDublinCityData& Data,
		FDublinFractureRecord& Record, UGeometryCollection*& Asset, const FDublinCityBuilding*& Building)
	{
		FString Error;
		if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { Test.AddError(Error); return false; }
		const UDublinCityFractureLibrary* Library = DublinFractureBake::FindLibrary();
		if (!Test.TestNotNull(TEXT("Saved collision library"), Library)) { return false; }
		const FDublinFractureRecord* Saved = Library->Find(TEXT("osm/way/233804853"));
		if (!Test.TestNotNull(TEXT("Saved native collision fixture"), Saved)) { return false; }
		Record = *Saved;
		Asset = Record.Collection.LoadSynchronous();
		Building = Data.Buildings.FindByPredicate([&Record](const auto& B) { return B.Id == Record.SourceId; });
		return Test.TestNotNull(TEXT("Actual saved asset"), Asset) && Test.TestNotNull(TEXT("Actual source"), Building);
	}

	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> RoundTrip(FGeometryCollection& Geometry)
	{
		TArray<uint8> Bytes;
		FCustomVersionContainer Versions;
		{
			FMemoryWriter Writer(Bytes, true);
			Chaos::FChaosArchive Archive(Writer);
			Geometry.Serialize(Archive);
			Versions = Writer.GetCustomVersions();
		}
		auto Copy = MakeShared<FGeometryCollection, ESPMode::ThreadSafe>();
		{
			FMemoryReader Reader(Bytes, true);
			Reader.SetCustomVersions(Versions);
			Chaos::FChaosArchive Archive(Reader);
			Copy->Serialize(Archive);
		}
		return Copy;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionFingerprintMutationTest,
	"DublinFlight.Destruction.CollisionCertification.NativeContentMutations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCollisionFingerprintMutationTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FDublinFractureRecord Record; UGeometryCollection* Saved = nullptr;
	const FDublinCityBuilding* Building = nullptr;
	if (!LoadFixture(*this, Data, Record, Saved, Building)) { return false; }
	// Duplicate render metadata but independently serialize physical data before mutating it.
	TStrongObjectPtr<UGeometryCollection> Asset(DuplicateObject<UGeometryCollection>(Saved, GetTransientPackage()));
	Asset->SetGeometryCollection(RoundTrip(*Saved->GetGeometryCollection()));
	FString Error;
	Record.CollisionValidationVersion = 0;
	Record.CollisionValidationDigest.Reset();
	if (!TestTrue(TEXT("Legacy version zero retains the full validation path"),
		DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Record, Error)) ||
		!TestTrue(TEXT("Certification runs the full proof and building probes"),
			DublinFractureBake::CertifyCollisionData(*Asset.Get(), *Building, Record, Error)))
	{
		AddError(Error); return false;
	}
	TestEqual(TEXT("Schema remains version three"), UDublinCityFractureLibrary::CurrentBakeVersion, 3);
	TestEqual(TEXT("New certificate version"), Record.CollisionValidationVersion, 2);
	TestEqual(TEXT("Lowercase BLAKE3 integrity digest length"), Record.CollisionValidationDigest.Len(), 64);
	if (!TestTrue(TEXT("Certified actual saved physical contents accepted"),
		DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Record, Error))) { AddError(Error); return false; }
	const FDublinFractureRecord Certified = Record;
	FDublinFractureRecord Legacy = Certified;
	Legacy.CollisionValidationVersion = 1;
	if (!DublinFractureBake::ComputeCollisionValidationDigest(*Asset.Get(), Legacy, Legacy.CollisionValidationDigest, Error) ||
		!DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Legacy, Error))
	{
		AddError(Error); return false;
	}
	auto RejectRecord = [&](const TCHAR* Reason)
	{
		TestFalse(Reason, DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Record, Error));
		TestFalse(TEXT("Rejection reports explicit cause"), Error.IsEmpty());
		if (Record.CollisionValidationVersion == Certified.CollisionValidationVersion &&
			Record.CollisionValidationDigest == Certified.CollisionValidationDigest)
		{
			FDublinFractureRecord ChangedLegacy = Record;
			ChangedLegacy.CollisionValidationVersion = 1;
			ChangedLegacy.CollisionValidationDigest = Legacy.CollisionValidationDigest;
			TestFalse(TEXT("V1 rejects the same physical record corruption"),
				DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), ChangedLegacy, Error));
		}
		Record = Certified;
	};
	Record.CollisionValidationVersion = 3; RejectRecord(TEXT("Unknown version rejected"));
	Record.CollisionValidationVersion = 1; RejectRecord(TEXT("Algorithm declaration cannot reuse another algorithm's digest"));
	Record.CollisionValidationDigest.Reset(); RejectRecord(TEXT("Missing v2 digest cannot fallback"));
	Record.CollisionValidationDigest = TEXT("not-a-digest"); RejectRecord(TEXT("Malformed digest rejected"));
	Record.CollisionValidationDigest = FString::ChrN(64, TEXT('0')); RejectRecord(TEXT("Well-formed mismatch rejected"));
	Record.CollisionValidationVersion = 0; RejectRecord(TEXT("Legacy version with a certificate is inconsistent"));
	Record.SourceDigest[0] = Record.SourceDigest[0] == TEXT('0') ? TEXT('1') : TEXT('0'); RejectRecord(TEXT("Source identity bound"));
	++Record.HullCount; RejectRecord(TEXT("Physical allocation cost bound"));
	++Record.StructuralVolumeCm3; RejectRecord(TEXT("Conservation baseline bound"));
	Record.Anchors[0] = Record.LeafTransforms.Last(); RejectRecord(TEXT("Anchor mapping bound"));
	Swap(Record.LeafTransforms[0], Record.LeafTransforms[1]); RejectRecord(TEXT("Leaf order bound"));
	Record.CollisionProbes[0].StartCm.X += 1; RejectRecord(TEXT("Probe coordinates bound"));
	Record.CollisionProbes[0].bExpectHit = !Record.CollisionProbes[0].bExpectHit; RejectRecord(TEXT("Probe expectation bound"));
	Record.CollisionProbes[0].bExpectHit = !Record.CollisionProbes[0].bExpectHit;
	TestFalse(TEXT("Certification cannot bless a failing building probe contract"),
		DublinFractureBake::CertifyCollisionData(*Asset.Get(), *Building, Record, Error));
	TestEqual(TEXT("Failed probe certification preserves existing digest"), Record.CollisionValidationDigest, Certified.CollisionValidationDigest);
	Record = Certified;
	auto Geometry = Asset->GetGeometryCollection();
	const int32 Leaf = Record.LeafTransforms[0];
	auto RejectGeometry = [&](const TCHAR* Reason)
	{
		TestFalse(Reason, DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Record, Error));
		TestFalse(TEXT("V1 also detects actual geometry corruption"),
			DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Legacy, Error));
	};
	const FTransform3f Transform = Geometry->Transform[Leaf];
	Geometry->Transform[Leaf].AddToTranslation(FVector3f(1, 0, 0));
	RejectGeometry(TEXT("Actual rest transform bound"));
	Geometry->Transform[Leaf] = Transform;
	auto& MassToLocal = Geometry->ModifyAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
	const FTransform MassTransform = MassToLocal[Leaf];
	MassToLocal[Leaf].AddToTranslation(FVector(0, 1, 0));
	RejectGeometry(TEXT("Actual mass transform bound"));
	MassToLocal[Leaf] = MassTransform;
	auto& Mass = Geometry->ModifyAttribute<float>(TEXT("Mass"), FGeometryCollection::TransformGroup);
	const float BeforeMass = Mass[Leaf]; Mass[Leaf] += 1;
	RejectGeometry(TEXT("Actual mass bound")); Mass[Leaf] = BeforeMass;
	auto& Anchored = Geometry->ModifyAttribute<bool>(TEXT("Anchored"), FGeometryCollection::TransformGroup);
	Anchored[Leaf] = !Anchored[Leaf]; RejectGeometry(TEXT("Actual anchoring bound")); Anchored[Leaf] = !Anchored[Leaf];
	const int32 Initial = Geometry->InitialDynamicState[Leaf];
	Geometry->InitialDynamicState[Leaf] = Initial + 1; RejectGeometry(TEXT("Initial dynamic state bound"));
	Geometry->InitialDynamicState[Leaf] = Initial;
	auto& Implicits = Geometry->ModifyAttribute<Chaos::FImplicitObjectPtr>(TEXT("Implicits"), FGeometryCollection::TransformGroup);
	const Chaos::FImplicitObject* Shape = Implicits[Leaf].GetReference();
	while (const auto* Wrapper = Shape->GetObject<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>())
	{
		Shape = Wrapper->GetTransformedObject();
	}
	const Chaos::FConvex* Hull = Shape->GetObject<Chaos::FConvex>();
	if (!TestNotNull(TEXT("Actual native convex"), Hull)) { return false; }
	auto& Vertex = const_cast<Chaos::FConvex::FVec3Type&>(Hull->GetVertex(0));
	const auto OriginalVertex = Vertex;
	Vertex.X += 1;
	RejectGeometry(TEXT("Actual convex vertex bound even with unchanged cached AABB"));
	FDublinFractureRecord RejectedSeal = Record;
	TestFalse(TEXT("Certification cannot bless corrupted collision"),
		DublinFractureBake::CertifyCollisionData(*Asset.Get(), *Building, RejectedSeal, Error));
	TestEqual(TEXT("Failed certification preserves prior digest"), RejectedSeal.CollisionValidationDigest, Record.CollisionValidationDigest);
	TestEqual(TEXT("Failed certification preserves prior version"), RejectedSeal.CollisionValidationVersion, Record.CollisionValidationVersion);
	Vertex = OriginalVertex;
	auto& External = Geometry->ModifyAttribute<Chaos::FImplicitObjectPtr>(
		FGeometryCollection::ExternalCollisionsAttribute, FGeometryCollection::TransformGroup);
	const auto SavedExternal = External[Leaf];
	if (!TestNotNull(TEXT("Negative fixture must have a real persistent authored leaf"), SavedExternal.GetReference())) { return false; }
	External[Leaf] = nullptr; RejectGeometry(TEXT("Persistent authored collision bound")); External[Leaf] = SavedExternal;
	GeometryCollection::Facades::FCollectionConnectionGraphFacade Graph(*Geometry);
	Graph.Connect(Record.LeafTransforms[0], Record.LeafTransforms[1]);
	RejectGeometry(TEXT("Actual contact graph bound"));
	Asset->SetGeometryCollection(RoundTrip(*Saved->GetGeometryCollection()));
	if (!TestTrue(TEXT("Restored certified data still accepted"),
		DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Record, Error))) { AddError(Error); return false; }
	FDublinCityBuilding WrongSource = *Building;
	WrongSource.Mesh.VerticesCm[0].X += 1;
	TestFalse(TEXT("Actual source corruption cannot be certified"),
		DublinFractureBake::CertifyCollisionData(*Asset.Get(), WrongSource, Record, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionFingerprintSerializationTest,
	"DublinFlight.Destruction.CollisionCertification.NativeSerializationAndRecook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCollisionFingerprintSerializationTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FDublinFractureRecord Record; UGeometryCollection* Saved = nullptr;
	const FDublinCityBuilding* Building = nullptr;
	if (!LoadFixture(*this, Data, Record, Saved, Building)) { return false; }
	FString Error, Before, After;
	if (!DublinFractureBake::CertifyCollisionData(*Saved, *Building, Record, Error) ||
		!DublinFractureBake::ComputeCollisionValidationDigest(*Saved, Record, Before, Error)) { AddError(Error); return false; }
	TStrongObjectPtr<UGeometryCollection> Copy(DuplicateObject<UGeometryCollection>(Saved, GetTransientPackage()));
	Copy->SetGeometryCollection(RoundTrip(*Saved->GetGeometryCollection()));
	if (!DublinFractureBake::ComputeCollisionValidationDigest(*Copy.Get(), Record, After, Error)) { AddError(Error); return false; }
	if (!TestEqual(TEXT("Native physical serialization preserves actual-content fingerprint"), After, Before)) { return false; }
	if (!TestTrue(TEXT("Serialized physical contents use the certified fast path"),
		DublinFractureBake::ValidateRuntimeCollisionData(*Copy.Get(), Record, Error))) { AddError(Error); return false; }
	Copy->InvalidateCollection();
	Copy->CreateSimulationData();
	if (!DublinFractureBake::ValidateCollisionData(*Copy.Get(), Record, Error) ||
		!DublinStructural::ValidateBuildingCollision(*Copy.Get(), *Building, Record, Error) ||
		!DublinFractureBake::ComputeCollisionValidationDigest(*Copy.Get(), Record, After, Error))
	{
		AddError(Error); return false;
	}
	TestEqual(TEXT("Native recook preserves certified physical contents; any difference blocks rollout"), After, Before);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionCertifiedCatalogTest,
	"DublinFlight.Destruction.CollisionCertification.NativeSaved1044",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCollisionCertifiedCatalogTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FString Error;
	if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { AddError(Error); return false; }
	TStrongObjectPtr<UDublinCityFractureLibrary> Library(DublinFractureBake::FindLibrary());
	if (!TestNotNull(TEXT("Saved certified library"), Library.Get()) ||
		!TestEqual(TEXT("All 1044 saved records"), Library->Records.Num(), 1044)) { return false; }
	int32 Passed = 0, Version1 = 0, Version2 = 0;
	for (const auto& Record : Library->Records)
	{
		const auto* Building = Data.Buildings.FindByPredicate([&Record](const auto& B) { return B.Id == Record.SourceId; });
		const auto* Asset = Record.Collection.LoadSynchronous();
		if (!Building || !Asset || (Record.CollisionValidationVersion != 1 && Record.CollisionValidationVersion != 2) ||
			!DublinFractureBake::IsCurrentRecord(*Building, Record) ||
			!DublinFractureBake::ValidateRuntimeCollisionData(*Asset, Record, Error))
		{
			AddError(Record.SourceId + TEXT(": saved certification failed: ") + Error); return false;
		}
		++Passed;
		Version1 += Record.CollisionValidationVersion == 1;
		Version2 += Record.CollisionValidationVersion == 2;
		if (Passed % 32 == 0) { CollectGarbage(RF_NoFlags); }
	}
	AddInfo(FString::Printf(TEXT("Accepted all %d actual saved certified collision assets version1=%d version2=%d"), Passed, Version1, Version2));
	return Passed == 1044;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionSavedV1TimingTest,
	"DublinFlight.Destruction.CollisionCertification.NativeSavedV1GoldenAndTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCollisionSavedV1TimingTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UDublinCityFractureLibrary> Library(DublinFractureBake::FindLibrary());
	if (!TestNotNull(TEXT("Published certified library"), Library.Get())) { return false; }
	// Captured from the original v1 certification, before batching changes. Do not re-certify these fixtures.
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("osm/relation/3375620"), TEXT("efb86675bf2c79b395732c58b9c1f02e")},
		{TEXT("osm/relation/289528"), TEXT("d87bf9f203a149088ee8b58d3846ecf5")}};
	for (const auto& Case : Cases)
	{
		const FDublinFractureRecord* SavedRecord = Library->Find(Case.Key);
		if (!TestNotNull(TEXT("Original representative record"), SavedRecord)) { return false; }
		FDublinFractureRecord Legacy = *SavedRecord;
		Legacy.CollisionValidationVersion = 1;
		Legacy.CollisionValidationDigest = Case.Value;
		const FDublinFractureRecord* Record = &Legacy;
		if (!TestEqual(TEXT("Representative has 1536 physical pieces"), Record->PieceCount, 1536) ||
			!TestEqual(TEXT("Original certificate version retained"), Record->CollisionValidationVersion, 1) ||
			!TestEqual(TEXT("Stored original v1 digest is unchanged"), Record->CollisionValidationDigest, FString(Case.Value)))
		{
			return false;
		}
		TStrongObjectPtr<UGeometryCollection> Asset(Record->Collection.LoadSynchronous());
		if (!TestNotNull(TEXT("Original saved representative asset"), Asset.Get())) { return false; }
		FString Actual, Error;
		if (!DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), *SavedRecord, Error) ||
			!DublinFractureBake::ValidateRuntimeCollisionData(*Asset.Get(), Legacy, Error))
		{
			AddError(Error); return false;
		}
		if (!DublinFractureBake::ComputeCollisionValidationDigest(*Asset.Get(), *Record, Actual, Error))
		{
			AddError(Error); return false;
		}
		if (!TestEqual(TEXT("Actual content retains the original v1 golden digest"), Actual, FString(Case.Value))) { return false; }
		constexpr int32 Samples = 8;
		double TotalMs = 0, MinMs = TNumericLimits<double>::Max(), MaxMs = 0;
		for (int32 I = 0; I < Samples; ++I)
		{
			const double Start = FPlatformTime::Seconds();
			const bool bValid = DublinFractureBake::ComputeCollisionValidationDigest(*Asset.Get(), *Record, Actual, Error);
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000;
			if (!bValid) { AddError(Error); return false; }
			if (!TestEqual(TEXT("Repeated hashing cannot change the v1 digest"), Actual, FString(Case.Value)) ||
				!TestTrue(TEXT("Microtiming is finite and nonnegative"), FMath::IsFinite(Ms) && Ms >= 0))
			{
				return false;
			}
			TotalMs += Ms; MinMs = FMath::Min(MinMs, Ms); MaxMs = FMath::Max(MaxMs, Ms);
		}
		AddInfo(FString::Printf(TEXT("Fingerprint diagnostic only source=%s pieces=1536 storage=%d samples=%d minMs=%.3f avgMs=%.3f maxMs=%.3f digest=%s; no machine-time/FPS acceptance threshold"),
			Case.Key, Record->CollisionStorageVersion, Samples, MinMs, TotalMs / Samples, MaxMs, *Actual));
	}
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionPreparationBoundaryTest,
	"DublinFlight.Destruction.CollisionCertification.NativePreparationValidationBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCollisionPreparationBoundaryTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data; FDublinFractureRecord Record; UGeometryCollection* Asset = nullptr;
	const FDublinCityBuilding* Building = nullptr;
	if (!LoadFixture(*this, Data, Record, Asset, Building)) { return false; }
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Preparation fixture world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Preparation fixture owner"), Owner)) { return false; }
	UGeometryCollectionComponent* Component = NewObject<UGeometryCollectionComponent>(Owner);
	Owner->AddInstanceComponent(Component);
	Component->SetWorldLocation(Building->PivotCm);
	FDublinFractureRecord Invalid = Record;
	Invalid.CollisionValidationDigest = FString::ChrN(32, TEXT('0'));
	FString Error;
	TestFalse(TEXT("Invalid collision is rejected before any rest-asset binding"),
		DublinFractureBake::PrepareAndRegisterRuntimePhysics(*Component, *Asset, Invalid, Error));
	TestNull(TEXT("Rejected preparation leaves rest collection unbound"), Component->GetRestCollection());
	TestFalse(TEXT("Rejected preparation does not register the component"), Component->IsRegistered());
	TestFalse(TEXT("Rejected preparation does not create physics"), Component->IsPhysicsStateCreated());
	TestNull(TEXT("Rejected preparation cannot expose a proxy"), Component->GetPhysicsProxy());

	UGeometryCollectionComponent* Independent = NewObject<UGeometryCollectionComponent>(Owner);
	Owner->AddInstanceComponent(Independent);
	Independent->SetRestCollection(Asset);
	TestFalse(TEXT("Independent registration still validates its actual rest collection"),
		DublinFractureBake::RegisterRuntimePhysics(*Independent, Invalid, Error));
	TestFalse(TEXT("Independent failed validation does not register"), Independent->IsRegistered());
	TestNull(TEXT("Independent failed validation has no proxy"), Independent->GetPhysicsProxy());
	TestFalse(TEXT("Live probing rejects an unregistered collection"),
		DublinStructural::ValidateRegisteredCollision(*Independent, *Building, Record, Error));

	if (!DublinFractureBake::PrepareAndRegisterRuntimePhysics(*Component, *Asset, Record, Error))
	{
		AddError(Error); return false;
	}
	TestTrue(TEXT("Validated asset is bound"), Component->GetRestCollection() == Asset);
	TestTrue(TEXT("Validated preparation registers live physics"), Component->IsRegistered() && Component->IsPhysicsStateCreated());
	TestTrue(TEXT("City clustering and dynamic state overrides retained"),
		Component->EnableClustering && Component->ObjectType == EObjectStateTypeEnum::Chaos_Object_Dynamic);
	TestTrue(TEXT("City damage thresholds override asset defaults in the original order"),
		Component->GetDamageThreshold() == TArray<float>{100, 75, 50});
	TestFalse(TEXT("City damage propagation remains disabled"), Component->DamagePropagationData.bEnabled);
	TestTrue(TEXT("City removal overrides retained"), !Component->bAllowRemovalOnSleep && !Component->bAllowRemovalOnBreak);
	TestTrue(TEXT("City collision channel configuration retained"),
		Component->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics &&
		Component->GetCollisionObjectType() == ECC_WorldDynamic &&
		Component->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block);
	if (!DublinStructural::ValidateRegisteredCollision(*Component, *Building, Record, Error))
	{
		AddError(Error); return false;
	}
	if (!DublinStructural::ValidateRegisteredProbeEquivalence(*Component, Record, Error))
	{
		AddError(Error); return false;
	}
	FDublinFractureRecord WrongProbe = Record;
	WrongProbe.CollisionProbes[0].bExpectHit = !WrongProbe.CollisionProbes[0].bExpectHit;
	TestFalse(TEXT("Batching cannot waive a wrong live probe expectation"),
		DublinStructural::ValidateRegisteredCollision(*Component, *Building, WrongProbe, Error));
	TestFalse(TEXT("Equivalence checks retain each authored expectation"),
		DublinStructural::ValidateRegisteredProbeEquivalence(*Component, WrongProbe, Error));
	return !HasAnyErrors();
}
#endif
