#include "City/DublinCityFractureLibrary.h"

#include "Chaos/Convex.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "GeometryCollection/Facades/CollectionConnectionGraphFacade.h"
#include "Misc/ByteSwap.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformTime.h"
#include "Hash/Blake3.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>
#endif

namespace
{
	struct FHashMeasurement
	{
		uint64 EncodedBytes = 0;
		uint64 ConvexOccurrences = 0;
		double HashUpdateSeconds = 0;
		TArray<uint8> CanonicalBytes;
	};

	// Canonical stream v1 is unchanged: little-endian integers, finite binary64 (+0),
	// byte booleans and length-prefixed UTF-8. Certificate v1 uses MD5; v2 uses BLAKE3.
	// Both are content-integrity fingerprints, not authentication.
	class FCollisionDigest
	{
	public:
		explicit FCollisionDigest(int32 InAlgorithm = 1, FHashMeasurement* InMeasurement = nullptr)
			: Measurement(InMeasurement), Algorithm(InAlgorithm) {}
		bool bValid = true;

		FORCEINLINE void Byte(uint8 V)
		{
			if (Used == sizeof(Buffer)) { Flush(); }
			Buffer[Used++] = V;
		}
		FORCEINLINE void Integer(int64 V)
		{
			const uint64 Bits = INTEL_ORDER64(static_cast<uint64>(V));
			Append(&Bits, sizeof(Bits));
		}
		FORCEINLINE void Number(double V)
		{
			const uint64 Bits = NumberBits(V);
			Append(&Bits, sizeof(Bits));
		}
		template<typename T> FORCEINLINE void Vector(const T& V)
		{
			const uint64 Words[] = {NumberBits(V.X), NumberBits(V.Y), NumberBits(V.Z)};
			Append(Words, sizeof(Words));
		}
		template<typename T> FORCEINLINE void Transform(const T& V)
		{
			const auto P = V.GetTranslation(), S = V.GetScale3D();
			const auto Q = V.GetRotation();
			const uint64 Words[] = {NumberBits(P.X), NumberBits(P.Y), NumberBits(P.Z),
				NumberBits(Q.X), NumberBits(Q.Y), NumberBits(Q.Z), NumberBits(Q.W),
				NumberBits(S.X), NumberBits(S.Y), NumberBits(S.Z)};
			Append(Words, sizeof(Words));
		}
		void String(const FString& V)
		{
			const FTCHARToUTF8 Utf8(*V);
			Integer(Utf8.Length());
			Append(Utf8.Get(), Utf8.Length());
		}
		void Integers(const TArray<int32>& Values)
		{
			Integer(Values.Num());
			for (int32 V : Values) { Integer(V); }
		}
		FString Finish()
		{
			Flush();
			if (Algorithm == 2) { return LexToString(Blake3.Finalize()); }
			uint8 Result[16];
			Hash.Final(Result);
			return BytesToHex(Result, sizeof(Result)).ToLower();
		}
		bool Shape(const Chaos::FImplicitObject* Value, int32 Depth = 0)
		{
			Byte(Value != nullptr);
			if (!Value) { return true; }
			if (Depth > 16) { return false; }
			Integer(static_cast<int64>(Value->GetType()));
			Byte(Value->HasBoundingBox());
			if (Value->HasBoundingBox())
			{
				const auto Bounds = Value->BoundingBox();
				Vector(Bounds.Min()); Vector(Bounds.Max());
			}
			Number(Value->GetMarginf());
			if (const auto* Hull = Value->GetObject<Chaos::FConvex>())
			{
				if (Measurement) { ++Measurement->ConvexOccurrences; }
				Number(Hull->GetVolume());
				Integer(Hull->NumVertices());
				for (int32 I = 0; I < Hull->NumVertices(); ++I) { Vector(Hull->GetVertex(I)); }
				Integer(Hull->NumPlanes());
				for (int32 P = 0; P < Hull->NumPlanes(); ++P)
				{
					const auto Plane = Hull->GetPlane(P);
					Vector(Plane.X()); Vector(Plane.Normal());
					Integer(Hull->NumPlaneVertices(P));
					for (int32 V = 0; V < Hull->NumPlaneVertices(P); ++V) { Integer(Hull->GetPlaneVertex(P, V)); }
				}
				Integer(Hull->NumEdges());
				for (int32 E = 0; E < Hull->NumEdges(); ++E)
				{
					for (int32 End = 0; End < 2; ++End)
					{
						Integer(Hull->GetEdgeVertex(E, End));
						Integer(Hull->GetEdgePlane(E, End));
					}
				}
				return bValid;
			}
			if (const auto* Wrapped = Value->GetObject<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>())
			{
				Transform(FTransform(Wrapped->GetTransform()));
				return Shape(Wrapped->GetTransformedObject(), Depth + 1);
			}
			if (const auto* Union = Value->GetObject<Chaos::FImplicitObjectUnion>())
			{
				Integer(Union->GetObjects().Num());
				for (const auto& Part : Union->GetObjects())
				{
					if (!Shape(Part.GetReference(), Depth + 1)) { return false; }
				}
				return bValid;
			}
			return false;
		}
	private:
		FORCEINLINE uint64 NumberBits(double V)
		{
			if (!FMath::IsFinite(V)) { bValid = false; return 0; }
			if (V == 0) { V = 0; }
			uint64 Bits;
			FMemory::Memcpy(&Bits, &V, sizeof(Bits));
			return INTEL_ORDER64(Bits);
		}
		FORCEINLINE void Append(const void* Data, uint32 Size)
		{
			if (Size == 0) { return; }
			if (Size > sizeof(Buffer) - Used)
			{
				Flush();
				if (Size >= sizeof(Buffer))
				{
					const uint32 WholeBlocks = Size - Size % sizeof(Buffer);
					UpdateHash(static_cast<const uint8*>(Data), WholeBlocks);
					Data = static_cast<const uint8*>(Data) + WholeBlocks;
					Size -= WholeBlocks;
				}
			}
			FMemory::Memcpy(Buffer + Used, Data, Size);
			Used += Size;
		}
		void Flush()
		{
			if (Used > 0) { UpdateHash(Buffer, Used); Used = 0; }
		}
		void UpdateHash(const uint8* Data, uint32 Size)
		{
			if (Measurement)
			{
				Measurement->EncodedBytes += Size;
				Measurement->CanonicalBytes.Append(Data, Size);
			}
			const double Start = Measurement ? FPlatformTime::Seconds() : 0;
			if (Algorithm == 2) { Blake3.Update(Data, Size); }
			else { Hash.Update(Data, Size); }
			if (Measurement) { Measurement->HashUpdateSeconds += FPlatformTime::Seconds() - Start; }
		}
		FHashMeasurement* Measurement;
		int32 Algorithm;
		FMD5 Hash;
		FBlake3 Blake3;
		uint8 Buffer[16384];
		uint32 Used = 0;
	};

