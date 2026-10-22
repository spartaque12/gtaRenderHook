#include "stdafx.h"
#include "FrameProfiler.h"
#include "RwRenderEngine.h"
#include "RwD3D1XEngine.h"
#include "D3DRenderer.h"
#include "Renderer.h"
#include <game_sa\CStreaming.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <timeapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <intrin.h>

#pragma comment( lib, "psapi.lib" )
#pragma comment( lib, "winmm.lib" )

extern int drawCallCount;

// ------------------------------------------------------------------------------------------------ CGame::Process calls
// Each call CGame::Process (0x53BEE0, 1.0 US) makes is redirected through a 10-byte stub (push site index; jmp
// ProfSiteEnter) that swaps the return address for ProfSiteExit, so the callee's time is measured whatever its calling
// convention or arguments. Installed on the first frame, after other plugins have placed their own hooks there: a
// plugin's hook is then timed as part of the call it hooks.
namespace
{
struct ProfSite
{
    uintptr_t at, original;  // call instruction, target in the unmodified game
    const char *name;
    uintptr_t target;        // what the call pointed to when the stub was installed
    char label[96];
};
ProfSite g_profSites[] = {
    { 0x53BEE6, 0x541DD0, "CPad::UpdatePads" }, { 0x53BEF0, 0x53D030, "CLoadMonitor::BeginFrame" },
    { 0x53BF0B, 0x40E670, "CStreaming::Update" }, { 0x53BF28, 0x4D5D00, "CCutsceneMgr::Update" },
    { 0x53BF44, 0x57B440, "sub_57B440 (menu system)" }, { 0x53BF49, 0x572D10, "sub_572D10" },
    { 0x53BF4E, 0x6997E0, "sub_6997E0" }, { 0x53BF7D, 0x5083C0, "sub_5083C0 (audio)" },
    { 0x53BFA9, 0x727260, "sub_727260" }, { 0x53BFAE, 0x727350, "sub_727350" }, { 0x53BFB3, 0x719800, "sub_719800" },
    { 0x53BFB8, 0x439AF0, "sub_439AF0 (cheats)" }, { 0x53BFBD, 0x52CF10, "CClock::Update" },
    { 0x53BFC2, 0x72B850, "CWeather::Update" }, { 0x53BFC7, 0x46A000, "CTheScripts::Process" },
    { 0x53BFCC, 0x411E20, "CCollision::Update" }, { 0x53BFD8, 0x450A60, "sub_450A60" },
    { 0x53BFDD, 0x6F5900, "CTrain::UpdateTrains" }, { 0x53BFE2, 0x6C79A0, "CHeli::UpdateHelis" },
    { 0x53BFE7, 0x43DAC0, "CDarkel::Update" }, { 0x53BFEC, 0x7205C0, "CSkidmarks::Update" },
    { 0x53BFF1, 0x71B0D0, "CGlass::Update" }, { 0x53BFF6, 0x562360, "CWanted::UpdateEachFrame" },
    { 0x53BFFB, 0x539CE0, "CCreepingFire::Update" }, { 0x53C000, 0x49AA00, "CSetPieces::Update" },
    { 0x53C00A, 0x53AF00, "CFireManager::Update" }, { 0x53C030, 0x616650, "CPopulation::Update (1)" },
    { 0x53C054, 0x616650, "CPopulation::Update (2)" }, { 0x53C05C, 0x73A360, "CWeapon::UpdateWeapons" },
    { 0x53C06A, 0x6F3F40, "CTheCarGenerators::Process" }, { 0x53C077, 0x6F3FE0, "sub_6F3FE0 (cranes)" },
    { 0x53C07C, 0x712FF0, "CClouds::Update" }, { 0x53C081, 0x7185B0, "CMovingThings::Update" },
    { 0x53C086, 0x72A3C0, "CWaterCannons::Update" }, { 0x53C08B, 0x5720A0, "CUserDisplay::Process" },
    { 0x53C090, 0x460500, "CReplay::Update" }, { 0x53C095, 0x5684A0, "CWorld::Process" },
    { 0x53C09F, 0x53D0B0, "CLoadMonitor::EndFrame" }, { 0x53C0AD, 0x458DE0, "CPickups::Update" },
    { 0x53C0B2, 0x423F10, "CCarCtrl::PruneVehiclesOfInterest" }, { 0x53C0B7, 0x44C8C0, "CGarages::Update" },
    { 0x53C0BC, 0x440D10, "CEntryExitManager::Update" }, { 0x53C0C1, 0x49C490, "CStuntJumpManager::Update" },
    { 0x53C0C6, 0x712330, "CBirds::Update" }, { 0x53C0CB, 0x726AA0, "CSpecialFX::Update" },
    { 0x53C0D0, 0x558D70, "CRopes::Update" }, { 0x53C0D5, 0x7046A0, "sub_7046A0 (post effects)" },
    { 0x53C0DA, 0x561760, "CTimeCycle::Update" }, { 0x53C0DF, 0x610BF0, "CPopCycle::Update" },
    { 0x53C0F1, 0x605A30, "sub_605A30" }, { 0x53C0F6, 0x45C440, "sub_45C440" }, { 0x53C104, 0x52B730, "sub_52B730" },
    { 0x53C10B, 0x50B5D0, "sub_50B5D0" }, { 0x53C110, 0x72DEC0, "sub_72DEC0" },
    { 0x53C11D, 0x442AD0, "sub_442AD0" }, { 0x53C122, 0x446610, "CGangWars::Update" },
    { 0x53C127, 0x43C590, "CConversations::Update" }, { 0x53C12C, 0x43B0F0, "CPedToPlayerConversations::Update" },
    { 0x53C131, 0x41BC80, "CBridge::Update" }, { 0x53C136, 0x6FC5A0, "sub_6FC5A0" }, { 0x53C13B, 0x6FADF0, "sub_6FADF0" },
    { 0x53C140, 0x70C950, "CShadows::UpdatePermanentShadows" }, { 0x53C159, 0x5DCFA0, "CPlantMgr::Update" },
    { 0x53C15E, 0x5D8050, "sub_5D8050" }, { 0x53C18F, 0x710B50, "sub_710B50" },
};
const int kProfSiteCount = sizeof( g_profSites ) / sizeof( g_profSites[0] );

struct ProfSiteCall
{
    int       site;
    uintptr_t ret;
    int64_t   start;
};
ProfSiteCall g_profSiteStack[64];
int          g_profSiteDepth;
double *     g_profSiteMs;   // per-site times of the frame being recorded
} // namespace

