// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "RTBridge.h"
#include "RTShared.h"
#include "D3DRenderer.h"
#include "D3D1XBaseTexture.h"
#include "RwD3D1XEngine.h"
#include "SettingsHolder.h"
#include <game_sa\CScene.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <unordered_set>

namespace
{
template <class T> void Release( T *&p )
{
    if ( p )
        p->Release();
    p = nullptr;
}

void RTLog( const char *fmt, ... )
{
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>( &RTLog ), &self );
    GetModuleFileNameA( self, path, MAX_PATH );
    if ( char *slash = strrchr( path, '\\' ) )
        strcpy_s( slash + 1, path + MAX_PATH - slash - 1, "RHRayTrace\\RHRTBridge.log" );
    static bool first = true;
    FILE *f = fopen( path, first ? "w" : "a" );
    first = false;
    if ( !f )
        return;
    va_list args;
    va_start( args, fmt );
    vfprintf( f, fmt, args );
    va_end( args );
    fputc( '\n', f );
    fclose( f );
}

enum class State
{
    Off,     // setting off, or not started yet
    Running,
    Broken,  // helper failed: RenderHook's own AO for the rest of the session
};
State                  g_state = State::Off;
ID3D11Device5         *g_device  = nullptr;
ID3D11DeviceContext4  *g_context = nullptr;
rhrt::Shared          *g_shared  = nullptr;
HANDLE                 g_mapping = nullptr, g_frameEvent = nullptr, g_submitEvent = nullptr, g_helper = nullptr, g_job = nullptr;
ID3D11Texture2D       *g_gbuffer = nullptr, *g_ao = nullptr;
ID3D11ShaderResourceView *g_aoSrv = nullptr;
ID3D11Fence           *g_readyFence = nullptr, *g_doneFence = nullptr;
uint32_t               g_width = 0, g_height = 0;
uint64_t               g_frame = 0;
bool                   g_collecting = false; // this frame's instances go into a frame slot
bool                   g_published  = false; // ... and the helper has it
rhrt::Frame           *g_slot = nullptr;
std::unordered_set<uint32_t> g_sentMeshes;
uint32_t               g_stats[3] = {}; // meshes sent, frames with RT AO, frames without (late)

bool Enabled() { return gDebugSettings.GetToggleField( "RayTracedAO" ); }

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

void Break( const char *why )
{
    RTLog( "ray-traced AO off: %s", why );
    g_state = State::Broken;
}

bool StartHelper()
{
    GET_D3D_DEVICE->QueryInterface( __uuidof( ID3D11Device5 ), reinterpret_cast<void **>( &g_device ) );
    GET_D3D_CONTEXT->QueryInterface( __uuidof( ID3D11DeviceContext4 ), reinterpret_cast<void **>( &g_context ) );
    if ( !g_device || !g_context )
    {
        Break( "no D3D11.4 device (shared fences)" );
        return false;
    }
    const DWORD pid = GetCurrentProcessId();
    char mapName[64], frameName[64], submitName[64];
    rhrt::Names( pid, mapName, frameName, submitName, sizeof mapName );
    g_mapping = CreateFileMappingA( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof( rhrt::Shared ), mapName );
    g_shared  = g_mapping ? static_cast<rhrt::Shared *>( MapViewOfFile( g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof( rhrt::Shared ) ) )
                          : nullptr;
    g_frameEvent  = CreateEventA( nullptr, FALSE, FALSE, frameName );
    g_submitEvent = CreateEventA( nullptr, FALSE, FALSE, submitName );
    if ( !g_shared || !g_frameEvent || !g_submitEvent )
    {
        Break( "could not create shared memory/events" );
        return false;
    }
    memset( g_shared, 0, offsetof( rhrt::Shared, ring ) );
    g_shared->version = rhrt::kVersion;
    IDXGIDevice  *dxgi    = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC desc = {};
    if ( SUCCEEDED( g_device->QueryInterface( __uuidof( IDXGIDevice ), reinterpret_cast<void **>( &dxgi ) ) ) &&
         SUCCEEDED( dxgi->GetAdapter( &adapter ) ) )
        adapter->GetDesc( &desc );
    Release( adapter );
    Release( dxgi );
    g_shared->adapterLuidLow  = desc.AdapterLuid.LowPart;
    g_shared->adapterLuidHigh = desc.AdapterLuid.HighPart;

    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    HMODULE self = nullptr;
    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>( &StartHelper ), &self );
    GetModuleFileNameA( self, exe, MAX_PATH );
    if ( char *slash = strrchr( exe, '\\' ) )
        strcpy_s( slash + 1, exe + MAX_PATH - slash - 1, "RHRayTrace\\RHRT64.exe" );
    snprintf( cmd, sizeof cmd, "\"%s\" %lu", exe, pid );
    g_job = CreateJobObjectA( nullptr, nullptr );
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if ( g_job )
        SetInformationJobObject( g_job, JobObjectExtendedLimitInformation, &limits, sizeof limits );
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if ( !CreateProcessA( exe, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi ) )
    {
        Break( "cannot start RHRayTrace\\RHRT64.exe" );
        return false;
    }
    if ( g_job )
        AssignProcessToJobObject( g_job, pi.hProcess );
    ResumeThread( pi.hThread );
    CloseHandle( pi.hThread );
    g_helper = pi.hProcess;
    g_state  = State::Running;
    RTLog( "helper started (pid %lu)", pi.dwProcessId );
    return true;
}

