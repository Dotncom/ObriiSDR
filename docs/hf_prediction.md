# Optional HF Prediction

## Scope and Status

This is an isolated, opt-in monthly HF sky-wave planning module. It has no IQ,
RX, audio, radio tuning, network request, background schedule, or transmitter
dependency. Only an explicit Calculate command launches the native helper.
Root CMake, existing PropagationLabDialog, mainwindow, translations, runtime
packaging, Android, and the parent's GNSS/LRPT/scientific files were not edited.

Windows x64 MSVC/Qt 5.15.2 build and model/UI tests have been executed. Linux
x86-64/glibc ELF helper and both shared libraries also cross-compile successfully
with Zig 0.17.0 / Clang 22.1.8 (glibc 2.17 target). This Windows host has no
installed WSL/Linux/container runtime: native Linux execution/model fixtures
are still a release validation requirement, not a claimed pass.
The Linux runtime gate now selects the compiling Qt application's architecture
and pointer size, including Raspberry Pi AArch64/ARM32. Deterministic synthetic
ELF-header tests run on this Windows host; native ARM execution is still
unverified and must be tested on the target Raspberry Pi before release.

## Files and API

- `src/propagation/ituhf/hfprediction.h/.cpp`: independent types, validation,
  strict report parsing, optional engine discovery, cancellable asynchronous
  `hf::PredictionJob` using QProcess.
- `src/propagation/ituhf/hfimagevalidation_p.h`: internal ELF/PE architecture
  header checks shared with deterministic tests, not a public runtime override.
- `src/propagation/ituhf/hfpredictiondialog.h/.cpp`: standalone Qt dialog;
  manual controls, hour/frequency matrix, metric selection, cell details,
  explicit CSV export, stale-result invalidation.
- `src/propagation/ituhf/hfprediction_ui_en.json`: stable UI key-to-English
  fallback list for the parent's en/uk translations.json integration.
- `src/propagation/ituhf/CMakeLists.txt`: optional static Qt library only.
- `tools/itu_hf/CMakeLists.txt`: standalone native engine/helper build, tests,
  isolated install rules. It never invokes the application root build.
- `tools/itu_hf_helper.c`: low-priority launcher for the official CLI.
- `tools/itu_hf_linux_toolchain.cmake`: optional Linux ELF cross-build check;
  native Linux builds do not need it or Zig.
- `tools/itu_hf_fetch.ps1`, `tools/itu_hf_fetch.py`: selective pinned downloads.
- `tools/itu_hf_selftest.cpp`, `tools/itu_hf_modeltest.py`: Qt lifecycle/UI and
  portable model/fixture tests, respectively.
- `third_party/itu_hf`: untouched official C/header files, README/free-use
  notice, small fixtures, provenance note, Git blob manifest. No DLLs/data.

Public dialog API (UI thread only):

```cpp
HfPredictionDialog(QWidget *parent = nullptr);
using Translation = std::function<QString(const QString &key, const QString &fallback)>;
HfPredictionDialog(QWidget *parent, Translation translation);
bool setLink(hf::Point tx, hf::Point rx, double powerW,
             double txGainDbi, double rxGainDbi);
bool setRequest(const hf::Request &request);
hf::Request request() const;
void setRuntime(const hf::Runtime &runtime);
hf::Runtime runtime() const;
// Signal: void resultReady(const hf::Result &result);
```

Invalid imported requests return false, invalidate old results, and display an
error without silently clamping power, gains, SSN, coordinates or frequencies.
The setters never calculate. Closing cancels; controls are locked during a run.
Numeric UI precision is six decimal places for coordinates, three for watts
and bandwidth, two for gain/SNR. The job API keeps the original double values.
Default endpoints are unset/coincident, so a dialog cannot accidentally model
an invented route. Import both valid endpoints before use.

`hf::Point` has decimal-degree latitude/longitude. `hf::Request` also selects
year, month (1..12), smoothed SSN, bandwidth, required SNR, SNR exceedance
percent, P.372 noise environment, short/long great-circle path, UTC hours and
frequencies. Empty hours mean all 24; nonempty hours must be unique 0..23.
Frequencies are unique 1.60..30.00 MHz, up to 32, in 0.01 MHz steps. This is
deliberately narrower than the engine validator's 1.0 MHz lower bound and
matches the documented HF application range. Upstream CSV rounds frequency
to two decimals; finer input would conceal the actual grid, so is rejected.
Paths under 1 km are rejected; no silent terrestrial fallback is performed.

