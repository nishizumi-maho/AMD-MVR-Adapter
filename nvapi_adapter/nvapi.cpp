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
#include "shaders.h"

namespace {

    // Known IDs.
    enum : NvU32 {
        kId_NvAPI_Initialize = 0x0150E828,
        kId_NvAPI_Unload = 0xD22BDD7E,
        kId_NvAPI_D3D_RegisterDevice = 0x8C02C4D0,
        kId_NvAPI_D3D11_IsNvShaderExtnOpCodeSupported = 0x5F68DA40,
        kId_NvAPI_D3D_QueryMultiViewSupport = 0xB6E0A41C,
        kId_NvAPI_D3D_QueryModifiedWSupport = 0xCBF9F4F5,
        kId_NvAPI_D3D_QuerySinglePassStereoSupport = 0x6F5F0A6D,
        kId_NvAPI_D3D_SetMultiViewMode = 0x8285C8DA,
        kId_NvAPI_D3D_SetModifiedWMode = 0x06EA4BF4,
        kId_NvAPI_D3D_SetSinglePassStereoMode = 0xA39E6E6E,
        kId_NvAPI_D3D11_CreateVertexShaderEx = 0x0BEAA0B2,
        kId_NvAPI_D3D11_CreateHullShaderEx = 0xB53CAB00,
        kId_NvAPI_D3D11_CreateDomainShaderEx = 0xA0D7180D,
        kId_NvAPI_D3D11_CreateGeometryShaderEx_2 = 0x99ED5C1C,
        kId_NvAPI_D3D11_CreateFastGeometryShaderExplicit = 0x71AB7C9C,
        kId_NvAPI_D3D11_CreatePixelShaderEx_2 = 0x4162822B,
        kId_NvAPI_GetErrorMessage = 0x6C2D048C,
        kId_NvAPI_DRS_CreateSession = 0x0694D52E,
        kId_NvAPI_DRS_DestroySession = 0xDAD9CFF8,
        kId_NvAPI_DRS_LoadSettings = 0x375DBD6B,
        kId_NvAPI_DRS_GetBaseProfile = 0xDA8466A0,
        kId_NvAPI_DRS_GetCurrentGlobalProfile = 0x617BFF9F,
        kId_NvAPI_DRS_FindProfileByName = 0x7E4A9A0B,
        kId_NvAPI_DRS_FindApplicationByName = 0xEEE566B2,
        kId_NvAPI_DRS_GetSetting = 0x73BF8338,
    };

    // IDs queried by the static library that we know about and deliberately do not implement (returning nullptr is
    // handled gracefully by the library).
    bool IsKnownUnimplemented(NvU32 id) {
        switch (id) {
        case 0xAD298D3F: // initInterfaceEx
        case 0x33C7358C: // callStart
        case 0x593E8644: // callReturn
        case 0xD7C61344: // used during unload
            return true;
        default:
            return false;
        }
    }

#define LOG_CALL() logging::Log("--> %s\n", __func__)
// For the functions called for every shader: ProcessShader() already logs one line per shader.
#define LOG_CALL_DETAIL() LOG_DETAIL("--> %s\n", __func__)

    // Whether the device (or the device of the context) was created through AGS with multi-view.
    bool IsMultiViewAvailable(IUnknown* deviceOrContext, adapter::AgsDevice* agsDevice = nullptr) {
        adapter::AgsDevice device;
        if (!adapter::FindAgsDevice(deviceOrContext, device) || !device.extensions.multiView) {
            return false;
        }
        if (agsDevice) {
            *agsDevice = device;
        }
        return true;
    }

