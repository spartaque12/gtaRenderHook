#pragma once
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
    static void Shutdown();
};
