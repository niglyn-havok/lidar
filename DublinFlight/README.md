# DublinFlight

A UE 5.8 flight-and-destruction game over a **768 m Dublin city-centre core**
around O'Connell Bridge. Fly a slow, maneuverable aircraft, or switch to stationary
god mode to aim and reposition. Cannon shells and gravity-driven bombs affect
fractured buildings, terrain and river water. The Dublin map starts **mid-air**:
there is no runway or synthetic flight course.

## Requirements

- Windows, PowerShell 7 or newer, and Unreal Engine **5.8.2**.
- Visual Studio 2022 C++ build tools and a Windows SDK supported by that engine.
- The complete project content, including Git LFS assets. After cloning, run
  `git lfs pull` from the repository root; pointer files are not usable assets.
- For agent-driven editor work, the `ue-editor-control` plugin and its bundled
  project/editor skills. Keep unrestricted Python Remote Execution disabled.

The scripts locate the project relative to themselves, not the current directory.
Examples below assume a terminal in this project directory. Set your own engine
installation root (the directory containing `Engine`):

```powershell
$env:UE_ENGINE_ROOT = '<engine-root>'
```

## Build and package

Close this project's editor and finish other builds/cooks before packaging.
The runner refuses to build while it detects this project's editor; it never
terminates processes.

```powershell
pwsh -File .\Scripts\Build-Package.ps1
# An explicit path overrides UE_ENGINE_ROOT:
pwsh -File .\Scripts\Build-Package.ps1 -EngineRoot '<engine-root>'
# After source changes are frozen, select Shipping explicitly:
pwsh -File .\Scripts\Build-Package.ps1 -EngineRoot '<engine-root>' `
    -Configuration Shipping -ArchiveDirectory 'Saved\Builds\ShippingCandidate'
```

This runs `BuildCookRun -build -cook -stage -pak -archive` for Win64, building
the Editor and Game targets needed for a fresh cook. `-Configuration` accepts
`Development` (the compatibility default) or `Shipping`. Shipping adds
`-prereqs -nodebuginfo`; Development retains its existing diagnostic build
behavior. Engine EULA/non-redistributable checks are never overridden.
The runner checks for UE
5.8 and warns if the patch version differs from the validated 5.8.2. Build
parallelism defaults to three actions; `-MaxParallelActions` accepts only 1-3.

The map is `/Game/Maps/Dublin`. Existing packaging settings include runtime
`Content\Data` files as NonUFS data and an editor-only `GameFeatureData` asset rule.
The saved fracture library supplies the current 1,044 collection references;
packaging does not rebake or edit source assets.

- UAT archive root: `Saved\Builds\Windows` by default; override with `-ArchiveDirectory`.
- UE 5.8 payload: the archive root's `Windows` child (by default,
  `Saved\Builds\Windows\Windows`). Older flat archives remain supported.
- Staging: `Saved\StagedBuilds\Windows`.
- Invocation and complete build/cook output: `Saved\Packaging\<run-id>`.

Relative archive paths are project-relative. Failures remain nonzero and identify
the log; a successful UAT exit also requires a nonempty archived game executable.
The result distinguishes `ArchiveRootDirectory` (passed to UAT) from the actual
`ArchiveDirectory` and `GameExecutable`. Post-build validation and the launcher
use the same layout discovery; archive contents are never moved or flattened.
Use `-DryRun` to inspect paths and argument arrays without writing files or
starting a build:

```powershell
.\Scripts\Build-Package.ps1 -DryRun | ConvertTo-Json
```

In a build dry run, `ArchiveDirectory` and `GameExecutable` remain null because
UAT has not produced them. To resolve an existing archive without rebuilding or
launching, use `.\Scripts\Launch-Game.ps1 -ArchiveDirectory '<archive-root-or-payload>' -Configuration Shipping -DryRun`
(omit `-Configuration` for Development).

## Redistributable ZIP

Use a **fresh, fully checked Shipping archive** for final delivery. Finish the
integrated build and fresh cook, then its own startup-only CLI benchmark cohort,
gameplay, visual and cooked-performance checks before assembling anything
for delivery. Do not repackage the older baseline after source/plugin changes.
Shipping must pass its own startup-only benchmark checks;
Development benchmark results do not certify the Shipping executable.

```powershell
.\Scripts\Package-Redistributable.ps1 `
    -ArchiveDirectory '<verified-archive>' `
    -BuildReceiptPath 'Binaries\Win64\DublinFlight-Win64-Shipping.target' `
    -OutputDirectory '<local-release-directory>' -Version '<unique-version>' `
    -EngineRoot '<engine-root>' -ArchiveVerified -VerifiedFullscreen1080p