`hf::Sample` supplies basic/operational MUFs and deciles, median RX dBm, P.372
external noise factor, median SNR, upper/lower SNR decile deviations, SNR
exceeded at the requested percentage of the month, and basic circuit
reliability (BCR). `hasSignal=false` marks the upstream -307 dBW no-mode
sentinel: UI/export leave signal/SNR blank rather than presenting it as RF.

## Parent Integration

Parent owns all root/UI integration. After finding Qt5 Core/Widgets, opt in:

```cmake
option(FOBOS_ENABLE_HF_PREDICTION "Optional ITU HF planning dialog" OFF)
if(FOBOS_ENABLE_HF_PREDICTION)
    add_subdirectory(src/propagation/ituhf)
    target_link_libraries(ObriiSDR PRIVATE fobos_hf_prediction)
    target_compile_definitions(ObriiSDR PRIVATE FOBOS_ENABLE_HF_PREDICTION=1)
endif()
```

Use the actual application's target if it differs. The Qt library has no
link-time dependency on either ITU DLL: the application still runs when the
optional helper/data is absent. Build the helper separately with the commands
below; do not add the standalone `project()` build as a root subdirectory.

At the existing PropagationLabDialog's explicit HF action, snapshot its Link
parameters only when both endpoints exist:

```cpp
#ifdef FOBOS_ENABLE_HF_PREDICTION
#include "hfpredictiondialog.h"
// Store/reuse as a child dialog if desired; this code does not belong to RX.
auto *hfDialog = new HfPredictionDialog(this);
const bool imported = hfDialog->setLink(
    {parameters.tx.latitude, parameters.tx.longitude},
    {parameters.rx.latitude, parameters.rx.longitude},
    parameters.powerW, parameters.txGainDbi, parameters.rxGainDbi);
hfDialog->show();
// imported=false is a visible validation error, not a replacement estimate.
#endif
```

Do not seed the terrestrial frequency blindly (its default is 145 MHz).
`setLink()` preserves the dialog's HF frequency grid and climatology controls.
For a single supported HF frequency, supply a validated `hf::Request`.
No automatic mapping of Kp, NOAA F10.7, receiver sensitivity, fade reserve,
terrain, cable loss, antenna heights, or additional terrestrial loss occurs.
Power is transmitter power in watts; gains are constant isotropic offsets.
If TX feeder loss must be represented, explicitly reduce delivered watts
before import; report that correction and respect the engine's >=1 W limit.
RX feeder loss and receiver noise figure need a proper noise-budget extension,
not an unlabelled subtraction from a model gain. No directional pattern or
antenna-height database is fetched.

The optional callback uses the parent's JSON translation signature, not .qm:

```cpp
auto *hfDialog = new HfPredictionDialog(this,
    [this](const QString &key, const QString &fallback) {
        return translateUi(key, fallback); // parent's existing JSON lookup
    });
```

`translateUi` above is an integration placeholder, not a new required function.
Pass the parent's existing std::function directly when available. The old
one-argument/default constructor remains unchanged and delegates to the same
UI with no callback. No callback or an empty callback response uses the English
fallback. The callback is invoked only on the UI thread, must remain valid for
the dialog lifetime, and is supplied before controls are built. Recreate the
dialog after changing language to refresh existing static labels.

All UI titles, tabs, labels, choices, tooltips, status/result text, unit suffixes,
grid headers and file-dialog text use 58 stable `hf_prediction_*` keys. Copy the
flat key-to-English list from `hfprediction_ui_en.json` into the parent's English
catalog and add corresponding Ukrainian values in its existing format. Keep
format tokens `%1`..`%14`, literal `%`, newlines, file glob patterns, and leading spaces
in unit suffixes intact. CSV column names/metadata, frequencies, noise enums,
engine inputs, and scientific numbers are deliberately language-independent.
Raw model validation/process errors and OS file errors remain diagnostic text;
the callback prioritizes the normal UI/results/tooltips. This module does not
edit translations.json or load any catalog at runtime. It does not persist
coordinates or copy any QTH/map-provider credentials.

## Pin and License

