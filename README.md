# Photo Denoise 1.2

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
.\dist\denoise\denoise.exe "E:\photos\image.jpg" 50 --output "E:\photos\clean.tif"
```

Interactive terminals show a full-screen interface instead of scrolling progress
logs. Run without arguments for the image-path/strength form. During a job, the UI
shows decoding, cache verification, download progress, model loading, inference,
and output saving. Press Esc/Ctrl+C to request cancellation; Enter returns after
completion or failure. Cancellation waits for the current network/runtime call
to return. Terminal contents and input modes are restored on exit.

The model library supports Up/Down or J/K, Home/End, Enter to install/repair,
R to recheck the cache, and Esc/Q to leave. It shows descriptions, RGB/grayscale
requirements, disk size, verified installed status, and qualitative performance
notes. Models share the SCUNet architecture; no invented GPU timing or speedup
claims are presented. A terminal of at least 72 columns by 24 rows is recommended.

For scripts or redirected output:

```powershell
.\dist\denoise\denoise.exe model list
.\dist\denoise\denoise.exe model install scunet_color_50 --no-tui
.\dist\denoise\denoise.exe "E:\photos\image.CR3" 100 --model scunet_color_50 --no-tui
```

Plain English progress is automatic when standard input/output is redirected.
`model` without a subcommand requires an interactive terminal.

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

`--model` accepts these exact IDs, the corresponding `.onnx` filename, or aliases
`auto`, `real`, `15`, `25`, `50`. A named model overrides automatic selection at
every strength. A grayscale model on a color image is rejected rather than
silently discarding color. Arbitrary external ONNX paths are not supported.

Automatic selection is a documented heuristic: grayscale/RGB plus strength
1-33 -> sigma15, 34-66 -> sigma25, **67-100 -> sigma50**. It does not estimate the
image's actual noise. Network output is blended with the original using
`strength / 100`. Changing networks at the thresholds can cause visual jumps;
pin a model to keep the same network throughout a strength adjustment.
Strength 0 makes an exact byte-for-byte copy and needs no model.

Each variant has a ~3.8 MB graph and ~73.1 MB external weight file. Both must be
present and SHA-256 verified before inference. Downloads use HTTPS, pinned file
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

Models: [SCUNet upstream](https://github.com/cszn/SCUNet), ONNX exports from
[Heliosoph/scunet-onnx](https://huggingface.co/Heliosoph/scunet-onnx), pinned to
`6d11417ee2fbcc73783c502a238ac115097754fe`. Catalog, descriptions and checksums are
in `src/model/catalog.c`. The four Adobe `.data` containers in `winml/` remain
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
1.15.4 and LibRaw 0.22.3 Windows packages. DirectML's multi-architecture archive
is ~202 MB; only required x64 DLLs are distributed. Licensing notices, LibRaw
CDDL/LGPL terms and its unmodified source/binary archive are under `licenses/`.
The upstream model is Apache-2.0; the export also includes a separate MIT notice,
both retained. The package includes SHA-256 checksums and only complete cached
model file pairs from `build/bin/models`.

```powershell
ctest --test-dir build -C Release --output-on-failure
python tests\integration.py --inference
python tests\formats.py
python tests\models.py
python tests\tui_console.py
```

Python is a development test tool only. Tests cover native byte-identical
roundtrips, 8/16-bit grayscale/RGB/RGBA, endian/strip variants, alpha/metadata,
RAW/DNG/JPEG/PNG/deflate TIFF, precision, Unicode, output collisions, GPU/CPU
agreement and fallback, model selection and exact-copy strength 0. Real Win32
console tests exercise rendered buffers, keyboard selection, terminal resizing,
nested install/back screens, download cancellation and terminal restoration.

A real Canon EOS 1100D CR2 was developed and denoised to 4290x2856 RGB16 TIFF on
the RX 6700 XT in ~77 seconds; the original SHA-256 stayed identical. A synthetic
sigma25 test reduced MSE from 624.511 to 1.843. These are individual validation
results, not performance/quality guarantees for every camera or image.

```text
include/          Shared C interfaces
src/cli/          English command parsing and script output
src/app/          Processing/install jobs and progress events
src/tui/          Native full-screen terminal rendering and keyboard handling
src/model/        Catalog, selection, verification, resumable HTTPS downloads
src/backend/      DXGI and ONNX Runtime DirectML/CPU
src/image/        Native TIFF/BMP, LibRaw C API, Windows codecs, TIFF16 writer
src/pipeline/     Tiling, reflection, blending and CPU retry
scripts/          Setup, build and packaging
tests/            C and integration tests; generated results are ignored
licenses/         Third-party licenses
```

Git is initialized. `.gitignore` excludes builds, distributions, weights, model
downloads, Adobe blobs, dependency archives, generated test output and local
investigation evidence. No commit is created automatically.
