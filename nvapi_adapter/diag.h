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

#pragma once

// Diagnostics of the 4th view requested by iRacing's triple screen SMP. Branch diag-4th-view only: never merged.
namespace diag {

    // Hooks RSSetViewports() and RSSetScissorRects() (once), to log the viewports used with view broadcasting.
    void OnDeviceCreated(ID3D11DeviceContext* immediateContext);

    // Logs each distinct set of multi-view parameters.
    void OnMultiViewMode(const NV_MULTIVIEW_PARAMS* params);

    // Logs the pipeline state when view broadcasting is enabled, and returns the viewport mask to use: the requested
    // mask, restricted by the hexadecimal mask in %LOCALAPPDATA%\AMD-MVR-Adapter\vpmask_override.txt if that file
    // exists. The file is read at most once per second, so it can be changed while the game runs.
    unsigned long long OnViewBroadcast(IUnknown* deviceOrContext, unsigned long long vpMask);

} // namespace diag
