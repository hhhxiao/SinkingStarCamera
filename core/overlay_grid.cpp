#include "overlay_grid.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "config.h"
#include "logger.h"
#include "mem.h"
#include "scanner.h"

namespace overlay {

    namespace {

        // ---- the engine's projection matrix ---------------------------------
        // Measured from the running game (see the repo history): row major, view
        // space is X forward / Y right / Z up, depth rides in w, reversed z.
        //
        //   out.x = -hz * y_view   out.y = vz * z_view
        //   out.z =  k * x_view + w   out.w = x_view
        //
        // It sits at Viewpoint_Params + 0x300; the view matrix at +0x280 and the
        // camera origin at +0x340.
        struct EngineProj {
            bool valid = false;
            float hz = 0, vz = 0, k = 0, w = 0;
        };

        EngineProj parse_proj(const float *m) {
            EngineProj p{};
            const float zeros[9] = {m[0], m[2], m[3], m[4], m[5], m[7], m[9], m[10], m[13]};
            for (float z : zeros)
                if (fabsf(z) > 1.0e-6f) return p;
            if (fabsf(m[14]) > 1.0e-6f) return p;
            if (fabsf(m[12] - 1.0f) > 1.0e-4f) return p;
            if (fabsf(m[15]) > 1.0e-6f) return p;
            p.hz = -m[1];
            p.vz = m[6];
            p.k = m[8];
            p.w = m[11];
            if (!(p.hz > 1.0e-4f && p.hz < 1.0e5f)) return p;
            if (!(p.vz > 1.0e-4f && p.vz < 1.0e5f)) return p;
            if (!(p.w > 0.0f) || fabsf(p.k) < 1.0e-12f) return p;
            p.valid = true;
            return p;
        }

        struct CameraState {
            uintptr_t base = 0;
            float view[16]{};
            float proj[16]{};
            float origin[3]{};
            bool row_vector_view = true;  // decided once by self test
            bool decided = false;
            bool valid = false;
        };

        CameraState g_camera;
        std::mutex g_camera_lock;
        std::atomic<bool> g_located{false};

        bool copy_guard(const void *src, void *dst, size_t bytes) {
            __try {
                std::memcpy(dst, src, bytes);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool valid_view_matrix(const float *v, const float origin[3]) {
            // A camera view matrix is a rigid transform: orthonormal 3x3 rotation
            // and a 0,0,0,1 bottom row. build17 has other matrices with the same
            // sparse projection shape earlier in memory, so shape alone is not
            // enough to identify the live viewpoint.
            if (fabsf(v[12]) > 1.0e-4f || fabsf(v[13]) > 1.0e-4f || fabsf(v[14]) > 1.0e-4f ||
                fabsf(v[15] - 1.0f) > 1.0e-4f)
                return false;

            const float *r0 = v + 0;
            const float *r1 = v + 4;
            const float *r2 = v + 8;
            const float l0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
            const float l1 = r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2];
            const float l2 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
            if (fabsf(l0 - 1.0f) > 0.02f || fabsf(l1 - 1.0f) > 0.02f || fabsf(l2 - 1.0f) > 0.02f)
                return false;
            const float d01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
            const float d02 = r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2];
            const float d12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
            if (fabsf(d01) > 0.02f || fabsf(d02) > 0.02f || fabsf(d12) > 0.02f) return false;

            // The world-space camera origin must map to the view-space origin.
            const float ox = v[0] * origin[0] + v[1] * origin[1] + v[2] * origin[2] + v[3];
            const float oy = v[4] * origin[0] + v[5] * origin[1] + v[6] * origin[2] + v[7];
            const float oz = v[8] * origin[0] + v[9] * origin[1] + v[10] * origin[2] + v[11];
            return fabsf(ox) < 0.1f && fabsf(oy) < 0.1f && fabsf(oz) < 0.1f;
        }

