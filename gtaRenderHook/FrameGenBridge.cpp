#include "stdafx.h"
#include "FrameGenBridge.h"
#include "FrameGenShared.h"
#include "D3DRenderer.h"
#include "D3D1XBaseTexture.h"
#include "DeferredRenderer.h"
#include "RwD3D1XEngine.h"
#include "SettingsHolder.h"
#include "TemporalAA.h"
#include "NvReflex.h"
#include "HudLayer.h"
#include <game_sa\CScene.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

namespace
{
// Motion vectors and hardware-style depth for frame generation, from the G-buffer's view depth (gbuffer 1 .zw) and
// RenderHook's object motion (t1). Like RHDLAA's, but from the camera without jitter: frame generation works on the
// finished, resolved image.
const char kMotionShader[] = R"(
cbuffer Reprojection : register(b0)
{
    row_major float4x4 mViewInv;     // view -> world
    row_major float4x4 mViewProj;    // world -> clip, this frame
    row_major float4x4 mPrevViewProj;// world -> clip, previous frame
    float4 vProj;                    // projection _11, _22, _31, _32
    float2 vScreen;
    float fFarClip;
    float fObjectMotion;             // 1: txObjectMotion is bound
}
Texture2D txNormalDepth : register(t0);
Texture2D txObjectMotion : register(t1); // xy pixels current -> previous, z = 1 where an object wrote it

void VS(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 ClipToPixel(float4 clip) { return (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * vScreen; }

struct Output { float2 motion : SV_Target0; float depth : SV_Target1; };

Output PS(float4 pos : SV_Position)
{
    float2 enc = txNormalDepth.Load(int3(pos.xy, 0)).zw;
    float viewZ = dot(enc, float2(1.0, 1.0 / 255.0)) * fFarClip;
    float2 ndc = pos.xy / vScreen * float2(2, -2) + float2(-1, 1);
    float3 ray = float3((ndc.x - vProj.z) / vProj.x, (ndc.y - vProj.w) / vProj.y, 1.0);
    Output o;
    float4 cur, prev;
    if (viewZ <= 0.0) {
        float4 dir = mul(float4(ray, 0), mViewInv); // sky: only the camera rotation matters
        cur = mul(dir, mViewProj);
        prev = mul(dir, mPrevViewProj);
        o.depth = 1.0;
    } else {
        float4 world = mul(float4(ray * viewZ, 1), mViewInv);
        cur = mul(world, mViewProj);
        prev = mul(world, mPrevViewProj);
        o.depth = saturate(cur.z / cur.w);
    }
    o.motion = (prev.w > 0.0 && cur.w > 0.0) ? ClipToPixel(prev) - ClipToPixel(cur) : 0.0;
    if (fObjectMotion > 0.5) {
        float4 own = txObjectMotion.Load(int3(pos.xy, 0));
        if (own.z > 0.5)
            o.motion = own.xy;
    }
    return o;
}
)";

struct Mat
{
    float m[16];
};
struct Reprojection
{
    Mat   viewInv, viewProj, prevViewProj;
    float proj[4];
    float screen[2];
    float farClip;
    float objectMotion;
};

bool                    g_started, g_broken;
rhfg::Shared *          g_shared;
HANDLE                  g_frameEvent, g_configEvent, g_helper, g_job;
ID3D11Device5 *         g_device;
ID3D11DeviceContext4 *  g_context;
ID3DDeviceContextState *g_state;
ID3D11VertexShader *    g_vs;
ID3D11PixelShader *     g_ps;
ID3D11Buffer *          g_cb;
ID3D11Texture2D *       g_frames[rhfg::kSlots];
ID3D11Texture2D *       g_depth[rhfg::kSlots];
ID3D11Texture2D *       g_motion[rhfg::kSlots];
ID3D11Texture2D *       g_hudless[rhfg::kSlots];
ID3D11Texture2D *       g_ui[rhfg::kSlots];
// the frame before the HUD, copied when the HUD layer is composited (CaptureHudless), until the frame is published
ID3D11Texture2D *       g_hudlessStage;
bool                    g_hudCaptured;
ID3D11RenderTargetView *g_depthRtv[rhfg::kSlots];
ID3D11RenderTargetView *g_motionRtv[rhfg::kSlots];
ID3D11Fence *           g_readyFence, *g_doneFence;
UINT                    g_width, g_height;
DXGI_FORMAT             g_format;
uint64_t                g_frame;
Mat                     g_prevView, g_prevProj;
bool                    g_hasPrev;
// Reflex: the game's frame index (counted at frame start) and the events of the marker queue
HANDLE                  g_markerEvent, g_sleptEvent;
uint32_t                g_simIndex;
int64_t                 g_frameStartQpc; // start of the current game frame, for latency measurements
bool                    g_reflexOff;

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

Mat Mul( const Mat &a, const Mat &b )
{
    Mat r = {};
    for ( int i = 0; i < 4; i++ )
        for ( int j = 0; j < 4; j++ )
            for ( int k = 0; k < 4; k++ )
                r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

// general 4x4 inverse (cofactors)
bool Invert( const Mat &in, Mat &out )
{
    const float *m = in.m;
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
    if ( det == 0.0f )
        return false;
    for ( int i = 0; i < 16; i++ )
        out.m[i] = inv[i] / det;
    return true;
}

bool CreateMotionPass()
{
    D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
    ID3DBlob *vs = nullptr, *ps = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile( kMotionShader, sizeof kMotionShader - 1, "FrameGenMotion", nullptr, nullptr, "VS", "vs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors );
    if ( SUCCEEDED( hr ) )
        hr = D3DCompile( kMotionShader, sizeof kMotionShader - 1, "FrameGenMotion", nullptr, nullptr, "PS", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors );
    if ( errors )
    {
        g_pDebug->printError( std::string( "Frame generation motion shader: " ) +
                              static_cast<const char *>( errors->GetBufferPointer() ) );
        errors->Release();
    }
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateVertexShader( vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs );
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreatePixelShader( ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps );
    Release( vs );
    Release( ps );
    D3D11_BUFFER_DESC cbd = { sizeof( Reprojection ), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateBuffer( &cbd, nullptr, &g_cb );
    // separate pipeline state, so RenderHook's isn't touched
    if ( SUCCEEDED( hr ) )
        hr = g_device->CreateDeviceContextState( 0, &level, 1, D3D11_SDK_VERSION, __uuidof( ID3D11Device1 ), nullptr, &g_state );
    return SUCCEEDED( hr );
}

bool StartHelper()
{
    g_started = true;
    ID3D11Device *device = GET_D3D_DEVICE;
    device->QueryInterface( __uuidof( ID3D11Device5 ), reinterpret_cast<void **>( &g_device ) );
    GET_D3D_CONTEXT->QueryInterface( __uuidof( ID3D11DeviceContext4 ), reinterpret_cast<void **>( &g_context ) );
    if ( !g_device || !g_context )
    {
        Break( "D3D11.4 (Windows 10+) interfaces not available" );
        return false;
    }
    if ( !CreateMotionPass() )
    {
        Break( "could not create the motion vector pass" );
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
    char markerName[64], sleptName[64];
    rhfg::ReflexNames( pid, markerName, sleptName, sizeof markerName );
    g_markerEvent = CreateEventA( nullptr, FALSE, FALSE, markerName );
    g_sleptEvent  = CreateEventA( nullptr, FALSE, FALSE, sleptName );
    if ( !g_shared || !g_frameEvent || !g_configEvent || !g_markerEvent || !g_sleptEvent )
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
    for ( uint32_t i = 0; i < rhfg::kSlots; i++ )
    {
        Release( g_depthRtv[i] );
        Release( g_motionRtv[i] );
        Release( g_frames[i] );
        Release( g_depth[i] );
        Release( g_motion[i] );
        Release( g_hudless[i] );
        Release( g_ui[i] );
    }
    Release( g_hudlessStage );
    g_hudCaptured = false;
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
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = S_OK;
    for ( uint32_t i = 0; i < rhfg::kSlots && SUCCEEDED( hr ); i++ )
    {
        d.Format    = backBuffer.Format;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = g_device->CreateTexture2D( &d, nullptr, &g_frames[i] );
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateTexture2D( &d, nullptr, &g_hudless[i] );
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // CHudLayer's raster (rwRASTERFORMATDEFAULT)
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateTexture2D( &d, nullptr, &g_ui[i] );
        d.Format    = DXGI_FORMAT_R32_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateTexture2D( &d, nullptr, &g_depth[i] );
        d.Format = DXGI_FORMAT_R16G16_FLOAT;
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateTexture2D( &d, nullptr, &g_motion[i] );
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateRenderTargetView( g_depth[i], nullptr, &g_depthRtv[i] );
        if ( SUCCEEDED( hr ) )
            hr = g_device->CreateRenderTargetView( g_motion[i], nullptr, &g_motionRtv[i] );
    }
    if ( SUCCEEDED( hr ) )
    {
        D3D11_TEXTURE2D_DESC stage = d;
        stage.Format    = backBuffer.Format;
        stage.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        stage.MiscFlags = 0;
        hr = g_device->CreateTexture2D( &stage, nullptr, &g_hudlessStage );
    }
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
    {
        g_shared->hFrame[i]  = reinterpret_cast<uint64_t>( ShareWithHelper( g_frames[i], false ) );
        g_shared->hDepth[i]  = reinterpret_cast<uint64_t>( ShareWithHelper( g_depth[i], false ) );
        g_shared->hMotion[i] = reinterpret_cast<uint64_t>( ShareWithHelper( g_motion[i], false ) );
        g_shared->hHudless[i] = reinterpret_cast<uint64_t>( ShareWithHelper( g_hudless[i], false ) );
        g_shared->hUi[i]      = reinterpret_cast<uint64_t>( ShareWithHelper( g_ui[i], false ) );
    }
    g_shared->uiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_shared->hReadyFence = reinterpret_cast<uint64_t>( ShareWithHelper( g_readyFence, true ) );
    g_shared->hDoneFence  = reinterpret_cast<uint64_t>( ShareWithHelper( g_doneFence, true ) );
    g_shared->width  = g_width = d.Width;
    g_shared->height = g_height = d.Height;
    g_shared->format = g_format = backBuffer.Format;
    g_frame = 0;
    g_hasPrev = false;
    g_shared->frame = 0;
    g_shared->configGeneration++;
    SetEvent( g_configEvent );
    return true;
}

// depth and motion vectors of this frame into the slot, and its camera into the shared memory
void DrawMotionVectors( uint32_t slot )
{
    Mat view, proj;
    memcpy( view.m, &CTemporalAA::GetView(), sizeof view.m );
    memcpy( proj.m, &CTemporalAA::GetProjection(), sizeof proj.m );
    // a camera that jumped (cut, teleport) has no usable previous frame
    bool reset = !g_hasPrev;
    if ( g_hasPrev )
    {
        Mat viewInv, prevViewInv;
        if ( Invert( view, viewInv ) && Invert( g_prevView, prevViewInv ) )
        {
            const float dx = viewInv.m[12] - prevViewInv.m[12], dy = viewInv.m[13] - prevViewInv.m[13],
                        dz = viewInv.m[14] - prevViewInv.m[14];
            reset = dx * dx + dy * dy + dz * dz > 30.0f * 30.0f;
        }
    }
    const Mat &prevView = reset ? view : g_prevView;
    const Mat &prevProj = reset ? proj : g_prevProj;

    Reprojection r = {};
    Invert( view, r.viewInv );
    r.viewProj     = Mul( view, proj );
    r.prevViewProj = Mul( prevView, prevProj );
    r.proj[0] = proj.m[0];
    r.proj[1] = proj.m[5];
    r.proj[2] = proj.m[8];
    r.proj[3] = proj.m[9];
    r.screen[0] = static_cast<float>( g_width );
    r.screen[1] = static_cast<float>( g_height );
    r.farClip = Scene.m_pRwCamera->farPlane;
    ID3D11ShaderResourceView *objectMotion = nullptr;
    if ( auto raster = CTemporalAA::m_pObjectMotionRaster )
        if ( auto viewable = dynamic_cast<ID3D1XShaderViewable *>( GetD3D1XRaster( raster )->resourse ) )
            objectMotion = viewable->GetShaderResourceView();
    r.objectMotion = objectMotion ? 1.0f : 0.0f;
    ID3D11ShaderResourceView *normalDepth = nullptr;
    if ( auto viewable = dynamic_cast<ID3D1XShaderViewable *>( GetD3D1XRaster( g_pDeferredRenderer->GetNormalDepthRaster() )->resourse ) )
        normalDepth = viewable->GetShaderResourceView();
    D3D11_MAPPED_SUBRESOURCE map;
    if ( SUCCEEDED( g_context->Map( g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map ) ) )
    {
        memcpy( map.pData, &r, sizeof r );
        g_context->Unmap( g_cb, 0 );
    }

    ID3DDeviceContextState *rhState = nullptr;
    g_context->SwapDeviceContextState( g_state, &rhState ); // keeps all of RenderHook's pipeline state
    ID3D11RenderTargetView *rtvs[2] = { g_motionRtv[slot], g_depthRtv[slot] };
    g_context->OMSetRenderTargets( 2, rtvs, nullptr );
    D3D11_VIEWPORT vp = { 0, 0, r.screen[0], r.screen[1], 0, 1 };
    g_context->RSSetViewports( 1, &vp );
    g_context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    g_context->VSSetShader( g_vs, nullptr, 0 );
    g_context->PSSetShader( g_ps, nullptr, 0 );
    g_context->PSSetConstantBuffers( 0, 1, &g_cb );
    ID3D11ShaderResourceView *srvs[2] = { normalDepth, objectMotion };
    g_context->PSSetShaderResources( 0, 2, srvs );
    g_context->Draw( 3, 0 );
    ID3D11ShaderResourceView *nullSrvs[2] = {};
    g_context->PSSetShaderResources( 0, 2, nullSrvs );
    g_context->OMSetRenderTargets( 0, nullptr, nullptr );
    g_context->SwapDeviceContextState( rhState, nullptr );
    Release( rhState );

    rhfg::Camera &camera = g_shared->camera[slot];
    memcpy( camera.view, view.m, sizeof camera.view );
    memcpy( camera.proj, proj.m, sizeof camera.proj );
    memcpy( camera.prevView, prevView.m, sizeof camera.prevView );
    memcpy( camera.prevProj, prevProj.m, sizeof camera.prevProj );
    camera.reset = reset ? 1 : 0;
    g_prevView = view;
    g_prevProj = proj;
    g_hasPrev  = true;
}
bool ReflexActive()
{
    return g_started && !g_broken && !g_reflexOff && g_shared && g_shared->state == rhfg::StateReady &&
           g_shared->reflexReady && gDebugSettings.GetToggleField( "FrameGenBridge" );
}

// queued for the helper's marker thread, which passes it to Streamline at once; a full queue drops the marker
void PushMarker( uint32_t marker )
{
    const uint32_t head = g_shared->markerHead;
    if ( head - g_shared->markerTail >= rhfg::kMarkers )
        return;
    g_shared->markers[head % rhfg::kMarkers] = { g_simIndex, marker };
    _WriteBarrier();
    g_shared->markerHead = head + 1;
    SetEvent( g_markerEvent );
}
} // namespace

void CFrameGenBridge::CaptureHudless()
{
    if ( !g_hudlessStage || g_broken || !gDebugSettings.GetToggleField( "FrameGenBridge" ) )
        return;
    ID3D11Texture2D *backBuffer = nullptr;
    if ( FAILED( GET_D3D_RENDERER->getSwapChain()->GetBuffer( 0, __uuidof( ID3D11Texture2D ), reinterpret_cast<void **>( &backBuffer ) ) ) )
        return;
    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc( &desc );
    if ( desc.Width == g_width && desc.Height == g_height && desc.Format == g_format )
    {
        g_context->CopyResource( g_hudlessStage, backBuffer );
        g_hudCaptured = true;
    }
    backBuffer->Release();
}

void CFrameGenBridge::FrameStart()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    g_frameStartQpc = now.QuadPart; // after NvReflex's sleep (called before us), before the frame reads input
    if ( !ReflexActive() )
        return;
    g_shared->gameMarkers = 1;
    g_simIndex++;
    if ( gDebugSettings.GetToggleField( "FrameGenReflexSleep" ) && !CNvReflex::IsActive() ) // NvReflex sleeps on our device
    {
        // Reflex decides when this frame should start (before the game reads input); the helper sleeps for us
        PushMarker( rhfg::MarkerSleep );
        if ( WaitForSingleObject( g_sleptEvent, 100 ) != WAIT_OBJECT_0 )
        {
            g_pDebug->printError( "Frame generation bridge: Reflex sleep not answered, Reflex off for this session" );
            g_reflexOff          = true;
            g_shared->gameMarkers = 0;
            return;
        }
    }
    PushMarker( rhfg::MarkerSimulationStart );
}

void CFrameGenBridge::Marker( uint32_t marker )
{
    if ( ReflexActive() )
        PushMarker( marker );
}

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
        g_hasPrev = false; // the helper won't see this frame: the next one starts fresh
        backBuffer->Release();
        return;
    }
    const uint32_t slot = static_cast<uint32_t>( next % rhfg::kSlots );
    DrawMotionVectors( slot );
    g_context->CopyResource( g_frames[slot], backBuffer );
    backBuffer->Release();
    // HUD-less frame and HUD layer, when the HUD layer drew this frame's HUD
    bool hasHud = false;
    if ( g_hudCaptured && CHudLayer::m_pLayerRaster )
    {
        auto viewable = dynamic_cast<ID3D1XShaderViewable *>( GetD3D1XRaster( CHudLayer::m_pLayerRaster )->resourse );
        ID3D11ShaderResourceView *srv = viewable ? viewable->GetShaderResourceView() : nullptr;
        ID3D11Resource *layer = nullptr;
        if ( srv )
            srv->GetResource( &layer );
        D3D11_TEXTURE2D_DESC layerDesc = {};
        if ( layer )
            static_cast<ID3D11Texture2D *>( layer )->GetDesc( &layerDesc );
        if ( layer && layerDesc.Width == g_width && layerDesc.Height == g_height && layerDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM )
        {
            g_context->CopyResource( g_hudless[slot], g_hudlessStage );
            g_context->CopyResource( g_ui[slot], layer );
            hasHud = true;
        }
        Release( layer );
    }
    g_hudCaptured = false;
    g_shared->slotHasHud[slot] = hasHud ? 1 : 0;
    g_context->Signal( g_readyFence, next );
    g_context->Flush();
    g_shared->slotFrameIndex[slot] = g_simIndex; // presented under the game's frame index (Reflex markers)
    g_shared->frameStartQpc[slot]  = g_frameStartQpc;
    Marker( rhfg::MarkerRenderSubmitEnd );

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
    Release( g_cb );
    Release( g_vs );
    Release( g_ps );
    Release( g_state );
    Release( g_context );
    Release( g_device );
    if ( g_job )
        CloseHandle( g_job ); // ends the helper
    g_job = nullptr;
}
