// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openvr_driver.h>
#include "state.hpp"
#include "d3d.hpp"
#include "realtime.hpp"
#include <functional>
#include <unordered_map>

namespace pocketvr::driver {
struct GpuFrame {
    uint8_t slot{};
    ID3D11Texture2D *texture{};
    wire::VideoHeader metadata{};
    uint64_t submit_ns{};
};
class Compositor final : public vr::IVRDriverDirectModeComponent {
  public:
    using Sink = std::function<bool(const GpuFrame &)>;
    Compositor(Gpu &gpu, State &state, uint32_t eye_width = 1080, uint32_t height = 1080);
    ~Compositor() = default;
    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *description,
                              SwapTextureSet_t *output) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override;
    void DestroyAllSwapTextureSets(uint32_t pid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2],
                                    uint32_t (*indices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&eyes)[2]) override;
    void Present(vr::SharedTextureHandle_t sync) override;
    void PostPresent(const Throttling_t *) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *timing) override;
    void sink(Sink callback); // Change only when streaming is stopped.
    void release(uint8_t slot) noexcept;
    size_t texture_sets() const;
    nlohmann::json diagnostics() const;
    void request_capture() { capture_requested_.store(true); }

  private:
    struct Texture {
        Com<ID3D11Texture2D> texture;
        Com<ID3D11ShaderResourceView> view;
        HANDLE handle{};
        bool manual_srgb{};
    };
    struct TextureSet {
        uint32_t pid{}, index{};
        uint64_t bytes{};
        std::array<Texture, 3> textures;
    };
    struct Layer {
        std::array<Com<ID3D11ShaderResourceView>, 2> views;
        std::array<vr::VRTextureBounds_t, 2> bounds{};
        std::array<bool, 2> manual_srgb{};
        vr::HmdMatrix34_t pose{};
        double tracking_origin_yaw{};
        uint64_t submitted_ns{};
    };
    struct Output {
        Com<ID3D11Texture2D> linear_target, encoded;
        Com<ID3D11RenderTargetView> view;
        std::atomic<bool> busy{};
    };
    Gpu &gpu_;
    State &state_;
    uint32_t width_, height_;
    mutable std::mutex mutex_;
    std::unordered_map<vr::SharedTextureHandle_t, std::shared_ptr<TextureSet>> sets_;
    std::unordered_map<vr::SharedTextureHandle_t, Com<IDXGIKeyedMutex>> syncs_;
    std::array<Layer, 32> layers_{};
    size_t layer_count_{};
    std::array<SubmitLayerPerEye_t, 2> last_submitted_{};
    std::array<Output, 3> outputs_;
    Com<ID3D11Texture2D> test_pattern_;
    Sink sink_;
    Com<ID3D11DeviceContext> render_context_;
    Com<ID3D11VertexShader> vertex_;
    Com<ID3D11PixelShader> pixel_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11BlendState> blend_;
    Com<ID3D11RasterizerState> raster_;
    std::atomic<uint32_t> presented_{}, dropped_{};
    PreciseTimer post_present_timer_;
    std::atomic<bool> capture_requested_{};
    void render(std::span<const Layer> layers, Output &output);
};
} // namespace pocketvr::driver
