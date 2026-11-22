// MIT License
//
// Copyright (c) 2026 Matthieu Bucchianeri
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

#include "adapter.h"
#include "diag.h"
#include "log.h"

namespace {

    decltype(&LoadLibraryExW) g_original_LoadLibraryExW = nullptr;
    decltype(&D3D11CreateDeviceAndSwapChain) g_original_D3D11CreateDeviceAndSwapChain = nullptr;
    decltype(&D3D11CreateDevice) g_original_D3D11CreateDevice = nullptr;

    // Set while we create a device, so that nested calls (AGS calls D3D11CreateDevice itself) go straight to the
    // original functions.
    thread_local bool t_inDeviceCreation = false;

    // Vendor ID spoofing.
    //
    // Applications may check that the GPU is from NVIDIA (through the DXGI adapter description) before they even query
    // NVAPI for the VR features. We report the AMD GPUs as NVIDIA ones, but only to the application's own code (the
    // main executable): AGS, the AMD driver and the D3D runtime must see the real vendor.
    // TODO: Only the vendor ID is changed: the device ID and description remain AMD's.
    constexpr UINT kVendorIdAmd = 0x1002;
    constexpr UINT kVendorIdNvidia = 0x10de;

    decltype(&CreateDXGIFactory) g_original_CreateDXGIFactory = nullptr;
    decltype(&CreateDXGIFactory1) g_original_CreateDXGIFactory1 = nullptr;
    decltype(&CreateDXGIFactory2) g_original_CreateDXGIFactory2 = nullptr;

