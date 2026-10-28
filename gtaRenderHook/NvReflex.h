#pragma once
/*!
    NVIDIA Reflex low latency on RenderHook's D3D11 device, through the driver's nvapi.dll (no NVAPI library needed:
    functions by interface id). Reflex delays the start of each frame, before the game reads input, so the GPU has no
    queue of finished frames waiting and input reaches the screen sooner. Debug block: NvReflex, NvReflexBoost.
    Markers tell the driver where each frame is simulated, submitted and presented.
*/
class CNvReflex
{
public:
    enum Marker
    {
        SimulationStart   = 0,
        SimulationEnd     = 1,
        RenderSubmitStart = 2,
        RenderSubmitEnd   = 3,
        PresentStart      = 4,
        PresentEnd        = 5,
    };
    /*!
        Start of the game's frame, before input: sleeps as long as Reflex wants, then marks the simulation start
    */
    static void FrameStart();
    static void SetMarker( Marker marker );
    static bool IsActive() { return m_bActive; }

private:
    static bool m_bActive;
};
