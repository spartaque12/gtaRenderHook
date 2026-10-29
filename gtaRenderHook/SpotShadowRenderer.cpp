// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "SpotShadowRenderer.h"
#include "LightManager.h"
#include "Renderer.h"
#include "ShadowRenderer.h"
#include "DeferredRenderer.h"
#include "SettingsHolder.h"
#include "D3DRenderer.h"
#include "RwD3D1XEngine.h"
#include "D3D1XStateManager.h"
#include "D3D1XRenderBuffersManager.h"
#include "RwMethods.h"
#include <game_sa\CScene.h>
#include <game_sa\CCamera.h>
#include <game_sa\CEntity.h>
#include <game_sa\CModelInfo.h>

CSpotShadowRenderer::Caster CSpotShadowRenderer::m_aCasters[SPOT_SHADOW_COUNT]{};
float CSpotShadowRenderer::m_aStrength[SPOT_SHADOW_COUNT]{};
RwCamera* CSpotShadowRenderer::m_pCamera = nullptr;
CD3D1XConstantBuffer<CBSpotShadows>* CSpotShadowRenderer::m_pCB = nullptr;
bool CSpotShadowRenderer::m_bRendered = false;

namespace
{
// the beam's half angle (Deferred.hlsl: SpotLightIntensity(..., PI / 3, ...)), 60 degrees
constexpr float kViewWindow = 1.7320508f;
// Deferred.hlsl's HEADLIGHT_RANGE_SCALE (2) with some to spare
constexpr float kReachScale = 2.5f;
// a chosen car's lit area must start within this distance of the camera
constexpr float kMaxDistance = 60.0f;
constexpr float kNear = 0.3f;

float Dot( const RwV3d &a, const RwV3d &b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
RwV3d Sub( const RwV3d &a, const RwV3d &b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
RwV3d Cross( const RwV3d &a, const RwV3d &b )
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
RwV3d Normalize( const RwV3d &a )
{
    const float len = sqrtf( Dot( a, a ) );
    return len > 1e-5f ? RwV3d{ a.x / len, a.y / len, a.z / len } : RwV3d{ 0, 0, 1 };
}
float Distance( const RwV3d &a, const RwV3d &b )
{
    const RwV3d d = Sub( a, b );
    return sqrtf( Dot( d, d ) );
}
// headlights (white, bright), not tail lights (0.1) or other spot lights
bool IsHeadlight( const CLight &light )
{
    return light.m_nLightType == 1 && max( light.m_vColor.x, max( light.m_vColor.y, light.m_vColor.z ) ) > 0.5f;
}
// both headlights of one car: a metre or two apart, pointing the same way
bool SameCar( const RwV3d &posA, const RwV3d &dirA, const RwV3d &posB, const RwV3d &dirB, float maxDist )
{
    return Distance( posA, posB ) < maxDist && Dot( dirA, dirB ) > 0.8f;
}
} // namespace

void CSpotShadowRenderer::Init()
{
    m_pCamera = RwCameraCreate();
    RwCameraSetProjection( m_pCamera, rwPERSPECTIVE );
    RwCameraSetNearClipPlane( m_pCamera, kNear );
    RwCameraSetFarClipPlane( m_pCamera, 30.0f );
    RwCameraSetRaster( m_pCamera, nullptr );
    RwCameraSetZRaster( m_pCamera, RwRasterCreate( SPOT_SHADOW_SIZE * SPOT_SHADOW_COUNT, SPOT_SHADOW_SIZE, 32,
                                                   rwRASTERTYPEZBUFFER | rwRASTERFORMAT32 ) );
    RwV2d vw{ kViewWindow, kViewWindow };
    RwCameraSetViewWindow( m_pCamera, &vw );
    RwObjectHasFrameSetFrame( m_pCamera, RwFrameCreate() );
    RpWorldAddCamera( Scene.m_pRpWorld, m_pCamera );
    gDebugSettings.DebugRenderTargetList.push_back( m_pCamera->zBuffer );
    m_pCB = new CD3D1XConstantBuffer<CBSpotShadows>();
    m_pCB->SetDebugName( "SpotShadowsCB" );
}

void CSpotShadowRenderer::Shutdown()
{
    delete m_pCB;
    m_pCB = nullptr;
    if ( m_pCamera )
    {
        RwRasterDestroy( m_pCamera->zBuffer );
        RwCameraDestroy( m_pCamera );
        m_pCamera = nullptr;
    }
}

void CSpotShadowRenderer::Select()
{
    m_bRendered = false;
    Caster previous[SPOT_SHADOW_COUNT];
    memcpy( previous, m_aCasters, sizeof( previous ) );
    for ( auto &c : m_aCasters )
        c.active = false;
    if ( m_pCamera == nullptr || !gShadowSettings.GetToggleField( "HeadlightShadows" ) )
    {
        for ( auto &s : m_aStrength )
            s = 0.0f;
        return;
    }

    const RwV3d camPos = TheCamera.GetPosition().ToRwV3d();
    const RwV3d camFwd = RwFrameGetLTM( RwCameraGetFrame( Scene.m_pRwCamera ) )->at;

    // one entry per car: its headlights averaged
    struct Candidate
    {
        RwV3d pos, dir;
        float range;
        int   count;
        float score;
    };
    static Candidate cars[256];
    int carCount = 0;
    for ( int i = 0; i < CLightManager::m_nLightCount; i++ )
    {
        const CLight &light = CLightManager::m_aLights[i];
        if ( !IsHeadlight( light ) )
            continue;
        int c = 0;
        for ( ; c < carCount; c++ )
        {
            const RwV3d centre = { cars[c].pos.x / cars[c].count, cars[c].pos.y / cars[c].count,
                                   cars[c].pos.z / cars[c].count };
            if ( SameCar( centre, Normalize( cars[c].dir ), light.m_vPos, light.m_vDir, 2.5f ) )
                break;
        }
        if ( c == carCount )
        {
            if ( carCount == 256 )
                continue;
            cars[carCount++] = { { 0, 0, 0 }, { 0, 0, 0 }, 0.0f, 0, 0.0f };
        }
        Candidate &car = cars[c];
        car.pos   = { car.pos.x + light.m_vPos.x, car.pos.y + light.m_vPos.y, car.pos.z + light.m_vPos.z };
        car.dir   = { car.dir.x + light.m_vDir.x, car.dir.y + light.m_vDir.y, car.dir.z + light.m_vDir.z };
        car.range = max( car.range, light.m_fRange );
        car.count++;
    }

    // nearest lit area in view first; the cars chosen last frame keep their place unless another is much nearer
    for ( int c = 0; c < carCount; c++ )
    {
        Candidate &car = cars[c];
        car.pos = { car.pos.x / car.count, car.pos.y / car.count, car.pos.z / car.count };
        car.dir = Normalize( car.dir );
        const float reach = car.range * kReachScale;
        const RwV3d litCentre = { car.pos.x + car.dir.x * reach * 0.4f, car.pos.y + car.dir.y * reach * 0.4f,
                                  car.pos.z + car.dir.z * reach * 0.4f };
        const float dist = Distance( litCentre, camPos );
        const bool inView = Dot( Sub( litCentre, camPos ), camFwd ) > -reach * 0.5f;
        car.score = ( inView && dist < kMaxDistance + reach ) ? dist : -1.0f;
        for ( const auto &prev : previous )
            if ( prev.active && car.score > 0 && SameCar( prev.pos, prev.dir, car.pos, car.dir, 4.0f ) )
                car.score *= 0.7f;
    }

    Caster chosen[SPOT_SHADOW_COUNT]{};
    int chosenCount = 0;
    for ( ; chosenCount < SPOT_SHADOW_COUNT; chosenCount++ )
    {
        int best = -1;
        for ( int c = 0; c < carCount; c++ )
            if ( cars[c].score > 0 && ( best < 0 || cars[c].score < cars[best].score ) )
                best = c;
        if ( best < 0 )
            break;
        chosen[chosenCount] = { cars[best].pos, cars[best].dir, cars[best].range, true };
        cars[best].score = -1.0f;
    }

    // a car keeps its map while chosen (no flicker between maps); new cars fade in
    bool used[SPOT_SHADOW_COUNT]{};
    bool placed[SPOT_SHADOW_COUNT]{};
    for ( int k = 0; k < chosenCount; k++ )
        for ( int s = 0; s < SPOT_SHADOW_COUNT; s++ )
            if ( !used[s] && previous[s].active &&
                 SameCar( previous[s].pos, previous[s].dir, chosen[k].pos, chosen[k].dir, 4.0f ) )
            {
                m_aCasters[s] = chosen[k];
                used[s] = placed[k] = true;
                break;
            }
    for ( int k = 0; k < chosenCount; k++ )
    {
        if ( placed[k] )
            continue;
        for ( int s = 0; s < SPOT_SHADOW_COUNT; s++ )
            if ( !used[s] )
            {
                m_aCasters[s] = chosen[k];
                m_aStrength[s] = 0.0f;
                used[s] = true;
                break;
            }
    }
    for ( int s = 0; s < SPOT_SHADOW_COUNT; s++ )
        m_aStrength[s] = m_aCasters[s].active ? min( m_aStrength[s] + 0.15f, 1.0f ) : 0.0f;
}

void CSpotShadowRenderer::Render()
{
    bool any = false;
    for ( const auto &c : m_aCasters )
        any |= c.active;
    if ( !any )
        return;

    CD3DRenderer* renderer = static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine )->getRenderer();
    renderer->BeginDebugEvent( L"Headlight shadows" );
    const UINT oldStage = m_uiDeferredStage;
    m_uiDeferredStage = 2;
    RwRGBA clearColour{ 0, 0, 0, 0 };
    RwCameraClear( m_pCamera, &clearColour, rwCAMERACLEARZ );

    for ( int s = 0; s < SPOT_SHADOW_COUNT; s++ )
    {
        const Caster &caster = m_aCasters[s];
        if ( !caster.active )
            continue;
        const float farPlane = caster.range * kReachScale + 2.0f;

        // looking down the beam; the same basis as the sun's (right = up x at)
        RwMatrix* m = RwFrameGetMatrix( RwCameraGetFrame( m_pCamera ) );
        const RwV3d at = caster.dir;
        const RwV3d worldUp = fabsf( at.z ) > 0.95f ? RwV3d{ 1, 0, 0 } : RwV3d{ 0, 0, 1 };
        const RwV3d right = Normalize( Cross( worldUp, at ) );
        m->right = right;
        m->up    = Cross( right, at );
        m->at    = at;
        m->pos   = caster.pos;
        m->flags = 0;
        m->pad1 = m->pad2 = 0;
        m->pad3 = 0x3F800000;
        RwFrameUpdateObjects( RwCameraGetFrame( m_pCamera ) );
        RwCameraSetNearClipPlane( m_pCamera, kNear );
        RwCameraSetFarClipPlane( m_pCamera, farPlane );
        RwCameraSync( m_pCamera );
        RwCameraBeginUpdate( m_pCamera );

        D3D11_VIEWPORT vp;
        vp.Width    = static_cast<FLOAT>( SPOT_SHADOW_SIZE );
        vp.Height   = static_cast<FLOAT>( SPOT_SHADOW_SIZE );
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = static_cast<FLOAT>( SPOT_SHADOW_SIZE * s );
        vp.TopLeftY = 0;
        g_pStateMgr->SetViewport( vp );
        g_pRenderBuffersMgr->Multipy4x4Matrices( (RwGraphicsMatrix *)&m_pCB->data.ViewProj[s],
                                                 (RwGraphicsMatrix *)&RwD3D9D3D9ViewTransform,
                                                 (RwGraphicsMatrix *)&RwD3D9D3D9ProjTransform );

        // What's on screen and around the camera (the sun's caster lists are empty at night), inside the beam. Not
        // the car itself: its headlights sit inside its body.
        auto renderIfInBeam = [&]( CEntity* entity ) {
            if ( entity == nullptr || entity->m_pRwObject == nullptr )
                return;
            CBaseModelInfo* info = CModelInfo::ms_modelInfoPtrs[entity->m_nModelIndex];
            if ( info == nullptr || info->m_pColModel == nullptr )
                return;
            const float radius = info->m_pColModel->m_boundSphere.m_fRadius;
            const RwV3d pos = entity->GetPosition().ToRwV3d();
            const RwV3d toEntity = Sub( pos, caster.pos );
            const float dist = sqrtf( Dot( toEntity, toEntity ) );
            if ( dist > farPlane + radius || Dot( toEntity, at ) < -radius )
                return;
            if ( entity->m_nType == eEntityType::ENTITY_TYPE_VEHICLE && dist < radius )
                return;
            // outside the 60 degree cone, sphere included
            if ( dist > radius && Dot( toEntity, at ) < dist * 0.5f - radius * 0.866f )
                return;
            CRendererRH::RenderShadowCasterEntity( entity );
        };
        for ( UINT i = 0; i < CRendererRH::ms_nNoOfVisibleEntities; i++ )
            renderIfInBeam( CRendererRH::ms_aVisibleEntityPtrs[i] );
        for ( UINT i = 0; i < CRendererRH::ms_nNoOfInVisibleEntities; i++ )
            renderIfInBeam( CRendererRH::ms_aInVisibleEntityPtrs[i] );
        for ( auto entity : CRendererRH::ms_aVisibleEntities )
            renderIfInBeam( entity );

        RwCameraEndUpdate( m_pCamera );
    }
    m_uiDeferredStage = oldStage;
    m_bRendered = true;
    renderer->EndDebugEvent();
}

float CSpotShadowRenderer::GetShadowSlot( const CLight &light )
{
    if ( !m_bRendered || !IsHeadlight( light ) )
        return 0.0f;
    for ( int s = 0; s < SPOT_SHADOW_COUNT; s++ )
        if ( m_aCasters[s].active && SameCar( m_aCasters[s].pos, m_aCasters[s].dir, light.m_vPos, light.m_vDir, 1.6f ) )
            return static_cast<float>( s + 1 );
    return 0.0f;
}

void CSpotShadowRenderer::SetShadowBuffer()
{
    if ( m_pCB == nullptr )
        return;
    for ( int s = 0; s < 4; s++ )
        m_pCB->data.Strength[s] = ( m_bRendered && s < SPOT_SHADOW_COUNT && m_aCasters[s].active ) ? m_aStrength[s] : 0.0f;
    m_pCB->Update();
    g_pStateMgr->SetConstantBufferPS( m_pCB, 11 );
    g_pStateMgr->SetRaster( m_bRendered ? m_pCamera->zBuffer : nullptr, 8 );
}

void CSpotShadowRenderer::UnsetShadowBuffer()
{
    g_pStateMgr->SetRaster( nullptr, 8 );
}