    // Translate NVIDIA's multi-view/single-pass stereo state into AGS view broadcasting.
    //
    // With NVIDIA, view N is routed by the viewport mask output by the (fast) geometry shader. For planar projection,
    // view N goes to viewport N. With AGS, the draw is replicated to every viewport set in vpMask, and the shaders
    // can query which viewport they are running for with AmdDxExtShaderIntrinsics_GetViewportIndex().
    NvAPI_Status SetViewBroadcast(IUnknown* deviceOrContext, NvU32 numViews, const NvU32* renderTargetIndexOffset) {
        auto& state = adapter::GetState();
        adapter::AgsDevice agsDevice;
        if (!IsMultiViewAvailable(deviceOrContext, &agsDevice)) {
            static bool logged = false;
            if (!logged) {
                logging::Log("View broadcasting requested on %p, which was not created through AGS with multi-view\n",
                             deviceOrContext);
                logged = true;
            }
            return NVAPI_NOT_SUPPORTED;
        }
        if (numViews > NV_MULTIVIEW_MAX_SUPPORTED_VIEWS) {
            return NVAPI_INVALID_ARGUMENT;
        }

        // A single view means no replication: AGS disables it when both masks are 0.
        unsigned long long vpMask = 0;
        unsigned long long rtSliceMask = 0;
        if (numViews > 1) {
            vpMask = (1ull << numViews) - 1;
            for (NvU32 i = 0; i < numViews; i++) {
                rtSliceMask |= 1ull << renderTargetIndexOffset[i];
            }
            // TODO: AGS replicates to every (viewport, slice) combination. Supporting one slice per view would require
            // additional logic.
            if (__popcnt64(rtSliceMask) > 1) {
                logging::Log("Rendering views to different render target slices is not supported\n");
            }
        }

        vpMask = diag::OnViewBroadcast(deviceOrContext, vpMask);

        // The application toggles the same states every frame: only log each one the first time.
        if (vpMask && (vpMask != state.vpMask || rtSliceMask != state.rtSliceMask)) {
            logging::Log("Setting view broadcast masks: vpMask=0x%llx rtSliceMask=0x%llx\n", vpMask, rtSliceMask);
            state.vpMask = vpMask;
            state.rtSliceMask = rtSliceMask;
        }

        // TODO: This applies to the immediate context of the device, even when the application passes a deferred
        // context.
        const AGSReturnCode result =
            agsDriverExtensionsDX11_SetViewBroadcastMasks(agsDevice.context, vpMask, rtSliceMask, 0);
        if (result != AGS_SUCCESS) {
            logging::Log("agsDriverExtensionsDX11_SetViewBroadcastMasks() failed: %d\n", result);
            return NVAPI_ERROR;
        }
        return NVAPI_OK;
    }

} // namespace

