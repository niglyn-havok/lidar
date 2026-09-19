#requires -Version 7.0
<#
.SYNOPSIS
Runs fresh cooked UE 5.8.2 game processes and verifies both durable benchmark reports.
.DESCRIPTION
Defaults: normal/light/maximal, 30s measured, 5s warmup. Development keeps its legacy
console route. Shipping requires BuildReceiptPath and uses only the startup contract;
no Exec, TestExit, logging, trace or network support is enabled in Shipping.
WindowMode defaults to Windowed. Fullscreen uses supported startup -Res=1920x1080f,
not desktop-sized borderless mode; actual report mode and 1920x1080 size must match.
Native startup must acquire a visible, focused, mouse-captured 1920x1080 client. This
script never sets focus/capture, changes quality/pacing, or retries invalid runs.
The game uses normal player startup, not UE -unattended mode. The runner adds no
input prompts, never answers native dialogs, and fails startup stalls at its timeout.
An accessible desktop is required. If it is locked/disconnected, a human must restore
access; foreground=false alone does not establish that cause. No authentication or
operating-system safety prompts are handled by the runner.
Do not use the desktop or run builds/cooks during measurement. Failed reports and
raw frames are retained. The current manifest verdict is derived from its explicit
completion/validity/admission/target fields because it has no benchmarkPassed field.
Reports are located beside the staged project or in its LocalApplicationData folder;
PerformanceDirectory adds an explicit lookup location without changing game settings.
Timeout defaults to duration + warmup + 180s, plus bounded shutdown/log-drain time.
Shipping requires schema-v2 result.json published last and matching run/PID/config/
capture identity. OS exit 0 is transport only: status passed, outcomeCode 0 and all
native gates are required. The driver returns nonzero for every failed/error outcome.
The driver hashes the archive before/after each run; no caller cohort GUID.
.EXAMPLE
pwsh -File .\Scripts\Run-Benchmarks.ps1 -PackagedExe 'D:\Package\DublinFlight.exe'
.EXAMPLE
pwsh -File .\Scripts\Run-Benchmarks.ps1 -PackagedExe 'D:\Package\DublinFlight.exe' -Scenario normal
.EXAMPLE
pwsh -File .\Scripts\Run-Benchmarks.ps1 -PackagedExe 'D:\Package\DublinFlight.exe' -Scenario normal -WindowMode Fullscreen
.EXAMPLE
pwsh -File .\Scripts\Run-Benchmarks.ps1 -PackagedExe 'D:\Shipping\Windows\DublinFlight.exe' -BuildReceiptPath '.\Binaries\Win64\DublinFlight-Win64-Shipping.target'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackagedExe,
    [string]$BuildReceiptPath,
    [ValidateSet('normal', 'light', 'maximal')][string[]]$Scenario = @('normal', 'light', 'maximal'),
    [ValidateSet('Windowed', 'Fullscreen')][string]$WindowMode = 'Windowed',
    [ValidateRange(30, 300)][double]$DurationSeconds = 30,
    [ValidateRange(5, 60)][double]$WarmupSeconds = 5,
    [ValidateRange(0, 1800)][int]$TimeoutSeconds = 0,
    [string]$OutputDirectory,
    [string]$PerformanceDirectory
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchmarkReport.ps1')

function Save-RunnerJson {
    param($Value, [string]$Path)
    $bytes = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $Value -Depth 100))
    $file = [IO.File]::Create($Path)
    try { $file.Write($bytes); $file.Flush($true) } finally { $file.Dispose() }
}

