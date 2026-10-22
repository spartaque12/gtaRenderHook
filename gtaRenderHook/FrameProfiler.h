#pragma once
// Frame profiler for finding stutters. Measures every frame: CPU time of each phase of the frame, GPU time of each
// render pass (timestamp queries, read back a few frames later without stalling), timed engine events (texture reads
// from streaming, texture uploads, read-back locks, shader compiles, buffer maps, Present), draw calls, visibility,
// streaming and memory counters. Every frame slower than SlowFrameMs gets a full breakdown in RHFrameProfiler.log, and a
// summary of all frames is written every SummarySeconds.
// Configured by RHFrameProfiler.ini in the game folder ([Profiler] Enabled=1). Disabled, every hook is a single branch.
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace FrameProfiler
{
enum Zone : int
{
    ZoneFrame,          // GPU only: all GPU work of the frame up to Present (Present is called inside Idle)
    ZoneTimeUpdate,
    ZoneGameUpdate,     // CGame::Process: game logic, scripts, physics, streaming
    ZoneAudio,
    ZoneLightUpdate,
    ZoneReloads,        // texture/shader reload checks
    ZoneRenderInGame,   // world rendering as a whole
    ZonePrepare,
    ZoneForwardBefore,  // sky, clouds
    ZoneShadowPrepare,
    ZoneRenderList,     // CRendererRH::ConstructRenderList
    ZonePreRender,
    ZoneCubemap,
    ZoneShadows,
    ZoneGBuffer,
    ZoneDeferred,       // deferred lighting/composition
    ZoneForwardAfter,   // transparent objects, effects
    ZoneTonemap,
    Zone2D,             // Render2dStuff: HUD, radar, text
    ZoneGUI,
    ZoneHUD,
    ZonePresent,
    ZoneCount
};

enum Event : int
{
    EventTextureRead,   // NativeTextureRead: a texture loaded from a TXD (streaming), creation + upload
    EventRasterCreate,
    EventTextureUpload, // UpdateSubresource when a raster is unlocked after writing
    EventLockRead,      // raster locked for reading: copy to staging + Map, waits for the GPU
    EventUnlockRead,
    EventShaderCompile,
    EventBufferMap,     // Map(WRITE_DISCARD) of dynamic/constant buffers
    EventCount
};

bool Enabled();
void Init();                    // reads RHFrameProfiler.ini; called once at startup
void EndFrame();                // right after Present
void AddDrawCall();
void InstallGameProcessHooks();   // times each call made by CGame::Process; done on the first frame
void AddUploadBytes( uint32_t bytes );

void BeginZone( Zone zone );
void EndZone( Zone zone );

struct ZoneScope
{
    Zone m_zone;
    explicit ZoneScope( Zone zone ) : m_zone( zone ) { if ( Enabled() ) BeginZone( zone ); }
    ~ZoneScope() { if ( Enabled() ) EndZone( m_zone ); }
};

// Times one event; Detail() describes it (only kept for the longest event of its kind in a frame).
struct EventScope
{
    Event   m_event;
    int64_t m_start = 0;
    char    m_detail[96];
    explicit EventScope( Event event );
    ~EventScope();
    void Detail( const char *fmt, ... );
};
} // namespace FrameProfiler

#define RH_PROFILE_CONCAT2( a, b ) a##b
#define RH_PROFILE_CONCAT( a, b ) RH_PROFILE_CONCAT2( a, b )
#define RH_PROFILE_ZONE( zone ) FrameProfiler::ZoneScope RH_PROFILE_CONCAT( rhProfileZone, __LINE__ )( FrameProfiler::zone )
