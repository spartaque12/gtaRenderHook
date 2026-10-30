// Protocol between RenderHook (32-bit game, D3D11) and RHRT64.exe (64-bit, D3D12 + DXR) for ray-traced ambient
// occlusion. DXR exists only in 64-bit processes, so the scene is mirrored into the helper:
//  - meshes: RenderHook streams each model's opaque triangles once (MeshLoad) and drops them when RenderWare frees
//    the model (MeshUnload), through a byte ring in shared memory;
//  - frames: after the G-buffer pass RenderHook writes the frame's instance list and camera into one of two frame
//    slots, copies G-buffer 1 (normals + depth) into a shared texture and signals readyFence = frame. The helper
//    makes its GPU wait for that, builds the TLAS, traces and writes the AO texture, signals doneFence = frame and
//    publishes submittedFrame. RenderHook makes its GPU wait for doneFence only once submittedFrame says the
//    signal is queued, so a dead helper can never hang the game's GPU.
// Shared by both: gtaRenderHook and rt_helper (RHRT64.exe).
#pragma once
#include <cstdint>
#include <cstdio>

namespace rhrt
{
constexpr uint32_t kVersion      = 1;
constexpr uint32_t kMaxInstances = 16384;
constexpr uint32_t kRingSize     = 16u << 20;
constexpr uint32_t kFrameSlots   = 2;

enum HelperState : uint32_t
{
    kStarting = 0,
    kReady    = 1, // device and shared resources open
    kFailed   = 2, // gave up, see RHRT64.log
};

enum MessageType : uint32_t
{
    kMsgWrap       = 0, // rest of the ring is unused, continue at offset 0
    kMsgMeshLoad   = 1, // a = vertex count, b = triangle count; float[3] * a, then uint16[3] * b
    kMsgMeshUnload = 2,
};

struct Message
{
    uint32_t type;
    uint32_t size; // bytes including this header, multiple of 8
    uint32_t key;  // the model (RenderWare resource entry)
    uint32_t a, b;
    uint32_t pad;
};

enum InstanceFlags : uint32_t
{
    kInstanceVehicle = 1,
};

struct Instance
{
    uint32_t key;
    uint32_t flags;
    float    transform[12]; // object to world, 3 rows of 4 (D3D12_RAYTRACING_INSTANCE_DESC::Transform)
};

struct Frame
{
    uint64_t frame;
    float    viewInv[16];   // inverse of RenderHook's view matrix (row vectors: world = view-space * viewInv)
    float    proj00, proj11; // projection scale, for view-space position from depth
    float    farClip;       // G-buffer depth is stored as viewZ / farClip
    float    radius;        // AO ray length, metres
    uint32_t rays;          // rays per pixel
    uint32_t numInstances;
    Instance instances[kMaxInstances];
};

struct Shared
{
    uint32_t version;
    uint32_t helperState;
    uint32_t adapterLuidLow;
    int32_t  adapterLuidHigh;
    // shared NT handles, duplicated into the helper; recreated (configSerial + 1) when the screen size changes
    uint64_t hGBuffer;     // R16G16B16A16_FLOAT copy of G-buffer 1
    uint64_t hAO;          // R16_FLOAT, written by the helper (UAV)
    uint64_t hReadyFence;
    uint64_t hDoneFence;
    uint32_t width, height;
    uint32_t configSerial;
    uint32_t pad0;
    // helper: last frame whose doneFence signal is queued on its GPU
    volatile int64_t submittedFrame;
    // ring positions (bytes written / read, ever increasing)
    volatile int64_t ringWrite;
    volatile int64_t ringRead;
    Frame   frames[kFrameSlots];
    uint8_t ring[kRingSize];
};

inline void Names( uint32_t pid, char *map, char *frameEvent, char *submitEvent, size_t size )
{
    snprintf( map, size, "RHRT_Shared_%u", pid );
    snprintf( frameEvent, size, "RHRT_Frame_%u", pid );
    snprintf( submitEvent, size, "RHRT_Submit_%u", pid );
}
} // namespace rhrt