```

Pass `-VerifiedFullscreen1080p` **only after the exact Shipping artifact passes
its final 1920x1080 Fullscreen profile and visual checks**. This records the
caller's confirmation; it does not run or certify those checks. Only then does
assembly include `Play.cmd`, which launches the real game executable with
`-fullscreen -Res=1920x1080f` and no quality overrides. No final player launcher
or presentation acceptance is implied by a dry run or source-ready script.
Diagnostic Development archives remain supported with their matching
`Binaries\Win64\DublinFlight.target` and explicit `-AllowDevelopment`.

For input checks without publishing, replace `-ArchiveVerified` with
`-ValidateOnly` and omit `-VerifiedFullscreen1080p`. Pass the actual payload
`ArchiveDirectory`, not its UAT parent;
for example, an archive root `Saved\Builds\Candidate` has payload
`Saved\Builds\Candidate\Windows`. Neither switch bypasses receipt/configuration matching, actual
pak/plugin inspection, detected-secret checks or file hashes. The script never
builds, launches the game or installs software. It preserves the six staged data
files and attribution, adds player controls and engine/third-party notices, and
includes the matching engine's signed `vc_redist.x64.exe` and arm64 companion.
GameInput's installer is required only when the cooked configuration opts in.
Prerequisites are supplied for the recipient to install if needed, not run here.

The returned `ReleaseDirectory` contains a configuration-labeled Win64 ZIP,
its SHA256 file and a per-file manifest. Unmanifested logs, crash state,
benchmark reports, raw archives and debug symbols are not copied; existing
acceptance evidence is left untouched. Copy the **whole completed release
directory** to the agreed delivery destination only after final acceptance and
successful package verification, then verify the copied file hashes.
Archive integrity is not gameplay, performance or license certification;
distribution-rights review remains the publisher's responsibility.

**Engineering finding:** the matching installed UE 5.8.2 Shipping TraceLog
object contains return-only trace-control stubs, whereas Development includes
the TCP listener. Standard Shipping removes that listener without an engine fork
or permission changes. This neither identifies a Windows Security prompt nor
proves foreground validity; the Shipping game's actual checks remain required.

## Play

```powershell
pwsh -File .\Scripts\Launch-Game.ps1
pwsh -File .\Scripts\Launch-Game.ps1 -Width 2560 -Height 1440 -Fullscreen
# Explicit presentation request; not a claim that this profile has passed:
pwsh -File .\Scripts\Launch-Game.ps1 -Configuration Shipping `
    -ArchiveDirectory 'Saved\Builds\ShippingCandidate' -Fullscreen
```

The launcher defaults to Development in the archive above and a 1920x1080 window.
Shipping requires `-Configuration Shipping`, which resolves the real
`DublinFlight-Win64-Shipping.exe`, not the bootstrap or a Development fallback.
Use
`-ArchiveDirectory` for a different archive, or `-DryRun` to inspect the launch
without starting it. It does not force focus, alter graphics quality, or start a
benchmark. It accepts UAT roots, explicit `Windows` payloads, legacy flat archives
and `Saved\StagedBuilds\Windows`. An existing `Windows` child takes precedence;
if that child is incomplete, the launcher fails rather than starting a stale
flat game. In the final redistributable, use the verified `Play.cmd` entry;
running the bootstrap directly does not supply its fixed presentation arguments.

| Control | Flight | God mode |
|---|---|---|
| W / S | Nose down / up | Forward / back |
| A / D | Bank left / right | Strafe left / right |
| Q / E | Yaw left / right | Unused |
| Shift / Ctrl | Hold to raise / lower speed by 10 m/s each second | Unused |
| Mouse wheel up / down | Raise / lower speed by 5 m/s per step | Unused |
| Mouse | Orbit camera (no button held; no steering) | Aim |
| Space / C | Unused | Move up / down |
| G | Toggle flight / god mode | Toggle flight / god mode |
| Home | Restore aircraft at mid-air spawn + chase view | Restore aircraft at mid-air spawn + chase view |
| Hold left mouse | Cannon | Cannon |
| B or right mouse | Drop bomb | Drop bomb |
| [ / ] | Halve / double bomb yield | Halve / double bomb yield |
| F1 | Show / hide controls, diagnostics and credits | Show / hide controls, diagnostics and credits |

