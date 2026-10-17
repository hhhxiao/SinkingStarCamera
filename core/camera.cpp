#include "camera.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "config.h"
#include "logger.h"
#include "mem.h"
#include "scanner.h"
#include "overlay_grid.h"
#include "overlay_panel.h"

namespace camera {

    namespace {

        constexpr size_t kEntityTypeInfoOff = 0x108;
        constexpr size_t kUpDistanceOff = 0x360;
        constexpr size_t kFovOff = 0x340;
        constexpr size_t kMaxHits = 4096;

        struct Entity {
            uintptr_t addr = 0;
            float orig_up = 0.0f;
            float orig_south = 0.0f;
            float orig_look_at_south = 0.0f;
            float orig_fov = 35.0f;
        };

        std::mutex g_mutex;
        std::vector<Entity> g_entities;  // owned by the tick thread
        std::vector<Entity> g_pending;   // handed over by the scan thread
        std::atomic<bool> g_pending_ready{false};
        std::atomic<bool> g_scan_running{false};

        float g_scale = 1.0f;
        float g_applied = 1.0f;
        float g_fov_scale = 1.0f;
        float g_fov_applied = 1.0f;
        bool g_top_down = false;
        bool g_force_rescan = false;
        std::chrono::steady_clock::time_point g_last_scan{};

        bool sane(float fov, float zn, float zf) {
            if (!(fov > 0.01f && fov < 179.9f)) return false;
            if (!(zn > 0.0f && zn < 1.0e5f)) return false;
            if (!(zf > zn && zf < 1.0e7f)) return false;
            return true;
        }

        // Locate the Camera_Control Type_Info at runtime instead of hardcoding an
        // rva: game updates move it (the 2026-10-16 build already did). The type
        // name string lives in the image, and a qword pointing at it minus 0x18 is
        // the Type_Info (its name field sits at +0x18 in both layouts). The size
        // field (0x430) confirms the match.
        uintptr_t find_camera_type_info() {
            // Called every tick for the staleness check, so the result is cached:
            // scanning a 12 MB image at 60 Hz would cost real frame time.
            static std::atomic<uintptr_t> cached{0};
            if (const uintptr_t hit = cached.load()) return hit;
            HMODULE host = GetModuleHandleW(nullptr);
            if (!host) return 0;
            const auto *base = reinterpret_cast<const uint8_t *>(host);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
            const uintptr_t begin = reinterpret_cast<uintptr_t>(host);
            const uintptr_t end = begin + nt->OptionalHeader.SizeOfImage;
            constexpr char kName[] = "Camera_Control";
            constexpr size_t kNameLen = sizeof(kName);  // includes the terminator

            for (uintptr_t p = begin; p + kNameLen <= end; ++p) {
                if (std::memcmp(reinterpret_cast<const void *>(p), kName, kNameLen) != 0) continue;
                for (uintptr_t q = begin; q + 8 <= end; q += 8) {
                    if (*reinterpret_cast<const uintptr_t *>(q) != p) continue;
                    const uintptr_t ti = q - 0x18;
                    if (ti < begin) continue;
                    if (mem::read<uint64_t>(ti + 0x08) == sizeof(CameraControl) ||
                        mem::read<uint64_t>(ti + 0x28) == sizeof(CameraControl)) {
                        LOG("Camera_Control Type_Info at exe+0x%llx", (unsigned long long)(ti - begin));
                        cached = ti;
                        return ti;
                    }
                }
            }
            return 0;
        }

        Entity *find(std::vector<Entity> &list, uintptr_t addr) {
            for (auto &e : list)
                if (e.addr == addr) return &e;
            return nullptr;
        }