	bool Fail(FString& Error, const TCHAR* Reason)
	{
		Error = FString(TEXT("Collision certification: ")) + Reason;
		return false;
	}

	bool WellFormed(const FString& Value, int32 Length)
	{
		if (Value.Len() != Length) { return false; }
		for (TCHAR C : Value)
		{
			if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f')))) { return false; }
		}
		return true;
	}

	#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionV1ByteEncodingTest,
		"DublinFlight.Destruction.CollisionCertification.NativeV1CanonicalBytes",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FDublinCollisionV1ByteEncodingTest::RunTest(const FString& Parameters)
	{
		FCollisionDigest Writer;
		TArray<uint8> Reference;
		const auto Word = [&Reference](uint64 Bits)
		{
			for (int32 I = 0; I < 8; ++I) { Reference.Add(static_cast<uint8>(Bits >> (I * 8))); }
		};
		const auto Number = [&Word](double V)
		{
			if (V == 0) { V = 0; }
			uint64 Bits;
			FMemory::Memcpy(&Bits, &V, sizeof(Bits));
			Word(Bits);
		};
		// Cross block boundaries with deliberately unaligned scalars and packed vectors/transforms.
		for (int32 I = 0; I < 4097; ++I)
		{
			Writer.Byte(I & 1); Reference.Add(I & 1);
			Writer.Integer(-int64(I)); Word(static_cast<uint64>(-int64(I)));
			const FVector V(double(I) * .25, -double(I), -0.0);
			Writer.Vector(V); Number(V.X); Number(V.Y); Number(V.Z);
			const FTransform T(FQuat::Identity, V, FVector(1, 2, 3));
			Writer.Transform(T);
			Number(V.X); Number(V.Y); Number(V.Z);
			Number(0); Number(0); Number(0); Number(1);
			Number(1); Number(2); Number(3);
		}
		const FString Text = FString::ChrN(32771, TEXT('x')) + TEXT("\u00e9");
		const FTCHARToUTF8 Utf8(*Text);
		Writer.String(Text); Word(Utf8.Length());
		Reference.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Writer.Number(-0.0); Number(-0.0);
		TestEqual(TEXT("Version-1 canonical bytes are unchanged across batching boundaries"),
			Writer.Finish(), FMD5::HashBytes(Reference.GetData(), Reference.Num()));
		FCollisionDigest Invalid;
		Invalid.Number(std::numeric_limits<double>::infinity());
		TestFalse(TEXT("Nonfinite numeric input is still rejected"), Invalid.bValid);
		return !HasAnyErrors();
	}
	#endif
}

