#pragma once
#include "D3D1XConstantBuffer.h"

constexpr int SPOT_SHADOW_COUNT = 2;    // cars whose headlights cast shadows; must match mSpotViewProj in Deferred.hlsl
constexpr int SPOT_SHADOW_SIZE  = 1024; // each car's shadow map, side by side in one depth texture

struct CBSpotShadows
{
    RwMatrix ViewProj[SPOT_SHADOW_COUNT];
    float    Strength[4]; // per map: 0 = not rendered this frame, fades in over a few frames for a newly chosen car
};

/*!
    \class CSpotShadowRenderer
    \brief Shadow maps for car headlights.

    Each frame the nearest cars with headlights on that light something in view are chosen. A car's two headlights
    share one perspective depth map, from between them looking down the beam. The point light pass tests those cars'
    headlights against it (the light's fPadding holds its map: 0 = none, 1.. = map + 1).
*/
class CSpotShadowRenderer
{
public:
    static void Init();
    static void Shutdown();
    // picks the cars from this frame's lights (call after PreRender)
    static void Select();
    static void Render();
    // marks a light that belongs to a chosen car with its map; 0 for any other light
    static float GetShadowSlot( const struct CLight &light );
    static void SetShadowBuffer();
    static void UnsetShadowBuffer();
private:
    struct Caster
    {
        RwV3d pos;
        RwV3d dir;
        float range;
        bool  active;
    };
    static Caster    m_aCasters[SPOT_SHADOW_COUNT];
    static float     m_aStrength[SPOT_SHADOW_COUNT];
    static RwCamera* m_pCamera;
    static CD3D1XConstantBuffer<CBSpotShadows>* m_pCB;
    static bool      m_bRendered;
};
