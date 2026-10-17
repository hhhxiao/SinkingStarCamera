#include "overlay_panel.h"

#include <Windows.h>
#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "camera.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/imgui.h"
#include "logger.h"
#include "overlay_grid.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace panel {

    namespace {

        std::atomic<bool> g_visible{false};
        bool g_quit = false;

        ID3D11Device *g_device = nullptr;
        ID3D11DeviceContext *g_context = nullptr;
        IDXGISwapChain *g_swap = nullptr;
        ID3D11RenderTargetView *g_rtv = nullptr;
        HWND g_hwnd = nullptr;

        LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
            if (msg == WM_CLOSE) {
                g_visible = false;
                return 0;
            }
            if (msg == WM_DESTROY) {
                // The panel window is destroyed every time it is hidden; that must
                // not end the worker thread, or the panel could never come back.
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        HWND make_window() {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = wnd_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
            wc.lpszClassName = L"SinkingStarCameraPanel";
            RegisterClassExW(&wc);
            // Borderless and owned by nothing: it just sits over the game's top
            // right corner like an overlay. No title bar, drag it with the mouse.
            return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SinkingStarCameraPanel", L"SinkingStarCamera",
                                   WS_POPUP, 120, 120, 460, 620, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        }

        void load_fonts(ImGuiIO &io) {
            // The default font has no CJK glyphs, which is why Chinese showed up as
            // question marks. Prefer Microsoft YaHei at a readable size.
            const wchar_t *candidates[] = {L"C:\\Windows\\Fonts\\msyh.ttc", L"C:\\Windows\\Fonts\\msyhbd.ttc",
                                           L"C:\\Windows\\Fonts\\simhei.ttf"};
            for (const wchar_t *path : candidates) {
                if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
                ImFont *font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 20.0f, nullptr,
                                                           io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                char narrow[MAX_PATH]{};
                WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
                if (font) {
                    LOG("panel: font %s at 20px", narrow);
                    return;
                }
            }
            io.Fonts->AddFontDefault();
            LOG("panel: no CJK font found, using the default (Chinese will not render)");
        }

        HWND find_game_window_for_panel() {
            struct Search {
                DWORD pid;
                HWND found;
            } search{GetCurrentProcessId(), nullptr};
            EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL {
                    auto *s = reinterpret_cast<Search *>(lp);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != s->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
                    RECT r{};
                    if (!GetClientRect(hwnd, &r) || (r.right - r.left) < 320) return TRUE;
                    s->found = hwnd;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&search));
            return search.found;
        }

        bool make_device() {
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = g_hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL got{};
            if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
                                                     D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context)))
                return false;
            ID3D11Texture2D *back = nullptr;
            if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
            g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
            back->Release();
            return g_rtv != nullptr;
        }

        void drop_device() {
            if (g_rtv) g_rtv->Release();
            if (g_swap) g_swap->Release();
            if (g_context) g_context->Release();
            if (g_device) g_device->Release();
            g_rtv = nullptr;
            g_swap = nullptr;
            g_context = nullptr;
            g_device = nullptr;
        }

        void draw_ui() {
            const ImGuiIO &io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("SinkingStarCamera", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            if (ImGui::CollapsingHeader("网格 Grid", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool on = overlay::enabled();
                if (ImGui::Checkbox("显示网格 (F5)", &on)) overlay::set_enabled(on);
                float cell = overlay::cell();
                if (ImGui::SliderFloat("格子尺寸", &cell, 0.1f, 8.0f, "%.3f")) overlay::set_cell(cell);
                float offset = overlay::offset();
                if (ImGui::SliderFloat("半格偏移", &offset, -1.0f, 1.0f, "%.3f")) overlay::set_offset(offset);
                int range = overlay::range();
                if (ImGui::SliderInt("范围 (格)", &range, 1, 80)) overlay::set_range(range);
                int width = overlay::width();
                if (ImGui::SliderInt("线宽", &width, 1, 8)) overlay::set_width(width);
                int r = 0, g = 0, b = 0;
                overlay::get_color(r, g, b);
                float color[3] = {r / 255.0f, g / 255.0f, b / 255.0f};
                if (ImGui::ColorEdit3("线色", color)) {
                    overlay::set_color(static_cast<int>(color[0] * 255.0f + 0.5f),
                                       static_cast<int>(color[1] * 255.0f + 0.5f),
                                       static_cast<int>(color[2] * 255.0f + 0.5f));
                }
                ImGui::Spacing();
                if (ImGui::Button("重置间距/偏移", ImVec2(-1, 0))) {
                    overlay::set_cell(1.0f);
                    overlay::set_offset(0.5f);
                }
            }

            if (ImGui::CollapsingHeader("相机 Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
                float scale = camera::scale();
                if (ImGui::SliderFloat("距离缩放 (F9/F10)", &scale, 0.05f, 8.0f, "%.3f")) camera::set_scale(scale);
                float fov = camera::fov_scale();
                if (ImGui::SliderFloat("视场角 (F7/F8)", &fov, 0.1f, 3.0f, "%.3f")) camera::set_fov_scale(fov);
                bool top_down = camera::top_down();
                if (ImGui::Checkbox("纯俯视 (F6)", &top_down)) camera::set_top_down(top_down);
                ImGui::Spacing();
                if (ImGui::Button("全部复位 (F4)", ImVec2(-1, 0))) camera::reset();
            }

            ImGui::Separator();
            ImGui::TextDisabled("F2 关闭面板 / F5 网格 / F4 复位");
            ImGui::End();
        }

        void thread_main() {
            bool created = false;
            while (!g_quit) {
                if (!g_visible.load()) {
                    if (created) {
                        ImGui_ImplDX11_Shutdown();
                        ImGui_ImplWin32_Shutdown();
                        ImGui::DestroyContext();
                        drop_device();
                        DestroyWindow(g_hwnd);
                        g_hwnd = nullptr;
                        created = false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (!created) {
                    g_hwnd = make_window();
                    if (!g_hwnd || !make_device()) {
                        if (g_hwnd) DestroyWindow(g_hwnd);
                        g_hwnd = nullptr;
                        drop_device();
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        continue;
                    }
                    ShowWindow(g_hwnd, SW_SHOW);
                    // Fixed position: top right of the desktop work area. The panel
                    // does not track the game window and does not hide when the game
                    // loses focus; F2 is the only thing that shows or hides it.
                    {
                        RECT work{};
                        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                        const int w = 460, h = 620;
                        int x = work.right - w - 16;
                        int y = work.top + 16;
                        if (x < work.left + 8) x = work.left + 8;
                        if (y + h > work.bottom - 8) y = work.bottom - h - 8;
                        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
                    }
                    IMGUI_CHECKVERSION();
                    ImGui::CreateContext();
                    load_fonts(ImGui::GetIO());
                    ImGui::StyleColorsDark();
                    ImGui_ImplWin32_Init(g_hwnd);
                    ImGui_ImplDX11_Init(g_device, g_context);
                    created = true;
                    LOG("panel: ImGui window created");
                }

                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                // Nothing moves or hides the panel: F2 owns its visibility.

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                draw_ui();
                ImGui::Render();

                const float clear[4] = {0.08f, 0.09f, 0.11f, 1.0f};
                g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
                g_context->ClearRenderTargetView(g_rtv, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                g_swap->Present(1, 0);

                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        }

    }  // namespace

    void init() { std::thread(thread_main).detach(); }

    void toggle() {
        g_visible = !g_visible.load();
        LOG("panel -> %s", g_visible.load() ? "ON" : "OFF");
    }

    bool visible() { return g_visible.load(); }

}  // namespace panel
