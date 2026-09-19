#requires -Version 7.0
<#
.SYNOPSIS
Launches the archived game without changing focus, quality or performance settings.
.DESCRIPTION
Defaults to Saved\Builds\Windows and a 1920x1080 window. Relative archive paths are
project-relative. Accepts a UAT archive root, its Windows payload directory, or a
legacy flat archive. An existing Windows child takes precedence over a flat game;
an incomplete child is an error, not permission to launch a stale flat build.
Configuration defaults to Development; Shipping selects its real game executable.
DryRun returns the resolved ArchiveDirectory and invocation without launching.
#>
[CmdletBinding()]
param(
    [string]$ArchiveDirectory = 'Saved\Builds\Windows',
    [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
    [ValidateRange(320, 16384)][int]$Width = 1920,
    [ValidateRange(240, 16384)][int]$Height = 1080,
    [switch]$Fullscreen,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$IsWindows) { throw 'This launcher runs the Win64 archive on Windows.' }
$projectRoot = Split-Path -Parent $PSScriptRoot
$projects = @(Get-ChildItem -LiteralPath $projectRoot -Filter '*.uproject' -File)
if ($projects.Count -ne 1) { throw "Expected exactly one .uproject in $projectRoot." }
if ([string]::IsNullOrWhiteSpace($ArchiveDirectory)) { throw 'ArchiveDirectory must not be empty.' }
$archiveRoot = [IO.Path]::GetFullPath($ArchiveDirectory, $projectRoot)
$platformArchive = Join-Path $archiveRoot 'Windows'
$archive = if (Test-Path -LiteralPath $platformArchive -PathType Container) { $platformArchive } else { $archiveRoot }
$name = $projects[0].BaseName
$workingDirectory = Join-Path $archive $name
$exeName = if ($Configuration -eq 'Shipping') { "$name-Win64-Shipping.exe" } else { "$name.exe" }
$executable = Join-Path $workingDirectory "Binaries\Win64\$exeName"
if (!(Test-Path -LiteralPath $executable -PathType Leaf) -or (Get-Item -LiteralPath $executable).Length -eq 0) {
    throw "Archived $Configuration game is missing or empty: $executable. Requested archive: $archiveRoot. Run Build-Package.ps1 or supply the correct -ArchiveDirectory and -Configuration."
}
$arguments = @('/Game/Maps/Dublin')
$arguments += if ($Fullscreen) { @('-fullscreen', "-Res=${Width}x${Height}f") } else { @('-windowed', "-ResX=$Width", "-ResY=$Height") }
$plan = [pscustomobject]@{
    Configuration = $Configuration
    ArchiveDirectory = $archive
    Executable = $executable
    Arguments = $arguments
    WorkingDirectory = $workingDirectory
}
if ($DryRun) { return $plan }

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $executable
$startInfo.WorkingDirectory = $workingDirectory
$startInfo.UseShellExecute = $false
foreach ($argument in $arguments) { $startInfo.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::Start($startInfo)
if (!$process) { throw "Could not launch $executable." }
try {
    Write-Host "Game started (PID $($process.Id)): $executable"
} finally {
    $process.Dispose()
}