NvAPI_Status __cdecl NvAPI_Initialize() {
    LOG_CALL();
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_Unload() {
    LOG_CALL();
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetErrorMessage(NvAPI_Status nr, NvAPI_ShortString szDesc) {
    logging::Log("--> %s(nr=%d)\n", __func__, nr);
    if (!szDesc) {
        return NVAPI_INVALID_ARGUMENT;
    }
    const char* name = nullptr;
    switch (nr) {
#define NAME(status)                                                                                                   \
    case status:                                                                                                       \
        name = #status;                                                                                                \
        break
        NAME(NVAPI_OK);
        NAME(NVAPI_ERROR);
        NAME(NVAPI_LIBRARY_NOT_FOUND);
        NAME(NVAPI_NO_IMPLEMENTATION);
        NAME(NVAPI_API_NOT_INITIALIZED);
        NAME(NVAPI_INVALID_ARGUMENT);
        NAME(NVAPI_NVIDIA_DEVICE_NOT_FOUND);
        NAME(NVAPI_NOT_SUPPORTED);
        NAME(NVAPI_INCOMPATIBLE_STRUCT_VERSION);
        NAME(NVAPI_PROFILE_NOT_FOUND);
        NAME(NVAPI_EXECUTABLE_NOT_FOUND);
        NAME(NVAPI_SETTING_NOT_FOUND);
#undef NAME
    }
    if (name) {
        strcpy_s(szDesc, NVAPI_SHORT_STRING_MAX, name);
    } else {
        sprintf_s(szDesc, NVAPI_SHORT_STRING_MAX, "NVAPI error %d", nr);
    }
    return NVAPI_OK;
}

// Driver settings (DRS): This is not needed for SPS/MVR, but iRacing will declare that the adapter is not NVIDIA if those fail.
// Implement some very basic mock-ups.

namespace {
    struct FakeDrsObject {
        int unused;
    };
    FakeDrsObject g_drsSession;
    FakeDrsObject g_drsBaseProfile;

    std::string ToString(const NvU16* unicodeString) {
        std::string result;
        for (size_t i = 0; i < NVAPI_UNICODE_STRING_MAX && unicodeString[i]; i++) {
            result += unicodeString[i] < 0x80 ? static_cast<char>(unicodeString[i]) : '?';
        }
        return result;
    }
} // namespace

NvAPI_Status __cdecl NvAPI_DRS_CreateSession(NvDRSSessionHandle* phSession) {
    LOG_CALL();
    if (!phSession) {
        return NVAPI_INVALID_ARGUMENT;
    }
    *phSession = reinterpret_cast<NvDRSSessionHandle>(&g_drsSession);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_DRS_DestroySession(NvDRSSessionHandle hSession) {
    LOG_CALL();
    return hSession ? NVAPI_OK : NVAPI_INVALID_ARGUMENT;
}

NvAPI_Status __cdecl NvAPI_DRS_LoadSettings(NvDRSSessionHandle hSession) {
    LOG_CALL();
    return hSession ? NVAPI_OK : NVAPI_INVALID_ARGUMENT;
}

NvAPI_Status __cdecl NvAPI_DRS_GetBaseProfile(NvDRSSessionHandle hSession, NvDRSProfileHandle* phProfile) {
    LOG_CALL();
    if (!hSession || !phProfile) {
        return NVAPI_INVALID_ARGUMENT;
    }
    *phProfile = reinterpret_cast<NvDRSProfileHandle>(&g_drsBaseProfile);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_DRS_GetCurrentGlobalProfile(NvDRSSessionHandle hSession, NvDRSProfileHandle* phProfile) {
    LOG_CALL();
    return NvAPI_DRS_GetBaseProfile(hSession, phProfile);
}

NvAPI_Status __cdecl NvAPI_DRS_FindProfileByName(NvDRSSessionHandle hSession,
                                                 NvAPI_UnicodeString profileName,
                                                 NvDRSProfileHandle* phProfile) {
    logging::Log("--> %s(%s)\n", __func__, profileName ? ToString(profileName).c_str() : "null");
    if (!hSession || !profileName || !phProfile) {
        return NVAPI_INVALID_ARGUMENT;
    }
    return NVAPI_PROFILE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_DRS_FindApplicationByName(NvDRSSessionHandle hSession,
                                                     NvAPI_UnicodeString appName,
                                                     NvDRSProfileHandle* phProfile,
                                                     NVDRS_APPLICATION* pApplication) {
    logging::Log("--> %s(%s)\n", __func__, appName ? ToString(appName).c_str() : "null");
    if (!hSession || !appName || !phProfile || !pApplication) {
        return NVAPI_INVALID_ARGUMENT;
    }
    return NVAPI_EXECUTABLE_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_DRS_GetSetting(NvDRSSessionHandle hSession,
                                          NvDRSProfileHandle hProfile,
                                          NvU32 settingId,
                                          NVDRS_SETTING* pSetting) {
    logging::Log("--> %s(settingId=0x%08x)\n", __func__, settingId);
    if (!hSession || !hProfile || !pSetting) {
        return NVAPI_INVALID_ARGUMENT;
    }
    return NVAPI_SETTING_NOT_FOUND;
}

NvAPI_Status __cdecl NvAPI_D3D_RegisterDevice(IUnknown* pDev) {
    LOG_CALL();
    if (!pDev) {
        return NVAPI_INVALID_ARGUMENT;
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D11_IsNvShaderExtnOpCodeSupported(IUnknown* pDev, NvU32 opCode, bool* pSupported) {
    logging::Log("--> %s(opCode=%u)\n", __func__, opCode);
    if (!pDev || !pSupported) {
        return NVAPI_INVALID_ARGUMENT;
    }
    // TODO: The VRWorks sample checks FP16 atomics as a proxy for Maxwell or later before it queries the VR features,
    // so we claim support. Shaders actually using them would not work.
    *pSupported = opCode == NV_EXTN_OP_FP16_ATOMIC && IsMultiViewAvailable(pDev);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D_QueryMultiViewSupport(
    IUnknown* pDevice, NV_QUERY_MULTIVIEW_SUPPORT_PARAMS* pQueryMultiViewSupportedParams) {
    LOG_CALL();
    if (!pDevice || !pQueryMultiViewSupportedParams) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (pQueryMultiViewSupportedParams->version != NV_QUERY_MULTIVIEW_SUPPORT_PARAMS_VER1) {
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }
    pQueryMultiViewSupportedParams->bMultiViewSupported = IsMultiViewAvailable(pDevice);
    pQueryMultiViewSupportedParams->bSinglePassStereoSupported = IsMultiViewAvailable(pDevice);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D_QueryModifiedWSupport(
    IUnknown* pDev, NV_QUERY_MODIFIED_W_SUPPORT_PARAMS* pQueryModifiedWSupportedParams) {
    LOG_CALL();
    if (!pDev || !pQueryModifiedWSupportedParams) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (pQueryModifiedWSupportedParams->version != NV_QUERY_MODIFIED_W_SUPPORT_PARAMS_VER1) {
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }
    // iRacing only enables "Nvidia Simultaneous Multi-Projection" (triple screen rendering with multi-view) when
    // Modified W is reported as supported, although it does not use it for that mode. We report it, but only the
    // identity (all coefficients 0) is actually accepted (see NvAPI_D3D_SetModifiedWMode()).
    pQueryModifiedWSupportedParams->bModifiedWSupported = IsMultiViewAvailable(pDev);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D_QuerySinglePassStereoSupport(
    IUnknown* pDevice, NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS* pQuerySinglePassStereoSupportedParams) {
    LOG_CALL();
    if (!pDevice || !pQuerySinglePassStereoSupportedParams) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (pQuerySinglePassStereoSupportedParams->version != NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS_VER1 &&
        pQuerySinglePassStereoSupportedParams->version != NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS_VER2) {
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }
    pQuerySinglePassStereoSupportedParams->bSinglePassStereoSupported = IsMultiViewAvailable(pDevice);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D_SetMultiViewMode(IUnknown* pDevOrContext, NV_MULTIVIEW_PARAMS* pMultiViewParams) {
    if (!pDevOrContext || !pMultiViewParams) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (pMultiViewParams->version != NV_MULTIVIEW_PARAMS_VER1) {
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }
    diag::OnMultiViewMode(pMultiViewParams);
    // TODO: independentViewportMaskEnable is ignored: views always go to viewport 0..numViews-1.
    return SetViewBroadcast(pDevOrContext, pMultiViewParams->numViews, pMultiViewParams->renderTargetIndexOffset);
}

NvAPI_Status __cdecl NvAPI_D3D_SetModifiedWMode(IUnknown* pDevOrContext, NV_MODIFIED_W_PARAMS* psModifiedWParams) {
    if (!pDevOrContext || !psModifiedWParams) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (psModifiedWParams->version != NV_MODIFIED_W_PARAMS_VER1) {
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }
    if (psModifiedWParams->numEntries > NV_MODIFIED_W_MAX_VIEWPORTS) {
        return NVAPI_INVALID_ARGUMENT;
    }

    // TODO: w' = w + A*x + B*y is not implemented (it would need the coefficients in the patched shaders, selected
    // with the viewport index). Only the identity, which changes nothing, is accepted.
    for (NvU32 i = 0; i < psModifiedWParams->numEntries; i++) {
        const auto& coefficients = psModifiedWParams->modifiedWCoefficients[i];
        if (coefficients.fA != 0.f || coefficients.fB != 0.f) {
            static bool logged = false;
            if (!logged) {
                logging::Log("Modified W is not supported (viewport %u: A=%f B=%f)\n", i, coefficients.fA,
                             coefficients.fB);
                logged = true;
            }
            return NVAPI_NOT_SUPPORTED;
        }
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D_SetSinglePassStereoMode(IUnknown* pDevOrContext,
                                                       NvU32 numViews,
                                                       NvU32 renderTargetIndexOffset,
                                                       NvU8 independentViewportMaskEnable) {
    if (!pDevOrContext) {
        return NVAPI_INVALID_ARGUMENT;
    }
    // Single pass stereo is multi-view limited to 2 views, where the right view is offset by renderTargetIndexOffset.
    // TODO: independentViewportMaskEnable is ignored: views always go to viewport 0 and 1.
    if (numViews > 2) {
        return NVAPI_INVALID_ARGUMENT;
    }
    const NvU32 offsets[2] = {0, renderTargetIndexOffset};
    return SetViewBroadcast(pDevOrContext, numViews, offsets);
}

// Patch the shader bytecode to translate necessary constructs from NVIDIA SPS/MVR to AMD AGS.
template <typename ExArgs>
std::pair<const void*, SIZE_T> ProcessShader(const char* stage,
                                             const void* pShaderBytecode,
                                             SIZE_T BytecodeLength,
                                             const ExArgs* args,
                                             std::vector<uint8_t>& storage) {
    if (shaders::ProcessShader(
            stage, pShaderBytecode, BytecodeLength, args->NumCustomSemantics, args->pCustomSemantics, storage)) {
        return {storage.data(), storage.size()};
    }
    return {pShaderBytecode, BytecodeLength};
}

NvAPI_Status __cdecl NvAPI_D3D11_CreateVertexShaderEx(
    ID3D11Device* pDevice,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    const NvAPI_D3D11_CREATE_VERTEX_SHADER_EX* pCreateVertexShaderExArgs,
    ID3D11VertexShader** ppVertexShader) {
    LOG_CALL_DETAIL();
    if (!pDevice || !pCreateVertexShaderExArgs || !ppVertexShader) {
        return NVAPI_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> storage;
    const auto [bytecode, length] = ProcessShader("vs", pShaderBytecode, BytecodeLength, pCreateVertexShaderExArgs, storage);
    return SUCCEEDED(pDevice->CreateVertexShader(bytecode, length, pClassLinkage, ppVertexShader))
               ? NVAPI_OK
               : NVAPI_ERROR;
}

NvAPI_Status __cdecl NvAPI_D3D11_CreateHullShaderEx(ID3D11Device* pDevice,
                                                    const void* pShaderBytecode,
                                                    SIZE_T BytecodeLength,
                                                    ID3D11ClassLinkage* pClassLinkage,
                                                    const NvAPI_D3D11_CREATE_HULL_SHADER_EX* pCreateHullShaderExArgs,
                                                    ID3D11HullShader** ppHullShader) {
    LOG_CALL_DETAIL();
    if (!pDevice || !pCreateHullShaderExArgs || !ppHullShader) {
        return NVAPI_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> storage;
    const auto [bytecode, length] = ProcessShader("hs", pShaderBytecode, BytecodeLength, pCreateHullShaderExArgs, storage);
    return SUCCEEDED(pDevice->CreateHullShader(bytecode, length, pClassLinkage, ppHullShader))
               ? NVAPI_OK
               : NVAPI_ERROR;
}

NvAPI_Status __cdecl NvAPI_D3D11_CreateDomainShaderEx(
    ID3D11Device* pDevice,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    const NvAPI_D3D11_CREATE_DOMAIN_SHADER_EX* pCreateDomainShaderExArgs,
    ID3D11DomainShader** ppDomainShader) {
    LOG_CALL_DETAIL();
    if (!pDevice || !pCreateDomainShaderExArgs || !ppDomainShader) {
        return NVAPI_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> storage;
    const auto [bytecode, length] = ProcessShader("ds", pShaderBytecode, BytecodeLength, pCreateDomainShaderExArgs, storage);
    return SUCCEEDED(pDevice->CreateDomainShader(bytecode, length, pClassLinkage, ppDomainShader))
               ? NVAPI_OK
               : NVAPI_ERROR;
}

NvAPI_Status __cdecl NvAPI_D3D11_CreateGeometryShaderEx_2(
    ID3D11Device* pDevice,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    const NvAPI_D3D11_CREATE_GEOMETRY_SHADER_EX* pCreateGeometryShaderExArgs,
    ID3D11GeometryShader** ppGeometryShader) {
    LOG_CALL_DETAIL();
    if (!pDevice || !pCreateGeometryShaderExArgs || !ppGeometryShader) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (pCreateGeometryShaderExArgs->ForceFastGS) {
        std::vector<uint8_t> storage;
        // A fast GS emits a single vertex per triangle with a viewport mask, which is only possible on NVIDIA
        // hardware. We drop it (ProcessShader() logs it). iRacing doesn't seem to use these anyway.
        ProcessShader("fastgs", pShaderBytecode, BytecodeLength, pCreateGeometryShaderExArgs, storage);
        *ppGeometryShader = nullptr;
        return NVAPI_OK;
    }
    std::vector<uint8_t> storage;
    const auto [bytecode, length] = ProcessShader("gs", pShaderBytecode, BytecodeLength, pCreateGeometryShaderExArgs, storage);
    return SUCCEEDED(pDevice->CreateGeometryShader(bytecode, length, pClassLinkage, ppGeometryShader))
               ? NVAPI_OK
               : NVAPI_ERROR;
}

NvAPI_Status __cdecl NvAPI_D3D11_CreateFastGeometryShaderExplicit(
    ID3D11Device* pDevice,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    const NvAPI_D3D11_CREATE_FASTGS_EXPLICIT_DESC* pCreateFastGSArgs,
    ID3D11GeometryShader** ppGeometryShader) {
    LOG_CALL();
    return NVAPI_NOT_SUPPORTED;
}

NvAPI_Status __cdecl NvAPI_D3D11_CreatePixelShaderEx_2(
    ID3D11Device* pDevice,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    const NvAPI_D3D11_CREATE_PIXEL_SHADER_EX* pCreatePixelShaderExArgs,
    ID3D11PixelShader** ppPixelShader) {
    LOG_CALL_DETAIL();
    if (!pDevice || !pCreatePixelShaderExArgs || !ppPixelShader) {
        return NVAPI_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> storage;
    const auto [bytecode, length] = ProcessShader("ps", pShaderBytecode, BytecodeLength, pCreatePixelShaderExArgs, storage);
    return SUCCEEDED(pDevice->CreatePixelShader(bytecode, length, pClassLinkage, ppPixelShader))
               ? NVAPI_OK
               : NVAPI_ERROR;
}

// The only export of nvapi64.dll.
extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(NvU32 id) {
    switch (id) {
#define RESOLVE(name)                                                                                                  \
    case kId_##name:                                                                                                   \
        return reinterpret_cast<void*>(&name)
        RESOLVE(NvAPI_Initialize);
        RESOLVE(NvAPI_Unload);
        RESOLVE(NvAPI_D3D_RegisterDevice);
        RESOLVE(NvAPI_D3D11_IsNvShaderExtnOpCodeSupported);
        RESOLVE(NvAPI_D3D_QueryMultiViewSupport);
        RESOLVE(NvAPI_D3D_QueryModifiedWSupport);
        RESOLVE(NvAPI_D3D_QuerySinglePassStereoSupport);
        RESOLVE(NvAPI_D3D_SetMultiViewMode);
        RESOLVE(NvAPI_D3D_SetModifiedWMode);
        RESOLVE(NvAPI_D3D_SetSinglePassStereoMode);
        RESOLVE(NvAPI_D3D11_CreateVertexShaderEx);
        RESOLVE(NvAPI_D3D11_CreateHullShaderEx);
        RESOLVE(NvAPI_D3D11_CreateDomainShaderEx);
        RESOLVE(NvAPI_D3D11_CreateGeometryShaderEx_2);
        RESOLVE(NvAPI_D3D11_CreateFastGeometryShaderExplicit);
        RESOLVE(NvAPI_D3D11_CreatePixelShaderEx_2);
        RESOLVE(NvAPI_GetErrorMessage);
        RESOLVE(NvAPI_DRS_CreateSession);
        RESOLVE(NvAPI_DRS_DestroySession);
        RESOLVE(NvAPI_DRS_LoadSettings);
        RESOLVE(NvAPI_DRS_GetBaseProfile);
        RESOLVE(NvAPI_DRS_GetCurrentGlobalProfile);
        RESOLVE(NvAPI_DRS_FindProfileByName);
        RESOLVE(NvAPI_DRS_FindApplicationByName);
        RESOLVE(NvAPI_DRS_GetSetting);
#undef RESOLVE
    }

    if (!IsKnownUnimplemented(id)) {
        logging::Log("nvapi_QueryInterface(0x%08x): unimplemented\n", id);
    }
    return nullptr;
}
