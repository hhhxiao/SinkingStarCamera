#ifndef SINKINGSTAR_OVERLAY_PANEL_H
#define SINKINGSTAR_OVERLAY_PANEL_H

// ImGui control panel. Runs on its own D3D11 device and window, completely
// separate from the game's D3D12 pipeline, so it cannot disturb rendering.

namespace panel {
    void init();
    void toggle();  // F2
    bool visible();
}  // namespace panel

#endif