Flight speed is directly selected within **5-100 m/s**, starting at **40 m/s**.
Wheel steps change speed immediately by 5 m/s each; holding Shift/Ctrl changes
the same selected speed by 10 m/s per second. The normal HUD shows current and
selected speed plus these bindings; in god mode it shows the retained resume speed.
W/S, A/D and Q/E rotate about the aircraft's own pitch, roll and yaw axes.
Bank-turn assist separately turns the world heading; counter-bank to level.
Flight allows **full body roll, including inverted flight**; there is no world-bank
cap. The **+/-80-degree vertical pitch guard** keeps the nose away from the chase
camera's heading poles. Roll remains free, and each body-local pitch/yaw command
is shortened separately only if it would cross that vertical guard. An outward
command therefore cannot freeze another usable axis; countersteering stays responsive.
Mouse orbit uses 0.36 degrees per input unit (3x the previous sensitivity);
god-mode aim remains 0.12. Orbit changes only the camera, not aircraft steering or cannon aim;
right mouse remains bomb release. G clears the orbit; Home restores the aircraft
at Dublin's mid-air spawn, restores 40 m/s and recenters the chase view, including from outside
the surveyed area. Losing focus keeps the orbit view but discards pending mouse
and wheel input. Wheel input is ignored in god mode and discarded on G/Home or
possession changes, including the transition frame.
God mode stays still without movement input; horizontal movement and altitude
are separate. After G, Home or losing input focus, release and re-press weapon
input. The HUD shows mode, speed, altitude, heading, yield and rejection details.

Bombs release beneath the aircraft, not toward the cannon crosshair. In flight
they retain the aircraft's forward velocity; in stationary god mode they fall
straight down. Impact takes several seconds from the initial altitude. Stay
over the surveyed city, and tap B or right mouse for each bomb rather than
holding the button. Bomb releases share a 1.5-second cooldown.
The normal HUD shows released and in-flight bomb counts. Every live bomb has an
amber `BOMB` label with a stable actor ID, not a release number. For visible bombs,
leader lines connect the labels to diamonds at their actual projected positions.
Each other bomb reports `below view`, `off-screen` (including behind the camera),
or `unavailable`; no landing position is predicted. Labels use a bounded grid
without a visual-count cap. Impact, destruction or expiry removes only that bomb's
marker; cannon shells never appear in this list.

## Validation and performance

### City-wide destruction

The current source expands maximum game yield to a 120 m building-damage radius
with a 60 m demolition core. This is gameplay tuning, not a real-world blast
model. Large impacts use building footprints and height-aware fragment selection;
crater radius/depth remain separately bounded. Real fragment activation and
collision replacement precede confirmed-surface debris effects.

Physics budget policy 2 reserves a finite catalog of at most 1,044 collections,
1,048,576 leaf slots and 8,388,608 hull slots. Allocations are lazy; sleeping
collections still count as resident. Asset prefetch is capped at 16 concurrent
loads and registration at eight collections per frame. Reports disclose resident
and awake usage separately; these limits are ceilings, not acceptable frame-time
claims or a request to allocate the whole catalog at startup.

Bomb release checks now sweep the actual aircraft-to-release segment so a low
drop cannot begin beyond an intervening solid surface. Deformed terrain rebuilds
the changed chunk's collision synchronously: native regression drops exposed
missed sphere sweeps after in-place collision-vertex updates. Water surface
crossings retain their explicit source-polygon handling; no invisible global
ground plane is introduced.

The editor's saved city-wide library now covers all 1,044 volumes: 993
`StructuralCity` and 51 `LayeredMasonry` records, including all 16 courtyards.
Its 546,462 physical fragments use irregular Voronoi cuts through inferred
walls, slabs and masonry, rather than a solid grid. Double-precision attributed
clipping preserves exterior materials before conversion to mesh storage.
Detailed tiers are 128, 384 and 768 pieces, or 1,536 for explicitly eligible
large surfaces.

