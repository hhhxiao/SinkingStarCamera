#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <thread>

#include "camera.h"
#include "config.h"
#include "logger.h"

namespace {

    volatile bool g_running = true;

    DWORD WINAPI start(LPVOID) {
        // Let the engine finish its own start-up before touching its memory.
        std::this_thread::sleep_for(std::chrono::seconds(3));

        init_logger();
        config::init();
        config::print_all_info();

        camera::init();

        int hz = config::poll_hz();
        const auto period = std::chrono::milliseconds(1000 / hz);
        while (g_running) {
            camera::tick();
            std::this_thread::sleep_for(period);
        }
        camera::shutdown();
        return 0;
    }

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
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
