#requires -Version 7.0
<#
.SYNOPSIS
Assembles an already verified UE 5.8.2 Win64 archive; never builds or runs the game.
.DESCRIPTION
ArchiveDirectory, BuildReceiptPath and OutputDirectory are explicit, project-relative
unless absolute. Version must be a unique filename-safe release identifier.
ValidateOnly checks inputs without publishing a package. ArchiveVerified is the
caller's confirmation that the exact archive passed final gameplay/visual checks;
this script checks packaging integrity, not gameplay, performance or legal clearance.
Development requires AllowDevelopment. Every configuration needs its own checks.
Shipping assembly also requires VerifiedFullscreen1080p, the caller's confirmation
that this exact archive passed its final 1920x1080 Fullscreen presentation checks.
Only then is a deterministic Play.cmd included; ValidateOnly publishes nothing.
Outputs one versioned directory containing a ZIP, its SHA256 and manifest.json.
Publish/copy that whole directory only after this command succeeds.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArchiveDirectory,
    [Parameter(Mandatory)][string]$BuildReceiptPath,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$Version,
    [string]$EngineRoot = $env:UE_ENGINE_ROOT,
    [switch]$AllowDevelopment,
    [switch]$ArchiveVerified,
    [switch]$VerifiedFullscreen1080p,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if (!$IsWindows) { throw 'This packager verifies Win64 archives on Windows.' }
$projectRoot = Split-Path -Parent $PSScriptRoot

function Get-FullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Required path is empty.' }
    return [IO.Path]::GetFullPath($Path, $projectRoot)
}

function Get-RelativeName([string]$Path) {
    $name = $Path.Replace('/', '\')
    if ([IO.Path]::IsPathRooted($name) -or $name -match '[:\x00-\x1f]' -or
        @($name.Split('\') | Where-Object { $_ -in @('', '.', '..') -or $_ -match '[. ]$' }).Count) {
        throw "Unsafe archive-relative path: $Path"
    }
    return $name
}

function Assert-File([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "Required file is missing or empty: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    while ($item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Reparse points are not accepted as release inputs: $($item.FullName)"
        }
        $item = if ($item -is [IO.FileInfo]) { $item.Directory } else { $item.Parent }
    }
}

function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Assert-NoSecret([string]$Path, [string]$Label) {
    $text = [IO.File]::ReadAllText($Path)
    $keys = '(AccessToken|RefreshToken|ClientSecret|ApiKey|ApiToken|SecurityToken|AuthToken|Password|EncryptionKey|SigningPrivateExponent)'
    if ($text -match "(?im)^[ `t]*$keys[ `t]*=[ `t]*(?![ `t]*(?:`"`"|None)?[ `t]*$)\S[^\r\n]*" -or
        $text -match "(?i)`"$keys`"\s*:\s*`"(?!`")") {
        throw "Possible secret in $Label, key $($Matches[1]). Remove/denylist it and recook; values are deliberately not printed."
    }
}

function Read-StagingManifest([string]$Path) {
    Assert-File $Path
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $fields = $line.Split("`t")
        if ($fields.Count -ne 2 -or [string]::IsNullOrWhiteSpace($fields[1])) {
            throw "Invalid UAT manifest line in $Path (expected relative path TAB timestamp)."
        }
        $name = Get-RelativeName $fields[0]
        if (!$names.Add($name)) { throw "Duplicate UAT manifest entry: $name" }
    }
    if (!$names.Count) { throw "UAT manifest contains no files: $Path" }
    return ,$names
}