// shared G-buffer copy, AO texture and fences for this screen size
bool Configure( ID3D11Texture2D *gbuffer )
{
    D3D11_TEXTURE2D_DESC src;
    gbuffer->GetDesc( &src );
    if ( g_gbuffer && src.Width == g_width && src.Height == g_height )
        return true;
    Release( g_aoSrv );
    Release( g_gbuffer );
    Release( g_ao );
    Release( g_readyFence );
    Release( g_doneFence );
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = src.Width;
    d.Height = src.Height;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Format    = src.Format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = g_device->CreateTexture2D( &d, nullptr, &g_gbuffer );
    d.Format    = DXGI_FORMAT_R16_FLOAT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateTexture2D( &d, nullptr, &g_ao );
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateShaderResourceView( g_ao, nullptr, &g_aoSrv );
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateFence( 0, D3D11_FENCE_FLAG_SHARED, __uuidof( ID3D11Fence ), reinterpret_cast<void **>( &g_readyFence ) );
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateFence( 0, D3D11_FENCE_FLAG_SHARED, __uuidof( ID3D11Fence ), reinterpret_cast<void **>( &g_doneFence ) );
    if ( FAILED( hr ) )
    {
        Break( "could not create the shared textures/fences" );
        return false;
    }
    g_width  = src.Width;
    g_height = src.Height;
    g_frame  = 0;
    for ( auto &f : g_shared->frames )
        f.frame = 0;
    g_shared->submittedFrame = 0;
    g_shared->hGBuffer    = reinterpret_cast<uint64_t>( ShareWithHelper( g_gbuffer, false ) );
    g_shared->hAO         = reinterpret_cast<uint64_t>( ShareWithHelper( g_ao, false ) );
    g_shared->hReadyFence = reinterpret_cast<uint64_t>( ShareWithHelper( g_readyFence, true ) );
    g_shared->hDoneFence  = reinterpret_cast<uint64_t>( ShareWithHelper( g_doneFence, true ) );
    g_shared->width       = g_width;
    g_shared->height      = g_height;
    MemoryBarrier();
    g_shared->configSerial++;
    RTLog( "configured %ux%u, G-buffer format %d", g_width, g_height, src.Format );
    return true;
}

