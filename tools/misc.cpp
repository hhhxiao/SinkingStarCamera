#include "misc.h"

#include <Windows.h>

#include "logger.h"

namespace misc {

    std::filesystem::path thisDllDirectory() {
        char path[MAX_PATH]{};
        HMODULE hm = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&thisDllDirectory), &hm)) {
            ERR("GetModuleHandleEx failed, error = %lu", GetLastError());
            return {};
        }
        if (!GetModuleFileNameA(hm, path, sizeof(path))) {
            ERR("GetModuleFileName failed, error = %lu", GetLastError());
            return {};
        }
        return std::filesystem::path(path).parent_path();
    }

}  // namespace misc
