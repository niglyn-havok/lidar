#include "City/DublinCityData.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using FValues = TArray<TSharedPtr<FJsonValue>>;

	struct FCityReader
	{
		FString& Error;
		int64 Vertices = 0;
		int64 Triangles = 0;

		bool Fail(const FString& Field, const FString& Reason)
		{
			Error = Field + TEXT(": ") + Reason;
			return false;
		}

		bool Array(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const FValues*& Out,
			const FString& Context)
		{
			return Object->TryGetArrayField(Key, Out) || Fail(Context + TEXT(".") + Key, TEXT("expected array"));
		}

		bool Number(const TSharedPtr<FJsonValue>& Value, double& Out, const FString& Context,
			double Min, double Max)
		{
			if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Out) ||
				!FMath::IsFinite(Out) || Out < Min || Out > Max)
			{
				return Fail(Context, TEXT("expected finite number within allowed bounds"));
			}
			return true;
		}

		bool Integer(const TSharedPtr<FJsonValue>& Value, int32& Out, const FString& Context, int32 Max)
		{
			double NumberValue = 0;
			if (!Number(Value, NumberValue, Context, 0, Max))
			{
				return false;
			}
			Out = static_cast<int32>(NumberValue);
			return NumberValue == Out || Fail(Context, TEXT("expected integer"));
		}

		bool String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& Out,
			const FString& Context, bool AllowEmpty = false)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Key);
			return (Value.IsValid() && Value->Type == EJson::String && Value->TryGetString(Out) && Out.Len() <= 4096 &&
				(AllowEmpty || !Out.IsEmpty())) || Fail(Context + TEXT(".") + Key, TEXT("expected bounded string"));
		}

		bool Tuple(const TSharedPtr<FJsonValue>& Value, int32 Count, double* Out,
			const FString& Context, double Min, double Max)
		{
			const FValues* Values = nullptr;
			if (!Value.IsValid() || !Value->TryGetArray(Values) || Values->Num() != Count)
			{
				return Fail(Context, FString::Printf(TEXT("expected %d-number tuple"), Count));
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Number((*Values)[Index], Out[Index], Context, Min, Max))
				{
					return false;
				}
			}
			return true;
		}

		bool Mesh(const TSharedPtr<FJsonObject>& Object, FDublinCityMesh& Out,
			const FString& Context, const FVector& Pivot, bool AllowEmpty)
		{
			const FValues* Positions = nullptr;
			const FValues* Indices = nullptr;
			const FValues* UVs = nullptr;
			if (!Array(Object, TEXT("verticesCm"), Positions, Context) ||
				!Array(Object, TEXT("triangles"), Indices, Context) || !Array(Object, TEXT("uv"), UVs, Context))
			{
				return false;
			}
			Vertices += Positions->Num();
			Triangles += Indices->Num() / 3;
			if (Vertices > DublinCity::MaxSourceVertices || Triangles > DublinCity::MaxSourceTriangles ||
				Indices->Num() % 3 != 0 || UVs->Num() != Positions->Num() ||
				(Positions->IsEmpty() != Indices->IsEmpty()) || (!AllowEmpty && Indices->IsEmpty()))
			{
				return Fail(Context, TEXT("invalid mesh counts, UV count or global geometry budget exceeded"));
			}
			Out.VerticesCm.Reserve(Positions->Num());
			Out.UV.Reserve(UVs->Num());
			for (int32 Index = 0; Index < Positions->Num(); ++Index)
			{
				double XYZ[3];
				double UV[2];
				if (!Tuple((*Positions)[Index], 3, XYZ, Context + TEXT(".verticesCm"), -200000, 200000) ||
					!Tuple((*UVs)[Index], 2, UV, Context + TEXT(".uv"), -10000, 10000))
				{
					return false;
				}
				const FVector Position(XYZ[0], XYZ[1], XYZ[2]);
				const FVector World = Position + Pivot;
				// Boundary-crossing footprints may extend one chunk beyond the core crop.
				constexpr double XYLimit = DublinCity::ExtentMeters * 50 + DublinCity::ChunkSizeCm;
				if (FMath::Abs(World.X) > XYLimit || FMath::Abs(World.Y) > XYLimit ||
					World.Z < -10000 || World.Z > 200000)
				{
					return Fail(Context, TEXT("vertex outside bounded Dublin survey envelope"));
				}
				Out.VerticesCm.Add(Position);
				Out.UV.Emplace(UV[0], UV[1]);
			}
			Out.Triangles.Reserve(Indices->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Indices)
			{
				int32 Index;
				if (!Integer(Value, Index, Context + TEXT(".triangles"), Positions->Num() - 1))
				{
					return false;
				}
				Out.Triangles.Add(Index);
			}
			for (int32 Index = 0; Index < Out.Triangles.Num(); Index += 3)
			{
				const FVector& A = Out.VerticesCm[Out.Triangles[Index]];
				const FVector& B = Out.VerticesCm[Out.Triangles[Index + 1]];
				const FVector& C = Out.VerticesCm[Out.Triangles[Index + 2]];
				if (FVector::CrossProduct(B - A, C - A).SizeSquared() <= 1.e-12)
				{
					return Fail(Context, TEXT("degenerate triangle"));
				}
			}
			if (Object->HasField(TEXT("colorsRGBA")))
			{
				const FValues* Colors = nullptr;
				if (!Array(Object, TEXT("colorsRGBA"), Colors, Context) || Colors->Num() != Positions->Num())
				{
					return Fail(Context + TEXT(".colorsRGBA"), TEXT("expected one RGBA byte tuple per vertex"));
				}
				Out.ColorsRGBA.Reserve(Colors->Num());
				for (const TSharedPtr<FJsonValue>& Value : *Colors)
				{
					double RGBA[4];
					if (!Tuple(Value, 4, RGBA, Context + TEXT(".colorsRGBA"), 0, 255))
					{
						return false;
					}
					for (double Channel : RGBA)
					{
						if (Channel != static_cast<int32>(Channel))
						{
							return Fail(Context + TEXT(".colorsRGBA"), TEXT("channels must be integer bytes"));
						}
					}
					Out.ColorsRGBA.Emplace(static_cast<uint8>(RGBA[0]), static_cast<uint8>(RGBA[1]),
						static_cast<uint8>(RGBA[2]), static_cast<uint8>(RGBA[3]));
				}
			}
			return true;
		}
	};

	bool CheckJsonNesting(const FString& Json, FString& Error)
	{
		int32 Depth = 0;
		bool InString = false;
		bool Escaped = false;
		for (TCHAR Character : Json)
		{
			if (Character == 0)
			{
				Error = TEXT("JSON contains embedded NUL");
				return false;
			}
			if (InString)
			{
				if (Escaped) { Escaped = false; }
				else if (Character == TEXT('\\')) { Escaped = true; }
				else if (Character == TEXT('"')) { InString = false; }
			}
			else if (Character == TEXT('"')) { InString = true; }
			else if (Character == TEXT('[') || Character == TEXT('{'))
			{
				if (++Depth > 32)
				{
					Error = TEXT("JSON nesting exceeds 32 levels");
					return false;
				}
			}
			else if (Character == TEXT(']') || Character == TEXT('}')) { --Depth; }
		}
		return true;
	}
}