Collision storage version 1 preserves cooked native leaf convexes and their
rigid transforms in the engine's persistent authored-collision data. The root
is their exact compound, avoiding an independent convex rebuild that can
simplify thin features differently. Existing valid version-0 records remain
supported; unknown versions, missing shapes, changed hulls, occupied voids and
volume loss are rejected. This does not change fracture seeds or the original
source geometry.

Each current record also carries a versioned collision-validation fingerprint.
Certification runs the full static geometry and void/probe proofs against the
actual saved collection before updating the library. Activation verifies the
actual collision contents against that fingerprint and still checks the live
physics instance, filters, anchors and probes. Missing legacy certificates use
the original full validation; malformed, stale or unsupported certificates fail
explicitly. This is content-integrity evidence, not an authentication mechanism.

The verified city-wide Shipping build is at
`Saved\Builds\CityDestruction-BVH-20260919\Windows`, alongside the preserved
earlier archives. Native gameplay/collision coverage, actual rendered breakup,
packaged presentation and redistribution-input checks have passed.
Release approval explicitly accepts the performance caveat below; it is not a
claim that every light-combat frame meets 60 FPS.

Native tests are available in the editor Automation window under `DublinFlight`.
For example, `Automation RunTests DublinFlight.Flight` exercises flight-state
math and input contracts. Headless/NullRHI native results are not proof of
rendering, physical gameplay or frame rate.

For PIE tests, open `/Game/Maps/Dublin` and let the fixture create its own PIE
session. **Run each test as a separate invocation**, wait for completion, then
start the next; do not submit these as one RunTests array. Shared destructive
state otherwise violates the next test's fresh-world guard:

```text
Automation RunTests DublinFlight.Destruction.PIE.RecoveredBuildingDamage
Automation RunTests DublinFlight.Destruction.PIE.ActualCityDamage
Automation RunTests DublinFlight.Destruction.PIE.TerrainAndWater
Automation RunTests DublinFlight.Weapons.PIE.ActualCannonBombInputs
Automation RunTests DublinFlight.Weapons.PIE.AllBombMarkersActualInputLifecycle
Automation RunTests DublinFlight.PIE.ActualPlayerControllerInput
```

`Scripts\Run-Benchmarks.ps1` owns cooked normal/light/maximal performance runs.
Development keeps its legacy console route; Shipping uses the startup-only
contract and requires a matching Game build receipt:

```powershell
$exe = (.\Scripts\Launch-Game.ps1 -DryRun).Executable
pwsh -File .\Scripts\Run-Benchmarks.ps1 -PackagedExe $exe

pwsh -File .\Scripts\Run-Benchmarks.ps1 `
    -PackagedExe 'Saved\Builds\ShippingCandidate\Windows\DublinFlight.exe' `
    -BuildReceiptPath 'Binaries\Win64\DublinFlight-Win64-Shipping.target' `
    -WindowMode Fullscreen
