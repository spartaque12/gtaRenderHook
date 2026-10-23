#pragma once
#include "D3D1XStructuredBuffer.h"
#include "D3D1XConstantBuffer.h"
struct CLight
{
    RwV3d m_vPos;
    int   m_nLightType;
    RwV3d m_vColor;
    float m_fRange;
    RwV3d m_vDir;
    float m_fPadding;
};
// nearest lights used per frame; must match aDynamicLights in Deferred.hlsl. With 16, lamps further down a street
// were dropped as soon as traffic lights, signs and cars nearby took the slots
constexpr int MAX_DYNAMIC_LIGHTS = 64;
struct LightsCB
{
    CLight m_aLights[MAX_DYNAMIC_LIGHTS];
};
class CLightManager
{
public:
    static CLight m_aLights[1024];
    static int m_nLightCount;
private:
    static CD3D1XConstantBuffer<LightsCB>* m_pLightBuffer;
public:
    static void Init();
    static void Shutdown();
    static void Reset();
    static bool AddLight( const CLight &light );
    static void SortByDistance( const RwV3d& from );
    static void Update();
    static CD3D1XConstantBuffer<LightsCB>* GetBuffer();
};

