# RHRT64 — ray-traced AO helper

DXR only exists in 64-bit processes, so RenderHook (32-bit) mirrors the scene into this x64 D3D12 helper:

- RenderHook streams each model's opaque triangles once (bottom-level acceleration structures are built here) and,
  every frame, the G-buffer pass's instances, the camera and G-buffer 1 (normals + depth).
- The helper builds the TLAS, traces a few cosine-weighted rays per pixel (inline ray queries, `cs_6_5`), blurs the
  result and hands an AO texture back through a shared texture + fences. RenderHook uses it in place of its own AO
  when it is ready in time, otherwise keeps its own.
- Protocol: `gtaRenderHook/RTShared.h`. Game side: `gtaRenderHook/RTBridge.cpp`.

Needs a GPU with DXR tier 1.1 (RTX 20 series / RX 6000 or newer).

## Build

`build.bat` (Visual Studio 2022 or Build Tools with the Windows 10/11 SDK, dxc from the SDK) produces
`bin\RHRayTrace\RHRT64.exe`. Copy the `RHRayTrace` folder next to `_gtaRenderHook.asi`.

## Settings (settings.xml, Debug)

- `RayTracedAO` — on/off (default off)
- `RayTracedAORadius` — ray length in metres (default 1.5)
- `RayTracedAORays` — rays per pixel (default 4)

Logs: `RHRayTrace\RHRTBridge.log` (game side), `RHRayTrace\RHRT64.log` (helper).

Not covered yet: peds and other skinned models, alpha-tested geometry (leaves, fences), occluders outside the view,
temporal denoising.
