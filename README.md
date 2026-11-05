# Photo Denoise 1.3.1

Modular C11 photo denoising for Windows x64. Interactive terminal UI, automatic
model installation, DirectML GPU inference on compatible AMD/NVIDIA/Intel devices,
and CPU fallback. The application code is C; ONNX Runtime, DirectML, and LibRaw are
external native libraries. Python and .NET are not needed to run the application.

## Quick start

The ready-to-run package is `dist/denoise/`. Keep `denoise.exe`, `onnxruntime.dll`,
`DirectML.dll`, and `libraw.dll` together. Other PCs need the Microsoft Visual C++
x64 runtime. Models always live in **`models` beside the executable**, regardless
of the working directory. The old `--models` directory override was removed.

```powershell
# Open the model library TUI (a trailing dot is accepted too).
.\dist\denoise\denoise.exe model
.\dist\denoise\denoise.exe model .

# Process an image with a specific model.
.\dist\denoise\denoise.exe "E:\photos\image.CR2" 100 --model scunet_color_50
.\dist\denoise\denoise.exe "E:\photos\image.DNG" 50 --model scunet_color_real_psnr
.\dist\denoise\denoise.exe "E:\photos\image.CR2" 70 --model nafnet_sidd_w64
.\dist\denoise\denoise.exe "E:\photos\image.CR2" 70 --model restormer_real
.\dist\denoise\denoise.exe "E:\photos\image.jpg" 50 --output "E:\photos\clean.tif"
```

Interactive terminals show a full-screen interface instead of scrolling progress
logs. Run without arguments for the image-path/strength form. During a job, the UI
shows decoding, cache verification, download progress, model loading, inference,
and output saving. After processing, the UI closes automatically, restores the
terminal, prints only the absolute output path to stdout, and exits. No final
keypress is needed. Errors go to stderr with a nonzero exit code. Press
Esc/Ctrl+C to request cancellation, which waits for the current network/runtime
call to return. Model-install screens still use Enter/Esc to return to the library.

The model library **fetches the online catalog on entry**. R fetches it again;
Up/Down or J/K, Home/End and PageUp/PageDown navigate the scrolling list, Enter
installs/repairs, and Esc/Q leaves. It shows descriptions, RGB/grayscale
requirements, disk size, verified installed status, and qualitative performance
notes across several architectures; no invented GPU timing or universal-best
claims are presented. A terminal of at least 72 columns by 24 rows is recommended.

For scripts or redirected output:

```powershell
.\dist\denoise\denoise.exe model list
.\dist\denoise\denoise.exe model refresh
.\dist\denoise\denoise.exe model list --offline
.\dist\denoise\denoise.exe model install scunet_color_50 --no-tui
.\dist\denoise\denoise.exe "E:\photos\image.CR3" 100 --model scunet_color_50 --no-tui
```

Plain English progress is automatic when standard input/output is redirected.
Progress goes to stderr; successful image-processing stdout is exactly one path
followed by a newline, including for strength 0.
`model` without a subcommand requires an interactive terminal.

## Windows installer

Download `PhotoDenoise-1.3.1-x64.msi` from the GitHub release. The per-user installer
uses `%LOCALAPPDATA%\Programs\PhotoDenoise`, adds it to the user PATH (open a new
terminal), and creates Start menu shortcuts. Models remain in the writable
`models` directory beside the executable. The MSI includes native dependencies,
the Visual C++ runtime DLLs, licenses, and the model catalog; weights are downloaded
on demand. Upgrades/uninstall preserve downloaded weights and user files.

```powershell
denoise.exe model
denoise.exe "E:\photos\moon.CR2" 25 --model scunet_color_15

# Build the MSI after building the executable. Requires .NET 8 SDK only to build WiX.
.\scripts\installer.ps1
# Unattended installation:
msiexec /i PhotoDenoise-1.3.1-x64.msi /qn /norestart
```

## Model selection and downloads

| Catalog ID | Intended input/use | Installed size |
| --- | --- | --- |
| `scunet_color_15` | RGB, light Gaussian noise (sigma 15) | ~73.37 MiB |
| `scunet_color_25` | RGB, moderate Gaussian noise (sigma 25) | ~73.37 MiB |
| `scunet_color_50` | RGB, heavy Gaussian noise (sigma 50) | ~73.37 MiB |
| `scunet_gray_15` | Grayscale, light Gaussian noise | ~73.37 MiB |
| `scunet_gray_25` | Grayscale, moderate Gaussian noise | ~73.37 MiB |
| `scunet_gray_50` | Grayscale, heavy Gaussian noise | ~73.37 MiB |
| `scunet_color_real_psnr` | RGB, mixed real-world photo noise; fidelity-oriented | ~73.37 MiB |
| `scunet_color_real_gan` | RGB, perceptual cleanup; may invent texture | ~73.37 MiB |
| `nafnet_sidd_w64` | High-capacity real-noise denoiser, SIDD width64 | ~446.08 MiB |
| `nafnet_sidd_w32` | Smaller NAFNet real-noise alternative, SIDD width32 | ~111.66 MiB |
| `restormer_real` | Transformer for real-image noise | ~102.04 MiB |
| `swinir_color_25` | Gaussian sigma25 specialist; fixed 128x128 tiles | ~52.32 MiB |
| `drunet_color` | Convolutional denoiser with continuous sigma-map control | ~124.54 MiB |

