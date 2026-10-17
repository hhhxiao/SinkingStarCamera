#ifndef SINKINGSTAR_OVERLAY_GRID_H
#define SINKINGSTAR_OVERLAY_GRID_H

// Screen space grid overlay.
//
// The engine registers a grid_overlay shader but never draws it, so the grid is
// drawn here instead: a click through, topmost layered window on top of the game
// window, with world space grid lines projected through the camera matrices the
// engine keeps in memory (read only - nothing in the game is written).

namespace overlay {

    void init();
    void tick();      // cheap, called from the mod's poll loop
    void toggle();    // F5
    bool enabled();
    void set_enabled(bool on);

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
