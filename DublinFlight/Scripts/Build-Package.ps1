#requires -Version 7.0
<#
.SYNOPSIS
Builds the Editor and Game targets, then cooks and archives Win64.
.DESCRIPTION
EngineRoot defaults to UE_ENGINE_ROOT. Relative archive paths are project-relative.
The default UAT archive root is Saved\Builds\Windows; UE may add a Windows payload
directory. A successful build returns the actual ArchiveDirectory and GameExecutable.
DryRun returns the invocation without starting processes or writing files; its
resolved output fields remain null until UAT completes. Close the editor first.
Configuration defaults to Development. Shipping stages prerequisites without
debug symbols; it retains the engine's normal redistribution checks and defaults.
#>
[CmdletBinding()]
param(
    [string]$EngineRoot = $env:UE_ENGINE_ROOT,
    [string]$ArchiveDirectory = 'Saved\Builds\Windows',
    [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
    [ValidateRange(1, 3)][int]$MaxParallelActions = 3,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if (!$IsWindows) { throw 'This runner builds Win64 on Windows.' }

$projectRoot = Split-Path -Parent $PSScriptRoot
$projects = @(Get-ChildItem -LiteralPath $projectRoot -Filter '*.uproject' -File)
if ($projects.Count -ne 1) { throw "Expected exactly one .uproject in $projectRoot." }
$project = $projects[0]
if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    throw 'Supply -EngineRoot or set UE_ENGINE_ROOT to the installed UE 5.8 root.'
}
$engine = (Resolve-Path -LiteralPath $EngineRoot).Path
$versionPath = Join-Path $engine 'Engine\Build\Build.version'
$version = Get-Content -LiteralPath $versionPath -Raw | ConvertFrom-Json
if ($version.MajorVersion -ne 5 -or $version.MinorVersion -ne 8) {
    throw "Expected UE 5.8; found $($version.MajorVersion).$($version.MinorVersion)."
}
if ($version.PatchVersion -ne 2) {
    Write-Warning "This project was validated with UE 5.8.2, not 5.8.$($version.PatchVersion)."
}
$uat = (Get-Item -LiteralPath (Join-Path $engine 'Engine\Build\BatchFiles\RunUAT.bat')).FullName
if ([string]::IsNullOrWhiteSpace($ArchiveDirectory)) { throw 'ArchiveDirectory must not be empty.' }
$archive = [IO.Path]::GetFullPath($ArchiveDirectory, $projectRoot)
$runId = (Get-Date -Format 'yyyyMMddTHHmmss') + '-' + [guid]::NewGuid().ToString('N')
$logDirectory = Join-Path $projectRoot "Saved\Packaging\$runId"
$arguments = @(
    'BuildCookRun', "-project=$($project.FullName)", '-nop4',
    '-platform=Win64', "-clientconfig=$Configuration",
    '-build', '-cook', '-map=/Game/Maps/Dublin', '-stage', '-pak', '-archive',
    "-archivedirectory=$archive", '-unattended', '-utf8output', '-noxge',
    "-ubtargs=-MaxParallelActions=$MaxParallelActions"
)
if ($Configuration -eq 'Shipping') { $arguments += @('-prereqs', '-nodebuginfo') }
$plan = [pscustomobject]@{
    Configuration = $Configuration
    Executable = $uat
    Arguments = $arguments
    WorkingDirectory = $projectRoot
    ArchiveRootDirectory = $archive
    ArchiveDirectory = $null
    GameExecutable = $null
    LogFile = Join-Path $logDirectory 'BuildCookRun.log'
}
if ($DryRun) { return $plan }

$runningEditors = @(Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe'" |
    Where-Object {
        $_.CommandLine -and
        $_.CommandLine.IndexOf($project.FullName, [StringComparison]::OrdinalIgnoreCase) -ge 0
    })
if ($runningEditors.Count) {
    throw "Close this project's editor before building (PID $($runningEditors.ProcessId -join ', ')). No process was stopped."
}
[IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$plan | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logDirectory 'invocation.json') -Encoding utf8
Write-Host "Build log: $($plan.LogFile)"
Push-Location -LiteralPath $projectRoot
try {
    & $uat @arguments 2>&1 | Tee-Object -FilePath $plan.LogFile | Out-Host
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($exitCode -ne 0) {
    throw "BuildCookRun failed with exit code $exitCode. Log: $($plan.LogFile)"
}
try {
    $game = & (Join-Path $PSScriptRoot 'Launch-Game.ps1') -ArchiveDirectory $archive -Configuration $Configuration -DryRun
} catch {
    throw "BuildCookRun returned success but archive validation failed: $($_.Exception.Message) Log: $($plan.LogFile)"
}
$plan.ArchiveDirectory = $game.ArchiveDirectory
$plan.GameExecutable = $game.Executable
Write-Host "Archive payload: $($plan.ArchiveDirectory)"
Write-Host "Archived game: $($plan.GameExecutable)"
return $plan
