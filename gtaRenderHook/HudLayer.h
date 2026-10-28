#pragma once
/*!
    HUD layer: the game's 2D (HUD, radar, text, fades, menus drawn in game) goes into its own transparent raster
    instead of onto the frame, and is composited over the frame right before it's presented. The frame itself then
    stays free of the HUD until that point, which frame generation needs: it interpolates the world only, and the
    HUD is put on top of every presented frame (Debug block: HudLayer, HudLayerView).
*/
class CHudLayer
{
public:
    static void Init();
    static void Shutdown();
    static void QueueTextureReload();
    /*!
        Binds the (cleared) HUD layer as render target and switches the state manager to HUD layer blending.
        Call after the frame is complete without HUD (tonemapped output).
    */
    static void Begin();
    /*!
        Binds the frame again and composites the HUD layer over it (depending on HudLayerView). No-op without Begin.
    */
    static void End();
    static bool IsActive() { return m_bActive; }
    static RwRaster *m_pLayerRaster;

private:
    static bool m_bActive;
};
