#ifndef SINKINGSTAR_OVERLAY_PANEL_H
#define SINKINGSTAR_OVERLAY_PANEL_H

// ImGui control panel. Runs on its own D3D11 device and window, completely
// separate from the game's D3D12 pipeline, so it cannot disturb rendering.

namespace panel {
    void init();
    void toggle();  // F2
    bool visible();

    // Shared by both backends. `in_game` pins the panel to the top right of the
    // render target instead of filling a dedicated window.
    void draw_ui(bool in_game, float width, float height);
    void load_fonts();
}  // namespace panel

#endif
