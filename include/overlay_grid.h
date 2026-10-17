#ifndef SINKINGSTAR_OVERLAY_GRID_H
#define SINKINGSTAR_OVERLAY_GRID_H

#include <vector>

// Screen space grid overlay.
//
// The engine registers a grid_overlay shader but never draws it, so the grid is
// drawn here instead: a click through, topmost layered window on top of the game
// window, with world space grid lines projected through the camera matrices the
// engine keeps in memory (read only - nothing in the game is written).

namespace overlay {

    struct ScreenLine {
        float x0, y0, x1, y1;
    };

    void init();
    void tick();      // cheap, called from the mod's poll loop
    void toggle();    // F5
    bool enabled();
    void set_enabled(bool on);

    // Route 2: when the grid is drawn straight into the game's swapchain the
    // layered window must not exist at all, otherwise DWM drops independent
    // flip and the frame rate loss comes back.
    void set_external_renderer(bool on);
    bool external_renderer();

    // Projects the current grid into screen pixels using the live camera
    // matrices. Returns false until the matrices have been located. Read only.
    bool grid_lines(std::vector<ScreenLine> &out, float width, float height);

    // Runtime tunables (seeded from config.json).
    float cell();
    void set_cell(float value);
    float offset();
    void set_offset(float value);
    int range();
    void set_range(int value);
    void set_color(int r, int g, int b);
    void get_color(int &r, int &g, int &b);
    int width();
    void set_width(int value);

}  // namespace overlay

#endif