The largest/high-capacity choice here is **`nafnet_sidd_w64`**. It is not a claim
that one model wins on every camera/noise pattern. Restormer and blind SCUNet
are alternatives for real noise. A stronger Gaussian setting or GAN sharpness
does not automatically mean higher fidelity. Compare detail and noise on your
own image; no supported model is advertised as the most powerful in absolute terms.

The catalog is fetched from
[`catalog/models.json`](https://github.com/Artur73737/denoise/blob/main/catalog/models.json),
not hardcoded to a single model/family. A validated copy is stored in the local
`models/catalog.json`; unavailable/invalid updates fall back to the last valid
copy or the embedded build-time snapshot. New entries using supported adapters
can appear without rebuilding. Each row declares exact artifacts, hashes, sizes,
URLs, licenses, input channels and tile constraints. Catalog validation rejects
unsupported adapters, malformed URLs/paths, duplicate entries and invalid sizes.
See [catalog maintenance and provenance](catalog/README.md).

`--model` accepts these exact IDs, the corresponding `.onnx` filename, or aliases
`auto`, `real`, `15`, `25`, `50`. A named model overrides automatic selection at
every strength. A grayscale model on a color image is rejected rather than
silently discarding color. Arbitrary external ONNX paths are not supported.

Automatic selection is a documented heuristic: grayscale/RGB plus strength
1-33 -> sigma15, 34-66 -> sigma25, **67-100 -> sigma50**. It does not estimate the
image's actual noise. Network output is blended with the original using
`strength / 100`. Changing networks at the thresholds can cause visual jumps;
pin a model to keep the same network throughout a strength adjustment. DRUNet
additionally receives a fourth constant noise-map channel: sigma is
`50 * strength / 100`, normalized by 255. Its RGB output still uses the same blend.
Fixed-shape exports such as SwinIR automatically use the catalog's required tile.
Strength 0 makes an exact byte-for-byte copy and needs no model.

SCUNet variants have a ~3.8 MB graph plus ~73.1 MB external weights. Other entries
use single-file ONNX artifacts of the sizes shown above. Every declared file must
be present and SHA-256 verified before inference. Downloads use HTTPS, pinned file
sizes and SHA-256 values, an immutable upstream revision, and atomic installation.
The UI shows bytes, percentage, transfer speed and ETA once enough data arrives.
Interrupted transfers stay in `.download` files and resume using HTTP Range.
Transient errors are retried up to three times. Complete but corrupt partials
are discarded and redownloaded; installed files are replaced only after validation.
Old inactive `.part-PID-*` files can be adopted without modifying the old file.
`--offline` requires a complete verified pair and never starts a download.

The previously interrupted strength-100 release download was incomplete at
54,001,664 / 73,138,176 weight bytes. Recovery from that partial, SHA-256 validation,
and strength-100 inference were tested successfully. A first install can take
time; the UI now distinguishes downloading from loading the inference session.

Model sources, immutable revisions, licenses and artifact hashes are recorded in
`catalog/models.json` and `catalog/README.md`. The four Adobe `.data` containers in `winml/` remain
unchanged and are not loaded: their proprietary wrapper has not been integrated.

## Input and output formats

RAW input uses LibRaw 0.22.3. Its installed build lists 1,258 camera models and
supports CR2/CR3/CRW, DNG, NEF/NRW, ARW/SR2/SRF, RAF, ORF, RW2, PEF, SRW, 3FR,
FFF, IIQ, MOS, X3F and other RAW families. Detection uses file contents, not an
extension allowlist. Camera/codec compatibility is still required; an extension
alone cannot guarantee support for every new camera or damaged file.

JPEG, PNG, compressed TIFF, BMP and GIF use Windows image codecs when needed.
Additional formats depend on installed codecs. Multi-frame/page inputs process
the first image with an explicit notice. CMYK needs a dedicated color pipeline
and is not silently treated as RGB.

- Compatible uncompressed integer TIFF/BMP: native editing preserves the source
  container, extension, dimensions, bit depth, metadata, alpha and row padding;
  only denoised RGB/grayscale sample bytes change.
- RAW/codec inputs: output is **uncompressed RGB/RGBA16 TIFF**. No PNG conversion,
  JPEG recompression, or intermediate 8-bit RAW development step is introduced.
  The source is never overwritten. This TIFF contains developed pixels, not the
  original sensor mosaic. Development, color conversion and denoise change pixels
  by design; keep the RAW for all original sensor data and dynamic range.
- A default result is `image_denoised_100.tif` beside the source. Native editable
  TIFF/BMP retains its extension. Explicit output names must match the actual
  format; a TIFF is never disguised with a `.CR2`/`.CR3` extension.
- Existing output files are rejected. Files are written through a temporary
  file and renamed only after success. Source paths support spaces and Unicode.

RAW development uses camera white balance, AHD demosaic, 16-bit sRGB output,
sRGB gamma and disabled automatic brightness. The Windows sRGB ICC profile is
embedded when available. Basic camera/exposure metadata is transferred. Proprietary
MakerNotes and every other original metadata field remain in the unchanged source;
not all are duplicated in the developed TIFF. Native TIFF/BMP editing preserves
all unrelated container bytes. HDR/float codec inputs are rendered to the model's
normalized range and TIFF16; retain the original for its full range.

## Other options

```text
--cpu                 Force CPU inference
--device <index>      Choose a DXGI adapter
--tile <size>         Multiple of 64, from 128 through 512; default 256
--offline             Require a complete local model
--no-tui              Plain English progress for scripting
--list-devices        List graphics adapters
--formats             List image/RAW support
--list-cameras        List LibRaw cameras
--inspect <RAW>       Identify a RAW without processing it
--help
```

DirectML requires a compatible DirectX 12 device/driver; unsupported historical
GPU series fall back to CPU. Adapters are tried in descending dedicated-memory
order unless explicitly selected. If GPU initialization fails, CPU is used. If GPU
inference fails, the complete image is retried on CPU from the original pixels.
Individual unsupported operators may run on CPU even in a DirectML session.
[DirectML requirements](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html).

Inference uses fixed NCHW float32 tiles, reflected 32-pixel borders and the center
region of each result. Finite context can introduce subtle seams compared with
whole-image inference. Smaller tiles reduce GPU memory use. Pixel RAM is about
28 bytes/pixel plus runtime memory; the input limit is 200 megapixels.

## Build, package and tests

Requirements: Windows x64, Visual Studio 2022 MSVC/Windows SDK, CMake >=3.24.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -IncludeCachedModel
```

Setup downloads and verifies official ONNX Runtime DirectML 1.24.1, DirectML
1.15.4 and LibRaw 0.22.3 Windows packages. The MIT-licensed cJSON 1.7.19 parser is
vendored as unmodified C source. DirectML's multi-architecture archive
is ~202 MB; only required x64 DLLs are distributed. Licensing notices, LibRaw
CDDL/LGPL terms and its unmodified source/binary archive are under `licenses/`.
The upstream model is Apache-2.0; the export also includes a separate MIT notice,
both retained. The package includes SHA-256 checksums and only complete cached
model files from `build/bin/models`; fetched catalog metadata can always be refreshed.

```powershell
ctest --test-dir build -C Release --output-on-failure
python tests\integration.py --inference
python tests\formats.py
python tests\models.py
python tests\catalog.py
python tests\families.py  # Optional: install the additional models first
python tests\tui_console.py
python tests\installer.py  # Install/uninstall the built MSI in an isolated test folder
```

Python is a development test tool only. Tests cover native byte-identical
roundtrips, 8/16-bit grayscale/RGB/RGBA, endian/strip variants, alpha/metadata,
RAW/DNG/JPEG/PNG/deflate TIFF, precision, Unicode, output collisions, GPU/CPU
agreement and fallback, model selection and exact-copy strength 0. Real Win32
console tests exercise rendered buffers, keyboard selection, terminal resizing,
nested install/back screens, online refresh, catalog scrolling, download cancellation
and terminal restoration. Catalog tests also load and run a newly introduced model
ID without rebuilding the executable, and verify invalid-cache fallback.
The added NAFNet width32/64, Restormer, SwinIR and DRUNet exports were each run on
the RX 6700 XT, including DRUNet's distinct input/output channel counts.

A real Canon EOS 1100D CR2 was developed and denoised to 4290x2856 RGB16 TIFF on
the RX 6700 XT in ~77 seconds; the original SHA-256 stayed identical. A synthetic
sigma25 test reduced MSE from 624.511 to 1.843. These are individual validation
results, not performance/quality guarantees for every camera or image.

```text
include/          Shared C interfaces
src/cli/          English command parsing and script output
src/app/          Processing/install jobs and progress events
src/tui/          Native full-screen terminal rendering and keyboard handling
catalog/          Online JSON catalog with supported adapters and provenance
src/model/        Catalog fetch/cache/validation, selection and model downloads
vendor/cjson/     Pinned MIT-licensed C JSON parser
src/backend/      DXGI and ONNX Runtime DirectML/CPU
src/image/        Native TIFF/BMP, LibRaw C API, Windows codecs, TIFF16 writer
src/pipeline/     Tiling, reflection, blending and CPU retry
scripts/          Setup, build and packaging
tests/            C and integration tests; generated results are ignored
licenses/         Third-party licenses
```

Git is initialized. `.gitignore` excludes builds, distributions, weights, model
downloads, Adobe blobs, dependency archives, generated test output and local
investigation evidence. Source and release tags are published to the GitHub repository.
