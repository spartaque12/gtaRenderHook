#include "stdafx.h"
#include "FrameProfiler.h"
#include "RwRenderEngine.h"
#include "RwD3D1XEngine.h"
#include "D3DRenderer.h"
#include "Renderer.h"
#include <game_sa\CStreaming.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>

#pragma comment( lib, "psapi.lib" )

extern int drawCallCount;

namespace FrameProfiler
{
namespace
{
const char *const kZoneNames[ZoneCount] = {
    "idle total", "time", "game logic", "audio", "lights", "reload checks", "render total", "prepare", "sky/clouds",
    "shadow prepare", "render list", "pre-render", "cubemap", "shadows", "g-buffer", "deferred", "forward/alpha",
    "tonemap", "2D/HUD", "settings GUI", "HUD", "present" };
// Zones that also get GPU timestamps (rendering work).
const bool kGpuZone[ZoneCount] = {
    true, false, false, false, false, false, true, false, true,
    false, false, false, true, true, true, true, true,
    true, true, true, true, false };
const char *const kEventNames[EventCount] = {
    "texture reads", "raster creates", "texture uploads", "read-back locks", "read-back unlocks", "shader compiles",
    "buffer maps" };

struct EventStats
{
    uint32_t count;
    double   ms, maxMs;
    char     maxDetail[96];
};

struct FrameRecord
{
    uint64_t   index;
    SYSTEMTIME time;
    double     frameMs;
    double     cpuMs[ZoneCount];
    double     gpuMs[ZoneCount];
    bool       gpuValid;
    EventStats events[EventCount];
    uint32_t   draws, buildingDraws, uploadBytes;
    uint32_t   visibleEntities, visibleLods, shadowCasters, reflectionObjects;
    uint32_t   streamingKB, modelsRequested;
    int32_t    streamingDeltaKB;
    uint32_t   pageFaults;
};

// GPU timestamps are read back kSlots - 1 frames later, so each slot keeps its frame's record until then.
const int kSlots = 6;
struct Slot
{
    ID3D11Query *disjoint;
    ID3D11Query *begin[ZoneCount], *end[ZoneCount];
    bool         used[ZoneCount];
    bool         pending;
    FrameRecord  record;
};

bool                 g_enabled;
double               g_slowMs = 33.3, g_summarySeconds = 10.0;
bool                 g_gpuTimers = true;
FILE *               g_log;
DWORD                g_mainThread;
LARGE_INTEGER        g_freq;
int64_t              g_lastFrameEnd;
uint64_t             g_frameIndex;
bool                 g_gpuReady, g_gpuFailed;
Slot                 g_slots[kSlots];
int                  g_current;            // slot of the frame being recorded
int                  g_oldest, g_pending;  // finished frames waiting for their GPU times
int64_t              g_zoneStart[ZoneCount];
int                  g_zoneDepth[ZoneCount];
uint32_t             g_lastStreamingKB;
uint32_t             g_lastPageFaults;
ID3D11Device *       g_device;
ID3D11DeviceContext *g_context;
IDXGIAdapter3 *      g_adapter;

// summary accumulators
struct Summary
{
    int64_t             start;
    std::vector<float>  frameMs;
    double              cpuSum[ZoneCount], cpuMax[ZoneCount], gpuSum[ZoneCount], gpuMax[ZoneCount];
    uint32_t            gpuFrames;
    uint32_t            eventCount[EventCount];
    double              eventMs[EventCount], eventMax[EventCount];
    uint64_t            draws, uploadBytes;
    uint32_t            slowFrames;
} g_summary;

int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    return t.QuadPart;
}

double Ms( int64_t ticks ) { return 1000.0 * double( ticks ) / double( g_freq.QuadPart ); }

void Log( const char *fmt, ... )
{
    if ( !g_log )
        return;
    va_list a;
    va_start( a, fmt );
    vfprintf( g_log, fmt, a );
    va_end( a );
    fputc( '\n', g_log );
}

// Appends to a line buffer, cutting off instead of failing when it is full.
struct Line
{
    char buf[1400];
    int  len = 0;
    void Add( const char *fmt, ... )
    {
        if ( len >= int( sizeof buf ) - 1 )
            return;
        va_list a;
        va_start( a, fmt );
        const int r = _vsnprintf_s( buf + len, sizeof buf - len, _TRUNCATE, fmt, a );
        va_end( a );
        len = r < 0 ? int( strlen( buf ) ) : len + r;
    }
};

void InitGpu()
{
    g_gpuReady = true;
    auto *renderer = GET_D3D_RENDERER;
    if ( !renderer )
    {
        g_gpuReady = false;
        return;
    }
    g_device  = renderer->getDevice();
    g_context = renderer->getContext();
    if ( !g_device || !g_context )
    {
        g_gpuReady = false;
        return;
    }
    IDXGIDevice *dxgiDevice = nullptr;
    if ( SUCCEEDED( g_device->QueryInterface( __uuidof( IDXGIDevice ), reinterpret_cast<void **>( &dxgiDevice ) ) ) )
    {
        IDXGIAdapter *adapter = nullptr;
        if ( SUCCEEDED( dxgiDevice->GetAdapter( &adapter ) ) )
        {
            adapter->QueryInterface( __uuidof( IDXGIAdapter3 ), reinterpret_cast<void **>( &g_adapter ) );
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    if ( !g_gpuTimers )
        return;
    D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, stamp{ D3D11_QUERY_TIMESTAMP, 0 };
    for ( auto &slot : g_slots )
    {
        bool ok = SUCCEEDED( g_device->CreateQuery( &disjoint, &slot.disjoint ) );
        for ( int z = 0; ok && z < ZoneCount; ++z )
            if ( kGpuZone[z] )
                ok = SUCCEEDED( g_device->CreateQuery( &stamp, &slot.begin[z] ) ) &&
                     SUCCEEDED( g_device->CreateQuery( &stamp, &slot.end[z] ) );
        if ( !ok )
        {
            g_gpuFailed = true;
            Log( "GPU timestamp queries could not be created - GPU times off" );
            return;
        }
    }
}

void StartFrame()
{
    Slot &slot = g_slots[g_current];
    FrameRecord &r = slot.record;
    memset( &r, 0, sizeof r );
    r.index = g_frameIndex;
    memset( slot.used, 0, sizeof slot.used );
    memset( g_zoneDepth, 0, sizeof g_zoneDepth );
    if ( g_gpuTimers && !g_gpuFailed && slot.disjoint )
        g_context->Begin( slot.disjoint );
    drawCallCount = 0;
}

void MemoryLine( Line &line )
{
    MEMORYSTATUSEX ms{ sizeof ms };
    GlobalMemoryStatusEx( &ms );
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo( GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>( &pmc ), sizeof pmc );
    line.Add( "free address space %.0f MB of %.0f MB | private memory %.0f MB", ms.ullAvailVirtual / 1048576.0,
              ms.ullTotalVirtual / 1048576.0, pmc.PrivateUsage / 1048576.0 );
    if ( g_adapter )
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        g_adapter->QueryVideoMemoryInfo( 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local );
        g_adapter->QueryVideoMemoryInfo( 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal );
        line.Add( " | VRAM used %.0f MB of budget %.0f MB, shared %.0f MB", local.CurrentUsage / 1048576.0,
                  local.Budget / 1048576.0, nonLocal.CurrentUsage / 1048576.0 );
    }
}

void LogFrame( const FrameRecord &r )
{
    char gpuTotal[32] = "unknown";
    if ( r.gpuValid )
        sprintf_s( gpuTotal, "%.1f ms", r.gpuMs[ZoneIdle] );
    Log( "SLOW frame %llu at %02d:%02d:%02d.%03d: %.1f ms | GPU work %s", r.index, r.time.wHour, r.time.wMinute,
         r.time.wSecond, r.time.wMilliseconds, r.frameMs, gpuTotal );
    Line cpu;
    cpu.Add( "  CPU ms:" );
    for ( int z = 0; z < ZoneCount; ++z )
        if ( z != ZoneIdle && z != ZoneRenderInGame && r.cpuMs[z] >= 0.05 )
            cpu.Add( " %s %.2f |", kZoneNames[z], r.cpuMs[z] );
    cpu.Add( " outside RenderHook %.2f", max( 0.0, r.frameMs - r.cpuMs[ZoneIdle] - r.cpuMs[ZonePresent] ) );
    Log( "%s", cpu.buf );
    if ( r.gpuValid )
    {
        Line gpu;
        gpu.Add( "  GPU ms:" );
        for ( int z = 0; z < ZoneCount; ++z )
            if ( kGpuZone[z] && z != ZoneIdle && z != ZoneRenderInGame && r.gpuMs[z] >= 0.05 )
                gpu.Add( " %s %.2f |", kZoneNames[z], r.gpuMs[z] );
        Log( "%s", gpu.buf );
    }
    Line ev;
    ev.Add( "  events:" );
    bool any = false;
    for ( int e = 0; e < EventCount; ++e )
    {
        const EventStats &s = r.events[e];
        if ( !s.count )
            continue;
        any = true;
        ev.Add( " %s %u (%.2f ms, longest %.2f ms%s%s)", kEventNames[e], s.count, s.ms, s.maxMs, s.maxDetail[0] ? ": " : "",
                s.maxDetail );
        if ( e == EventTextureUpload )
            ev.Add( " [%.2f MB]", r.uploadBytes / 1048576.0 );
        ev.Add( " |" );
    }
    if ( any )
        Log( "%s", ev.buf );
    Log( "  counts: draws %u (buildings %u) | visible %u entities, %u LODs, %u shadow casters, %u reflection objects | "
         "streaming %u MB (%+d KB), %u models requested | page faults +%u",
         r.draws, r.buildingDraws, r.visibleEntities, r.visibleLods, r.shadowCasters, r.reflectionObjects,
         r.streamingKB / 1024, r.streamingDeltaKB, r.modelsRequested, r.pageFaults );
    Line mem;
    mem.Add( "  memory: " );
    MemoryLine( mem );
    Log( "%s", mem.buf );
}

void LogSummary( int64_t now )
{
    Summary &s = g_summary;
    const size_t n = s.frameMs.size();
    if ( n < 10 )
        return;
    std::vector<float> sorted = s.frameMs;
    std::sort( sorted.begin(), sorted.end() );
    double sum = 0;
    for ( float v : sorted )
        sum += v;
    const size_t worst = ( std::max )( size_t( 1 ), n / 100 );
    double worstSum = 0;
    for ( size_t i = n - worst; i < n; ++i )
        worstSum += sorted[i];
    SYSTEMTIME t;
    GetLocalTime( &t );
    Log( "=== %02d:%02d:%02d summary of %zu frames over %.1f s: %.1f FPS avg (%.2f ms) | 1%% low %.1f FPS | p99 %.1f ms | worst %.1f ms | "
         "%u slow frames",
         t.wHour, t.wMinute, t.wSecond, n, Ms( now - s.start ) / 1000.0, 1000.0 * n / sum, sum / n, 1000.0 * worst / worstSum,
         sorted[min( n - 1, n * 99 / 100 )], sorted[n - 1], s.slowFrames );
    Line cpu;
    cpu.Add( "  CPU avg/max ms:" );
    for ( int z = 0; z < ZoneCount; ++z )
        if ( z != ZoneIdle && z != ZoneRenderInGame && s.cpuMax[z] >= 0.05 )
            cpu.Add( " %s %.2f/%.1f |", kZoneNames[z], s.cpuSum[z] / n, s.cpuMax[z] );
    Log( "%s", cpu.buf );
    if ( s.gpuFrames )
    {
        Line gpu;
        gpu.Add( "  GPU avg/max ms: total %.2f/%.1f |", s.gpuSum[ZoneIdle] / s.gpuFrames, s.gpuMax[ZoneIdle] );
        for ( int z = 0; z < ZoneCount; ++z )
            if ( kGpuZone[z] && z != ZoneIdle && z != ZoneRenderInGame && s.gpuMax[z] >= 0.05 )
                gpu.Add( " %s %.2f/%.1f |", kZoneNames[z], s.gpuSum[z] / s.gpuFrames, s.gpuMax[z] );
        Log( "%s", gpu.buf );
    }
    Line ev;
    ev.Add( "  events (count, total ms, longest ms):" );
    for ( int e = 0; e < EventCount; ++e )
        if ( s.eventCount[e] )
            ev.Add( " %s %u, %.1f, %.2f |", kEventNames[e], s.eventCount[e], s.eventMs[e], s.eventMax[e] );
    Log( "%s", ev.buf );
    Line mem;
    mem.Add( "  avg draws %.0f | uploads %.1f MB | memory: ", double( s.draws ) / n, s.uploadBytes / 1048576.0 );
    MemoryLine( mem );
    Log( "%s", mem.buf );
    fflush( g_log );

    const size_t reserve = s.frameMs.capacity();
    s = Summary{};
    s.frameMs.reserve( reserve );
    s.start = now;
}

void Finalize( Slot &slot )
{
    slot.pending = false;
    const FrameRecord &r = slot.record;
    if ( r.cpuMs[ZoneRenderInGame] <= 0.0 )   // loading screen or menu: the world was not rendered
        return;
    Summary &s = g_summary;
    s.frameMs.push_back( float( r.frameMs ) );
    for ( int z = 0; z < ZoneCount; ++z )
    {
        s.cpuSum[z] += r.cpuMs[z];
        s.cpuMax[z] = max( s.cpuMax[z], r.cpuMs[z] );
        if ( r.gpuValid )
        {
            s.gpuSum[z] += r.gpuMs[z];
            s.gpuMax[z] = max( s.gpuMax[z], r.gpuMs[z] );
        }
    }
    s.gpuFrames += r.gpuValid;
    for ( int e = 0; e < EventCount; ++e )
    {
        s.eventCount[e] += r.events[e].count;
        s.eventMs[e] += r.events[e].ms;
        s.eventMax[e] = max( s.eventMax[e], r.events[e].maxMs );
    }
    s.draws += r.draws;
    s.uploadBytes += r.uploadBytes;
    if ( r.frameMs > g_slowMs )
    {
        ++s.slowFrames;
        LogFrame( r );
    }
}

// Reads a finished frame's GPU timestamps without waiting. False if the GPU has not got that far yet.
bool ReadGpu( Slot &slot )
{
    FrameRecord &r = slot.record;
    if ( !g_gpuTimers || g_gpuFailed || !slot.disjoint )
        return true;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if ( g_context->GetData( slot.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK )
        return false;
    r.gpuValid = !dj.Disjoint && dj.Frequency;
    for ( int z = 0; r.gpuValid && z < ZoneCount; ++z )
    {
        if ( !kGpuZone[z] || !slot.used[z] )
            continue;
        UINT64 b = 0, e = 0;
        if ( g_context->GetData( slot.begin[z], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK ||
             g_context->GetData( slot.end[z], &e, sizeof e, D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK )
            return false;
        r.gpuMs[z] = e > b ? 1000.0 * double( e - b ) / double( dj.Frequency ) : 0.0;
    }
    return true;
}

// Finalizes finished frames oldest first; with force, the oldest one even if its GPU times are not there yet.
void Resolve( bool force )
{
    while ( g_pending )
    {
        Slot &slot = g_slots[g_oldest];
        if ( !ReadGpu( slot ) )
        {
            if ( !force )
                return;
            slot.record.gpuValid = false;
            force = false;
        }
        Finalize( slot );
        g_oldest = ( g_oldest + 1 ) % kSlots;
        --g_pending;
    }
}
} // namespace

bool Enabled() { return g_enabled; }

void Init()
{
    char ini[MAX_PATH];
    GetFullPathNameA( "RHFrameProfiler.ini", MAX_PATH, ini, nullptr );
    g_enabled = GetPrivateProfileIntA( "Profiler", "Enabled", 0, ini ) != 0;
    if ( !g_enabled )
        return;
    char buf[32];
    GetPrivateProfileStringA( "Profiler", "SlowFrameMs", "33.3", buf, sizeof buf, ini );
    g_slowMs = atof( buf );
    GetPrivateProfileStringA( "Profiler", "SummarySeconds", "10", buf, sizeof buf, ini );
    g_summarySeconds = max( 1.0, atof( buf ) );
    g_gpuTimers = GetPrivateProfileIntA( "Profiler", "GpuTimers", 1, ini ) != 0;
    QueryPerformanceFrequency( &g_freq );
    g_mainThread = GetCurrentThreadId();
    fopen_s( &g_log, "RHFrameProfiler.log", "w" );
    if ( g_log )
        setvbuf( g_log, nullptr, _IOFBF, 1 << 16 );
    g_summary.frameMs.reserve( 4096 );
    Log( "RenderHook frame profiler: slow frame > %.1f ms, summary every %.0f s, GPU timers %s", g_slowMs,
         g_summarySeconds, g_gpuTimers ? "on" : "off" );
    Log( "Slow frames are logged when their GPU times are read back, a few frames later. 'outside RenderHook' = game loop "
         "time outside RenderHook's Idle and Present (other plugins' hooks, message pump)." );
    fflush( g_log );
}

void BeginZone( Zone zone )
{
    if ( GetCurrentThreadId() != g_mainThread || g_zoneDepth[zone]++ )
        return;
    g_zoneStart[zone] = Now();
    if ( g_gpuReady && g_gpuTimers && !g_gpuFailed && kGpuZone[zone] )
    {
        Slot &slot = g_slots[g_current];
        if ( !slot.used[zone] )
        {
            slot.used[zone] = true;
            g_context->End( slot.begin[zone] );
        }
    }
}

void EndZone( Zone zone )
{
    if ( GetCurrentThreadId() != g_mainThread || !g_zoneDepth[zone] || --g_zoneDepth[zone] )
        return;
    g_slots[g_current].record.cpuMs[zone] += Ms( Now() - g_zoneStart[zone] );
    if ( g_gpuReady && g_gpuTimers && !g_gpuFailed && kGpuZone[zone] && g_slots[g_current].used[zone] )
        g_context->End( g_slots[g_current].end[zone] );
}

void AddDrawCall()
{
    if ( g_enabled )
        ++g_slots[g_current].record.draws;
}

void AddUploadBytes( uint32_t bytes )
{
    if ( g_enabled )
        g_slots[g_current].record.uploadBytes += bytes;
}

EventScope::EventScope( Event event ) : m_event( event )
{
    m_detail[0] = 0;
    if ( g_enabled && GetCurrentThreadId() == g_mainThread )
        m_start = Now();
}

EventScope::~EventScope()
{
    if ( !m_start )
        return;
    const double ms = Ms( Now() - m_start );
    EventStats &s = g_slots[g_current].record.events[m_event];
    ++s.count;
    s.ms += ms;
    if ( ms > s.maxMs )
    {
        s.maxMs = ms;
        strcpy_s( s.maxDetail, m_detail );
    }
}

void EventScope::Detail( const char *fmt, ... )
{
    if ( !m_start )
        return;
    va_list a;
    va_start( a, fmt );
    _vsnprintf_s( m_detail, sizeof m_detail, _TRUNCATE, fmt, a );
    va_end( a );
}

void EndFrame()
{
    if ( !g_enabled || GetCurrentThreadId() != g_mainThread )
        return;
    const int64_t now = Now();
    if ( !g_gpuReady && !g_gpuFailed )
    {
        InitGpu();
        g_lastFrameEnd = now;
        g_summary.start = now;
        StartFrame();
        return;
    }

    Slot &slot = g_slots[g_current];
    FrameRecord &r = slot.record;
    r.frameMs = Ms( now - g_lastFrameEnd );
    GetLocalTime( &r.time );
    r.buildingDraws = static_cast<uint32_t>( drawCallCount );
    r.visibleEntities = CRendererRH::ms_nNoOfVisibleEntities;
    r.visibleLods = CRendererRH::ms_nNoOfVisibleLods;
    for ( const auto &list : CRendererRH::ms_aVisibleShadowCasters )
        r.shadowCasters += static_cast<uint32_t>( list.size() );
    r.reflectionObjects = static_cast<uint32_t>( CRendererRH::ms_aVisibleReflectionObjects.size() );
    r.streamingKB = CStreaming::ms_memoryUsed / 1024;
    r.streamingDeltaKB = g_lastStreamingKB ? int32_t( r.streamingKB ) - int32_t( g_lastStreamingKB ) : 0;
    g_lastStreamingKB = r.streamingKB;
    r.modelsRequested = CStreaming::ms_numModelsRequested;
    PROCESS_MEMORY_COUNTERS pmc{ sizeof pmc };
    if ( GetProcessMemoryInfo( GetCurrentProcess(), &pmc, sizeof pmc ) )
    {
        r.pageFaults = g_lastPageFaults ? pmc.PageFaultCount - g_lastPageFaults : 0;
        g_lastPageFaults = pmc.PageFaultCount;
    }
    if ( g_gpuTimers && !g_gpuFailed && slot.disjoint )
        g_context->End( slot.disjoint );
    slot.pending = true;
    if ( !g_pending++ )
        g_oldest = g_current;
    g_lastFrameEnd = now;
    ++g_frameIndex;

    Resolve( false );
    g_current = ( g_current + 1 ) % kSlots;
    if ( g_slots[g_current].pending )   // ring full: the oldest frame's GPU times never arrived
        Resolve( true );
    StartFrame();
    if ( Ms( now - g_summary.start ) >= 1000.0 * g_summarySeconds )
        LogSummary( now );
}
} // namespace FrameProfiler
