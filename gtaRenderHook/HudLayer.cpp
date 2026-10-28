#include "stdafx.h"
#include "HudLayer.h"
#include "D3D1XStateManager.h"
#include "D3DRenderer.h"
#include "D3D1XTexture.h"
#include "D3D1XBaseTexture.h"
#include "D3D1XDepthStencilTexture.h"
#include "FullscreenQuad.h"
#include "FrameGenBridge.h"
#include "RwD3D1XEngine.h"
#include "SettingsHolder.h"
#include <game_sa\CScene.h>

RwRaster *CHudLayer::m_pLayerRaster = nullptr;
bool      CHudLayer::m_bActive      = false;

void CHudLayer::Init()
{
    m_pLayerRaster = RwRasterCreate( RsGlobal.maximumWidth, RsGlobal.maximumHeight, 32,
                                     rwRASTERTYPECAMERATEXTURE | rwRASTERFORMATDEFAULT ); // RGBA8, premultiplied
}

void CHudLayer::Shutdown()
{
    if ( m_pLayerRaster )
        RwRasterDestroy( m_pLayerRaster );
    m_pLayerRaster = nullptr;
}

void CHudLayer::QueueTextureReload()
{
    CRwD3D1XEngine *dxEngine = (CRwD3D1XEngine *)g_pRwCustomEngine;
    if ( dxEngine->m_bScreenSizeChanged && m_pLayerRaster )
    {
        m_pLayerRaster->width  = RsGlobal.maximumWidth;
        m_pLayerRaster->height = RsGlobal.maximumHeight;
        dxEngine->m_pRastersToReload.push_back( m_pLayerRaster );
    }
}

void CHudLayer::Begin()
{
    if ( !gDebugSettings.GetToggleField( "HudLayer" ) || !m_pLayerRaster )
        return;
    // SetRenderTargets clears the layer to (0, 0, 0, 0)
    g_pRwCustomEngine->SetRenderTargets( &m_pLayerRaster, Scene.m_pRwCamera->zBuffer, 1 );
    g_pStateMgr->FlushRenderTargets();
    g_pStateMgr->SetHudLayerMode( true );
    m_bActive = true;
}

void CHudLayer::End()
{
    if ( !m_bActive )
        return;
    m_bActive = false;
    g_pStateMgr->SetHudLayerMode( false );

    // the frame again, without clearing it (CRwD3D1XEngine::SetRenderTargets would)
    auto frameTarget = dynamic_cast<ID3D1XRenderTargetViewable *>( GetD3D1XRaster( Scene.m_pRwCamera->frameBuffer )->resourse );
    auto depthTarget = dynamic_cast<CD3D1XDepthStencilTexture *>( GetD3D1XRaster( Scene.m_pRwCamera->zBuffer )->resourse );
    if ( !frameTarget )
        return;
    ID3D11RenderTargetView *rtv = frameTarget->GetRenderTargetView();
    g_pStateMgr->SetRenderTargets( 1, &rtv, depthTarget ? depthTarget->GetDepthStencilView() : nullptr );
    g_pStateMgr->FlushRenderTargets();
    CFrameGenBridge::CaptureHudless(); // the frame without HUD, for frame generation

    const int view = gDebugSettings.GetInt( "HudLayerView" );
    if ( view == 1 ) // the frame without HUD, as frame generation would get it
        return;
    if ( view == 2 ) // the HUD layer alone, over grey
    {
        float grey[] = { 0.5f, 0.5f, 0.5f, 1.0f };
        GET_D3D_CONTEXT->ClearRenderTargetView( rtv, grey );
    }
    CFullscreenQuad::DrawPremultiplied( m_pLayerRaster );
}
