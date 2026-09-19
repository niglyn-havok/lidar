#requires -Version 7.0
<#
.SYNOPSIS
Dependency-free synthetic report, command and executable-path checks; never launches Unreal.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\BenchmarkReport.ps1')
$directory = Join-Path ([IO.Path]::GetTempPath()) ('dublin-benchmark-fixtures-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($directory) | Out-Null
$processStarted = [DateTimeOffset]'2026-09-16T19:00:00Z'
$processFinished = $processStarted.AddMinutes(5)
$passed = 0

function Copy-Fixture {
    param($Value)
    return ConvertFrom-Json -InputObject (ConvertTo-Json -InputObject $Value -Depth 100) -AsHashtable
}

function New-Fixture {
    param([string]$Scenario = 'normal', [double]$Duration = 30)
    $step = if ($Scenario -eq 'maximal') { 0.025 } else { 0.01 }
    $frames = [double[]]::new([int]($Duration / $step))
    [Array]::Fill($frames, $step)
    $count = $frames.Count
    $state = @{
        worldName = 'Dublin'; worldType = 'Game'; isPIE = $false; isDedicatedServer = $false
        worldPaused = $false; effectiveTimeDilation = 1; buildConfiguration = 'Development'
        requiresCookedData = $true; isEditorProcess = $false; nullRHI = $false
        useFixedTimeStep = $false; engineFixedFrameRate = $false; engineShowFlags = 'fixture-rendering'
        viewport = @{ available = $true; dimensionsAvailable = $true; width = 1920; height = 1080
            hasFocus = $true; isForegroundWindow = $true; windowMode = 'Windowed' }
        qualityAndTimingCVars = @{ 'r.DynamicRes.OperationMode' = 0; 'r.ScreenPercentage' = 100; 'sg.ShadowQuality' = 3; 't.MaxFPS' = 0 }
    }
    $benchmark = @{
        scenario = $Scenario; completionReason = 'durationReached'; benchmarkValid = $true
        workloadAdmitted = $true; measurementStarted = $true; measuredWallSeconds = $Duration
        invalidReasons = @(); acceptedOperations = 0
        weaponSettingsAtStart = @{ bombYieldTonsTNT = 1; cannonRoundsPerSecond = 7 }
        weaponSettingsAtEnd = @{ bombYieldTonsTNT = 1; cannonRoundsPerSecond = 7 }
        measurementStartSettings = $state; measurementEndSettings = Copy-Fixture $state
    }
    $profile = @{
        schemaVersion = 1; engineVersion = '5.8.2'; captureId = '0123456789abcdef0123456789abcdef'
        label = $Scenario; status = 'completed'; completionReason = 'durationReached'
        completedRequestedDuration = $true; benchmarkValid = $true; workloadAdmitted = $true
        scenarioTargetMet = $true; benchmarkPassed = $true; frameTargetMet = $true
        metricsAvailable = $true; clockPrimed = $true; error = $null
        requestedDurationSeconds = $Duration; requestedWarmupSeconds = 5
        excludedWarmupSeconds = 5; excludedWarmupFrames = 500
        scenarioTargetMinimumFPS = $(if ($Scenario -eq 'maximal') { 30 } else { 60 })
        thresholdToleranceMilliseconds = 0.000001; frameBudgetMilliseconds = 1000.0 / 60
        frameBudget30Milliseconds = 1000.0 / 30
        startedUtc = $processStarted.AddSeconds(2).ToString('o')
        finishedUtc = $processStarted.AddSeconds($Duration + 7).ToString('o')
        benchmark = $benchmark; frameTimesSeconds = $frames; durationSeconds = $Duration
        frameCount = $count; minFPS = 1.0 / $step; averageFPS = 1.0 / $step
        worstFrameMilliseconds = $step * 1000
        framesOver60FPSBudget = $(if ($step -gt 1.0 / 60) { $count } else { 0 })
        framesOver30FPSBudget = 0; framesOver30FPSBudgetWithTolerance = 0
        framesOver60FPSBudgetWithTolerance = $(if ($step -gt 1.0 / 60) { $count } else { 0 })
        allFramesAtLeast60 = $step -le 1.0 / 60; allFramesAtLeast30 = $true
    }
    $manifest = Copy-Fixture $benchmark
    foreach ($field in @('captureId', 'status', 'completedRequestedDuration', 'scenarioTargetMet',
        'frameCount', 'minFPS', 'averageFPS', 'worstFrameMilliseconds', 'framesOver60FPSBudget', 'framesOver30FPSBudget')) {
        $manifest[$field] = $profile[$field]
    }
    $manifest['profileFile'] = "$Scenario-20260916T190037Z-639000000000000000.json"
    return @{ profile = $profile; manifest = $manifest; scenario = $Scenario; duration = $Duration }
}

function Invoke-Fixture {
    param([string]$Name, [scriptblock]$Mutate = {}, [string]$ExpectedError = '',
        [string]$Scenario = 'normal', [double]$Duration = 30, [string]$WindowMode = 'Windowed')
    $fixture = New-Fixture $Scenario $Duration
    & $Mutate $fixture.profile $fixture.manifest
    $profilePath = Join-Path $directory "$Scenario-20260916T190037Z-639000000000000000.json"
    $manifestPath = $profilePath -replace '\.json$', '-manifest.json'
    ConvertTo-Json -InputObject $fixture.profile -Depth 100 | Set-Content -LiteralPath $profilePath -Encoding utf8
    ConvertTo-Json -InputObject $fixture.manifest -Depth 100 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    $failure = $null
    try {
        $result = Test-BenchmarkReport $profilePath $manifestPath $Scenario $Duration 5 $processStarted $processFinished $WindowMode
        Assert-Benchmark $result.passed "$Name did not pass."
        Assert-Benchmark ($result.manifestVerdictSource -match 'Derived from native|Native benchmarkPassed') 'Manifest verdict provenance missing.'
    } catch { $failure = $_.Exception.Message }
    if ($ExpectedError) {
        Assert-Benchmark ($failure -and $failure -match $ExpectedError) "$Name`: expected '$ExpectedError', got '$failure'"
    } else { Assert-Benchmark (!$failure) "$Name`: $failure" }
    $script:passed++
    Write-Output "PASS $Name"
}

function Set-FixtureEnvironment {
    param($Profile, $Manifest, [scriptblock]$Change)
    foreach ($benchmark in @($Profile.benchmark, $Manifest)) {
        foreach ($field in @('measurementStartSettings', 'measurementEndSettings')) { & $Change $benchmark[$field] }
    }
}

function Set-SpectacleFixture {
    param($Profile, $Manifest)
    $count = 600
    $Profile.frameTimesSeconds = [double[]]::new($count)
    [Array]::Fill($Profile.frameTimesSeconds, 0.05)
    foreach ($document in @($Profile, $Manifest)) {
        $document.performancePolicy = 'maximal-spectacle-v1'
        $document.frameTargetRequired = $false
        $document.frameTargetMet = $false
        $document.scenarioTargetMet = $false
        $document.frameCount = $count
        $document.minFPS = 20
        $document.averageFPS = 20
        $document.worstFrameMilliseconds = 50
        $document.framesOver60FPSBudget = $count
        $document.framesOver30FPSBudget = $count
    }
    $Profile.framesOver60FPSBudgetWithTolerance = $count
    $Profile.framesOver30FPSBudgetWithTolerance = $count
    $Profile.allFramesAtLeast60 = $false
    $Profile.allFramesAtLeast30 = $false
}

function Invoke-NativeFixture {
    param([string]$Name, [scriptblock]$Mutate = {}, [string]$ExpectedError = '', [int]$ProcessExitCode = 0)
    $fixture = New-Fixture
    foreach ($document in @($fixture.profile.benchmark, $fixture.manifest)) {
        foreach ($field in @('skippedOperations', 'rejectedOperations', 'weaponRejectedImpacts', 'maxAwakeCollections',
            'maxAwakePieces', 'maxQueuedImpacts', 'maxActiveImpactFX', 'cityAcceptedImpacts', 'weaponAcceptedImpacts',
            'cannonShots', 'bombsDropped', 'effectsRequests')) { $document[$field] = 0 }
        $document['flightDistanceCm'] = 120000
    }
    $identity = @{ runId = 'abcdefabcdefabcdefabcdefabcdefab'; processId = 43210; configuration = 'Shipping' }
    foreach ($document in @($fixture.profile, $fixture.manifest)) {
        foreach ($key in $identity.Keys) { $document[$key] = $identity[$key] }
    }
    Set-FixtureEnvironment $fixture.profile $fixture.manifest { param($state) $state.buildConfiguration = 'Shipping' }
    $nativeDirectory = Join-Path $directory ([guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($nativeDirectory) | Out-Null
    $profileName = $fixture.manifest.profileFile
    $manifestName = $profileName -replace '\.json$', '-manifest.json'
    $terminal = @{
        schemaVersion = 2; runId = $identity.runId; processId = $identity.processId
        configuration = 'Shipping'; scenario = 'normal'; status = 'passed'; outcomeCode = 0
        processExitPolicy = 'graceful-zero'
        captureId = $fixture.profile.captureId; profileFile = $profileName; manifestFile = $manifestName
        errorCode = $null; errors = @()
    }
    $control = @{ omitTerminal = $false; omitManifest = $false; malformedTerminal = $false; expectedRunId = $identity.runId
        terminalTime = $processStarted.UtcDateTime.AddSeconds(40); profileTime = $processStarted.UtcDateTime.AddSeconds(38) }
    & $Mutate $fixture.profile $fixture.manifest $terminal $control
    $identity.runId = $control.expectedRunId
    foreach ($entry in @(@{ name = $profileName; data = $fixture.profile }, @{ name = $manifestName; data = $fixture.manifest })) {
        if ($entry.name -eq $manifestName -and $control.omitManifest) { continue }
        $path = Join-Path $nativeDirectory $entry.name
        ConvertTo-Json -InputObject $entry.data -Depth 100 | Set-Content -LiteralPath $path -Encoding utf8
        [IO.File]::SetLastWriteTimeUtc($path, $control.profileTime)
    }
    $terminalName = if ($control.omitTerminal) { 'result.json.pending.tmp' } else { 'result.json' }
    $terminalPath = Join-Path $nativeDirectory $terminalName
    $terminalText = if ($control.malformedTerminal) { '{ partial' } else { ConvertTo-Json -InputObject $terminal -Depth 100 }
    $terminalText | Set-Content -LiteralPath $terminalPath -Encoding utf8
    [IO.File]::SetLastWriteTimeUtc($terminalPath, $control.terminalTime)
    $failure = $null
    try {
        $result = Test-NativeBenchmarkResult $nativeDirectory $identity $ProcessExitCode normal 30 5 $processStarted $processFinished
        Assert-Benchmark $result.passed 'Native valid fixture did not pass.'
    } catch { $failure = $_.Exception.Message }
    if ($ExpectedError) { Assert-Benchmark ($failure -and $failure -match $ExpectedError) "$Name`: expected '$ExpectedError', got '$failure'" }
    else { Assert-Benchmark (!$failure) "$Name`: $failure" }
    $script:passed++
    Write-Output "PASS $Name"
}

function Set-CatalogBudgetFixture {
    param($Profile, $Manifest)
    foreach ($document in @($Profile.benchmark, $Manifest)) {
        $document.budgetPolicyVersion = 2
        $document.budgetLimits = @{
            collections = 1044; pieces = 1048576; hullSlots = 8388608
            queuedImpacts = 1024; pendingAssetLoads = 16; registrationsPerFrame = 8; impactFX = 16
        }
        $document.maxAwakeCollections = 100
        $document.maxAwakePieces = 50000
        $document.maxAllocatedCollections = 150
        $document.maxAllocatedPieces = 75000
        $document.maxAllocatedHullSlots = 150000
        $document.maxPendingAssetLoads = 16
        $document.maxRegistrationsPerFrame = 8
    }
}

function Set-DeferredMaximalFixture {
    param($Profile, $Manifest)
    Set-SpectacleFixture $Profile $Manifest
    # Synthetic one-second frames plus a real 29.5s boundary: all slots fit before the unchanged end.
    $frames = [double[]]::new(29)
    [Array]::Fill($frames, 1.0)
    $frames += @(0.5, 0.5)
    $Profile.frameTimesSeconds = $frames
    foreach ($document in @($Profile, $Manifest)) {
        $document.frameCount = 31
        $document.minFPS = 1.0
        $document.averageFPS = 31.0 / 30.0
        $document.worstFrameMilliseconds = 1000.0
        $document.framesOver60FPSBudget = 31
        $document.framesOver30FPSBudget = 31
    }
    $Profile.framesOver60FPSBudgetWithTolerance = 31
    $Profile.framesOver30FPSBudgetWithTolerance = 31
    $events = @(); $elapsed = 0.0; $next = 0; $maxLate = 0.0; $lastAdmission = -1.0
    for ($boundary = 0; $boundary -lt $frames.Count; $boundary++) {
        $due = [Math]::Min(60, [Math]::Floor($elapsed * 2) + 1)
        $batch = 0
        while ($next -lt $due) {
            $actual = $elapsed + 0.001 + $batch * 0.01
            $admitted = $actual + 0.001
            $late = $admitted - $next * 0.5
            $events += @{
                slot = $next; plannedSeconds = $next * 0.5; actualSeconds = $actual; admittedSeconds = $admitted
                boundaryIndex = $boundary; boundarySeconds = $elapsed; batchIndex = $batch
                pendingDueOperationsBeforeAttempt = $due - $next; admissionLatenessSeconds = $late; result = 'accepted'
            }
            $maxLate = [Math]::Max($maxLate, $late); $lastAdmission = $admitted
            $next++; $batch++
        }
        $elapsed += $frames[$boundary]
    }
    foreach ($document in @($Profile.benchmark, $Manifest)) {
        $document.maximalSchedulePolicyVersion = 2
        $document.maximalSchedule = @{
            periodSeconds = 0.5; maxAttemptsPerBoundary = 8; dueOperations = 60; pendingOperations = 0
            pendingSlots = @(); peakPendingOperations = 2; peakAttemptsPerBoundary = 2; peakAdmissionsPerBoundary = 2
            backpressureBoundaries = 0; lastAdmissionSeconds = $lastAdmission; maxAdmissionLatenessSeconds = $maxLate
        }
        $document.events = Copy-Fixture $events
        foreach ($field in @('plannedOperations', 'attemptedOperations', 'acceptedOperations', 'cityAcceptedImpacts', 'effectsRequests')) {
            $document[$field] = 60
        }
        foreach ($field in @('skippedOperations', 'rejectedOperations', 'weaponRejectedImpacts')) { $document[$field] = 0 }
    }
}

function Set-SaturationMaximalFixture {
    param($Profile, $Manifest)
    Set-DeferredMaximalFixture $Profile $Manifest
    $frames = [double[]]::new(14)
    [Array]::Fill($frames, 2.0)
    $frames += @(0.914, 2.177)
    $events = @(); $elapsed = 0.0; $next = 0; $lastAdmission = -1.0
    for ($boundary = 0; $boundary -lt $frames.Count; $boundary++) {
        for ($batch = 0; $batch -lt 8 -and $next -lt 60; $batch++) {
            $actual = $elapsed + 0.001 + $batch * 0.01
            $admitted = $actual + 0.001
            $events += @{
                slot = $next; plannedSeconds = 0.0; releaseSeconds = 0.0
                actualSeconds = $actual; admittedSeconds = $admitted; admissionDelaySeconds = $admitted
                boundaryIndex = $boundary; boundarySeconds = $elapsed; batchIndex = $batch
                pendingDueOperationsBeforeAttempt = 60 - $next; result = 'accepted'
            }
            $lastAdmission = $admitted; $next++
        }
        $elapsed += $frames[$boundary]
    }
    $Profile.frameTimesSeconds = $frames
    $Profile.durationSeconds = $elapsed
    $Profile.finishedUtc = $processStarted.AddSeconds($elapsed + 7).ToString('o')
    foreach ($document in @($Profile, $Manifest)) {
        $document.frameCount = $frames.Count
        $document.minFPS = 1.0 / 2.177
        $document.averageFPS = $frames.Count / $elapsed
        $document.worstFrameMilliseconds = 2177.0
        $document.framesOver60FPSBudget = $frames.Count
        $document.framesOver30FPSBudget = $frames.Count
    }
    $Profile.framesOver60FPSBudgetWithTolerance = $frames.Count
    $Profile.framesOver30FPSBudgetWithTolerance = $frames.Count
    foreach ($document in @($Profile.benchmark, $Manifest)) {
        $document.measuredWallSeconds = $elapsed
        $document.maximalSchedulePolicyVersion = 3
        $document.maximalSchedule = @{
            releasePolicy = 'allAtFirstMeasuredBoundary'; releaseSeconds = 0.0; releaseBoundaryIndex = 0
            releasedOperations = 60; dueOperations = 60; pendingOperations = 0; pendingSlots = @()
            maxAttemptsPerBoundary = 8; peakPendingOperations = 60; peakAttemptsPerBoundary = 8
            peakAdmissionsPerBoundary = 8; backpressureBoundaries = 0
            lastAdmissionSeconds = $lastAdmission; maxAdmissionDelaySeconds = $lastAdmission
        }
        $document.events = Copy-Fixture $events
    }
}

try {
    Invoke-Fixture 'V3 saturation preserves sixty operations despite the final 28.914 to 31.091 boundary gap' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
    } -Scenario maximal
    Invoke-Fixture 'V3 cannot claim V2 periodic release timing' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.periodSeconds = 0.5 }
    } 'periodic V2 timing' -Scenario maximal
    Invoke-Fixture 'V3 must disclose its whole-workload release policy' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.releasePolicy = 'unreported' }
    } 'explicit saturation release' -Scenario maximal
    Invoke-Fixture 'V3 cannot defer logical release after measurement start' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.releaseSeconds = 1 }
    } 'releaseSeconds' -Scenario maximal
    Invoke-Fixture 'V3 cannot admit during warmup' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[0].actualSeconds = -0.001 }
    } 'admission timing' -Scenario maximal
    Invoke-Fixture 'V3 cannot reduce the sixty-operation workload' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.plannedOperations = 58 }
    } 'plannedOperations' -Scenario maximal
    Invoke-Fixture 'V3 cannot hide unreleased requests' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.releasedOperations = 58 }
    } 'releasedOperations' -Scenario maximal
    Invoke-Fixture 'V3 cannot inflate its batch limit' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.maxAttemptsPerBoundary = 60 }
    } 'maxAttemptsPerBoundary' -Scenario maximal
    Invoke-Fixture 'V3 cannot process a ninth request on one actual boundary' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) {
            $e = $d.events[8]; $e.boundaryIndex = 0; $e.boundarySeconds = 0; $e.batchIndex = 8
            $e.actualSeconds = 0.081; $e.admittedSeconds = 0.082; $e.admissionDelaySeconds = 0.082
        }
    } 'batch exceeded' -Scenario maximal
    Invoke-Fixture 'V3 end backlog still fails' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.pendingOperations = 2; $d.maximalSchedule.pendingSlots = @(58,59) }
    } 'pendingOperations' -Scenario maximal
    Invoke-Fixture 'V3 cannot admit at the ending boundary after the deadline' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[59].admittedSeconds = 31.091 }
    } 'admission timing' -Scenario maximal
    Invoke-Fixture 'V3 cannot hide a rejected request' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[59].result = 'rejected' }
    } 'unaccepted request' -Scenario maximal
    Invoke-Fixture 'V3 cannot falsify delay from its released workload' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.maxAdmissionDelaySeconds = 0 }
    } 'maxAdmissionDelaySeconds' -Scenario maximal
    Invoke-Fixture 'V3 cannot relabel its saturation burst as historical V2' {
        param($p,$m) Set-SaturationMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedulePolicyVersion = 2 }
    } 'periodSeconds' -Scenario maximal
    Invoke-Fixture 'V2 cannot be retroactively relabelled as V3' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedulePolicyVersion = 3 }
    } 'explicit saturation release' -Scenario maximal
    Invoke-Fixture 'retained maximal slots admit all sixty during slow measured frames' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
    } -Scenario maximal
    Invoke-Fixture 'retained schedule cannot omit its explicit version' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        $p.benchmark.Remove('maximalSchedulePolicyVersion'); $m.Remove('maximalSchedulePolicyVersion')
    } 'explicit policy version' -Scenario maximal
    Invoke-Fixture 'retained event shape cannot hide behind absent legacy policy' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.Remove('maximalSchedulePolicyVersion'); $d.Remove('maximalSchedule') }
    } 'explicit policy version' -Scenario maximal
    Invoke-Fixture 'retained schedule rejects unknown versions' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        $p.benchmark.maximalSchedulePolicyVersion = 99; $m.maximalSchedulePolicyVersion = 99
    } 'maximalSchedulePolicyVersion' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot inflate its per-boundary cap' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        $p.benchmark.maximalSchedule.maxAttemptsPerBoundary = 9; $m.maximalSchedule.maxAttemptsPerBoundary = 9
    } 'maxAttemptsPerBoundary' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot change the planned half-second period' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        $p.benchmark.maximalSchedule.periodSeconds = 1; $m.maximalSchedule.periodSeconds = 1
    } 'periodSeconds' -Scenario maximal
    Invoke-Fixture 'retained schedule end backlog still fails' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.maximalSchedule.pendingOperations = 1; $d.maximalSchedule.pendingSlots = @(59) }
    } 'pendingOperations' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot omit a planned request' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events = $d.events[0..58] }
    } 'every planned slot' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot move work beyond thirty seconds' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[59].admittedSeconds = 30.001 }
    } 'admission timing' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot queue future work at measurement start' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) {
            $d.events[1].boundaryIndex = 0; $d.events[1].boundarySeconds = 0; $d.events[1].batchIndex = 1
        }
    } 'future slot' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot falsify its backlog peak' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        $p.benchmark.maximalSchedule.peakPendingOperations = 1; $m.maximalSchedule.peakPendingOperations = 1
    } 'peakPendingOperations' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot falsify actual burst size' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        $p.benchmark.maximalSchedule.peakAdmissionsPerBoundary = 1; $m.maximalSchedule.peakAdmissionsPerBoundary = 1
    } 'peakAdmissionsPerBoundary' -Scenario maximal
    Invoke-Fixture 'retained schedule verifies actual attempts per engine boundary' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) {
            for ($i = 0; $i -lt 9; $i++) {
                $e = $d.events[$i]; $e.boundaryIndex = 8; $e.boundarySeconds = 8; $e.batchIndex = $i
                $e.actualSeconds = 8 + $i * 0.01; $e.admittedSeconds = $e.actualSeconds + 0.001
                $e.admissionLatenessSeconds = $e.admittedSeconds - $e.plannedSeconds
                $e.pendingDueOperationsBeforeAttempt = 17 - $i
            }
        }
    } 'batch exceeded' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot detach events from measured frame boundaries' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[1].boundarySeconds = 0.9 }
    } 'raw frame trace' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot report an attempted slot as accepted' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[1].result = 'rejected' }
    } 'unaccepted request' -Scenario maximal
    Invoke-Fixture 'retained schedule cannot duplicate an earlier slot' {
        param($p,$m) Set-DeferredMaximalFixture $p $m
        foreach ($d in @($p.benchmark,$m)) { $d.events[1].slot = 0 }
    } 'slot' -Scenario maximal
    Invoke-NativeFixture 'explicit catalog policy accepts disclosed larger finite costs' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
    }
    Invoke-NativeFixture 'catalog cannot exceed resident pieces even if awake counts fit' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
        $p.benchmark.maxAllocatedPieces = 1048577; $m.maxAllocatedPieces = 1048577
    } 'maxAllocatedPieces'
    Invoke-NativeFixture 'catalog declared caps cannot be inflated' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
        $p.benchmark.budgetLimits.pieces = 99999999; $m.budgetLimits.pieces = 99999999
    } 'pieces'
    Invoke-NativeFixture 'catalog requires explicit version' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
        $p.benchmark.Remove('budgetPolicyVersion'); $m.Remove('budgetPolicyVersion')
    } 'policy version'
    Invoke-NativeFixture 'catalog cannot omit resident telemetry' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
        $p.benchmark.Remove('maxAllocatedHullSlots'); $m.Remove('maxAllocatedHullSlots')
    } 'maxAllocatedHullSlots'
    Invoke-NativeFixture 'catalog registrations remain bounded per frame' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
        $p.benchmark.maxRegistrationsPerFrame = 9; $m.maxRegistrationsPerFrame = 9
    } 'maxRegistrationsPerFrame'
    Invoke-NativeFixture 'catalog does not exempt normal FPS floor' {
        param($p,$m,$t,$c) Set-CatalogBudgetFixture $p $m
        $p.frameTimesSeconds[0] = 0.1
    } 'minimum-FPS'
    Invoke-Fixture 'normal native manifest without benchmarkPassed'
    Invoke-Fixture 'light native schema' -Scenario light
    Invoke-Fixture 'maximal at 40 FPS passes 30, not 60' -Scenario maximal
    Invoke-Fixture 'spectacle accepts 20 FPS without claiming frame-floor success' {
        param($p,$m) Set-SpectacleFixture $p $m
    } -Scenario maximal
    Invoke-Fixture 'legacy maximal keeps its historical strict floor' {
        param($p,$m)
        Set-SpectacleFixture $p $m
        foreach ($d in @($p,$m)) { $d.Remove('performancePolicy'); $d.Remove('frameTargetRequired') }
    } 'scenarioTargetMet' -Scenario maximal
    Invoke-Fixture 'normal cannot claim spectacle exemption' {
        param($p,$m) Set-SpectacleFixture $p $m
    } 'only valid for maximal'
    Invoke-Fixture 'spectacle still verifies actual target verdict' {
        param($p,$m) Set-SpectacleFixture $p $m; $p.frameTargetMet = $true
    } 'frameTargetMet' -Scenario maximal
    Invoke-Fixture 'spectacle cannot forge the manifest target' {
        param($p,$m) Set-SpectacleFixture $p $m; $m.scenarioTargetMet = $true
    } 'scenarioTargetMet' -Scenario maximal
    Invoke-Fixture 'spectacle cannot shorten the measurement' {
        param($p,$m) Set-SpectacleFixture $p $m; $p.frameTimesSeconds = $p.frameTimesSeconds[0..580]
    } 'full-duration' -Scenario maximal
    Invoke-Fixture 'spectacle cannot bypass foreground validity' {
        param($p,$m) Set-SpectacleFixture $p $m
        Set-FixtureEnvironment $p $m { param($s) $s.viewport.isForegroundWindow = $false }
    } 'isForegroundWindow' -Scenario maximal
    Invoke-Fixture 'spectacle policy must match both reports' {
        param($p,$m) Set-SpectacleFixture $p $m; $m.performancePolicy = 'strict-frame-floor-v1'
    } 'performance policy' -Scenario maximal
    Invoke-Fixture 'unknown performance policy is rejected' {
        param($p,$m) $p.performancePolicy = 'anything-passes'; $m.performancePolicy = 'anything-passes'
    } 'performance policy'
    Invoke-Fixture 'spectacle does not permit forged aggregate metrics' {
        param($p,$m) Set-SpectacleFixture $p $m; $p.averageFPS = 60
    } 'averageFPS' -Scenario maximal
    Invoke-Fixture 'explicit longer duration' -Duration 31.5
    Invoke-Fixture 'explicit native manifest pass also enforced' { param($p,$m) $m.benchmarkPassed = $true }
    Invoke-Fixture 'mismatched capture ID' { param($p,$m) $m.captureId = 'ffffffffffffffffffffffffffffffff' } 'captureId mismatch'
    Invoke-Fixture 'mismatched filename' { param($p,$m) $m.profileFile = '..\other.json' } 'filename mismatch'
    Invoke-Fixture 'completed is not passed' { param($p,$m) $p.benchmarkPassed = $false } 'benchmarkPassed'
    Invoke-Fixture 'manifest native rejection' { param($p,$m) $m.benchmarkValid = $false } 'benchmarkValid'
    Invoke-Fixture 'manifest missing admission' { param($p,$m) $m.Remove('workloadAdmitted') } 'workloadAdmitted'
    Invoke-Fixture 'manifest explicit failed verdict' { param($p,$m) $m.benchmarkPassed = $false } 'benchmarkPassed'
    Invoke-Fixture 'string boolean rejected' { param($p,$m) $p.benchmarkPassed = 'true' } 'benchmarkPassed'
    Invoke-Fixture 'stale report' { param($p,$m) $p.startedUtc = '2026-09-15T19:00:00Z' } 'stale'
    Invoke-Fixture 'raw hitch defeats forged passing metrics' { param($p,$m) $p.frameTimesSeconds[0] = 0.05 } 'minimum-FPS'
    Invoke-Fixture 'two nanoseconds over 60 FPS budget fail' { param($p,$m) $p.frameTimesSeconds[0] = 1.0 / 60 + 2e-9 } 'minimum-FPS'
    Invoke-Fixture 'only native one-nanosecond tolerance is allowed' {
        param($p,$m)
        $seconds = 1.0 / 60 + 0.5e-9
        $p.frameTimesSeconds[0] = $seconds
        $sum = 30.0 - 0.01 + $seconds
        $p.durationSeconds = $sum
        $p.benchmark.measuredWallSeconds = $sum
        $m.measuredWallSeconds = $sum
        foreach ($d in @($p,$m)) {
            $d.minFPS = 1.0 / $seconds
            $d.averageFPS = 3000.0 / $sum
            $d.worstFrameMilliseconds = $seconds * 1000
            $d.framesOver60FPSBudget = 1
        }
    }
    Invoke-Fixture 'raw zero rejected' { param($p,$m) $p.frameTimesSeconds[0] = 0 } 'Invalid raw frame'
    Invoke-Fixture 'raw nonfinite rejected' { param($p,$m) $p.frameTimesSeconds[0] = [double]::NaN } 'nonfinite number'
    Invoke-Fixture 'empty trace rejected' { param($p,$m) $p.frameTimesSeconds = @() } 'raw frame trace'
    Invoke-Fixture 'shortened trace rejected' { param($p,$m) $p.frameTimesSeconds = $p.frameTimesSeconds[0..2899] } 'full-duration'
    Invoke-Fixture 'warmup cannot be skipped' { param($p,$m) $p.excludedWarmupSeconds = 0 } 'warmup'
    Invoke-Fixture 'fractional frame count rejected' { param($p,$m) $p.frameCount = 3000.00000001 } 'frameCount mismatch'
    Invoke-Fixture 'tolerance cannot be relaxed' { param($p,$m) $p.thresholdToleranceMilliseconds = 0.01 } 'frame tolerance'
    Invoke-Fixture 'NullRHI rejected' { param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.nullRHI = $true } } 'nullRHI'
    Invoke-Fixture 'PIE rejected' { param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.isPIE = $true } } 'isPIE'
    Invoke-Fixture 'uncooked rejected' { param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.requiresCookedData = $false } } 'requiresCookedData'
    Invoke-Fixture 'wrong client dimensions rejected' { param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.viewport.width = 1280 } } 'width'
    Invoke-Fixture 'background rejected' { param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.viewport.isForegroundWindow = $false } } 'isForegroundWindow'
    Invoke-Fixture 'fullscreen report must match declared presentation' {
        param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.viewport.windowMode = 'Fullscreen' }
    } -WindowMode Fullscreen
    Invoke-Fixture 'borderless is not fullscreen even at 1080p' {
        param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.viewport.windowMode = 'WindowedFullscreen' }
    } 'presentation mode' -WindowMode Fullscreen
    Invoke-Fixture 'fullscreen cannot accept desktop 3840x2160' {
        param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.viewport.windowMode = 'Fullscreen'; $s.viewport.width = 3840; $s.viewport.height = 2160 }
    } 'width' -WindowMode Fullscreen
    Invoke-Fixture 'fullscreen cannot bypass foreground validation' {
        param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.viewport.windowMode = 'Fullscreen'; $s.viewport.isForegroundWindow = $false }
    } 'isForegroundWindow' -WindowMode Fullscreen
    Invoke-Fixture 'dynamic resolution rejected' { param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.qualityAndTimingCVars['r.DynamicRes.OperationMode'] = 1 } } 'OperationMode'
    Invoke-Fixture 'counter contradiction rejected' { param($p,$m) $m.acceptedOperations = 1 } 'contradicts'
    Invoke-Fixture 'quality changes rejected' { param($p,$m) $p.benchmark.measurementEndSettings.qualityAndTimingCVars['sg.ShadowQuality'] = 0; $m.measurementEndSettings.qualityAndTimingCVars['sg.ShadowQuality'] = 0 } 'environment changed'
    Invoke-Fixture 'non-default native weapon tuning rejected' { param($p,$m) $p.benchmark.weaponSettingsAtStart.bombYieldTonsTNT = 2; $m.weaponSettingsAtStart.bombYieldTonsTNT = 2 } 'bombYieldTonsTNT'
    Invoke-Fixture 'legacy route cannot accept Shipping without terminal identity' {
        param($p,$m) Set-FixtureEnvironment $p $m { param($s) $s.buildConfiguration = 'Shipping' }
    } 'Shipping requires validated'
    Invoke-NativeFixture 'Shipping complete result-last contract'
    Invoke-NativeFixture 'Shipping schema v1 is rejected' { param($p,$m,$t,$c) $t.schemaVersion = 1 } 'Unsupported terminal schema'
    Invoke-NativeFixture 'Shipping process policy is required' { param($p,$m,$t,$c) $t.Remove('processExitPolicy') } 'missing field: processExitPolicy'
    Invoke-NativeFixture 'Shipping process policy cannot imply forced exit' { param($p,$m,$t,$c) $t.processExitPolicy = 'forced-status' } 'processExitPolicy'
    Invoke-NativeFixture 'Shipping old exitCode cannot replace outcomeCode' { param($p,$m,$t,$c) $t.Remove('outcomeCode'); $t.exitCode = 0 } 'missing field: outcomeCode'
    Invoke-NativeFixture 'Shipping passed status cannot carry failed outcome' { param($p,$m,$t,$c) $t.outcomeCode = 1 } 'status/outcomeCode'
    Invoke-NativeFixture 'Shipping string outcome cannot imply success' { param($p,$m,$t,$c) $t.outcomeCode = '0' } 'nonfinite number: outcomeCode'
    Invoke-NativeFixture 'Shipping uppercase input matches canonical published GUID value' { param($p,$m,$t,$c) $c.expectedRunId = $c.expectedRunId.ToUpperInvariant() }
    Invoke-NativeFixture 'Shipping terminal RunID must be canonical lowercase' { param($p,$m,$t,$c) $t.runId = $t.runId.ToUpperInvariant() } 'canonical lowercase'
    Invoke-NativeFixture 'Shipping profile RunID must be canonical lowercase' { param($p,$m,$t,$c) $p.runId = $p.runId.ToUpperInvariant() } 'canonical lowercase'
    Invoke-NativeFixture 'Shipping manifest RunID must be canonical lowercase' { param($p,$m,$t,$c) $m.runId = $m.runId.ToUpperInvariant() } 'canonical lowercase'
    Invoke-NativeFixture 'Shipping wrong terminal PID' { param($p,$m,$t,$c) $t.processId = 43211 } 'processId mismatch'
    Invoke-NativeFixture 'Shipping wrong profile PID' { param($p,$m,$t,$c) $p.processId = 43211 } 'processId mismatch'
    Invoke-NativeFixture 'Shipping wrong run GUID' { param($p,$m,$t,$c) $t.runId = 'ffffffffffffffffffffffffffffffff' } 'runId mismatch'
    Invoke-NativeFixture 'Shipping wrong manifest run GUID' { param($p,$m,$t,$c) $m.runId = 'ffffffffffffffffffffffffffffffff' } 'runId mismatch'
    Invoke-NativeFixture 'Shipping terminal lies about configuration' { param($p,$m,$t,$c) $t.configuration = 'Development' } 'configuration mismatch'
    Invoke-NativeFixture 'Shipping measured Development is rejected' {
        param($p,$m,$t,$c) Set-FixtureEnvironment $p $m { param($s) $s.buildConfiguration = 'Development' }
    } 'Measured build configuration'
    Invoke-NativeFixture 'Shipping missing identity is rejected' { param($p,$m,$t,$c) $m.Remove('processId') } 'processId'
    Invoke-NativeFixture 'Shipping capture ID mismatch' { param($p,$m,$t,$c) $t.captureId = 'ffffffffffffffffffffffffffffffff' } 'captureId mismatch'
    Invoke-NativeFixture 'Shipping successful status with failed process exit' -ProcessExitCode 1 -ExpectedError 'transport failure'
    Invoke-NativeFixture 'Shipping fractional PID rejected' { param($p,$m,$t,$c) $t.processId = 43210.5 } 'processId mismatch'
    Invoke-NativeFixture 'Shipping missing terminal, partial file is not success' { param($p,$m,$t,$c) $c.omitTerminal = $true } 'Cannot find path|does not exist'
    Invoke-NativeFixture 'Shipping malformed terminal' { param($p,$m,$t,$c) $c.malformedTerminal = $true } 'JSON|Json'
    Invoke-NativeFixture 'Shipping missing manifest' { param($p,$m,$t,$c) $c.omitManifest = $true } 'Cannot find path|does not exist'
    Invoke-NativeFixture 'Shipping terminal is stale' { param($p,$m,$t,$c) $c.terminalTime = $processStarted.UtcDateTime.AddDays(-1) } 'Stale native terminal'
    Invoke-NativeFixture 'Shipping reports are stale' { param($p,$m,$t,$c) $c.profileTime = $processStarted.UtcDateTime.AddDays(-1) } 'stale reports'
    Invoke-NativeFixture 'Shipping terminal must be written last' { param($p,$m,$t,$c) $c.profileTime = $c.terminalTime.AddSeconds(1) } 'not published last'
    Invoke-NativeFixture 'Shipping traversal rejected before file access' { param($p,$m,$t,$c) $t.profileFile = '..\outside.json' } 'safe contained'
    Invoke-NativeFixture 'Shipping absolute report path rejected' { param($p,$m,$t,$c) $t.profileFile = 'C:\outside.json' } 'safe contained'
    Invoke-NativeFixture 'Shipping alternate data stream rejected' { param($p,$m,$t,$c) $t.profileFile = 'normal.json:stream' } 'safe contained'
    Invoke-NativeFixture 'Shipping terminal cannot stand in for profile' { param($p,$m,$t,$c) $t.profileFile = 'result.json' } 'distinct reports'
    Invoke-NativeFixture 'Shipping owner marker cannot stand in for profile' { param($p,$m,$t,$c) $t.profileFile = 'owner.json' } 'distinct reports'
    Invoke-NativeFixture 'Shipping passed status contradicts native report' { param($p,$m,$t,$c) $p.benchmarkPassed = $false } 'benchmarkPassed'
    Invoke-NativeFixture 'Shipping passed status cannot contain errors' { param($p,$m,$t,$c) $t.errorCode = 'unexpected'; $t.errors = @('failure') } 'passed status contains errors'
    Invoke-NativeFixture 'Shipping absent raw telemetry is rejected' { param($p,$m,$t,$c) $p.Remove('frameTimesSeconds') } 'raw frame trace'
    Invoke-NativeFixture 'Shipping absent workload telemetry is rejected' {
        param($p,$m,$t,$c) $p.benchmark.Remove('flightDistanceCm'); $m.Remove('flightDistanceCm')
    } 'flightDistanceCm'
    Invoke-NativeFixture 'Shipping native budget overrun is rejected' {
        param($p,$m,$t,$c) $p.benchmark.maxQueuedImpacts = 65; $m.maxQueuedImpacts = 65
    } 'Native budget exceeded'
    Invoke-NativeFixture 'Shipping cannot bypass foreground' {
        param($p,$m,$t,$c) Set-FixtureEnvironment $p $m { param($s) $s.viewport.isForegroundWindow = $false }
    } 'isForegroundWindow'
    Invoke-NativeFixture 'Shipping failed terminal cannot contradict a passed native profile' {
        param($p,$m,$t,$c) $t.status = 'failed'; $t.outcomeCode = 1; $t.errorCode = 'benchmarkFailed'; $t.errors = @('failure')
    } 'benchmarkPassed'
    Invoke-NativeFixture 'Shipping failed status cannot carry successful outcome' {
        param($p,$m,$t,$c) $t.status = 'failed'; $t.outcomeCode = 0; $t.errorCode = 'benchmarkFailed'; $t.errors = @('failure')
    } 'status/outcomeCode'
    Invoke-NativeFixture 'Shipping semantic failure with OS exit zero remains failure' {
        param($p,$m,$t,$c)
        $t.status = 'failed'; $t.outcomeCode = 1; $t.errorCode = 'benchmarkFailed'; $t.errors = @('foreground failure')
        $p.benchmarkPassed = $false; $m.scenarioTargetMet = $false
    } 'Native benchmark failed'
    Invoke-NativeFixture 'Shipping semantic startup error with OS exit zero remains failure' {
        param($p,$m,$t,$c)
        $t.status = 'error'; $t.outcomeCode = 2; $t.errorCode = 'startupFailed'; $t.errors = @('startup failed')
        $t.captureId = $null; $t.profileFile = $null; $t.manifestFile = $null
    } 'Native startup/publication error'
    Invoke-NativeFixture 'Shipping failure cannot omit its reason' {
        param($p,$m,$t,$c) $t.status = 'error'; $t.outcomeCode = 2
    } 'explicit error code'
    $previousCulture = [Globalization.CultureInfo]::CurrentCulture
    try {
        [Globalization.CultureInfo]::CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('fr-FR')
        Invoke-Fixture 'locale-independent report timestamps'
        $localized = @(New-BenchmarkArguments normal 31.5 5 (Join-Path $directory 'engine.log'))
        Assert-Benchmark ($localized -contains '-ExecCmds=DublinFlight.Benchmark.Start normal 31.5 5') 'Native duration used a localized decimal separator.'
    } finally { [Globalization.CultureInfo]::CurrentCulture = $previousCulture }

    $valid = New-Fixture
    $profilePath = Join-Path $directory $valid.manifest.profileFile
    $manifestPath = $profilePath -replace '\.json$', '-manifest.json'
    ConvertTo-Json -InputObject $valid.profile -Depth 100 | Set-Content -LiteralPath $profilePath -Encoding utf8
    Remove-Item -LiteralPath $manifestPath -Force
    $missingRejected = $false
    try { [void](Test-BenchmarkReport $profilePath $manifestPath normal 30 5 $processStarted $processFinished) }
    catch { $missingRejected = $_.Exception.Message -match 'does not exist|Cannot find path' }
    Assert-Benchmark $missingRejected 'A missing manifest was not rejected.'
    '{ invalid JSON' | Set-Content -LiteralPath $manifestPath -Encoding utf8
    $malformedRejected = $false
    try { [void](Test-BenchmarkReport $profilePath $manifestPath normal 30 5 $processStarted $processFinished) }
    catch { $malformedRejected = $_.Exception.Message -match 'JSON|Json' }
    Assert-Benchmark $malformedRejected 'Malformed manifest JSON was not rejected.'
    $script:passed += 2

    $arguments = @(New-BenchmarkArguments normal 30 5 (Join-Path $directory 'engine.log'))
    $windowed = @(New-BenchmarkArguments normal 30 5 (Join-Path $directory 'engine.log') -WindowMode Windowed)
    Assert-Benchmark (($arguments -join "`n") -ceq ($windowed -join "`n") -and
        $windowed -contains '-windowed' -and $windowed -contains '-ResX=1920' -and
        $windowed -contains '-ResY=1080') 'Default windowed launch changed.'
    $fullscreen = @(New-BenchmarkArguments normal 30 5 (Join-Path $directory 'engine.log') -WindowMode Fullscreen)
    Assert-Benchmark ($fullscreen -contains '-fullscreen' -and $fullscreen -contains '-Res=1920x1080f' -and
        $fullscreen -notcontains '-windowed') 'Fullscreen must use the native Res=...f override, not just the preference-sensitive fullscreen switch.'
    Assert-Benchmark (!(($fullscreen -join ' ') -match '(?i)1920x1080wf|3840|r.FullScreenMode|ini:|screenpercentage|vsync|maxfps|ForceRes|SetRes')) 'Fullscreen launch changes more than the declared presentation mode.'
    $script:passed++
    $nativeArgs = @(New-BenchmarkArguments normal 30 5 '' -NativeRunId 'abcdefabcdefabcdefabcdefabcdefab' -NativeOutputDirectory $directory)
    Assert-Benchmark (@($nativeArgs | Where-Object { $_ -match '^-DublinBenchmark' }).Count -eq 5 -and
        $nativeArgs -contains '-DublinBenchmark=normal' -and
        $nativeArgs -contains '-DublinBenchmarkRunId=abcdefabcdefabcdefabcdefabcdefab') 'Closed native request flags changed.'
    Assert-Benchmark (@($nativeArgs | Where-Object { $_ -match '(?i)^-(ExecCmds|TestExit|stdout|FullStdOutLogOutput|abslog|log|trace|cohort)' }).Count -eq 0) 'Shipping must not rely on or enable general console/logging/tracing/cohort switches.'
    $customNames = @($nativeArgs | Where-Object { $_ -match '^-DublinBenchmark' } |
        ForEach-Object { ($_ -split '=', 2)[0].ToLowerInvariant() })
    Assert-Benchmark (@($customNames | Sort-Object -Unique).Count -eq $customNames.Count) 'Driver emitted duplicate case-insensitive native option names.'
    $upperArgs = @(New-BenchmarkArguments normal 30 5 '' -NativeRunId 'ABCDEFABCDEFABCDEFABCDEFABCDEFAB' -NativeOutputDirectory $directory)
    Assert-Benchmark ($upperArgs -contains '-DublinBenchmarkRunId=ABCDEFABCDEFABCDEFABCDEFABCDEFAB') 'Uppercase input GUID must remain a valid request.'
    foreach ($incomplete in @(@{ NativeRunId = '' }, @{ NativeOutputDirectory = $directory })) {
        $rejected = $false
        try { [void](New-BenchmarkArguments normal 30 5 '' @incomplete) }
        catch { $rejected = $_.Exception.Message -match 'fresh run GUID' }
        Assert-Benchmark $rejected 'An attempted but incomplete native request fell back to legacy console entry.'
    }
    $script:passed++
    $zeroRejected = $false
    try { [void](New-BenchmarkArguments normal 30 5 '' -NativeRunId ('0' * 32) -NativeOutputDirectory $directory) }
    catch { $zeroRejected = $_.Exception.Message -match 'fresh run GUID' }
    Assert-Benchmark $zeroRejected 'Zero GUID cannot identify a fresh native run.'
    $script:passed++
    foreach ($playerArguments in @($windowed, $fullscreen)) {
        Assert-Benchmark (!(($playerArguments -join ' ') -match '(?i)(^|\s)-(unattended|unattendedinput|nosound|muteaudio|benchmark)(\s|$)')) 'Benchmark must retain normal player startup and audio rather than UE unattended/builtin benchmarking modes.'
        Assert-Benchmark ($playerArguments -contains '-ExecCmds=DublinFlight.Benchmark.Start normal 30 5' -and
            $playerArguments -contains '-TestExit=Benchmark profile published:') 'Player startup must retain automatic benchmark entry and completion.'
    }
    $script:passed++
    Assert-Benchmark ($arguments -contains '-ExecCmds=DublinFlight.Benchmark.Start normal 30 5' -and
        $arguments -contains '-TestExit=Benchmark profile published:') 'Native start/publication exit command mismatch.'
    Assert-Benchmark (!(($arguments -join ' ') -match '(?i)nullrhi|userdir|saveddirsuffix|screenpercentage|vsync|maxfps|nosound|fixed|sg\.')) 'Launch changes quality/timing/user settings.'
    $log = 'LogDublinFlightPerformance: Display: Benchmark profile published: status=completed, Saved\Performance\normal-20260916T190037Z-639000000000000000.json'
    $names = @(Get-BenchmarkReportNames ($log + "`n" + $log))
    Assert-Benchmark ($names.Count -eq 1) 'Own report names must be deduplicated.'
    Assert-Benchmark (@(Get-BenchmarkReportNames '-ExecCmds=DublinFlight.Benchmark.Start normal 30 5').Count -eq 0) 'Command echo cannot supply a report.'
    $capturedLine = '[2026.09.16-19.52.32:764][748]LogDublinFlightPerformance: Display: Benchmark profile published: status=invalid, Saved\Performance\normal-20260916T195232Z-639251851527440000.json'
    $capturedNames = @(Get-BenchmarkReportNames $capturedLine)
    $published = Join-Path $directory 'Published'
    $recovered = Join-Path $directory 'Recovered'
    [IO.Directory]::CreateDirectory($published) | Out-Null
    [IO.Directory]::CreateDirectory($recovered) | Out-Null
    $manifestName = $capturedNames[0] -replace '\.json$', '-manifest.json'
    $artifactNames = @($capturedNames[0], $manifestName, ($capturedNames[0] + '.capture.tmp'), ($manifestName + '.capture.tmp'))
    foreach ($artifact in $artifactNames) {
        [IO.File]::WriteAllText((Join-Path $published $artifact), "Preserve the full payload of $artifact")
    }
    [IO.File]::WriteAllText((Join-Path $published 'unrelated.json'), 'Do not collect other runs.')
    Copy-BenchmarkArtifacts $capturedNames @($published) $recovered
    $copied = @(Get-ChildItem -LiteralPath $recovered -File)
    Assert-Benchmark ($copied.Count -eq 4) "Captured-log copy regression: complete profile/manifest and partial files expected; got $($copied.Count) of 4."
    foreach ($artifact in $artifactNames) {
        Assert-Benchmark ((Get-FileHash -LiteralPath (Join-Path $published $artifact)).Hash -eq
            (Get-FileHash -LiteralPath (Join-Path $recovered $artifact)).Hash) 'Artifact copy changed raw bytes.'
    }
    $duplicateRejected = $false
    try { Copy-BenchmarkArtifacts $capturedNames @($published) $recovered }
    catch { $duplicateRejected = $_.Exception.Message -match 'Ambiguous report location' }
    Assert-Benchmark $duplicateRejected 'Existing evidence was silently overwritten.'
    $script:passed++
    $binaryDirectory = Join-Path $directory 'FixtureGame\Binaries\Win64'
    [IO.Directory]::CreateDirectory($binaryDirectory) | Out-Null
    $binary = Join-Path $binaryDirectory 'FixtureGame.exe'
    $bootstrap = Join-Path $directory 'FixtureGame.exe'
    [IO.File]::WriteAllBytes($binary, [byte[]]@())
    [IO.File]::WriteAllBytes($bootstrap, [byte[]]@())
    Assert-Benchmark ((Get-BenchmarkExecutable $bootstrap).FullName -eq $binary) 'Bootstrap did not resolve to owned game binary.'
    Assert-Benchmark ((Get-BenchmarkExecutable $binary).FullName -eq $binary) 'Direct executable path changed.'
    $shippingArchive = Join-Path $directory 'ShippingArchive'
    $shippingBinaries = Join-Path $shippingArchive 'FixtureGame\Binaries\Win64'
    $shippingPaks = Join-Path $shippingArchive 'FixtureGame\Content\Paks'
    [IO.Directory]::CreateDirectory($shippingBinaries) | Out-Null
    [IO.Directory]::CreateDirectory($shippingPaks) | Out-Null
    $shippingBinary = Join-Path $shippingBinaries 'FixtureGame-Win64-Shipping.exe'
    $shippingBootstrap = Join-Path $shippingArchive 'FixtureGame.exe'
    $pak = Join-Path $shippingPaks 'fixture.pak'
    foreach ($file in @($shippingBinary, $shippingBootstrap, $pak)) { [IO.File]::WriteAllText($file, 'fixture bytes') }
    $receiptPath = Join-Path $directory 'shipping.target'
    $receipt = @{
        TargetName = 'FixtureGame'; TargetType = 'Game'; Platform = 'Win64'; Configuration = 'Shipping'
        Version = @{ MajorVersion = 5; MinorVersion = 8; PatchVersion = 2 }
        BuildProducts = @(@{ Type = 'Executable'; Path = '$(ProjectDir)/Binaries/Win64/FixtureGame-Win64-Shipping.exe' })
    }
    ConvertTo-Json -InputObject $receipt -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8
    Assert-Benchmark ((Get-BenchmarkExecutable $shippingBootstrap $receiptPath).FullName -eq $shippingBinary) 'Shipping receipt did not resolve the actual child executable.'
    $rejected = $false
    try { [void](Get-BenchmarkExecutable $shippingBinary) } catch { $rejected = $_.Exception.Message -match 'BuildReceiptPath' }
    Assert-Benchmark $rejected 'Shipping filename alone was trusted.'
    $receipt.Configuration = 'Development'
    ConvertTo-Json -InputObject $receipt -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8
    $rejected = $false
    try { [void](Get-BenchmarkExecutable $shippingBinary $receiptPath) } catch { $rejected = $_.Exception.Message -match 'receipt.*mismatch' }
    Assert-Benchmark $rejected 'Shipping accepted the wrong receipt configuration.'
    $receipt.Configuration = 'Shipping'
    ConvertTo-Json -InputObject $receipt -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8
    $resolved = Get-BenchmarkExecutable $shippingBinary $receiptPath
    $cohort = Get-BenchmarkCohort $resolved $receiptPath
    Assert-Benchmark ($cohort.files.Count -eq 3) 'Cohort must include bootstrap, actual executable and cooked content.'
    [IO.File]::WriteAllText($pak, 'changed bytes')
    $changed = Get-BenchmarkCohort $resolved $receiptPath
    Assert-Benchmark ($changed.cohortSHA256 -cne $cohort.cohortSHA256) 'Content substitution was not detected.'
    [IO.File]::WriteAllText($pak, 'fixture bytes')
    [IO.File]::WriteAllText($shippingBinary, 'changed bytes')
    $changed = Get-BenchmarkCohort $resolved $receiptPath
    Assert-Benchmark ($changed.cohortSHA256 -cne $cohort.cohortSHA256) 'Executable substitution was not detected.'
    [IO.File]::WriteAllText($shippingBinary, 'fixture bytes')
    $generated = Join-Path $shippingArchive 'FixtureGame\Saved'
    [IO.Directory]::CreateDirectory($generated) | Out-Null
    [IO.File]::WriteAllText((Join-Path $generated 'runtime.log'), 'generated log')
    $unchanged = Get-BenchmarkCohort $resolved $receiptPath
    Assert-Benchmark ($unchanged.cohortSHA256 -ceq $cohort.cohortSHA256) 'Generated logs changed the binary/content cohort.'
    $script:passed += 4
    Assert-Benchmark (Test-BenchmarkValueEqual ([ordered]@{ a = 1; b = 2 }) ([ordered]@{ b = 2; a = 1 })) 'JSON key order is not semantic.'
    $script:passed += 3
    Write-Output "PASS command, log ownership and executable path checks"
    foreach ($file in @((Join-Path $PSScriptRoot '..\BenchmarkReport.ps1'),
        (Join-Path $PSScriptRoot '..\Run-Benchmarks.ps1'), $PSCommandPath)) {
        $tokens = $null; $parseErrors = $null
        [void][Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$parseErrors)
        $messages = @($parseErrors | ForEach-Object { $_.Message })
        Assert-Benchmark ($parseErrors.Count -eq 0) "$([IO.Path]::GetFileName($file)): $($messages -join '; ')"
    }
    $runnerPath = Join-Path $PSScriptRoot '..\Run-Benchmarks.ps1'
    $runnerText = Get-Content -LiteralPath $runnerPath -Raw
    Assert-Benchmark ($runnerText -notmatch 'DllImport|SendKeys|SetForegroundWindow|AppActivate|SetWindowPos|SendInput|Add-Type') 'Forbidden OS focus/input mechanism.'
    $ast = [Management.Automation.Language.Parser]::ParseFile($runnerPath, [ref]$tokens, [ref]$parseErrors)
    $stops = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Stop-Process'
    }, $true))
    Assert-Benchmark ($stops.Count -eq 1 -and $stops[0].Extent.Text -match 'Stop-Process -Id \$process.Id -Force') 'Termination must target only the owned process ID.'
    $script:passed++
    Write-Output "$passed fixture groups passed; all script syntax parsed; no Unreal processes launched."
} finally {
    # Only files under this invocation's freshly created, resolved fixture directory.
    Get-ChildItem -LiteralPath $directory -File -Recurse | Remove-Item -Force
    Get-ChildItem -LiteralPath $directory -Directory -Recurse | Sort-Object FullName -Descending | Remove-Item -Force
    Remove-Item -LiteralPath $directory -Force
}
