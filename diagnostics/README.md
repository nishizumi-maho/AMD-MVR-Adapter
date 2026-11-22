# Diagnostics: the 4th view of iRacing's triple screen SMP

This branch is reference material, **not meant to be merged**. It is based on [PR #2](https://github.com/mbucchia/AMD-MVR-Adapter/pull/2) (triple screen SMP), and adds a diagnostic build of the adapter and the scripts used to measure it.

## Findings

With "Render Scene Using 3 Projections" and "Nvidia Simultaneous Multi-Projection", iRacing calls `NvAPI_D3D_SetMultiViewMode()` with 4 views and independent viewport masks. Its vertex shaders send views 0, 1 and 2 to viewports 0, 1 and 2, and disable view 3 when there are only 3 projections: its viewport mask is 0 and its position is `(-1, -1, -1, -1)`.

The adapter ignores the viewport masks, so on AMD the vertex shaders still run for view 3, whose triangles are then all clipped. Restricting the viewport mask to 3 views at runtime saved 0.4 to 1.7 ms of GPU time per frame, with no visual difference, but only about 1.5% of frame rate on a CPU-bound system.

Details and measurements: [comment on PR #2](https://github.com/mbucchia/AMD-MVR-Adapter/pull/2#issuecomment-7083865144).

## The diagnostic build

`#define MVR_DIAG 1` in `nvapi_adapter/pch.h` turns it on. Most of it is in `nvapi_adapter/diag.cpp`.

- **Log** (`[diag]` lines in `nvapi_adapter.log`): each distinct set of `NvAPI_D3D_SetMultiViewMode()` parameters, the pipeline state when view broadcasting is enabled (viewports, scissor rects, scissor test, render target size), and the calls to `RSSetViewports()` and `RSSetScissorRects()` while broadcasting is on. Those two methods are hooked through the context's vtable, after checking the vtable layout. Each distinct line is logged once, and the logging stops after a fixed number of attempts, so that it does not keep costing CPU time.
- **Shader details:** `LOG_DETAIL` is enabled in Release (signatures, custom semantics, rewrite details), and the disassembly of the first shaders (80 files, original and rewritten) is written to `%LOCALAPPDATA%\AMD-MVR-Adapter\shaders`.
- **Viewport mask override:** when `%LOCALAPPDATA%\AMD-MVR-Adapter\vpmask_override.txt` contains a hexadecimal mask (for example `0x7`), the viewport mask sent to AGS is restricted to it. The file is read at most once per second, so creating or deleting it switches the mask while the game runs, and every switch is logged (`[diag] vpMask override`).

Build as usual (`bin\Release\nvapi64.dll`), and replace the `nvapi64.dll` installed next to `iRacingSim64DX11.exe`.

## Tools

- `viewport_masks.py`: groups how the dumped vertex shaders write `NV_VIEWPORT_MASK`, `NV_VIEWPORT_MASK_2_SEMANTIC` and the position of view 3.
- `bench/capture.ps1`: records one run of the game. It combines per-frame metrics from [PresentMon](https://github.com/GameTechDev/PresentMon) 2.x, Windows performance counters (CPU per core, CPU of the iRacing process, GPU 3D engine of the iRacing process), and iRacing's telemetry.
- `bench/irsdk_logger.py`: logs iRacing's telemetry from its SDK shared memory (`FrameRate`, `CpuUsageFG`, `GpuUsage`, `Speed`...), used by `capture.ps1`.
- `bench/analyze.py`: summary table of the runs, grouping the repetitions of a configuration (labels ending in `_r1`, `_r2`...).
- `bench/charts.py`: the charts of PR #2.

## Reproducing the A/B measurement

With the diagnostic build installed, the game running with 3 projections and SMP, and the car stationary in the pit box, from an elevated PowerShell prompt in `diagnostics\bench`:

```powershell
$override = "$env:LOCALAPPDATA\AMD-MVR-Adapter\vpmask_override.txt"
$phase = 0
foreach ($views in 4, 3, 4, 3) {
    $phase++
    if ($views -eq 4) { Remove-Item $override -ErrorAction SilentlyContinue } else { Set-Content $override '0x7' -NoNewline }
    Start-Sleep -Seconds 3
    foreach ($r in 1..2) { .\capture.ps1 -Label ("ab{0}_{1}views_r{2}" -f $phase, $views, $r) -Seconds 5 -Delay 0 -OutDir runs_ab }
}
Remove-Item $override -ErrorAction SilentlyContinue
python -I analyze.py runs_ab
```

Alternating the phases (4, 3, 4, 3 views) within the same session keeps the session-to-session variation out of the comparison.