        void run_scan() {
            const uint64_t needle = static_cast<uint64_t>(find_camera_type_info());
            std::vector<Entity> found;
            std::vector<Entity> previous;
            {
                // Keep the distance values captured on first sight: re-deriving them
                // now would capture our own scaled values and the zoom would march
                // away with every rescan.
                std::lock_guard<std::mutex> lock(g_mutex);
                previous = g_entities;
            }
            if (!needle) {
                ERR("host module base unresolved, cannot scan");
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                auto hits = scanner::find_u64(needle, kMaxHits);
                if (hits.empty()) hits = scanner::find_u64(needle, kMaxHits, false);
                const auto stats = scanner::last_stats();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                LOG("scan -> %zu hits in %lld ms | %zu MiB touched, %zu regions", hits.size(), (long long)ms,
                    stats.resident_bytes / 1048576, stats.regions);

                for (uintptr_t hit : hits) {
                    if (hit < kEntityTypeInfoOff) continue;
                    const uintptr_t entity = hit - kEntityTypeInfoOff;
                    if (find(found, entity)) continue;
                    if (!mem::is_readable(entity, sizeof(CameraControl))) continue;
                    if (mem::read<void *>(entity + kEntityTypeInfoOff) != reinterpret_cast<void *>(needle)) continue;
                    const auto *cc = reinterpret_cast<const CameraControl *>(entity);
                    if (!sane(cc->fov_vertical, cc->z_near, cc->z_far)) continue;
                    if (!mem::is_writable(entity + kUpDistanceOff, 12)) continue;

                    Entity e{};
                    e.addr = entity;
                    if (const Entity *prev = find(previous, entity)) {
                        e.orig_up = prev->orig_up;
                        e.orig_south = prev->orig_south;
                        e.orig_look_at_south = prev->orig_look_at_south;
                        e.orig_fov = prev->orig_fov;
                    } else {
                        e.orig_up = cc->up_distance;
                        e.orig_south = cc->south_distance;
                        e.orig_look_at_south = cc->look_at_south_distance;
                        e.orig_fov = cc->fov_vertical;
                    }
                    found.push_back(e);
                }
                LOG("accepted %zu Camera_Control entities", found.size());
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pending.swap(found);
                g_pending_ready = true;
            }
            g_scan_running = false;
        }

        void start_scan() {
            if (g_scan_running.exchange(true)) return;
            std::thread(run_scan).detach();
        }

        void apply(const Entity &e, float scale) {
            mem::write<float>(e.addr + kUpDistanceOff + 0, e.orig_up * scale);
            // south_distance / look_at_south_distance are the horizontal offset that
            // creates the tilt. In top-down mode they shrink to 0.5% instead of
            // exactly zero: at exactly zero the camera's up vector is undefined and
            // the engine picks a different screen orientation, which made WASD
            // disagree with the screen (A/D became up/down). A 0.3 degree residual
            // tilt keeps the frame north-up and is invisible.
            const float tilt = g_top_down ? scale * 0.005f : scale;
            mem::write<float>(e.addr + kUpDistanceOff + 4, e.orig_south * tilt);
            mem::write<float>(e.addr + kUpDistanceOff + 8, e.orig_look_at_south * tilt);
        }

        void apply_fov(const Entity &e, float scale) {
            float fov = e.orig_fov * scale;
            if (fov < 1.0f) fov = 1.0f;
            if (fov > 170.0f) fov = 170.0f;
            mem::write<float>(e.addr + kFovOff, fov);
        }

        void restore(const Entity &e) {
            mem::write<float>(e.addr + kUpDistanceOff + 0, e.orig_up);
            mem::write<float>(e.addr + kUpDistanceOff + 4, e.orig_south);
            mem::write<float>(e.addr + kUpDistanceOff + 8, e.orig_look_at_south);
            mem::write<float>(e.addr + kFovOff, e.orig_fov);
        }

