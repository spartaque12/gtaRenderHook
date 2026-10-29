// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "LightManager.h"
#include "RwD3D1XEngine.h"
#include "D3DRenderer.h"
#include "D3D1XStateManager.h"
#include "RwMethods.h"
#include <game_sa\CCamera.h>
#include <game_sa\CScene.h>
#include "SpotShadowRenderer.h"

CLight CLightManager::m_aLights[1024];
CD3D1XConstantBuffer<LightsCB>* CLightManager::m_pLightBuffer = nullptr;
int CLightManager::m_nLightCount = 0;
float CLightManager::m_fCutImportance = 0.0f;
float CLightManager::m_aKeptImportance[MAX_DYNAMIC_LIGHTS];

void CLightManager::Init()
{
    m_pLightBuffer = new CD3D1XConstantBuffer<LightsCB>();
}

void CLightManager::Shutdown()
{
    delete m_pLightBuffer;
}

void CLightManager::Reset()
{
    m_nLightCount = 0;
}

bool CLightManager::AddLight( const CLight& light )
{
    if ( m_nLightCount < 1024 )
    {
        m_aLights[m_nLightCount] = light;
        m_nLightCount++;
        return true;
    }
    else
        return false;
}

// How much a light adds to the picture, to pick the MAX_DYNAMIC_LIGHTS that matter most. The nearest ones used to win:
// a few cars nearby (2 headlights and 2 dim tail lights each) took half the slots and the street lamps further down
// the road went dark, coming and going with the traffic. Here: (reach / distance to the reach)^2, the share of the
// view the lit area covers, times brightness cubed, so dim lights (tail lights, 0.1) rank below bright street lamps
// (1.4) further away; lights entirely behind the camera last.
static float LightImportance( const CLight &light, const RwV3d &from, const RwV3d &forward )
{
    // the presets reach up to twice fRange (Serendipity's falloff)
    const float reach = light.m_fRange * 2.0f;
    const RwV3d d = { light.m_vPos.x - from.x, light.m_vPos.y - from.y, light.m_vPos.z - from.z };
    const float dist = sqrtf( d.x * d.x + d.y * d.y + d.z * d.z );
    const float brightness = max( light.m_vColor.x, max( light.m_vColor.y, light.m_vColor.z ) );
    const float edge = max( dist - reach, 1.0f ); // within its reach: as if 1 m away
    float importance = brightness * brightness * brightness * ( reach * reach ) / ( edge * edge );
    if ( d.x * forward.x + d.y * forward.y + d.z * forward.z < -reach )
        importance *= 0.01f; // all of it behind the camera
    return importance;
}

void CLightManager::SortByDistance( const RwV3d& from )
{
    RwV3d forward = { 0, 0, 0 };
    if ( Scene.m_pRwCamera )
        forward = RwFrameGetLTM( RwCameraGetFrame( Scene.m_pRwCamera ) )->at;
    // only this frame's lights: the rest of the array holds stale lights from earlier frames
    static float importance[1024];
    static int   order[1024];
    for ( int i = 0; i < m_nLightCount; i++ )
    {
        importance[i] = LightImportance( m_aLights[i], from, forward );
        order[i]      = i;
    }
    const int keep = min( m_nLightCount, MAX_DYNAMIC_LIGHTS );
    std::partial_sort( order, order + keep, order + m_nLightCount,
                       [&]( int a, int b ) { return importance[a] > importance[b]; } );
    // reorder the whole list (it may be sorted again this frame: no light may get lost or doubled)
    static CLight sorted[1024];
    for ( int i = 0; i < m_nLightCount; i++ )
        sorted[i] = m_aLights[order[i]];
    memcpy( m_aLights, sorted, sizeof( CLight ) * m_nLightCount );
    // No popping at the cut: the kept lights fade from nothing at the score of the first one left out to full strength
    // at 1.5 times that, so a light that enters or leaves the set is already dark. Done in Update (once per frame) on the
    // copies sent to the shaders, so a second sort this frame sees the original colours.
    m_fCutImportance = m_nLightCount > MAX_DYNAMIC_LIGHTS ? importance[order[MAX_DYNAMIC_LIGHTS]] : 0.0f;
    for ( int i = 0; i < keep; i++ )
        m_aKeptImportance[i] = importance[order[i]];
}

void CLightManager::Update()
{
    for ( auto i = 0; i < MAX_DYNAMIC_LIGHTS; i++ )
    {
        CLight light = m_aLights[i];
        light.m_fPadding = CSpotShadowRenderer::GetShadowSlot( light ); // its headlight shadow map, 0: none
        if ( m_fCutImportance > 0.0f && i < m_nLightCount )
        {
            const float fade = min( max( ( m_aKeptImportance[i] / m_fCutImportance - 1.0f ) * 2.0f, 0.0f ), 1.0f );
            light.m_vColor = { light.m_vColor.x * fade, light.m_vColor.y * fade, light.m_vColor.z * fade };
        }
        m_pLightBuffer->data.m_aLights[i] = light;
    }

    m_pLightBuffer->Update();
    g_pStateMgr->SetLightCount( min( m_nLightCount, MAX_DYNAMIC_LIGHTS ) );
}

CD3D1XConstantBuffer<LightsCB> * CLightManager::GetBuffer()
{
    return m_pLightBuffer;
}
