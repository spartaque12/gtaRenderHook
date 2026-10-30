// Ray-traced ambient occlusion for RenderHook (compiled with dxc, cs_6_5: inline ray queries).
// TraceAO: per pixel, position and normal from RenderHook's G-buffer 1, a few cosine-weighted rays over the
// hemisphere against the scene's TLAS; output = fraction of rays that escape within Radius.
// BlurAO: separable depth/normal-aware blur of the noisy result (run twice: horizontal, vertical).

cbuffer Constants : register(b0)
{
    row_major float4x4 ViewInv; // view space -> world (RenderHook's mViewInv)
    float  Proj00, Proj11;      // projection scale
    float  FarClip;             // G-buffer depth = viewZ / FarClip
    float  Radius;              // ray length, metres
    uint   Width, Height;
    uint   Frame;               // varies the noise
    uint   Rays;                // rays per pixel
    int2   BlurStep;            // (1, 0) or (0, 1)
    float2 Pad;
};

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float4>   GBuffer : register(t1); // xy: view-space normal (stereographic), zw: depth (2 x 8 bit)
Texture2D<float>    AOIn    : register(t2);
RWTexture2D<float>  AOOut   : register(u0);

// RenderHook's GBuffer.hlsl
float DecodeDepth(float2 enc)
{
    return dot(enc, float2(1.0f, 1.0f / 255.0f)) * FarClip;
}
float3 DecodeNormal(float2 enc)
{
    const float scale = 1.7777f;
    float3 nn = float3(2 * scale * enc.x, 2 * scale * enc.y, 0.0f) + float3(-scale, -scale, 1);
    float  g  = 2.0f / dot(nn, nn);
    return float3(g * nn.xy, g - 1);
}

// view depth (FarClip: sky / nothing) and world normal of a pixel
bool ReadGBuffer(int2 p, out float depth, out float3 normal)
{
    float4 g = GBuffer.Load(int3(p, 0));
    depth    = DecodeDepth(g.zw);
    normal   = normalize(mul(DecodeNormal(g.xy), (float3x3)ViewInv));
    return depth > 0.0f && depth < FarClip * 0.98f;
}

float3 WorldPos(int2 p, float depth)
{
    float2 uv = (p + 0.5f) / float2(Width, Height);
    float2 ndc = float2(uv.x * 2 - 1, (1 - uv.y) * 2 - 1);
    float4 view = float4(ndc.x / Proj00 * depth, ndc.y / Proj11 * depth, depth, 1);
    return mul(view, ViewInv).xyz;
}

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
float Rand(inout uint s)
{
    s = Hash(s);
    return (s & 0xFFFFFF) / 16777216.0f;
}

[numthreads(8, 8, 1)]
void TraceAO(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;
    float depth;
    float3 n;
    if (!ReadGBuffer(id.xy, depth, n))
    {
        AOOut[id.xy] = 1.0f;
        return;
    }
    // off the surface a little more further away (depth precision of the G-buffer)
    const float3 origin = WorldPos(id.xy, depth) + n * (0.02f + depth * 0.002f);
    const float3 t = normalize(abs(n.z) < 0.9f ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
    const float3 b = cross(n, t);
    uint seed = Hash(id.x + id.y * 7919u + Frame * 104729u);
    float visible = 0;
    for (uint r = 0; r < Rays; r++)
    {
        // cosine-weighted hemisphere
        const float u1 = Rand(seed), u2 = Rand(seed);
        const float rad = sqrt(u1), phi = 6.2831853f * u2;
        const float3 dir = normalize(t * (rad * cos(phi)) + b * (rad * sin(phi)) + n * sqrt(max(0.0f, 1 - u1)));
        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = dir;
        ray.TMin      = 0.0f;
        ray.TMax      = Radius;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_NOTHING)
            visible += 1;
        else
            visible += saturate(q.CommittedRayT() / Radius) * 0.35f; // far hits occlude less
    }
    AOOut[id.xy] = visible / Rays;
}

[numthreads(8, 8, 1)]
void BlurAO(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;
    float depth;
    float3 n;
    if (!ReadGBuffer(id.xy, depth, n))
    {
        AOOut[id.xy] = 1.0f;
        return;
    }
    float sum = 0, weights = 0;
    [unroll]
    for (int k = -6; k <= 6; k++)
    {
        const int2 p = clamp(int2(id.xy) + BlurStep * k, int2(0, 0), int2(Width - 1, Height - 1));
        float d;
        float3 nk;
        if (!ReadGBuffer(p, d, nk))
            continue;
        const float w = exp(-k * k / 18.0f) * exp(-abs(d - depth) / (0.05f + depth * 0.02f)) *
                        pow(saturate(dot(n, nk)), 8.0f);
        sum += AOIn.Load(int3(p, 0)) * w;
        weights += w;
    }
    AOOut[id.xy] = weights > 1e-4f ? sum / weights : AOIn.Load(int3(id.xy, 0));
}
