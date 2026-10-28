// Protocol between RenderHook (32-bit game, D3D11) and RHFrameGen64.exe (64-bit helper, D3D12) that presents the
// frames in its own window over the game: the base for frame generation, which needs D3D12 and 64-bit (Streamline).
// Keep in sync with the helper's copy; kVersion guards against a mismatch.
#pragma once
#include <cstdint>
#include <cstdio>

namespace rhfg {

const uint32_t kVersion = 1;
const uint32_t kSlots   = 3; // shared frame textures, used round-robin

enum : uint32_t { StateStarting, StateReady, StateFailed };

struct Shared
{
    uint32_t version;
    volatile uint32_t state;   // helper: StateReady once its D3D12 device and window are up
    char message[256];         // helper: what went wrong
    uint32_t adapterLuidLow;   // game: adapter of its D3D11 device
    int32_t  adapterLuidHigh;
    uint64_t gameWindow;       // game: HWND the helper's window covers
    int32_t  vsync;            // game: sync interval for the helper's Present (0 = tearing allowed if supported)

    // game: set before configGeneration is raised; handles are valid in the helper process
    uint32_t width, height, format; // DXGI_FORMAT of the frames
    uint64_t hFrame[kSlots];
    uint64_t hReadyFence;           // game signals frame n when slot n % kSlots holds it
    uint64_t hDoneFence;            // helper signals frame n once it's copied out of its slot
    volatile uint32_t configGeneration;

    // game, per frame: frame n (1, 2, ...) is in slot n % kSlots, ready on the GPU at hReadyFence >= n
    volatile uint64_t frame;
    volatile int64_t  publishQpc[kSlots];  // QueryPerformanceCounter when the frame was published
    volatile uint32_t droppedFrames;       // game: frames skipped because the helper still held every slot
};

inline void Names( uint32_t pid, char *mapping, char *frameEvent, char *configEvent, size_t size )
{
    snprintf( mapping, size, "Local\\RHFG_%u_map", pid );
    snprintf( frameEvent, size, "Local\\RHFG_%u_frame", pid );
    snprintf( configEvent, size, "Local\\RHFG_%u_config", pid );
}

} // namespace rhfg
