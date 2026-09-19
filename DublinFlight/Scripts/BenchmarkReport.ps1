#requires -Version 7.0
Set-StrictMode -Version Latest

function Assert-Benchmark {
    param([bool]$Condition, [string]$Message)
    if (!$Condition) { throw [IO.InvalidDataException]::new($Message) }
}

function Read-BenchmarkDocument {
    param([string]$Path)
    $document = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -AsHashtable -Depth 100
    Assert-Benchmark ($document -is [Collections.IDictionary]) "Not a JSON object: $Path"
    return $document
}

function Assert-BenchmarkFlag {
    param([Collections.IDictionary]$Document, [string]$Name, [bool]$Expected = $true)
    Assert-Benchmark ($Document.Contains($Name) -and $Document[$Name] -is [bool] -and
        $Document[$Name] -eq $Expected) "Missing or incorrect boolean: $Name (expected $Expected)"
}

function Get-BenchmarkNumber {
    param([Collections.IDictionary]$Document, [string]$Name)
    $value = $Document[$Name]
    Assert-Benchmark (($value -is [long] -or $value -is [int] -or $value -is [double] -or
        $value -is [decimal]) -and [double]::IsFinite([double]$value)) "Missing or nonfinite number: $Name"
    return [double]$value
}

function Get-BenchmarkTimestamp {
    param([Collections.IDictionary]$Document, [string]$Name)
    $value = $Document[$Name]
    # Newer PowerShell versions deserialize ISO strings as DateTime automatically.
    if ($value -is [DateTime] -or $value -is [DateTimeOffset]) { return [DateTimeOffset]$value }
    Assert-Benchmark ($value -is [string] -and $value -match '(Z|[+-]\d{2}:\d{2})$') "Missing UTC/offset timestamp: $Name"
    return [DateTimeOffset]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)
}

function Assert-BenchmarkNumber {
    param([Collections.IDictionary]$Document, [string]$Name, [double]$Expected)
    $actual = Get-BenchmarkNumber $Document $Name
    Assert-Benchmark ([Math]::Abs($actual - $Expected) -le [Math]::Max(1e-9, [Math]::Abs($Expected) * 1e-9)) `
        "Inconsistent $Name`: $actual; expected $Expected"
}

function Test-BenchmarkValueEqual {
    param($Left, $Right)
    if ($null -eq $Left -or $null -eq $Right) { return $null -eq $Left -and $null -eq $Right }
    if ($Left -is [Collections.IDictionary] -and $Right -is [Collections.IDictionary]) {
        if ($Left.Count -ne $Right.Count) { return $false }
        foreach ($key in $Left.Keys) {
            if (!$Right.Contains($key) -or !(Test-BenchmarkValueEqual $Left[$key] $Right[$key])) { return $false }
        }
        return $true
    }
    if ($Left -is [array] -and $Right -is [array]) {
        if ($Left.Count -ne $Right.Count) { return $false }
        for ($i = 0; $i -lt $Left.Count; $i++) {
            if (!(Test-BenchmarkValueEqual $Left[$i] $Right[$i])) { return $false }
        }
        return $true
    }
    return $Left.GetType() -eq $Right.GetType() -and $Left -ceq $Right
}

