# Radio Propagation Lab / HF Conditions

Open **Spectrum measurements -> Radio propagation / HF**, or the DSP board's
**Radio propagation / HF** block. This is an independent planning tool: it
does not tune a receiver, transmit RF, or continuously analyze IQ.

## Terrain and Map

Choose **SRTM HGT**, **MapTiler online**, or **Flat reference** as the elevation
source. Local SRTM requires a directory containing uncompressed `.hgt` tiles. For example,
`N49E030.hgt` covers latitude 49..50 N, longitude 30..31 E; negative tile
coordinates use `S`/`W`. Both 1201x1201 and 3601x3601 signed big-endian files
are supported, including NASA's `N49E030.SRTMGL1.hgt` and
`N49E030.SRTMGL3.hgt` naming variants. Heights are meters above mean sea level, not antenna heights.
The reader maps at most eight files and interpolates their samples. Missing
tiles and SRTM voids are never silently replaced with zero elevation.

TX/RX markers and the Link controls display `terrain / antenna top m MSL`:
for example, `150.0 / 160.0 m MSL` means 150 m terrain plus a 10 m AGL antenna.
With local HGT, the values update after point, folder, or antenna height changes
without running coverage or requesting elevations online. With MapTiler online,
the endpoint heights are fetched after the separate per-session consent is checked;
calculation loads the required Terrain RGB tiles into memory before evaluating
the path or coverage. A
missing tile/void displays `-- / -- m MSL`; explicit Flat reference uses 0 m
terrain and is marked as a synthetic reference on the map. Map imagery alone
cannot supply these elevations.

Windows elevation requests use the system HTTPS transport when OpenSSL is not
available to Qt, like the base map. No separate OpenSSL installation is needed.
Requests run off the GUI thread; moving markers or closing the window cancels
outdated endpoint requests. Error details distinguish network/TLS failures from
HTTP 401/403 (key or service access) and 429 (quota/rate limit), without including
API keys or coordinates. Base-map access does not necessarily grant access to the
elevation service.

