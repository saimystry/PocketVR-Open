// SPDX-License-Identifier: Apache-2.0
#include "d3d.hpp"
#include <d3d11_4.h>
#include <cstring>

namespace pocketvr::driver {
Gpu::Gpu(bool warp) {
    Com<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory creation failed");
    Com<IDXGIAdapter1> selected;
    if (!warp)
        for (UINT index = 0;; ++index) {
            Com<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            check(result, "DXGI adapter enumeration failed");
            DXGI_ADAPTER_DESC1 description{};
            check(candidate->GetDesc1(&description), "DXGI adapter description failed");
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (!selected || description.DedicatedVideoMemory > adapter.DedicatedVideoMemory) {
                selected = candidate;
                adapter = description;
            }
        }
    if (!warp && !selected)
        throw std::runtime_error("No D3D11 hardware adapter available");
    Com<ID3D11Device> base;
    D3D_FEATURE_LEVEL actual{};
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const auto type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN;
    check(D3D11CreateDevice(selected.Get(), type, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                (warp ? 0 : D3D11_CREATE_DEVICE_VIDEO_SUPPORT),
                            levels, 2, D3D11_SDK_VERSION, &base, &actual, &context),
          "D3D11 device creation failed");
    check(base.As(&device), "D3D11.1 device interface unavailable");
    if (warp) {
        Com<IDXGIDevice> dxgi_device;
        Com<IDXGIAdapter> actual_adapter;
        Com<IDXGIAdapter1> adapter1;
        check(device.As(&dxgi_device), "WARP DXGI device unavailable");
        check(dxgi_device->GetAdapter(&actual_adapter), "WARP adapter query failed");
        check(actual_adapter.As(&adapter1), "WARP adapter description interface unavailable");
        check(adapter1->GetDesc1(&adapter), "WARP adapter description failed");
    }
    Com<ID3D11Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
}
uint64_t Gpu::luid() const {
    uint64_t value{};
    static_assert(sizeof(value) == sizeof(adapter.AdapterLuid));
    std::memcpy(&value, &adapter.AdapterLuid, sizeof(value));
    return value;
}
} // namespace pocketvr::driver