$archive = Get-FullPath $ArchiveDirectory
$output = Get-FullPath $OutputDirectory
$engine = Get-FullPath $EngineRoot
$receiptPath = Get-FullPath $BuildReceiptPath
Assert-File $receiptPath
$receiptHash = Get-Sha256 $receiptPath
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
foreach ($field in @('TargetName', 'TargetType', 'Platform', 'Architecture', 'Configuration', 'Version', 'BuildPlugins', 'BuildProducts', 'RuntimeDependencies')) {
    if ($null -eq $receipt -or !$receipt.PSObject.Properties[$field]) { throw "Invalid build receipt: required field '$field' is missing." }
}
if ($receipt.TargetName -ne 'DublinFlight' -or $receipt.TargetType -ne 'Game' -or
    $receipt.Platform -ne 'Win64' -or $receipt.Architecture -ne 'x64' -or
    $receipt.Configuration -notin @('Development', 'Shipping')) {
    throw 'BuildReceiptPath must describe a DublinFlight Win64 x64 Game target, Development or Shipping.'
}
$configuration = $receipt.Configuration
if ($configuration -eq 'Development' -and !$AllowDevelopment) {
    throw 'Development retains developer functionality and does not run the Shipping non-redistributable-module check. Use Shipping, or explicitly accept this with -AllowDevelopment after review.'
}
if (!$ArchiveVerified -and !$ValidateOnly) {
    throw 'Run final checks on this exact archive, then pass -ArchiveVerified. Packaging cannot certify gameplay, performance or distribution rights.'
}
if ($configuration -eq 'Shipping' -and !$ValidateOnly -and !$VerifiedFullscreen1080p) {
    throw 'Shipping delivery requires -VerifiedFullscreen1080p after this exact Shipping archive passes its own final 1920x1080 Fullscreen profile and visual checks. This confirmation creates Play.cmd; it does not run or certify those checks. Use -ValidateOnly for preflight.'
}
$releaseName = "DublinFlight-$Version-Win64-$configuration"
$releaseDirectory = Join-Path $output $releaseName
if (Test-Path -LiteralPath $releaseDirectory) { throw "Release already exists; choose a new -Version: $releaseDirectory" }
if ($output.Equals($archive, [StringComparison]::OrdinalIgnoreCase) -or
    $output.StartsWith($archive.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must be outside the input archive.'
}
$engineVersionPath = Join-Path $engine 'Engine\Build\Build.version'
Assert-File $engineVersionPath
$engineVersion = Get-Content -LiteralPath $engineVersionPath -Raw | ConvertFrom-Json
foreach ($field in @('MajorVersion', 'MinorVersion', 'PatchVersion', 'Changelist')) {
    if ($receipt.Version.$field -ne $engineVersion.$field) { throw "Receipt and installed engine differ: $field. Supply the matching engine." }
}
if ($engineVersion.MajorVersion -ne 5 -or $engineVersion.MinorVersion -ne 8 -or $engineVersion.PatchVersion -ne 2) {
    throw 'This redistribution recipe requires the matching UE 5.8.2 installation.'
}

$editorPlugins = @('ModelContextProtocol', 'AllToolsets',
    'ChaosClothAssetToolset', 'LiveCodingToolset', 'MetaHumanGenerator', 'MVVMToolset',
    'SequencerAnimMixerToolset', 'ToolsetRegistry')
# GeometryCollection and Niagara legitimately retain PlanarCut and Python/editor
# descriptor dependencies. Block actual automation/NoRedist, not naming lookalikes.
$pluginDescriptors = @(Get-ChildItem -LiteralPath (Join-Path $engine 'Engine\Plugins') -Filter '*.uplugin' -Recurse -File |
    Where-Object BaseName -in $receipt.BuildPlugins)
foreach ($pluginFile in $pluginDescriptors) {
    $descriptor = Get-Content -LiteralPath $pluginFile.FullName -Raw | ConvertFrom-Json
    $noRedist = $descriptor.PSObject.Properties['NoRedist']
    if (($noRedist -and $noRedist.Value -eq $true) -or $pluginFile.FullName -match '\\Experimental\\Toolsets\\') {
        $editorPlugins += $pluginFile.BaseName
    }
}
$unwantedPlugins = @($receipt.BuildPlugins | Where-Object { $_ -in $editorPlugins })
if ($unwantedPlugins.Count) {
    throw "Game receipt still enables editor/automation plugins: $($unwantedPlugins -join ', '). Rebuild and recook with Editor TargetAllowList; do not repackage the old archive."
}
$nonUfsPath = Join-Path $archive 'Manifest_NonUFSFiles_Win64.txt'
$ufsPath = Join-Path $archive 'Manifest_UFSFiles_Win64.txt'
$nonUfs = Read-StagingManifest $nonUfsPath
$ufs = Read-StagingManifest $ufsPath
$nonUfsHash = Get-Sha256 $nonUfsPath
$ufsHash = Get-Sha256 $ufsPath
if (!$ufs.Contains('DublinFlight\Content\Maps\Dublin.umap')) { throw 'UFS manifest does not contain the cooked Dublin map.' }
$forbidden = '(?i)(^|\\)(Saved|Intermediate|Source|NoRedist|NotForLicensees|Restricted|\.git|\.github)(\\|$)|\.(zip|7z|rar|las|laz|pdb|log|pfx|pem|key)$|(^|\\)(\.env[^\\]*|credentials[^\\]*|mcp[^\\]*)$'
foreach ($name in @($nonUfs) + @($ufs)) {
    if ($name -match $forbidden -or $name -match '(?i)\\Plugins\\Experimental\\(ModelContextProtocol|Toolsets|ToolsetRegistry)\\') {
        throw "Staging manifest contains forbidden release content: $name. Fix staging and recook; opaque containers cannot be safely filtered here."
    }
}

$files = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
function Add-Payload([string]$RelativePath, [string]$SourcePath) {
    $name = Get-RelativeName $RelativePath
    Assert-File $SourcePath
    if ($files.ContainsKey($name)) {
        if ((Get-Sha256 $files[$name]) -ne (Get-Sha256 $SourcePath)) { throw "Conflicting payload sources: $name" }
    } else { $files.Add($name, $SourcePath) }
}
foreach ($name in $nonUfs) {
    if ($name -notmatch '^(DublinFlight\\(Binaries|Content)\\|Engine\\(Binaries|Content|Config|Plugins|Extras\\Redist)\\|DublinFlight\.exe$|NOTICES\.txt$)') {
        throw "Unexpected NonUFS payload location: $name. Review the staging rule before adding it."
    }
    Add-Payload $name (Join-Path $archive $name)
    if ([IO.Path]::GetExtension($name) -in @('.ini', '.json', '.txt')) {
        Assert-NoSecret $files[$name] "loose runtime file $name"
    }
}
$pakDirectory = Join-Path $archive 'DublinFlight\Content\Paks'
if (!(Test-Path -LiteralPath $pakDirectory -PathType Container)) { throw "Cooked container directory is missing: $pakDirectory. A bootstrap executable alone is not a game." }
$containers = @(Get-ChildItem -LiteralPath $pakDirectory -File | Where-Object Extension -in @('.pak', '.utoc', '.ucas'))
if (!@($containers | Where-Object Extension -eq '.pak').Count) { throw 'Cooked .pak payload is missing; a bootstrap executable alone is not a game.' }
foreach ($container in $containers) {
    Add-Payload "DublinFlight\Content\Paks\$($container.Name)" $container.FullName
    if ($container.Extension -in @('.utoc', '.ucas')) {
        $partner = [IO.Path]::ChangeExtension($container.FullName, $(if ($container.Extension -eq '.utoc') { '.ucas' } else { '.utoc' }))
        Assert-File $partner
    }
}
foreach ($name in @('DublinFlight-Windows.pak', 'DublinFlight-Windows.utoc', 'DublinFlight-Windows.ucas', 'global.utoc', 'global.ucas')) {
    if (!$files.ContainsKey("DublinFlight\Content\Paks\$name")) { throw "Required UE 5.8 IoStore container is missing: $name" }
}
$exeName = if ($configuration -eq 'Shipping') { 'DublinFlight-Win64-Shipping.exe' } else { 'DublinFlight.exe' }
$gamePath = "DublinFlight\Binaries\Win64\$exeName"
$required = @('DublinFlight.exe', $gamePath, 'NOTICES.txt',
    'DublinFlight\Content\Data\ATTRIBUTION.txt', 'DublinFlight\Content\Data\dublin-city.json',
    'DublinFlight\Content\Data\dublin-ortho-georeference.json', 'DublinFlight\Content\Data\facade-provenance.json',
    'DublinFlight\Content\Data\dublin-ortho.png', 'DublinFlight\Content\Data\dublin-ortho-validity.png')
foreach ($name in $required) { if (!$files.ContainsKey($name)) { throw "Required staged runtime file is absent from the NonUFS manifest: $name" } }
foreach ($product in $receipt.BuildProducts | Where-Object Type -in @('Executable', 'DynamicLibrary')) {
    $source = $product.Path.Replace('$(ProjectDir)', $projectRoot).Replace('$(EngineDir)', (Join-Path $engine 'Engine')).Replace('/', '\')
    $staged = $product.Path.Replace('$(ProjectDir)', 'DublinFlight').Replace('$(EngineDir)', 'Engine').Replace('/', '\')
    if (!$files.ContainsKey($staged)) { throw "Build receipt runtime product is missing from staging: $staged" }
    Assert-File $source
    if ((Get-Sha256 $source) -ne (Get-Sha256 $files[$staged])) { throw "Archive does not match the current build receipt product: $staged. Rebuild/rearchive, then repeat final checks." }
}
if (!@($receipt.BuildProducts | Where-Object { $_.Type -eq 'Executable' -and $_.Path.Replace('/', '\').EndsWith("\$exeName") }).Count) {
    throw "Receipt does not identify the expected game executable: $exeName"
}
foreach ($dependency in $receipt.RuntimeDependencies | Where-Object Type -in @('NonUFS', 'SystemNonUFS')) {
    $staged = $dependency.Path.Replace('$(ProjectDir)', 'DublinFlight').Replace('$(EngineDir)', 'Engine').Replace('/', '\')
    if (!$files.ContainsKey($staged)) { throw "Build receipt runtime dependency is missing from staging: $staged" }
}

foreach ($name in @('vc_redist.x64.exe', 'vc_redist.arm64.exe')) {
    $relative = "Engine\Extras\Redist\en-us\$name"
    $source = Join-Path $engine $relative
    Assert-File $source
    $signature = Get-AuthenticodeSignature -LiteralPath $source
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Prerequisite installer lacks a valid Microsoft signature: $source"
    }
    Add-Payload $relative $source
}
$licenseDirectory = Join-Path $engine 'Engine\Source\ThirdParty\Licenses'
$licenses = @(Get-ChildItem -LiteralPath $licenseDirectory -File -Recurse)
if (!$licenses.Count) { throw "Engine third-party license notices are missing: $licenseDirectory" }
foreach ($license in $licenses) {
    Add-Payload ("ThirdPartyNotices\" + [IO.Path]::GetRelativePath($licenseDirectory, $license.FullName)) $license.FullName
}
$unrealPak = Join-Path $engine 'Engine\Binaries\Win64\UnrealPak.exe'
Assert-File $unrealPak
$work = Join-Path ([IO.Path]::GetTempPath()) ('dublin-redist-' + [guid]::NewGuid().ToString('N'))
$pending = $null
$inputHashes = @{}
foreach ($name in $files.Keys) { $inputHashes[$name] = Get-Sha256 $files[$name] }
[IO.Directory]::CreateDirectory($work) | Out-Null
try {
    # Inspect cooked INIs, not the unsanitized project Config directory.
    $configCount = 0
    $pakEntryCount = 0
    foreach ($pak in $containers | Where-Object Extension -eq '.pak') {
        $auditDirectory = Join-Path $work $pak.BaseName
        $inventoryPath = Join-Path $work "$($pak.BaseName)-inventory.txt"
        $toolOutput = & $unrealPak $pak.FullName '-List' "-csv=$inventoryPath" '-ExtractToMountPoint' '-unattended' 2>&1
        if ($LASTEXITCODE -ne 0) { throw "UnrealPak inventory failed for $($pak.Name), exit $LASTEXITCODE. No release was published." }
        Assert-File $inventoryPath
        $entries = @(Import-Csv -LiteralPath $inventoryPath)
        if (!$entries.Count) { throw "Pak contains no inspectable file entries: $($pak.Name)" }
        foreach ($entry in $entries) {
            $name = Get-RelativeName ($entry.Filename.Trim().Replace('/', '\') -replace '^(\.\.\\){3}', '')
            if (!$ufs.Contains($name)) { throw "Actual pak entry is absent from the checked UFS manifest: $name. Stale or mismatched cooked payload; recook/rearchive." }
            if ([IO.Path]::GetExtension($name) -eq '.uplugin' -and [IO.Path]::GetFileNameWithoutExtension($name) -notin $receipt.BuildPlugins) {
                throw "Actual pak plugin is absent from the game receipt: $name. Rebuild/recook a consistent archive."
            }
        }
        $pakEntryCount += $entries.Count
        $toolOutput = & $unrealPak $pak.FullName '-Extract' $auditDirectory '-Filter=*.ini' '-unattended' 2>&1
        if ($LASTEXITCODE -ne 0) { throw "UnrealPak config inspection failed for $($pak.Name), exit $LASTEXITCODE. No release was published." }
        $toolOutput = & $unrealPak $pak.FullName '-Extract' $auditDirectory '-Filter=*.uplugin' '-unattended' 2>&1
        if ($LASTEXITCODE -ne 0) { throw "UnrealPak plugin inspection failed for $($pak.Name), exit $LASTEXITCODE. No release was published." }
        foreach ($plugin in Get-ChildItem -LiteralPath $auditDirectory -Filter '*.uplugin' -File -Recurse) {
            $descriptor = Get-Content -LiteralPath $plugin.FullName -Raw | ConvertFrom-Json
            $noRedist = $descriptor.PSObject.Properties['NoRedist']
            if (($noRedist -and $noRedist.Value -eq $true) -or $plugin.BaseName -in $editorPlugins) {
                throw "Actual pak contains an automation/NoRedist plugin: $($plugin.Name). Rebuild/recook without this plugin."
            }
        }
        $configs = @(Get-ChildItem -LiteralPath $auditDirectory -Filter '*.ini' -File -Recurse)
        $configCount += $configs.Count
        foreach ($config in $configs) {
            Assert-NoSecret $config.FullName "cooked INI: $($config.Name)"
            $text = Get-Content -LiteralPath $config.FullName -Raw
            if ($text -match '(?im)^\[GameInput\]' -and $text -match '(?im)^\s*IncludeRedistFiles\s*=\s*True') {
                if (!$files.ContainsKey('Engine\Extras\Redist\en-us\GameInputRedist.msi')) {
                    throw 'Cooked GameInput opts into its installer, but GameInputRedist.msi is not staged. Repackage with -prereqs.'
                }
            }
        }
    }
    if (!$configCount) { throw 'No cooked INIs could be inspected. This recipe requires unencrypted .pak configuration content; review this archive before extending the recipe.' }
    foreach ($container in $containers) {
        $name = "DublinFlight\Content\Paks\$($container.Name)"
        if ((Get-Sha256 $container.FullName) -ne $inputHashes[$name]) { throw "Cooked container changed during inspection: $name. No release was published." }
    }
    if ($ValidateOnly) {
        return [pscustomobject]@{ Status = 'InputsValidatedOnly'; Configuration = $configuration; FileCount = $files.Count; ReleaseDirectory = $releaseDirectory; Published = $false }
    }
    [IO.Directory]::CreateDirectory($output) | Out-Null
    $pending = Join-Path $output (".$releaseName.partial-" + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($pending) | Out-Null
    $payload = Join-Path $work 'payload'
    [IO.Directory]::CreateDirectory($payload) | Out-Null
    $records = [Collections.Generic.List[object]]::new()
    foreach ($name in ($files.Keys | Sort-Object)) {
        $destination = Join-Path $payload $name
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        [IO.File]::Copy($files[$name], $destination, $false)
        $hash = Get-Sha256 $destination
        if ($hash -ne $inputHashes[$name] -or $hash -ne (Get-Sha256 $files[$name])) { throw "Input changed while packaging: $name. Stop writers and repeat final verification." }
        $records.Add([pscustomobject]@{ path = $name; bytes = (Get-Item -LiteralPath $destination).Length; sha256 = $hash })
    }
    $playerEntry = if ($VerifiedFullscreen1080p) { 'Play.cmd' } else { 'DublinFlight.exe' }
    $presentationText = if ($VerifiedFullscreen1080p) {
        'Play.cmd requests 1920x1080 Fullscreen, as confirmed by the publisher, without quality overrides. Directly launching DublinFlight.exe does not apply these presentation arguments.'
    } else {
        'No fixed presentation launcher was requested for this package.'
    }
    $playText = @"
DUBLIN FLIGHT - $Version ($configuration, Win64 x64)

Offline single-player over a 768 m Dublin city-centre core around O'Connell Bridge,
not all of Dublin. Buildings, terrain and water are approximations. "Tons of TNT"
is a game-yield setting, not a physical prediction.

Extract the entire ZIP to a writable local directory. Do not run inside the ZIP
or move only the small DublinFlight.exe bootstrap. Launch $playerEntry.
$presentationText
Windows x64 and a current DirectX 12 / Shader Model 6 capable graphics driver are
required by this build. No Unreal Engine, Visual Studio or PowerShell installation
is required. If needed, run Engine\Extras\Redist\en-us\vc_redist.x64.exe to install/repair
Microsoft Visual C++ runtime prerequisites. The arm64 installer is retained as
staged by UE for emulation hosts; ARM64 gameplay has not been verified.
If GameInputRedist.msi is present, the bootstrap may request its installation.
Prerequisite installers can require administrator approval; this ZIP installs nothing.

CONTROLS
W/S: nose down/up (god: forward/back). A/D: bank left/right (god: strafe).
Q/E: yaw left/right. Flight pitch/roll/yaw use the aircraft's own axes.
Flight speed: 5-100 m/s, default 40. Wheel up/down: +/-5 m/s per step.
Hold Shift/Ctrl: +/-10 m/s each second. HUD shows current and selected speed.
Bank-turn assist changes world heading separately; counter-bank to level.
Full body roll permits inverted flight; there is no world-bank cap.
The +/-80 degree vertical pitch guard limits each body pitch/yaw command
separately, never freezing another usable axis. Roll and countersteering remain free.
Mouse: orbit camera in flight (0.36 deg/unit, 3x faster; no hold or steering).
God-mode mouse aim remains 0.12 deg/unit.
G: toggle flight/god mode and clear orbit. Space/C: god-mode up/down.
Home: restore aircraft at Dublin's mid-air spawn, reset 40 m/s and recenter the chase view,
including from outside the surveyed area.
Speed controls are flight-only. G/Home, possession changes and lost focus discard
pending wheel input; the transition frame cannot change the reset/resumed speed.
Hold left mouse: cannon. B/right mouse: drop bomb.
Amber BOMB labels track ALL live bombs, never predicted landing positions.
Stable actor IDs identify bombs (not release numbers); lines link visible positions.
Each bomb has its own below-view/off-screen/unavailable status in a bounded grid,
with no visual-count cap. Impact, destruction or expiry removes only that marker.
Released/In flight counts show bomb progress; cannon shells are excluded.
[/]: halve/double bomb yield. F1: controls, diagnostics and credits. Alt+F4: quit.
Release and re-press weapon input after G, Home or losing focus.

Keep CREDITS.txt, NOTICES.txt, ThirdPartyNotices and DublinFlight\Content\Data
with the game. Read the data attribution, imagery gaps and inference limitations.
Development retains developer functionality. Benchmark evidence is configuration-specific.
This file and manifest certify no performance target, visual acceptance or licensing
clearance. Final gameplay checks and distribution-rights review belong to the publisher.
"@
    $registered = [char]0x00ae
    $copyrightYear = (Get-Date).Year
    $credits = @"
DUBLIN FLIGHT
DublinFlight uses Unreal$registered Engine. Unreal$registered is a trademark or
registered trademark of Epic Games, Inc. in the United States of America and elsewhere.
Unreal$registered Engine, Copyright 1998 - $copyrightYear, Epic Games, Inc. All rights reserved.

NOTICES.txt is preserved from UAT. ThirdPartyNotices contains the matching
engine installation's supplied third-party license notices, including components
not necessarily linked into this game. Their presence is not a license audit.
The publisher must review engine/asset terms and applicable data obligations;
this package does not grant rights beyond the respective original licenses.

"@ + (Get-Content -LiteralPath $files['DublinFlight\Content\Data\ATTRIBUTION.txt'] -Raw)
    $documents = @{ 'PLAY.txt' = $playText; 'CREDITS.txt' = $credits }
    if ($VerifiedFullscreen1080p) {
        $documents['Play.cmd'] = @"
@echo off
setlocal
pushd "%~dp0DublinFlight"
if errorlevel 1 exit /b 2
"Binaries\Win64\$exeName" /Game/Maps/Dublin -fullscreen -Res=1920x1080f
set "GameExitCode=%ERRORLEVEL%"
popd
exit /b %GameExitCode%
"@
        $documents['Play.cmd'] = $documents['Play.cmd'].Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n"
    }
    foreach ($document in $documents.GetEnumerator()) {
        $path = Join-Path $payload $document.Key
        [IO.File]::WriteAllText($path, $document.Value, [Text.UTF8Encoding]::new($false))
        $records.Add([pscustomobject]@{ path = $document.Key; bytes = (Get-Item -LiteralPath $path).Length; sha256 = Get-Sha256 $path })
    }
    $manifest = [ordered]@{
        schemaVersion = 1; release = $releaseName; createdUtc = [DateTimeOffset]::UtcNow.ToString('o')
        configuration = $configuration; platform = 'Win64'; architecture = 'x64'
        engineVersion = '5.8.2'; engineChangelist = $engineVersion.Changelist
        inspectedPakEntries = $pakEntryCount; inspectedCookedIniFiles = $configCount
        archiveVerifiedByCaller = [bool]$ArchiveVerified; licenseComplianceCertified = $false
        buildReceiptSha256 = $receiptHash
        nonUfsManifestSha256 = $nonUfsHash; ufsManifestSha256 = $ufsHash
        entryPoint = $playerEntry; gameExecutable = $gamePath
        playerPresentation = $(if ($VerifiedFullscreen1080p) {
            @{ width = 1920; height = 1080; windowMode = 'Fullscreen'; verifiedByCaller = $true; qualityOverrides = @() }
        } else { $null })
        files = @($records | Sort-Object path)
    }
    $manifestPath = Join-Path $payload 'manifest.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    $zipName = "$releaseName.zip"
    $zipPath = Join-Path $pending $zipName
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
    $expected = @{}
    foreach ($record in $records) { $expected[$record.path] = $record }
    $expected['manifest.json'] = @{ bytes = (Get-Item -LiteralPath $manifestPath).Length; sha256 = Get-Sha256 $manifestPath }
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        if ($zip.Entries.Count -ne $expected.Count) { throw 'ZIP entry count differs from the verified payload.' }
        foreach ($entry in $zip.Entries) {
            $name = Get-RelativeName $entry.FullName
            if (!$expected.ContainsKey($name)) { throw "Unexpected or duplicate ZIP entry: $name" }
            $stream = $entry.Open()
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant() }
            finally { $stream.Dispose() }
            if ($entry.Length -ne $expected[$name].bytes -or $hash -ne $expected[$name].sha256) { throw "ZIP verification failed: $name" }
            $expected.Remove($name)
        }
        if ($expected.Count) { throw 'ZIP is missing manifest-listed files.' }
    } finally { $zip.Dispose() }
    $zipHash = Get-Sha256 $zipPath
    [IO.File]::Copy($manifestPath, (Join-Path $pending 'manifest.json'), $false)
    "$zipHash  $zipName" | Set-Content -LiteralPath (Join-Path $pending "$zipName.sha256") -Encoding ascii
    foreach ($name in $files.Keys) {
        if ((Get-Sha256 $files[$name]) -ne $inputHashes[$name]) { throw "Input changed before publication: $name. Repeat final verification on a stable archive." }
    }
    if ((Get-Sha256 $receiptPath) -ne $receiptHash -or (Get-Sha256 $nonUfsPath) -ne $nonUfsHash -or (Get-Sha256 $ufsPath) -ne $ufsHash) {
        throw 'Build receipt or staging manifests changed while packaging. No release was published.'
    }
    [IO.Directory]::Move($pending, $releaseDirectory)
    $pending = $null
    return [pscustomobject]@{
        Status = 'PackageVerified'; ReleaseDirectory = $releaseDirectory
        Zip = Join-Path $releaseDirectory $zipName
        Sha256File = Join-Path $releaseDirectory "$zipName.sha256"
        Manifest = Join-Path $releaseDirectory 'manifest.json'
        ZipSha256 = $zipHash; FileCount = $records.Count + 1; Configuration = $configuration
    }
} finally {
    if ($pending -and (Test-Path -LiteralPath $pending)) { Remove-Item -LiteralPath $pending -Recurse -Force }
    Remove-Item -LiteralPath $work -Recurse -Force
}
