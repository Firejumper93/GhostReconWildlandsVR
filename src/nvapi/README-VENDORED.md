# NVAPI SDK, vendored (BUILD 216)

Third-party code. Source: `https://github.com/NVIDIA/nvapi`, commit
`87dca625e83fd89a983e19b904e5f3a580da90d2` (2026-09-04), MIT licensed; the
license is `LICENSE.txt` beside this file and must travel with these files.

What is here and why:

- `nvapi.h` and the `nvapi_lite_*.h` headers it includes: the public API. Only
  the Direct3D 11 Variable Rate Shading entry points and the GPU utilization
  query are used (`src\Foveate.cpp`).
- `nvapi64.lib` (from the SDK's `amd64\`): a small static stub. It does NOT add
  an import of `nvapi64.dll` to our proxy; it resolves the driver's
  `nvapi64.dll` at run time inside `NvAPI_Initialize`, which simply returns an
  error on a machine without the NVIDIA driver. So the proxy loads and runs
  unchanged on AMD and Intel; the foveation module logs one line and stays
  inert there.

The headers are included under `#pragma warning(push, 0)` so our own sources
keep `/W4`. Nothing in this folder is modified from upstream.