extern "C" uintptr_t __cdecl ProfSiteEnterC( uintptr_t ret, int site )
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    if ( g_profSiteDepth < 64 )
        g_profSiteStack[g_profSiteDepth] = { site, ret, t.QuadPart };
    ++g_profSiteDepth;
    return g_profSites[site].target;
}

extern "C" uintptr_t __cdecl ProfSiteLeaveC()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    --g_profSiteDepth;
    const ProfSiteCall &c = g_profSiteStack[g_profSiteDepth < 64 ? g_profSiteDepth : 63];
    static LARGE_INTEGER freq;
    if ( !freq.QuadPart )
        QueryPerformanceFrequency( &freq );
    if ( g_profSiteMs )
        g_profSiteMs[c.site] += 1000.0 * double( t.QuadPart - c.start ) / double( freq.QuadPart );
    return c.ret;
}

// [esp] = site index, [esp+4] = return address into CGame::Process.
static __declspec( naked ) void ProfSiteExit()
{
    __asm {
        push 0              // becomes the original return address
        pushad
        pushfd
        call ProfSiteLeaveC
        mov [esp+36], eax
        popfd
        popad
        ret
    }
}

// ------------------------------------------------------------------------------------------------ plugin code sampler
// While the main thread is in CGame::Process but not inside one of the timed calls - i.e. in other plugins' hooks
// wrapped around it - a sampler thread pauses it about 1000 times a second and records which module's code it is in.
// Time in Windows/driver DLLs is charged to the first plugin/game return address on the stack.
namespace
{
volatile LONG g_profInGameLogic;
struct ProfModule
{
    uintptr_t lo, hi;
    char      name[64];
    bool      system;
};
ProfModule    g_profModules[256];
volatile LONG g_profModuleCount;
struct ProfSample
{
    int64_t   t;
    uintptr_t addr;
};
const LONG    kProfRing = 1 << 13;
ProfSample    g_profRing[kProfRing];
volatile LONG g_profWritten;
DWORD         g_profMainThreadId;
uintptr_t     g_profStackBase;

bool ProfIsSystemName( const char *name )
{
    static const char *const kSystem[] = { "ntdll.dll", "kernel32.dll", "kernelbase.dll", "win32u.dll", "user32.dll",
                                           "ucrtbase.dll", "msvcrt.dll", "msvcp_win.dll", "gdi32.dll", "gdi32full.dll",
                                           "d3d11.dll", "dxgi.dll", "dxcore.dll", "nvwgf2um.dll", "vcruntime140.dll",
                                           "msvcp140.dll", "combase.dll", "std.asi.dll", "modloader.asi" };
    for ( const char *n : kSystem )
        if ( _stricmp( n, name ) == 0 )
            return true;
    return false;
}

void ProfRefreshModules()
{
    HMODULE mods[256];
    DWORD   needed = 0;
    if ( !EnumProcessModules( GetCurrentProcess(), mods, sizeof mods, &needed ) )
        return;
    const int n = min( int( needed / sizeof( HMODULE ) ), 256 );
    for ( int i = 0; i < n; ++i )
    {
        MODULEINFO mi;
        if ( !GetModuleInformation( GetCurrentProcess(), mods[i], &mi, sizeof mi ) )
            continue;
        const uintptr_t lo = reinterpret_cast<uintptr_t>( mi.lpBaseOfDll );
        bool known = false;
        for ( int k = 0; k < g_profModuleCount && !known; ++k )
            known = g_profModules[k].lo == lo;
        if ( known || g_profModuleCount >= 256 )
            continue;
        ProfModule &m = g_profModules[g_profModuleCount];
        m.lo = lo;
        m.hi = lo + mi.SizeOfImage;
        GetModuleBaseNameA( GetCurrentProcess(), mods[i], m.name, sizeof m.name );
        m.system = ProfIsSystemName( m.name );
        InterlockedIncrement( &g_profModuleCount );   // entry complete before it is counted
    }
}

int ProfFindModule( uintptr_t a )
{
    const int n = g_profModuleCount;
    for ( int k = 0; k < n; ++k )
        if ( a >= g_profModules[k].lo && a < g_profModules[k].hi )
            return k;
    return -1;
}

// True if the bytes before v are a call instruction (v is a return address).
bool ProfIsAfterCall( uintptr_t v )
{
    const uint8_t *c = reinterpret_cast<const uint8_t *>( v );
    if ( c[-5] == 0xE8 )
        return true;
    if ( c[-6] == 0xFF && ( c[-5] == 0x15 || ( c[-5] >= 0x90 && c[-5] <= 0x97 && c[-5] != 0x94 ) ) )
        return true;
    if ( c[-3] == 0xFF && c[-2] >= 0x50 && c[-2] <= 0x57 && c[-2] != 0x54 )
        return true;
    if ( c[-2] == 0xFF && ( ( c[-1] >= 0xD0 && c[-1] <= 0xD7 ) || ( c[-1] >= 0x10 && c[-1] <= 0x17 && c[-1] != 0x14 && c[-1] != 0x15 ) ) )
        return true;
    return false;
}

DWORD WINAPI ProfSampler( LPVOID )
{
    HANDLE main = OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_profMainThreadId );
    if ( !main )
        return 0;
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_HIGHEST );
    timeBeginPeriod( 1 );
    static uintptr_t stack[2048];
    DWORD lastRefresh = 0;
    for ( ;; )
    {
        Sleep( 1 );
        if ( GetTickCount() - lastRefresh > 2000 )
        {
            ProfRefreshModules();
            lastRefresh = GetTickCount();
        }
        if ( !g_profInGameLogic || g_profSiteDepth )
            continue;
        if ( SuspendThread( main ) == DWORD( -1 ) )
            continue;
        // Only the context and a copy of the stack top while suspended: no locks, no allocation.
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        const bool inRegion = g_profInGameLogic && !g_profSiteDepth;
        const bool ok = inRegion && GetThreadContext( main, &ctx );
        LARGE_INTEGER t;
        QueryPerformanceCounter( &t );
        int words = 0;
        if ( ok && ctx.Esp < g_profStackBase && g_profStackBase - ctx.Esp < 0x1000000 )
        {
            words = int( ( std::min )( uintptr_t( g_profStackBase - ctx.Esp ) / 4, uintptr_t( 2048 ) ) );
            __try
            {
                memcpy( stack, reinterpret_cast<void *>( ctx.Esp ), words * 4 );
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
                words = 0;
            }
        }
        ResumeThread( main );
        if ( !ok )
            continue;
        uintptr_t addr = ctx.Eip;
        const int m = ProfFindModule( addr );
        if ( m < 0 || g_profModules[m].system )
        {
            addr = 0;
            __try
            {
                for ( int i = 0; i < words && !addr; ++i )
                {
                    const int vm = ProfFindModule( stack[i] );
                    if ( vm >= 0 && !g_profModules[vm].system && stack[i] >= g_profModules[vm].lo + 0x1007 &&
                         ProfIsAfterCall( stack[i] ) )
                        addr = stack[i];
                }
            }
            __except ( EXCEPTION_EXECUTE_HANDLER )
            {
            }
        }
        g_profRing[g_profWritten & ( kProfRing - 1 )] = { t.QuadPart, addr };
        InterlockedIncrement( &g_profWritten );
    }
}
} // namespace

