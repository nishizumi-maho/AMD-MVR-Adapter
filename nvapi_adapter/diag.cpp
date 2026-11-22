// MIT License
//
// Copyright (c) 2026 nishizumi-maho
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "diag.h"
#include "log.h"

namespace {

    using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
    using PFN_RSSetScissorRects = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
    PFN_RSSetViewports g_original_RSSetViewports = nullptr;
    PFN_RSSetScissorRects g_original_RSSetScissorRects = nullptr;

    // The viewport mask of the last view broadcast call (0 while broadcasting is off).
    std::atomic<unsigned long long> g_activeMask{0};

    // Logging attempts left. Once spent, the hooks only forward the calls, so that the diagnostics stop costing CPU
    // time (the A/B measurements with the override file happen after that).
    std::atomic<int> g_budget{50000};

    bool TakeBudget() {
        return g_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
    }

    // Each distinct line is logged once, up to a limit.
    std::mutex g_mutex;
    std::set<std::string> g_logged;
    constexpr size_t kMaxLines = 300;

    void LogOnce(const std::string& line) {
        std::lock_guard lock(g_mutex);
        if (g_logged.size() >= kMaxLines || !g_logged.insert(line).second) {
            return;
        }
        logging::Log("[diag] %s\n", line.c_str());
    }

    std::string Format(const char* fmt, ...) {
        char buffer[256];
        va_list args;
        va_start(args, fmt);
        vsprintf_s(buffer, fmt, args);
        va_end(args);
        return buffer;
    }

    std::string DescribeViewports(UINT count, const D3D11_VIEWPORT* viewports) {
        std::string result = Format("%u viewport(s):", count);
        for (UINT i = 0; i < count; i++) {
            const D3D11_VIEWPORT& v = viewports[i];
            result += Format(" [%u] %.0f,%.0f %.0fx%.0f z=%.2f-%.2f",
                             i, v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth);
        }
        return result;
    }

    std::string DescribeRects(UINT count, const D3D11_RECT* rects) {
        std::string result = Format("%u scissor rect(s):", count);
        for (UINT i = 0; i < count; i++) {
            const D3D11_RECT& r = rects[i];
            result += Format(" [%u] %ld,%ld-%ld,%ld", i, r.left, r.top, r.right, r.bottom);
        }
        return result;
    }

    void STDMETHODCALLTYPE hooked_RSSetViewports(ID3D11DeviceContext* context,
                                                 UINT count,
                                                 const D3D11_VIEWPORT* viewports) {
        const unsigned long long mask = g_activeMask.load(std::memory_order_relaxed);
        if (mask && viewports && TakeBudget()) {
            LogOnce(Format("vpMask=0x%llx RSSetViewports ", mask) + DescribeViewports(count, viewports));
        }
        g_original_RSSetViewports(context, count, viewports);
    }

    void STDMETHODCALLTYPE hooked_RSSetScissorRects(ID3D11DeviceContext* context, UINT count, const D3D11_RECT* rects) {
        const unsigned long long mask = g_activeMask.load(std::memory_order_relaxed);
        if (mask && rects && TakeBudget()) {
            LogOnce(Format("vpMask=0x%llx RSSetScissorRects ", mask) + DescribeRects(count, rects));
        }
        g_original_RSSetScissorRects(context, count, rects);
    }

    // The override mask (0 = none), read from the file at most once per second.
    unsigned long long GetOverrideMask() {
        static std::atomic<unsigned long long> s_override{0};
        static std::atomic<ULONGLONG> s_lastRead{0};
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastRead.load(std::memory_order_relaxed) < 1000) {
            return s_override.load(std::memory_order_relaxed);
        }
        s_lastRead = now;