        void handle_hotkeys() {
            // A level change frees the entity storage, so a cached pointer can go
            // stale; writing through one would corrupt whatever reused the memory.
            if (!g_entities.empty()) {
                const uint64_t needle = static_cast<uint64_t>(find_camera_type_info());
                bool ok = true;
                for (const auto &e : g_entities) {
                    if (!mem::is_readable(e.addr, sizeof(CameraControl)) ||
                        mem::read<void *>(e.addr + kEntityTypeInfoOff) != reinterpret_cast<void *>(needle)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    LOG("cached entities went stale, rescanning");
                    g_entities.clear();
                    g_last_scan = std::chrono::steady_clock::time_point{};
                    g_force_rescan = true;
                }
            }

            static bool prev[256]{};
            bool cur[256]{};
            for (int vk = 0x01; vk < 0x100; ++vk) cur[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
            auto pressed = [&](int vk) { return cur[vk] && !prev[vk]; };

            if (pressed(VK_F9)) zoom(config::zoom_step());           // further away
            if (pressed(VK_F10)) zoom(1.0f / config::zoom_step());   // closer in
            if (pressed(VK_F7)) nudge_fov(config::fov_step());        // wider field of view
            if (pressed(VK_F8)) nudge_fov(1.0f / config::fov_step()); // narrower field of view
            if (pressed(VK_F6)) {
                g_top_down = !g_top_down;
                LOG("top-down view -> %s", g_top_down ? "ON (camera straight above, no tilt)" : "OFF");
            }
            if (pressed(VK_F4)) reset();
            if (pressed(VK_F5)) overlay::toggle();
            if (pressed(VK_F2)) panel::toggle();

            for (int vk = 0x01; vk < 0x100; ++vk) prev[vk] = cur[vk];
        }

    }  // namespace

    void init() {
        g_scale = config::zoom_scale();
        g_applied = g_scale;
        LOG("camera ready: scale %.3f | F9 zoom out, F10 zoom in | F7 wider fov, F8 narrower fov | "
            "F6 top-down | F4 reset",
            g_scale);
    }

    void tick() {
        handle_hotkeys();

        if (g_pending_ready.exchange(false)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_entities = std::move(g_pending);
            g_pending.clear();
            LOG("tracking %zu camera entities", g_entities.size());
        }

        const auto now = std::chrono::steady_clock::now();
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_scan).count();
        if ((g_entities.empty() || g_force_rescan) && !g_scan_running && since >= config::rescan_interval_ms()) {
            g_last_scan = now;
            g_force_rescan = false;
            start_scan();
        }

        if (g_entities.empty()) return;
        if (fabsf(g_scale - g_applied) > 1.0e-4f) {
            LOG("zoom -> %.3f", g_scale);
            g_applied = g_scale;
        }
        if (fabsf(g_fov_scale - g_fov_applied) > 1.0e-4f) {
            LOG("fov -> %.3f", g_fov_scale);
            g_fov_applied = g_fov_scale;
        }
        if (fabsf(g_scale - 1.0f) <= 1.0e-4f && !g_top_down) {
            for (const auto &e : g_entities) restore(e);
        } else {
            for (const auto &e : g_entities) apply(e, g_scale);
        }
        if (fabsf(g_fov_scale - 1.0f) > 1.0e-4f) {
            for (const auto &e : g_entities) apply_fov(e, g_fov_scale);
        }
    }

    void shutdown() {
        for (const auto &e : g_entities) restore(e);
        LOG("camera restored");
    }

    void zoom(float factor) {
        const float next = g_scale * factor;
        g_scale = next < config::zoom_min() ? config::zoom_min()
                                            : (next > config::zoom_max() ? config::zoom_max() : next);
    }

    void reset() {
        g_scale = 1.0f;
        g_fov_scale = 1.0f;
        g_top_down = false;
        LOG("zoom reset");
    }

    void nudge_fov(float factor) {
        const float next = g_fov_scale * factor;
        g_fov_scale = next < config::fov_min() ? config::fov_min()
                                               : (next > config::fov_max() ? config::fov_max() : next);
    }

    float fov_scale() { return g_fov_scale; }

    void set_scale(float scale) {
        if (!(scale > 0.0f)) return;
        g_scale = scale < config::zoom_min() ? config::zoom_min()
                                             : (scale > config::zoom_max() ? config::zoom_max() : scale);
    }

    void set_fov_scale(float scale) {
        if (!(scale > 0.0f)) return;
        g_fov_scale = scale < config::fov_min() ? config::fov_min()
                                                : (scale > config::fov_max() ? config::fov_max() : scale);
    }

    bool top_down() { return g_top_down; }
    void set_top_down(bool on) { g_top_down = on; }

    float scale() { return g_scale; }

    size_t entity_count() { return g_entities.size(); }

}  // namespace camera
