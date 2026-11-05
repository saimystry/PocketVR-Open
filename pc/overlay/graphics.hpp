// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>

namespace pocketvr::dashboard {
template<class T> using Com = Microsoft::WRL::ComPtr<T>;
void check(HRESULT result, const char *operation);
class Graphics {
  public:
    Graphics(int adapter, bool warp, HWND preview = nullptr);
    ~Graphics();
    void begin(float elapsed);
    void render();
    void capture(const std::filesystem::path &path); // Test-only readback; never used in VR.
    ID3D11Texture2D *texture() const { return texture_.Get(); }
  private:
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11Texture2D> texture_, back_;
    Com<ID3D11RenderTargetView> target_;
    Com<IDXGISwapChain> swap_;
    bool initialized_{}, platform_{};
};
} // namespace pocketvr::dashboard