```

Shipping resolves the real nested `DublinFlight-Win64-Shipping.exe`, creates a
fresh run GUID and native output directory per scenario, and uses only the closed
`DublinBenchmark` startup flags. It does not enable or depend on general console,
Exec, logging, tracing, networking or `TestExit`. Defaults remain 30 measured
seconds after 5 warmup seconds; `-Scenario normal` selects one run.
Native option names are case-insensitive and duplicates are rejected. Run IDs
accept either input case but must be nonzero 32-hex GUIDs published in lowercase.
`-WindowMode` defaults to `Windowed`; `Fullscreen` requests actual 1920x1080
fullscreen, not desktop-sized borderless rendering.

The driver waits for its owned process to exit, then requires schema-version-2
`result.json` published last with `processExitPolicy: "graceful-zero"`.
**Process exit 0 is not benchmark success:** stock UE Windows graceful shutdown
returns 0 even for failed/error benchmark outcomes. Acceptance requires process
exit 0 **and** `status: "passed"` **and** semantic `outcomeCode: 0`, both native
reports, matching run/PID/configuration/capture identity, fresh contained paths,
and all raw-frame, quality, foreground and workload gates. Outcome 1 means failed
benchmark; outcome 2 means request/startup/publication error. Both make the
driver return nonzero, as do missing/partial reports or a nonzero process exit.
No forced-exit, CRT or entrypoint workaround is used. It independently SHA-256 hashes the
executable and archive content before and after runs; the cohort is derived from
bytes, never a caller GUID. Hashing occurs outside measurement. Root/project
Saved/Intermediate/DerivedDataCache trees, PDBs and this driver's fresh evidence
directory are excluded. All raw failures remain under `Saved\Automation\Benchmarks`;
missing, partial, contradictory or failed results return nonzero. Timeouts may
stop only the owned game process after retaining captured evidence. No focus or
OS-input workarounds are used. Development results cannot certify Shipping.

Finish shader compilation, editor rendering, builds/cooks and other heavy work
first. Use a visible, focused 1920x1080 game with fixed, disclosed quality; do not
interact with the desktop during measurement. Preserve invalid runs and slow
frames. The HUD's smoothed simulation-delta FPS is not acceptance evidence.
Normal/light action retain a hard 60 FPS target. Following the September 18
spectacle-first decision, maximal destruction reports the 30 FPS reference
without making it an acceptance floor. New reports declare
`performancePolicy=maximal-spectacle-v1` and `frameTargetRequired=false`;
`frameTargetMet` and `scenarioTargetMet` still report the actual raw-frame result.
Successful completion under this policy is not a claim that 30 FPS was met.
Full duration, valid measurement conditions, admitted workload and report
integrity remain mandatory. Historical reports without the new policy retain
their original strict frame-floor interpretation; slow frames are never removed.
The maximal benchmark is downstream destruction stress, not a claim of a
complete aircraft/weapons workload.

Maximal schedule policy 3 releases the entire planned stress workload at the
first measured boundary, then attempts at most eight requests per engine
boundary. The 30-second scenario still requests 60 operations with the same
targets and yields; none are performed during warmup. This is an explicit
saturation burst, not a two-bombs-per-second flight simulation. Actual
attempt/admission times, delay, backlog and peak batch are reported. Rejections,
deadline overruns and requests still pending at measurement end remain failures.
Historical policy 2 retains its half-second release/deferred-request semantics;
normal/light schedules and weapon cooldowns retain their no-catch-up behaviour.

New players default to a 120 FPS user limit through
`Config\DefaultGameUserSettings.ini` (UE settings version 5). Valid saved choices,
including `FrameRateLimit=0` for uncapped play, remain authoritative. This ordinary
startup preference does not change the 60/30 FPS acceptance gates. Pacing A/B
checks must use isolated profiles with identical graphics settings and copies of
existing user PSO caches, never reset real user preferences or caches.

**Release caveat, September 19, 2026:** the final BVH Shipping build admitted all
86 light-action operations and all 60 maximal-stress operations. The latest
recorded run averaged approximately 186 FPS in normal flight and 168 FPS in
light action, but light action contained four 21-35 ms frames and therefore did
not meet its strict per-frame 60 FPS floor. The deliberately extreme maximal
stress run averaged about 1.2 FPS; its explicit spectacle policy has no FPS
floor. Other jobs were running on the shared machine, so these numbers are
diagnostic observations, not uncontended performance certification or a general
FPS guarantee. The user approved release with this caveat. All raw failures and
historical policy interpretations remain preserved.

The final collision path verifies versioned actual-content fingerprints before
binding an asset, batches live physics-query setup, and conservatively uses
native BVH candidates with unchanged narrowphase/probes. Ordinary impacts
register at most one new collection per engine frame; maximum-tier impacts
retain the eight-collection ceiling. Damage remains queued, not discarded.

## Data and simulation limits

This is a bounded city core, not all of Dublin. OSM footprints, river and bridges
are combined with the 2015 survey's LiDAR and licensed imagery; source dates and
uncertain/excluded geometry differ. Roofs and bridges are approximations.
The 4096-square orthophoto covers about 94.76% of the core at 18.75 cm per pixel.
Four gaps deliberately use neutral material rather than invented photography.
Facade treatments vary, but about 93.23% of material assignments are contextual
inferences, not surveyed facade photographs.

The fracture baker uses schema 3: a 1.75 m target spacing, irregular cut spacing
and lightly roughened internal faces, with at most 192 cut cells and 384 physical
pieces per source building. Large buildings use coarser cells to stay within
that bound. A failed roughened cut retries at the same fine density without
noise before the final conservative repair. Existing schema-2 assets must be
rebaked before packaging; changing the recipe alone does not change saved rubble.
The editor bake queue does not depend on realtime viewport rendering; each
completed asset and its library record are saved so a restarted bake can resume.
The earlier solid-grid rebake produced 150,246 physical pieces (the previous recipe had
28,406), with a median of 156 pieces per volume rather than 16. The three
structural-pilot replacements brought that archive's mixed library to 150,587
pieces. It used the former 24-collection / 1,536-awake-piece limit. The current
city-wide source and library use the larger, finite catalog policy described
above; retained rubble is not deleted to make room.

Intact-city chunks retain the supplied building triangles; they do not switch
to simpler distance LODs. Generated city meshes report the aerial texture's
768 m-per-UV mapping to the texture streamer, rather than estimating density
from each small chunk's bounds. This preserves fine aerial mips where needed
without forcing residency or changing the global texture pool.
The coarse surveyed/reconstructed geometry and the
18.75 cm/pixel aerial imagery remain detail limits, independently of rendering
resolution, material filtering and texture residency.

Destruction, retained rubble,
terrain craters and bounded water waves are game simulations, not structural
engineering or fluid-dynamics models. Damage persists during the play session,
not across restarting the map. Bomb "tons of TNT" is a bounded game-yield tuning
parameter, not an explosives-engineering prediction.

Bomb land/building impacts can produce rising, drifting smoke for 45-55 seconds.
At most four columns share the existing 16-effect budget; distance, visibility
and significance culling can retire a column sooner. Water impacts never create
persistent smoke. Cannon water hits produce stronger upward spray and expanding
foam without changing projectile physics or water-wave impulse strength.

Keep `Content\Data\ATTRIBUTION.txt`, the georeference and facade provenance with
the game. They record OpenStreetMap/ODbL and Laefer et al./CC BY 4.0 attribution,
processing changes, imagery gaps and inference limits; F1 exposes credits in game.

### Structural-fracture pilot

The optional `StructuralPilot` recipe is restricted to three source volumes:
`osm/way/233804861`, `osm/way/233804877` and `osm/way/389853640`.
In the pilot archive, those three records use the pilot and the other 1,041
retain `SolidGrid`; the current city-wide library supersedes that mixture.
It derives 30 cm exterior walls, 20 cm base/floor/roof slabs and uniformly
distributed rooms from their existing convex, flat-roof envelopes. These
interiors and thicknesses are artistic inferences, not surveyed construction.
The original source data and exterior material mapping remain authoritative.

Unlike the solid-grid recipe, structural members receive non-lattice Voronoi
cuts. The aggregate target is 288 pieces per building, with the existing
384-piece maximum and that archive's runtime activation budgets unchanged. Root collision is
a compound of the child hulls rather than a solid hull filling the rooms.
Structural-volume conservation, empty-room queries, contact connectivity and
nonoverlap checks must pass before publishing a candidate.

The library remains schema 3; a per-record recipe distinguishes pilot assets
from existing `SolidGrid` assets. Legacy identities remain unchanged.
Structural baking requires an explicit selection of at most these three IDs;
`BakeAllBuildings` is rejected. A failed candidate does not replace a working
record. City-wide coverage uses the separate detailed recipes, not an
unrestricted rollout of the pilot's convex/flat-roof assumptions.

Native checks are under `DublinFlight.Destruction.StructuralPilot`.
Run each `DublinFlight.Destruction.PIE.StructuralPilot` comparison case
separately in fresh PIE. Captures and manifests are stored under
`Saved\Screenshots\StructuralPilot-20260918`, separated by source digest and
run ID. Encode clips using each frame's captured world timestamp, not an
assumed frame rate. Legacy captures are comparison baselines, not structural
acceptance; rendered inspection remains required, and recording is not an FPS
benchmark.

Timestamp-preserving, impact-aligned before/after videos are available in
`Saved\Comparisons\StructuralPilot-20260918`. Neighboring buildings can still
produce legacy solid-grid rubble. River water is not a rigid collision surface;
debris can sink through its terrain gaps, and no riverbed is inferred.