        // The projection matrix is the most distinctive thing in the process: a
        // 1.0f at m[12] followed by 0.0f, with the sparse shape above. Scanning for
        // that pair and verifying the shape finds it without any game address.
        bool locate_camera(CameraState &out) {
            const uint64_t one_zero = 0x000000003F800000ull;  // 1.0f, 0.0f
            auto hits = scanner::find_u64(one_zero, 2000000, true);
            for (uintptr_t hit : hits) {
                if (hit < 0x30) continue;
                const uintptr_t matrix = hit - 0x30;
                float m[16];
                if (!copy_guard(reinterpret_cast<const void *>(matrix), m, sizeof(m))) continue;
                if (!parse_proj(m).valid) continue;
                float view[16];
                float origin[3];
                if (!copy_guard(reinterpret_cast<const void *>(matrix - 0x80), view, sizeof(view))) continue;
                if (!copy_guard(reinterpret_cast<const void *>(matrix + 0x40), origin, sizeof(origin))) continue;
                const float olen = sqrtf(origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2]);
                if (!(olen > 0.01f && olen < 100000.0f)) continue;
                if (!valid_view_matrix(view, origin)) continue;
                std::memcpy(out.proj, m, sizeof(m));
                std::memcpy(out.view, view, sizeof(view));
                std::memcpy(out.origin, origin, sizeof(origin));
                out.base = matrix - 0x300;
                out.valid = true;
                return true;
            }
            return false;
        }

        void world_to_view(const CameraState &cam, const float world[3], float v[4]) {
            // Both matrices are stored row major but applied to column vectors
            // (v_view = V * v, clip = P * v).
            for (int r = 0; r < 4; ++r)
                v[r] = cam.view[r * 4 + 0] * world[0] + cam.view[r * 4 + 1] * world[1] +
                       cam.view[r * 4 + 2] * world[2] + cam.view[r * 4 + 3];
        }

