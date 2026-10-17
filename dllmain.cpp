#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <thread>

#include "camera.h"
#include "config.h"
#include "d3d12_overlay.h"
#include "logger.h"
#include "overlay_grid.h"
#include "overlay_panel.h"

namespace {

    volatile bool g_running = true;

    DWORD WINAPI start(LPVOID) {
        // Let the engine finish its own start-up before touching its memory.
        std::this_thread::sleep_for(std::chrono::seconds(3));

        init_logger();
        config::init();
        config::print_all_info();

        if (config::d3d12_enabled()) overlay::set_external_renderer(true);
        camera::init();
        overlay::init();
        panel::init();
        d3d12_overlay::init();

        int hz = config::poll_hz();
        const auto period = std::chrono::milliseconds(1000 / hz);
        while (g_running) {
            camera::tick();
            std::this_thread::sleep_for(period);
        }
        camera::shutdown();
        d3d12_overlay::shutdown();
        return 0;
    }

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            d3d12_overlay::early_init();
            CreateThread(nullptr, 0, start, hModule, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            g_running = false;
            break;
        default:
            break;
    }
    return TRUE;
}
