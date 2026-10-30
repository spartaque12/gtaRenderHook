// Calls the building draw callback a plugin registered with RHSetBuildingDrawCallback (RenderHookAPI.h).
#pragma once
#include "RenderHookAPI.h"

// RHDrawPass of the current m_uiDeferredStage
int CurrentBuildingDrawPass();

// Returns the callback's RH_DRAW_* flags, 0 when no callback is registered.
// Calls the scene draw callback (RHSetSceneDrawCallback), if any.
void NotifySceneDraw( int pass );

unsigned int NotifyBuildingDraw( void *atomic, const RwMatrix *worldMatrix, int meshIndex, int pass,
                                 bool dualPassEdges );
