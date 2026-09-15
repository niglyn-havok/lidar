#include "City/DublinCityData.h"
#include "City/DublinCityGeometry.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "City/DublinCityWorld.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "KismetProceduralMeshLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	FString CityFixture()
	{
		return TEXT(R"JSON({
			"schemaVersion":1,"originEN":[315989,234393],"extentMeters":768,
			"attribution":["Fixture only; not a source dataset"],
			"terrain":{"verticesCm":[[-100,-100,0],[-100,100,0],[100,-100,0]],
				"triangles":[0,1,2],"uv":[[0,0],[0,1],[1,0]]},
			"water":{"verticesCm":[],"triangles":[],"uv":[]},
			"buildings":[{"id":"fixture/1","name":"Fixture","confidence":0.9,"heightMethod":"lidar",
				"pivotCm":[0,0,100],"verticesCm":[[0,0,0],[0,100,0],[100,0,0]],
				"triangles":[0,1,2],"uv":[[0,0],[0,1],[1,0]],"materialIds":[0]}]
		})JSON");
	}

	FDublinCityMesh GridFixture(int32 Side, double Spacing)
	{
		FDublinCityMesh Mesh;
		const double Half = (Side - 1) * Spacing / 2;
		for (int32 Y = 0; Y < Side; ++Y)
		{
			for (int32 X = 0; X < Side; ++X)
			{
				Mesh.VerticesCm.Emplace(X * Spacing - Half, Y * Spacing - Half, 25);
				Mesh.UV.Emplace(static_cast<double>(X) / (Side - 1), static_cast<double>(Y) / (Side - 1));
			}
		}
		for (int32 Y = 0; Y < Side - 1; ++Y)
		{
			for (int32 X = 0; X < Side - 1; ++X)
			{
				const int32 A = Y * Side + X, B = A + 1, C = A + Side, D = C + 1;
				Mesh.Triangles.Append({A, C, B, B, C, D});
			}
		}
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityParserTest, "DublinFlight.City.Parser.ShapeAndBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityParserTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	FString Error;
	const FString Valid = CityFixture();
	TestTrue(TEXT("Small typed fixture parses"), DublinCity::ParseCityJson(Valid, Data, Error));
	TestEqual(TEXT("Building records retained"), Data.Buildings.Num(), 1);
	TestEqual(TEXT("Empty water is valid"), Data.Water.Triangles.Num(), 0);
	if (Data.Buildings.Num() == 1)
	{
		TestEqual(TEXT("Source id"), Data.Buildings[0].Id, FString(TEXT("fixture/1")));
		TestEqual(TEXT("Confidence"), Data.Buildings[0].Confidence, 0.9);
	}
	const TArray<TPair<FString, FString>> Replacements = {
		{TEXT("\"schemaVersion\":1"), TEXT("\"schemaVersion\":2")},
		{TEXT("\"schemaVersion\":1"), TEXT("\"schemaVersion\":1.5")},
		{TEXT("[315989,234393]"), TEXT("[315990,234393]")},
		{TEXT("\"extentMeters\":768"), TEXT("\"extentMeters\":1024")},
		{TEXT("\"triangles\":[0,1,2]"), TEXT("\"triangles\":[0,1,3]")},
		{TEXT("\"triangles\":[0,1,2]"), TEXT("\"triangles\":[0,-1,2]")},
		{TEXT("\"triangles\":[0,1,2]"), TEXT("\"triangles\":[0,1.5,2]")},
		{TEXT("\"triangles\":[0,1,2]"), TEXT("\"triangles\":[0,1]")},
		{TEXT("\"triangles\":[0,1,2]"), TEXT("\"triangles\":[0,0,2]")},
		{TEXT("\"materialIds\":[0]"), TEXT("\"materialIds\":[3]")},
		{TEXT("\"materialIds\":[0]"), TEXT("\"materialIds\":[]")},
		{TEXT("\"materialIds\":[0]"), TEXT("\"materialIds\":[0.5]")},
		{TEXT("\"pivotCm\":[0,0,100]"), TEXT("\"pivotCm\":[0,0,1e999]")},
		{TEXT("\"pivotCm\":[0,0,100]"), TEXT("\"pivotCm\":[0,0,NaN]")},
		{TEXT("\"pivotCm\":[0,0,100]"), TEXT("\"pivotCm\":[0,0,\"100\"]")},
		{TEXT("\"pivotCm\":[0,0,100]"), TEXT("\"pivotCm\":[900000,0,100]")},
		{TEXT("\"pivotCm\":[0,0,100]"), TEXT("\"pivotCm\":[0,100]")},
		{TEXT("\"confidence\":0.9"), TEXT("\"confidence\":true")},
		{TEXT("\"confidence\":0.9"), TEXT("\"confidence\":1.1")},
		{TEXT("\"confidence\":0.9"), TEXT("\"confidence\":\"high\"")},
		{TEXT("\"uv\":[[0,0],[0,1],[1,0]]"), TEXT("\"uv\":[[0,0]]")},
		{TEXT("\"heightMethod\":\"lidar\""), TEXT("\"heightMethod\":null")},
		{TEXT("\"heightMethod\":\"lidar\""), TEXT("\"heightMethod\":123")},
		{TEXT("\"id\":\"fixture/1\""), TEXT("\"id\":1")},
		{TEXT("\"attribution\":[\"Fixture only; not a source dataset\"]"), TEXT("\"attribution\":[true]")},
		{TEXT("\"attribution\":[\"Fixture only; not a source dataset\"]"), TEXT("\"attribution\":[]")}
	};
	for (const TPair<FString, FString>& Replacement : Replacements)
	{
		const FString Invalid = Valid.Replace(*Replacement.Key, *Replacement.Value);
		TestFalse(*FString::Printf(TEXT("Reject %s"), *Replacement.Value), DublinCity::ParseCityJson(Invalid, Data, Error));
		TestFalse(TEXT("Readable failure reason"), Error.IsEmpty());
		TestEqual(TEXT("No stale partial city after failure"), Data.Buildings.Num(), 0);
	}
	TestFalse(TEXT("Bad JSON"), DublinCity::ParseCityJson(TEXT("{broken"), Data, Error));
	TestFalse(TEXT("Nonobject root"), DublinCity::ParseCityJson(TEXT("[]"), Data, Error));
	FString Nested;
	for (int32 Index = 0; Index < 40; ++Index) { Nested += TEXT("["); }
	TestFalse(TEXT("Bound nesting"), DublinCity::ParseCityJson(Nested, Data, Error));
	const FString WithColors = Valid.Replace(TEXT("\"materialIds\":[0]"),
		TEXT("\"materialIds\":[0],\"colorsRGBA\":[[255,1,2,255],[0,0,0,255],[255,255,255,255]]"));
	TestTrue(TEXT("RGBA byte tuples accepted"), DublinCity::ParseCityJson(WithColors, Data, Error));
	TestFalse(TEXT("RGBA channel over byte bound"),
		DublinCity::ParseCityJson(WithColors.Replace(TEXT("[255,1,2,255]"), TEXT("[256,1,2,255]")), Data, Error));
	TestFalse(TEXT("Fractional RGBA channels rejected"),
		DublinCity::ParseCityJson(WithColors.Replace(TEXT("[255,1,2,255]"), TEXT("[255,1.5,2,255]")), Data, Error));
	TestTrue(TEXT("Numeric confidence accepted without coercing unrelated strings"),
		DublinCity::ParseCityJson(Valid.Replace(TEXT("\"confidence\":0.9"), TEXT("\"confidence\":0.75")), Data, Error));
	const int32 BuildingStart = Valid.Find(TEXT("{\"id\":\"fixture/1\""));
	const int32 BuildingEnd = Valid.Find(TEXT("\"materialIds\":[0]}")) + FString(TEXT("\"materialIds\":[0]}")).Len();
	if (BuildingStart != INDEX_NONE && BuildingEnd > BuildingStart)
	{
		const FString Building = Valid.Mid(BuildingStart, BuildingEnd - BuildingStart);
		TestFalse(TEXT("Duplicate building source IDs rejected"),
			DublinCity::ParseCityJson(Valid.Replace(*Building, *(Building + TEXT(",") + Building)), Data, Error));
		TArray<FString> TooMany;
		TooMany.Init(Building, DublinCity::MaxBuildings + 1);
		TestFalse(TEXT("Building count bound enforced before records allocated"),
			DublinCity::ParseCityJson(Valid.Replace(*Building, *FString::Join(TooMany, TEXT(","))), Data, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityPathTest, "DublinFlight.City.Parser.SafeRelativePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityPathTest::RunTest(const FString& Parameters)
{
	FString Path, Error;
	const FString Root = FPaths::ProjectContentDir();
	TestTrue(TEXT("Default path"), DublinCity::ResolveContentJsonPath(Root, TEXT("Data/dublin-city.json"), Path, Error));
	TestTrue(TEXT("Windows relative separators"), DublinCity::ResolveContentJsonPath(Root, TEXT("Data\\dublin-city.json"), Path, Error));
	for (const FString& Invalid : TArray<FString>{TEXT("../outside.json"), TEXT("Data/../outside.json"),
		TEXT("Data\\..\\outside.json"), TEXT("/outside.json"), TEXT("C:\\outside.json"),
		TEXT("\\\\server\\share\\outside.json"), TEXT("Data/test.py"), TEXT("Data/file.json:stream"),
		TEXT("Data//file.json"), TEXT("Data/./file.json"), TEXT("Data/%2e%2e/file.json"),
		TEXT("Data /file.json"), TEXT("Data./file.json"), TEXT("Data/*.json"), TEXT("Data/NUL.json"),
		TEXT("COM1/file.json"), TEXT("")})
	{
		TestFalse(*Invalid, DublinCity::ResolveContentJsonPath(Root, Invalid, Path, Error));
		TestTrue(TEXT("Rejected path cleared"), Path.IsEmpty());
		TestFalse(TEXT("Path failure is readable"), Error.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityCoordinateTest, "DublinFlight.City.Coordinates.KnownSurveyAndClockwise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityCoordinateTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("O'Connell Bridge survey origin"),
		DublinCity::SurveyToCity(315989, 234393, 0), FVector::ZeroVector);
	TestEqual(TEXT("One meter east north up"),
		DublinCity::SurveyToCity(315990, 234394, 1), FVector(100, -100, 100));
	TestEqual(TEXT("Fixed midair spawn survey coordinates"),
		DublinCity::SurveyToCity(315709, 234313, 180), FVector(-28000, 8000, 18000));
	TestEqual(TEXT("Northwest orthophoto corner"), DublinCity::AerialUV(FVector(-38400, -38400, 0)), FVector2D(0, 0));
	TestEqual(TEXT("Southeast orthophoto corner"), DublinCity::AerialUV(FVector(38400, 38400, 0)), FVector2D(1, 1));
	const FVector A(0, 0, 0), B(0, 100, 0), C(100, 0, 0);
	TestEqual(TEXT("Tiny clockwise triangle faces sky"), DublinCity::ClockwiseNormal(A, B, C), FVector::UpVector);
	TestEqual(TEXT("Reverse faces down; never silently flipped"),
		DublinCity::ClockwiseNormal(A, C, B), -FVector::UpVector);
	TArray<FVector> EngineNormals;
	TArray<FProcMeshTangent> EngineTangents;
	UKismetProceduralMeshLibrary::CalculateTangentsForMesh({A, B, C}, {0, 1, 2},
		{FVector2D(0, 0), FVector2D(0, 1), FVector2D(1, 0)}, EngineNormals, EngineTangents);
	if (TestEqual(TEXT("Engine generated three triangle normals"), EngineNormals.Num(), 3))
	{
		TestEqual(TEXT("Tiny triangle agrees with installed UE winding convention"), EngineNormals[0], FVector::UpVector);
		TestEqual(TEXT("Native normal agrees with engine"), DublinCity::ClockwiseNormal(A, B, C), EngineNormals[0]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityChunkTest, "DublinFlight.City.Geometry.ChunkMappingAndMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityChunkTest::RunTest(const FString& Parameters)
{
	FDublinCityData Data;
	FString Error;
	if (!TestTrue(TEXT("Fixture parse"), DublinCity::ParseCityJson(CityFixture(), Data, Error))) { return false; }
	Data.Buildings[0].PivotCm = FVector(100, 100, 500);
	FDublinCityBuilding Near = Data.Buildings[0];
	Near.Id = TEXT("near");
	Near.PivotCm.X += 200;
	Near.MaterialIds[0] = 1;
	Data.Buildings.Add(Near);
	FDublinCityBuilding Far = Near;
	Far.Id = TEXT("far");
	Far.PivotCm.X += 6400;
	Far.MaterialIds[0] = 2;
	Data.Buildings.Add(Far);
	TArray<FDublinCityChunk> Chunks;
	if (!TestTrue(TEXT("Build spatial chunks"), DublinCity::MakeBuildingChunks(Data.Buildings, Chunks, Error))) { return false; }
	TestEqual(TEXT("Nearby grouped, 64m-separated split"), Chunks.Num(), 2);
	int32 Triangles = 0;
	for (const FDublinCityChunk& Chunk : Chunks)
	{
		TestEqual(TEXT("Three material sections per chunk"), Chunk.Sections.Num(), 3);
		for (int32 Material = 0; Material < Chunk.Sections.Num(); ++Material)
		{
			const FDublinCitySection& Section = Chunk.Sections[Material];
			Triangles += Section.Triangles.Num() / 3;
			for (int32 Triangle = 0; Triangle < Section.SourceBuildingPerTriangle.Num(); ++Triangle)
			{
				const FDublinCityBuilding& Building = Data.Buildings[Section.SourceBuildingPerTriangle[Triangle]];
				TestEqual(TEXT("Source triangle mapped"), Section.SourceTriangleIndices[Triangle], 0);
				TestEqual(TEXT("Source material maintained"), static_cast<int32>(Building.MaterialIds[0]), Material);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Local = Triangle * 3 + Corner;
					const int32 Source = Section.SourceVertexIndices[Local];
					TestEqual(TEXT("Pivot to chunk transform is lossless"), Section.Vertices[Local] + Chunk.OriginCm,
						Building.Mesh.VerticesCm[Source] + Building.PivotCm);
					TestEqual(TEXT("No index reversal"), Source, Building.Mesh.Triangles[Corner]);
					TestEqual(TEXT("UV1 retains source UV"), Section.UV1[Local], Building.Mesh.UV[Source]);
					TestEqual(TEXT("Roof uses aerial; walls/foundations retain generic UV"), Section.UV0[Local],
						Material == 0 ? DublinCity::AerialUV(Building.Mesh.VerticesCm[Source] + Building.PivotCm) : Building.Mesh.UV[Source]);
				}
			}
		}
	}
	TestEqual(TEXT("Exactly one render triangle per source triangle"), Triangles, 3);
	TestEqual(TEXT("Positive boundary clamped to last cell"), DublinCity::SpatialCell(FVector(38400, 38400, 0)), FIntPoint(11, 11));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityTerrainTest, "DublinFlight.City.Geometry.FixedTerrainTopology",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityTerrainTest::RunTest(const FString& Parameters)
{
	FDublinCityMesh Mesh = GridFixture(3, 200);
	TArray<FDublinCityChunk> Chunks;
	FString Error;
	if (!TestTrue(TEXT("Tiny fixed-grid fixture"), DublinCity::MakeTerrainChunks(Mesh, Chunks, Error, 3, 200))) { return false; }
	int32 TriangleCount = 0;
	for (FDublinCityChunk& Chunk : Chunks)
	{
		FDublinCitySection& Section = Chunk.Sections[0];
		TriangleCount += Section.Triangles.Num() / 3;
		for (int32 Index = 0; Index < Section.Vertices.Num(); ++Index)
		{
			const int32 Source = Section.SourceVertexIndices[Index];
			TestEqual(TEXT("Source node maps into chunk"), Section.Vertices[Index] + Chunk.OriginCm, Mesh.VerticesCm[Source]);
			TestEqual(TEXT("Seam normals are globally calculated"), Section.Normals[Index], FVector::UpVector);
		}
		Section.Vertices[0].Z -= 10;
	}
	TestEqual(TEXT("All source triangles retained"), TriangleCount, 8);
	TestEqual(TEXT("Mutable chunk does not change immutable source"), Mesh.VerticesCm[0].Z, 25.0);
	Swap(Mesh.Triangles[0], Mesh.Triangles[1]);
	TestFalse(TEXT("Wrong front face rejected, not silently flipped"), DublinCity::MakeTerrainChunks(Mesh, Chunks, Error, 3, 200));
	Mesh = GridFixture(3, 200);
	Mesh.VerticesCm[0].X += 10;
	TestFalse(TEXT("Off-grid node rejected"), DublinCity::MakeTerrainChunks(Mesh, Chunks, Error, 3, 200));
	Mesh = GridFixture(3, 200);
	Mesh.Triangles[3] = Mesh.Triangles[0];
	Mesh.Triangles[4] = Mesh.Triangles[1];
	Mesh.Triangles[5] = Mesh.Triangles[2];
	TestFalse(TEXT("Duplicate cell triangle rejected"), DublinCity::MakeTerrainChunks(Mesh, Chunks, Error, 3, 200));
	Mesh = GridFixture(3, 200);
	Mesh.Triangles[3] = 0;
	Mesh.Triangles[4] = 4;
	Mesh.Triangles[5] = 1;
	TestFalse(TEXT("Overlapping triangles sharing cell boundary rejected"),
		DublinCity::MakeTerrainChunks(Mesh, Chunks, Error, 3, 200));
	Mesh = GridFixture(3, 200);
	Mesh.Triangles.RemoveAt(0, 9);
	TestTrue(TEXT("River mask may omit a whole cell and one bank triangle"),
		DublinCity::MakeTerrainChunks(Mesh, Chunks, Error, 3, 200));
	int32 MaskedTriangles = 0;
	for (const FDublinCityChunk& Chunk : Chunks) { MaskedTriangles += Chunk.Sections[0].Triangles.Num() / 3; }
	TestEqual(TEXT("River mask not silently filled"), MaskedTriangles, 5);
	FDublinCitySection Water;
	TestTrue(TEXT("Empty water supported"), DublinCity::MakeWaterSection(FDublinCityMesh(), Water, Error));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinCityLifecycleTest, "DublinFlight.City.Lifecycle.ReclaimsUntrackedDuplicates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinCityLifecycleTest::RunTest(const FString& Parameters)
{
	UWorld* TestWorld = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Fixture world"), TestWorld)) { return false; }
	ON_SCOPE_EXIT { TestWorld->DestroyWorld(false); };
	ADublinCityWorld* City = TestWorld->SpawnActorDeferred<ADublinCityWorld>(
		ADublinCityWorld::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Deferred city fixture"), City)) { return false; }
	City->bAutoBuild = false;
	City->FinishSpawning(FTransform::Identity);
	const TArray<FDublinCitySection> EmptySections;
	const TArray<UMaterialInterface*> EmptyMaterials;
	UProceduralMeshComponent* Generated = City->CreateChunkComponent(
		TEXT("DublinTerrain_00_00"), FVector::ZeroVector, EmptySections, EmptyMaterials, false);
	if (!TestNotNull(TEXT("Generated fixture component"), Generated)) { return false; }
	UProceduralMeshComponent* Duplicated = DuplicateObject<UProceduralMeshComponent>(
		Generated, City, FName(TEXT("DublinTerrain_00_00_PIECopy")));
	if (!TestNotNull(TEXT("Duplicated generated component"), Duplicated)) { return false; }
	City->AddInstanceComponent(Duplicated);
	Duplicated->SetupAttachment(City->CityRoot);
	Duplicated->RegisterComponent();
	TestTrue(TEXT("Explicit generation marker survives UObject duplication"),
		City->IsMarkedGeneratedComponent(Duplicated));

	UProceduralMeshComponent* Unrelated = NewObject<UProceduralMeshComponent>(City, TEXT("DublinTerrain_Manual"));
	City->AddInstanceComponent(Unrelated);
	Unrelated->SetupAttachment(City->CityRoot);
	Unrelated->RegisterComponent();
	TestFalse(TEXT("Matching generated name is insufficient without marker"),
		City->IsMarkedGeneratedComponent(Unrelated));

	AActor* OtherActor = TestWorld->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Foreign owner fixture"), OtherActor)) { return false; }
	UProceduralMeshComponent* Foreign = NewObject<UProceduralMeshComponent>(OtherActor);
	OtherActor->AddInstanceComponent(Foreign);
	Foreign->RegisterComponent();

	// Reproduce lost transient tracking with both original and duplicated generated components still alive.
	City->GeneratedComponents.Reset();
	City->GeneratedComponents.Add(Foreign);
	City->ClearGeneratedCity();
	TInlineComponentArray<UActorComponent*> Remaining;
	City->GetComponents(Remaining);
	TestEqual(TEXT("Only root and unrelated component survive cleanup"), Remaining.Num(), 2);
	TestTrue(TEXT("Unrelated component survives"), Remaining.Contains(Unrelated));
	TestTrue(TEXT("Root survives"), Remaining.Contains(City->CityRoot.Get()));
	TestFalse(TEXT("Untracked original removed"), Remaining.Contains(Generated));
	TestFalse(TEXT("Untracked duplicate removed"), Remaining.Contains(Duplicated));
	TestTrue(TEXT("Foreign-owned tracked component is never destroyed"), IsValid(Foreign) && Foreign->IsRegistered());

	for (int32 Rebuild = 0; Rebuild < 2; ++Rebuild)
	{
		UProceduralMeshComponent* Rebuilt = City->CreateChunkComponent(
			TEXT("DublinWater"), FVector::ZeroVector, EmptySections, EmptyMaterials, false);
		if (!TestNotNull(TEXT("Rebuilt component"), Rebuilt)) { return false; }
		City->GeneratedComponents.Reset();
		City->ClearGeneratedCity();
		Remaining.Reset();
		City->GetComponents(Remaining);
		TestEqual(TEXT("Repeated rebuild cleanup never accumulates components"), Remaining.Num(), 2);
	}
	return true;
}
#endif
