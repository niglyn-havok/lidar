# Dublin playable-city derivative

This offline Python pipeline writes the **fixed schemaVersion 1** city contract
to `DublinFlight\Content\Data\dublin-city.json`. It does not run Unreal, process
LAZ, acquire/process imagery, or modify source datasets.

## Reproduce from the workspace root

```powershell
# First acquisition only: two bounded sequential OSM queries; cache reused later.
python .\generated-data\acquire_osm.py

# Offline reconstruction, regression tests, persisted-output validation.
python .\generated-data\city_pipeline.py

# Independent checks without downloading or rewriting the output.
python -B -m unittest discover -s .\generated-data -p "test_*.py" -v
python -B .\generated-data\city_pipeline.py --validate-only
python -B .\generated-data\apply_facades.py --check-only
```

Dependencies: existing NumPy, SciPy, Shapely 2.1+ with GEOS 3.10+, and pyproj.
No packages are installed by these scripts. Workspace discovery is relative to
the script, not a username or working-directory assumption. Acquisition has a
10,000,000-byte response cap, a 40-second server query limit, 65-second network
timeout, at most three sequential attempts per query, and delayed retries.
`--refresh` explicitly requests a new OSM snapshot; ordinary reruns verify and
reuse cached source SHA-256 values.

## Windows environment and exact source bytes

Use an isolated environment if the machine's Python packages are missing or
broken; do not replace unrelated global packages. From the workspace root:

```powershell
python -m venv .\generated-data\.venv
$Python = (Resolve-Path .\generated-data\.venv\Scripts\python.exe).Path
& $Python -B -m unittest discover -s .\generated-data -p "test_*.py" -v
```

If imports report missing dependencies, install the tested Python 3.13 package
set below, then rerun the checks above with `& $Python` instead of `python`.
Do not treat other test failures as a reason to install or refresh source data.

```powershell
& $Python -m pip install "numpy==2.5.3" "scipy==1.18.1" "shapely==2.1.2" `
    "pyproj==3.8.0" "requests==2.34.2" "Pillow==12.3.0" "tifffile==2026.9.9"
```

Source SHA-256 values bind the original bytes, including line endings.
`.gitattributes` disables text conversion for the raw hydrology response and
the four original orthophoto TFW files. Existing LFS rules preserve the other
binary/large inputs. The separately hashed generated facade provenance uses
LF, while the orthophoto georeference and partial-import contract use their
original CRLF endings. A blanket JSON newline rule would break these contracts.

`test_checkout_bytes.py` exercises actual Git checkouts in temporary repositories
with `core.autocrlf=true`, `false`, and `input`, and verifies the current raw
source hashes. Updating attributes does not necessarily rewrite an existing
working file. For an old checkout with newline drift, first verify the indexed
Git blob against the retained source SHA-256 and establish that the working
copy differs only by newline conversion. Export that exact path to a temporary
checkout, verify its hash, then replace only the verified stale copy. Preserve
unrelated local edits; never weaken a digest, normalize arbitrary downloads at
read time, or use `--refresh` to hide a cache mismatch.

The read-only checks do not regenerate the city or historical handoffs.
For missing original imagery ZIP caches, use the report-preserving `restore`
command documented in `imagery\README.txt`, not the acquisition/rebuild workflow.

## Geometry and coordinate contract

- Exact 768 m square: E315605–316373, N234009–234777, legacy EPSG:29903.
- Origin E315989/N234393; centimetres, +X east, +Y south, +Z up.
- Terrain: 385×385 regular 2 m slots, north-first row-major. All vertex/UV
  slots remain present; triangles inside OSM water are omitted. The runtime
  owns chunking.
- Water: constrained triangulation of OSM polygons with holes/islands retained.
  H1.1 m is an **artistic provisional tide**, not surveyed tide or bathymetry.
- Buildings: local pivots, separate closed solids, roof/wall/foundation material
  IDs 0/1/2. Relation members and duplicate shells are suppressed. Detailed parts
  receive priority, with uncovered parent residuals retained. Exact core
  clipping introduces **flagged artificial closed crop walls**.
- Roof/terrain UVs are global top-down:
  `u=.5+Xcm/76800`, `v=.5+Ycm/76800`. Wall UVs are **metres**:
  U is cumulative boundary distance and V is height above the local base.
  They deliberately use values outside [0,1]; material repeat spacing belongs
  to the runtime facade material. No measured-facade claim is made.
- **UE clockwise front faces**: outward normal is
  `cross(C-A, B-A)`, not `cross(B-A, C-A)`. Correspondingly, the exported
  closed-solid signed volume is the negative of the conventional right-handed
  determinant sum. Do not reverse indices without also changing normals.
- UV/hard-edge seams intentionally duplicate render vertices. Geometry QA
  welds exact coincident positions before verifying two oppositely oriented
  incident faces per edge.

## Ground, roofs and landmarks

Ground is rebuilt from lower-quartile **surface** samples outside buffered
building, water and bridge footprints. The misleading cached class-2 terrain
is not used. Wider low-elevation support rejects elevated rooftop islands
left by a mismatch between 2015 LiDAR and contemporary OSM footprints.
Interpolation beneath buildings has recorded support distances; this is not a
certified bare-earth DTM.

Roof fitting uses measured, populated interior raster cells. Supported planes
and two-plane ridges are retained, including real courtyard holes. Ambiguous
roofs receive an explicitly coarse robust-median flat cap. Explicit OSM heights
are the next fallback; levels assume 3.2 m/storey and carry lower confidence.
Unsupported or unsafe volumes are deferred with their IDs and reasons rather
than manufactured silently. `min_height`, when usable, preserves suspended
bottom closures; other volumes have an explicitly artificial 1 m foundation.

River bridges/boardwalks are separate low-raster-height closed slabs, never
ground across the river. A supported Ha'penny Bridge arch is reconstructed;
piers, railings, trusses and precise deck thickness are not recovered. The
maximum-return raster can blend railings/vehicles into bridge heights.

Exactly one separate Spire replaces the OSM Spire shell: XY
[-8550,-27950] cm, base H645 cm, tip H12460.9 cm. Dimensions come from the
previous verified report, not a new raw-point measurement. A small explicitly
artistic grade pad prevents a floating base.

## Artifacts and evidence

- `sources\osm-*.json`: unchanged acquisition responses.
- `sources\osm-*-provenance.json`: queries, endpoints, timestamps, byte counts,
  licensing and SHA-256.
- `city-provenance.json`: full source/transform provenance, per-volume methods,
  confidence/evidence, omissions, diagnostics, geometry counts and caveats.
- `clean-ground-support.npz`: compact independent-ground support nodes.
- `test-results.json`: persisted regression-test results.
- `DublinFlight\Saved\Automation\data-generation-handoff.json`: early/progress/
  final status for the native runtime and coordinator.

Both the cache and saved JSON are checked; output writing uses an atomic
same-directory replacement. Original `artwork\surface.npz` is SHA-256 checked
before and after processing. No ortho file is accessed.

Preserve survey CC BY 4.0 authors/source/changes and © OpenStreetMap
contributors / ODbL 1.0 notices embedded in the output and provenance.
Public distribution of the enriched OSM-derived database needs the applicable
attribution/share-alike obligations addressed separately. Data validation is
not Unreal collision, saved-map appearance, destruction or frame-rate
acceptance. Registration and 2015/current-building changes remain explicit
limitations.
