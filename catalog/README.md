# Online model catalog

Runtime source:
`https://raw.githubusercontent.com/Artur73737/denoise/main/catalog/models.json`

The model TUI fetches this JSON on entry and on **R**. No weights are fetched
until a model is installed or selected for inference. A validated copy is cached
at `models/catalog.json` beside the executable. Offline operation uses that copy
or the snapshot embedded at build time.

`schema: 1` supports up to 128 models. Each entry declares an ID, input/output
channel counts, adapter, optional fixed tile size, descriptions, license/source,
and one or two artifact records with local basename, immutable HTTPS URL, exact
byte count, and SHA-256. File basenames cannot overlap between models. The seven
original SCUNet IDs remain required for backward-compatible automatic selection.

Supported adapters:

- `image`: float32 NCHW image input/output, one or three channels; same resolution.
- `noise-map`: RGB plus a fourth constant sigma channel, RGB output. Current
  DRUNet strength maps to sigma 0..50, normalized by 255, before output blending.

Dynamic models accept the chosen tile size. `fixed_tile` exports override it
explicitly; SwinIR uses 128x128. Super-resolution, deblur-only networks, arbitrary
precision/layouts and RAW-mosaic networks must not be relabeled as denoisers.
New architectures requiring another interface need a new tested adapter in C.

## Families and sources

- SCUNet: `cszn/SCUNet`; all eight ONNX variants from
  `Heliosoph/scunet-onnx`, revision `6d11417ee2fbcc73783c502a238ac115097754fe`.
- NAFNet-SIDD width64: `megvii-research/NAFNet`, ONNX from
  `deepghs/image_restoration`, revision `77885239ff28876d6b37dd080ab86aa1de2c49cc`.
- NAFNet-SIDD width32: `tomfl/obscura-models`, revision
  `d55684db2a6d43c29bed6223c6bbf0fb11bbcdae`.
- Restormer real denoising: `swz30/Restormer`, ONNX from
  `kortique/photoview-ai-models`, revision `db2b050bcecf5c9b9398b92dd4fba38f573c9ae6`.
- SwinIR sigma25: `JingyunLiang/SwinIR`, ONNX from `Heliosoph/swinir-onnx`,
  revision `a4a77e2c33c32cfa4f39953b2aa87afec7f34aef`.
- DRUNet: `cszn/DPIR` / `cszn/KAIR`, ONNX from `synthscript/drunet-color-onnx`,
  revision `a2b9fccfa27b197f44a3876c567f5e48970c44a7`.

Licenses are preserved in `licenses/`; per-artifact hashes and sizes are obtained
from the source repositories' LFS metadata and checked against downloaded bytes.
Models are fetched from upstream repositories; their weight binaries are not
committed here. No endorsement by model authors is implied.

NAFNet width64 is the largest/high-capacity option in this catalog, not a claim
of universal superiority. The upstream SIDD benchmark reports 40.30 dB for that
checkpoint; this does not guarantee the same result on DSLR RAW renders or this
application's tiles. Restormer and blind SCUNet are alternatives for real noise;
SwinIR sigma25 is a matched-noise specialist. GAN texture sharpness is not fidelity.
