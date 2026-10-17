#ifndef ORTHOCAM_D3D12_OVERLAY_H
#define ORTHOCAM_D3D12_OVERLAY_H

// In-game overlay, route 2.
//
// The DWM compositor costs this game roughly 10% of its frame rate whenever a
// transparent window sits on top of it, because the swapchain drops out of
// independent flip. This module instead draws into the game's own swapchain by
// appending a command list to the game's direct queue right before Present.
//
// Safety rules learned from the earlier D3D12 attempts:
//   * never scan for D3D12 objects and never call methods on one we did not get
//     from the game through a documented entry point;
//   * the only vtable slots touched are ID3D12Device::CreateCommandQueue (8)
//     and IDXGISwapChain::Present/Present1 (8/17), all validated to live in the
//     owning module before patching;
//   * every object we call is either ours or was handed to us by the game.

namespace d3d12_overlay {

    // Called from DllMain, before the engine creates its device. Patches the
    // game's D3D12CreateDevice import slot by matching the resolved function
    // address (the import is by ordinal 101, so a name match never worked).
    void early_init();

    // Called from the mod's worker thread after config.json is loaded. Installs
    // the swapchain Present hook and prepares the renderer if d3d12.enabled.
    void init();
    void shutdown();

    // Runtime switch (F11) for the test geometry. The hooks stay installed.
    bool enabled();
    void toggle();

    // Screen-space drawing board. Mouse strokes are sampled on the render
    // thread and drawn into the same backbuffer as the grid.
    bool doodle_supported();
    bool doodle_enabled();
    void set_doodle_enabled(bool on);
    void toggle_doodle();  // F3
    void clear_doodle();   // Shift+F3 or the panel button
    int doodle_width();
    void set_doodle_width(int value);
    void get_doodle_color(int &r, int &g, int &b);
    void set_doodle_color(int r, int g, int b);
    bool doodle_straight();
    void set_doodle_straight(bool on);

}  // namespace d3d12_overlay

#endif
