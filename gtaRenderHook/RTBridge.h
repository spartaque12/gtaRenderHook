#pragma once
/*!
    Ray-traced ambient occlusion through RHRT64.exe (RHRayTrace folder next to the asi; DXR runs only in 64-bit
    processes). RenderHook streams each model's opaque triangles to it once, and every frame the G-buffer pass's
    instances, the camera and G-buffer 1. The helper's AO replaces RenderHook's (t7 of the composition pass) when it
    is ready in time, otherwise RenderHook's own AO stays. Debug settings: RayTracedAO, RayTracedAORadius,
    RayTracedAORays.
*/
namespace CRTBridge
{
// before the G-buffer pass: starts collecting this frame's instances
void BeginFrame();
// a model drawn in the G-buffer pass (AtomicAllInOneNode)
void AddInstance( void *resEntry, RpGeometry *geometry, const RwMatrix *ltm );
// after the G-buffer pass, with the main camera's matrices set: hands the frame to the helper
void EndGBuffer( RwRaster *normalDepth );
// in the composition pass: binds the helper's AO to this slot when it's done in time; false keeps RenderHook's
bool BindAO( int slot );
// RenderWare freed a model's resource entry
void OnResEntryDestroyed( void *resEntry );
void Shutdown();
}
