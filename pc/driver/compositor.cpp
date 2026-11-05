// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/profiles.hpp>
#include "compositor.hpp"
#include "boundary.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <unordered_set>
#include <fstream>

namespace pocketvr::driver {
namespace {
void capture_texture(Gpu &gpu, ID3D11Texture2D *source, const std::filesystem::path &path) {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                      description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.BindFlags = description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(gpu.device->CreateTexture2D(&description, nullptr, &staging), "Capture staging failed");
    gpu.context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Capture readback failed");
    struct Unmap {
        Gpu &gpu; ID3D11Texture2D *texture;
        ~Unmap() { gpu.context->Unmap(texture, 0); }
    } unmap{gpu, staging.Get()};
    const auto bytes = description.Width * description.Height * 4;
    BITMAPFILEHEADER header{0x4d42, 54 + bytes, 0, 0, 54};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(description.Width);
    info.biHeight = -static_cast<LONG>(description.Height);
    info.biPlanes = 1; info.biBitCount = 32; info.biSizeImage = bytes;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    file.write(reinterpret_cast<const char *>(&info), sizeof(info));
    std::vector<uint8_t> row(description.Width * 4);
    for (UINT y = 0; y < description.Height; ++y) {
        std::memcpy(row.data(), static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch, row.size());
        if (rgba) for (size_t x = 0; x < row.size(); x += 4) std::swap(row[x], row[x + 2]);
        file.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    if (!file) throw std::runtime_error("Cannot save compositor capture");
}
constexpr char shader[] = R"(
cbuffer Layer : register(b0) { float4 bounds; uint opaque; uint manual_srgb; float2 pad; };
Texture2D source : register(t0); SamplerState filter : register(s0);
struct Vertex { float4 position:SV_Position;float2 uv:TEXCOORD0; };
Vertex vs(uint id:SV_VertexID) { Vertex o;float2 p=float2(id==1?3:-1,id==2?3:-1);o.position=float4(p,0,1);o.uv=float2((p.x+1)*0.5,(1-p.y)*0.5);return o; }
float3 decode_srgb(float3 c) { return lerp(c/12.92,pow((c+0.055)/1.055,2.4),step(0.04045,c)); }
float4 ps(Vertex i):SV_Target { float4 c=source.Sample(filter,lerp(bounds.xy,bounds.zw,i.uv));if(manual_srgb)c.rgb=decode_srgb(c.rgb);if(opaque)c.a=1;return c; }
)";
struct Constants {
    float bounds[4];
    uint32_t opaque, manual_srgb;
    float pad[2];
};
static Com<ID3DBlob> compile(const char *entry, const char *target) {
    Com<ID3DBlob> code, error;
    const auto result =
        D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, entry, target,
                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (FAILED(result))
        throw std::runtime_error(error ? static_cast<const char *>(error->GetBufferPointer())
                                       : "Shader compilation failed");
    return code;
}
static DXGI_FORMAT srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return format;
    default:
        throw std::invalid_argument("Unsupported direct-mode texture format");
    }
}
static Quat orientation(const vr::HmdMatrix34_t &matrix) {
    const auto &m = matrix.m;
    for (const auto &row : m)
        for (float v : row)
            if (!std::isfinite(v))
                throw std::invalid_argument("Non-finite rendered pose");
    const Vec3 x{m[0][0], m[1][0], m[2][0]}, y{m[0][1], m[1][1], m[2][1]},
        z{m[0][2], m[1][2], m[2][2]};
    if (std::abs(x.norm() - 1) > 0.01 || std::abs(y.norm() - 1) > 0.01 ||
        std::abs(z.norm() - 1) > 0.01 || std::abs(x.dot(y)) > 0.01 || std::abs(x.dot(z)) > 0.01 ||
        std::abs(y.dot(z)) > 0.01 || x.cross(y).dot(z) < 0.99)
        throw std::invalid_argument("Rendered pose is not a rigid transform");
    const double trace = m[0][0] + m[1][1] + m[2][2];
    Quat q;
    if (trace > 0) {
        const auto s = std::sqrt(trace + 1) * 2;
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const auto s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const auto s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const auto s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s};
    }
    return q.unit();
}
} // namespace
Compositor::Compositor(Gpu &gpu, State &state, uint32_t eye_width, uint32_t height)
    : gpu_(gpu), state_(state), width_(eye_width), height_(height) {
    if (width_ == 0 || height_ == 0 || width_ > 4096 || height_ > 4096)
        throw std::invalid_argument("Compositor dimensions");
    check(gpu_.device->CreateDeferredContext(0, &render_context_),
          "Deferred compositor context failed");
    const auto vertex = compile("vs", "vs_5_0"), pixel = compile("ps", "ps_5_0");
    check(gpu_.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
                                          nullptr, &vertex_),
          "Vertex shader creation failed");
    check(gpu_.device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr,
                                         &pixel_),
          "Pixel shader creation failed");
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(gpu_.device->CreateBuffer(&cb, nullptr, &constants_), "Compositor constants failed");
    D3D11_SAMPLER_DESC sample{};
    sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sample.MaxLOD = D3D11_FLOAT32_MAX;
    check(gpu_.device->CreateSamplerState(&sample, &sampler_), "Compositor sampler failed");
    D3D11_BLEND_DESC blend{};
    auto &target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check(gpu_.device->CreateBlendState(&blend, &blend_), "Compositor blend state failed");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    check(gpu_.device->CreateRasterizerState(&raster, &raster_), "Compositor raster state failed");
    for (auto &output : outputs_) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width_ * 2;
        description.Height = height_;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(gpu_.device->CreateTexture2D(&description, nullptr, &output.linear_target),
              "Compositor output allocation failed");
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        check(gpu_.device->CreateRenderTargetView(output.linear_target.Get(), &view, &output.view),
              "Compositor sRGB view failed");
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        check(gpu_.device->CreateTexture2D(&description, nullptr, &output.encoded),
              "Encoder texture allocation failed");
    }
}
void Compositor::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *description,
                                      SwapTextureSet_t *output) {
    if (output)
        *output = {};
    boundary("CreateSwapTextureSet", [&] {
        if (!description || !output || !description->nWidth || !description->nHeight ||
            description->nWidth > 4096 || description->nHeight > 4096 ||
            description->nSampleCount != 1)
            throw std::invalid_argument("Swap texture dimensions");
        const auto view_format = srv_format(static_cast<DXGI_FORMAT>(description->nFormat));
        auto set = std::make_shared<TextureSet>();
        set->pid = pid;
        set->bytes = static_cast<uint64_t>(description->nWidth) * description->nHeight * 4 * 3;
        std::unique_lock allocation_lock(mutex_);
        uint64_t allocated = 0;
        std::unordered_set<const TextureSet *> unique;
        for (const auto &item : sets_)
            if (unique.insert(item.second.get()).second)
                allocated += item.second->bytes;
        if (allocated + set->bytes > 512ULL * 1024 * 1024 || sets_.size() >= 96)
            throw std::runtime_error("Swap texture memory budget exceeded");
        for (auto &texture : set->textures) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = description->nWidth;
            desc.Height = description->nHeight;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = static_cast<DXGI_FORMAT>(description->nFormat);
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            check(gpu_.device->CreateTexture2D(&desc, nullptr, &texture.texture),
                  "Shared swap texture creation failed");
            Com<IDXGIResource> resource;
            check(texture.texture.As(&resource), "Shared resource interface failed");
            check(resource->GetSharedHandle(&texture.handle),
                  "Shared texture handle creation failed");
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = view_format;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            check(
                gpu_.device->CreateShaderResourceView(texture.texture.Get(), &view, &texture.view),
                "Shared texture view failed");
            texture.manual_srgb = view_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
                                  view_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
        // Legacy DXGI shared handles are not NT HANDLEs and must not be CloseHandle'd.
        for (size_t i = 0; i < 3; ++i) {
            const auto handle = static_cast<vr::SharedTextureHandle_t>(
                reinterpret_cast<uintptr_t>(set->textures[i].handle));
            sets_.emplace(handle, set);
            output->rSharedTextureHandles[i] = handle;
        }
        output->unTextureFlags = 0;
    });
}
void Compositor::DestroySwapTextureSet(vr::SharedTextureHandle_t handle) {
    boundary("DestroySwapTextureSet", [&] {
        std::lock_guard lock(mutex_);
        const auto found = sets_.find(handle);
        if (found == sets_.end())
            return;
        const auto set = found->second;
        for (const auto &texture : set->textures)
            sets_.erase(static_cast<vr::SharedTextureHandle_t>(
                reinterpret_cast<uintptr_t>(texture.handle)));
    });
}
void Compositor::DestroyAllSwapTextureSets(uint32_t pid) {
    boundary("DestroyAllSwapTextureSets", [&] {
        std::lock_guard lock(mutex_);
        std::erase_if(sets_, [&](const auto &item) { return item.second->pid == pid; });
        layer_count_ = 0;
        for (auto &layer : layers_)
            layer = {};
        syncs_.clear();
    });
}
void Compositor::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2],
                                            uint32_t (*indices)[2]) {
    boundary("GetNextSwapTextureSetIndex", [&] {
        if (!handles || !indices)
            return;
        std::lock_guard lock(mutex_);
        std::shared_ptr<TextureSet> previous;
        for (size_t eye = 0; eye < 2; ++eye) {
            auto found = sets_.find(handles[eye]);
            if (found == sets_.end()) {
                (*indices)[eye] = 0;
                continue;
            }
            auto &set = found->second;
            if (set != previous)
                set->index = (set->index + 1) % 3;
            (*indices)[eye] = set->index;
            previous = set;
        }
    });
}
void Compositor::SubmitLayer(const SubmitLayerPerEye_t (&eyes)[2]) {
    boundary("SubmitLayer", [&] {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || layer_count_ == layers_.size()) {
            ++dropped_;
            return;
        }
        Layer layer;
        if (layer_count_ == 0)
            std::copy(std::begin(eyes), std::end(eyes), last_submitted_.begin());
        for (size_t eye = 0; eye < 2; ++eye) {
            const auto found = sets_.find(eyes[eye].hTexture);
            if (found == sets_.end())
                throw std::invalid_argument("Unrecognized layer texture");
            const auto &bounds = eyes[eye].bounds;
            if (!std::isfinite(bounds.uMin) || !std::isfinite(bounds.uMax) ||
                !std::isfinite(bounds.vMin) || !std::isfinite(bounds.vMax) ||
                std::abs(bounds.uMin) > 8 || std::abs(bounds.uMax) > 8 ||
                std::abs(bounds.vMin) > 8 || std::abs(bounds.vMax) > 8)
                throw std::invalid_argument("Invalid layer bounds");
            for (const auto &texture : found->second->textures)
                if (static_cast<vr::SharedTextureHandle_t>(
                        reinterpret_cast<uintptr_t>(texture.handle)) == eyes[eye].hTexture) {
                    layer.views[eye] = texture.view;
                    layer.manual_srgb[eye] = texture.manual_srgb;
                }
            layer.bounds[eye] = bounds;
        }
        layer.pose = eyes[0].mHmdPose;
        layer.tracking_origin_yaw = state_.recenter_yaw.load();
        layer.submitted_ns = monotonic_ns();
        (void)orientation(layer.pose);
        layers_[layer_count_++] = std::move(layer);
    });
}
void Compositor::render(std::span<const Layer> layers, Output &output) {
    auto *context = render_context_.Get();
    constexpr float black[]{0, 0, 0, 1};
    context->ClearRenderTargetView(output.view.Get(), black);
    auto *view = output.view.Get();
    context->OMSetRenderTargets(1, &view, nullptr);
    context->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    context->RSSetState(raster_.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    auto *cb = constants_.Get();
    context->PSSetConstantBuffers(0, 1, &cb);
    auto *sampler = sampler_.Get();
    context->PSSetSamplers(0, 1, &sampler);
    for (size_t i = 0; i < layers.size(); ++i)
        for (size_t eye = 0; eye < 2; ++eye) {
            const auto &layer = layers[i];
            const auto &b = layer.bounds[eye];
            const Constants constants{{b.uMin, b.vMin, b.uMax, b.vMax},
                                      i == 0 ? 1U : 0U,
                                      layer.manual_srgb[eye] ? 1U : 0U,
                                      {0, 0}};
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                  "Compositor constants map failed");
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(constants_.Get(), 0);
            const D3D11_VIEWPORT viewport{
                static_cast<float>(eye * width_), 0, static_cast<float>(width_),
                static_cast<float>(height_),      0, 1};
            context->RSSetViewports(1, &viewport);
            auto *source = layer.views[eye].Get();
            context->PSSetShaderResources(0, 1, &source);
            context->Draw(3, 0);
        }
    ID3D11ShaderResourceView *empty = nullptr;
    context->PSSetShaderResources(0, 1, &empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(output.encoded.Get(), output.linear_target.Get());
    if (state_.test_pattern.load()) {
        if (!test_pattern_) {
            const auto pixels = test_pattern_pixels(width_ * 2, height_);
            D3D11_TEXTURE2D_DESC description{};
            output.encoded->GetDesc(&description);
            D3D11_SUBRESOURCE_DATA data{pixels.data(), width_ * 2 * 4, 0};
            check(gpu_.device->CreateTexture2D(&description, &data, &test_pattern_), "Test picture allocation failed");
        }
        context->CopyResource(output.encoded.Get(), test_pattern_.Get());
    }
    Com<ID3D11CommandList> commands;
    check(context->FinishCommandList(FALSE, &commands), "Compositor command recording failed");
    gpu_.context->ExecuteCommandList(commands.Get(), TRUE);
}
void Compositor::Present(vr::SharedTextureHandle_t sync) {
    boundary("Present", [&] {
        if (FAILED(gpu_.device->GetDeviceRemovedReason())) {
            state_.gpu_lost.store(true);
            return;
        }
        std::array<Layer, 32> layers;
        size_t count{};
        Com<IDXGIKeyedMutex> keyed;
        Sink sink;
        {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                ++dropped_;
                return;
            }
            count = layer_count_;
            std::copy_n(layers_.begin(), count, layers.begin());
            layer_count_ = 0;
            for (auto &layer : layers_)
                layer = {};
            sink = sink_;
            if (sync) {
                auto found = syncs_.find(sync);
                if (found == syncs_.end()) {
                    Com<ID3D11Texture2D> texture;
                    const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sync));
                    auto result = gpu_.device->OpenSharedResource1(handle, IID_PPV_ARGS(&texture));
                    if (FAILED(result))
                        result = gpu_.device->OpenSharedResource(handle, IID_PPV_ARGS(&texture));
                    check(result, "Open compositor sync texture failed");
                    check(texture.As(&keyed), "Compositor sync texture has no keyed mutex");
                    if (syncs_.size() >= 64)
                        syncs_.clear();
                    syncs_.emplace(sync, keyed);
                } else
                    keyed = found->second;
            }
        }
        if (!count)
            return;
        if (!sink)
            return;
        if (!keyed) {
            ++dropped_;
            throw std::invalid_argument("Missing compositor synchronization texture");
        }
        struct SyncGuard {
            Com<IDXGIKeyedMutex> mutex;
            ~SyncGuard() {
                if (mutex)
                    mutex->ReleaseSync(0);
            }
        } guard;
        if (keyed) {
            if (keyed->AcquireSync(0, 5) != S_OK) {
                ++dropped_;
                return;
            }
            guard.mutex = keyed;
        }
        size_t index = outputs_.size();
        for (size_t i = 0; i < outputs_.size(); ++i) {
            bool expected = false;
            if (outputs_[i].busy.compare_exchange_strong(expected, true)) {
                index = i;
                break;
            }
        }
        if (index == outputs_.size()) {
            ++dropped_;
            return;
        }
        auto &output = outputs_[index];
        try {
            render(std::span(layers).first(count), output);
            gpu_.context->Flush();
            if (capture_requested_.exchange(false)) {
                const auto directory = win::appdata(true) / "logs";
                std::filesystem::create_directories(directory);
                for (size_t eye = 0; eye < 2; ++eye) {
                    Com<ID3D11Resource> resource;
                    layers[0].views[eye]->GetResource(&resource);
                    Com<ID3D11Texture2D> texture;
                    check(resource.As(&texture), "Capture texture query failed");
                    capture_texture(gpu_, texture.Get(), directory /
                                    (eye ? "stereo-source-right.bmp" : "stereo-source-left.bmp"));
                }
                capture_texture(gpu_, output.encoded.Get(), directory / "stereo-composed.bmp");
            }
            GpuFrame frame;
            frame.slot = static_cast<uint8_t>(index);
            frame.texture = output.encoded.Get();
            frame.submit_ns = monotonic_ns();
            frame.metadata.frame_id = state_.frames.fetch_add(1) + 1;
            // OpenVR supplies a rendered pose, not the application's render
            // start timestamp. Record layer arrival, never relabel GPU flush
            // time as application render time.
            frame.metadata.render_ns = layers[0].submitted_ns;
            frame.metadata.projection_id = state_.viewer_geometry()->id;
            // SteamVR renders in the driver's recentered coordinates. The phone
            // predicts in sensor coordinates, so remove the driver's origin
            // rotation from the exact submitted pose before transporting it.
            // Snapshot the origin with the layer; a later recenter must not
            // relabel an already queued layer.
            frame.metadata.render_pose.orientation =
                (Quat::exponential({0, -layers[0].tracking_origin_yaw, 0}) *
                 orientation(layers[0].pose)).unit();
            frame.metadata.render_pose.position = {layers[0].pose.m[0][3], layers[0].pose.m[1][3],
                                                   layers[0].pose.m[2][3]};
            // SubmitLayer does not supply angular velocity. A later head sample
            // would belong to a different pose and must not be substituted.
            frame.metadata.render_pose.angular_velocity = {};
            state_.record_frame(frame.metadata);
            ++presented_;
            if (!sink || !sink(frame))
                output.busy.store(false);
        } catch (...) {
            output.busy.store(false);
            throw;
        }
    });
}
void Compositor::PostPresent(const Throttling_t *) {
    boundary("PostPresent", [&] {
        const auto target = state_.running_start_ns.load();
        const auto now = monotonic_ns();
        if (target > now && target - now < 30'000'000)
            post_present_timer_.sleep(target - now);
    });
}
void Compositor::GetFrameTiming(vr::DriverDirectMode_FrameTiming *timing) {
    if (!timing || timing->m_nSize < sizeof(*timing))
        return;
    timing->m_nNumFramePresents = presented_.exchange(0);
    timing->m_nNumMisPresented = 0;
    timing->m_nNumDroppedFrames = dropped_.exchange(0);
    timing->m_nReprojectionFlags = 0;
}
void Compositor::sink(Sink callback) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(callback);
}
void Compositor::release(uint8_t slot) noexcept {
    if (slot < outputs_.size())
        outputs_[slot].busy.store(false);
}
size_t Compositor::texture_sets() const {
    std::lock_guard lock(mutex_);
    return sets_.size() / 3;
}
nlohmann::json Compositor::diagnostics() const {
    std::lock_guard lock(mutex_);
    auto result = nlohmann::json::array();
    for (const auto &eye : last_submitted_) {
        nlohmann::json projection = nlohmann::json::array();
        for (const auto &row : eye.mProjection.m)
            for (const auto value : row) projection.push_back(value);
        nlohmann::json pose = nlohmann::json::array();
        for (const auto &row : eye.mHmdPose.m)
            for (const auto value : row) pose.push_back(value);
        result.push_back({{"bounds", {eye.bounds.uMin, eye.bounds.vMin, eye.bounds.uMax, eye.bounds.vMax}},
                          {"projection", projection},
                          {"pose", pose},
                          {"same_texture_as_left", eye.hTexture == last_submitted_[0].hTexture}});
    }
    const auto sensor = state_.head.read().pose.orientation;
    return {{"ok", true}, {"eyes", result},
            {"tracking_origin_yaw_rad", state_.recenter_yaw.load()},
            {"sensor_orientation", {sensor.x, sensor.y, sensor.z, sensor.w}}};
}
} // namespace pocketvr::driver