try {
    $exe = Get-BenchmarkExecutable $PackagedExe $BuildReceiptPath
    $shipping = $exe.BaseName -match '-Win64-Shipping$'
    if ($shipping) { Assert-Benchmark (!$PerformanceDirectory) 'Shipping uses its fresh native output directory, not PerformanceDirectory lookup.' }
    $projectDirectory = $exe.Directory.Parent.Parent.FullName
    $projectName = $exe.Directory.Parent.Parent.Name
    if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\Saved\Automation\Benchmarks' }
    $root = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ((Get-Date -Format 'yyyyMMddTHHmmss') + '-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $performanceDirectories = @(
        (Join-Path $projectDirectory 'Saved\Performance'),
        (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "$projectName\Saved\Performance")
    )
    if ($PerformanceDirectory) { $performanceDirectories += [IO.Path]::GetFullPath($PerformanceDirectory) }
    $performanceDirectories = @($performanceDirectories | Sort-Object -Unique)
    if ($TimeoutSeconds -eq 0) { $TimeoutSeconds = [int][Math]::Ceiling($DurationSeconds + $WarmupSeconds + 180) }
    Assert-Benchmark ($TimeoutSeconds -gt $DurationSeconds + $WarmupSeconds) 'Timeout must exceed duration plus warmup.'
    Assert-Benchmark ($Scenario.Count -gt 0 -and @($Scenario | Sort-Object -Unique).Count -eq $Scenario.Count) 'Select each scenario at most once.'
    $results = @()
    $cohort = if ($shipping) { Get-BenchmarkCohort $exe $BuildReceiptPath $root } else { $null }
    if ($cohort) { Save-RunnerJson $cohort (Join-Path $root 'cohort.json') }
    Save-RunnerJson @{ requestedScenarios = $Scenario; complete = $false; passed = $false; runs = @(); cohort = $cohort } (Join-Path $root 'summary.json')
    Write-Host "Evidence: $root"
    Write-Host 'Native startup must own foreground focus and mouse capture. No focus/quality/timing overrides or automatic retries are used.'

    foreach ($name in $Scenario) {
        if ($shipping) {
            $before = Get-BenchmarkCohort $exe $BuildReceiptPath $root
            Assert-Benchmark ($before['cohortSHA256'] -ceq $cohort['cohortSHA256'] -and
                $before['receiptSHA256'] -ceq $cohort['receiptSHA256']) 'Shipping cohort changed before launch; no further game process will start.'
        }
        $run = Join-Path $root $name
        [IO.Directory]::CreateDirectory($run) | Out-Null
        $logPath = Join-Path $run 'engine.log'
        $stdoutPath = Join-Path $run 'stdout.log'
        $nativeDirectory = $null; $runId = $null
        if ($shipping) {
            $runId = [guid]::NewGuid().ToString('N')
            $nativeDirectory = Join-Path $run 'native'
            Assert-Benchmark (!(Test-Path -LiteralPath $nativeDirectory)) 'Refusing native output-directory reuse.'
            [IO.Directory]::CreateDirectory($nativeDirectory) | Out-Null
            $arguments = @(New-BenchmarkArguments $name $DurationSeconds $WarmupSeconds $logPath $WindowMode `
                -NativeRunId $runId -NativeOutputDirectory $nativeDirectory)
        } else {
            $arguments = @(New-BenchmarkArguments $name $DurationSeconds $WarmupSeconds $logPath $WindowMode)
        }
        $record = [ordered]@{ scenario = $name; passed = $false; requestedWindowMode = $WindowMode; executable = $exe.FullName; arguments = $arguments; timeoutSeconds = $TimeoutSeconds }
        if ($shipping) {
            $record['entryPoint'] = 'startup-contract-v2'
            $record['runId'] = $runId
            $record['nativeOutputDirectory'] = $nativeDirectory
            $record['configuration'] = 'Shipping'
            $record['cohortSHA256'] = $cohort['cohortSHA256']
        }
        Save-RunnerJson $record (Join-Path $run 'launch.json')
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = [Diagnostics.ProcessStartInfo]::new()
        $process.StartInfo.FileName = $exe.FullName
        $process.StartInfo.WorkingDirectory = $projectDirectory
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        foreach ($argument in $arguments) { $process.StartInfo.ArgumentList.Add($argument) }
        $stdout = [IO.File]::Create($stdoutPath)
        $stderr = [IO.File]::Create((Join-Path $run 'stderr.log'))
        $started = [DateTimeOffset]::UtcNow
        $timedOut = $false
        $launched = $false
        $outTask = $null; $errTask = $null
        try {
            $launched = $process.Start()
            Assert-Benchmark $launched 'Could not launch the cooked game.'
            $outTask = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
            $errTask = $process.StandardError.BaseStream.CopyToAsync($stderr)
            $record['pid'] = $process.Id
            $record['processStarted'] = $started.ToString('o')
            Save-RunnerJson $record (Join-Path $run 'launch.json')
            $timedOut = !$process.WaitForExit($TimeoutSeconds * 1000)
        } finally {
            if ($launched -and !$process.HasExited) {
                # Preserve captured logs and the owned PID before any forced termination.
                try {
                    $stdout.Flush($true); $stderr.Flush($true)
                    Save-RunnerJson @{ pid = $process.Id; timeout = $timedOut; reason = 'Owned process exceeded deadline or runner failed.' } (Join-Path $run 'termination.json')
                } catch {
                    throw "Could not preserve termination evidence; owned PID $($process.Id) was NOT stopped: $($_.Exception.Message)"
                }
                Stop-Process -Id $process.Id -Force -ErrorAction Stop
                Assert-Benchmark ($process.WaitForExit(10000)) "Owned PID $($process.Id) did not exit within the shutdown deadline."
            }
            try {
                $drains = @($outTask, $errTask | Where-Object { $null -ne $_ })
                if ($drains.Count -gt 0 -and ![Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]$drains, 10000)) {
                    $process.StandardOutput.Close(); $process.StandardError.Close()
                    throw 'Timed out draining process logs; captured files are retained, and this run is not accepted.'
                }
            } finally {
                $stdout.Flush($true); $stderr.Flush($true)
                $stdout.Dispose(); $stderr.Dispose()
            }
        }
        $finished = [DateTimeOffset]::UtcNow
        $record['processFinished'] = $finished.ToString('o')
        $record['engineExitCode'] = $process.ExitCode
        $record['timedOut'] = $timedOut
        $process.Dispose()
        try {
            if ($shipping) {
                $after = Get-BenchmarkCohort $exe $BuildReceiptPath $root
                Save-RunnerJson $after (Join-Path $run 'cohort-after.json')
                $record['cohortUnchanged'] = $after['cohortSHA256'] -ceq $cohort['cohortSHA256'] -and
                    $after['receiptSHA256'] -ceq $cohort['receiptSHA256']
                $terminalPath = Join-Path $nativeDirectory 'result.json'
                if (Test-Path -LiteralPath $terminalPath) {
                    $terminalFile = Get-NativeBenchmarkFile $nativeDirectory 'result.json'
                    $record['nativeResult'] = Read-BenchmarkDocument $terminalFile.FullName
                }
                Assert-Benchmark (!$timedOut) "Shipping timed out after ${TimeoutSeconds}s; native output and captured streams are retained."
                Assert-Benchmark $record['cohortUnchanged'] 'Shipping executable/content/receipt cohort changed during the run.'
                $identity = @{ runId = $runId; processId = $record['pid']; configuration = 'Shipping' }
                $record['verification'] = Test-NativeBenchmarkResult $nativeDirectory $identity $record['engineExitCode'] `
                    $name $DurationSeconds $WarmupSeconds $started $finished $WindowMode
                $record['passed'] = $true
            } else {
                $logText = [IO.File]::ReadAllText($stdoutPath)
                if (Test-Path -LiteralPath $logPath) { $logText += "`n" + [IO.File]::ReadAllText($logPath) }
                $profileNames = @(Get-BenchmarkReportNames $logText)
                # Preserve complete AND partial reports named by this process, even on failure.
                Copy-BenchmarkArtifacts $profileNames $performanceDirectories $run
                $record['artifacts'] = @(Get-ChildItem -LiteralPath $run -File -Filter '*.json*' |
                    Where-Object { $_.Name -match '^(normal|light|maximal)-' } |
                    ForEach-Object { @{ file = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
                if ($profileNames.Count -eq 1) {
                    $profilePath = Join-Path $run $profileNames[0]
                    if (Test-Path -LiteralPath $profilePath) {
                        $native = Read-BenchmarkDocument $profilePath
                        $record['nativeStatus'] = $native['status']
                        $record['nativeBenchmarkPassed'] = $native['benchmarkPassed']
                        if ($native['benchmark'] -is [Collections.IDictionary]) {
                            $record['nativeInvalidReasons'] = $native['benchmark']['invalidReasons']
                            $record['qualityAudit'] = $native['benchmark']['measurementStartSettings']
                        }
                    }
                }
                Assert-Benchmark (!$timedOut) "Timed out after ${TimeoutSeconds}s; only the owned PID was stopped. Inspect captured logs."
                Assert-Benchmark ($record['engineExitCode'] -eq 0) "Game exited with code $($record['engineExitCode'])."
                Assert-Benchmark ($profileNames.Count -eq 1) 'Expected exactly one process-owned report pair; inspect logs for unavailable world, Shipping build or startup failure.'
                Assert-Benchmark ($logText -match 'LogDublinFlightPerformance:\s+Display:\s+Benchmark profile published:') 'No final profile publication confirmation.'
                $profilePath = Join-Path $run $profileNames[0]
                $manifestPath = $profilePath -replace '\.json$', '-manifest.json'
                $verification = Test-BenchmarkReport $profilePath $manifestPath $name $DurationSeconds $WarmupSeconds $started $finished $WindowMode
                $record['verification'] = $verification
                $record['profileSHA256'] = (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash
                $record['manifestSHA256'] = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
                $record['passed'] = $true
            }
        } catch {
            $record['passed'] = $false
            $record['error'] = $_.Exception.Message
            $record['setupRequirement'] = 'Native foreground/focus/capture is required. If the desktop is locked/disconnected, a human must restore access; foreground=false alone does not prove this. The runner never activates windows, answers dialogs/authentication/safety prompts, or manufactures validity.'
            Write-Warning "$name FAILED: $($_.Exception.Message)"
            if ($record.Contains('nativeInvalidReasons') -and $record['nativeInvalidReasons']) {
                Write-Warning ("Native invalid reasons: " + ($record['nativeInvalidReasons'] -join '; '))
            }
            Write-Warning $record['setupRequirement']
        }
        Save-RunnerJson $record (Join-Path $run 'verification.json')
        $results += $record
        $complete = $results.Count -eq $Scenario.Count
        Save-RunnerJson @{ requestedScenarios = $Scenario; complete = $complete; runs = $results; cohort = $cohort;
            passed = $complete -and @($results | Where-Object { !$_.passed }).Count -eq 0 } (Join-Path $root 'summary.json')
        Write-Host "$name`: passed=$($record['passed']); evidence=$run"
    }
    if (@($results | Where-Object { !$_.passed }).Count -gt 0) { exit 1 }
} catch {
    if (Test-Path variable:root) {
        Save-RunnerJson @{ requestedScenarios = $Scenario; complete = $false; passed = $false;
            error = $_.Exception.Message } (Join-Path $root 'runner-error.json')
    }
    Write-Error -ErrorAction Continue $_
    exit 1
}
