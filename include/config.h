#ifndef ORTHOCAM_CONFIG_H
#define ORTHOCAM_CONFIG_H

#include <string>

namespace config {

    // Reads config.json next to the dll. Every value has a sane fallback, so a
    // missing or broken file still boots.
    void init();
    void print_all_info();

    bool debug_mode();
    int poll_hz();
    int rescan_interval_ms();
    std::string target_module();
    std::string log_level();

    // Map zoom. `scale` is the zoom factor at start-up (1.0 = the game's own
    // camera distance), `step` is the multiplier applied per key press.
    float zoom_scale();
    float zoom_step();
    float zoom_min();
    float zoom_max();
    float fov_step();
    float fov_min();
    float fov_max();

}  // namespace config

#endif
