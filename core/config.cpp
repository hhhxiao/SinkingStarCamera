#include "config.h"

#include <algorithm>
#include <exception>
#include <fstream>

#include "json.hpp"
#include "logger.h"
#include "misc.h"

namespace config {

    namespace {

        bool g_debug_mode = false;
        int g_poll_hz = 60;
        int g_rescan_interval_ms = 3000;
        std::string g_target_module = "sinking_star.exe";
        std::string g_log_level = "info";

        float g_zoom_scale = 1.0f;
        float g_zoom_step = 1.25f;
        float g_zoom_min = 0.05f;
        float g_zoom_max = 50.0f;
        float g_fov_step = 1.15f;
        float g_fov_min = 0.1f;
        float g_fov_max = 3.0f;

        bool g_grid_enabled = false;
        float g_grid_cell = 1.0f;
        int g_grid_range = 20;
        float g_grid_offset = 0.5f;
        int g_grid_width = 2;
        int g_grid_color_r = 0;
        int g_grid_color_g = 0;
        int g_grid_color_b = 0;

    }  // namespace

    void init() {
        const auto path = misc::thisDllDirectory() / "config.json";
        LOG("Config path is %s", path.string().c_str());
        try {
            std::ifstream f(path);
            if (!f.is_open()) throw std::runtime_error("cannot open config file");
            nlohmann::json data = nlohmann::json::parse(f);

            g_debug_mode = data.value("debug_mode", g_debug_mode);
            g_poll_hz = data.value("poll_hz", g_poll_hz);
            if (g_poll_hz < 1 || g_poll_hz > 1000) g_poll_hz = 60;
            g_rescan_interval_ms = data.value("rescan_interval_ms", g_rescan_interval_ms);
            if (g_rescan_interval_ms < 500) g_rescan_interval_ms = 500;
            g_target_module = data.value("target_module", g_target_module);
            g_log_level = data.value("log_level", g_log_level);

            if (data.contains("zoom") && data["zoom"].is_object()) {
                const auto &z = data["zoom"];
                g_zoom_scale = z.value("scale", g_zoom_scale);
                g_zoom_step = z.value("step", g_zoom_step);
                g_zoom_min = z.value("min", g_zoom_min);
                g_zoom_max = z.value("max", g_zoom_max);
            }
            g_zoom_step = std::clamp(g_zoom_step, 1.01f, 4.0f);
            g_zoom_min = std::clamp(g_zoom_min, 0.01f, 1.0f);
            g_zoom_max = std::clamp(g_zoom_max, 1.0f, 1000.0f);
            g_zoom_scale = std::clamp(g_zoom_scale, g_zoom_min, g_zoom_max);

            if (data.contains("fov") && data["fov"].is_object()) {
                const auto &f = data["fov"];
                g_fov_step = f.value("step", g_fov_step);
                g_fov_min = f.value("min", g_fov_min);
                g_fov_max = f.value("max", g_fov_max);
            }
            g_fov_step = std::clamp(g_fov_step, 1.01f, 4.0f);
            g_fov_min = std::clamp(g_fov_min, 0.05f, 1.0f);
            g_fov_max = std::clamp(g_fov_max, 1.0f, 4.0f);

            if (data.contains("grid") && data["grid"].is_object()) {
                const auto &gr = data["grid"];
                g_grid_enabled = gr.value("enabled", g_grid_enabled);
                g_grid_cell = gr.value("cell", g_grid_cell);
                g_grid_range = gr.value("range", g_grid_range);
                g_grid_offset = gr.value("offset", g_grid_offset);
                g_grid_width = gr.value("width", g_grid_width);
                g_grid_color_r = gr.value("color_r", g_grid_color_r);
                g_grid_color_g = gr.value("color_g", g_grid_color_g);
                g_grid_color_b = gr.value("color_b", g_grid_color_b);
            }
            if (!(g_grid_cell > 0.01f)) g_grid_cell = 1.0f;
            if (g_grid_range < 1 || g_grid_range > 200) g_grid_range = 20;
            if (g_grid_offset < -2.0f || g_grid_offset > 2.0f) g_grid_offset = 0.5f;
            if (g_grid_width < 1 || g_grid_width > 8) g_grid_width = 2;
            if (g_grid_color_r < 0 || g_grid_color_r > 255) g_grid_color_r = 0;
            if (g_grid_color_g < 0 || g_grid_color_g > 255) g_grid_color_g = 0;
            if (g_grid_color_b < 0 || g_grid_color_b > 255) g_grid_color_b = 0;
        } catch (std::exception &e) {
            ERR("Using defaults, config load failed: %s", e.what());
        }
    }

    void print_all_info() {
        DBG("debug_mode         = %d", (int)g_debug_mode);
        DBG("poll_hz            = %d", g_poll_hz);
        DBG("rescan_interval_ms = %d", g_rescan_interval_ms);
        DBG("target_module      = %s", g_target_module.c_str());
        DBG("log_level          = %s", g_log_level.c_str());
        DBG("zoom.scale         = %.3f", g_zoom_scale);
        DBG("zoom.step          = %.3f", g_zoom_step);
        DBG("zoom.min           = %.3f", g_zoom_min);
        DBG("zoom.max           = %.3f", g_zoom_max);
        DBG("fov.step           = %.3f", g_fov_step);
        DBG("fov.min            = %.3f", g_fov_min);
        DBG("fov.max            = %.3f", g_fov_max);
        DBG("grid.enabled       = %d", (int)g_grid_enabled);
        DBG("grid.cell          = %.3f", g_grid_cell);
        DBG("grid.range         = %d", g_grid_range);
        DBG("grid.offset        = %.3f", g_grid_offset);
        DBG("grid.width         = %d", g_grid_width);
        DBG("grid.color         = %d,%d,%d", g_grid_color_r, g_grid_color_g, g_grid_color_b);
    }

    bool debug_mode() { return g_debug_mode; }
    int poll_hz() { return g_poll_hz; }
    int rescan_interval_ms() { return g_rescan_interval_ms; }
    std::string target_module() { return g_target_module; }
    std::string log_level() { return g_log_level; }
    float zoom_scale() { return g_zoom_scale; }
    float zoom_step() { return g_zoom_step; }
    float zoom_min() { return g_zoom_min; }
    float zoom_max() { return g_zoom_max; }
    float fov_step() { return g_fov_step; }
    float fov_min() { return g_fov_min; }
    float fov_max() { return g_fov_max; }
    bool grid_enabled() { return g_grid_enabled; }
    float grid_cell() { return g_grid_cell; }
    int grid_range() { return g_grid_range; }
    float grid_offset() { return g_grid_offset; }
    int grid_width() { return g_grid_width; }
    int grid_color_r() { return g_grid_color_r; }
    int grid_color_g() { return g_grid_color_g; }
    int grid_color_b() { return g_grid_color_b; }

}  // namespace config