// writes a message into the ring; false: no room now (try again on a later frame)
bool WriteMessage( const rhrt::Message &header, const void *a, size_t aSize, const void *b, size_t bSize )
{
    const uint32_t size = static_cast<uint32_t>( ( sizeof header + aSize + bSize + 7 ) & ~size_t( 7 ) );
    if ( size > rhrt::kRingSize / 4 )
        return false;
    int64_t write = g_shared->ringWrite;
    uint32_t offset = static_cast<uint32_t>( write % rhrt::kRingSize );
    int64_t needed = size;
    if ( offset + size > rhrt::kRingSize )
        needed += rhrt::kRingSize - offset; // the rest of the ring is skipped
    if ( write + needed - g_shared->ringRead > rhrt::kRingSize )
        return false;
    if ( offset + size > rhrt::kRingSize )
    {
        if ( rhrt::kRingSize - offset >= sizeof( rhrt::Message ) )
        {
            rhrt::Message wrap = {};
            wrap.type = rhrt::kMsgWrap;
            memcpy( g_shared->ring + offset, &wrap, sizeof wrap );
        }
        write += rhrt::kRingSize - offset;
        offset = 0;
    }
    rhrt::Message m = header;
    m.size = size;
    memcpy( g_shared->ring + offset, &m, sizeof m );
    if ( aSize )
        memcpy( g_shared->ring + offset + sizeof m, a, aSize );
    if ( bSize )
        memcpy( g_shared->ring + offset + sizeof m + aSize, b, bSize );
    MemoryBarrier();
    g_shared->ringWrite = write + size;
    return true;
}

// the model's triangles that block light: alpha-tested and see-through materials are left out for now
bool SendMesh( uint32_t key, RpGeometry *geometry )
{
    const RpMorphTarget *morph = geometry->morphTarget;
    if ( !morph || !morph->verts || !geometry->triangles || geometry->numVertices <= 0 || geometry->numVertices > 65535 )
        return WriteMessage( { rhrt::kMsgMeshLoad, 0, key, 0, 0, 0 }, nullptr, 0, nullptr, 0 ); // nothing to trace
    static std::vector<uint16_t> indices;
    indices.clear();
    for ( int t = 0; t < geometry->numTriangles; t++ )
    {
        const RpTriangle &tri = geometry->triangles[t];
        if ( tri.matIndex >= 0 && tri.matIndex < geometry->matList.numMaterials )
        {
            const RpMaterial *material = geometry->matList.materials[tri.matIndex];
            if ( !material || material->color.alpha < 250 )
                continue;
            if ( material->texture && material->texture->raster && GetD3D1XRaster( material->texture->raster )->alpha )
                continue;
        }
        indices.push_back( tri.vertIndex[0] );
        indices.push_back( tri.vertIndex[1] );
        indices.push_back( tri.vertIndex[2] );
    }
    const uint32_t triangles = static_cast<uint32_t>( indices.size() / 3 );
    rhrt::Message m = { rhrt::kMsgMeshLoad, 0, key, triangles ? static_cast<uint32_t>( geometry->numVertices ) : 0, triangles, 0 };
    return triangles ? WriteMessage( m, morph->verts, sizeof( RwV3d ) * geometry->numVertices, indices.data(),
                                     indices.size() * sizeof( uint16_t ) )
                     : WriteMessage( m, nullptr, 0, nullptr, 0 );
}

// general 4x4 inverse (RenderHook's view matrix has x flipped)
bool Invert( const float *m, float *out )
{
    float inv[16];
    inv[0]  = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7]  = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if ( fabsf( det ) < 1e-12f )
        return false;
    for ( int i = 0; i < 16; i++ )
        out[i] = inv[i] / det;
    return true;
}
} // namespace

void CRTBridge::BeginFrame()
{
    g_collecting = false;
    g_published  = false;
    if ( g_state == State::Broken || !Enabled() )
        return;
    if ( g_state == State::Off && !StartHelper() )
        return;
    if ( g_shared->helperState == rhrt::kFailed )
    {
        Break( "the helper failed, see RHRayTrace\\RHRT64.log" );
        return;
    }
    if ( g_shared->helperState != rhrt::kReady )
        return; // still starting
    // the slot two frames back: the helper must have taken it
    rhrt::Frame &slot = g_shared->frames[( g_frame + 1 ) % rhrt::kFrameSlots];
    if ( slot.frame > static_cast<uint64_t>( g_shared->submittedFrame ) )
        return;
    g_slot              = &slot;
    g_slot->numInstances = 0;
    g_collecting        = true;
}

