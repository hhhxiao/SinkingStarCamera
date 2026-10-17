// SinkingStarCameraInjector - waits for the game and injects sinkingstar_camera.dll.
//
// Why not a proxy dll: sinking_star.exe imports exactly one symbol from WINMM,
// which makes winmm.dll look like a perfect hijack target. It is not. Steam's
// steamclient64.dll also imports winmm.dll, and once a module named winmm.dll is
// loaded into the process every later importer binds to it. A partial proxy
// therefore makes steamclient64.dll fail to load and the game dies with
// "Steam initialization failed". Injecting keeps the game directory untouched.

#include <Windows.h>
#include <TlHelp32.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

    void print(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stdout, fmt, args);
        va_end(args);
        fputc('\n', stdout);
        fflush(stdout);
    }

    DWORD find_process(const std::wstring &exe_name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exe_name.c_str()) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    bool inject(DWORD pid, const std::wstring &dll_path) {
        HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                      PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, pid);
        if (!proc) {
            print("  OpenProcess failed, error %lu", GetLastError());
            return false;
        }

        const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
        void *remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) {
            print("  VirtualAllocEx failed, error %lu", GetLastError());
            CloseHandle(proc);
            return false;
        }

        bool ok = false;
        if (!WriteProcessMemory(proc, remote, dll_path.c_str(), bytes, nullptr)) {
            print("  WriteProcessMemory failed, error %lu", GetLastError());
        } else {
            auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
            HANDLE thread = CreateRemoteThread(proc, nullptr, 0, load_library, remote, 0, nullptr);
            if (!thread) {
                print("  CreateRemoteThread failed, error %lu", GetLastError());
            } else {
                WaitForSingleObject(thread, 15000);
                DWORD module_base = 0;
                GetExitCodeThread(thread, &module_base);
                CloseHandle(thread);
                ok = module_base != 0;
                print("  LoadLibraryW returned 0x%08lX %s", module_base, ok ? "(ok)" : "(failed)");
            }
        }
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return ok;
    }

}  // namespace

int main(int argc, char **argv) {
    std::wstring exe_name = L"sinking_star.exe";
    std::wstring dll_path;
    DWORD steam_appid = 499170;
    int timeout_s = 120;
    bool launch = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--exe") {
            std::string v = next();
            exe_name.assign(v.begin(), v.end());
        } else if (a == "--dll") {
            std::string v = next();
            dll_path.assign(v.begin(), v.end());
        } else if (a == "--steam") {
            steam_appid = (DWORD)strtoul(next().c_str(), nullptr, 10);
        } else if (a == "--timeout") {
            timeout_s = atoi(next().c_str());
        } else if (a == "--no-launch") {
            launch = false;
        } else if (a == "--help" || a == "-h") {
            print("usage: OrthoCamInjector [--exe name] [--dll path] [--steam appid] [--timeout sec] [--no-launch]");
            return 0;
        }
    }

    // Default the dll to the one sitting next to this exe.
    if (dll_path.empty()) {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir(self);
        const auto slash = dir.find_last_of(L"\\/");
        dir.resize(slash == std::wstring::npos ? 0 : slash + 1);
        dll_path = dir + L"sinkingstar_camera.dll";
    }

    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        print("dll not found: %ls", dll_path.c_str());
        return 2;
    }
    print("dll        : %ls", dll_path.c_str());
    print("target     : %ls", exe_name.c_str());

    DWORD pid = find_process(exe_name);
    if (!pid && launch) {
        wchar_t url[64]{};
        swprintf_s(url, L"steam://rungameid/%lu", steam_appid);
        print("target not running, opening %ls", url);
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Poll tightly: the mod hooks the game's D3D12CreateDevice import from
    // DllMain, and the engine creates its device a couple of seconds in, so the
    // earlier we land the better.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (!pid && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pid = find_process(exe_name);
    }
    if (!pid) {
        print("gave up waiting for %ls", exe_name.c_str());
        return 3;
    }
    print("found %ls pid=%lu", exe_name.c_str(), pid);

    // The game's own imports are resolved before its entry point runs, which is
    // before we can even see the process, so there is nothing to wait for: the
    // earlier the mod lands, the more likely it is to be in place before the
    // engine creates its D3D12 device.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    print("injecting...");
    if (!inject(pid, dll_path)) {
        print("injection failed");
        return 4;
    }
    print("injected, check orthocam.log next to the dll");
    return 0;
}