        unsigned long long value = 0;
        FILE* file = nullptr;
        const std::wstring path = logging::GetDataDirectory() + L"\\vpmask_override.txt";
        if (_wfopen_s(&file, path.c_str(), L"r") == 0 && file) {
            if (fscanf_s(file, "%llx", &value) != 1) {
                value = 0;
            }
            fclose(file);
        }
        if (s_override.exchange(value) != value) {
            logging::Log("[diag] vpMask override: 0x%llx (0 = none)\n", value);
        }
        return value;
    }

    // The immediate context, from a device or a context.
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> GetContext(IUnknown* deviceOrContext) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        if (FAILED(deviceOrContext->QueryInterface(IID_PPV_ARGS(&context)))) {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            if (SUCCEEDED(deviceOrContext->QueryInterface(IID_PPV_ARGS(&device)))) {
                device->GetImmediateContext(&context);
            }
        }
        return context;
    }

    // The viewports, scissor rects, rasterizer state and render target currently bound.
    std::string DescribePipeline(ID3D11DeviceContext* context) {
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT viewportCount = 0;
        context->RSGetViewports(&viewportCount, nullptr);
        viewportCount = std::min<UINT>(viewportCount, ARRAYSIZE(viewports));
        context->RSGetViewports(&viewportCount, viewports);
        std::string result = DescribeViewports(viewportCount, viewports);

        D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT rectCount = 0;
        context->RSGetScissorRects(&rectCount, nullptr);
        rectCount = std::min<UINT>(rectCount, ARRAYSIZE(rects));
        context->RSGetScissorRects(&rectCount, rects);
        result += ", " + DescribeRects(rectCount, rects);

        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
        context->RSGetState(&rasterizer);
        if (rasterizer) {
            D3D11_RASTERIZER_DESC desc{};
            rasterizer->GetDesc(&desc);
            result += Format(", scissor test %s", desc.ScissorEnable ? "on" : "off");
        }

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        context->OMGetRenderTargets(1, &rtv, &dsv);
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        if (rtv) {
            rtv->GetResource(&resource);
        } else if (dsv) {
            dsv->GetResource(&resource);
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            result += Format(", %s %ux%u array=%u", rtv ? "render target" : "depth only", desc.Width, desc.Height,
                             desc.ArraySize);
        }
        return result;
    }

} // namespace

namespace diag {

    void OnDeviceCreated(ID3D11DeviceContext* context) {
        static std::once_flag once;
        std::call_once(once, [context] {
            // ID3D11DeviceContext vtable: IUnknown (0-2), ID3D11DeviceChild (3-6), then VSSetConstantBuffers (7) up
            // to RSSetState (43), RSSetViewports (44) and RSSetScissorRects (45).
            void** vtable = *reinterpret_cast<void***>(context);
            const auto setViewports = reinterpret_cast<PFN_RSSetViewports>(vtable[44]);
            const auto setScissorRects = reinterpret_cast<PFN_RSSetScissorRects>(vtable[45]);

            // Check the indices before hooking: set a viewport and a rect through the vtable entries, read them back,
            // then restore the initial (empty) state of the new device.
            const D3D11_VIEWPORT testViewport{1, 2, 3, 4, 0, 1};
            const D3D11_RECT testRect{5, 6, 7, 8};
            setViewports(context, 1, &testViewport);
            setScissorRects(context, 1, &testRect);
            D3D11_VIEWPORT viewport{};
            D3D11_RECT rect{};
            UINT count = 1;
            context->RSGetViewports(&count, &viewport);
            count = 1;
            context->RSGetScissorRects(&count, &rect);
            context->RSSetViewports(0, nullptr);
            context->RSSetScissorRects(0, nullptr);
            if (viewport.TopLeftX != 1 || viewport.Height != 4 || rect.left != 5 || rect.bottom != 8) {
                logging::Log("[diag] Unexpected ID3D11DeviceContext vtable layout: RS hooks not installed\n");
                return;
            }

            g_original_RSSetViewports = setViewports;
            g_original_RSSetScissorRects = setScissorRects;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(reinterpret_cast<PVOID*>(&g_original_RSSetViewports),
                         reinterpret_cast<PVOID>(hooked_RSSetViewports));
            DetourAttach(reinterpret_cast<PVOID*>(&g_original_RSSetScissorRects),
                         reinterpret_cast<PVOID>(hooked_RSSetScissorRects));
            logging::Log("[diag] RS hooks installed: %ld\n", DetourTransactionCommit());
        });
    }

    void OnMultiViewMode(const NV_MULTIVIEW_PARAMS* params) {
        if (!params || !TakeBudget()) {
            return;
        }
        std::string line = Format("SetMultiViewMode numViews=%u independentViewportMaskEnable=%u renderTargetIndexOffset=",
                                  params->numViews,
                                  params->independentViewportMaskEnable);
        for (NvU32 i = 0; i < NV_MULTIVIEW_MAX_SUPPORTED_VIEWS; i++) {
            line += Format("%s%u", i ? "," : "", params->renderTargetIndexOffset[i]);
        }
        LogOnce(line);
    }

    unsigned long long OnViewBroadcast(IUnknown* deviceOrContext, unsigned long long vpMask) {
        if (vpMask) {
            const unsigned long long overrideMask = GetOverrideMask();
            if (overrideMask) {
                vpMask &= overrideMask;
            }
        }
        g_activeMask.store(vpMask, std::memory_order_relaxed);
        if (vpMask && deviceOrContext && TakeBudget()) {
            if (const auto context = GetContext(deviceOrContext)) {
                LogOnce(Format("vpMask=0x%llx enabled with ", vpMask) + DescribePipeline(context.Get()));
            }
        }
        return vpMask;
    }

} // namespace diag