function Get-BenchmarkExecutable {
    param([string]$Path, [string]$BuildReceiptPath)
    $exe = Get-Item -LiteralPath $Path -ErrorAction Stop
    Assert-Benchmark (!$exe.PSIsContainer -and $exe.Extension -eq '.exe' -and
        $exe.BaseName -notmatch '(?i)UnrealEditor') 'Supply a cooked game executable, not UnrealEditor.'
    $receipt = if ($BuildReceiptPath) { Read-BenchmarkDocument $BuildReceiptPath } else { $null }
    # Resolve the staged bootstrap to the actual binary so the owned PID is the game.
    if ($exe.Directory.Name -ne 'Win64' -or $exe.Directory.Parent.Name -ne 'Binaries') {
        $binaryDirectory = Join-Path $exe.Directory.FullName ($exe.BaseName + '\Binaries\Win64')
        $candidates = @(Get-ChildItem -LiteralPath $binaryDirectory -Filter '*.exe' -ErrorAction Stop |
            Where-Object { $_.BaseName -match ('^' + [regex]::Escape($exe.BaseName) + '(-Win64-(Development|Test|Shipping))?$') })
        if ($receipt) {
            $candidates = @($candidates | Where-Object {
                ($receipt['Configuration'] -eq 'Shipping') -eq ($_.BaseName -match '-Win64-Shipping$')
            })
        } elseif (@($candidates | Where-Object { $_.BaseName -notmatch '-Win64-Shipping$' }).Count) {
            $candidates = @($candidates | Where-Object { $_.BaseName -notmatch '-Win64-Shipping$' })
        }
        Assert-Benchmark ($candidates.Count -eq 1) 'Bootstrap must resolve to exactly one matching game binary; supply its receipt when configurations coexist.'
        $exe = $candidates[0]
    }
    if ($exe.BaseName -match '-Win64-Shipping$') {
        Assert-Benchmark ($null -ne $receipt) 'Shipping requires -BuildReceiptPath; its name alone is not build evidence.'
        $projectName = $exe.Directory.Parent.Parent.Name
        Assert-Benchmark ($receipt['TargetName'] -ceq $projectName -and $receipt['Platform'] -ceq 'Win64' -and
            $receipt['TargetType'] -ceq 'Game' -and $receipt['Configuration'] -ceq 'Shipping') 'Shipping receipt target/platform/configuration mismatch.'
        $version = $receipt['Version']
        Assert-Benchmark ($version['MajorVersion'] -eq 5 -and $version['MinorVersion'] -eq 8 -and
            $version['PatchVersion'] -eq 2) 'Shipping receipt must identify UE 5.8.2.'
        Assert-Benchmark ($exe.Name -ceq "$projectName-Win64-Shipping.exe") 'Unexpected Shipping binary name.'
        $products = @($receipt['BuildProducts'] | Where-Object {
            $_['Type'] -ceq 'Executable' -and $_['Path'] -is [string] -and
            $_['Path'].Replace('/', '\').EndsWith('\Binaries\Win64\' + $exe.Name, [StringComparison]::OrdinalIgnoreCase)
        })
        Assert-Benchmark ($products.Count -eq 1) 'Shipping receipt does not identify the resolved executable.'
    } elseif ($receipt) {
        Assert-Benchmark ($receipt['Configuration'] -cne 'Shipping') 'Shipping receipt cannot select a Development binary.'
    }
    return $exe
}

function New-BenchmarkArguments {
    param([string]$Scenario, [double]$DurationSeconds, [double]$WarmupSeconds, [string]$LogPath,
        [ValidateSet('Windowed', 'Fullscreen')][string]$WindowMode = 'Windowed',
        [string]$NativeRunId, [string]$NativeOutputDirectory)
    Assert-Benchmark ($Scenario -cin @('normal', 'light', 'maximal')) 'Unknown benchmark scenario.'
    Assert-Benchmark ([double]::IsFinite($DurationSeconds) -and $DurationSeconds -ge 30 -and
        $DurationSeconds -le 300) 'Duration must be 30..300 seconds.'
    Assert-Benchmark ([double]::IsFinite($WarmupSeconds) -and $WarmupSeconds -ge 5 -and
        $WarmupSeconds -le 60) 'Warmup must be 5..60 seconds.'
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $duration = $DurationSeconds.ToString('0.#########', $culture)
    $warmup = $WarmupSeconds.ToString('0.#########', $culture)
    # GameEngine's Res= parser runs after FullScreen preference selection; f means Fullscreen, not wf.
    $presentation = if ($WindowMode -eq 'Fullscreen') { @('-fullscreen', '-Res=1920x1080f') }
        else { @('-windowed', '-ResX=1920', '-ResY=1080') }
    if ($PSBoundParameters.ContainsKey('NativeRunId') -or $PSBoundParameters.ContainsKey('NativeOutputDirectory')) {
        Assert-Benchmark ($NativeRunId -cmatch '^[0-9A-Fa-f]{32}$' -and $NativeRunId -ne ('0' * 32) -and
            [IO.Path]::IsPathFullyQualified($NativeOutputDirectory)) 'Native entry requires a fresh run GUID and absolute output directory.'
        return @('/Game/Maps/Dublin') + $presentation + @('-nop4', '-nosplash',
            "-DublinBenchmark=$Scenario", "-DublinBenchmarkRunId=$NativeRunId",
            "-DublinBenchmarkOutput=$NativeOutputDirectory",
            "-DublinBenchmarkDuration=$duration", "-DublinBenchmarkWarmup=$warmup")
    }
    # UE TickDeferredCommands uses LocalPlayer->GetWorld(); StartBenchmark then waits for BeginPlay.
    # TestExit fires only AFTER durable profile publication, not after the earlier manifest log.
    return @('/Game/Maps/Dublin') + $presentation + @(
        '-nop4', '-nosplash', '-stdout', '-FullStdOutLogOutput', "-abslog=$LogPath",
        "-ExecCmds=DublinFlight.Benchmark.Start $Scenario $duration $warmup",
        '-TestExit=Benchmark profile published:')
}

function Get-BenchmarkReportNames {
    param([string]$LogText)
    return @([regex]::Matches($LogText,
        '(?m)^.*LogDublinFlightPerformance:.*?(?<file>(?:normal|light|maximal)-\d{8}T\d{6}Z-\d+\.json)') |
        ForEach-Object { $_.Groups['file'].Value } | Sort-Object -Unique)
}

function Copy-BenchmarkArtifacts {
    param([string[]]$ProfileNames, [string[]]$PerformanceDirectories, [string]$DestinationDirectory)
    foreach ($file in $ProfileNames) {
        foreach ($directory in $PerformanceDirectories) {
            foreach ($candidate in @($file, ($file -replace '\.json$', '-manifest.json'))) {
                if (Test-Path -LiteralPath $directory) {
                    foreach ($artifact in @(Get-ChildItem -LiteralPath $directory -File |
                        Where-Object { $_.Name -eq $candidate -or ($_.Name.StartsWith($candidate + '.') -and $_.Name.EndsWith('.tmp')) })) {
                        $destination = Join-Path $DestinationDirectory $artifact.Name
                        Assert-Benchmark (!(Test-Path -LiteralPath $destination)) "Ambiguous report location: $($artifact.Name)"
                        Copy-Item -LiteralPath $artifact.FullName -Destination $destination -ErrorAction Stop
                    }
                }
            }
        }
    }
}

function Get-BenchmarkCohort {
    param([IO.FileInfo]$Executable, [string]$BuildReceiptPath, [string]$ExcludeDirectory)
    $archive = $Executable.Directory.Parent.Parent.Parent.FullName
    $content = Join-Path $Executable.Directory.Parent.Parent.FullName 'Content\Paks'
    Assert-Benchmark ((Test-Path -LiteralPath $content -PathType Container) -and
        @(Get-ChildItem -LiteralPath $content -File | Where-Object { $_.Extension -in @('.pak', '.utoc') }).Count -gt 0) 'Shipping requires a staged archive with cooked containers.'
    $exclude = if ($ExcludeDirectory) { [IO.Path]::GetFullPath($ExcludeDirectory).TrimEnd('\') } else { '' }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($archive)
    $paths = [Collections.Generic.List[string]]::new()
    while ($pending.Count) {
        $directory = $pending.Pop()
        Assert-Benchmark (((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Cohort directory is a reparse point: $directory"
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
            $relative = [IO.Path]::GetRelativePath($archive, $entry.FullName)
            if ($entry.FullName -eq $exclude -or $relative -match '^(?:[^\\]+\\)?(Saved|Intermediate|DerivedDataCache)(\\|$)' -or $entry.Extension -eq '.pdb') { continue }
            Assert-Benchmark (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Cohort entry is a reparse point: $relative"
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName) } else { $paths.Add($relative) }
        }
    }
    $paths.Sort([StringComparer]::Ordinal)
    $files = @()
    $canonical = [Text.StringBuilder]::new()
    foreach ($relative in $paths) {
        $file = Get-Item -LiteralPath (Join-Path $archive $relative)
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $files += [ordered]@{ path = $relative; bytes = $file.Length; sha256 = $hash }
        [void]$canonical.Append($relative).Append("`0").Append($file.Length.ToString([Globalization.CultureInfo]::InvariantCulture)).Append("`0").Append($hash).Append("`n")
    }
    $executableRelative = [IO.Path]::GetRelativePath($archive, $Executable.FullName)
    $contentPrefix = $Executable.Directory.Parent.Parent.Name + '\Content\'
    Assert-Benchmark ($files.path -contains $executableRelative -and
        @($files | Where-Object { $_.path.StartsWith($contentPrefix, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0) 'Cohort must contain the actual executable and cooked project content.'
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical.ToString()))).Replace('-', '') }
    finally { $algorithm.Dispose() }
    return [ordered]@{
        configuration = 'Shipping'; cohortSHA256 = $digest; files = $files
        executable = $executableRelative
        executableSHA256 = (Get-FileHash -LiteralPath $Executable.FullName -Algorithm SHA256).Hash
        receiptSHA256 = (Get-FileHash -LiteralPath $BuildReceiptPath -Algorithm SHA256).Hash
        policy = 'All archive files except root/project Saved, Intermediate, DerivedDataCache, PDBs and this fresh driver evidence directory. No caller-supplied cohort identity.'
    }
}

function Assert-NativeBenchmarkIdentity {
    param([Collections.IDictionary]$Document, [Collections.IDictionary]$Expected)
    Assert-Benchmark ($Expected['runId'] -is [string] -and $Expected['runId'] -cmatch '^[0-9A-Fa-f]{32}$' -and
        $Expected['runId'] -ne ('0' * 32) -and
        $Expected['configuration'] -cin @('Development', 'Shipping')) 'Invalid expected native identity.'
    $pidNumber = Get-BenchmarkNumber $Expected 'processId'
    Assert-Benchmark ($pidNumber -gt 0 -and $pidNumber -eq [Math]::Truncate($pidNumber)) 'Invalid owned process ID.'
    Assert-Benchmark ($Document['runId'] -is [string] -and $Document['runId'] -cmatch '^[0-9a-f]{32}$' -and
        $Document['runId'] -ne ('0' * 32)) 'Native runId publication must be nonzero canonical lowercase 32-hex.'
    Assert-Benchmark ([guid]::ParseExact($Document['runId'], 'N') -eq
        [guid]::ParseExact($Expected['runId'], 'N')) 'Native runId mismatch.'
    Assert-Benchmark ((Get-BenchmarkNumber $Document 'processId') -eq $pidNumber) 'Native processId mismatch.'
    Assert-Benchmark ($Document['configuration'] -ceq $Expected['configuration']) 'Native configuration mismatch.'
}

function Get-NativeBenchmarkFile {
    param([string]$Directory, [string]$Name)
    Assert-Benchmark ([IO.Path]::IsPathFullyQualified($Directory)) 'Native output directory must be absolute.'
    Assert-Benchmark ($Name -cmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*\.json$' -and
        $Name -notmatch '(^|[.])(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.]|$)') 'Native report path must be a safe contained JSON basename.'
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $path = [IO.Path]::GetFullPath((Join-Path $root $Name))
    Assert-Benchmark ($path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) 'Native report path escaped its output directory.'
    foreach ($item in @((Get-Item -LiteralPath $root -ErrorAction Stop), (Get-Item -LiteralPath $path -ErrorAction Stop))) {
        Assert-Benchmark (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Native output cannot use reparse points.'
    }
    Assert-Benchmark (Test-Path -LiteralPath $path -PathType Leaf) 'Native report is not a regular file.'
    return Get-Item -LiteralPath $path
}

function Test-NativeBenchmarkResult {
    param(
        [string]$Directory, [Collections.IDictionary]$Identity, [int]$ProcessExitCode,
        [string]$Scenario, [double]$DurationSeconds, [double]$WarmupSeconds,
        [DateTimeOffset]$ProcessStarted, [DateTimeOffset]$ProcessFinished,
        [ValidateSet('Windowed', 'Fullscreen')][string]$WindowMode = 'Windowed'
    )
    $terminalFile = Get-NativeBenchmarkFile $Directory 'result.json'
    $terminal = Read-BenchmarkDocument $terminalFile.FullName
    Assert-Benchmark ((Get-BenchmarkNumber $terminal 'schemaVersion') -eq 2) 'Unsupported terminal schema; version 2 is required.'
    foreach ($field in @('schemaVersion', 'runId', 'processId', 'configuration', 'scenario', 'status',
        'outcomeCode', 'processExitPolicy', 'captureId', 'profileFile', 'manifestFile', 'errorCode', 'errors')) {
        Assert-Benchmark ($terminal.Contains($field)) "Native result missing field: $field"
    }
    Assert-Benchmark ($terminal['processExitPolicy'] -ceq 'graceful-zero') 'Unsupported native processExitPolicy.'
    Assert-Benchmark ($ProcessExitCode -eq 0) "Native process transport failure: exit $ProcessExitCode."
    Assert-NativeBenchmarkIdentity $terminal $Identity
    Assert-Benchmark ($terminal['scenario'] -ceq $Scenario) 'Native terminal scenario mismatch.'
    $status = $terminal['status']
    Assert-Benchmark ($status -cin @('passed', 'failed', 'error')) 'Unknown native terminal status.'
    $expectedOutcome = @{ passed = 0; failed = 1; error = 2 }[$status]
    Assert-Benchmark ((Get-BenchmarkNumber $terminal 'outcomeCode') -eq $expectedOutcome) 'Native status/outcomeCode mismatch.'
    Assert-Benchmark ($terminal['errors'] -is [array] -and
        @($terminal['errors'] | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        ($null -eq $terminal['errorCode'] -or $terminal['errorCode'] -is [string])) 'Malformed native error telemetry.'
    if ($status -ne 'passed') {
        Assert-Benchmark (![string]::IsNullOrWhiteSpace($terminal['errorCode']) -and
            $terminal['errors'].Count -gt 0) 'Native failure must include an explicit error code and reasons.'
    }
    Assert-Benchmark ($terminalFile.LastWriteTimeUtc -ge $ProcessStarted.UtcDateTime.AddSeconds(-1) -and
        $terminalFile.LastWriteTimeUtc -le $ProcessFinished.UtcDateTime.AddSeconds(1)) 'Stale native terminal file.'
    if ($status -eq 'error') {
        foreach ($field in @('profileFile', 'manifestFile')) {
            if ($null -ne $terminal[$field]) { [void](Get-NativeBenchmarkFile $Directory $terminal[$field]) }
        }
        throw "Native startup/publication error ($($terminal['errorCode'])): $($terminal['errors'] -join '; ')"
    }
    Assert-Benchmark ($terminal['captureId'] -is [string] -and $terminal['captureId'] -cmatch '^[0-9A-Fa-f]{32}$') 'Missing native captureId.'
    Assert-Benchmark ($terminal['profileFile'] -cnotin @('result.json', 'owner.json') -and
        $terminal['manifestFile'] -cnotin @('result.json', 'owner.json') -and
        $terminal['profileFile'] -cne $terminal['manifestFile']) 'Native report names must identify distinct reports.'
    $profileFile = Get-NativeBenchmarkFile $Directory $terminal['profileFile']
    $manifestFile = Get-NativeBenchmarkFile $Directory $terminal['manifestFile']
    foreach ($file in @($profileFile, $manifestFile)) {
        Assert-Benchmark ($file.LastWriteTimeUtc -ge $ProcessStarted.UtcDateTime.AddSeconds(-1) -and
            $file.LastWriteTimeUtc -le $terminalFile.LastWriteTimeUtc) 'Native result.json was not published last or references stale reports.'
    }
    $profile = Read-BenchmarkDocument $profileFile.FullName
    $manifest = Read-BenchmarkDocument $manifestFile.FullName
    foreach ($document in @($profile, $manifest)) {
        Assert-NativeBenchmarkIdentity $document $Identity
        Assert-Benchmark ($document['captureId'] -is [string] -and
            $document['captureId'] -ieq $terminal['captureId']) 'Terminal/native captureId mismatch.'
    }
    if ($status -eq 'failed') {
        Assert-BenchmarkFlag $profile 'benchmarkPassed' $false
        Assert-BenchmarkFlag $manifest 'scenarioTargetMet' $false
        throw "Native benchmark failed ($($terminal['errorCode'])): $($terminal['errors'] -join '; ')"
    }
    Assert-Benchmark ($null -eq $terminal['errorCode'] -and $terminal['errors'].Count -eq 0) 'Native passed status contains errors.'
    return Test-BenchmarkReport $profileFile.FullName $manifestFile.FullName $Scenario $DurationSeconds $WarmupSeconds `
        $ProcessStarted $ProcessFinished $WindowMode -NativeIdentity $Identity
}

function Assert-MaximalDeferredSchedule {
    param([Collections.IDictionary]$Benchmark, [double]$DurationSeconds, [array]$Frames)
    Assert-BenchmarkNumber $Benchmark 'maximalSchedulePolicyVersion' 2
    $schedule = $Benchmark['maximalSchedule']
    Assert-Benchmark ($schedule -is [Collections.IDictionary]) 'Missing maximalSchedule evidence.'
    Assert-BenchmarkNumber $schedule 'periodSeconds' 0.5
    Assert-BenchmarkNumber $schedule 'maxAttemptsPerBoundary' 8
    $planned = [int][Math]::Ceiling($DurationSeconds * 2)
    foreach ($field in @('plannedOperations', 'attemptedOperations', 'acceptedOperations', 'cityAcceptedImpacts', 'effectsRequests')) {
        Assert-BenchmarkNumber $Benchmark $field $planned
    }
    foreach ($field in @('skippedOperations', 'rejectedOperations', 'weaponRejectedImpacts')) {
        Assert-BenchmarkNumber $Benchmark $field 0
    }
    Assert-BenchmarkNumber $schedule 'dueOperations' $planned
    Assert-BenchmarkNumber $schedule 'pendingOperations' 0
    Assert-Benchmark ($schedule['pendingSlots'] -is [array] -and $schedule['pendingSlots'].Count -eq 0) 'Maximal schedule has pending slots at capture end.'
    $backpressure = Get-BenchmarkNumber $schedule 'backpressureBoundaries'
    Assert-Benchmark ($backpressure -ge 0 -and $backpressure -eq [Math]::Truncate($backpressure) -and
        $backpressure -le $Frames.Count) 'Invalid maximal backpressure boundary count.'
    $events = $Benchmark['events']
    Assert-Benchmark ($events -is [array] -and $events.Count -eq $planned) 'Maximal schedule events must account for every planned slot.'
    $elapsed = 0.0; $processed = 0; $peakPending = 0; $peakBatch = 0; $lastAdmission = -1.0; $maxLateness = 0.0
    for ($boundary = 0; $boundary -le $Frames.Count; $boundary++) {
        $due = if ($elapsed -ge $DurationSeconds) { $planned }
            else { [Math]::Min($planned, [Math]::Floor($elapsed * 2) + 1) }
        $peakPending = [Math]::Max($peakPending, $due - $processed)
        $batch = 0
        while ($processed -lt $events.Count) {
            $event = $events[$processed]
            Assert-Benchmark ($event -is [Collections.IDictionary]) 'Malformed maximal schedule event.'
            $eventBoundary = Get-BenchmarkNumber $event 'boundaryIndex'
            Assert-Benchmark ($eventBoundary -ge $boundary -and $eventBoundary -lt $Frames.Count -and
                $eventBoundary -eq [Math]::Truncate($eventBoundary)) 'Maximal event boundary is out of order or outside measured frames.'
            if ($eventBoundary -ne $boundary) { break }
            Assert-Benchmark ($batch -lt 8) 'Maximal catch-up batch exceeded its finite per-boundary limit.'
            Assert-Benchmark ($processed -lt $due) 'Maximal schedule admitted a future slot before its absolute due boundary.'
            Assert-Benchmark ($event['result'] -ceq 'accepted') 'Maximal schedule contains an unaccepted request.'
            Assert-BenchmarkNumber $event 'slot' $processed
            Assert-BenchmarkNumber $event 'plannedSeconds' ($processed * 0.5)
            Assert-BenchmarkNumber $event 'batchIndex' $batch
            Assert-BenchmarkNumber $event 'pendingDueOperationsBeforeAttempt' ($due - $processed)
            $recordedBoundary = Get-BenchmarkNumber $event 'boundarySeconds'
            Assert-Benchmark ([Math]::Abs($recordedBoundary - $elapsed) -le 1e-6) 'Maximal schedule boundary does not match the raw frame trace.'
            $actual = Get-BenchmarkNumber $event 'actualSeconds'
            $admitted = Get-BenchmarkNumber $event 'admittedSeconds'
            Assert-Benchmark ($actual -ge $recordedBoundary -and $actual -ge $lastAdmission -and
                $admitted -ge $actual -and $admitted -lt $DurationSeconds -and
                $admitted -le $elapsed + [double]$Frames[$boundary] + 1e-6) 'Maximal admission timing is outside its measured frame or requested window.'
            $lateness = $admitted - $processed * 0.5
            Assert-BenchmarkNumber $event 'admissionLatenessSeconds' $lateness
            $maxLateness = [Math]::Max($maxLateness, $lateness)
            $lastAdmission = $admitted
            $processed++
            $batch++
        }
        $peakBatch = [Math]::Max($peakBatch, $batch)
        if ($boundary -lt $Frames.Count) { $elapsed += [double]$Frames[$boundary] }
    }
    Assert-Benchmark ($processed -eq $planned) 'Maximal schedule left unadmitted requests.'
    Assert-BenchmarkNumber $schedule 'peakPendingOperations' $peakPending
    Assert-BenchmarkNumber $schedule 'peakAttemptsPerBoundary' $peakBatch
    Assert-BenchmarkNumber $schedule 'peakAdmissionsPerBoundary' $peakBatch
    Assert-BenchmarkNumber $schedule 'lastAdmissionSeconds' $lastAdmission
    Assert-BenchmarkNumber $schedule 'maxAdmissionLatenessSeconds' $maxLateness
}

function Assert-MaximalSaturationSchedule {
    param([Collections.IDictionary]$Benchmark, [double]$DurationSeconds, [array]$Frames)
    Assert-BenchmarkNumber $Benchmark 'maximalSchedulePolicyVersion' 3
    $schedule = $Benchmark['maximalSchedule']
    Assert-Benchmark ($schedule -is [Collections.IDictionary]) 'Missing maximal saturation schedule evidence.'
    Assert-Benchmark ($schedule['releasePolicy'] -ceq 'allAtFirstMeasuredBoundary' -and
        !$schedule.Contains('periodSeconds') -and !$schedule.Contains('maxAdmissionLatenessSeconds')) 'V3 requires the explicit saturation release policy, not periodic V2 timing.'
    Assert-BenchmarkNumber $schedule 'releaseSeconds' 0
    Assert-BenchmarkNumber $schedule 'releaseBoundaryIndex' 0
    Assert-BenchmarkNumber $schedule 'maxAttemptsPerBoundary' 8
    $planned = [int][Math]::Ceiling($DurationSeconds * 2)
    foreach ($field in @('plannedOperations', 'attemptedOperations', 'acceptedOperations', 'cityAcceptedImpacts', 'effectsRequests')) {
        Assert-BenchmarkNumber $Benchmark $field $planned
    }
    foreach ($field in @('skippedOperations', 'rejectedOperations', 'weaponRejectedImpacts')) {
        Assert-BenchmarkNumber $Benchmark $field 0
    }
    foreach ($field in @('releasedOperations', 'dueOperations', 'peakPendingOperations')) {
        Assert-BenchmarkNumber $schedule $field $planned
    }
    Assert-BenchmarkNumber $schedule 'pendingOperations' 0
    Assert-Benchmark ($schedule['pendingSlots'] -is [array] -and $schedule['pendingSlots'].Count -eq 0) 'Maximal saturation has pending slots at capture end.'
    $backpressure = Get-BenchmarkNumber $schedule 'backpressureBoundaries'
    Assert-Benchmark ($backpressure -ge 0 -and $backpressure -eq [Math]::Truncate($backpressure) -and
        $backpressure -le $Frames.Count) 'Invalid maximal saturation backpressure boundary count.'
    $events = $Benchmark['events']
    Assert-Benchmark ($events -is [array] -and $events.Count -eq $planned) 'Maximal saturation events must account for every planned request.'
    $elapsed = 0.0; $processed = 0; $peakBatch = 0; $lastAdmission = -1.0; $maxDelay = 0.0
    for ($boundary = 0; $boundary -le $Frames.Count; $boundary++) {
        $batch = 0
        while ($processed -lt $events.Count) {
            $event = $events[$processed]
            Assert-Benchmark ($event -is [Collections.IDictionary]) 'Malformed maximal saturation event.'
            $eventBoundary = Get-BenchmarkNumber $event 'boundaryIndex'
            Assert-Benchmark ($eventBoundary -ge $boundary -and $eventBoundary -lt $Frames.Count -and
                $eventBoundary -eq [Math]::Truncate($eventBoundary)) 'Maximal saturation boundary is out of order or outside measured frames.'
            if ($eventBoundary -ne $boundary) { break }
            Assert-Benchmark ($batch -lt 8) 'Maximal saturation batch exceeded its finite per-boundary limit.'
            Assert-Benchmark ($event['result'] -ceq 'accepted') 'Maximal saturation contains an unaccepted request.'
            Assert-BenchmarkNumber $event 'slot' $processed
            Assert-BenchmarkNumber $event 'plannedSeconds' 0
            Assert-BenchmarkNumber $event 'releaseSeconds' 0
            Assert-BenchmarkNumber $event 'batchIndex' $batch
            Assert-BenchmarkNumber $event 'pendingDueOperationsBeforeAttempt' ($planned - $processed)
            Assert-Benchmark (!$event.Contains('admissionLatenessSeconds')) 'V3 admission telemetry must describe delay from release, not periodic lateness.'
            $recordedBoundary = Get-BenchmarkNumber $event 'boundarySeconds'
            Assert-Benchmark ([Math]::Abs($recordedBoundary - $elapsed) -le 1e-6) 'Maximal saturation boundary does not match the raw frame trace.'
            $actual = Get-BenchmarkNumber $event 'actualSeconds'
            $admitted = Get-BenchmarkNumber $event 'admittedSeconds'
            Assert-Benchmark ($actual -ge $recordedBoundary -and $actual -ge $lastAdmission -and
                $admitted -ge $actual -and $admitted -lt $DurationSeconds -and
                $admitted -le $elapsed + [double]$Frames[$boundary] + 1e-6) 'Maximal saturation admission timing is outside its measured frame or requested window.'
            Assert-BenchmarkNumber $event 'admissionDelaySeconds' $admitted
            $maxDelay = [Math]::Max($maxDelay, $admitted)
            $lastAdmission = $admitted
            $processed++
            $batch++
        }
        $peakBatch = [Math]::Max($peakBatch, $batch)
        if ($boundary -lt $Frames.Count) { $elapsed += [double]$Frames[$boundary] }
    }
    Assert-Benchmark ($processed -eq $planned) 'Maximal saturation left unadmitted requests.'
    Assert-BenchmarkNumber $schedule 'peakAttemptsPerBoundary' $peakBatch
    Assert-BenchmarkNumber $schedule 'peakAdmissionsPerBoundary' $peakBatch
    Assert-BenchmarkNumber $schedule 'lastAdmissionSeconds' $lastAdmission
    Assert-BenchmarkNumber $schedule 'maxAdmissionDelaySeconds' $maxDelay
}

function Test-BenchmarkReport {
    param(
        [string]$ProfilePath, [string]$ManifestPath, [string]$Scenario,
        [double]$DurationSeconds, [double]$WarmupSeconds,
        [DateTimeOffset]$ProcessStarted, [DateTimeOffset]$ProcessFinished,
        [ValidateSet('Windowed', 'Fullscreen')][string]$WindowMode = 'Windowed',
        [Collections.IDictionary]$NativeIdentity
    )
    $profile = Read-BenchmarkDocument $ProfilePath
    $manifest = Read-BenchmarkDocument $ManifestPath
    if ($NativeIdentity) {
        Assert-NativeBenchmarkIdentity $profile $NativeIdentity
        Assert-NativeBenchmarkIdentity $manifest $NativeIdentity
    }
    $target = if ($Scenario -eq 'maximal') { 30 } else { 60 }
    $performancePolicy = 'strict-frame-floor-v1'
    $frameTargetRequired = $true
    if ($profile.Contains('performancePolicy') -or $manifest.Contains('performancePolicy') -or
        $profile.Contains('frameTargetRequired') -or $manifest.Contains('frameTargetRequired')) {
        $performancePolicy = $profile['performancePolicy']
        Assert-Benchmark ($performancePolicy -is [string] -and
            $performancePolicy -cin @('strict-frame-floor-v1', 'maximal-spectacle-v1') -and
            $manifest['performancePolicy'] -ceq $performancePolicy) 'Unknown or mismatched performance policy.'
        $frameTargetRequired = $performancePolicy -cne 'maximal-spectacle-v1'
        Assert-Benchmark ($frameTargetRequired -or $Scenario -ceq 'maximal') 'Spectacle policy is only valid for maximal destruction.'
        foreach ($document in @($profile, $manifest)) {
            Assert-BenchmarkFlag $document 'frameTargetRequired' $frameTargetRequired
        }
    }
    Assert-Benchmark ($profile['schemaVersion'] -eq 1 -and $profile['engineVersion'] -ceq '5.8.2') 'Unsupported report schema/engine; expected schema 1, UE 5.8.2.'
    Assert-Benchmark ($profile['captureId'] -is [string] -and $profile['captureId'] -cmatch '^[0-9A-Fa-f]{32}$' -and
        $manifest['captureId'] -ceq $profile['captureId']) 'Profile/manifest captureId mismatch.'
    Assert-Benchmark ($manifest['profileFile'] -ceq [IO.Path]::GetFileName($ProfilePath) -and
        [IO.Path]::GetFileName($ManifestPath) -ceq ([IO.Path]::GetFileNameWithoutExtension($ProfilePath) + '-manifest.json')) 'Profile/manifest filename mismatch.'
    Assert-Benchmark ($profile['label'] -ceq $Scenario -and $manifest['scenario'] -ceq $Scenario) 'Wrong scenario.'
    Assert-Benchmark ($profile.Contains('error') -and $profile['error'] -eq $null -and
        !$profile.Contains('manifestError')) 'Native report contains an error or is missing its error field.'
    foreach ($document in @($profile, $manifest)) {
        Assert-Benchmark ($document['status'] -ceq 'completed' -and $document['completionReason'] -ceq 'durationReached') 'Incomplete or invalid native capture.'
        foreach ($field in @('completedRequestedDuration', 'benchmarkValid', 'workloadAdmitted')) {
            Assert-BenchmarkFlag $document $field
        }
        if ($frameTargetRequired) { Assert-BenchmarkFlag $document 'scenarioTargetMet' }
    }
    foreach ($field in @('benchmarkPassed', 'metricsAvailable', 'clockPrimed')) {
        Assert-BenchmarkFlag $profile $field
    }
    if ($frameTargetRequired) { Assert-BenchmarkFlag $profile 'frameTargetMet' }
    # Current native manifest has no benchmarkPassed field. Never invent that native field.
    $manifestVerdictSource = 'Derived from native completion/validity/admission/target fields; benchmarkPassed is not emitted.'
    if ($manifest.Contains('benchmarkPassed')) {
        Assert-BenchmarkFlag $manifest 'benchmarkPassed'
        $manifestVerdictSource = 'Native benchmarkPassed and completion/validity/admission/target fields.'
    }
    if (!$frameTargetRequired) {
        $manifestVerdictSource = 'Derived from native completion/validity/admission and explicit maximal-spectacle-v1 policy; frame-floor result is reported separately.'
    }
    Assert-BenchmarkNumber $profile 'requestedDurationSeconds' $DurationSeconds
    Assert-BenchmarkNumber $profile 'requestedWarmupSeconds' $WarmupSeconds
    Assert-BenchmarkNumber $profile 'scenarioTargetMinimumFPS' $target
    Assert-Benchmark ((Get-BenchmarkNumber $profile 'excludedWarmupSeconds') -ge $WarmupSeconds -and
        (Get-BenchmarkNumber $profile 'excludedWarmupFrames') -ge 1) 'The requested warmup was not observed.'
    Assert-Benchmark ((Get-BenchmarkNumber $profile 'thresholdToleranceMilliseconds') -eq 0.000001) 'Unexpected native frame tolerance.'
    Assert-BenchmarkNumber $profile 'frameBudgetMilliseconds' (1000.0 / 60)
    Assert-BenchmarkNumber $profile 'frameBudget30Milliseconds' (1000.0 / 30)
    $started = Get-BenchmarkTimestamp $profile 'startedUtc'
    $finished = Get-BenchmarkTimestamp $profile 'finishedUtc'
    Assert-Benchmark ($started -ge $ProcessStarted.AddMilliseconds(-1) -and $finished -ge $started -and
        $finished -le $ProcessFinished.AddMilliseconds(1)) 'Report is stale or outside the owned process lifetime.'

    $benchmark = $profile['benchmark']
    Assert-Benchmark ($benchmark -is [Collections.IDictionary]) 'Missing benchmark evidence.'
    foreach ($key in $benchmark.Keys) {
        Assert-Benchmark ($manifest.Contains($key) -and
            (Test-BenchmarkValueEqual $benchmark[$key] $manifest[$key])) "Manifest contradicts embedded benchmark: $key"
    }
    Assert-BenchmarkFlag $benchmark 'measurementStarted'
    Assert-Benchmark ($benchmark['invalidReasons'] -is [array] -and $benchmark['invalidReasons'].Count -eq 0) 'Native benchmark invalidReasons must be an empty array.'
    foreach ($field in @('weaponSettingsAtStart', 'weaponSettingsAtEnd')) {
        Assert-BenchmarkNumber $benchmark[$field] 'bombYieldTonsTNT' 1
        Assert-BenchmarkNumber $benchmark[$field] 'cannonRoundsPerSecond' 7
    }
    $settings = $benchmark['measurementStartSettings']
    $endSettings = $benchmark['measurementEndSettings']
    Assert-Benchmark (Test-BenchmarkValueEqual $settings $endSettings) 'Measured environment changed.'
    foreach ($state in @($settings, $endSettings)) {
        Assert-Benchmark ($state -is [Collections.IDictionary] -and $state['worldType'] -ceq 'Game') 'Not a cooked game-world benchmark.'
        foreach ($field in @('nullRHI', 'isPIE', 'isEditorProcess', 'isDedicatedServer', 'worldPaused', 'useFixedTimeStep', 'engineFixedFrameRate')) {
            Assert-BenchmarkFlag $state $field $false
        }
        Assert-BenchmarkFlag $state 'requiresCookedData'
        if ($NativeIdentity) {
            Assert-Benchmark ($state['buildConfiguration'] -ceq $NativeIdentity['configuration']) 'Measured build configuration contradicts native identity.'
        } else {
            Assert-Benchmark ($state['buildConfiguration'] -cin @('Development', 'Test', 'Debug', 'DebugGame')) 'Shipping requires validated native terminal identity, not the legacy route.'
        }
        Assert-BenchmarkNumber $state 'effectiveTimeDilation' 1
        foreach ($field in @('available', 'dimensionsAvailable', 'hasFocus', 'isForegroundWindow')) {
            Assert-BenchmarkFlag $state['viewport'] $field
        }
        Assert-Benchmark ($state['viewport']['windowMode'] -eq $WindowMode) "Actual presentation mode does not match requested $WindowMode."
        Assert-BenchmarkNumber $state['viewport'] 'width' 1920
        Assert-BenchmarkNumber $state['viewport'] 'height' 1080
        Assert-BenchmarkNumber $state['qualityAndTimingCVars'] 'r.DynamicRes.OperationMode' 0
    }
    if ($NativeIdentity) {
        foreach ($field in @('skippedOperations', 'rejectedOperations', 'weaponRejectedImpacts')) {
            Assert-Benchmark ((Get-BenchmarkNumber $benchmark $field) -eq 0) "Native workload has $field."
        }
        $nativeLimits = @(@('maxAwakeCollections', 24), @('maxAwakePieces', 1536),
            @('maxQueuedImpacts', 64), @('maxActiveImpactFX', 16))
        if ($benchmark.Contains('budgetPolicyVersion')) {
            Assert-BenchmarkNumber $benchmark 'budgetPolicyVersion' 2
            $declared = $benchmark['budgetLimits']
            Assert-Benchmark ($declared -is [Collections.IDictionary]) 'Missing native budgetLimits.'
            foreach ($limit in @(@('collections', 1044), @('pieces', 1048576), @('hullSlots', 8388608),
                @('queuedImpacts', 1024), @('pendingAssetLoads', 16), @('registrationsPerFrame', 8), @('impactFX', 16))) {
                Assert-BenchmarkNumber $declared $limit[0] $limit[1]
            }
            $nativeLimits = @(@('maxAwakeCollections', 1044), @('maxAwakePieces', 1048576),
                @('maxQueuedImpacts', 1024), @('maxActiveImpactFX', 16),
                @('maxAllocatedCollections', 1044), @('maxAllocatedPieces', 1048576),
                @('maxAllocatedHullSlots', 8388608), @('maxPendingAssetLoads', 16), @('maxRegistrationsPerFrame', 8))
        } else {
            Assert-Benchmark (!$benchmark.Contains('budgetLimits')) 'Native budget limits require an explicit policy version.'
        }
        foreach ($limit in $nativeLimits) {
            $value = Get-BenchmarkNumber $benchmark $limit[0]
            Assert-Benchmark ($value -eq [Math]::Truncate($value) -and $value -ge 0 -and $value -le $limit[1]) "Native budget exceeded: $($limit[0])"
        }
        $cityImpacts = Get-BenchmarkNumber $benchmark 'cityAcceptedImpacts'
        if ($Scenario -eq 'maximal') {
            $planned = [Math]::Ceiling($DurationSeconds * 2)
            Assert-Benchmark ((Get-BenchmarkNumber $benchmark 'acceptedOperations') -eq $planned -and
                (Get-BenchmarkNumber $benchmark 'plannedOperations') -eq $planned -and $cityImpacts -eq $planned -and
                (Get-BenchmarkNumber $benchmark 'fracturedBuildings') -gt 0 -and
                (Get-BenchmarkNumber $benchmark 'maxMovedFragments') -gt 0 -and
                (Get-BenchmarkNumber $benchmark 'maxActiveImpactFX') -gt 0) 'Native maximal workload/physical evidence is incomplete.'
        } else {
            Assert-Benchmark ((Get-BenchmarkNumber $benchmark 'flightDistanceCm') -ge 4000 * $DurationSeconds * 0.9) 'Native flight distance is incomplete.'
            $shots = 0; $bombs = 0
            if ($Scenario -eq 'light') {
                for ($burst = 0; $burst -lt $DurationSeconds; $burst += 5) {
                    for ($shot = 0; $shot -lt 14; $shot++) { if ($burst + $shot / 7.0 -lt $DurationSeconds) { $shots++ } }
                }
                $bombs = [int]($DurationSeconds -gt 5) + [int]($DurationSeconds -gt 20)
            }
            $impacts = Get-BenchmarkNumber $benchmark 'weaponAcceptedImpacts'
            Assert-Benchmark ((Get-BenchmarkNumber $benchmark 'cannonShots') -eq $shots -and
                (Get-BenchmarkNumber $benchmark 'bombsDropped') -eq $bombs -and $cityImpacts -eq $impacts -and
                (($Scenario -eq 'normal' -and $impacts -eq 0) -or ($Scenario -eq 'light' -and $impacts -gt 0 -and
                (Get-BenchmarkNumber $benchmark 'effectsRequests') -eq $impacts))) 'Native flight/weapon workload evidence is incomplete.'
        }
    }

    $frames = $profile['frameTimesSeconds']
    Assert-Benchmark ($frames -is [array] -and $frames.Count -gt 0 -and $frames.Count -le 120000) 'Missing/invalid raw frame trace.'
    $sum = 0.0; $correction = 0.0; $worst = 0.0
    $over60 = 0; $over30 = 0; $over60Tolerance = 0; $over30Tolerance = 0
    foreach ($frame in $frames) {
        $seconds = Get-BenchmarkNumber @{ frame = $frame } 'frame'
        Assert-Benchmark ($seconds -gt 0 -and [double]::IsFinite(1.0 / $seconds) -and
            [double]::IsFinite($seconds * 1000)) 'Invalid raw frame interval.'
        $adjusted = $seconds - $correction
        $next = $sum + $adjusted
        $correction = ($next - $sum) - $adjusted
        $sum = $next
        $worst = [Math]::Max($worst, $seconds)
        if ($seconds -gt 1.0 / 60) { $over60++ }
        if ($seconds -gt 1.0 / 30) { $over30++ }
        if ($seconds -gt 1.0 / 60 + 1e-9) { $over60Tolerance++ }
        if ($seconds -gt 1.0 / 30 + 1e-9) { $over30Tolerance++ }
    }
    $frameTargetMet = ($target -eq 60 -and $over60Tolerance -eq 0) -or ($target -eq 30 -and $over30Tolerance -eq 0)
    Assert-Benchmark ($sum -ge $DurationSeconds -and (!$frameTargetRequired -or $frameTargetMet)) 'Raw frames fail the full-duration minimum-FPS gate.'
    Assert-BenchmarkFlag $profile 'frameTargetMet' $frameTargetMet
    foreach ($document in @($profile, $manifest)) {
        Assert-BenchmarkFlag $document 'scenarioTargetMet' $frameTargetMet
    }
    if ($manifest.Contains('frameTargetMet')) { Assert-BenchmarkFlag $manifest 'frameTargetMet' $frameTargetMet }
    Assert-BenchmarkNumber $profile 'durationSeconds' $sum
    Assert-BenchmarkNumber $benchmark 'measuredWallSeconds' $sum
    foreach ($document in @($profile, $manifest)) {
        Assert-Benchmark ((Get-BenchmarkNumber $document 'frameCount') -eq $frames.Count) 'Raw frameCount mismatch.'
        Assert-BenchmarkNumber $document 'minFPS' (1.0 / $worst)
        Assert-BenchmarkNumber $document 'averageFPS' ($frames.Count / $sum)
        Assert-BenchmarkNumber $document 'worstFrameMilliseconds' ($worst * 1000)
        Assert-Benchmark ((Get-BenchmarkNumber $document 'framesOver60FPSBudget') -eq $over60 -and
            (Get-BenchmarkNumber $document 'framesOver30FPSBudget') -eq $over30) 'Strict frame-budget count mismatch.'
    }
    Assert-Benchmark ((Get-BenchmarkNumber $profile 'framesOver60FPSBudgetWithTolerance') -eq $over60Tolerance -and
        (Get-BenchmarkNumber $profile 'framesOver30FPSBudgetWithTolerance') -eq $over30Tolerance) 'Tolerant frame-budget count mismatch.'
    Assert-BenchmarkFlag $profile 'allFramesAtLeast60' ($over60Tolerance -eq 0)
    Assert-BenchmarkFlag $profile 'allFramesAtLeast30' ($over30Tolerance -eq 0)
    $newScheduleEvents = @($benchmark['events'] | Where-Object {
        $_ -is [Collections.IDictionary] -and ($_.Contains('boundaryIndex') -or $_.Contains('admittedSeconds'))
    })
    $hasSchedulePolicy = $benchmark.Contains('maximalSchedulePolicyVersion')
    Assert-Benchmark ($hasSchedulePolicy -eq $manifest.Contains('maximalSchedulePolicyVersion') -and
        $benchmark.Contains('maximalSchedule') -eq $manifest.Contains('maximalSchedule')) 'Maximal schedule policy must match both reports.'
    if ($hasSchedulePolicy -or $benchmark.Contains('maximalSchedule') -or $newScheduleEvents.Count) {
        Assert-Benchmark ($hasSchedulePolicy) 'Deferred maximal schedule requires an explicit policy version.'
        Assert-Benchmark ($Scenario -ceq 'maximal' -and $performancePolicy -ceq 'maximal-spectacle-v1') 'Deferred schedule is only valid for explicit maximal spectacle.'
        switch (Get-BenchmarkNumber $benchmark 'maximalSchedulePolicyVersion') {
            2 { Assert-MaximalDeferredSchedule $benchmark $DurationSeconds $frames }
            3 { Assert-MaximalSaturationSchedule $benchmark $DurationSeconds $frames }
            default { throw [IO.InvalidDataException]::new('Unsupported maximalSchedulePolicyVersion.') }
        }
    }
    return [ordered]@{
        scenario = $Scenario; passed = $true; captureId = $profile['captureId']
        performancePolicy = $performancePolicy; frameTargetRequired = $frameTargetRequired
        frameTargetMet = $frameTargetMet
        requestedWindowMode = $WindowMode
        profileBenchmarkPassed = $true; manifestBenchmarkPassed = $true
        manifestVerdictSource = $manifestVerdictSource
        frameCount = $frames.Count; durationSeconds = $sum; targetMinimumFPS = $target
        minFPS = $profile['minFPS']; averageFPS = $profile['averageFPS']
        worstFrameMilliseconds = $profile['worstFrameMilliseconds']
        framesOver60FPSBudget = $over60; framesOver30FPSBudget = $over30
        qualityAudit = $settings
        mouseCaptureEvidence = 'Native benchmarkValid plus empty invalidReasons; no separate capture snapshot is emitted.'
        measurementScope = 'Engine wall-frame cadence, not presentation FPS or GPU attribution.'
    }
}