    using PFN_GetDesc = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
    using PFN_GetDesc1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
    using PFN_GetDesc2 = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter2*, DXGI_ADAPTER_DESC2*);
    using PFN_GetDesc3 = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter4*, DXGI_ADAPTER_DESC3*);
    PFN_GetDesc g_original_GetDesc = nullptr;
    PFN_GetDesc1 g_original_GetDesc1 = nullptr;
    PFN_GetDesc2 g_original_GetDesc2 = nullptr;
    PFN_GetDesc3 g_original_GetDesc3 = nullptr;

    // Whether a return address is in the main executable.
    bool IsCalledFromApplication(void* returnAddress) {
        static const auto [begin, end] = [] {
            const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
            return std::pair{base, base + ntHeaders->OptionalHeader.SizeOfImage};
        }();
        const auto address = static_cast<const uint8_t*>(returnAddress);
        return address >= begin && address < end;
    }

    void SpoofVendorId(UINT& vendorId, const char* method, void* returnAddress) {
        if (vendorId != kVendorIdAmd || !IsCalledFromApplication(returnAddress)) {
            return;
        }
        vendorId = kVendorIdNvidia;
        static std::once_flag logged;
        std::call_once(logged, [&] {
            logging::Log("Reporting the AMD GPU as NVIDIA to the application (first time: %s from exe+0x%llx)\n",
                         method,
                         static_cast<unsigned long long>(static_cast<const uint8_t*>(returnAddress) -
                                                         reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr))));
        });
    }

    HRESULT STDMETHODCALLTYPE hooked_GetDesc(IDXGIAdapter* adapter, DXGI_ADAPTER_DESC* desc) {
        const HRESULT hr = g_original_GetDesc(adapter, desc);
        if (SUCCEEDED(hr) && desc) {
            SpoofVendorId(desc->VendorId, "GetDesc", _ReturnAddress());
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hooked_GetDesc1(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1* desc) {
        const HRESULT hr = g_original_GetDesc1(adapter, desc);
        if (SUCCEEDED(hr) && desc) {
            SpoofVendorId(desc->VendorId, "GetDesc1", _ReturnAddress());
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hooked_GetDesc2(IDXGIAdapter2* adapter, DXGI_ADAPTER_DESC2* desc) {
        const HRESULT hr = g_original_GetDesc2(adapter, desc);
        if (SUCCEEDED(hr) && desc) {
            SpoofVendorId(desc->VendorId, "GetDesc2", _ReturnAddress());
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hooked_GetDesc3(IDXGIAdapter4* adapter, DXGI_ADAPTER_DESC3* desc) {
        const HRESULT hr = g_original_GetDesc3(adapter, desc);
        if (SUCCEEDED(hr) && desc) {
            SpoofVendorId(desc->VendorId, "GetDesc3", _ReturnAddress());
        }
        return hr;
    }

    template <typename T>
    T GetVtableEntry(void* object, size_t index) {
        return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[index]);
    }

    // Hook the adapter description methods, on the first factory created. The methods are shared by all the adapters
    // (they are in dxgi.dll), so hooking the ones of the first adapter is enough.
    void HookAdapterMethods(IUnknown* factoryUnknown) {
        static std::once_flag once;
        std::call_once(once, [factoryUnknown] {
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factoryUnknown->QueryInterface(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapters1(0, &adapter))) {
                logging::Log("Cannot hook the DXGI adapter methods: no adapter\n");
                return;
            }

            // Vtable indices: IUnknown (3), IDXGIObject (4), then IDXGIAdapter::EnumOutputs (7), GetDesc (8),
            // CheckInterfaceSupport (9), IDXGIAdapter1::GetDesc1 (10), IDXGIAdapter2::GetDesc2 (11),
            // IDXGIAdapter3 (12 to 17) and IDXGIAdapter4::GetDesc3 (18).
            g_original_GetDesc = GetVtableEntry<PFN_GetDesc>(adapter.Get(), 8);
            g_original_GetDesc1 = GetVtableEntry<PFN_GetDesc1>(adapter.Get(), 10);
            Microsoft::WRL::ComPtr<IDXGIAdapter2> adapter2;
            if (SUCCEEDED(adapter.As(&adapter2))) {
                g_original_GetDesc2 = GetVtableEntry<PFN_GetDesc2>(adapter2.Get(), 11);
            }
            Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter4;
            if (SUCCEEDED(adapter.As(&adapter4))) {
                g_original_GetDesc3 = GetVtableEntry<PFN_GetDesc3>(adapter4.Get(), 18);
            }

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(reinterpret_cast<PVOID*>(&g_original_GetDesc), reinterpret_cast<PVOID>(hooked_GetDesc));
            DetourAttach(reinterpret_cast<PVOID*>(&g_original_GetDesc1), reinterpret_cast<PVOID>(hooked_GetDesc1));
            if (g_original_GetDesc2) {
                DetourAttach(reinterpret_cast<PVOID*>(&g_original_GetDesc2), reinterpret_cast<PVOID>(hooked_GetDesc2));
            }
            if (g_original_GetDesc3) {
                DetourAttach(reinterpret_cast<PVOID*>(&g_original_GetDesc3), reinterpret_cast<PVOID>(hooked_GetDesc3));
            }
            logging::Log("DXGI adapter hooks installed: %ld\n", DetourTransactionCommit());
        });
    }

    HRESULT WINAPI hooked_CreateDXGIFactory(REFIID riid, void** ppFactory) {
        const HRESULT hr = g_original_CreateDXGIFactory(riid, ppFactory);
        if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
            HookAdapterMethods(static_cast<IUnknown*>(*ppFactory));
        }
        return hr;
    }

    HRESULT WINAPI hooked_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
        const HRESULT hr = g_original_CreateDXGIFactory1(riid, ppFactory);
        if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
            HookAdapterMethods(static_cast<IUnknown*>(*ppFactory));
        }
        return hr;
    }

    HRESULT WINAPI hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
        const HRESULT hr = g_original_CreateDXGIFactory2(Flags, riid, ppFactory);
        if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
            HookAdapterMethods(static_cast<IUnknown*>(*ppFactory));
        }
        return hr;
    }

    // Describe the adapter a device is created on, with its real vendor (for logging).
    // The description of the adapter a device is created on (the default adapter when null), with its real vendor.
    bool GetAdapterDesc(IDXGIAdapter* adapter, DXGI_ADAPTER_DESC& desc) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> target = adapter;
        if (!target) {
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            if (!g_original_CreateDXGIFactory1 || FAILED(g_original_CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapters(0, &target))) {
                return false;
            }
        }
        return SUCCEEDED(g_original_GetDesc ? g_original_GetDesc(target.Get(), &desc) : target->GetDesc(&desc));
    }

    std::string DescribeAdapter(IDXGIAdapter* adapter) {
        DXGI_ADAPTER_DESC desc{};
        if (!GetAdapterDesc(adapter, desc)) {
            return adapter ? "?" : "default adapter (?)";
        }
        char description[128];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, description, sizeof(description), nullptr, nullptr);
        char result[192];
        sprintf_s(result, "%s%s (vendor 0x%04x)", adapter ? "" : "default adapter: ", description, desc.VendorId);
        return result;
    }

    // Tells the user, once, that SPS/MVR will not be available.
    void ReportMultiViewUnsupported(const char* details) {
        static std::once_flag once;
        std::call_once(once, [details] {
            logging::Log("ERROR: Your GPU or driver does not support multi-view (AMD view broadcasting): Single Pass "
                         "Stereo and Multi-View Rendering will not be available. %s\n",
                         details);
        });
    }

    // Checks whether a device created on an AMD GPU got multi-view. A device on another GPU (for example of a
    // hybrid system) never has multi-view, which is expected.
    void CheckMultiViewSupport(IDXGIAdapter* adapter, bool multiView, const std::string& reason = {}) {
        DXGI_ADAPTER_DESC desc{};
        if (multiView || !GetAdapterDesc(adapter, desc) || desc.VendorId != kVendorIdAmd) {
            return;
        }
        ReportMultiViewUnsupported(("GPU: " + DescribeAdapter(adapter) + "." + (reason.empty() ? "" : " " + reason)).c_str());
    }

    HMODULE WINAPI hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
        if (lpLibFileName && _wcsicmp(lpLibFileName, L"nvapi64.dll") == 0) {
            logging::Log("Redirecting LoadLibraryExW(%ls)\n", lpLibFileName);
            // Take a reference on ourselves, to balance the FreeLibrary() the caller will eventually do.
            HMODULE self = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&hooked_LoadLibraryExW), &self);
            return self;
        }
        return g_original_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }

    HRESULT WINAPI hooked_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter,
                                                        D3D_DRIVER_TYPE DriverType,
                                                        HMODULE Software,
                                                        UINT Flags,
                                                        const D3D_FEATURE_LEVEL* pFeatureLevels,
                                                        UINT FeatureLevels,
                                                        UINT SDKVersion,
                                                        const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                        IDXGISwapChain** ppSwapChain,
                                                        ID3D11Device** ppDevice,
                                                        D3D_FEATURE_LEVEL* pFeatureLevel,
                                                        ID3D11DeviceContext** ppImmediateContext) {
        auto& state = adapter::GetState();

        // Nested calls, and calls we cannot handle (feature level probing with no device returned).
        if (t_inDeviceCreation || !ppDevice || state.agsUnavailable) {
            return g_original_D3D11CreateDeviceAndSwapChain(pAdapter,
                                                            DriverType,
                                                            Software,
                                                            Flags,
                                                            pFeatureLevels,
                                                            FeatureLevels,
                                                            SDKVersion,
                                                            pSwapChainDesc,
                                                            ppSwapChain,
                                                            ppDevice,
                                                            pFeatureLevel,
                                                            ppImmediateContext);
        }
        t_inDeviceCreation = true;

        // A new context for each device (see adapter::AgsDevice).
        AGSContext* context = nullptr;
        {
            AGSGPUInfo gpuInfo{};
            const AGSReturnCode result = agsInitialize(AGS_CURRENT_VERSION, nullptr, &context, &gpuInfo);
            if (result != AGS_SUCCESS) {
                logging::Log("agsInitialize() returned %d\n", result);
                char details[128];
                sprintf_s(details,
                          "AMD GPU Services (AGS) could not be initialized (error %d): is an AMD GPU and driver installed?",
                          result);
                ReportMultiViewUnsupported(details);
                context = nullptr;
                state.agsUnavailable = true;
            } else {
                static bool logged = false;
                if (!logged) {
                    logging::Log("agsInitialize() succeeded (Radeon Software: %s)\n",
                                 gpuInfo.radeonSoftwareVersion ? gpuInfo.radeonSoftwareVersion : "?");
                    logged = true;
                }
            }
        }

        AGSDX11DeviceCreationParams creationParams{};
        creationParams.pAdapter = pAdapter;
        creationParams.DriverType = DriverType;
        creationParams.Software = Software;
        creationParams.Flags = Flags;
        creationParams.pFeatureLevels = pFeatureLevels;
        creationParams.FeatureLevels = FeatureLevels;
        creationParams.SDKVersion = SDKVersion;
        creationParams.pSwapChainDesc = ppSwapChain ? pSwapChainDesc : nullptr;

        AGSDX11ExtensionParams extensionParams{};
        extensionParams.pAppName = L"AMD-MVR-Adapter";
        extensionParams.pEngineName = L"AMD-MVR-Adapter";
        // TODO: Must pick a UAV slot that does not collide with the application's shaders.
        extensionParams.uavSlot = 7;
        extensionParams.crossfireMode = AGS_CROSSFIRE_MODE_DISABLE;

        AGSDX11ReturnedParams returnedParams{};
        HRESULT hr;
        const AGSReturnCode agsResult =
            context ? agsDriverExtensionsDX11_CreateDevice(context, &creationParams, &extensionParams, &returnedParams)
                    : AGS_FAILURE;
        if (context && agsResult == AGS_SUCCESS) {
            logging::Log("Created D3D11 device %p through AGS context %p (multiView=%u)\n",
                         returnedParams.pDevice,
                         context,
                         returnedParams.extensionsSupported.multiView);
            CheckMultiViewSupport(pAdapter, returnedParams.extensionsSupported.multiView);

            {
                std::lock_guard lock(state.agsDevicesMutex);
                state.agsDevices[returnedParams.pDevice] = {context, returnedParams.extensionsSupported};
            }
            diag::OnDeviceCreated(returnedParams.pImmediateContext);

            // With the debug layer, our patched shaders warn on every draw that nothing is bound to the sampler slot of
            // the AMD intrinsics (which is expected: the driver intercepts that sample). Hide the warning, since some
            // applications break on warnings.
            // TODO: This also hides the warning for the application's own sampler slots.
            Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
            if (SUCCEEDED(returnedParams.pDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
                D3D11_MESSAGE_ID hide[] = {D3D11_MESSAGE_ID_DEVICE_DRAW_SAMPLER_NOT_SET};
                D3D11_INFO_QUEUE_FILTER filter{};
                filter.DenyList.NumIDs = ARRAYSIZE(hide);
                filter.DenyList.pIDList = hide;
                infoQueue->AddStorageFilterEntries(&filter);
            }

            *ppDevice = returnedParams.pDevice;
            if (ppImmediateContext) {
                *ppImmediateContext = returnedParams.pImmediateContext;
            } else {
                returnedParams.pImmediateContext->Release();
            }
            if (ppSwapChain) {
                *ppSwapChain = returnedParams.pSwapChain;
            }
            if (pFeatureLevel) {
                *pFeatureLevel = returnedParams.featureLevel;
            }
            hr = S_OK;
        } else {
            if (context) {
                logging::Log("Could not create the device through AGS (error %d), falling back without AGS: %s, "
                             "driverType=%d, flags=0x%x, %u feature levels (first 0x%x), swap chain=%s\n",
                             agsResult,
                             DescribeAdapter(pAdapter).c_str(),
                             DriverType,
                             Flags,
                             FeatureLevels,
                             pFeatureLevels && FeatureLevels ? pFeatureLevels[0] : 0,
                             ppSwapChain && pSwapChainDesc ? "yes" : "no");
                agsDeInitialize(context);
                CheckMultiViewSupport(pAdapter, false, "AGS could not create the device (error " + std::to_string(agsResult) + ").");
            }
            hr = g_original_D3D11CreateDeviceAndSwapChain(pAdapter,
                                                          DriverType,
                                                          Software,
                                                          Flags,
                                                          pFeatureLevels,
                                                          FeatureLevels,
                                                          SDKVersion,
                                                          pSwapChainDesc,
                                                          ppSwapChain,
                                                          ppDevice,
                                                          pFeatureLevel,
                                                          ppImmediateContext);
        }

        t_inDeviceCreation = false;
        return hr;
    }

    HRESULT WINAPI hooked_D3D11CreateDevice(IDXGIAdapter* pAdapter,
                                            D3D_DRIVER_TYPE DriverType,
                                            HMODULE Software,
                                            UINT Flags,
                                            const D3D_FEATURE_LEVEL* pFeatureLevels,
                                            UINT FeatureLevels,
                                            UINT SDKVersion,
                                            ID3D11Device** ppDevice,
                                            D3D_FEATURE_LEVEL* pFeatureLevel,
                                            ID3D11DeviceContext** ppImmediateContext) {
        if (t_inDeviceCreation) {
            return g_original_D3D11CreateDevice(pAdapter,
                                                DriverType,
                                                Software,
                                                Flags,
                                                pFeatureLevels,
                                                FeatureLevels,
                                                SDKVersion,
                                                ppDevice,
                                                pFeatureLevel,
                                                ppImmediateContext);
        }
        return hooked_D3D11CreateDeviceAndSwapChain(pAdapter,
                                                    DriverType,
                                                    Software,
                                                    Flags,
                                                    pFeatureLevels,
                                                    FeatureLevels,
                                                    SDKVersion,
                                                    nullptr,
                                                    nullptr,
                                                    ppDevice,
                                                    pFeatureLevel,
                                                    ppImmediateContext);
    }

    // A function that cannot be found is skipped: passing a null pointer to DetourAttach() would fail the whole
    // transaction, leaving every other function unhooked.
    template <typename T>
    void Attach(T& original, T hooked, HMODULE module, const char* functionName) {
        original = module ? reinterpret_cast<T>(GetProcAddress(module, functionName)) : nullptr;
        if (!original) {
            logging::Log("Cannot hook %s: not found\n", functionName);
            return;
        }
        DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hooked));
    }

    template <typename T>
    void Attach(T& original, T hooked, const wchar_t* moduleName, const char* functionName) {
        Attach(original, hooked, GetModuleHandleW(moduleName), functionName);
    }

    template <typename T>
    void Detach(T& original, T hooked) {
        if (original) {
            DetourDetach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hooked));
        }
    }

    // The dxgi.dll of the system folder: the application may be using a proxy dxgi.dll (such as ours), whose exports
    // jump to the system one.
    HMODULE LoadSystemDxgi() {
        wchar_t path[MAX_PATH];
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 10) {
            return nullptr;
        }
        wcscat_s(path, L"\\dxgi.dll");
        // Never unloaded: we hook it for the lifetime of the process.
        return LoadLibraryW(path);
    }

} // namespace

