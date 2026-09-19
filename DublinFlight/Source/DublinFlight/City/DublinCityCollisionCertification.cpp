#include "City/DublinCityFractureLibrary.h"

#if WITH_EDITOR
#include "City/DublinCityData.h"
#include "City/DublinCityStructural.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/Linker.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogDublinCollisionCertification, Log, All);

bool DublinFractureBake::CertifyCollisionData(const UGeometryCollection& Collection, const FDublinCityBuilding& Building,
	FDublinFractureRecord& Record, FString& Error)
{
	if (!IsCurrentRecord(Building, Record))
	{
		Error = TEXT("Collision certification rejected stale source/recipe identity");
		return false;
	}
	// Deliberately bypass either runtime fast path when certifying.
	if (!ValidateCollisionData(Collection, Record, Error) ||
		((Record.Recipe == EDublinFractureRecipe::StructuralPilot || IsDetailedRecipe(Record.Recipe)) &&
			!DublinStructural::ValidateBuildingCollision(Collection, Building, Record, Error)))
	{
		return false;
	}
	FString Digest;
	FDublinFractureRecord Candidate = Record;
	Candidate.CollisionValidationVersion = 2;
	if (!ComputeCollisionValidationDigest(Collection, Candidate, Digest, Error)) { return false; }
	Record.CollisionValidationVersion = 2;
	Record.CollisionValidationDigest = MoveTemp(Digest);
	return true;
}

bool DublinFractureBake::CertifyCurrentLibrary(FString& BackupPath, FString& Error)
{
	BackupPath.Reset();
	FDublinCityData Data;
	if (!DublinCity::LoadCityJson(TEXT("Data/dublin-city.json"), Data, Error)) { return false; }
	UDublinCityFractureLibrary* Library = FindLibrary();
	TStrongObjectPtr<UDublinCityFractureLibrary> PinnedLibrary(Library);
	TArray<FString> Pending;
	if (!Library || Library->GetOutermost()->IsDirty() ||
		Library->Records.Num() != Data.Buildings.Num() ||
		!HasCompleteDetailCoverage(Data.Buildings, *Library, Pending))
	{
		Error = TEXT("Certification requires the complete, current, clean saved library; no bake or partial publication is allowed");
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(Library->GetOutermost()->GetName(),
		FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("CollisionCertification"),
		FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		Error = TEXT("Cannot create project-local certification backup directory");
		return false;
	}
	BackupPath = FPaths::Combine(Directory, TEXT("DA_DublinFractureLibrary.before.uasset"));
	const FMD5Hash BeforeHash = FMD5Hash::HashFile(*Filename);
	if (!BeforeHash.IsValid() || IFileManager::Get().Copy(*BackupPath, *Filename, false) != COPY_OK ||
		FMD5Hash::HashFile(*BackupPath) != BeforeHash)
	{
		Error = TEXT("Certification library backup failed verification; nothing published");
		return false;
	}
	TArray<FDublinFractureRecord> Certified = Library->Records;
	TSet<FString> Seen;
	for (FDublinFractureRecord& Record : Certified)
	{
		const FDublinCityBuilding* Building = Data.Buildings.FindByPredicate(
			[&Record](const FDublinCityBuilding& B) { return B.Id == Record.SourceId; });
		if (!Building || Seen.Contains(Record.SourceId))
		{
			Error = TEXT("Certification rejected a missing/duplicate source record");
			return false;
		}
		Seen.Add(Record.SourceId);
		const UGeometryCollection* Asset = Record.Collection.LoadSynchronous();
		if (!Asset || !CertifyCollisionData(*Asset, *Building, Record, Error))
		{
			Error = Record.SourceId + TEXT(": ") + (Asset ? Error : TEXT("saved asset unavailable"));
			return false;
		}
		UE_LOG(LogDublinCollisionCertification, Display, TEXT("Certified %d/%d source=%s digest=%s"),
			Seen.Num(), Certified.Num(), *Record.SourceId, *Record.CollisionValidationDigest);
		// No GC asset is edited or saved. Release loaded collections between bounded batches.
		if (Seen.Num() % 32 == 0) { CollectGarbage(RF_NoFlags); }
	}
	if (FMD5Hash::HashFile(*Filename) != BeforeHash)
	{
		Error = TEXT("Library changed during certification; refusing publication");
		return false;
	}
	const TArray<FDublinFractureRecord> Previous = Library->Records;
	const bool bWasDirty = Library->GetOutermost()->IsDirty();
	Library->Records = MoveTemp(Certified);
	// SavePackage handles the loaded-package file handles and replacement. A manual move
	// over a still-loaded asset fails on Windows; use the established library save path.
	if (!SaveLibrary(*Library, Error) || !FMD5Hash::HashFile(*Filename).IsValid())
	{
		Library->Records = Previous;
		Library->GetOutermost()->SetDirtyFlag(bWasDirty);
		ResetLoaders(Library->GetOutermost());
		if (FMD5Hash::HashFile(*Filename) != BeforeHash &&
			(IFileManager::Get().Copy(*Filename, *BackupPath, true, false) != COPY_OK ||
				FMD5Hash::HashFile(*Filename) != BeforeHash))
		{
			Error = TEXT("Certification publication failed AND original library restoration failed; restore verified backup: ") + BackupPath;
			return false;
		}
		Error = TEXT("Certification publication failed; previous library preserved. Backup: ") + BackupPath;
		return false;
	}
	Library->GetOutermost()->SetDirtyFlag(false);
	UE_LOG(LogDublinCollisionCertification, Display, TEXT("Published %d version-2 BLAKE3 collision certificates; schema=%d; verified backup=%s"),
		Library->Records.Num(), Library->BakeVersion, *BackupPath);
	Error.Reset();
	return true;
}

namespace
{
	FAutoConsoleCommand CertifyCommand(
		TEXT("DublinFlight.Bake.CertifyCurrentLibrary"),
		TEXT("Explicit authoring: fully validate every saved collision asset, back up and atomically replace only library certificates. Never invoked by automation test enumeration."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FString Backup, Error;
			if (!DublinFractureBake::CertifyCurrentLibrary(Backup, Error))
			{
				UE_LOG(LogDublinCollisionCertification, Error, TEXT("CERTIFICATION FAILED: %s backup=%s"), *Error, *Backup);
			}
			else
			{
				UE_LOG(LogDublinCollisionCertification, Display, TEXT("CERTIFICATION SUCCEEDED backup=%s"), *Backup);
			}
		}));
}
#endif
