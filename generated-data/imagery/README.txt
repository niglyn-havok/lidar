DublinFlight licensed orthophoto crop
====================================

Run from the project workspace root (the parent of DublinFlight):

  python .\generated-data\imagery\acquire_orthophotos.py prepare
  python .\generated-data\imagery\acquire_orthophotos.py download
  python .\generated-data\imagery\inspect_orthophotos.py
  python .\generated-data\imagery\process_orthophotos.py
  python .\generated-data\imagery\test_orthophotos.py

Uses available requests, numpy, scipy, Pillow, pyproj and tifffile. No new
dependencies were needed. No Unreal operations are performed.

Scope and restart safety
-----------------------
Only four tiles: 315500_234000, 315500_234500, 316000_234000, 316000_234500.
All archive HEAD lengths must be known and their total at most 2,000,000,000
bytes before downloading. At most two initial download workers; retries are
globally sequential, paused and limited to two. Metadata retries are also
paused and sequential. Existing matching archives are reused; conflicting
originals fail rather than being overwritten. Partial downloads are not
appended because the NYU server was previously observed to ignore Range.
progress.json is persisted early, during transfers and after each completion.
This budget covers archive sizes; separate disk headroom includes extracted
originals and image processing. Rerunning a partial transfer may reread bytes.

Original archive files and the eight extracted TIFF/TFW files are unchanged.
Each TIFF and TFW is verified against the existing publisher SHA-256 manifest
under metadata. Fresh archive SHA-256 values are retained per tile. Only
exact matching TIFF/TFW ZIP members are extracted, never extractall.
All ZIP members (including skipped directory entries) are first checked for
absolute paths, drive/UNC paths, dot/traversal segments, NULs, Windows alternate
streams, ambiguous trailing dots/spaces and symlink/special-file attributes.
An unexpected, duplicate or missing file rejects the archive. TIFFs are capped
at 500 MB expanded and TFWs at 16 KiB. Destination and staging paths must resolve
inside generated-data\imagery before writing, including existing link targets.
Only the approved TIFF/TFW basenames are streamed to those checked destinations;
archive directory names are never used to create extraction directories.

Output and projection
---------------------
DublinFlight\Content\Data\dublin-ortho.png
DublinFlight\Content\Data\dublin-ortho-georeference.json
DublinFlight\Saved\Automation\imagery-generation-handoff.json

The PNG is RGB, 4096 x 4096, exactly [315605,234009,316373,234777] metres,
west/south/east/north; north is at the top. Pixel spacing is 0.1875 m
(18.75 cm), NOT 5 cm. Native source samples are 5 cm and band four is
infrared, NOT alpha. RGB values are area-averaged with antialiasing before
rounding to bytes; no enhancement, hallucinated fill or sharpening is used.
Processing reads only required windows in 64-output-row chunks and joins
tiles before filtering, including fractional output pixels at seams.

The actual GeoTIFF projected identifier is EPSG:29902 (TM65 / Irish Grid),
not the legacy EPSG:29903 (TM75) label. Its raw geographic key is user-defined;
raw keys and ellipsoid parameters are preserved in the sidecar. The TIFF
corner transforms agree with each corresponding centre-based TFW.
Rendering deliberately preserves the shared numeric survey coordinates in
city-data-handoff.json, with NO per-layer datum reprojection or hidden offset.
This is not a claim that the two datums are equivalent. The old LAS custom
false-origin discrepancy remains documented. Absolute registration accuracy
needs independent surveyed control, beyond coordinate and pixel-value tests.

UE basis is east +X, south +Y, centimetres, origin E315989,N234393.
u=0.5+Xcm/76800; v=0.5+Ycm/76800. A 768 m texture must NOT be stretched over
the former 800 m or 1600 m extent. No imagery halo has been supplied.
The native editor coordinator owns import and material setup.
The full-core roof and ground consumers share this 768 m top-origin UV mapping.
Source credit text is ready for the consumer's later HUD/about implementation;
this pipeline does not alter the HUD, editor, C++ or configuration.

Approved honest partial rendering
---------------------------------
The later partial-import follow-up supersedes the earlier no-import gate:
partial rendering is approved, but complete visual coverage is not claimed.
Run publish_partial_import.py and test_partial_import.py to publish/verify
DublinFlight\Content\Data\dublin-ortho-validity.png, without changing the original
RGB PNG or its original georeference. The baseline hashes enforce preservation.
The output L8 mask contains only 0 and 255: 255 selects licensed imagery;
0 selects explicitly generic neutral rooftop/ground, not inferred source colour.
It thresholds the validated fractional mask at fully covered pixels, then adds
a one-output-texel neutral guard around missing coverage to prevent white RGB
fringes with bilinear LOD0. Sample mask with point/nearest filtering, sRGB off,
no mipmaps, clamp; never interpolate/blend the mask edge. Higher RGB mip/anisotropic
footprints are not certified. Dimensions, exact north-first UV alignment, gap
bounds in native source pixels/Irish Grid and confidence are in
partial-import-contract.json and the saved handoff. Full-coverage=false and the
final complete-visual gate remain explicit. Source gaps are not an engine blocker.

Validation and coverage
-----------------------
Tests verify exact bounds, pixel centres, north orientation, fractional area
weights, synthetic tile seams, RGB/IR separation, original published hashes,
archive budget, PNG hash/round-trip, full histograms, three recovered known
coordinates plus the four-tile junction, all image corners and 16 seeded
independent source integrations. Known coordinates are not certified GCPs.
The handoff is marked ready_for_import only after tests pass AND usable
source coverage is complete. The actual four source tiles have large white
survey gaps inside the core, so this run stops with a concrete coverage
blocker, not a misleading complete-texture claim. The exact candidate PNG
is retained unchanged; no synthetic, stretched or unlicensed fill is used.
validation.json records exact native coverage and suspicious/empty counts.
There is no declared source nodata tag; geometric extent coverage is not a
survey-validity mask. Zero IR alone is valid. All-four-band-zero holes fail.
All-four-band-white regions are conservatively identified as source voids
(tiny saturated pixels may also be excluded). Four large boundary-connected
white regions are visible in the source crop. core-validity.png records
inferred covered area per destination pixel: 0 void, 255 covered, intermediate
values are fractional coverage; it is NOT infrared used as alpha. Exact
conservative usable fraction and missing area are in the final handoff.
Historical changes, shadows, roof lean and source radiometric artifacts are
not corrected. core-preview.png is a convenience preview, not the deliverable.

Source and terms
----------------
2015 Aerial Laser and Photogrammetry Survey of Dublin City.
Debra F. Laefer, Saleh Abuwarda, Anh-Vu Vo, Linh Truong-Hong, Hamid Gharibi.
Survey acquired 26 March 2015; distributed by NYU Faculty Digital Archive.
Collection and all four full tile records explicitly identify CC BY 4.0.
Machine-readable source URLs, verification timestamps, HTML response hashes,
authors, acquisition date and licensing terms are in source-manifest.json
and the output georeference sidecar.

Source: https://archive.nyu.edu/handle/2451/38684
Survey: https://serv.cusp.nyu.edu/projects/ALSDublin15/ALSDublin15.html
Licence: https://creativecommons.org/licenses/by/4.0/

Derivative changes: crop, mosaic, infrared exclusion, RGB resampling to
18.75 cm output pixels. Retain creator/source/licence credit and the changes
notice, including visible application/game credits when used. Do not imply
endorsement or impose additional restrictions inconsistent with the licence.
Originals remain under their original licence; this does not license the
entire game or resolve other rights. No Esri or rights-uncleared imagery is
used in this pipeline.
