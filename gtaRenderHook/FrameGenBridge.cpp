#include "stdafx.h"
#include "FrameGenBridge.h"
#include "FrameGenShared.h"
#include "D3DRenderer.h"
#include "RwD3D1XEngine.h"
#include "SettingsHolder.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>

namespace
{
bool                  g_started, g_broken;
rhfg::Shared *        g_shared;
HANDLE                g_frameEvent, g_configEvent, g_helper, g_job;
ID3D11Device5 *       g_device;
ID3D11DeviceContext4 *g_context;
ID3D11Texture2D *     g_frames[rhfg::kSlots];
ID3D11Fence *         g_readyFence, *g_doneFence;
UINT                  g_width, g_height;
DXGI_FORMAT           g_format;
uint64_t              g_frame;

template <class T> void Release( T *&p )
{
    if ( p )
    {
        p->Release();
        p = nullptr;
    }
}

void Break( const char *why )
{
    g_pDebug->printError( std::string( "Frame generation bridge disabled: " ) + why );
    g_broken = true;
}

bool StartHelper()
{
    g_started = true;
    g_device  = nullptr;
    ID3D11Device *device = GET_D3D_DEVICE;
    device->QueryInterface( __uuidof( ID3D11Device5 ), reinterpret_cast<void **>( &g_device ) );
    GET_D3D_CONTEXT->QueryInterface( __uuidof( ID3D11DeviceContext4 ), reinterpret_cast<void **>( &g_context ) );
    if ( !g_device || !g_context )
    {
        Break( "D3D11.4 (Windows 10+) interfaces not available" );
        return false;
    }

    const DWORD pid = GetCurrentProcessId();
    char mapName[64], frameName[64], configName[64];
    rhfg::Names( pid, mapName, frameName, configName, sizeof mapName );
    HANDLE mapping = CreateFileMappingA( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof( rhfg::Shared ), mapName );
    g_shared       = mapping ? static_cast<rhfg::Shared *>( MapViewOfFile( mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof( rhfg::Shared ) ) )
                             : nullptr;
    g_frameEvent  = CreateEventA( nullptr, FALSE, FALSE, frameName );
    g_configEvent = CreateEventA( nullptr, FALSE, FALSE, configName );
    if ( !g_shared || !g_frameEvent || !g_configEvent )
    {
        Break( "could not create shared memory/events" );
        return false;
    }
    memset( g_shared, 0, sizeof *g_shared );
    g_shared->version    = rhfg::kVersion;
    g_shared->gameWindow = reinterpret_cast<uint64_t>( GET_D3D_RENDERER->getHWND() );
    g_shared->vsync      = gDebugSettings.GetInt( "FrameGenBridgeVSync" );

    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC desc = {};
    if ( SUCCEEDED( g_device->QueryInterface( __uuidof( IDXGIDevice ), reinterpret_cast<void **>( &dxgi ) ) ) &&
         SUCCEEDED( dxgi->GetAdapter( &adapter ) ) )
        adapter->GetDesc( &desc );
    Release( adapter );
    Release( dxgi );
    g_shared->adapterLuidLow  = desc.AdapterLuid.LowPart;
    g_shared->adapterLuidHigh = desc.AdapterLuid.HighPart;

    // RHFrameGen\RHFrameGen64.exe next to RenderHook's asi
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    HMODULE self = nullptr;
    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>( &StartHelper ), &self );
    GetModuleFileNameA( self, exe, MAX_PATH );
    if ( char *slash = strrchr( exe, '\\' ) )
        strcpy_s( slash + 1, exe + MAX_PATH - slash - 1, "RHFrameGen\\RHFrameGen64.exe" );
    snprintf( cmd, sizeof cmd, "\"%s\" %lu", exe, pid );

    // the helper dies with the game
    g_job = CreateJobObjectA( nullptr, nullptr );
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if ( g_job )
        SetInformationJobObject( g_job, JobObjectExtendedLimitInformation, &limits, sizeof limits );
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if ( !CreateProcessA( exe, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi ) )
    {
        Break( ( std::string( "cannot start " ) + exe ).c_str() );
        return false;
    }
    if ( g_job )
        AssignProcessToJobObject( g_job, pi.hProcess );
    ResumeThread( pi.hThread );
    CloseHandle( pi.hThread );
    g_helper = pi.hProcess;
    return true;
}

