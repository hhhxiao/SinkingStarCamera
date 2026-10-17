#ifndef ORTHOCAM_MEM_H
#define ORTHOCAM_MEM_H

#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "logger.h"

namespace mem {

    // ---- pattern scanning -------------------------------------------------
    // Signature syntax: "48 8B 05 ?? ?? ?? ?? 48 85 C0", a single ? is also
    // accepted as a wildcard byte. Tokens are whitespace separated.
    inline uintptr_t FindSig(const char *moduleName, const char *signature) {
        HMODULE moduleHandle = GetModuleHandleA(moduleName);
        if (!moduleHandle) {
            ERR("FindSig: module %s is not loaded", moduleName ? moduleName : "(host exe)");
            return 0;
        }
        MODULEINFO moduleInfo{};
        if (!GetModuleInformation(GetCurrentProcess(), moduleHandle, &moduleInfo, sizeof(moduleInfo))) {
            ERR("FindSig: GetModuleInformation failed (%lu)", GetLastError());
            return 0;
        }

        struct Token {
            uint8_t value;
            bool wildcard;
        };
        std::vector<Token> pattern;
        const char *p = signature;
        while (*p) {
            while (*p == ' ') ++p;
            if (!*p) break;
            if (*p == '?') {
                pattern.push_back({0, true});
                while (*p == '?') ++p;  // "??" and "?" both mean one wildcard byte
                continue;
            }
            char buf[3]{p[0], p[1], 0};
            if (!isxdigit((unsigned char)buf[0]) || !isxdigit((unsigned char)buf[1])) {
                ERR("FindSig: bad token '%s' in signature", buf);
                return 0;
            }
            pattern.push_back({(uint8_t)strtoul(buf, nullptr, 16), false});
            p += 2;
        }
        if (pattern.empty()) return (uintptr_t)moduleHandle;

        const uint8_t *begin = (const uint8_t *)moduleHandle;
        const uint8_t *end = begin + moduleInfo.SizeOfImage;
        for (const uint8_t *cur = begin; cur + pattern.size() <= end; ++cur) {
            size_t i = 0;
            for (; i < pattern.size(); ++i) {
                if (!pattern[i].wildcard && cur[i] != pattern[i].value) break;
            }
            if (i == pattern.size()) return (uintptr_t)cur;
        }
        return 0;
    }

    inline uintptr_t FindSig(const char *signature) { return FindSig(nullptr, signature); }

    // Resolve a rip-relative displacement found inside the pattern:
    //   lea rax, [rip + disp32]   ->  address of the instruction + offset + 4 + additional
    inline uintptr_t FingSigWithOffset(const char *signature, uint64_t offset, uint64_t additional) {
        auto addr = FindSig(signature);
        if (addr == 0) {
            ERR("Can't found Signature: [%s]", signature);
            return 0;
        }
        return addr + *reinterpret_cast<int32_t *>(addr + offset) + additional;
    }

    // ---- address helpers --------------------------------------------------
    // rva2va/va2rva are relative to the host executable, which is what we want
    // for offsets taken out of IDA against sinking_star.exe.
    inline uintptr_t va2rva(uintptr_t va) { return va - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)); }
    inline uintptr_t rva2va(uintptr_t rva) { return rva + reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)); }

    inline bool is_committed(uintptr_t addr, size_t size, bool need_write) {
        if (!addr) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (addr + size > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) return false;
        if (need_write) {
            const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & writable)) return false;
        }
        return true;
    }

    inline bool is_readable(uintptr_t addr, size_t size) { return is_committed(addr, size, false); }
    inline bool is_writable(uintptr_t addr, size_t size) { return is_committed(addr, size, true); }

    template <typename T>
    inline T read(uintptr_t addr) {
        T out{};
        std::memcpy(&out, (const void *)addr, sizeof(T));
        return out;
    }

    template <typename T>
    inline void write(uintptr_t addr, T value) {
        std::memcpy((void *)addr, &value, sizeof(T));
    }

    // ---- debug dumps ------------------------------------------------------
    inline std::string hex_dump(uintptr_t base, size_t bytes) {
        std::stringstream ss;
        ss << "\n";
        for (size_t i = 0; i < bytes; i++) {
            if (i % 16 == 0) {
                ss << "0x" << std::setfill('0') << std::setw(sizeof(uintptr_t) * 2) << std::hex << (base + i) << " ";
            }
            ss << std::setfill('0') << std::setw(2) << (int)*reinterpret_cast<uint8_t *>(base + i) << " ";
            if (i % 16 == 15) ss << "\n";
        }
        return ss.str();
    }

    inline std::string print_num(uintptr_t base, size_t bytes) {
        std::stringstream ss;
        ss << "\n";
        for (size_t i = 0; i + 4 <= bytes; i += 4) {
            ss << "0x" << std::setfill('0') << std::setw(sizeof(uintptr_t) * 2) << std::hex << (base + i) << " +"
               << std::setfill('0') << std::setw(3) << std::hex << i << "  dec " << std::setfill(' ') << std::setw(14)
               << std::dec << *reinterpret_cast<int32_t *>(base + i) << "   flt "
               << *reinterpret_cast<float *>(base + i) << "\n";
        }
        return ss.str();
    }

}  // namespace mem

#endif
