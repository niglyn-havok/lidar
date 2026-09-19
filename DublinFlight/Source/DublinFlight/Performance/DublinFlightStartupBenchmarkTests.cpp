#include "DublinFlightPerformanceSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DublinFlightBenchmark.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace DublinFlight::Performance;

namespace
{
	FString StartupTestDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("Automation"), TEXT("StartupBenchmark-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	FString StartupCommand(const FString& Directory, const FString& RunId, const TCHAR* Scenario = TEXT("normal"))
	{
		return FString::Printf(TEXT("-DublinBenchmark=%s -DublinBenchmarkRunId=%s -DublinBenchmarkOutput=\"%s\""),
			Scenario, *RunId, *Directory);
	}

	TSharedPtr<FJsonObject> ReadStartupJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Result;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStartupParserTest,
	"DublinFlight.Performance.Startup.Parser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStartupParserTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::Combine(StartupTestDirectory(), TEXT("directory with spaces"));
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Command = StartupCommand(Directory, RunId);
	FStartupBenchmarkRequest Request;
	FString Error;
	TestTrue(TEXT("Ordinary engine switches leave gameplay alone"),
		ParseStartupBenchmarkRequest(TEXT("-fullscreen -ResX=1920 -log"), Request, Error));
	TestFalse(TEXT("No custom options means no automatic capture or exit"), Request.bRequested);
	TestFalse(TEXT("An orphan identity flag is an invalid attempted request"),
		ParseStartupBenchmarkRequest(TEXT("-DublinBenchmarkRunId=") + RunId, Request, Error));
	TestTrue(TEXT("Identity-only attempts cannot be silently ignored"), Request.bRequested);
	if (!TestTrue(TEXT("Quoted absolute directory and mandatory identity parse"),
		ParseStartupBenchmarkRequest(Command + TEXT(" -fullscreen -Res=1920x1080f"), Request, Error))) { return false; }
	TestTrue(TEXT("Exact mode selector opts in"), Request.bRequested);
	TestEqual(TEXT("Caller run ID is canonical lowercase"), Request.RunId, RunId.ToLower());
	TestEqual(TEXT("Quotes are removed without losing directory spaces"), Request.OutputDirectory, Directory);
	TestEqual(TEXT("Duration default"), Request.Arguments[1], FString(TEXT("30")));
	TestEqual(TEXT("Warmup default"), Request.Arguments[2], FString(TEXT("5")));
	FOptions Options;
	EBenchmark Kind;
	TestTrue(TEXT("CLI arguments use the real native workload parser"),
		ParseBenchmarkArguments(Request.Arguments, Kind, Options, Error));
	TestEqual(TEXT("Normal retains native 60 FPS target"), Options.ScenarioTargetMinimumFPS, 60);
	TestTrue(TEXT("Maximal and native inclusive numeric bounds"),
		ParseStartupBenchmarkRequest(StartupCommand(Directory, RunId, TEXT("maximal"))
			+ TEXT(" -DublinBenchmarkDuration=300 -DublinBenchmarkWarmup=60"), Request, Error));
	TestTrue(TEXT("Maximal options remain native"), ParseBenchmarkArguments(Request.Arguments, Kind, Options, Error));
	TestEqual(TEXT("Maximal retains native 30 FPS target"), Options.ScenarioTargetMinimumFPS, 30);
	TestTrue(TEXT("Minimum duration and zero warmup remain allowed"),
		ParseStartupBenchmarkRequest(Command + TEXT(" -DublinBenchmarkDuration=1 -DublinBenchmarkWarmup=0"), Request, Error));
	for (const TCHAR* Suffix : {
		TEXT(" -DublinBenchmarkCohortId=anything"), TEXT(" -DublinBenchmarkUnknown=1"),
		TEXT(" -DublinBenchmark=light"), TEXT(" -DublinBenchmarkRunId=duplicate"),
		TEXT(" -DublinBenchmarkOutput=relative"), TEXT(" -DublinBenchmarkDuration"),
		TEXT(" -DublinBenchmarkWarmup="), TEXT(" -DublinBenchmarkDuration=0"),
		TEXT(" -DublinBenchmarkDuration=301"), TEXT(" -DublinBenchmarkDuration=nan"),
		TEXT(" -DublinBenchmarkDuration=1e2"), TEXT(" -DublinBenchmarkDuration=+1"),
		TEXT(" -DublinBenchmarkWarmup=-1"), TEXT(" -DublinBenchmarkWarmup=61"),
		TEXT(" -DublinBenchmarkDuration=1 -DublinBenchmarkDuration=2"),
		TEXT(" -DublinBenchmarkWarmup=1 -DublinBenchmarkWarmup=2") })
	{
		TestFalse(FString(TEXT("Invalid request rejected: ")) + Suffix,
			ParseStartupBenchmarkRequest(Command + Suffix, Request, Error));
		TestTrue(TEXT("Parser supplies an explicit error"), !Error.IsEmpty());
	}
	for (const TCHAR* InvalidId : { TEXT(""), TEXT("00000000000000000000000000000000"),
		TEXT("invalid"), TEXT("fffffffffffffffffffffffffffffffg"), TEXT("12345678-1234-1234-1234-123456789abc") })
	{
		TestFalse(TEXT("Missing, nil or malformed GUID rejected"),
			ParseStartupBenchmarkRequest(StartupCommand(Directory, InvalidId), Request, Error));
	}
	TestFalse(TEXT("Scenario is not arbitrary execution"),
		ParseStartupBenchmarkRequest(StartupCommand(Directory, RunId, TEXT("exec")), Request, Error));
	TestFalse(TEXT("Relative output rejected"), ParseStartupBenchmarkRequest(StartupCommand(TEXT("relative"), RunId), Request, Error));
	TestFalse(TEXT("Unknown prefixed switches are explicitly rejected even without activation"),
		ParseStartupBenchmarkRequest(TEXT("-DublinBenchmarkTypo=normal"), Request, Error));
	TestTrue(TEXT("Unknown custom switch alone is still an attempted request"), Request.bRequested);
	TestFalse(TEXT("Unmatched output quotes rejected"),
		ParseStartupBenchmarkRequest(Command.LeftChop(1), Request, Error));
	TestFalse(TEXT("Even identical duplicate run IDs are ambiguous"),
		ParseStartupBenchmarkRequest(Command + TEXT(" -DublinBenchmarkRunId=") + RunId, Request, Error));
	TestTrue(TEXT("Duplicate identity is explicitly diagnosed"), Error.Contains(TEXT("Duplicate")));
	TestFalse(TEXT("Case differences cannot hide duplicate custom options"),
		ParseStartupBenchmarkRequest(Command + TEXT(" -DUBLINBENCHMARK=normal"), Request, Error));
	TestFalse(TEXT("Even identical output directories are ambiguous"),
		ParseStartupBenchmarkRequest(Command + FString::Printf(TEXT(" -DublinBenchmarkOutput=\"%s\""), *Directory),
			Request, Error));
	TestTrue(TEXT("Ambiguous output directory is not available for error publication"), Request.OutputDirectory.IsEmpty());
	for (const TCHAR* InputId : { TEXT("0123456789ABCDEF0123456789ABCDEF"), TEXT("0123456789abcdef0123456789abcdef") })
	{
		TestTrue(TEXT("Either input GUID case is accepted"),
			ParseStartupBenchmarkRequest(StartupCommand(Directory, InputId), Request, Error));
		TestEqual(TEXT("Both input cases have one canonical identity"), Request.RunId,
			FString(TEXT("0123456789abcdef0123456789abcdef")));
	}
	for (const TCHAR* Orphan : { TEXT("-DublinBenchmarkDuration=1"), TEXT("-DUBLINBENCHMARKWARMUP=0"),
		TEXT("-DublinBenchmarkOutput=relative") })
	{
		TestFalse(TEXT("Helper flags without mandatory request fields fail explicitly"),
			ParseStartupBenchmarkRequest(Orphan, Request, Error));
		TestTrue(TEXT("Every custom prefix marks an attempted request"), Request.bRequested);
		TestTrue(TEXT("Incomplete request has an explicit error"), !Error.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStartupWorldLifecycleTest,
	"DublinFlight.Performance.Startup.WorldLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStartupWorldLifecycleTest::RunTest(const FString& Parameters)
{
	bool ProcessClaimed = false;
	TestFalse(TEXT("A transient cooked Game world before BeginPlay cannot consume the request"),
		TryClaimStartupBenchmarkRequest(true, false, false, ProcessClaimed));
	TestFalse(TEXT("Pre-BeginPlay inspection leaves the process request unclaimed"), ProcessClaimed);
	TestFalse(TEXT("Transient-world teardown cannot consume the request or authorize output/exit"),
		TryClaimStartupBenchmarkRequest(true, false, true, ProcessClaimed));
	TestFalse(TEXT("The real world can still claim after transient-world teardown"), ProcessClaimed);
	TestFalse(TEXT("Excluded world/process contexts cannot claim even after BeginPlay"),
		TryClaimStartupBenchmarkRequest(false, true, false, ProcessClaimed));
	TestFalse(TEXT("A begun world already tearing down cannot claim"),
		TryClaimStartupBenchmarkRequest(true, true, true, ProcessClaimed));
	TestTrue(TEXT("Actual eligible gameplay consumes the request only after BeginPlay"),
		TryClaimStartupBenchmarkRequest(true, true, false, ProcessClaimed));
	TestTrue(TEXT("The process-wide request is now consumed"), ProcessClaimed);
	TestFalse(TEXT("A subsequent world cannot run the same startup request again"),
		TryClaimStartupBenchmarkRequest(true, true, false, ProcessClaimed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStartupDirectoryTest,
	"DublinFlight.Performance.Startup.DirectoryOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStartupDirectoryTest::RunTest(const FString& Parameters)
{
	const FString Root = StartupTestDirectory();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString Directory = FPaths::Combine(Root, TEXT("new"));
	FString Prepared;
	FString Error;
	if (!TestTrue(TEXT("A nonexistent absolute directory can be exclusively claimed"),
		PrepareStartupBenchmarkDirectory(Directory, Prepared, Error))) { return false; }
	const FString Owner = FPaths::Combine(Prepared, TEXT("owner.json"));
	TArray<uint8> Before;
	TArray<uint8> After;
	TestTrue(TEXT("Durable claim exists before the capture"), FFileHelper::LoadFileToArray(Before, *Owner));
	TestFalse(TEXT("A running or failed run directory cannot be reused"),
		PrepareStartupBenchmarkDirectory(Directory, Prepared, Error));
	TestTrue(TEXT("Original ownership bytes remain readable"), FFileHelper::LoadFileToArray(After, *Owner));
	TestTrue(TEXT("Replay never modifies old data"), Before == After);
	TestFalse(TEXT("No premature terminal result"),
		IFileManager::Get().FileExists(*FPaths::Combine(Directory, TEXT("result.json"))));
	TestFalse(TEXT("Relative directories fail"), PrepareStartupBenchmarkDirectory(TEXT("relative"), Prepared, Error));
	TestFalse(TEXT("Traversal fails"), PrepareStartupBenchmarkDirectory(
		FPaths::Combine(Root, TEXT(".."), TEXT("outside")), Prepared, Error));
	TestFalse(TEXT("A regular file cannot become an output directory"),
		PrepareStartupBenchmarkDirectory(Owner, Prepared, Error));
	const FString Empty = FPaths::Combine(Root, TEXT("empty"));
	TestTrue(TEXT("Create an existing empty fixture directory"), IFileManager::Get().MakeDirectory(*Empty, true));
	TestTrue(TEXT("An existing empty directory is allowed"), PrepareStartupBenchmarkDirectory(Empty, Prepared, Error));
	TestTrue(TEXT("Native report basename allowed"), IsSafeBenchmarkReportName(TEXT("normal-20260916T120000Z-123-manifest.json")));
	for (const TCHAR* Bad : { TEXT("../profile.json"), TEXT("C:/profile.json"), TEXT("nested\\profile.json"), TEXT("nested/profile.json"),
		TEXT("/profile.json"), TEXT("CON.json"), TEXT("profile.json.exe"), TEXT("x:profile.json"), TEXT("profile.tmp") })
	{
		TestFalse(FString(TEXT("Unsafe report reference rejected: ")) + Bad, IsSafeBenchmarkReportName(Bad));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStartupDecisionTest,
	"DublinFlight.Performance.Startup.PublicationDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStartupDecisionTest::RunTest(const FString& Parameters)
{
	for (int32 Mask = 0; Mask < 8; ++Mask)
	{
		const bool Started = (Mask & 1) != 0;
		const bool Published = (Mask & 2) != 0;
		const bool Passed = (Mask & 4) != 0;
		const uint8 Expected = !Started || !Published ? 2 : Passed ? 0 : 1;
		TestEqual(TEXT("Startup/publication errors cannot become native passes"),
			StartupBenchmarkOutcomeCode(Started, Published, Passed), Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinStartupTerminalTest,
	"DublinFlight.Performance.Startup.TerminalPublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinStartupTerminalTest::RunTest(const FString& Parameters)
{
	const FString Root = StartupTestDirectory();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	FStartupBenchmarkRequest Request;
	Request.bRequested = true;
	Request.Scenario = TEXT("normal");
	Request.RunId = TEXT("0123456789ABCDEF0123456789ABCDEF");
	FString Error;
	if (!TestTrue(TEXT("Claim result fixture directory"),
		PrepareStartupBenchmarkDirectory(FPaths::Combine(Root, TEXT("complete")), Request.OutputDirectory, Error))) { return false; }
	const FString CaptureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ResultPath = FPaths::Combine(Request.OutputDirectory, TEXT("result.json"));
	TestFalse(TEXT("Pass cannot precede profile/manifest publication"),
		PublishStartupBenchmarkResult(Request, CaptureId, TEXT("profile.json"), TEXT("manifest.json"), 0, FString(), {}, Error));
	TestFalse(TEXT("No success-shaped result exists"), IFileManager::Get().FileExists(*ResultPath));
	TestTrue(TEXT("Create profile fixture"),
		FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Request.OutputDirectory, TEXT("profile.json"))));
	TestTrue(TEXT("Create manifest fixture"),
		FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Request.OutputDirectory, TEXT("manifest.json"))));
	TestFalse(TEXT("Traversal cannot be published as a report reference"),
		PublishStartupBenchmarkResult(Request, CaptureId, TEXT("../profile.json"), TEXT("manifest.json"), 0, FString(), {}, Error));
	TestTrue(TEXT("Terminal publication follows both native files"),
		PublishStartupBenchmarkResult(Request, CaptureId, TEXT("profile.json"), TEXT("manifest.json"), 0, FString(), {}, Error));
	const TSharedPtr<FJsonObject> Result = ReadStartupJson(ResultPath);
	if (TestTrue(TEXT("Terminal JSON round-trips"), Result.IsValid()))
	{
		TestEqual(TEXT("Native process identity"), Result->GetNumberField(TEXT("processId")),
			static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
		TestEqual(TEXT("Actual build configuration"), Result->GetStringField(TEXT("configuration")),
			FString(LexToString(FApp::GetBuildConfiguration())));
		TestEqual(TEXT("Published caller identity is canonical lowercase"), Result->GetStringField(TEXT("runId")), Request.RunId.ToLower());
		TestEqual(TEXT("Capture identity"), Result->GetStringField(TEXT("captureId")), CaptureId);
		TestEqual(TEXT("Profile is a basename"), Result->GetStringField(TEXT("profileFile")), FString(TEXT("profile.json")));
		TestEqual(TEXT("Manifest is a basename"), Result->GetStringField(TEXT("manifestFile")), FString(TEXT("manifest.json")));
		TestEqual(TEXT("Success status"), Result->GetStringField(TEXT("status")), FString(TEXT("passed")));
		TestEqual(TEXT("Terminal schema version"), Result->GetNumberField(TEXT("schemaVersion")), 2.0);
		TestEqual(TEXT("Success semantic outcome"), Result->GetNumberField(TEXT("outcomeCode")), 0.0);
		TestEqual(TEXT("OS exit policy is separate from the outcome"), Result->GetStringField(TEXT("processExitPolicy")),
			FString(TEXT("graceful-zero")));
		TestFalse(TEXT("Terminal does not conflate semantics with a process exit code"), Result->HasField(TEXT("exitCode")));
		TestEqual(TEXT("Success has no errors"), Result->GetArrayField(TEXT("errors")).Num(), 0);
	}
	TArray<uint8> Before;
	TArray<uint8> After;
	FFileHelper::LoadFileToArray(Before, *ResultPath);
	TestFalse(TEXT("An existing terminal result is never replaced"),
		PublishStartupBenchmarkResult(Request, CaptureId, TEXT("profile.json"), TEXT("manifest.json"), 1,
			TEXT("benchmark_failed"), { TEXT("invalid workload") }, Error));
	FFileHelper::LoadFileToArray(After, *ResultPath);
	TestTrue(TEXT("Prior result bytes preserved"), Before == After);
	if (!TestTrue(TEXT("Claim startup-error fixture"),
		PrepareStartupBenchmarkDirectory(FPaths::Combine(Root, TEXT("error")), Request.OutputDirectory, Error))) { return false; }
	TestTrue(TEXT("Safe startup errors need not invent capture/report identities"),
		PublishStartupBenchmarkResult(Request, FString(), FString(), FString(), 2, TEXT("invalid_request"),
			{ TEXT("Duration is outside native bounds.") }, Error));
	const TSharedPtr<FJsonObject> Failure = ReadStartupJson(FPaths::Combine(Request.OutputDirectory, TEXT("result.json")));
	if (TestTrue(TEXT("Error result exists"), Failure.IsValid()))
	{
		TestEqual(TEXT("Error status"), Failure->GetStringField(TEXT("status")), FString(TEXT("error")));
		TestEqual(TEXT("Error semantic outcome"), Failure->GetNumberField(TEXT("outcomeCode")), 2.0);
		TestEqual(TEXT("Error also uses graceful-zero transport"), Failure->GetStringField(TEXT("processExitPolicy")),
			FString(TEXT("graceful-zero")));
		TestFalse(TEXT("Error has no obsolete exitCode field"), Failure->HasField(TEXT("exitCode")));
		for (const TCHAR* Field : { TEXT("captureId"), TEXT("profileFile"), TEXT("manifestFile") })
		{
			const TSharedPtr<FJsonValue> Value = Failure->TryGetField(Field);
			TestTrue(TEXT("Unavailable identity/reference remains null"), Value.IsValid() && Value->Type == EJson::Null);
		}
	}
	if (!TestTrue(TEXT("Claim native-failure fixture"),
		PrepareStartupBenchmarkDirectory(FPaths::Combine(Root, TEXT("failed")), Request.OutputDirectory, Error))) { return false; }
	TestTrue(TEXT("Create native-failure profile fixture"),
		FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Request.OutputDirectory, TEXT("profile.json"))));
	TestTrue(TEXT("Create native-failure manifest fixture"),
		FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Request.OutputDirectory, TEXT("manifest.json"))));
	TestTrue(TEXT("A foreground failure remains a semantic failure under graceful-zero transport"),
		PublishStartupBenchmarkResult(Request, CaptureId, TEXT("profile.json"), TEXT("manifest.json"), 1,
			TEXT("benchmark_failed"), { TEXT("viewportNotForegroundOrFocusTransition") }, Error));
	const TSharedPtr<FJsonObject> NativeFailure = ReadStartupJson(FPaths::Combine(Request.OutputDirectory, TEXT("result.json")));
	if (TestTrue(TEXT("Native failure result exists"), NativeFailure.IsValid()))
	{
		TestEqual(TEXT("Failed status is retained"), NativeFailure->GetStringField(TEXT("status")), FString(TEXT("failed")));
		TestEqual(TEXT("Failed outcome remains one"), NativeFailure->GetNumberField(TEXT("outcomeCode")), 1.0);
		TestEqual(TEXT("All semantic outcomes use the same stock process exit policy"),
			NativeFailure->GetStringField(TEXT("processExitPolicy")), FString(TEXT("graceful-zero")));
		TestFalse(TEXT("Native failure has no obsolete exitCode field"), NativeFailure->HasField(TEXT("exitCode")));
	}
	Request.OutputDirectory = FPaths::Combine(Root, TEXT("unclaimed"));
	TestTrue(TEXT("Create an unclaimed directory fixture"),
		IFileManager::Get().MakeDirectory(*Request.OutputDirectory, true));
	const FString Existing = FPaths::Combine(Request.OutputDirectory, TEXT("keep.txt"));
	TestTrue(TEXT("Create unrelated existing contents"), FFileHelper::SaveStringToFile(TEXT("preserve me"), *Existing));
	TestFalse(TEXT("Even an error result cannot write into an unclaimed nonempty directory"),
		PublishStartupBenchmarkResult(Request, FString(), FString(), FString(), 2, TEXT("invalid_request"),
			{ TEXT("Invalid request.") }, Error));
	TestFalse(TEXT("No result created outside the validated claim"),
		IFileManager::Get().FileExists(*FPaths::Combine(Request.OutputDirectory, TEXT("result.json"))));
	FString Preserved;
	TestTrue(TEXT("Unrelated contents remain readable"), FFileHelper::LoadFileToString(Preserved, *Existing));
	TestEqual(TEXT("Unrelated contents remain unchanged"), Preserved, FString(TEXT("preserve me")));
	return true;
}

#endif