HANDLE ShareWithHelper( IUnknown *object, bool fence )
{
    HANDLE h = nullptr, dup = nullptr;
    if ( fence )
        static_cast<ID3D11Fence *>( object )->CreateSharedHandle( nullptr, GENERIC_ALL, nullptr, &h );
    else
    {
        IDXGIResource1 *res = nullptr;
        if ( SUCCEEDED( object->QueryInterface( __uuidof( IDXGIResource1 ), reinterpret_cast<void **>( &res ) ) ) )
        {
            res->CreateSharedHandle( nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h );
            res->Release();
        }
    }
    if ( h )
    {
        DuplicateHandle( GetCurrentProcess(), h, g_helper, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS );
        CloseHandle( h );
    }
    return dup;
}

void ReleaseFrames()
{
    for ( auto &frame : g_frames )
        Release( frame );
    Release( g_readyFence );
    Release( g_doneFence );
}

// (Re)creates the shared slots for the back buffer's size and format and hands them to the helper
bool Configure( const D3D11_TEXTURE2D_DESC &backBuffer )
{
    ReleaseFrames();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = backBuffer.Width;
    d.Height = backBuffer.Height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = backBuffer.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = S_OK;
    for ( auto &frame : g_frames )
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateTexture2D( &d, nullptr, &frame );
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateFence( 0, D3D11_FENCE_FLAG_SHARED, __uuidof( ID3D11Fence ), reinterpret_cast<void **>( &g_readyFence ) );
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateFence( 0, D3D11_FENCE_FLAG_SHARED, __uuidof( ID3D11Fence ), reinterpret_cast<void **>( &g_doneFence ) );
    if ( FAILED( hr ) )
    {
        Break( "could not create the shared frames/fences" );
        return false;
    }
    for ( uint32_t i = 0; i < rhfg::kSlots; i++ )
        g_shared->hFrame[i] = reinterpret_cast<uint64_t>( ShareWithHelper( g_frames[i], false ) );
    g_shared->hReadyFence = reinterpret_cast<uint64_t>( ShareWithHelper( g_readyFence, true ) );
    g_shared->hDoneFence  = reinterpret_cast<uint64_t>( ShareWithHelper( g_doneFence, true ) );
    g_shared->width  = g_width = d.Width;
    g_shared->height = g_height = d.Height;
    g_shared->format = g_format = d.Format;
    g_frame = 0;
    g_shared->frame = 0;
    g_shared->configGeneration++;
    SetEvent( g_configEvent );
    return true;
}
} // namespace

void CFrameGenBridge::PublishFrame()
{
    if ( !gDebugSettings.GetToggleField( "FrameGenBridge" ) || g_broken )
        return;
    if ( !g_started && !StartHelper() )
        return;
    if ( WaitForSingleObject( g_helper, 0 ) == WAIT_OBJECT_0 )
    {
        Break( "helper exited (see RHFrameGen\\RHFrameGen64.log)" );
        return;
    }
    if ( g_shared->state == rhfg::StateFailed )
    {
        Break( g_shared->message );
        return;
    }

    ID3D11Texture2D *backBuffer = nullptr;
    if ( FAILED( GET_D3D_RENDERER->getSwapChain()->GetBuffer( 0, __uuidof( ID3D11Texture2D ), reinterpret_cast<void **>( &backBuffer ) ) ) )
        return;
    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc( &desc );
    if ( ( !g_frames[0] || desc.Width != g_width || desc.Height != g_height || desc.Format != g_format ) && !Configure( desc ) )
    {
        backBuffer->Release();
        return;
    }

    // slot of frame n + 1 held frame n + 1 - kSlots: free once the helper has copied that one out
    const uint64_t next = g_frame + 1;
    if ( next > rhfg::kSlots && g_doneFence->GetCompletedValue() < next - rhfg::kSlots )
    {
        g_shared->droppedFrames++;
        backBuffer->Release();
        return;
    }
    const uint32_t slot = static_cast<uint32_t>( next % rhfg::kSlots );
    g_context->CopyResource( g_frames[slot], backBuffer );
    backBuffer->Release();
    g_context->Signal( g_readyFence, next );
    g_context->Flush();

    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    g_shared->publishQpc[slot] = now.QuadPart;
    g_frame = next;
    g_shared->frame = next;
    SetEvent( g_frameEvent );
}

void CFrameGenBridge::Shutdown()
{
    ReleaseFrames();
    Release( g_context );
    Release( g_device );
    if ( g_job )
        CloseHandle( g_job ); // ends the helper
    g_job = nullptr;
}
