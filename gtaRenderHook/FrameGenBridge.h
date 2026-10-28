#pragma once
#include <cstdint>
/*!
    Frame generation bridge, step 1: hands every finished frame (HUD included) to RHFrameGen64.exe, a 64-bit D3D12
    helper that presents it in its own window over the game. No frame generation yet; this measures what the bridge
    itself costs (Debug block: FrameGenBridge, FrameGenBridgeVSync). The game never waits for the helper: when the
    helper still holds every shared slot, the frame is skipped (counted in the helper's log).
*/
class CFrameGenBridge
{
public:
    /*!
        Publishes the back buffer as the next frame. Call when the frame is complete, before it's presented.
    */
    static void PublishFrame();
    /*!
        Snapshot of the frame without the HUD, taken by CHudLayer::End before it composites the HUD; published with
        the HUD layer itself, so frame generation keeps the HUD out of the interpolation.
    */
    static void CaptureHudless();
    /*!
        Reflex, at the start of the game's frame (before input is read): sleeps as long as Reflex wants (the helper
        calls slReflexSleep) and marks the simulation start. No-op until the helper is up.
    */
    static void FrameStart();
    /*!
        Reflex marker (rhfg::Marker*) for the current game frame, passed on by the helper
    */
    static void Marker( uint32_t marker );
    static void Shutdown();
};
