#include "stdafx.h"
#include "NvReflex.h"
#include "D3DRenderer.h"
#include "RwD3D1XEngine.h"
#include "SettingsHolder.h"

bool CNvReflex::m_bActive = false;

namespace
{
// nvapi.h (R560+) layouts and interface ids (nvapi_interface.h); same types, so the same layout as NVIDIA's headers
typedef unsigned char      NvBool;
typedef unsigned int       NvU32;
typedef unsigned long long NvU64;
struct SleepModeParams
{
    NvU32  version;
    NvBool bLowLatencyMode;
    NvBool bLowLatencyBoost;
    NvU32  minimumIntervalUs;
    NvBool bUseMarkersToOptimize;
    NvBool bUseMinQueueTime;
    unsigned char rsvd[30];
};
struct LatencyMarkerParams
{
    NvU32 version;
    NvU64 frameID;
    int   markerType;
    NvU64 rsvd0;
    unsigned char rsvd[56];
};
static_assert( sizeof( SleepModeParams ) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 layout" );
static_assert( sizeof( LatencyMarkerParams ) == 88, "NV_LATENCY_MARKER_PARAMS_V1 layout" );
template <class T> NvU32 Version( NvU32 ver ) { return static_cast<NvU32>( sizeof( T ) | ( ver << 16 ) ); }

typedef void *( __cdecl *QueryInterfaceFn )( NvU32 id );
typedef int( __cdecl *InitializeFn )();
typedef int( __cdecl *SetSleepModeFn )( IUnknown *device, SleepModeParams *params );
typedef int( __cdecl *SleepFn )( IUnknown *device );
typedef int( __cdecl *SetLatencyMarkerFn )( IUnknown *device, LatencyMarkerParams *params );

bool               g_tried;
SetSleepModeFn     g_setSleepMode;
SleepFn            g_sleep;
SetLatencyMarkerFn g_setLatencyMarker;
NvU64              g_frameId;
bool               g_boost;

bool Init()
{
    g_tried         = true;
    HMODULE nvapi   = LoadLibraryA( "nvapi.dll" ); // the driver's 32-bit NVAPI (SysWOW64)
    auto    query   = nvapi ? reinterpret_cast<QueryInterfaceFn>( GetProcAddress( nvapi, "nvapi_QueryInterface" ) ) : nullptr;
    auto    init    = query ? reinterpret_cast<InitializeFn>( query( 0x0150e828 ) ) : nullptr;
    g_setSleepMode     = query ? reinterpret_cast<SetSleepModeFn>( query( 0xac1ca9e0 ) ) : nullptr;
    g_sleep            = query ? reinterpret_cast<SleepFn>( query( 0x852cd1d2 ) ) : nullptr;
    g_setLatencyMarker = query ? reinterpret_cast<SetLatencyMarkerFn>( query( 0xd9984c05 ) ) : nullptr;
    if ( !init || !g_setSleepMode || !g_sleep || !g_setLatencyMarker || init() != 0 )
    {
        g_pDebug->printMsg( "Reflex: NVAPI not available (not an NVIDIA GPU or driver too old)", 0 ); // printError throws
        return false;
    }
    return true;
}

bool ApplySleepMode( bool boost )
{
    SleepModeParams params = {};
    params.version               = Version<SleepModeParams>( 1 );
    params.bLowLatencyMode       = 1;
    params.bLowLatencyBoost      = boost ? 1 : 0;
    params.bUseMarkersToOptimize = 1;
    const int status = g_setSleepMode( GET_D3D_DEVICE, &params );
    if ( status != 0 )
        g_pDebug->printMsg( "Reflex: NvAPI_D3D_SetSleepMode failed (" + std::to_string( status ) + ")", 0 );
    g_boost = boost;
    return status == 0;
}
} // namespace

void CNvReflex::FrameStart()
{
    const bool wanted = gDebugSettings.GetToggleField( "NvReflex" );
    if ( !wanted )
    {
        if ( m_bActive )
        {
            SleepModeParams params = {}; // low latency mode off
            params.version = Version<SleepModeParams>( 1 );
            g_setSleepMode( GET_D3D_DEVICE, &params );
            m_bActive = false;
        }
        return;
    }
    if ( !m_bActive )
    {
        if ( g_tried && !g_sleep )
            return;
        if ( ( !g_tried && !Init() ) || !ApplySleepMode( gDebugSettings.GetToggleField( "NvReflexBoost" ) ) )
        {
            g_sleep = nullptr; // give up for this session
            return;
        }
        m_bActive = true;
    }
    else if ( g_boost != gDebugSettings.GetToggleField( "NvReflexBoost" ) )
        ApplySleepMode( !g_boost );
    g_frameId++;
    g_sleep( GET_D3D_DEVICE );
    SetMarker( SimulationStart );
}

void CNvReflex::SetMarker( Marker marker )
{
    if ( !m_bActive )
        return;
    LatencyMarkerParams params = {};
    params.version    = Version<LatencyMarkerParams>( 1 );
    params.frameID    = g_frameId;
    params.markerType = marker;
    g_setLatencyMarker( GET_D3D_DEVICE, &params );
}