        bool project_view(const CameraState &cam, const float v[4], float &sx, float &sy, int width, int height) {
            float clip[4];
            for (int r = 0; r < 4; ++r)
                clip[r] = cam.proj[r * 4 + 0] * v[0] + cam.proj[r * 4 + 1] * v[1] +
                          cam.proj[r * 4 + 2] * v[2] + cam.proj[r * 4 + 3] * v[3];
            if (clip[3] <= 1.0e-6f) return false;
            const float ndc_x = clip[0] / clip[3];
            const float ndc_y = clip[1] / clip[3];
            if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) return false;
            sx = (ndc_x * 0.5f + 0.5f) * width;
            sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * height;
            return true;
        }

        // World -> screen for a single point. Points behind the camera are
        // rejected; segments use project_segment() so the visible part survives.
        bool project(const CameraState &cam, const float world[3], float &sx, float &sy, int width, int height) {
            float v[4];
            world_to_view(cam, world, v);
            return project_view(cam, v, sx, sy, width, height);
        }

        struct ClipVertex {
            float x, y, z, w;
        };

        void world_to_clip(const CameraState &cam, const float world[3], ClipVertex &out) {
            float v[4];
            world_to_view(cam, world, v);
            out.x = cam.proj[0] * v[0] + cam.proj[1] * v[1] + cam.proj[2] * v[2] + cam.proj[3] * v[3];
            out.y = cam.proj[4] * v[0] + cam.proj[5] * v[1] + cam.proj[6] * v[2] + cam.proj[7] * v[3];
            out.z = cam.proj[8] * v[0] + cam.proj[9] * v[1] + cam.proj[10] * v[2] + cam.proj[11] * v[3];
            out.w = cam.proj[12] * v[0] + cam.proj[13] * v[1] + cam.proj[14] * v[2] + cam.proj[15] * v[3];
        }

        float clip_distance(const ClipVertex &p, int plane) {
            switch (plane) {
                case 0: return p.w - 0.05f;  // near
                case 1: return p.x + p.w;    // left
                case 2: return p.w - p.x;    // right
                case 3: return p.y + p.w;    // bottom
                default: return p.w - p.y;   // top
            }
        }

        // Clips a homogeneous segment against the near plane and the four side
        // planes of the viewport. The old code instead rejected a segment when
        // either endpoint was more than 50 NDC units outside the screen. A grid
        // line crossing the camera can have an endpoint thousands of units away
        // even though the visible part runs across the whole screen, so that
        // check discarded one entire grid direction at common camera angles.
        bool clip_segment(ClipVertex &a, ClipVertex &b) {
            for (int plane = 0; plane < 5; ++plane) {
                const float da = clip_distance(a, plane);
                const float db = clip_distance(b, plane);
                if (da < 0.0f && db < 0.0f) return false;
                if (da < 0.0f) {
                    const float t = da / (da - db);
                    a.x += (b.x - a.x) * t;
                    a.y += (b.y - a.y) * t;
                    a.z += (b.z - a.z) * t;
                    a.w += (b.w - a.w) * t;
                } else if (db < 0.0f) {
                    const float t = db / (db - da);
                    b.x += (a.x - b.x) * t;
                    b.y += (a.y - b.y) * t;
                    b.z += (a.z - b.z) * t;
                    b.w += (a.w - b.w) * t;
                }
            }
            return true;
        }

        // Projects a world-space segment after clipping it to the viewport.
        // Clipping keeps the visible part of long lines instead of dropping the
        // whole segment when an endpoint is off-screen or behind the camera.
        bool project_segment(const CameraState &cam, const float a[3], const float b[3], float &x0, float &y0,
                             float &x1, float &y1, int width, int height) {
            ClipVertex ca{}, cb{};
            world_to_clip(cam, a, ca);
            world_to_clip(cam, b, cb);
            if (!clip_segment(ca, cb)) return false;
            if (!(ca.w > 1.0e-6f) || !(cb.w > 1.0e-6f)) return false;
            const float nax = ca.x / ca.w;
            const float nay = ca.y / ca.w;
            const float nbx = cb.x / cb.w;
            const float nby = cb.y / cb.w;
            if (!std::isfinite(nax) || !std::isfinite(nay) || !std::isfinite(nbx) || !std::isfinite(nby))
                return false;
            x0 = (nax * 0.5f + 0.5f) * width;
            y0 = (1.0f - (nay * 0.5f + 0.5f)) * height;
            x1 = (nbx * 0.5f + 0.5f) * width;
            y1 = (1.0f - (nby * 0.5f + 0.5f)) * height;
            return true;
        }

        // Sanity check used once: a point ten units straight ahead must project to
        // the middle of the screen.
        bool verify_projection(const CameraState &cam, int width, int height) {
            const float ahead[3] = {cam.origin[0] + cam.view[0] * 10.0f, cam.origin[1] + cam.view[1] * 10.0f,
                                    cam.origin[2] + cam.view[2] * 10.0f};
            float sx = 0, sy = 0;
            if (!project(cam, ahead, sx, sy, width, height)) {
                LOG("overlay: point ahead does not project");
                return false;
            }
            LOG("overlay: point ahead projects to %.0f,%.0f (screen %dx%d)", sx, sy, width, height);
            return fabsf(sx - width * 0.5f) < width * 0.05f && fabsf(sy - height * 0.5f) < height * 0.05f;
        }

        // ---- the overlay window ---------------------------------------------
        constexpr COLORREF kKeyColor = RGB(255, 0, 255);  // transparency key
        std::atomic<int> grid_color_r{0};
        std::atomic<int> grid_color_g{0};
        std::atomic<int> grid_color_b{0};
        std::atomic<int> grid_width_value{2};

        COLORREF line_color() { return RGB(grid_color_r.load(), grid_color_g.load(), grid_color_b.load()); }

        HWND g_game_window = nullptr;
        HWND g_overlay = nullptr;
        std::atomic<bool> g_enabled{false};
        std::atomic<float> grid_cell_value{1.0f};
        std::atomic<float> grid_offset_value{0.5f};
        std::atomic<int> grid_range_value{20};
        std::atomic<bool> g_running{false};
        std::atomic<bool> g_external_renderer{false};

        struct WindowSearch {
            DWORD pid = 0;
            HWND found = nullptr;
            long long best_area = 0;
        };

        BOOL CALLBACK find_game_window(HWND hwnd, LPARAM lparam) {
            auto *search = reinterpret_cast<WindowSearch *>(lparam);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != search->pid) return TRUE;
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindow(hwnd, GW_OWNER)) return TRUE;
            // Never pick our own windows: the ImGui panel is smaller than the game
            // but would otherwise win a "first match" search and the grid would be
            // drawn over it instead of over the game.
            wchar_t cls[128]{};
            if (GetClassNameW(hwnd, cls, 128) &&
                (wcscmp(cls, L"SinkingStarCameraPanel") == 0 || wcscmp(cls, L"SinkingStarCameraOverlay") == 0)) {
                return TRUE;
            }
            RECT r{};
            if (!GetClientRect(hwnd, &r) || (r.right - r.left) < 320) return TRUE;
            const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
            if (area <= search->best_area) return TRUE;
            search->best_area = area;
            search->found = hwnd;
            return TRUE;
        }

        HWND find_game_window() {
            WindowSearch search{};
            search.pid = GetCurrentProcessId();
            EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
            return search.found;
        }

        LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            if (msg == WM_ERASEBKGND) return 1;
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        HWND create_overlay_window() {
            static bool registered = false;
            if (!registered) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = overlay_proc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"SinkingStarCameraOverlay";
                wc.hbrBackground = nullptr;
                RegisterClassW(&wc);
                registered = true;
            }
            HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                            WS_EX_NOACTIVATE,
                                        L"SinkingStarCameraOverlay", L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
            if (!hwnd) return nullptr;
            SetLayeredWindowAttributes(hwnd, kKeyColor, 0, LWA_COLORKEY);
            return hwnd;
        }

        void collect_grid(const CameraState &cam, float width, float height, std::vector<ScreenLine> &out) {
            out.clear();
            const float cell = grid_cell_value.load();
            const int range = grid_range_value.load();
            const float offset = grid_offset_value.load();  // in cells
            if (cell <= 0.001f || range <= 0 || range > 200) return;
            // Grid centred on the camera, snapped to the cell lattice. The ground
            // plane is XY with Z up.
            const float cx = (floorf(cam.origin[0] / cell) + offset) * cell;
            const float cy = (floorf(cam.origin[1] / cell) + offset) * cell;
            const float span = range * cell;
            for (int i = -range; i <= range; ++i) {
                const float x = cx + i * cell;
                float x0, y0, x1, y1;
                const float a[3] = {x, cy - span, 0.0f};
                const float b[3] = {x, cy + span, 0.0f};
                if (project_segment(cam, a, b, x0, y0, x1, y1, width, height)) {
                    out.push_back({x0, y0, x1, y1});
                }
                const float y = cy + i * cell;
                const float c[3] = {cx - span, y, 0.0f};
                const float d[3] = {cx + span, y, 0.0f};
                if (project_segment(cam, c, d, x0, y0, x1, y1, width, height)) {
                    out.push_back({x0, y0, x1, y1});
                }
            }
        }

        void draw_grid(HDC target, int width, int height, const CameraState &cam) {
            // Double buffered: painting straight into the window DC flickered.
            static HDC mem_dc = nullptr;
            static HBITMAP mem_bmp = nullptr;
            static int mem_w = 0, mem_h = 0;
            if (!mem_dc) mem_dc = CreateCompatibleDC(target);
            if (!mem_dc) return;
            if (mem_w != width || mem_h != height) {
                if (mem_bmp) DeleteObject(mem_bmp);
                mem_bmp = CreateCompatibleBitmap(target, width, height);
                if (!mem_bmp) return;
                SelectObject(mem_dc, mem_bmp);
                mem_w = width;
                mem_h = height;
            }
            HDC dc = mem_dc;
            HPEN pen = CreatePen(PS_SOLID, grid_width_value.load(), line_color());
            HGDIOBJ old_pen = SelectObject(dc, pen);
            HBRUSH key = CreateSolidBrush(kKeyColor);
            RECT full{0, 0, width, height};
            FillRect(dc, &full, key);
            DeleteObject(key);

            std::vector<ScreenLine> lines;
            collect_grid(cam, (float)width, (float)height, lines);
            if (lines.empty()) {
                SelectObject(dc, old_pen);
                DeleteObject(pen);
                return;
            }
            // Bounding box of everything we draw, so the window can be clipped to
            // it: the DWM compositing a full 4K layered surface is what cost the
            // frame rate, not the line count.
            int min_x = width, min_y = height, max_x = 0, max_y = 0;
            auto include = [&](float x, float y) {
                if (x < min_x) min_x = (int)x;
                if (y < min_y) min_y = (int)y;
                if (x > max_x) max_x = (int)x;
                if (y > max_y) max_y = (int)y;
            };
            for (const auto &line : lines) {
                MoveToEx(dc, (int)line.x0, (int)line.y0, nullptr);
                LineTo(dc, (int)line.x1, (int)line.y1);
                include(line.x0, line.y0);
                include(line.x1, line.y1);
            }
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 3)
                LOG("overlay: drew %zu grid lines (cell %.3f)", lines.size(), grid_cell_value.load());

            // A cross at the camera origin: the first thing to look at when
            // checking that the projection lines up with the world.
            float sx = 0, sy = 0;
            if (project(cam, cam.origin, sx, sy, width, height)) {
                MoveToEx(dc, (int)sx - 20, (int)sy, nullptr);
                LineTo(dc, (int)sx + 20, (int)sy);
                MoveToEx(dc, (int)sx, (int)sy - 20, nullptr);
                LineTo(dc, (int)sx, (int)sy + 20);
                include(sx - 20, sy - 20);
                include(sx + 20, sy + 20);
            }
            SelectObject(dc, old_pen);
            DeleteObject(pen);
            // Clip the window to the drawn area and only blit that part.
            if (max_x > min_x && max_y > min_y) {
                min_x = min_x < 0 ? 0 : min_x;
                min_y = min_y < 0 ? 0 : min_y;
                max_x = max_x > width ? width : max_x;
                max_y = max_y > height ? height : max_y;
                const int pad = 4;
                RECT box{min_x - pad, min_y - pad, max_x + pad, max_y + pad};
                if (box.left < 0) box.left = 0;
                if (box.top < 0) box.top = 0;
                if (box.right > width) box.right = width;
                if (box.bottom > height) box.bottom = height;
                HRGN region = CreateRectRgn(box.left, box.top, box.right, box.bottom);
                if (region) {
                    SetWindowRgn(g_overlay, region, TRUE);  // window owns the region
                }
                BitBlt(target, box.left, box.top, box.right - box.left, box.bottom - box.top, mem_dc, box.left,
                       box.top, SRCCOPY);
            }
        }

        void overlay_thread() {
            g_running = true;
            while (g_running) {
                if (g_external_renderer.load()) {
                    if (g_overlay) ShowWindow(g_overlay, SW_HIDE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                if (!g_enabled.load()) {
                    if (g_overlay) ShowWindow(g_overlay, SW_HIDE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                if (!g_overlay) {
                    g_overlay = create_overlay_window();
                    if (!g_overlay) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        continue;
                    }
                }
                if (!IsWindow(g_game_window)) g_game_window = find_game_window();
                // Only hide while the game is minimised. Tying this to foreground
                // focus hid the grid the moment the user clicked the settings
                // panel.
                if (!g_game_window || !IsWindowVisible(g_game_window) || IsIconic(g_game_window)) {
                    ShowWindow(g_overlay, SW_HIDE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

                RECT client{};
                POINT top_left{0, 0};
                if (!GetClientRect(g_game_window, &client) || !ClientToScreen(g_game_window, &top_left)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                const int width = client.right - client.left;
                const int height = client.bottom - client.top;
                if (width < 64 || height < 64) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                static RECT last{};
                if (last.left != top_left.x || last.top != top_left.y || last.right != width || last.bottom != height ||
                    !IsWindowVisible(g_overlay)) {
                    SetWindowPos(g_overlay, HWND_TOPMOST, top_left.x, top_left.y, width, height,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    last.left = top_left.x;
                    last.top = top_left.y;
                    last.right = width;
                    last.bottom = height;
                }

                CameraState cam;
                {
                    std::lock_guard<std::mutex> lock(g_camera_lock);
                    cam = g_camera;
                }
                if (cam.valid && !cam.decided) {
                    verify_projection(cam, width, height);
                    std::lock_guard<std::mutex> lock(g_camera_lock);
                    g_camera.decided = true;
                }
                if (cam.valid) {
                    // Only repaint when something actually changed: a static view
                    // repainting 60 times a second is what flickered.
                    static CameraState last_drawn;
                    static int last_w = 0, last_h = 0;
                    if (last_w == width && last_h == height &&
                        std::memcmp(last_drawn.view, cam.view, sizeof(cam.view)) == 0 &&
                        std::memcmp(last_drawn.proj, cam.proj, sizeof(cam.proj)) == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        continue;
                    }
                    HDC dc = GetDC(g_overlay);
                    if (dc) {
                        draw_grid(dc, width, height, cam);
                        ReleaseDC(g_overlay, dc);
                    }
                    last_drawn = cam;
                    last_w = width;
                    last_h = height;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

    }  // namespace

    void init() {
        grid_cell_value = config::grid_cell();
        grid_offset_value = config::grid_offset();
        grid_range_value = config::grid_range();
        grid_width_value = config::grid_width();
        set_color(config::grid_color_r(), config::grid_color_g(), config::grid_color_b());
        if (!config::grid_enabled()) {
            LOG("overlay: grid disabled by config (F5 toggles it at runtime)");
        } else {
            g_enabled = true;
            LOG("overlay: grid enabled by config");
        }
        std::thread(overlay_thread).detach();
        std::thread([] {
            for (;;) {
                // Nothing here may run while the grid is off: the initial full
                // memory scan costs seconds and made the game hitch, which in turn
                // made the camera hotkeys look broken.
                if (!g_enabled.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                CameraState known;
                {
                    std::lock_guard<std::mutex> lock(g_camera_lock);
                    known = g_camera;
                }
                // Once located, only re-read the same addresses: a full memory scan
                // takes seconds and must not run in a loop.
                bool refreshed = false;
                if (known.valid && mem::is_readable(known.base + 0x280, 0x100)) {
                    float view[16], proj[16], origin[3];
                    EngineProj p{};
                    if (copy_guard(reinterpret_cast<const void *>(known.base + 0x280), view, sizeof(view)) &&
                        copy_guard(reinterpret_cast<const void *>(known.base + 0x300), proj, sizeof(proj)) &&
                        copy_guard(reinterpret_cast<const void *>(known.base + 0x340), origin, sizeof(origin)) &&
                        (p = parse_proj(proj)).valid) {
                        std::lock_guard<std::mutex> lock(g_camera_lock);
                        std::memcpy(g_camera.view, view, sizeof(view));
                        std::memcpy(g_camera.proj, proj, sizeof(proj));
                        std::memcpy(g_camera.origin, origin, sizeof(origin));
                        refreshed = true;
                    }
                }
                if (!refreshed) {
                    // Only ever scan once. Re-scanning took seconds and stalled the
                    // whole process (frame drops, and the grid vanished while it
                    // ran). If the cached addresses stop reading, keep the last good
                    // matrices instead.
                    if (g_located.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue;
                    }
                    // A full scan costs seconds. Doing it on every loop iteration
                    // (16 ms) starved the whole game and made the camera hotkeys
                    // look broken, so retries are throttled hard.
                    static auto last_attempt = std::chrono::steady_clock::time_point{};
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_attempt).count() < 3000)
                        continue;
                    last_attempt = now;
                    CameraState cam;
                    if (locate_camera(cam)) {
                        LOG("overlay: view matrix rows:");
                        for (int r = 0; r < 4; ++r)
                            LOG("overlay:   %.4f %.4f %.4f %.4f", cam.view[r * 4], cam.view[r * 4 + 1],
                                cam.view[r * 4 + 2], cam.view[r * 4 + 3]);
                        LOG("overlay: camera origin %.3f %.3f %.3f", cam.origin[0], cam.origin[1], cam.origin[2]);
                        std::lock_guard<std::mutex> lock(g_camera_lock);
                        cam.row_vector_view = g_camera.row_vector_view;
                        cam.decided = g_camera.decided;
                        g_camera = cam;
                        g_located = true;
                        LOG("overlay: camera matrices located at base 0x%llx", (unsigned long long)cam.base);
                    } else if (!g_located.load()) {
                        static std::atomic<int> retries{0};
                        if (retries.fetch_add(1) < 3) LOG("overlay: camera matrices not found yet, will retry");
                    }
                }
                // Refresh a few times a second: the matrices change with the camera,
                // and re-reading them keeps the grid aligned.
                // Reading a hundred bytes is free; a slow refresh made the grid
                // stutter whenever the camera moved.
                // ~15 Hz: the grid only needs to keep up loosely, and a full screen
                // 4K blit is the expensive part of this mod.
                std::this_thread::sleep_for(std::chrono::milliseconds(66));
            }
        }).detach();
    }

    void tick() {
        // The overlay thread does the work; nothing to do per mod tick.
    }

    void toggle() {
        g_enabled = !g_enabled.load();
        LOG("overlay grid -> %s", g_enabled.load() ? "ON" : "OFF");
    }

    bool enabled() { return g_enabled.load(); }

    void set_external_renderer(bool on) {
        g_external_renderer = on;
        if (on && g_overlay) ShowWindow(g_overlay, SW_HIDE);
        LOG("overlay: external renderer -> %s", on ? "ON (layered window disabled)" : "OFF");
    }

    bool external_renderer() { return g_external_renderer.load(); }

    bool grid_lines(std::vector<ScreenLine> &out, float width, float height) {
        CameraState cam;
        {
            std::lock_guard<std::mutex> lock(g_camera_lock);
            cam = g_camera;
        }
        if (!cam.valid) {
            out.clear();
            return false;
        }
        collect_grid(cam, width, height, out);
        return !out.empty();
    }

    void set_enabled(bool on) {
        if (g_enabled.exchange(on) == on) return;
        // Turning the grid off makes the next enable re-locate the matrices, which
        // is how to recover after a level change.
        if (!on) g_located = false;
        LOG("overlay grid -> %s", on ? "ON" : "OFF");
    }

    float cell() { return grid_cell_value.load(); }
    void set_cell(float value) {
        if (value < 0.05f) value = 0.05f;
        if (value > 16.0f) value = 16.0f;
        grid_cell_value = value;
    }
    float offset() { return grid_offset_value.load(); }
    void set_offset(float value) {
        if (value < -2.0f) value = -2.0f;
        if (value > 2.0f) value = 2.0f;
        grid_offset_value = value;
    }
    int range() { return grid_range_value.load(); }
    void set_range(int value) {
        if (value < 1) value = 1;
        if (value > 200) value = 200;
        grid_range_value = value;
    }

    void set_color(int r, int g, int b) {
        grid_color_r = r < 0 ? 0 : (r > 255 ? 255 : r);
        grid_color_g = g < 0 ? 0 : (g > 255 ? 255 : g);
        grid_color_b = b < 0 ? 0 : (b > 255 ? 255 : b);
    }

    void get_color(int &r, int &g, int &b) {
        r = grid_color_r.load();
        g = grid_color_g.load();
        b = grid_color_b.load();
    }

    int width() { return grid_width_value.load(); }

    void set_width(int value) {
        if (value < 1) value = 1;
        if (value > 8) value = 8;
        grid_width_value = value;
    }

}  // namespace overlay