static bool ComputeCollisionValidationDigestImpl(const UGeometryCollection& Collection,
	const FDublinFractureRecord& Record, FString& Digest, FString& Error, FHashMeasurement* Measurement)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_ActualContentFingerprint);
	Digest.Reset();
	if (Record.CollisionValidationVersion < 0 || Record.CollisionValidationVersion > 2)
	{
		return Fail(Error, TEXT("unknown fingerprint algorithm"));
	}
	const auto Geometry = Collection.GetGeometryCollection();
	if (!Geometry || !DublinFractureBake::HasValidPieceBudget(Record) || Record.LeafTransforms.Num() != Record.PieceCount ||
		Record.Anchors.IsEmpty() || Record.Anchors.Num() >= Record.PieceCount ||
		Record.RootTransform != Collection.GetRootIndex() || !Geometry->Transform.IsValidIndex(Record.RootTransform))
	{
		return Fail(Error, TEXT("invalid record/root/leaf contract"));
	}
	const int32 Count = Geometry->Transform.Num();
	const auto* Mass = Geometry->FindAttribute<float>(TEXT("Mass"), FGeometryCollection::TransformGroup);
	const auto* MassToLocal = Geometry->FindAttribute<FTransform>(TEXT("MassToLocal"), FGeometryCollection::TransformGroup);
	const auto* Simulatable = Geometry->FindAttribute<bool>(FGeometryCollection::SimulatableParticlesAttribute, FGeometryCollection::TransformGroup);
	const auto* Implicits = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(TEXT("Implicits"), FGeometryCollection::TransformGroup);
	const auto* External = Geometry->FindAttribute<Chaos::FImplicitObjectPtr>(FGeometryCollection::ExternalCollisionsAttribute, FGeometryCollection::TransformGroup);
	const auto* Anchored = Geometry->FindAttribute<bool>(TEXT("Anchored"), FGeometryCollection::TransformGroup);
	const auto* DynamicState = Geometry->FindAttribute<uint8>(TEXT("DynamicState"), FGeometryCollection::TransformGroup);
	if (!Mass || Mass->Num() != Count || !MassToLocal || MassToLocal->Num() != Count ||
		!Simulatable || Simulatable->Num() != Count || !Implicits || Implicits->Num() != Count ||
		(External && External->Num() != Count) || (Anchored && Anchored->Num() != Count) ||
		(DynamicState && DynamicState->Num() != Count) ||
		Geometry->Parent.Num() != Count || Geometry->Children.Num() != Count ||
		Geometry->SimulationType.Num() != Count || Geometry->StatusFlags.Num() != Count ||
		Geometry->InitialDynamicState.Num() != Count || !(*Implicits)[Record.RootTransform])
	{
		return Fail(Error, TEXT("missing or inconsistent physical attributes"));
	}
	const GeometryCollection::Facades::FCollectionConnectionGraphFacade Graph(*Geometry);
	if (!Graph.IsValid() || !Graph.HasValidConnections() || Graph.NumConnections() == 0)
	{
		return Fail(Error, TEXT("missing or invalid contact graph"));
	}
	FCollisionDigest H(Record.CollisionValidationVersion == 2 ? 2 : 1, Measurement);
	// Lifecycle readiness is checked by callers. Bake statistics/diagnostics, object paths,
	// GUIDs and render/material/UV payloads are not inputs to the static physical proof.
	// Source digests still bind the source identity; actual collision is hashed independently.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_FingerprintRecordHeader);
		H.String(TEXT("Dublin.CollisionValidation.v1"));
		H.Integer(1);
		H.Integer(static_cast<uint8>(Record.Recipe));
		H.String(Record.SourceId); H.String(Record.SourceDigest); H.String(Record.GeometryDigest);
		H.Integer(Record.CollisionStorageVersion);
		H.Integer(Record.PieceCount); H.Integer(Record.HullCount); H.Integer(Record.RootTransform);
		H.Integers(Record.LeafTransforms); H.Integers(Record.Anchors);
		H.Number(Record.SourceVolumeCm3); H.Number(Record.StructuralVolumeCm3); H.Number(Record.RetainedVolumeCm3);
		H.Integer(Record.DetailTier); H.Integer(Record.MemberCount); H.String(Record.PlanReason);
		H.Number(Record.BroadSurfaceAreaM2); H.Number(Record.LargestMemberAreaM2); H.Byte(Record.bDenseForOversize);
		H.Byte(Record.bNaniteReady);
		H.Integer(Record.ProbeVersion);
		H.Integer(Record.VoidSamplesCm.Num());
		for (const FVector& Point : Record.VoidSamplesCm) { H.Vector(Point); }
		H.Integer(Record.CollisionProbes.Num());
		for (const FDublinFractureProbe& Probe : Record.CollisionProbes)
		{
			H.Vector(Probe.StartCm); H.Vector(Probe.EndCm); H.Byte(Probe.bExpectHit);
		}
		H.Byte(Collection.bOptimizeConvexes); H.Byte(Collection.bImportCollisionFromSource);
		H.Integer(Collection.GetRootIndex());
		H.Byte(External != nullptr); H.Byte(Anchored != nullptr); H.Byte(DynamicState != nullptr);
		H.Integer(Count);
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_FingerprintTransformStream);
		for (int32 I = 0; I < Count; ++I)
		{
			if ((Geometry->Parent[I] != INDEX_NONE && !Geometry->Transform.IsValidIndex(Geometry->Parent[I])) ||
				Geometry->Children[I].Num() > Count)
			{
				return Fail(Error, TEXT("invalid rest hierarchy"));
			}
			H.Transform(Geometry->Transform[I]); H.Integer(Geometry->Parent[I]);
			TArray<int32> Children = Geometry->Children[I].Array();
			Children.Sort();
			for (int32 Child : Children)
			{
				if (!Geometry->Transform.IsValidIndex(Child)) { return Fail(Error, TEXT("invalid child index")); }
			}
			H.Integers(Children);
			H.Integer(Geometry->SimulationType[I]); H.Integer(Geometry->StatusFlags[I]);
			H.Integer(Geometry->InitialDynamicState[I]);
			H.Byte((*Simulatable)[I]); H.Number((*Mass)[I]); H.Transform((*MassToLocal)[I]);
			if (Anchored) { H.Byte((*Anchored)[I]); }
			if (DynamicState) { H.Integer((*DynamicState)[I]); }
			TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(DublinFlight_Collision_FingerprintRootImplicits, I == Record.RootTransform);
			if (!H.Shape((*Implicits)[I].GetReference()) ||
				(External && !H.Shape((*External)[I].GetReference())))
			{
				return Fail(Error, TEXT("unsupported/nonfinite cooked or persistent collision shape"));
			}
		}
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_FingerprintContactGraph);
		H.Integer(Graph.NumConnections()); H.Byte(Graph.HasContactAreas());
		for (int32 I = 0; I < Graph.NumConnections(); ++I)
		{
			const auto Edge = Graph.GetConnection(I);
			H.Integer(Edge.Key); H.Integer(Edge.Value);
			if (Graph.HasContactAreas()) { H.Number(Graph.GetConnectionContactArea(I)); }
		}
	}
	if (!H.bValid) { return Fail(Error, TEXT("nonfinite physical proof input")); }
	Digest = H.Finish();
	Error.Reset();
	return true;
}