void CRTBridge::AddInstance( void *resEntry, RpGeometry *geometry, const RwMatrix *ltm )
{
    if ( !g_collecting || !resEntry || !geometry || !ltm || g_slot->numInstances >= rhrt::kMaxInstances )
        return;
    const uint32_t key = static_cast<uint32_t>( reinterpret_cast<uintptr_t>( resEntry ) );
    if ( g_sentMeshes.find( key ) == g_sentMeshes.end() )
    {
        if ( !SendMesh( key, geometry ) )
            return;
        g_sentMeshes.insert( key );
        g_stats[0]++;
    }
    rhrt::Instance &inst = g_slot->instances[g_slot->numInstances++];
    inst.key   = key;
    inst.flags = 0;
    const float t[12] = { ltm->right.x, ltm->up.x, ltm->at.x, ltm->pos.x,
                          ltm->right.y, ltm->up.y, ltm->at.y, ltm->pos.y,
                          ltm->right.z, ltm->up.z, ltm->at.z, ltm->pos.z };
    memcpy( inst.transform, t, sizeof t );
}

void CRTBridge::EndGBuffer( RwRaster *normalDepth )
{
    if ( !g_collecting )
        return;
    g_collecting = false;
    ID3D11Resource *res = GetD3D1XRaster( normalDepth )->resourse->GetResource();
    ID3D11Texture2D *tex = nullptr;
    if ( !res || FAILED( res->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast<void **>( &tex ) ) ) )
        return;
    const bool ok = Configure( tex );
    tex->Release();
    if ( !ok )
        return;
    const RwGraphicsMatrix *view = reinterpret_cast<const RwGraphicsMatrix *>( &RwD3D9D3D9ViewTransform );
    const RwGraphicsMatrix *proj = reinterpret_cast<const RwGraphicsMatrix *>( &RwD3D9D3D9ProjTransform );
    if ( !Invert( reinterpret_cast<const float *>( view ), g_slot->viewInv ) )
        return;
    g_slot->proj00  = proj->m[0].x;
    g_slot->proj11  = proj->m[1].y;
    g_slot->farClip = Scene.m_pRwCamera->farPlane;
    g_slot->radius  = gDebugSettings.GetFloat( "RayTracedAORadius" );
    g_slot->rays    = static_cast<uint32_t>( gDebugSettings.GetInt( "RayTracedAORays" ) );
    g_context->CopyResource( g_gbuffer, res );
    const uint64_t frame = ++g_frame;
    g_context->Signal( g_readyFence, frame );
    g_context->Flush(); // the helper waits on this signal
    MemoryBarrier();
    g_slot->frame = frame;
    SetEvent( g_frameEvent );
    g_published = true;
}

bool CRTBridge::BindAO( int slot )
{
    if ( !g_published )
        return false;
    g_published = false;
    // the helper queued its done signal for this frame: only then may our GPU wait for it
    const uint64_t frame = g_frame;
    const DWORD start = GetTickCount();
    while ( static_cast<uint64_t>( g_shared->submittedFrame ) < frame )
    {
        if ( GetTickCount() - start > 20 || g_shared->helperState == rhrt::kFailed )
        {
            g_stats[2]++;
            return false;
        }
        WaitForSingleObject( g_submitEvent, 2 );
    }
    g_context->Wait( g_doneFence, frame );
    g_context->PSSetShaderResources( slot, 1, &g_aoSrv );
    if ( ++g_stats[1] % 600 == 0 )
        RTLog( "frame %llu: %u models sent, RT AO on %u frames, %u late", frame, g_stats[0], g_stats[1], g_stats[2] );
    return true;
}

void CRTBridge::OnResEntryDestroyed( void *resEntry )
{
    if ( !g_shared || g_state != State::Running )
        return;
    const uint32_t key = static_cast<uint32_t>( reinterpret_cast<uintptr_t>( resEntry ) );
    if ( g_sentMeshes.erase( key ) )
        WriteMessage( { rhrt::kMsgMeshUnload, 0, key, 0, 0, 0 }, nullptr, 0, nullptr, 0 );
}

void CRTBridge::Shutdown()
{
    Release( g_aoSrv );
    Release( g_gbuffer );
    Release( g_ao );
    Release( g_readyFence );
    Release( g_doneFence );
    Release( g_context );
    Release( g_device );
    if ( g_job )
        CloseHandle( g_job ); // ends the helper
    g_job   = nullptr;
    g_state = State::Off;
}