FVector DublinCity::SurveyToCity(double East, double North, double HeightMeters)
{
	return FVector(100 * (East - 315989), -100 * (North - 234393), 100 * HeightMeters);
}

bool DublinCity::ResolveContentJsonPath(const FString& ContentDirectory, const FString& RelativePath,
	FString& OutPath, FString& OutError)
{
	OutPath.Reset();
	OutError.Reset();
	FString Relative = RelativePath;
	Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (Relative.IsEmpty() || Relative.Len() > 512 || !FPaths::IsRelative(Relative) ||
		Relative.StartsWith(TEXT("/")) || Relative.Contains(TEXT(":")) ||
		!FPaths::GetExtension(Relative).Equals(TEXT("json"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("SourceDataRelativePath must be a relative .json file beneath project Content");
		return false;
	}
	TArray<FString> Parts;
	Relative.ParseIntoArray(Parts, TEXT("/"), false);
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.EndsWith(TEXT(".")) ||
			Part.EndsWith(TEXT(" ")) || Part.Contains(TEXT("%")))
		{
			OutError = TEXT("SourceDataRelativePath contains traversal, empty or ambiguous path components");
			return false;
		}
		FString DeviceName;
		FString Unused;
		if (!Part.Split(TEXT("."), &DeviceName, &Unused)) { DeviceName = Part; }
		DeviceName.ToUpperInline();
		if (DeviceName == TEXT("CON") || DeviceName == TEXT("PRN") || DeviceName == TEXT("AUX") ||
			DeviceName == TEXT("NUL") || DeviceName == TEXT("CONIN$") || DeviceName == TEXT("CONOUT$") ||
			(DeviceName.Len() == 4 && (DeviceName.StartsWith(TEXT("COM")) || DeviceName.StartsWith(TEXT("LPT"))) &&
				DeviceName[3] >= TEXT('1') && DeviceName[3] <= TEXT('9')))
		{
			OutError = TEXT("SourceDataRelativePath contains a reserved device name");
			return false;
		}
		for (TCHAR Character : Part)
		{
			if (Character < 32 || Character == TEXT('*') || Character == TEXT('?') ||
				Character == TEXT('"') || Character == TEXT('<') || Character == TEXT('>') || Character == TEXT('|'))
			{
				OutError = TEXT("SourceDataRelativePath contains forbidden path characters");
				return false;
			}
		}
	}
	FString Root = FPaths::ConvertRelativePathToFull(ContentDirectory);
	FPaths::NormalizeDirectoryName(Root);
	Root += TEXT("/");
	OutPath = FPaths::ConvertRelativePathToFull(Root, Relative);
	FPaths::NormalizeFilename(OutPath);
	if (!OutPath.StartsWith(Root, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("SourceDataRelativePath escapes project Content");
		OutPath.Reset();
		return false;
	}
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FString Parent = Root;
	for (const FString& Part : Parts)
	{
		Parent = FPaths::Combine(Parent, Part);
		// UE's Windows IsSymlink checks FILE_ATTRIBUTE_REPARSE_POINT, including junctions.
		const ESymlinkResult LinkResult = PlatformFile.IsSymlink(*Parent);
		if (LinkResult != ESymlinkResult::NonSymlink)
		{
			OutError = LinkResult == ESymlinkResult::Symlink
				? TEXT("SourceDataRelativePath must not traverse symbolic links or reparse points")
				: TEXT("Cannot verify SourceDataRelativePath symlink/reparse safety on this platform");
			OutPath.Reset();
			return false;
		}
	}
	return true;
}

bool DublinCity::ParseCityJson(const FString& Json, FDublinCityData& OutData, FString& OutError)
{
	OutData = FDublinCityData();
	OutError.Reset();
	if (Json.IsEmpty() || Json.Len() > MaxFileBytes || !CheckJsonNesting(Json, OutError))
	{
		if (OutError.IsEmpty()) { OutError = TEXT("JSON is empty or exceeds 100 MiB"); }
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("Invalid city JSON: ") + Reader->GetErrorMessage();
		return false;
	}
	FCityReader Read{OutError};
	FDublinCityData Data;
	int32 Schema = 0;
	double Extent = 0;
	if (!Read.Integer(Root->TryGetField(TEXT("schemaVersion")), Schema, TEXT("schemaVersion"), 1) || Schema != 1)
	{
		return Read.Fail(TEXT("schemaVersion"), TEXT("only schemaVersion=1 is supported"));
	}
	if (!Read.Number(Root->TryGetField(TEXT("extentMeters")), Extent, TEXT("extentMeters"), ExtentMeters, ExtentMeters))
	{
		return false;
	}
	Data.ExtentMeters = Extent;
	double Origin[2];
	if (!Read.Tuple(Root->TryGetField(TEXT("originEN")), 2, Origin, TEXT("originEN"), 0, 1000000))
	{
		return false;
	}
	if (Origin[0] != 315989 || Origin[1] != 234393)
	{
		return Read.Fail(TEXT("originEN"), TEXT("expected legacy Irish Grid origin [315989,234393]"));
	}
	Data.OriginEN = FVector2D(Origin[0], Origin[1]);
	const FValues* Attribution = nullptr;
	if (!Read.Array(Root, TEXT("attribution"), Attribution, TEXT("city")) ||
		Attribution->IsEmpty() || Attribution->Num() > 128)
	{
		return Read.Fail(TEXT("attribution"), TEXT("expected 1..128 source attribution strings"));
	}
	for (const TSharedPtr<FJsonValue>& Value : *Attribution)
	{
		FString Text;
		if (Value->Type != EJson::String || !Value->TryGetString(Text) || Text.IsEmpty() || Text.Len() > 8192)
		{
			return Read.Fail(TEXT("attribution"), TEXT("invalid source attribution string"));
		}
		Data.Attribution.Add(MoveTemp(Text));
	}
	for (int32 Kind = 0; Kind < 2; ++Kind)
	{
		const TCHAR* Key = Kind == 0 ? TEXT("terrain") : TEXT("water");
		const TSharedPtr<FJsonObject>* MeshObject = nullptr;
		if (!Root->TryGetObjectField(Key, MeshObject) ||
			!Read.Mesh(*MeshObject, Kind == 0 ? Data.Terrain : Data.Water, Key, FVector::ZeroVector, Kind == 1))
		{
			if (OutError.IsEmpty()) { Read.Fail(Key, TEXT("expected mesh object")); }
			return false;
		}
	}
	const FValues* Buildings = nullptr;
	if (!Read.Array(Root, TEXT("buildings"), Buildings, TEXT("city")) || Buildings->Num() > MaxBuildings)
	{
		return Read.Fail(TEXT("buildings"), TEXT("expected array with at most 5000 buildings"));
	}
	TSet<FString> Ids;
	Data.Buildings.Reserve(Buildings->Num());
	for (int32 Index = 0; Index < Buildings->Num(); ++Index)
	{
		const FString Context = FString::Printf(TEXT("buildings[%d]"), Index);
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!(*Buildings)[Index]->TryGetObject(ObjectPtr) || !ObjectPtr->IsValid())
		{
			return Read.Fail(Context, TEXT("expected building object"));
		}
		const TSharedPtr<FJsonObject>& Object = *ObjectPtr;
		FDublinCityBuilding Building;
		if (!Read.String(Object, TEXT("id"), Building.Id, Context) ||
			!Read.String(Object, TEXT("name"), Building.Name, Context, true) ||
			!Read.String(Object, TEXT("heightMethod"), Building.HeightMethod, Context))
		{
			return false;
		}
		if (!Read.Number(Object->TryGetField(TEXT("confidence")), Building.Confidence,
			Context + TEXT(".confidence"), 0, 1)) { return false; }
		if (Ids.Contains(Building.Id)) { return Read.Fail(Context, TEXT("duplicate source building id")); }
		Ids.Add(Building.Id);
		double Pivot[3];
		if (!Read.Tuple(Object->TryGetField(TEXT("pivotCm")), 3, Pivot, Context + TEXT(".pivotCm"), -200000, 200000))
		{
			return false;
		}
		Building.PivotCm = FVector(Pivot[0], Pivot[1], Pivot[2]);
		if (FMath::Abs(Pivot[0]) > ExtentMeters * 50 || FMath::Abs(Pivot[1]) > ExtentMeters * 50 ||
			Pivot[2] < -10000 || Pivot[2] > 200000)
		{
			return Read.Fail(Context, TEXT("building pivot outside city core"));
		}
		if (!Read.Mesh(Object, Building.Mesh, Context, Building.PivotCm, false)) { return false; }
		const FValues* Materials = nullptr;
		if (!Read.Array(Object, TEXT("materialIds"), Materials, Context) ||
			Materials->Num() != Building.Mesh.Triangles.Num() / 3)
		{
			return Read.Fail(Context + TEXT(".materialIds"), TEXT("expected one material ID per triangle"));
		}
		Building.MaterialIds.Reserve(Materials->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Materials)
		{
			int32 Material;
			if (!Read.Integer(Value, Material, Context + TEXT(".materialIds"), 2)) { return false; }
			Building.MaterialIds.Add(static_cast<uint8>(Material));
		}
		Data.Buildings.Add(MoveTemp(Building));
	}
	OutData = MoveTemp(Data);
	return true;
}

bool DublinCity::LoadCityJson(const FString& RelativePath, FDublinCityData& OutData, FString& OutError)
{
	OutData = FDublinCityData();
	FString Path;
	if (!ResolveContentJsonPath(FPaths::ProjectContentDir(), RelativePath, Path, OutError)) { return false; }
	TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path));
	if (!File)
	{
		OutError = TEXT("Cannot open city data: ") + RelativePath;
		return false;
	}
	const int64 Size = File->TotalSize();
	if (Size <= 0 || Size > MaxFileBytes)
	{
		OutError = TEXT("City JSON must be nonempty and at most 100 MiB");
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	File->Serialize(Bytes.GetData(), Size);
	if (File->IsError())
	{
		OutError = TEXT("Failed reading bounded city JSON");
		return false;
	}
	FString Json;
	FFileHelper::BufferToString(Json, Bytes.GetData(), Bytes.Num());
	Bytes.Empty();
	return ParseCityJson(Json, OutData, OutError);
}
