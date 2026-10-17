#include "d3d12_overlay.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "config.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "logger.h"
#include "overlay_grid.h"
#include "overlay_panel.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace d3d12_overlay {

    namespace {

        // ---------------------------------------------------------------- hooks
        using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
        using CreateQueueFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *,
                                                           REFIID, void **);
        using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
        using Present1Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT,
                                                        const DXGI_PRESENT_PARAMETERS *);

        HRESULT STDMETHODCALLTYPE create_queue_hook(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID,
                                                    void **);

        CreateDeviceFn g_orig_create_device = nullptr;
        CreateQueueFn g_orig_create_queue = nullptr;
        PresentFn g_orig_present = nullptr;
        Present1Fn g_orig_present1 = nullptr;

        std::atomic<bool> g_iat_hooked{false};
        std::atomic<bool> g_hooks_ready{false};
        std::atomic<bool> g_enabled{false};
        std::atomic<bool> g_test_geometry{true};

        ID3D12Device *g_device = nullptr;       // one reference held by us
        ID3D12CommandQueue *g_queue = nullptr;  // one reference held by us
        std::mutex g_capture_lock;

        bool in_module(const void *p, HMODULE module) {
            if (!module || !p) return false;
            const auto *base = reinterpret_cast<const BYTE *>(module);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
            return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
        }

        bool patch_import(void *target, void *detour, void **original) {
            HMODULE host = GetModuleHandleW(nullptr);
            if (!host) return false;
            const auto *base = reinterpret_cast<const BYTE *>(host);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
            const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return false;
            auto *desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(const_cast<BYTE *>(base) + dir.VirtualAddress);
            for (; desc->Name; ++desc) {
                auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(const_cast<BYTE *>(base) + desc->FirstThunk);
                for (; thunk->u1.Function; ++thunk) {
                    if (reinterpret_cast<void *>(thunk->u1.Function) != target) continue;
                    DWORD old = 0;
                    if (!VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return false;
                    *original = reinterpret_cast<void *>(thunk->u1.Function);
                    thunk->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
                    DWORD ignored = 0;
                    VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &ignored);
                    return true;
                }
            }
            return false;
        }

        void install_queue_hook(ID3D12Device *device) {
            std::lock_guard<std::mutex> lock(g_capture_lock);
            if (g_device || !device) return;

            void **vtbl = *reinterpret_cast<void ***>(device);
            if (!vtbl) return;
            HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
            HMODULE loader = GetModuleHandleW(L"d3d12.dll");
            if (!in_module(vtbl[8], core) && !in_module(vtbl[8], loader)) {
                LOG("d3d12: CreateCommandQueue slot 8 does not point into D3D12Core, not hooking");
                return;
            }

            DWORD old = 0;
            if (!VirtualProtect(&vtbl[8], sizeof(void *), PAGE_READWRITE, &old)) {
                LOG("d3d12: VirtualProtect failed on device vtable");
                return;
            }
            g_orig_create_queue = reinterpret_cast<CreateQueueFn>(vtbl[8]);
            vtbl[8] = reinterpret_cast<void *>(&create_queue_hook);
            DWORD ignored = 0;
            VirtualProtect(&vtbl[8], sizeof(void *), old, &ignored);

            // The reference comes from the QueryInterface in create_device_hook.
            g_device = device;
            LOG("d3d12: device captured, CreateCommandQueue hooked");
        }

        HRESULT STDMETHODCALLTYPE create_queue_hook(ID3D12Device *self, const D3D12_COMMAND_QUEUE_DESC *desc,
                                                    REFIID riid, void **out) {
            HRESULT hr = g_orig_create_queue(self, desc, riid, out);
            if (SUCCEEDED(hr) && out && *out && desc) {
                // A direct queue is the one that can execute our draw and is the
                // one the swapchain is normally presented through. Other queue
                // types are left entirely alone.
                if (desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                    ID3D12CommandQueue *queue = nullptr;
                    if (SUCCEEDED(reinterpret_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&queue))) &&
                        queue) {
                        init_logger();
                        std::lock_guard<std::mutex> lock(g_capture_lock);
                        if (!g_queue) {
                            g_queue = queue;
                            LOG("d3d12: captured direct command queue %p (priority %d, node %u)", (void *)queue,
                                (int)desc->Priority, desc->NodeMask);
                        } else {
                            queue->Release();
                        }
                    }
                }
            }
            return hr;
        }

        HRESULT WINAPI create_device_hook(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **out) {
            HRESULT hr = g_orig_create_device(adapter, level, riid, out);
            if (SUCCEEDED(hr) && out && *out) {
                init_logger();
                ID3D12Device *device = nullptr;
                if (SUCCEEDED(reinterpret_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&device))) && device) {
                    install_queue_hook(device);
                    if (g_device != device) device->Release();
                }
            }
            return hr;
        }

        // ------------------------------------------------------------- renderer
        constexpr UINT kMaxBuffers = 8;
        constexpr UINT kAllocatorCount = 4;
        // The drawing board can accumulate thousands of short segments; a 2 MB
        // slot keeps a full-screen doodle inside one upload region.
        constexpr UINT64 kUploadSlotBytes = 2 * 1024 * 1024;
        constexpr UINT64 kUploadBytes = kUploadSlotBytes * kAllocatorCount;

        struct Vertex {
            float x, y;
            float r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 24, "vertex layout drifted");

        ID3D12CommandAllocator *g_allocators[kAllocatorCount]{};
        UINT64 g_allocator_fence[kAllocatorCount]{};
        ID3D12GraphicsCommandList *g_list = nullptr;
        ID3D12Fence *g_fence = nullptr;
        UINT64 g_fence_value = 0;
        ID3D12RootSignature *g_root = nullptr;
        ID3D12PipelineState *g_pso = nullptr;
        DXGI_FORMAT g_pso_format = DXGI_FORMAT_UNKNOWN;
        ID3D12DescriptorHeap *g_rtv_heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE g_rtv{};
        ID3D12Resource *g_upload = nullptr;
        Vertex *g_upload_ptr = nullptr;
        UINT g_slot = 0;

        IDXGISwapChain *g_cached_sc = nullptr;
        IDXGISwapChain3 *g_cached_sc3 = nullptr;
        ID3D12Resource *g_buffers[kMaxBuffers]{};
        UINT g_buffer_count = 0;
        IDXGISwapChain *g_buffers_owner = nullptr;
        UINT g_swap_width = 0;
        UINT g_swap_height = 0;

        std::mutex g_draw_lock;
        std::atomic<unsigned long long> g_presents{0};
        std::atomic<unsigned long long> g_draws{0};
        std::atomic<unsigned long long> g_draw_failures{0};
        bool g_renderer_broken = false;

        // ---------------------------------------------------------- in-game UI
        constexpr UINT kImGuiSrvCount = 64;
        ID3D12DescriptorHeap *g_imgui_srv_heap = nullptr;
        UINT g_imgui_srv_next = 0;
        bool g_imgui_ready = false;
        HWND g_game_hwnd = nullptr;
        WNDPROC g_orig_wndproc = nullptr;
        // ImGui's Win32 backend calls SetCapture(), which can deliver
        // WM_CAPTURECHANGED synchronously to the game window while we are still
        // inside the ImGui frame. A recursive mutex plus the in-frame flag below
        // keeps that re-entrant message from turning into std::system_error.
        std::recursive_mutex g_imgui_lock;
        thread_local bool g_imgui_in_frame = false;

        struct ImGuiFrameFlag {
            ImGuiFrameFlag() { g_imgui_in_frame = true; }
            ~ImGuiFrameFlag() { g_imgui_in_frame = false; }
        };

        // -------------------------------------------------------- drawing board
        struct DoodlePoint {
            float x, y;
        };

        std::atomic<bool> g_doodle_enabled{false};
        std::atomic<bool> g_doodle_straight{false};
        std::atomic<int> g_doodle_width{2};
        std::atomic<int> g_doodle_color_r{0};
        std::atomic<int> g_doodle_color_g{0};
        std::atomic<int> g_doodle_color_b{0};
        std::atomic<bool> g_imgui_want_mouse{false};
        std::atomic<float> g_cursor_x{0.0f};
        std::atomic<float> g_cursor_y{0.0f};
        std::atomic<bool> g_cursor_valid{false};
        std::mutex g_doodle_lock;
        std::vector<std::vector<DoodlePoint>> g_doodle_strokes;
        size_t g_doodle_points = 0;
        bool g_doodle_down = false;
        bool g_doodle_stroke_straight = false;

        void imgui_srv_alloc(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                             D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
            const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (g_imgui_srv_next >= kImGuiSrvCount) {
                LOG("panel: SRV descriptor heap exhausted");
                g_imgui_srv_next = kImGuiSrvCount - 1;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE c = g_imgui_srv_heap->GetCPUDescriptorHandleForHeapStart();
            c.ptr += (SIZE_T)g_imgui_srv_next * stride;
            D3D12_GPU_DESCRIPTOR_HANDLE g = g_imgui_srv_heap->GetGPUDescriptorHandleForHeapStart();
            g.ptr += (UINT64)g_imgui_srv_next * stride;
            ++g_imgui_srv_next;
            *cpu = c;
            *gpu = g;
        }

        void imgui_srv_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE,
                            D3D12_GPU_DESCRIPTOR_HANDLE) {
            // The only texture is the font atlas; a linear allocator is enough.
        }

        LRESULT CALLBACK game_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            try {
                if (g_imgui_ready && panel::visible() && !g_imgui_in_frame) {
                    std::lock_guard<std::recursive_mutex> lock(g_imgui_lock);
                    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
                        const ImGuiIO &io = ImGui::GetIO();
                        // Gameplay view switching uses keys such as Q/Tab/Space.
                        // ImGui reports WantCaptureKeyboard merely because its
                        // window is focused, which used to swallow those keys.
                        // Only block the keyboard while a text field is active;
                        // sliders and colour pickers still get their mouse input.
                        if (io.WantCaptureMouse || io.WantTextInput) return 1;
                    }
                }
            } catch (...) {
                // A mod bug must never unwind into the game's message loop.
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 3) LOG("panel: exception swallowed in WndProc hook");
            }
            // While the board is armed the mouse belongs to the doodle layer,
            // but only the left button does. Right/middle drag and the wheel
            // remain available to the game's camera controls, and mouse moves
            // pass through unless we are actually drawing.
            if (g_enabled.load() && g_doodle_enabled.load()) {
                switch (msg) {
                    case WM_LBUTTONDOWN:
                    case WM_LBUTTONUP:
                    case WM_LBUTTONDBLCLK:
                        return 0;
                    default:
                        break;
                }
            }
            return g_orig_wndproc ? CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp)
                                  : DefWindowProcW(hwnd, msg, wp, lp);
        }

        bool ensure_imgui(HWND hwnd, DXGI_FORMAT format) {
            if (g_imgui_ready) return true;
            if (!hwnd || !g_device || !g_queue) return false;

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = nullptr;  // never write imgui.ini into the game folder
            io.LogFilename = nullptr;
            ImGui::StyleColorsDark();
            panel::load_fonts();
            ImGui_ImplWin32_Init(hwnd);

            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = kImGuiSrvCount;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_imgui_srv_heap)))) {
                LOG("panel: CreateDescriptorHeap(SRV) failed");
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                return false;
            }

            ImGui_ImplDX12_InitInfo info{};
            info.Device = g_device;
            info.CommandQueue = g_queue;
            info.NumFramesInFlight = 4;
            info.RTVFormat = format;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.UserData = nullptr;
            info.SrvDescriptorHeap = g_imgui_srv_heap;
            info.SrvDescriptorAllocFn = &imgui_srv_alloc;
            info.SrvDescriptorFreeFn = &imgui_srv_free;
            if (!ImGui_ImplDX12_Init(&info)) {
                LOG("panel: ImGui_ImplDX12_Init failed");
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                return false;
            }

            g_game_hwnd = hwnd;
            g_orig_wndproc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&game_wndproc)));
            g_imgui_ready = true;
            LOG("panel: in-game ImGui ready (hwnd %p, format %d, wndproc %p)", (void *)hwnd, (int)format,
                (void *)g_orig_wndproc);
            return true;
        }

        bool compile_shaders(ID3DBlob **vs, ID3DBlob **ps) {
            static const char *source = R"(
struct VSIn  { float2 pos : POSITION; float4 col : COLOR0; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.col = i.col;
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return i.col; }
)";
            ID3DBlob *error = nullptr;
            HRESULT hr = D3DCompile(source, strlen(source), "overlay.hlsl", nullptr, nullptr, "VSMain", "vs_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs, &error);
            if (FAILED(hr)) {
                LOG("d3d12: vertex shader compile failed 0x%08lX %s", (unsigned long)hr,
                    error ? (const char *)error->GetBufferPointer() : "");
                if (error) error->Release();
                return false;
            }
            hr = D3DCompile(source, strlen(source), "overlay.hlsl", nullptr, nullptr, "PSMain", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps, &error);
            if (FAILED(hr)) {
                LOG("d3d12: pixel shader compile failed 0x%08lX %s", (unsigned long)hr,
                    error ? (const char *)error->GetBufferPointer() : "");
                if (error) error->Release();
                (*vs)->Release();
                *vs = nullptr;
                return false;
            }
            return true;
        }

        bool create_root_signature() {
            D3D12_ROOT_SIGNATURE_DESC desc{};
            desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ID3DBlob *blob = nullptr;
            ID3DBlob *error = nullptr;
            HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
            if (FAILED(hr)) {
                LOG("d3d12: root signature serialize failed 0x%08lX %s", (unsigned long)hr,
                    error ? (const char *)error->GetBufferPointer() : "");
                if (error) error->Release();
                return false;
            }
            hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&g_root));
            blob->Release();
            if (error) error->Release();
            if (FAILED(hr)) {
                LOG("d3d12: CreateRootSignature failed 0x%08lX", (unsigned long)hr);
                return false;
            }
            return true;
        }

        bool ensure_pipeline(DXGI_FORMAT format) {
            if (g_pso && g_pso_format == format) return true;
            if (g_pso) {
                g_pso->Release();
                g_pso = nullptr;
            }
            if (!g_root && !create_root_signature()) return false;

            ID3DBlob *vs = nullptr;
            ID3DBlob *ps = nullptr;
            if (!compile_shaders(&vs, &ps)) return false;

            D3D12_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_root;
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
            desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = FALSE;
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.StencilEnable = FALSE;
            desc.InputLayout = {layout, _countof(layout)};
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = format;
            desc.SampleDesc.Count = 1;

            HRESULT hr = g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_pso));
            vs->Release();
            ps->Release();
            if (FAILED(hr)) {
                LOG("d3d12: CreateGraphicsPipelineState failed 0x%08lX (format %d)", (unsigned long)hr, (int)format);
                return false;
            }
            g_pso_format = format;
            LOG("d3d12: pipeline created for swapchain format %d", (int)format);
            return true;
        }

        bool ensure_renderer() {
            if (g_renderer_broken) return false;
            if (g_list && g_fence && g_upload) return true;
            if (!g_device) return false;

            for (UINT i = 0; i < kAllocatorCount; ++i) {
                if (g_allocators[i]) continue;
                if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&g_allocators[i])))) {
                    LOG("d3d12: CreateCommandAllocator failed");
                    g_renderer_broken = true;
                    return false;
                }
            }
            if (!g_list) {
                if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr,
                                                       IID_PPV_ARGS(&g_list)))) {
                    LOG("d3d12: CreateCommandList failed");
                    g_renderer_broken = true;
                    return false;
                }
                g_list->Close();
            }
            if (!g_fence) {
                if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
                    LOG("d3d12: CreateFence failed");
                    g_renderer_broken = true;
                    return false;
                }
            }
            if (!g_rtv_heap) {
                D3D12_DESCRIPTOR_HEAP_DESC heap{};
                heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                heap.NumDescriptors = 1;
                if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap)))) {
                    LOG("d3d12: CreateDescriptorHeap failed");
                    g_renderer_broken = true;
                    return false;
                }
                g_rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            }
            if (!g_upload) {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC buffer{};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = kUploadBytes;
                buffer.Height = 1;
                buffer.DepthOrArraySize = 1;
                buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1;
                buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&g_upload)))) {
                    LOG("d3d12: CreateCommittedResource(upload) failed");
                    g_renderer_broken = true;
                    return false;
                }
                if (FAILED(g_upload->Map(0, nullptr, reinterpret_cast<void **>(&g_upload_ptr)))) {
                    LOG("d3d12: Map(upload) failed");
                    g_renderer_broken = true;
                    return false;
                }
            }
            return true;
        }

        ID3D12Resource *current_backbuffer(IDXGISwapChain *swapchain, UINT index) {
            if (!swapchain || index >= kMaxBuffers) return nullptr;
            if (g_buffers_owner != swapchain) {
                for (auto *&buffer : g_buffers) {
                    if (buffer) {
                        buffer->Release();
                        buffer = nullptr;
                    }
                }
                g_buffers_owner = swapchain;
                g_buffer_count = 0;
            }
            if (index + 1 > g_buffer_count) g_buffer_count = index + 1;
            if (!g_buffers[index]) {
                HRESULT hr = swapchain->GetBuffer(index, IID_PPV_ARGS(&g_buffers[index]));
                if (FAILED(hr)) {
                    g_buffers[index] = nullptr;
                    return nullptr;
                }
            }
            return g_buffers[index];
        }

        void release_backbuffers() {
            for (auto *&buffer : g_buffers) {
                if (buffer) {
                    buffer->Release();
                    buffer = nullptr;
                }
            }
            g_buffer_count = 0;
            g_buffers_owner = nullptr;
        }

        void build_test_geometry(Vertex *out, UINT &count, float width, float height) {
            // A magenta square in the middle of the render target. Deliberately
            // large: the first goal is to prove the command list reaches the
            // presented backbuffer, not to judge line quality.
            const float half = 48.0f;
            const float cx = width * 0.5f;
            const float cy = height * 0.5f;
            auto px = [&](float x, float y) {
                return Vertex{x / width * 2.0f - 1.0f, 1.0f - y / height * 2.0f, 1.0f, 0.0f, 1.0f, 0.85f};
            };
            const Vertex a = px(cx - half, cy - half);
            const Vertex b = px(cx + half, cy - half);
            const Vertex c = px(cx + half, cy + half);
            const Vertex d = px(cx - half, cy + half);
            out[0] = a;
            out[1] = b;
            out[2] = c;
            out[3] = a;
            out[4] = c;
            out[5] = d;
            count = 6;
        }

        void build_grid_geometry(Vertex *out, UINT capacity, UINT &count, float width, float height) {
            count = 0;
            std::vector<overlay::ScreenLine> lines;
            if (!overlay::grid_lines(lines, width, height)) return;
            int r = 0, g = 0, b = 0;
            overlay::get_color(r, g, b);
            const float cr = r / 255.0f;
            const float cg = g / 255.0f;
            const float cb = b / 255.0f;
            float half = overlay::width() * 0.5f;
            if (half < 1.0f) half = 1.0f;
            auto push = [&](float x, float y) {
                if (count >= capacity) return;
                out[count++] = {x / width * 2.0f - 1.0f, 1.0f - y / height * 2.0f, cr, cg, cb, 0.9f};
            };
            for (const auto &line : lines) {
                const float dx = line.x1 - line.x0;
                const float dy = line.y1 - line.y0;
                const float len = sqrtf(dx * dx + dy * dy);
                if (len < 1.0e-3f) continue;
                const float nx = -dy / len * half;
                const float ny = dx / len * half;
                // Two triangles per world-space grid line.
                push(line.x0 + nx, line.y0 + ny);
                push(line.x0 - nx, line.y0 - ny);
                push(line.x1 - nx, line.y1 - ny);
                push(line.x0 + nx, line.y0 + ny);
                push(line.x1 - nx, line.y1 - ny);
                push(line.x1 + nx, line.y1 + ny);
            }
        }

        void sample_doodle(HWND hwnd, float width, float height) {
            if (!g_doodle_enabled.load() || !hwnd) {
                g_cursor_valid = false;
                std::lock_guard<std::mutex> lock(g_doodle_lock);
                g_doodle_down = false;
                g_doodle_stroke_straight = false;
                return;
            }

            POINT p{};
            if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p)) return;
            RECT client{};
            if (!GetClientRect(hwnd, &client)) return;
            const float cw = (float)(client.right - client.left);
            const float ch = (float)(client.bottom - client.top);
            if (cw < 1.0f || ch < 1.0f) return;

            const bool inside = p.x >= 0 && p.y >= 0 && p.x < client.right && p.y < client.bottom;
            const float x = p.x * width / cw;
            const float y = p.y * height / ch;
            g_cursor_x = x;
            g_cursor_y = y;
            g_cursor_valid = inside;

            std::lock_guard<std::mutex> lock(g_doodle_lock);
            // Over the settings panel the mouse belongs to ImGui, not the brush.
            if (panel::visible() && g_imgui_want_mouse.load()) {
                g_doodle_down = false;
                g_doodle_stroke_straight = false;
                return;
            }

            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (!down) {
                if (g_doodle_down && g_doodle_stroke_straight && !g_doodle_strokes.empty()) {
                    auto &stroke = g_doodle_strokes.back();
                    if (stroke.size() == 1) {
                        const float dx = x - stroke[0].x;
                        const float dy = y - stroke[0].y;
                        if (dx * dx + dy * dy > 0.25f) {
                            stroke.push_back({x, y});
                            ++g_doodle_points;
                        }
                    } else {
                        stroke.back() = {x, y};
                    }
                }
                g_doodle_down = false;
                g_doodle_stroke_straight = false;
                return;
            }
            if (!g_doodle_down) {
                if (!inside) return;
                g_doodle_strokes.emplace_back();
                g_doodle_stroke_straight =
                    g_doodle_straight.load() || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                g_doodle_down = true;
            }
            if (g_doodle_strokes.empty() || g_doodle_points >= 12000) return;
            auto &stroke = g_doodle_strokes.back();
            if (stroke.empty()) {
                stroke.push_back({x, y});
                ++g_doodle_points;
                return;
            }
            if (g_doodle_stroke_straight) {
                // The straight tool keeps exactly two points while the mouse is
                // down; every move replaces the endpoint instead of adding
                // another segment.
                if (stroke.size() == 1) {
                    const float dx = x - stroke[0].x;
                    const float dy = y - stroke[0].y;
                    if (dx * dx + dy * dy > 0.25f) {
                        stroke.push_back({x, y});
                        ++g_doodle_points;
                    }
                } else {
                    stroke.back() = {x, y};
                }
                return;
            }
            if (!inside) {
                g_doodle_down = false;
                g_doodle_stroke_straight = false;
                return;
            }
            const DoodlePoint &last = stroke.back();
            const float dx = x - last.x;
            const float dy = y - last.y;
            // Skip sub-pixel jitter; it only multiplies geometry without
            // changing the visible stroke.
            if (dx * dx + dy * dy >= 9.0f) {
                stroke.push_back({x, y});
                ++g_doodle_points;
            }
        }

        void build_doodle_geometry(Vertex *out, UINT capacity, UINT &count, float width, float height) {
            count = 0;
            std::lock_guard<std::mutex> lock(g_doodle_lock);

            auto put = [&](float x, float y, float r, float g, float b, float a) {
                if (count >= capacity) return;
                out[count++] = {x / width * 2.0f - 1.0f, 1.0f - y / height * 2.0f, r, g, b, a};
            };
            auto segment = [&](float x0, float y0, float x1, float y1, float half, float r, float g, float b,
                               float a) {
                if (count + 6 > capacity) return;
                const float dx = x1 - x0;
                const float dy = y1 - y0;
                const float len = sqrtf(dx * dx + dy * dy);
                if (len < 1.0e-3f) return;
                const float nx = -dy / len * half;
                const float ny = dx / len * half;
                put(x0 + nx, y0 + ny, r, g, b, a);
                put(x0 - nx, y0 - ny, r, g, b, a);
                put(x1 - nx, y1 - ny, r, g, b, a);
                put(x0 + nx, y0 + ny, r, g, b, a);
                put(x1 - nx, y1 - ny, r, g, b, a);
                put(x1 + nx, y1 + ny, r, g, b, a);
            };

            const float r = g_doodle_color_r.load() / 255.0f;
            const float g = g_doodle_color_g.load() / 255.0f;
            const float b = g_doodle_color_b.load() / 255.0f;
            const float half = g_doodle_width.load() * 0.5f;
            for (const auto &stroke : g_doodle_strokes) {
                if (stroke.size() == 1) {
                    const float x = stroke[0].x, y = stroke[0].y, h = half;
                    put(x - h, y - h, r, g, b, 1.0f);
                    put(x + h, y - h, r, g, b, 1.0f);
                    put(x + h, y + h, r, g, b, 1.0f);
                    put(x - h, y - h, r, g, b, 1.0f);
                    put(x + h, y + h, r, g, b, 1.0f);
                    put(x - h, y + h, r, g, b, 1.0f);
                    continue;
                }
                for (size_t i = 1; i < stroke.size(); ++i) {
                    segment(stroke[i - 1].x, stroke[i - 1].y, stroke[i].x, stroke[i].y, half, r, g, b, 1.0f);
                }
            }

            if (g_doodle_enabled.load() && g_cursor_valid.load()) {
                const float x = g_cursor_x.load();
                const float y = g_cursor_y.load();
                segment(x - 12.0f, y, x + 12.0f, y, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f);
                segment(x, y - 12.0f, x, y + 12.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f);
            }
        }

        void draw(IDXGISwapChain *swapchain) {
            std::lock_guard<std::mutex> lock(g_draw_lock);
            if (g_renderer_broken || !g_enabled.load() || !g_device || !g_queue) return;
            if (!ensure_renderer()) return;

            DXGI_SWAP_CHAIN_DESC swap_desc{};
            if (FAILED(swapchain->GetDesc(&swap_desc))) return;
            if (swap_desc.BufferDesc.Width != g_swap_width || swap_desc.BufferDesc.Height != g_swap_height) {
                // ResizeBuffers keeps the same swapchain object but replaces the
                // buffer resources; cached references would point at the old
                // backbuffers.
                release_backbuffers();
                g_swap_width = swap_desc.BufferDesc.Width;
                g_swap_height = swap_desc.BufferDesc.Height;
            }

            if (g_cached_sc != swapchain) {
                if (g_cached_sc3) {
                    g_cached_sc3->Release();
                    g_cached_sc3 = nullptr;
                }
                g_cached_sc = swapchain;
                swapchain->QueryInterface(IID_PPV_ARGS(&g_cached_sc3));
            }
            const UINT index = g_cached_sc3 ? g_cached_sc3->GetCurrentBackBufferIndex() : 0;
            ID3D12Resource *buffer = current_backbuffer(swapchain, index);
            if (!buffer) return;

            const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
            const float width = (float)desc.Width;
            const float height = (float)desc.Height;
            if (width < 16.0f || height < 16.0f) return;
            if (!ensure_pipeline(desc.Format)) return;

            if (!g_game_hwnd) {
                IDXGISwapChain1 *sc1 = nullptr;
                if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
                    sc1->GetHwnd(&g_game_hwnd);
                    sc1->Release();
                }
            }
            // The panel now shares the game's device and backbuffer. It is only
            // initialized once the swapchain exists, because ImGui's Win32
            // backend needs the real game window for input.
            if (g_game_hwnd && !g_imgui_ready) ensure_imgui(g_game_hwnd, desc.Format);
            const bool want_panel = panel::visible() && g_imgui_ready;
            if (!want_panel) g_imgui_want_mouse = false;

            const UINT slot = g_slot;
            if (g_fence->GetCompletedValue() < g_allocator_fence[slot]) {
                // The GPU is still reading this upload slot. Dropping one overlay
                // frame is preferable to stalling the game's render thread.
                g_draw_failures.fetch_add(1);
                return;
            }
            g_slot = (g_slot + 1) % kAllocatorCount;

            Vertex *vertices = reinterpret_cast<Vertex *>(reinterpret_cast<BYTE *>(g_upload_ptr) +
                                                          slot * kUploadSlotBytes);
            UINT vertex_count = 0;
            constexpr UINT kVertexCapacity = (UINT)(kUploadSlotBytes / sizeof(Vertex));
            if (g_test_geometry.load()) {
                build_test_geometry(vertices, vertex_count, width, height);
            } else if (overlay::enabled()) {
                build_grid_geometry(vertices, kVertexCapacity, vertex_count, width, height);
            }
            sample_doodle(g_game_hwnd, width, height);
            UINT doodle_count = 0;
            build_doodle_geometry(vertices + vertex_count, kVertexCapacity - vertex_count, doodle_count, width,
                                  height);
            vertex_count += doodle_count;
            if (!vertex_count && !want_panel) return;

            HRESULT hr = g_allocators[slot]->Reset();
            if (FAILED(hr)) {
                LOG("d3d12: allocator Reset failed 0x%08lX", (unsigned long)hr);
                return;
            }
            hr = g_list->Reset(g_allocators[slot], g_pso);
            if (FAILED(hr)) {
                LOG("d3d12: command list Reset failed 0x%08lX", (unsigned long)hr);
                return;
            }

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = buffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_list->ResourceBarrier(1, &barrier);

            g_device->CreateRenderTargetView(buffer, nullptr, g_rtv);
            g_list->OMSetRenderTargets(1, &g_rtv, FALSE, nullptr);

            D3D12_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
            g_list->RSSetViewports(1, &viewport);
            g_list->RSSetScissorRects(1, &scissor);

            if (vertex_count) {
                D3D12_VERTEX_BUFFER_VIEW vb{};
                vb.BufferLocation = g_upload->GetGPUVirtualAddress() + slot * kUploadSlotBytes;
                vb.SizeInBytes = vertex_count * sizeof(Vertex);
                vb.StrideInBytes = sizeof(Vertex);
                g_list->IASetVertexBuffers(0, 1, &vb);
                g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_list->SetGraphicsRootSignature(g_root);
                g_list->SetPipelineState(g_pso);
                g_list->DrawInstanced(vertex_count, 1, 0, 0);
            }

            if (want_panel) {
                ImGuiFrameFlag frame_flag;
                std::lock_guard<std::recursive_mutex> lock(g_imgui_lock);
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGuiIO &io = ImGui::GetIO();
                // Win32 reports the client rect; the backbuffer is the authority
                // for both drawing and mouse coordinates.
                io.DisplaySize = ImVec2(width, height);
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                io.MouseDrawCursor = true;
                ImGui::NewFrame();
                g_imgui_want_mouse = io.WantCaptureMouse;
                panel::draw_ui(true, width, height);
                ImGui::Render();
                ID3D12DescriptorHeap *heaps[] = {g_imgui_srv_heap};
                g_list->SetDescriptorHeaps(1, heaps);
                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
            }

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_list->ResourceBarrier(1, &barrier);

            hr = g_list->Close();
            if (FAILED(hr)) {
                LOG("d3d12: command list Close failed 0x%08lX", (unsigned long)hr);
                return;
            }

            ID3D12CommandList *lists[] = {g_list};
            g_queue->ExecuteCommandLists(1, lists);
            const UINT64 value = ++g_fence_value;
            if (FAILED(g_queue->Signal(g_fence, value))) {
                g_renderer_broken = true;
                LOG("d3d12: queue Signal failed");
                return;
            }
            g_allocator_fence[slot] = value;
            g_draws.fetch_add(1);

            const auto draws = g_draws.load();
            if (draws == 1 || draws % 300 == 0) {
                LOG("d3d12: drew overlay (draw #%llu, %u grid verts, panel %s, buffer %u, %.0fx%.0f)", draws,
                    vertex_count, want_panel ? "on" : "off", index, width, height);
            }
        }

        HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain *swapchain, UINT sync_interval, UINT flags) {
            static thread_local bool inside = false;
            if (!inside) {
                inside = true;
                g_presents.fetch_add(1);
                if (!(flags & DXGI_PRESENT_TEST)) {
                    try {
                        draw(swapchain);
                    } catch (...) {
                        g_renderer_broken = true;
                        LOG("d3d12: exception in Present hook, overlay disabled");
                    }
                }
                inside = false;
            }
            const auto presents = g_presents.load();
            if (presents % 600 == 0) {
                LOG("d3d12: %llu presents, %llu draws, %llu skipped", presents, g_draws.load(),
                    g_draw_failures.load());
            }
            return g_orig_present(swapchain, sync_interval, flags);
        }

        HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1 *swapchain, UINT sync_interval, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *params) {
            static thread_local bool inside = false;
            if (!inside) {
                inside = true;
                g_presents.fetch_add(1);
                if (!(flags & DXGI_PRESENT_TEST)) {
                    try {
                        draw(swapchain);
                    } catch (...) {
                        g_renderer_broken = true;
                        LOG("d3d12: exception in Present1 hook, overlay disabled");
                    }
                }
                inside = false;
            }
            return g_orig_present1(swapchain, sync_interval, flags, params);
        }

        bool install_present_hook() {
            static const wchar_t *kClass = L"SinkingStarCameraDummy";
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
            HWND hwnd = CreateWindowExW(0, kClass, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
            if (!hwnd) {
                LOG("d3d12: dummy window creation failed");
                return false;
            }

            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 1;
            sd.BufferDesc.Width = 8;
            sd.BufferDesc.Height = 8;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *context = nullptr;
            IDXGISwapChain *swapchain = nullptr;
            HRESULT hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &sd, &swapchain, &device, nullptr, &context);
            if (FAILED(hr)) {
                hr = D3D11CreateDeviceAndSwapChain(
                    nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                    D3D11_SDK_VERSION, &sd, &swapchain, &device, nullptr, &context);
            }
            if (FAILED(hr) || !swapchain) {
                LOG("d3d12: dummy swapchain creation failed 0x%08lX", (unsigned long)hr);
                DestroyWindow(hwnd);
                return false;
            }

            void **vtbl = *reinterpret_cast<void ***>(swapchain);
            HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
            if (!vtbl || !in_module(vtbl[8], dxgi)) {
                LOG("d3d12: swapchain Present slot does not point into dxgi.dll, not hooking");
                swapchain->Release();
                if (context) context->Release();
                if (device) device->Release();
                DestroyWindow(hwnd);
                return false;
            }

            DWORD old = 0;
            if (!VirtualProtect(&vtbl[8], sizeof(void *), PAGE_READWRITE, &old)) {
                LOG("d3d12: VirtualProtect(swapchain Present) failed");
                swapchain->Release();
                if (context) context->Release();
                if (device) device->Release();
                DestroyWindow(hwnd);
                return false;
            }
            g_orig_present = reinterpret_cast<PresentFn>(vtbl[8]);
            vtbl[8] = reinterpret_cast<void *>(&present_hook);
            DWORD ignored = 0;
            VirtualProtect(&vtbl[8], sizeof(void *), old, &ignored);

            if (in_module(vtbl[17], dxgi)) {
                if (VirtualProtect(&vtbl[17], sizeof(void *), PAGE_READWRITE, &old)) {
                    g_orig_present1 = reinterpret_cast<Present1Fn>(vtbl[17]);
                    vtbl[17] = reinterpret_cast<void *>(&present1_hook);
                    VirtualProtect(&vtbl[17], sizeof(void *), old, &ignored);
                }
            }

            swapchain->Release();
            if (context) context->Release();
            if (device) device->Release();
            DestroyWindow(hwnd);
            LOG("d3d12: Present hook installed (Present1 %s)", g_orig_present1 ? "also hooked" : "unavailable");
            return true;
        }

    }  // namespace

    void early_init() {
        // The game imports D3D12CreateDevice by ordinal 101, so match the
        // resolved address rather than the import name.
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        if (!d3d12) {
            g_iat_hooked = false;
            return;
        }
        void *target = reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12CreateDevice"));
        if (!target) {
            g_iat_hooked = false;
            return;
        }
        void *original = nullptr;
        if (!patch_import(target, reinterpret_cast<void *>(&create_device_hook), &original)) {
            g_iat_hooked = false;
            return;
        }
        g_orig_create_device = reinterpret_cast<CreateDeviceFn>(original);
        g_iat_hooked = true;
    }

    void init() {
        init_logger();
        g_test_geometry = config::d3d12_test();
        g_doodle_width = config::doodle_width();
        g_doodle_color_r = config::doodle_color_r();
        g_doodle_color_g = config::doodle_color_g();
        g_doodle_color_b = config::doodle_color_b();
        if (!config::d3d12_enabled()) {
            LOG("d3d12: disabled by config (route 2 test stays off)");
            return;
        }
        g_enabled = true;
        g_doodle_enabled = config::doodle_enabled();
        // Route 2 lives in the game's own backbuffer, so the layered GDI window
        // must stay hidden or DWM will drop independent flip again.
        overlay::set_external_renderer(true);
        if (!g_iat_hooked) {
            LOG("d3d12: D3D12CreateDevice import hook was not installed; device will not be captured");
        }
        if (!install_present_hook()) return;
        g_hooks_ready = true;
        LOG("d3d12: route 2 overlay armed (%s, F11 toggles)",
            g_test_geometry.load() ? "test geometry" : "camera grid");
        if (g_doodle_enabled.load())
            LOG("doodle: enabled by config (width %d, color %d,%d,%d)", g_doodle_width.load(),
                g_doodle_color_r.load(), g_doodle_color_g.load(), g_doodle_color_b.load());
    }

    void shutdown() {
        g_enabled = false;
    }

    bool enabled() { return g_enabled.load(); }

    void toggle() {
        const bool on = !g_enabled.load();
        g_enabled = on;
        if (!on) {
            std::lock_guard<std::mutex> lock(g_doodle_lock);
            g_doodle_down = false;
            g_doodle_stroke_straight = false;
        }
        LOG("d3d12: overlay %s", on ? "ON" : "OFF");
    }

    bool doodle_supported() { return g_hooks_ready.load(); }

    bool doodle_enabled() { return g_doodle_enabled.load(); }

    int doodle_width() { return g_doodle_width.load(); }

    void set_doodle_width(int value) {
        if (value < 1) value = 1;
        if (value > 16) value = 16;
        g_doodle_width = value;
    }

    void get_doodle_color(int &r, int &g, int &b) {
        r = g_doodle_color_r.load();
        g = g_doodle_color_g.load();
        b = g_doodle_color_b.load();
    }

    void set_doodle_color(int r, int g, int b) {
        g_doodle_color_r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g_doodle_color_g = g < 0 ? 0 : (g > 255 ? 255 : g);
        g_doodle_color_b = b < 0 ? 0 : (b > 255 ? 255 : b);
    }

    bool doodle_straight() { return g_doodle_straight.load(); }

    void set_doodle_straight(bool on) { g_doodle_straight = on; }

    void set_doodle_enabled(bool on) {
        if (g_doodle_enabled.exchange(on) == on) return;
        if (!on) {
            std::lock_guard<std::mutex> lock(g_doodle_lock);
            g_doodle_down = false;
            g_doodle_stroke_straight = false;
        }
        LOG("doodle: %s", on ? "ON (mouse captured, left button draws)" : "OFF (mouse returned to game)");
    }

    void toggle_doodle() { set_doodle_enabled(!g_doodle_enabled.load()); }

    void clear_doodle() {
        std::lock_guard<std::mutex> lock(g_doodle_lock);
        g_doodle_strokes.clear();
        g_doodle_points = 0;
        g_doodle_down = false;
        g_doodle_stroke_straight = false;
        LOG("doodle: cleared");
    }

}  // namespace d3d12_overlay