namespace adapter {

    bool FindAgsDevice(IUnknown* deviceOrContext, AgsDevice& agsDevice) {
        if (!deviceOrContext) {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (FAILED(deviceOrContext->QueryInterface(IID_PPV_ARGS(&device)))) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            if (FAILED(deviceOrContext->QueryInterface(IID_PPV_ARGS(&context)))) {
                return false;
            }
            context->GetDevice(&device);
        }

        auto& state = GetState();
        std::lock_guard lock(state.agsDevicesMutex);
        const auto it = state.agsDevices.find(device.Get());
        if (it == state.agsDevices.end()) {
            return false;
        }
        agsDevice = it->second;
        return true;
    }

    void InstallHooks() {
        const HMODULE dxgi = LoadSystemDxgi();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Attach(g_original_LoadLibraryExW, hooked_LoadLibraryExW, L"kernelbase.dll", "LoadLibraryExW");
        Attach(g_original_D3D11CreateDevice, hooked_D3D11CreateDevice, L"d3d11.dll", "D3D11CreateDevice");
        Attach(g_original_D3D11CreateDeviceAndSwapChain,
               hooked_D3D11CreateDeviceAndSwapChain,
               L"d3d11.dll",
               "D3D11CreateDeviceAndSwapChain");
        Attach(g_original_CreateDXGIFactory, hooked_CreateDXGIFactory, dxgi, "CreateDXGIFactory");
        Attach(g_original_CreateDXGIFactory1, hooked_CreateDXGIFactory1, dxgi, "CreateDXGIFactory1");
        Attach(g_original_CreateDXGIFactory2, hooked_CreateDXGIFactory2, dxgi, "CreateDXGIFactory2");
        logging::Log("Hooks installed: %ld\n", DetourTransactionCommit());
    }

    void UninstallHooks() {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Detach(g_original_GetDesc, hooked_GetDesc);
        Detach(g_original_GetDesc1, hooked_GetDesc1);
        Detach(g_original_GetDesc2, hooked_GetDesc2);
        Detach(g_original_GetDesc3, hooked_GetDesc3);
        Detach(g_original_CreateDXGIFactory, hooked_CreateDXGIFactory);
        Detach(g_original_CreateDXGIFactory1, hooked_CreateDXGIFactory1);
        Detach(g_original_CreateDXGIFactory2, hooked_CreateDXGIFactory2);
        Detach(g_original_LoadLibraryExW, hooked_LoadLibraryExW);
        Detach(g_original_D3D11CreateDevice, hooked_D3D11CreateDevice);
        Detach(g_original_D3D11CreateDeviceAndSwapChain, hooked_D3D11CreateDeviceAndSwapChain);
        DetourTransactionCommit();
    }

} // namespace adapter
