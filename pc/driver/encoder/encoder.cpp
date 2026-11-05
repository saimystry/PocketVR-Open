// SPDX-License-Identifier: Apache-2.0
#include "encoder.hpp"
#include <d3d11_1.h>
#include <spdlog/spdlog.h>

namespace pocketvr::driver {
Nv12Converter::Nv12Converter(Gpu &gpu, uint32_t width, uint32_t height)
    : gpu_(gpu), width_(width), height_(height) {
    if (!width || !height || (width & 1) || (height & 1) || width > 4096 || height > 4096)
        throw std::invalid_argument("NV12 converter dimensions");
    check(gpu.device.As(&video_), "D3D11 video device unavailable");
    check(gpu.context.As(&context_), "D3D11 video context unavailable");
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
    description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    description.InputWidth = description.OutputWidth = width;
    description.InputHeight = description.OutputHeight = height;
    description.InputFrameRate = {60, 1};
    description.OutputFrameRate = {60, 1};
    description.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check(video_->CreateVideoProcessorEnumerator(&description, &enumerator_),
          "Video processor enumeration failed");
    UINT input_support{}, output_support{};
    check(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &input_support),
          "Video processor BGRA capability query failed");
    check(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &output_support),
          "Video processor NV12 capability query failed");
    if (!(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw std::runtime_error("GPU video processor cannot convert BGRA to NV12");
    check(video_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_),
          "Video processor creation failed");
    Com<ID3D11VideoContext1> context1;
    if (SUCCEEDED(context_.As(&context1))) {
        context1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0,
                                                     DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        context1->VideoProcessorSetOutputColorSpace1(processor_.Get(),
                                                     DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{}, output{};
        input.RGB_Range = 0;
        input.Nominal_Range = 2;
        output.YCbCr_Matrix = 1;
        output.Nominal_Range = 1;
        context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input);
        context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output);
    }
    const RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &rect);
    context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &rect);
    context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &rect);
    context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
}
Com<ID3D11Texture2D> Nv12Converter::create_output() {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width_;
    description.Height = height_;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> output;
    check(gpu_.device->CreateTexture2D(&description, nullptr, &output),
          "NV12 GPU surface creation failed");
    return output;
}
void Nv12Converter::convert(ID3D11Texture2D *source, ID3D11Texture2D *destination) {
    auto input = input_views_.find(source);
    if (input == input_views_.end()) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description{};
        description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        Com<ID3D11VideoProcessorInputView> view;
        check(video_->CreateVideoProcessorInputView(source, enumerator_.Get(), &description, &view),
              "Video input view failed");
        if (input_views_.size() >= 16)
            input_views_.clear();
        input = input_views_.emplace(source, std::move(view)).first;
    }
    auto output = output_views_.find(destination);
    if (output == output_views_.end()) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description{};
        description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        Com<ID3D11VideoProcessorOutputView> view;
        check(video_->CreateVideoProcessorOutputView(destination, enumerator_.Get(), &description,
                                                     &view),
              "Video output view failed");
        if (output_views_.size() >= 16)
            output_views_.clear();
        output = output_views_.emplace(destination, std::move(view)).first;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input->second.Get();
    check(context_->VideoProcessorBlt(processor_.Get(), output->second.Get(), 0, 1, &stream),
          "GPU BGRA to NV12 conversion failed");
}
std::unique_ptr<IVideoEncoder> create_encoder(Gpu &gpu, const EncoderConfig &config,
                                              bool software_only) {
    check(gpu.device->GetDeviceRemovedReason(), "GPU device lost; restart SteamVR");
    if (!config.width || !config.height || (config.width & 1) || (config.height & 1) ||
        config.width > 4096 || config.height > 4096 ||
        (config.fps != 30 && config.fps != 45 && config.fps != 60) || config.bitrate < 1'000'000 ||
        config.bitrate > 150'000'000 || config.slices < 1 || config.slices > 4)
        throw std::invalid_argument("Encoder configuration");
    if (!software_only) {
        if (gpu.adapter.VendorId == 0x1002) {
            try {
                return create_amf_encoder(gpu, config);
            } catch (const std::exception &e) {
                spdlog::warn("AMF unavailable: {}", e.what());
            }
        }
        if (gpu.adapter.VendorId == 0x10DE) {
            try {
                return create_nvenc_encoder(gpu, config);
            } catch (const std::exception &e) {
                spdlog::warn("NVENC unavailable: {}", e.what());
            }
        }
        try {
            return create_mf_encoder(gpu, config, true);
        } catch (const std::exception &e) {
            spdlog::warn("Hardware MFT unavailable: {}", e.what());
        }
    }
    if (config.codec != Codec::h264)
        throw std::runtime_error(
            "No HEVC hardware encoder available; use H.264 or automatic codec selection");
    if (!software_only && (config.width > 1280 || config.height > 720))
        throw std::runtime_error("No hardware encoder is available for this full-resolution stream; restart SteamVR/update the GPU driver. Explicit software diagnostics remain available.");
    return create_mf_encoder(gpu, config, false);
}
} // namespace pocketvr::driver