bool DublinFractureBake::ComputeCollisionValidationDigest(const UGeometryCollection& Collection,
	const FDublinFractureRecord& Record, FString& Digest, FString& Error)
{
	return ComputeCollisionValidationDigestImpl(Collection, Record, Digest, Error, nullptr);
}

bool DublinFractureBake::ValidateRuntimeCollisionData(const UGeometryCollection& Collection,
	const FDublinFractureRecord& Record, FString& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DublinFlight_Collision_RuntimeValidation);
	if (Record.CollisionValidationVersion == 0 && Record.CollisionValidationDigest.IsEmpty())
	{
		return ValidateCollisionData(Collection, Record, Error);
	}
	if (Record.CollisionValidationVersion != 1 && Record.CollisionValidationVersion != 2)
	{
		return Fail(Error, TEXT("unknown/inconsistent validation version; explicit recertification required"));
	}
	if (!WellFormed(Record.CollisionValidationDigest, Record.CollisionValidationVersion == 2 ? 64 : 32))
	{
		return Fail(Error, TEXT("missing/malformed algorithm-specific digest; explicit recertification required"));
	}
	if ((Record.Recipe == EDublinFractureRecipe::StructuralPilot || IsDetailedRecipe(Record.Recipe)) &&
		(!Record.bNaniteReady || !Collection.HasNaniteData()))
	{
		return Fail(Error, TEXT("required Nanite readiness is missing"));
	}
	FString Actual;
	if (!ComputeCollisionValidationDigest(Collection, Record, Actual, Error)) { return false; }
	if (Actual != Record.CollisionValidationDigest)
	{
		return Fail(Error, TEXT("actual cooked collision/record fingerprint mismatch; intact geometry must be retained"));
	}
	Error.Reset();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCollisionHashCostTest,
	"DublinFlight.Destruction.CollisionCertification.NativeHashCostBreakdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCollisionHashCostTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UDublinCityFractureLibrary> Library(DublinFractureBake::FindLibrary());
	if (!TestNotNull(TEXT("Certified saved library"), Library.Get())) { return false; }
	for (const TCHAR* Id : {TEXT("osm/relation/289528"), TEXT("osm/relation/3375620")})
	{
		const FDublinFractureRecord* Record = Library->Find(Id);
		if (!TestNotNull(Id, Record) || !TestEqual(TEXT("1536-piece measurement fixture"), Record->PieceCount, 1536)) { return false; }
		TStrongObjectPtr<UGeometryCollection> Asset(Record->Collection.LoadSynchronous());
		if (!TestNotNull(TEXT("Actual saved asset"), Asset.Get())) { return false; }
		FDublinFractureRecord V1 = *Record;
		V1.CollisionValidationVersion = 1;
		const FString Golden = FString(Id) == TEXT("osm/relation/289528")
			? TEXT("d87bf9f203a149088ee8b58d3846ecf5") : TEXT("efb86675bf2c79b395732c58b9c1f02e");
		V1.CollisionValidationDigest = Golden;
		FHashMeasurement Measurement;
		FString Digest, Error;
		const double Start = FPlatformTime::Seconds();
		if (!ComputeCollisionValidationDigestImpl(*Asset.Get(), V1, Digest, Error, &Measurement))
		{
			AddError(Error); return false;
		}
		const double TotalMs = (FPlatformTime::Seconds() - Start) * 1000;
		const double HashStart = FPlatformTime::Seconds();
		const FString Preencoded = FMD5::HashBytes(Measurement.CanonicalBytes.GetData(), Measurement.CanonicalBytes.Num());
		const double PreencodedMs = (FPlatformTime::Seconds() - HashStart) * 1000;
		TestEqual(TEXT("Measured stream still matches the original v1 golden"), Digest, Golden);
		TestEqual(TEXT("Preencoded bytes have exactly the same digest"), Preencoded, Digest);
		TestEqual(TEXT("All canonical bytes captured"), uint64(Measurement.CanonicalBytes.Num()), Measurement.EncodedBytes);
		AddInfo(FString::Printf(TEXT("HashBreakdown source=%s storage=%d bytes=%llu convexOccurrences=%llu totalWithCaptureMs=%.3f hashUpdateMs=%.3f preencodedMD5Ms=%.3f probes=%d voids=%d"),
			Id, Record->CollisionStorageVersion, Measurement.EncodedBytes, Measurement.ConvexOccurrences,
			TotalMs, Measurement.HashUpdateSeconds * 1000, PreencodedMs, Record->CollisionProbes.Num(), Record->VoidSamplesCm.Num()));
		FDublinFractureRecord V2 = *Record;
		V2.CollisionValidationVersion = 2;
		FHashMeasurement BlakeMeasurement;
		const double BlakeStart = FPlatformTime::Seconds();
		FString BlakeDigest;
		if (!ComputeCollisionValidationDigestImpl(*Asset.Get(), V2, BlakeDigest, Error, &BlakeMeasurement))
		{
			AddError(Error); return false;
		}
		const double BlakeTotalMs = (FPlatformTime::Seconds() - BlakeStart) * 1000;
		const double PreBlakeStart = FPlatformTime::Seconds();
		const FString PreBlake = LexToString(FBlake3::HashBuffer(Measurement.CanonicalBytes.GetData(), Measurement.CanonicalBytes.Num()));
		const double PreBlakeMs = (FPlatformTime::Seconds() - PreBlakeStart) * 1000;
		TestTrue(TEXT("V2 hashes exactly the V1 canonical physical bytes"), BlakeMeasurement.CanonicalBytes == Measurement.CanonicalBytes);
		TestEqual(TEXT("Streamed Core BLAKE3 equals its preencoded digest"), BlakeDigest, PreBlake);
		double NormalMs = 0;
		for (int32 I = 0; I < 8; ++I)
		{
			const double Sample = FPlatformTime::Seconds();
			if (!DublinFractureBake::ComputeCollisionValidationDigest(*Asset.Get(), V2, BlakeDigest, Error))
			{
				AddError(Error); return false;
			}
			NormalMs += (FPlatformTime::Seconds() - Sample) * 1000;
			TestEqual(TEXT("Uninstrumented V2 preserves all bytes"), BlakeDigest, PreBlake);
		}
		AddInfo(FString::Printf(TEXT("Blake3Comparison source=%s bytes=%llu convexOccurrences=%llu totalWithCaptureMs=%.3f hashUpdateMs=%.3f preencodedBlake3Ms=%.3f normalFingerprintAvgMs=%.3f isolatedSpeedup=%.2fx digest=%s"),
			Id, BlakeMeasurement.EncodedBytes, BlakeMeasurement.ConvexOccurrences, BlakeTotalMs,
			BlakeMeasurement.HashUpdateSeconds * 1000, PreBlakeMs, NormalMs / 8, PreencodedMs / FMath::Max(PreBlakeMs, .000001), *BlakeDigest));
	}
	return !HasAnyErrors();
}
#endif