static __declspec( naked ) void ProfSiteEnter()
{
    __asm {
        pushad
        pushfd
        push dword ptr [esp+36]     // site index
        push dword ptr [esp+44]     // return address
        call ProfSiteEnterC
        add esp, 8
        mov [esp+36], eax           // site index slot -> target, 'ret' below jumps there
        mov dword ptr [esp+40], offset ProfSiteExit
        popfd
        popad
        ret
    }
}

namespace FrameProfiler
{
namespace
{
const char *const kZoneNames[ZoneCount] = {
    "frame", "time", "game logic", "audio", "lights", "reload checks", "render total", "prepare", "sky/clouds",
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
    double     onCpuPct;                 // share of the frame the main thread was running on a CPU core
    double     siteMs[kProfSiteCount];   // CGame::Process calls
    // plugin code around CGame::Process (sampled): the modules with the most samples, and each one's hottest spot
    struct PluginShare
    {
        int16_t   module;
        uint16_t  count;
        uintptr_t hottest;
    } plugins[4];
    uint16_t   pluginSamples;
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
ULONG64              g_lastThreadCycles;
uint64_t             g_tsc0;
int64_t              g_qpc0;
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
    double              siteSum[kProfSiteCount], siteMax[kProfSiteCount];
    uint32_t            slowPluginSamples[257];   // per module (+1 for unknown), slow frames only
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

// Names the biggest CGame::Process calls of a frame (or averages), largest first.
void SiteLine( Line &line, const double *ms, double scale, const double *maxMs, int top, double minMs )
{
    int order[kProfSiteCount];
    for ( int i = 0; i < kProfSiteCount; ++i )
        order[i] = i;
    std::sort( order, order + kProfSiteCount, [ms]( int a, int b ) { return ms[a] > ms[b]; } );
    for ( int i = 0; i < top && ms[order[i]] * scale >= minMs; ++i )
    {
        if ( maxMs )
            line.Add( " %s %.2f/%.1f |", g_profSites[order[i]].label, ms[order[i]] * scale, maxMs[order[i]] );
        else
            line.Add( " %s %.2f |", g_profSites[order[i]].label, ms[order[i]] * scale );
    }
}

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
    g_profSiteMs = r.siteMs;
    memset( slot.used, 0, sizeof slot.used );
    if ( g_gpuTimers && !g_gpuFailed && slot.disjoint )
    {
        g_context->Begin( slot.disjoint );
        slot.used[ZoneFrame] = true;   // ends when Present starts
        g_context->End( slot.begin[ZoneFrame] );
    }
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

// Plugin samples of one frame, taken between from and to.
void CollectPluginSamples( FrameRecord &r, int64_t from, int64_t to )
{
    struct Bucket
    {
        uintptr_t addr;
        uint16_t  count;
    } buckets[256];
    int      used = 0;
    uint16_t moduleCount[257] = {};
    const LONG end = g_profWritten;
    for ( LONG i = max( 0L, end - kProfRing ); i < end; ++i )
    {
        const ProfSample &smp = g_profRing[i & ( kProfRing - 1 )];
        if ( smp.t <= from || smp.t > to )
            continue;
        ++r.pluginSamples;
        const int m = smp.addr ? ProfFindModule( smp.addr ) : -1;
        ++moduleCount[m + 1];
        const uintptr_t key = smp.addr & ~uintptr_t( 15 );
        int b = 0;
        while ( b < used && buckets[b].addr != key )
            ++b;
        if ( b == used && used < 256 )
            buckets[used++] = { key, 0 };
        if ( b < used )
            ++buckets[b].count;
    }
    for ( int k = 0; k < 4; ++k )
    {
        int best = -2;
        uint16_t bestCount = 0;
        for ( int m = -1; m < 256; ++m )
            if ( moduleCount[m + 1] > bestCount )
            {
                bestCount = moduleCount[m + 1];
                best = m;
            }
        if ( best == -2 )
            break;
        moduleCount[best + 1] = 0;
        uintptr_t hottest = 0;
        uint16_t hotCount = 0;
        for ( int b = 0; b < used; ++b )
            if ( buckets[b].count > hotCount && ( best < 0 ? !buckets[b].addr : ProfFindModule( buckets[b].addr ) == best ) )
            {
                hotCount = buckets[b].count;
                hottest = buckets[b].addr;
            }
        r.plugins[k] = { int16_t( best ), bestCount, hottest };
    }
}

void PluginName( char *out, size_t n, int module )
{
    if ( module < 0 )
        strcpy_s( out, n, "unknown (no plugin/game code on the stack)" );
    else
        _snprintf_s( out, n, _TRUNCATE, "%s", g_profModules[module].name );
}

// Frame time not inside any top-level zone: other plugins' code in the game loop, Windows messages, end-of-frame work.
double Untimed( const FrameRecord &r )
{
    static const Zone top[] = { ZoneTimeUpdate, ZoneGameUpdate, ZoneAudio, ZoneLightUpdate, ZoneReloads, ZoneRenderInGame,
                                ZoneGUI, ZoneHUD, ZonePresent };
    double timed = 0;
    for ( Zone z : top )
        timed += r.cpuMs[z];
    return max( 0.0, r.frameMs - timed );
}

void LogFrame( const FrameRecord &r )
{
    char gpuTotal[32] = "unknown";
    if ( r.gpuValid )
        sprintf_s( gpuTotal, "%.1f ms", r.gpuMs[ZoneFrame] );
    Log( "SLOW frame %llu at %02d:%02d:%02d.%03d: %.1f ms | GPU work %s", r.index, r.time.wHour, r.time.wMinute,
         r.time.wSecond, r.time.wMilliseconds, r.frameMs, gpuTotal );
    Line cpu;
    cpu.Add( "  CPU ms:" );
    for ( int z = 0; z < ZoneCount; ++z )
        if ( z != ZoneFrame && z != ZoneRenderInGame && r.cpuMs[z] >= 0.05 )
            cpu.Add( " %s %.2f |", kZoneNames[z], r.cpuMs[z] );
    cpu.Add( " untimed %.2f | main thread on CPU %.0f%% of the frame", Untimed( r ), r.onCpuPct );
    Log( "%s", cpu.buf );
    Line gl;
    gl.Add( "  game logic calls ms:" );
    double inCalls = 0;
    for ( double v : r.siteMs )
        inCalls += v;
    SiteLine( gl, r.siteMs, 1.0, nullptr, 6, 0.1 );
    gl.Add( " not in these calls (plugin hooks around CGame::Process) %.2f", max( 0.0, r.cpuMs[ZoneGameUpdate] - inCalls ) );
    Log( "%s", gl.buf );
    if ( r.pluginSamples )
    {
        Line pl;
        pl.Add( "  plugin hooks around CGame::Process (%u samples):", r.pluginSamples );
        for ( const auto &share : r.plugins )
        {
            if ( !share.count )
                break;
            char name[96];
            PluginName( name, sizeof name, share.module );
            pl.Add( " %s %.0f%%", name, 100.0 * share.count / r.pluginSamples );
            if ( share.module >= 0 && share.hottest )
                pl.Add( " (hottest +0x%X)", unsigned( share.hottest - g_profModules[share.module].lo ) );
            pl.Add( " |" );
        }
        Log( "%s", pl.buf );
    }
    if ( r.gpuValid )
    {
        Line gpu;
        gpu.Add( "  GPU ms:" );
        for ( int z = 0; z < ZoneCount; ++z )
            if ( kGpuZone[z] && z != ZoneFrame && z != ZoneRenderInGame && r.gpuMs[z] >= 0.05 )
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
        if ( z != ZoneFrame && z != ZoneRenderInGame && s.cpuMax[z] >= 0.05 )
            cpu.Add( " %s %.2f/%.1f |", kZoneNames[z], s.cpuSum[z] / n, s.cpuMax[z] );
    Log( "%s", cpu.buf );
    if ( s.gpuFrames )
    {
        Line gpu;
        gpu.Add( "  GPU avg/max ms: total %.2f/%.1f |", s.gpuSum[ZoneFrame] / s.gpuFrames, s.gpuMax[ZoneFrame] );
        for ( int z = 0; z < ZoneCount; ++z )
            if ( kGpuZone[z] && z != ZoneFrame && z != ZoneRenderInGame && s.gpuMax[z] >= 0.05 )
                gpu.Add( " %s %.2f/%.1f |", kZoneNames[z], s.gpuSum[z] / s.gpuFrames, s.gpuMax[z] );
        Log( "%s", gpu.buf );
    }
    Line gl;
    gl.Add( "  game logic calls avg/max ms:" );
    SiteLine( gl, s.siteSum, 1.0 / n, s.siteMax, 10, 0.05 );
    Log( "%s", gl.buf );
    uint32_t pluginTotal = 0;
    for ( uint32_t c : s.slowPluginSamples )
        pluginTotal += c;
    if ( pluginTotal )
    {
        Line pl;
        pl.Add( "  slow frames, plugin hooks around CGame::Process (%u samples):", pluginTotal );
        uint32_t counts[257];
        memcpy( counts, s.slowPluginSamples, sizeof counts );
        for ( int k = 0; k < 6; ++k )
        {
            int best = -1;
            for ( int i = 0; i < 257; ++i )
                if ( counts[i] && ( best < 0 || counts[i] > counts[best] ) )
                    best = i;
            if ( best < 0 )
                break;
            char name[96];
            PluginName( name, sizeof name, best - 1 );
            pl.Add( " %s %.0f%% |", name, 100.0 * counts[best] / pluginTotal );
            counts[best] = 0;
        }
        Log( "%s", pl.buf );
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
    for ( int i = 0; i < kProfSiteCount; ++i )
    {
        s.siteSum[i] += r.siteMs[i];
        s.siteMax[i] = max( s.siteMax[i], r.siteMs[i] );
    }
    if ( r.frameMs > g_slowMs )
    {
        ++s.slowFrames;
        for ( const auto &share : r.plugins )
            if ( share.count )
                s.slowPluginSamples[share.module + 1] += share.count;
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

void InstallGameProcessHooks()
{
    auto *stubs = static_cast<uint8_t *>( VirtualAlloc( nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
    if ( !stubs )
        return;
    int installed = 0, hooked = 0;
    for ( int i = 0; i < kProfSiteCount; ++i )
    {
        ProfSite &site = g_profSites[i];
        strcpy_s( site.label, site.name );
        auto *at = reinterpret_cast<uint8_t *>( site.at );
        if ( at[0] != 0xE8 )
        {
            Log( "CGame::Process call at 0x%X (%s) is not a call any more - not timed", site.at, site.name );
            continue;
        }
        site.target = site.at + 5 + *reinterpret_cast<int32_t *>( at + 1 );
        if ( site.target != site.original )
        {
            HMODULE mod = nullptr;
            char path[MAX_PATH] = "?";
            if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>( site.target ), &mod ) )
                GetModuleFileNameA( mod, path, MAX_PATH );
            const char *base = strrchr( path, '\\' );
            sprintf_s( site.label, "%s [hooked by %s]", site.name, base ? base + 1 : path );
            ++hooked;
        }
        uint8_t *stub = stubs + i * 16;
        stub[0] = 0x68;   // push i
        *reinterpret_cast<int32_t *>( stub + 1 ) = i;
        stub[5] = 0xE9;   // jmp ProfSiteEnter
        *reinterpret_cast<int32_t *>( stub + 6 ) =
            int32_t( reinterpret_cast<uintptr_t>( &ProfSiteEnter ) - reinterpret_cast<uintptr_t>( stub + 10 ) );
        DWORD prot;
        VirtualProtect( at + 1, 4, PAGE_EXECUTE_READWRITE, &prot );
        *reinterpret_cast<int32_t *>( at + 1 ) = int32_t( reinterpret_cast<uintptr_t>( stub ) - ( site.at + 5 ) );
        VirtualProtect( at + 1, 4, prot, &prot );
        ++installed;
    }
    FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
    Log( "timing %d calls made by CGame::Process (%d of them hooked by other plugins)", installed, hooked );
    g_profMainThreadId = GetCurrentThreadId();
    g_profStackBase = reinterpret_cast<uintptr_t>( reinterpret_cast<NT_TIB *>( NtCurrentTeb() )->StackBase );
    ProfRefreshModules();
    if ( HANDLE th = CreateThread( nullptr, 0, ProfSampler, nullptr, 0, nullptr ) )
        CloseHandle( th );
    for ( const ProfSite &site : g_profSites )
        if ( site.target && site.target != site.original )
            Log( "  %s", site.label );
}

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
    Log( "Slow frames are logged when their GPU times are read back, a few frames later. 'untimed' = frame time outside "
         "the timed phases (other plugins' code in the game loop, Windows messages). 'main thread on CPU' below ~90%% "
         "means it waited (driver, locks) or Windows ran other threads on its core." );
    fflush( g_log );
}

void BeginZone( Zone zone )
{
    if ( GetCurrentThreadId() != g_mainThread || g_zoneDepth[zone]++ )
        return;
    if ( zone == ZoneGameUpdate )
        g_profInGameLogic = 1;
    g_zoneStart[zone] = Now();
    if ( zone == ZonePresent && g_gpuReady && g_gpuTimers && !g_gpuFailed && g_slots[g_current].used[ZoneFrame] )
        g_context->End( g_slots[g_current].end[ZoneFrame] );
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
    if ( zone == ZoneGameUpdate )
        g_profInGameLogic = 0;
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
        InstallGameProcessHooks();
        g_tsc0 = __rdtsc();
        g_qpc0 = now;
        QueryThreadCycleTime( GetCurrentThread(), &g_lastThreadCycles );
        g_lastFrameEnd = now;
        g_summary.start = now;
        StartFrame();
        return;
    }

    Slot &slot = g_slots[g_current];
    FrameRecord &r = slot.record;
    r.frameMs = Ms( now - g_lastFrameEnd );
    GetLocalTime( &r.time );
    CollectPluginSamples( r, g_lastFrameEnd, now );
    // QueryThreadCycleTime counts in TSC ticks; the TSC rate is measured against the QPC clock since the first frame.
    ULONG64 cycles = 0;
    QueryThreadCycleTime( GetCurrentThread(), &cycles );
    const double tscPerMs = double( __rdtsc() - g_tsc0 ) / max( 1.0, Ms( now - g_qpc0 ) );
    if ( tscPerMs > 0 && r.frameMs > 0 )
        r.onCpuPct = min( 100.0, 100.0 * double( cycles - g_lastThreadCycles ) / ( tscPerMs * r.frameMs ) );
    g_lastThreadCycles = cycles;
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
