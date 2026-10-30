// RHRT64.exe - ray-traced ambient occlusion for RenderHook (DX11, 32-bit) with D3D12 + DXR (64-bit only).
// Started by RenderHook with the game's process id. Mirrors the scene RenderHook streams (RTShared.h): one BLAS per
// model, a TLAS per frame, then traces AO from RenderHook's G-buffer with inline ray queries and hands the result
// back through a shared texture and fences. Log: RHRT64.log next to the exe.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "RTShared.h"
#include "TraceAO.h"
#include "BlurAO.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
FILE *g_log = nullptr;

void Log( const char *fmt, ... )
{
    if ( !g_log )
        return;
    SYSTEMTIME t;
    GetLocalTime( &t );
    fprintf( g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds );
    va_list args;
    va_start( args, fmt );
    vfprintf( g_log, fmt, args );
    va_end( args );
    fputc( '\n', g_log );
    fflush( g_log );
}

template <class T> void Release( T *&p )
{
    if ( p )
        p->Release();
    p = nullptr;
}

rhrt::Shared *g_shared      = nullptr;
HANDLE        g_frameEvent  = nullptr;
HANDLE        g_submitEvent = nullptr;
HANDLE        g_game        = nullptr;

ID3D12Device5              *g_device = nullptr;
ID3D12CommandQueue         *g_queue  = nullptr;
ID3D12CommandAllocator     *g_alloc[rhrt::kFrameSlots] = {};
ID3D12GraphicsCommandList4 *g_list   = nullptr;
ID3D12Fence                *g_ownFence = nullptr; // our submissions, to reuse allocators and free resources
uint64_t                    g_ownValue = 0;
uint64_t                    g_slotValue[rhrt::kFrameSlots] = {};
HANDLE                      g_ownEvent = nullptr;

ID3D12RootSignature  *g_rootSig  = nullptr;
ID3D12PipelineState  *g_tracePso = nullptr;
ID3D12PipelineState  *g_blurPso  = nullptr;
ID3D12DescriptorHeap *g_heap     = nullptr;
UINT                  g_heapStep = 0;

// shared with RenderHook
uint32_t        g_configSerial = 0xFFFFFFFF;
ID3D12Resource *g_gbuffer = nullptr, *g_aoShared = nullptr;
ID3D12Fence    *g_readyFence = nullptr, *g_doneFence = nullptr;
ID3D12Resource *g_aoRaw = nullptr, *g_aoTemp = nullptr;
uint32_t        g_width = 0, g_height = 0;
uint64_t        g_lastFrame = 0; // newest frame taken; RenderHook counts from 1 again with new fences

// per frame slot: constants and TLAS instances (upload heap), TLAS and its scratch
ID3D12Resource *g_constants[rhrt::kFrameSlots] = {};
ID3D12Resource *g_instances[rhrt::kFrameSlots] = {};
ID3D12Resource *g_tlas[rhrt::kFrameSlots]      = {};
ID3D12Resource *g_tlasScratch = nullptr;

struct Mesh
{
    ID3D12Resource *upload = nullptr; // vertices + indices until the BLAS is built
    ID3D12Resource *blas   = nullptr;
    uint32_t        vertexCount = 0, triangleCount = 0;
    bool            built = false;
};
std::unordered_map<uint32_t, Mesh> g_meshes;
struct Garbage
{
    ID3D12Resource *resource;
    uint64_t        fence;
};
std::vector<Garbage> g_garbage;
ID3D12Resource      *g_blasScratch     = nullptr;
uint64_t             g_blasScratchSize = 0;

struct Constants
{
    float    viewInv[16];
    float    proj00, proj11, farClip, radius;
    uint32_t width, height, frame, rays;
    int32_t  blurStep[2];
    float    pad[2];
};

void Retire( ID3D12Resource *r )
{
    if ( r )
        g_garbage.push_back( { r, g_ownValue + 1 } ); // freed once the next submission is done
}

void CollectGarbage()
{
    const uint64_t done = g_ownFence->GetCompletedValue();
    for ( size_t i = 0; i < g_garbage.size(); )
        if ( g_garbage[i].fence <= done )
        {
            g_garbage[i].resource->Release();
            g_garbage[i] = g_garbage.back();
            g_garbage.pop_back();
        }
        else
            i++;
}

