// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <stdexcept>
#include <format>

namespace pocketvr::driver {
template <class T> using Com = Microsoft::WRL::ComPtr<T>;
inline void check(HRESULT result, const char *message) {
    if (FAILED(result))
        throw std::runtime_error(
            std::format("{} (HRESULT 0x{:08x})", message, static_cast<uint32_t>(result)));
}
class Gpu {
  public:
    explicit Gpu(bool warp = false);
    Com<ID3D11Device1> device;
    Com<ID3D11DeviceContext> context;
    DXGI_ADAPTER_DESC1 adapter{};
    uint64_t luid() const;
};
} // namespace pocketvr::driver
