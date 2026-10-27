# NVIDIA D3D12 pipeline-library static-sampler repro

A standalone, offscreen reproduction of incorrect static sampler behavior after loading a serialized
`ID3D12PipelineLibrary`. Recreating two root signatures in the opposite order causes point/linear sampling
results to swap on the tested NVIDIA driver, even though both `LoadGraphicsPipeline` calls return `S_OK`.

The application uses only C++17 and the Windows SDK: D3D12, DXGI, D3DCompiler and WRL COM pointers.
There is no DiligentEngine, third-party package, downloaded dependency, swapchain, window, threading,
engine shader cache or application-defined cache hash. CMake builds the executable and runs the experiment.

## Build and run

Requirements: Windows, CMake 3.20+, Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
The optional `--debug` mode also requires the Windows **Graphics Tools** feature.

From this directory in PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
.\build\Release\native_pso_repro.exe --list-adapters
cmake --build build --config Release --target repro
```

The `repro` target runs five **separate processes**, sharing one library file in `build/`.
Adapter index 0 is the default. Select another adapter or enable the debug layer with:

```powershell
cmake -S . -B build -DREPRO_ADAPTER=1
cmake --build build --config Release --target repro

cmake -S . -B build -DREPRO_ADAPTER=0 -DREPRO_DEBUG_LAYER=ON
cmake --build build --config Release --target repro
```

The runner reports `REPRODUCED` when the three controls pass and at least one reversed-root case has incorrect
pixels. It reports `NOT REPRODUCED` when all five cases have correct pixels. Either result means the experiment
completed successfully. Setup/API failures, timeouts, or incorrect control results make the runner fail.

## Minimal two-process reproduction

```powershell
# Creates two PSOs and saves the native library. Overwrites the specified output file.
.\build\Release\native_pso_repro.exe cold --adapter 0 --cache build\manual.bin

# Loads the same named PSOs after reversing only root-signature creation order.
.\build\Release\native_pso_repro.exe warm --adapter 0 --cache build\manual.bin --reverse-roots
$LASTEXITCODE
```

The second command returns **1** on the affected NVIDIA setup. This is the observed pixel failure, not an API
failure. Warm mode requires an existing library and **never falls back to `CreateGraphicsPipelineState`**.
Use the same adapter and driver for cold and warm runs; regenerate the library after a driver/runtime change.

Other controls, using the same cache:

```powershell
# Original root and PSO order.
.\build\Release\native_pso_repro.exe warm --cache build\manual.bin

# Only PSO load order changes; root creation order remains unchanged.
.\build\Release\native_pso_repro.exe warm --cache build\manual.bin --reverse-psos

# Both orders change.
.\build\Release\native_pso_repro.exe warm --cache build\manual.bin --reverse-roots --reverse-psos
```

Executable exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Correct pixels, or successful adapter listing/help |
| 1 | All API operations succeeded, but pixels are incorrect |
| 2 | Argument, file, API, GPU wait/device, or debug-layer validation error |

## What the program does

1. Creates two root signatures with identical bindings and different static sampler filters: **point**, then **linear**.
2. Compiles a single shared vertex/pixel shader pair with the Windows SDK compiler. Both PSOs use the same
   shader bytecode and graphics state; each references its corresponding root signature.
3. In cold mode, creates the PSOs and stores them under the stable names `point` and `linear`. After successful
   pixel validation it serializes the library. In warm mode it restores that file and loads the original named PSOs,
   passing their original descriptions. Root creation order and PSO load order can be changed independently.
4. Uploads a 2x1 RGBA8 texture containing **red, green**. Each shader samples mip 0 at fixed UV `(0.5, 0.5)`.
5. Draws a fullscreen triangle into a 1x1 RGBA8 render target, first with the point pipeline and then with the linear
   pipeline. Each result is copied into a separate aligned readback slot. A fence completes before CPU inspection.

Drawing order, sampler-to-root/PSO association, texture data, shader bytecode and PSO names never change.
Only the requested creation/load order changes. The library input buffer is retained unchanged until after the
library is released, as required by Microsoft's [CreatePipelineLibrary contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary).
The [LoadGraphicsPipeline contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12pipelinelibrary-loadgraphicspipeline)
requires the supplied PSO description to match the library entry; the failing cases return success for both loads.

Expected pixels in every case:

- Point: `(0,255,0,255)`.
- Linear: `(127..128,127..128,0,255)`. The one-unit range accommodates ordinary UNORM rounding across adapters;
  it does not mask a point/linear swap.

## Observed results

Validated on 2026-10-27, Windows 11 build 26300, system `D3D12Core.dll` 10.0.26100.9549,
Windows SDK 10.0.26100.0, MSVC 19.44.35229. No Agility SDK is bundled.

| Experiment | NVIDIA RTX 5060, 621.01 / 32.0.16.2101 | Intel UHD 770, 32.0.101.7092 |
| --- | --- | --- |
| Cold, original order | Correct | Correct |
| Warm, original order | Correct | Correct |
| Warm, reverse PSO load order only | Correct | Correct |
| Warm, reverse root creation order only | **Point/linear results swapped** | Correct |
| Warm, reverse both orders | **Point/linear results swapped** | Correct |

Affected NVIDIA output:

```text
Mode=warm, root order=linear,point, PSO order=point,linear
LoadGraphicsPipeline(point) = 0x00000000
LoadGraphicsPipeline(linear) = 0x00000000
point RGBA = (127,127,0,255)
linear RGBA = (0,255,0,255)
WRONG_PIXELS: expected point=(0,255,0,255), linear=(127..128,127..128,0,255)
```

The NVIDIA matrix also reproduces with `--debug`; all five processes report **0 debug-layer messages**.
The result isolates a native NVIDIA D3D12 pipeline-library/static-sampler restoration problem on this setup.
It does not establish which internal driver component is responsible, or which other driver versions are affected.

Original investigation: [HLND2T issue #170](https://github.com/HLND2T/HLND2T-DiligentGraphics/issues/170).
Application-side workaround: [PR #171](https://github.com/HLND2T/HLND2T-DiligentGraphics/pull/171).
