#include "logger.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <share.h>
#include <string>

#define KNRM "\x1B[0m"
#define KRED "\x1B[31m"
#define KGRN "\x1B[32m"
#define KYEL "\x1B[33m"
#define KCYN "\x1B[36m"
#define KWHT "\x1B[37m"

namespace {

    std::mutex g_mutex;
    FILE *g_file = nullptr;
    bool g_tried_open = false;
    int g_min_level = LOG_LEVEL_DEBUG;

    const char *level_color(int level) {
        switch (level) {
            case LOG_LEVEL_ERROR: return KRED;
            case LOG_LEVEL_INFO: return KGRN;
            case LOG_LEVEL_DEBUG: return KCYN;
            default: return KWHT;
        }
    }

    const char *level_name(int level) {
        switch (level) {
            case LOG_LEVEL_ERROR: return "ERR";
            case LOG_LEVEL_INFO: return "INF";
            case LOG_LEVEL_DEBUG: return "DBG";
            default: return "TRC";
        }
    }

    std::string timestamp() {
        auto t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[32]{};
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

    // Resolve <directory of this dll>\orthocam.log without depending on misc.h
    // so the logger stays usable from anywhere.
    bool open_log_file() {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&open_log_file), &self)) {
            return false;
        }
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;
        std::wstring w(path);
        auto slash = w.find_last_of(L"\\/");
        if (slash != std::wstring::npos) w.resize(slash + 1);
        w += L"sinkingstar_camera.log";
        // _SH_DENYWR: allow other processes to read the log while the game runs,
        // which plain fopen does not.
        g_file = _wfsopen(w.c_str(), L"w", _SH_DENYWR);
        return g_file != nullptr;
    }

}  // namespace

void init_logger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tried_open) return;
    g_tried_open = true;
    open_log_file();
}

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_min_level = level;
}

void log_msg(int level, const char *function_name, const char *fmt, ...) {
    if (level < g_min_level) return;

    char body[2048]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body) - 1, fmt, args);
    va_end(args);

    std::string line = timestamp() + " [" + level_name(level) + "] [" + function_name + "] " + body;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fputs(line.c_str(), g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    fprintf(stdout, "%s%s" KCYN " [%s] " KWHT "%s\n", level_color(level), timestamp().c_str(),
            function_name, body);
    fflush(stdout);
    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");
}