ID3D12Resource *CreateBuffer( uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE )
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type                  = heapType;
    D3D12_RESOURCE_DESC desc   = {};
    desc.Dimension             = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width                 = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count      = 1;
    desc.Layout                = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags                 = flags;
    ID3D12Resource *r          = nullptr;
    if ( FAILED( g_device->CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS( &r ) ) ) )
        Log( "buffer of %llu bytes failed", size );
    return r;
}

ID3D12Resource *CreateTexture( uint32_t w, uint32_t h, DXGI_FORMAT format )
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type                  = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc   = {};
    desc.Dimension             = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width                 = w;
    desc.Height                = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format                = format;
    desc.SampleDesc.Count      = 1;
    desc.Flags                 = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r          = nullptr;
    g_device->CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                       IID_PPV_ARGS( &r ) );
    return r;
}

template <class T> T *OpenShared( uint64_t handle )
{
    T *object = nullptr;
    HRESULT hr = g_device->OpenSharedHandle( reinterpret_cast<HANDLE>( handle ), IID_PPV_ARGS( &object ) );
    if ( FAILED( hr ) )
        Log( "OpenSharedHandle failed %08lX", hr );
    return object;
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu( int i )
{
    auto h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>( i ) * g_heapStep;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Gpu( int i )
{
    auto h = g_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>( i ) * g_heapStep;
    return h;
}

// descriptor heap: two SRV pairs (G-buffer + AO input) for the tables of t1-t2, then the UAVs
enum Descriptors
{
    kSrvGBufferA = 0, kSrvAoRaw,  // trace (AO input unused) and horizontal blur
    kSrvGBufferB, kSrvAoTemp,     // vertical blur
    kUavAoRaw, kUavAoTemp, kUavAoShared,
    kDescriptorCount
};

bool CreateDevice()
{
    IDXGIFactory4 *factory = nullptr;
    if ( FAILED( CreateDXGIFactory2( 0, IID_PPV_ARGS( &factory ) ) ) )
        return false;
    LUID luid = { g_shared->adapterLuidLow, g_shared->adapterLuidHigh };
    IDXGIAdapter1 *adapter = nullptr;
    if ( FAILED( factory->EnumAdapterByLuid( luid, IID_PPV_ARGS( &adapter ) ) ) )
    {
        Log( "the game's adapter was not found" );
        return false;
    }
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1( &desc );
    Log( "adapter: %ls", desc.Description );
    HRESULT hr = D3D12CreateDevice( adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS( &g_device ) );
    adapter->Release();
    factory->Release();
    if ( FAILED( hr ) )
    {
        Log( "D3D12CreateDevice failed %08lX", hr );
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    g_device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5 );
    Log( "raytracing tier %d", o5.RaytracingTier );
    if ( o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 )
    {
        Log( "inline ray tracing (DXR 1.1) not supported on this GPU" );
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC q = {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if ( FAILED( g_device->CreateCommandQueue( &q, IID_PPV_ARGS( &g_queue ) ) ) )
        return false;
    for ( auto &a : g_alloc )
        if ( FAILED( g_device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( &a ) ) ) )
            return false;
    if ( FAILED( g_device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS( &g_list ) ) ) )
        return false;
    g_list->Close();
    g_device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( &g_ownFence ) );
    g_ownEvent = CreateEventA( nullptr, FALSE, FALSE, nullptr );

    // root signature: b0 constants, t0 TLAS (root SRV), t1-t2 table, u0 table
    D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, 0 };
    D3D12_DESCRIPTOR_RANGE uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER   params[4] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable           = { 1, &srvRange };
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable           = { 1, &uavRange };
    D3D12_ROOT_SIGNATURE_DESC rs        = { 4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = nullptr, *error = nullptr;
    if ( FAILED( D3D12SerializeRootSignature( &rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error ) ) )
    {
        Log( "root signature: %s", error ? static_cast<const char *>( error->GetBufferPointer() ) : "?" );
        return false;
    }
    hr = g_device->CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( &g_rootSig ) );
    blob->Release();
    if ( FAILED( hr ) )
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.CS             = { g_TraceAO, sizeof g_TraceAO };
    if ( FAILED( hr = g_device->CreateComputePipelineState( &pso, IID_PPV_ARGS( &g_tracePso ) ) ) )
    {
        Log( "trace pipeline failed %08lX", hr );
        return false;
    }
    pso.CS = { g_BlurAO, sizeof g_BlurAO };
    if ( FAILED( hr = g_device->CreateComputePipelineState( &pso, IID_PPV_ARGS( &g_blurPso ) ) ) )
    {
        Log( "blur pipeline failed %08lX", hr );
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptorCount,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    g_device->CreateDescriptorHeap( &hd, IID_PPV_ARGS( &g_heap ) );
    g_heapStep = g_device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );

    // TLAS sized for the most instances
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
    in.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.NumDescs    = rhrt::kMaxInstances;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
    g_device->GetRaytracingAccelerationStructurePrebuildInfo( &in, &pre );
    for ( uint32_t s = 0; s < rhrt::kFrameSlots; s++ )
    {
        g_constants[s] = CreateBuffer( 256 * 3, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ );
        g_instances[s] = CreateBuffer( sizeof( D3D12_RAYTRACING_INSTANCE_DESC ) * rhrt::kMaxInstances, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ );
        g_tlas[s] = CreateBuffer( pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
    }
    g_tlasScratch = CreateBuffer( pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
    Log( "TLAS: %llu KB, scratch %llu KB", pre.ResultDataMaxSizeInBytes >> 10, pre.ScratchDataSizeInBytes >> 10 );
    return g_tlasScratch && g_tlas[rhrt::kFrameSlots - 1];
}

void WaitIdle()
{
    g_queue->Signal( g_ownFence, ++g_ownValue );
    g_ownFence->SetEventOnCompletion( g_ownValue, g_ownEvent );
    WaitForSingleObject( g_ownEvent, 5000 );
}

// (re)opens what RenderHook shares when its configuration changed (start, screen size)
bool OpenSharedResources()
{
    if ( g_configSerial == g_shared->configSerial )
        return g_gbuffer && g_aoShared;
    WaitIdle();
    Release( g_gbuffer );
    Release( g_aoShared );
    Release( g_readyFence );
    Release( g_doneFence );
    Release( g_aoRaw );
    Release( g_aoTemp );
    g_configSerial = g_shared->configSerial;
    g_lastFrame    = 0;
    if ( !g_shared->hGBuffer )
        return false;
    g_gbuffer    = OpenShared<ID3D12Resource>( g_shared->hGBuffer );
    g_aoShared   = OpenShared<ID3D12Resource>( g_shared->hAO );
    g_readyFence = OpenShared<ID3D12Fence>( g_shared->hReadyFence );
    g_doneFence  = OpenShared<ID3D12Fence>( g_shared->hDoneFence );
    g_width      = g_shared->width;
    g_height     = g_shared->height;
    if ( !g_gbuffer || !g_aoShared || !g_readyFence || !g_doneFence )
        return false;
    g_aoRaw  = CreateTexture( g_width, g_height, DXGI_FORMAT_R16_FLOAT );
    g_aoTemp = CreateTexture( g_width, g_height, DXGI_FORMAT_R16_FLOAT );
    if ( !g_aoRaw || !g_aoTemp )
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels             = 1;
    srv.Format                          = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_device->CreateShaderResourceView( g_gbuffer, &srv, Cpu( kSrvGBufferA ) );
    g_device->CreateShaderResourceView( g_gbuffer, &srv, Cpu( kSrvGBufferB ) );
    srv.Format = DXGI_FORMAT_R16_FLOAT;
    g_device->CreateShaderResourceView( g_aoRaw, &srv, Cpu( kSrvAoRaw ) );
    g_device->CreateShaderResourceView( g_aoTemp, &srv, Cpu( kSrvAoTemp ) );
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension                    = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format                           = DXGI_FORMAT_R16_FLOAT;
    g_device->CreateUnorderedAccessView( g_aoRaw, nullptr, &uav, Cpu( kUavAoRaw ) );
    g_device->CreateUnorderedAccessView( g_aoTemp, nullptr, &uav, Cpu( kUavAoTemp ) );
    g_device->CreateUnorderedAccessView( g_aoShared, nullptr, &uav, Cpu( kUavAoShared ) );
    Log( "shared resources %ux%u (config %u)", g_width, g_height, g_configSerial );
    return true;
}

// ---- meshes ----
void UnloadMesh( uint32_t key )
{
    auto it = g_meshes.find( key );
    if ( it == g_meshes.end() )
        return;
    Retire( it->second.upload );
    Retire( it->second.blas );
    g_meshes.erase( it );
}

void LoadMesh( const rhrt::Message &m, const uint8_t *data )
{
    UnloadMesh( m.key );
    if ( m.a == 0 || m.b == 0 )
        return;
    const uint64_t vertexBytes = uint64_t( m.a ) * 12, indexBytes = uint64_t( m.b ) * 6;
    Mesh mesh;
    mesh.vertexCount   = m.a;
    mesh.triangleCount = m.b;
    mesh.upload        = CreateBuffer( ( vertexBytes + indexBytes + 15 ) & ~15ull, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ );
    if ( !mesh.upload )
        return;
    void *mapped = nullptr;
    mesh.upload->Map( 0, nullptr, &mapped );
    memcpy( mapped, data, vertexBytes + indexBytes );
    mesh.upload->Unmap( 0, nullptr );
    g_meshes[m.key] = mesh;
}

D3D12_RAYTRACING_GEOMETRY_DESC GeometryOf( const Mesh &mesh )
{
    D3D12_RAYTRACING_GEOMETRY_DESC g      = {};
    g.Type                                = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags                               = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.Triangles.VertexFormat              = DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.VertexCount               = mesh.vertexCount;
    g.Triangles.VertexBuffer.StartAddress = mesh.upload->GetGPUVirtualAddress();
    g.Triangles.VertexBuffer.StrideInBytes = 12;
    g.Triangles.IndexFormat               = DXGI_FORMAT_R16_UINT;
    g.Triangles.IndexCount                = mesh.triangleCount * 3;
    g.Triangles.IndexBuffer               = mesh.upload->GetGPUVirtualAddress() + uint64_t( mesh.vertexCount ) * 12;
    return g;
}

// builds BLAS for loaded meshes (a budget per frame: a streaming burst spreads over frames)
void BuildPendingBlas()
{
    int built = 0;
    uint64_t scratchOffset = 0;
    for ( auto &kv : g_meshes )
    {
        Mesh &mesh = kv.second;
        if ( mesh.built || !mesh.upload )
            continue;
        if ( built >= 256 )
            break;
        D3D12_RAYTRACING_GEOMETRY_DESC g = GeometryOf( mesh );
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs       = 1;
        in.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
        g_device->GetRaytracingAccelerationStructurePrebuildInfo( &in, &pre );
        const uint64_t scratch = ( pre.ScratchDataSizeInBytes + 255 ) & ~255ull;
        if ( scratchOffset + scratch > g_blasScratchSize )
        {
            if ( built > 0 )
                break; // scratch full this frame
            Retire( g_blasScratch );
            g_blasScratchSize = scratch > ( 8ull << 20 ) ? scratch : ( 8ull << 20 );
            g_blasScratch = CreateBuffer( g_blasScratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
            if ( !g_blasScratch )
                return;
        }
        mesh.blas = CreateBuffer( pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
        if ( !mesh.blas )
            continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.Inputs                           = in;
        build.DestAccelerationStructureData    = mesh.blas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = g_blasScratch->GetGPUVirtualAddress() + scratchOffset;
        g_list->BuildRaytracingAccelerationStructure( &build, 0, nullptr );
        scratchOffset += scratch;
        mesh.built = true;
        // the upload buffer is read by this build: freed after it
        Retire( mesh.upload );
        mesh.upload = nullptr;
        built++;
    }
    if ( built > 0 )
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        g_list->ResourceBarrier( 1, &b );
    }
}

void ReadRing()
{
    int64_t read = g_shared->ringRead;
    const int64_t write = g_shared->ringWrite;
    MemoryBarrier();
    while ( read < write )
    {
        const uint32_t offset = static_cast<uint32_t>( read % rhrt::kRingSize );
        const auto *m = reinterpret_cast<const rhrt::Message *>( g_shared->ring + offset );
        if ( rhrt::kRingSize - offset < sizeof( rhrt::Message ) || m->type == rhrt::kMsgWrap )
        {
            read += rhrt::kRingSize - offset;
            continue;
        }
        if ( m->type == rhrt::kMsgMeshLoad )
            LoadMesh( *m, reinterpret_cast<const uint8_t *>( m + 1 ) );
        else if ( m->type == rhrt::kMsgMeshUnload )
            UnloadMesh( m->key );
        read += m->size;
    }
    MemoryBarrier();
    g_shared->ringRead = read;
}

void Transition( ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to )
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    g_list->ResourceBarrier( 1, &b );
}

void UavBarrier( ID3D12Resource *r )
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource          = r;
    g_list->ResourceBarrier( 1, &b );
}

uint64_t g_stats[3] = {}; // frames, instances, instances skipped (no BLAS yet)

void RenderFrame( const rhrt::Frame &frame )
{
    const uint32_t slot = static_cast<uint32_t>( frame.frame % rhrt::kFrameSlots );
    // the allocator and upload buffers of this slot were used two frames ago
    if ( g_ownFence->GetCompletedValue() < g_slotValue[slot] )
    {
        g_ownFence->SetEventOnCompletion( g_slotValue[slot], g_ownEvent );
        WaitForSingleObject( g_ownEvent, 1000 );
    }
    CollectGarbage();
    g_alloc[slot]->Reset();
    g_list->Reset( g_alloc[slot], nullptr );

    BuildPendingBlas();

    // instances whose model has a BLAS
    D3D12_RAYTRACING_INSTANCE_DESC *descs = nullptr;
    g_instances[slot]->Map( 0, nullptr, reinterpret_cast<void **>( &descs ) );
    uint32_t count = 0;
    const uint32_t n = frame.numInstances < rhrt::kMaxInstances ? frame.numInstances : rhrt::kMaxInstances;
    for ( uint32_t i = 0; i < n; i++ )
    {
        const rhrt::Instance &src = frame.instances[i];
        auto it = g_meshes.find( src.key );
        if ( it == g_meshes.end() || !it->second.built )
            continue;
        D3D12_RAYTRACING_INSTANCE_DESC &d = descs[count++];
        memcpy( d.Transform, src.transform, sizeof d.Transform );
        d.InstanceID                          = i;
        d.InstanceMask                        = 0xFF;
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        d.AccelerationStructure               = it->second.blas->GetGPUVirtualAddress();
    }
    g_instances[slot]->Unmap( 0, nullptr );
    g_stats[0]++;
    g_stats[1] += count;
    g_stats[2] += n - count;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas = {};
    tlas.Inputs.Type                      = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas.Inputs.Flags                     = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tlas.Inputs.NumDescs                  = count;
    tlas.Inputs.DescsLayout               = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas.Inputs.InstanceDescs             = g_instances[slot]->GetGPUVirtualAddress();
    tlas.DestAccelerationStructureData    = g_tlas[slot]->GetGPUVirtualAddress();
    tlas.ScratchAccelerationStructureData = g_tlasScratch->GetGPUVirtualAddress();
    g_list->BuildRaytracingAccelerationStructure( &tlas, 0, nullptr );
    UavBarrier( g_tlas[slot] );

    // constants: trace, horizontal blur, vertical blur
    uint8_t *cb = nullptr;
    g_constants[slot]->Map( 0, nullptr, reinterpret_cast<void **>( &cb ) );
    for ( int pass = 0; pass < 3; pass++ )
    {
        Constants c = {};
        memcpy( c.viewInv, frame.viewInv, sizeof c.viewInv );
        c.proj00      = frame.proj00;
        c.proj11      = frame.proj11;
        c.farClip     = frame.farClip;
        c.radius      = frame.radius;
        c.width       = g_width;
        c.height      = g_height;
        c.frame       = static_cast<uint32_t>( frame.frame );
        c.rays        = frame.rays;
        c.blurStep[0] = pass == 1 ? 1 : 0;
        c.blurStep[1] = pass == 2 ? 1 : 0;
        memcpy( cb + 256 * pass, &c, sizeof c );
    }
    g_constants[slot]->Unmap( 0, nullptr );

    Transition( g_gbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    Transition( g_aoShared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
    ID3D12DescriptorHeap *heaps[] = { g_heap };
    g_list->SetDescriptorHeaps( 1, heaps );
    g_list->SetComputeRootSignature( g_rootSig );
    g_list->SetComputeRootShaderResourceView( 1, g_tlas[slot]->GetGPUVirtualAddress() );
    const UINT gx = ( g_width + 7 ) / 8, gy = ( g_height + 7 ) / 8;
    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = g_constants[slot]->GetGPUVirtualAddress();
    // trace -> raw
    g_list->SetPipelineState( g_tracePso );
    g_list->SetComputeRootConstantBufferView( 0, cbBase );
    g_list->SetComputeRootDescriptorTable( 2, Gpu( kSrvGBufferA ) );
    g_list->SetComputeRootDescriptorTable( 3, Gpu( kUavAoRaw ) );
    g_list->Dispatch( gx, gy, 1 );
    UavBarrier( g_aoRaw );
    // raw -> temp (horizontal)
    g_list->SetPipelineState( g_blurPso );
    g_list->SetComputeRootConstantBufferView( 0, cbBase + 256 );
    g_list->SetComputeRootDescriptorTable( 2, Gpu( kSrvGBufferA ) );
    g_list->SetComputeRootDescriptorTable( 3, Gpu( kUavAoTemp ) );
    g_list->Dispatch( gx, gy, 1 );
    UavBarrier( g_aoTemp );
    // temp -> shared (vertical)
    g_list->SetComputeRootConstantBufferView( 0, cbBase + 512 );
    g_list->SetComputeRootDescriptorTable( 2, Gpu( kSrvGBufferB ) );
    g_list->SetComputeRootDescriptorTable( 3, Gpu( kUavAoShared ) );
    g_list->Dispatch( gx, gy, 1 );
    Transition( g_aoShared, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON );
    Transition( g_gbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON );
    g_list->Close();

    // the G-buffer copy is in place once RenderHook's ready fence reaches this frame
    g_queue->Wait( g_readyFence, frame.frame );
    ID3D12CommandList *lists[] = { g_list };
    g_queue->ExecuteCommandLists( 1, lists );
    g_queue->Signal( g_doneFence, frame.frame );
    g_queue->Signal( g_ownFence, ++g_ownValue );
    g_slotValue[slot] = g_ownValue;
    MemoryBarrier();
    g_shared->submittedFrame = static_cast<int64_t>( frame.frame );
    SetEvent( g_submitEvent );

    if ( g_stats[0] % 600 == 0 )
    {
        Log( "frame %llu: %llu instances/frame traced, %llu without BLAS yet, %zu models", frame.frame,
             g_stats[1] / 600, g_stats[2] / 600, g_meshes.size() );
        g_stats[1] = g_stats[2] = 0;
    }
}

int Run( uint32_t pid )
{
    char mapName[64], frameName[64], submitName[64];
    rhrt::Names( pid, mapName, frameName, submitName, sizeof mapName );
    HANDLE mapping = OpenFileMappingA( FILE_MAP_ALL_ACCESS, FALSE, mapName );
    g_shared       = mapping ? static_cast<rhrt::Shared *>( MapViewOfFile( mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof( rhrt::Shared ) ) )
                             : nullptr;
    g_frameEvent  = OpenEventA( SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, frameName );
    g_submitEvent = OpenEventA( SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, submitName );
    g_game        = OpenProcess( SYNCHRONIZE, FALSE, pid );
    if ( !g_shared || !g_frameEvent || !g_submitEvent )
    {
        Log( "could not open the game's shared memory/events" );
        return 1;
    }
    if ( g_shared->version != rhrt::kVersion )
    {
        Log( "protocol version %u, RenderHook has %u: update both together", rhrt::kVersion, g_shared->version );
        g_shared->helperState = rhrt::kFailed;
        return 1;
    }
    if ( !CreateDevice() )
    {
        g_shared->helperState = rhrt::kFailed;
        return 1;
    }
    g_shared->helperState = rhrt::kReady;
    Log( "ready" );
    for ( ;; )
    {
        HANDLE waits[] = { g_frameEvent, g_game };
        const DWORD r = WaitForMultipleObjects( g_game ? 2 : 1, waits, FALSE, 1000 );
        if ( r == WAIT_OBJECT_0 + 1 )
            break; // the game exited
        ReadRing();
        if ( r != WAIT_OBJECT_0 )
            continue;
        if ( !OpenSharedResources() )
            continue;
        // the newest frame RenderHook published
        const rhrt::Frame *latest = nullptr;
        for ( const auto &f : g_shared->frames )
            if ( f.frame > g_lastFrame && ( !latest || f.frame > latest->frame ) )
                latest = &f;
        if ( !latest )
            continue;
        g_lastFrame = latest->frame;
        RenderFrame( *latest );
    }
    WaitIdle();
    Log( "game exited" );
    return 0;
}
} // namespace

int WINAPI WinMain( HINSTANCE, HINSTANCE, LPSTR cmd, int )
{
    char path[MAX_PATH];
    GetModuleFileNameA( nullptr, path, MAX_PATH );
    if ( char *dot = strrchr( path, '.' ) )
        strcpy_s( dot, path + MAX_PATH - dot, ".log" );
    g_log = fopen( path, "w" );
    const uint32_t pid = static_cast<uint32_t>( strtoul( cmd, nullptr, 10 ) );
    Log( "RHRT64 started for game process %u", pid );
    return Run( pid );
}