Official upstream: [ITU-R HF](https://github.com/ITU-R-Study-Group-3/ITU-R-HF).
Source commit: `82017594a1c6cacfaa7e86954c4ae7b3a5825a3d`.
P533 reports version 14.2; the source implements the P.533-14 method and its
official P.372 noise implementation. Keep the actual pinned source/version
visible; do not claim this automatically implements future recommendations.

The official free-use statement and complete no-warranty disclaimer remain
verbatim in upstream README.md and source comments. This is a custom ITU
notice, not MIT/BSD/GPL or an inferred SPDX license. Older Common.h headers
also have 2018 all-rights-reserved language; nothing was deleted or rewritten.
Distributors should preserve both and obtain clarification/legal review if
necessary. The install rules retain C/header source and official notices.

No engine equations were patched. At compile time only the upstream CLI main
is renamed, allowing a separate launcher to restrict DLL search/lower priority.
Windows compiles fresh x64 P533.dll/P372.dll; tracked upstream binaries are
never downloaded or used. Linux compiles native libp533.so/libp372.so and links
libm/libdl, with `-fcommon` for the official shared-global C headers. The
Linux launcher preloads exact sibling SONAMEs and uses $ORIGIN runpath.
Linux CMake supplies `<errno.h>` by compiler include for MakeNoise.c's missing
upstream declaration and permits the upstream version date/time macros to
remain warnings. Source files stay byte-for-byte identical to the pin.

## Data and Size

Download only `P533/Data/ionos01..12.bin`, `COEFF01..12W.txt` (the source reader
uses text, not the older coefficient BIN files), and
`P1239-3 Decile Factors.txt`. No alternate P372/Data copy, huge antenna tree,
CCIR/URSI duplicates, text ionosphere maps, binaries, or repository history.

The download scripts validate the size and Git blob SHA-1 of each file against
the pinned tree. Already valid files are cached. SOURCE_MANIFEST.json retains
all selected source/data hashes. Runtime checks native binary architecture and
selected-month data presence/size; portable regression tests also verify blob
hashes. There is no in-app download, polling, or coordinate upload.

All 25 data files total **137,270,856 bytes (130.91 MiB)**, about **11.43 MB per
month** plus the shared decile table. Default storage is private local cache:

- Windows: `%LOCALAPPDATA%/ObriiSDR/itu_hf/data`.
- Linux: `~/.local/share/ObriiSDR/itu_hf/data` (Qt respects XDG data location).

The standalone helper install excludes datasets; the parent will include all
25 public model files in **build/runtime/itu_hf/data** alongside the helper/source
bundle. That complete release requires no end-user engine/data installation or
download. The private cache is a development/copy source, not a release
prerequisite. Runtime defaults already prefer `itu_hf/data` beside the application
over the cache; `Runtime::dataDirectory` can be set explicitly, and `ITU_HF_DATA`
overrides defaults for development. No packaging script/root CMake is edited
by this module; the parent owns copying and release manifest updates.
The helper and both libraries must be siblings. `Runtime::helperExecutable`
must be absolute, trusted, and match the compiling application's architecture
and pointer size. On Linux, Qt `Q_PROCESSOR_*`, `QT_POINTER_SIZE` and
`Q_BYTE_ORDER` select the expected ELF machine/class/byte order: AArch64=183,
ARM32=40, x86-64=62, x86-32=3. A 64-bit ARM application does not accept x86-64
or 32-bit ARM helpers, and an ARM32 application requires ELF32/EM_ARM. Unknown
processor targets fail closed. Header checks also reject invalid/truncated ELF
headers, unsupported file types and invalid header versions/sizes. On Windows,
support remains x64 only: the compiling application must be x86-64 with
64-bit pointers, and binaries must be AMD64/PE32+, not x86 or ARM64 PE files.
The UI label is **Native helper** under the unchanged `hf_prediction_helper`
translation key. Windows engine targets use static
MSVC CRT, so the helper itself does not need a separate VC runtime payload.
The application's own Qt dependencies are unchanged.
Verified Release helper/libraries total 814,080 bytes; the source-preserving
Windows install totals approximately 1.39 MB, with no datasets or Qt included.
The cross-compiled Linux ELF helper/libraries total 551,584 bytes, also without
datasets. Linux runtime execution is not implied by these build/size checks.

Verified Windows copying sources (all under private AppData):

- Built helper:
  `C:/Users/Svyat/AppData/Local/ObriiSDR/itu_hf/build/bin/Release/itu_hf_helper.exe`.
  `P533.dll` and `P372.dll` are in that same directory.
- Installed source-preserving bundle:
  `C:/Users/Svyat/AppData/Local/ObriiSDR/itu_hf/stage/itu_hf/`.
  Copy its helper, both DLLs, `source/`, and `source-notices/` into
  `build/runtime/itu_hf/`. This contains no datasets/private job files.
- All 25 pinned model datasets:
  `C:/Users/Svyat/AppData/Local/ObriiSDR/itu_hf/data/`.
  Copy only these public files into `build/runtime/itu_hf/data/`.
- Do not copy sibling `jobs/`, `build/`, `build-linux/`, `toolchain/`, caches,
  or UI test PNGs into the release.

The official readers have small narrow-character buffers. Dataset directories
must have an ASCII path <=180 characters including the trailing slash; on
Windows an existing Unicode path is converted to an ASCII 8.3 alias if
available. Otherwise the dialog reports the restriction. It never guesses a
truncated path. Private job cwd may contain Unicode because CLI filenames are
short relative names; data path validation is separate.

## Build and Verify

From the workspace in PowerShell, with the user's existing command approval:

```powershell
./tools/itu_hf_fetch.ps1             # all months, private data cache
# ./tools/itu_hf_fetch.ps1 -Months 1,4,5  # only the fixture/test months
$build = "$env:LOCALAPPDATA/ObriiSDR/itu_hf/build"
& 'C:/Program Files/CMake/bin/cmake.exe' -S tools/itu_hf -B $build -A x64 `
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64
& 'C:/Program Files/CMake/bin/cmake.exe' --build $build --config Release --parallel 2
$env:PATH = 'C:/Qt/5.15.2/msvc2019_64/bin;' + $env:PATH
& 'C:/Program Files/CMake/bin/ctest.exe' --test-dir $build -C Release --output-on-failure
```

Build with `-DITU_HF_BUILD_QT=OFF` when only the Qt-free native helper is needed.
Native Linux, without Qt (GCC/Clang, CMake and Python3 standard library):

```sh
python3 tools/itu_hf_fetch.py --months 1 4 5
cmake -S tools/itu_hf -B "$HOME/.local/share/ObriiSDR/itu_hf/build" \
  -DITU_HF_BUILD_QT=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOME/.local/share/ObriiSDR/itu_hf/build" -j2
ctest --test-dir "$HOME/.local/share/ObriiSDR/itu_hf/build" --output-on-failure
```

For Linux Qt tests use `-DITU_HF_BUILD_QT=ON` with Qt5 Core/Widgets/Test installed;
CTest uses offscreen rendering. Explicit data roots can be supplied to the
Python runner with `--data` or CMake `-DITU_HF_DATA_DIR=...`. Windows model tests
also run directly via `python tools/itu_hf_modeltest.py --helper <absolute-exe>`.
On Raspberry Pi, use the native compiler/Qt kit for the application's OS/ABI;
the isolated helper CMake permits 32-bit and 64-bit Linux builds. Build the
helper and both libraries together for that same target, not with the Windows
x64 option or x86-64 cross-toolchain. Native ARM execution/model tests remain
unverified here; matching header architecture is not an execution guarantee.

Optional Windows-to-Linux build check: set `ITU_HF_ZIG` to a trusted local
Zig executable, use Ninja, `-DITU_HF_BUILD_QT=OFF`, and
`-DCMAKE_TOOLCHAIN_FILE=<workspace>/tools/itu_hf_linux_toolchain.cmake` in a
separate build directory. This emits native Linux ELF files, not Windows
executables. Cross-builds intentionally do not register native execution tests.
The compiler/toolchain/cache is private build tooling, not included in the
portable bundle. Executing the resulting helper on native Linux is required
before shipping a Linux release.

Install only into a selected staging prefix (`cmake --install ... --prefix
<private-staging> --config Release`) to inspect the optional bundle. No runtime
packaging script or release directory has been changed by this work.

Verified regression coverage:

Final Windows CTest: **2/2 suites passed** (Qt job/dialog tests and the seven
portable model/provenance tests). Linux ELF cross-build: passed; native Linux
execution: not run on this host. Tests use published/synthetic coordinates,
never the operator's station or private saved settings.

- Published `1-5-85.in/.out`: all 12 P.372 noise values, including total noise
  51.31 dB, within 0.011 dB of the two-decimal official fixture.
- Published `itu_old22012020.in/.out`: antenna-independent distance 4477.003 km,
  basic MUF 27.770, MUF90 21.752, MUF10 36.427, operational MUF 36.101 MHz,
  operational deciles and total external noise, within 0.016 of rounded fixture.
  RX directional pattern is explicitly replaced with isotropic only for this
  test; directional RX power/SNR/reliability are NOT claimed identical.
- 20-cell hour/frequency grid, UTC endpoints 00/23, distinct hourly MUF/SNR,
  finite results, bounded BCR, correct MUF/exceedance ordering.
- Intermediate 7000..9000 km and long great-circle paths; analogue BCR and
  finite model fields, with the Qt long-path adapter also exercised.
- 10x watts adds 10 dB to RX/SNR; +3 dBi adds 3 dB; 10x noise bandwidth lowers
  SNR by 10 dB without changing received power; BCR moves consistently.
- Every cached month runs a native model case; source/data blob provenance.
- Qt validation, CSV shape/content/finite-value checks, dBW->dBm and UTC
  conversion, incomplete results, missing helper/data, reentry rejection,
  cancellation, timeout, temporary job cleanup, seeding and invalidation.
- JSON translation callback, English catalog consistency, translated result
  placeholders/headers/tooltips, cancellation status and English fallback.
- Deterministic ELF-header target matrix for AArch64, ARM32, x86-64 and x86-32;
  wrong machine/class/byte order, truncation, bad signature/version/header size;
  Qt compiling-host selection; Windows PE32+ AMD64 acceptance and x86/ARM64/PE32
  rejection. These tests do not imply native ARM execution passed.
- Offscreen desktop/compact dialog snapshots stored only in private AppData.

## Scientific and Runtime Limits

These are monthly climatological distributions, NOT a live forecast for a
particular date or an instantaneous measurement. Year is accepted metadata;
the map selection is month/hour and manually supplied R12. SSN is smoothed
sunspot number on the source model's scale, not Kp, daily counts or F10.7.
Upstream permits 1..311 and does not accept zero; its foF2/M(3000)F2/foE
interpolation internally caps SSN at 160. No silent conversion from modern
sunspot series or solar flux is provided. There is no storm/flare/auroral
update, ionosonde assimilation, real-time ionosphere, terrain diffraction,
building clutter, surface wave, sporadic-E guarantee, or measured interference.

This initial interface intentionally selects ANALOG to expose meaningful SNR
and BCR. Pinned CircuitReliability.c computes digital OCR/OCRs only under its
DIGITAL branch and short/intermediate distance conditions; the analogue
report's zero OCR fields are therefore not plotted or exposed as predictions.
Do not label BCR as total digital/FT8/modem success, or a certified service
availability. A future digital UI needs explicit amplitude/time/frequency
windows and SIR inputs, plus separate validation/tests.

P.372 noise is external atmospheric/man-made/galactic noise; receiver noise
figure, antenna mismatch/polarization, local interference and real antenna
elevation pattern are not included. Required SNR applies to the specified
bandwidth. MUF90 is the frequency exceeded on 90% of days, not a 90% confidence
interval or a universal operating frequency. There is no separate LUF output;
the sampled SNR/BCR grid must not be advertised as an exact LUF determination.

`ITURHFProp.h` at the pin defines normal `RTN_MAINOK=0` (validated by source
inspection and executed fixture tests). The Qt adapter also accepts 1000 for
older official wrappers, but only with normal process exit AND a complete,
strictly validated requested result grid. Exit code alone never means success.
POSIX truncates 1000 to 232; this native pinned helper returns 0 instead.

Jobs are below-normal priority processes, bounded to 768 samples, with a
120-second default deadline, 2 MiB maximum report, no shell interpolation,
drained/discarded stdout/stderr, and cancellation by process kill. Requests and
reports are disposable per-job private-cache files removed after completion,
failure, cancellation and normal object destruction. Destruction waits at most
3 seconds only to reap a killed process; normal calculations never block the
UI. A crash/power loss can leave a private job directory behind: do not include
that cache in diagnostic bundles or release artifacts. CSV export is explicit
and clearly includes coordinates; no coordinates appear on the command line.