Data reference: [NASA SRTMGL1](https://www.earthdata.nasa.gov/data/catalog/lpcloud-srtmgl1-003),
[SRTM User Guide](https://lpdaac.usgs.gov/documents/179/SRTM_User_Guide_V3.pdf).
SRTM can include the radar surface of trees/buildings; it is not a detailed
bare-earth, building, or vegetation propagation model. DEMs and geographic map
imagery are separate inputs.

The map reuses the QTH map's configured local XYZ directory or online provider
(including MapTiler and its already saved API key). Select **QTH: provider**
or the independent OpenStreetMap option. Online tiles still require the separate
**Allow online map** checkbox: a request discloses the displayed geographic
area to the provider. Keys are resolved in memory, not copied into exports or
diagnostic messages. The tool disables online map disk caching. Map image
tiles do not supply elevations; the independent MapTiler Terrain RGB service
does. For offline use, download SRTM for the entire calculation area, unpack
`.hgt`/`.hgt.zip` files into the selected HGT folder, and preserve their
original names. ZIP archives are not read directly.

Choose **Transmitter (TX) / Receiver (RX)** and click to place or replace the
corresponding point, enter coordinates, or copy an existing QTH user marker.
The QTH dropdown selects a saved marker from the main map. **QTH to TX / QTH
to RX** copies that marker's coordinates into the chosen endpoint, centers the
map there and reports the copied name. These are import actions, not map-picking
modes; they are disabled when the list is empty or a calculation is running.
**Ctrl + left-click** places/replaces TX and **Shift + left-click** places/replaces
RX in any mode, without changing the selected mode. **Left-drag an existing
TX/RX marker** to move that endpoint in any mode, independently of the selected
marker class. Coordinates and distance update during dragging, old coverage is
invalidated and the new position is autosaved. A click without dragging leaves
the marker unchanged. Ordinary dragging on empty space in Pan mode moves the
map; the wheel zooms it. **TX <-> RX** swaps points (including
whether a point exists), antenna heights, gains and cable losses. Transmitter
power and receiver sensitivity keep their respective roles.
**Distance TX-RX** below the map updates immediately as points are placed or
coordinates are edited, in meters or kilometers. It uses the same approximate
great-circle distance as the propagation model, not terrain-profile length or
antenna-tip distance. A missing endpoint clears the reading; identical points
show zero. No DEM, network request, or coverage calculation is required.
Right-click a marker to remove it; right-click empty map space while a class
is selected to place it. **Pan** restores map dragging. There is one TX and
one RX in this single-link tool; removing TX prevents coverage calculation,
and both markers are required for a profile. Heights are above
the local terrain (AGL). Parameters, point coordinates/presence, map view,
chosen map source, layer, opacity and window layout are saved automatically
after a short idle interval and on close in the per-user `ObriiSDR.ini`
(`propagationLab` group). This is private local state: points are not written
to diagnostic logs or included in release packages. Treat an explicit settings
backup as private because it now includes these points. Resolved provider API
keys are not copied into this group's settings. Online-map and online-elevation
permissions remain separate and session-only; neither is restored automatically.
MapTiler elevation requests disclose the endpoint coordinates and the requested
terrain area to MapTiler. The QTH map's user-supplied MapTiler key is reused in
memory. Terrain RGB tiles are held only for the current calculation, not saved
as a local DEM; the local HGT option remains available offline. Each calculation
downloads at most 96 terrain tiles and adapts tile zoom to the geographic span,
so very large areas may have coarser elevation detail. Missing online terrain
is not replaced with a flat surface. Profile CSV is an explicit
export that contains TX/RX coordinates. Calculated coverage rasters are not
persisted; recalculate after reopening.

## Models and Link Budget

- **Terrain + dominant knife edge**: free-space path loss plus a single
  dominant obstruction estimate. Effective Earth-radius k (default 4/3)
  affects curvature and the line-of-sight/Fresnel geometry. This is not a
  complete multiple-obstacle diffraction or ground-wave model.
- **ITM / Longley-Rice**: established NTIA terrain-based statistical model,
  20 MHz..20 GHz, confidence/reliability, climate, polarization, refractivity,
  ground permittivity and conductivity. Its path-loss result already includes
  free-space loss. Below 1 km the tool explicitly uses the knife-edge estimate
  and counts these fallback cells. ITM uses its own refraction model; the k
  setting is only for the displayed geometry and knife-edge mode.
- **Free space**: ideal reference ignoring terrain loss, not a claim that
  obstacles or the radio horizon disappear. It can return reference levels
  without DEM data. **Flat reference (0 m)** is an explicit synthetic terrain
  option, never an automatic missing-data fallback.

Received dBm = 30 + 10 log10(TX watts) + TX/RX gains (dBi) - cable losses -
additional losses - model path loss. Link margin subtracts receiver sensitivity
and fade reserve. Antenna gain is direction-independent in this version.
Neither receiver noise, interference, terrain uncertainty, nor building clutter
is automatically estimated. Coverage is a planning approximation, not a
certified service guarantee. HF ionospheric paths, MUF/LUF, sky-wave hops and
surface-wave attenuation are not implemented by these terrestrial models.

**Fade reserve** is subtracted from the reported link margin, not from the
predicted RX level: `margin = RX dBm - sensitivity dBm - fade reserve dB`.
For example, -90 dBm RX and -110 dBm sensitivity give 20 dB before reserve,
or 10 dB with a 10 dB reserve. It is a user-selected design allowance for signal
dips, not a predicted fading probability. ITM confidence/reliability already
model statistical variability, so an additional reserve is deliberately more
conservative, not a substitute for those percentages.

ITM climate, refractivity, ground conductivity/permittivity and polarization do
not select rural/suburban/urban clutter. No building-density model is implemented
yet. **Additional loss** permits an explicit user-supplied fixed correction,
but does not map individual buildings or vary with land use. A future clutter
layer requires a separate model (for example ITU-R P.2108) and suitable data;
terrain/vegetation already present in SRTM must not be counted twice.

References: [NTIA ITM](https://github.com/NTIA/itm),
[ITU-R P.525](https://www.itu.int/rec/R-REC-P.525/en),
[ITU-R P.526](https://www.itu.int/rec/R-REC-P.526/en).

## Calculation and Profile

Calculations start only by button, on a low-priority worker, with cancellation.
No receiver callback or audio worker runs the coverage calculation. Changing
inputs clears stale results instead of displaying an old estimate for new
parameters. Color/opacity/layer changes reuse the completed raster.

Coverage: radius 0.1..200 km, grid 32/64/96/128/256/512 per side, uniformly sampled
in map Mercator coordinates. The default remains 64x64; 512x512 allocates 262144
cells and requires 16 times as many path calculations as 128x128. Greater grid
density does not add detail missing from the elevation data or ray profiles.
The eight-stop palette runs from violet through blue, cyan, green, yellow,
orange and red to a light high-level endpoint. **Overlay** adjusts the RX-power
and link-margin color limits, saved locally. Defaults span -150..-20 dBm and
-40..+40 dB respectively; elevation uses an automatic MSL range. Changing these
limits recolors the existing result without recalculating paths or changing
numerical RX levels/margins. Missing cells remain
transparent. The central near-field region is not predicted. Warnings, missing
tiles and short-path fallback counts are shown.

The single **TX-RX profile** button calculates and opens a separate resizable
window. Clicking again reopens the cached profile without recalculating; editing
endpoints or propagation parameters invalidates it, so the next click calculates
a fresh result. There is no separate duplicate calculate/view action.
The selected TX-RX profile shows terrain plus
Earth curvature, antenna tips, line of sight, full and 60% first Fresnel zone,
distance, attenuation, received level, margin and minimum clearance. The
dominant-edge value is a separate estimate, not an ITM loss decomposition.
The requested terrain step is capped at 4096 samples for the detailed profile
and 256 samples for each coverage ray; actual spacing can therefore be larger.
Narrow obstacles can be missed by a coarse coverage grid/profile. Inspect the
detailed path before drawing conclusions. There is no computed confidence
interval for DEM/model error.

## HF / Sun

**Refresh NOAA** manually obtains global F10.7 solar flux (sfu), its available
90-day mean, and planetary Kp. Observation times are shown in UTC separately
from download time. Data older than 36 hours (flux) or 6 hours (Kp) is flagged;
a failed refresh retains the previous values with their original timestamps.
There is no automatic polling or transmission of coordinates/callsigns.

Sources: [NOAA F10.7](https://www.spaceweather.gov/phenomena/f107-cm-radio-emissions),
[NOAA planetary Kp](https://www.swpc.noaa.gov/products/planetary-k-index).
These are HF context, not a point-to-point propagation forecast. Data feeds:
`https://services.swpc.noaa.gov/json/f107_cm_flux.json` and
`https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json`.

## FT8 Countries and Stations on the Map

FT8 filters include 340 DXCC entities from the bundled CTY snapshot (2025-01-27),
search, editable prefixes, exact `=CALL` exceptions, portable-location prefix
selection, and import of a newer local `cty.dat`. WAE-only CTY entries are not
separate DXCC rows. All countries are allowed by default. First migration
retains existing country/custom rules; the former Russia rule is transferred
to Asiatic Russia, European Russia and Kaliningrad. DXCC entities are radio
award entities, not necessarily sovereign countries. Prefix inference is not
proof of a person's location or nationality.

**FT8 Station -> Log -> Stations to map** exports logged stations with valid
Maidenhead locators into the existing QTH map. Labels use callsigns; positions
are cell centers. Duplicate QSOs use the latest valid recorded locator, and
repeated export updates FT8 markers rather than multiplying them. Own-station
coordinates and records without a valid locator are excluded. Only this explicit
button changes QTH markers; it does not enable PSK Reporter/QRZ/LoTW uploads.

## Generator Rate (TX)

`Generator rate` is the generated complex-IQ sample rate, not the RF carrier
frequency or voice bandwidth. At 48000 samples/s the generated spectrum is
limited to approximately -24..+24 kHz around its center, with filter guard
bands reducing the usable range. HackRF separately resamples this stream to
its device IQ rate (2..20 Msps). `RF carrier` sets the actual radio frequency;
`Transmitted signal bandwidth` and FM deviation determine occupied bandwidth.
Raising the generator rate alone does not widen the voice filter or improve
range/power.
