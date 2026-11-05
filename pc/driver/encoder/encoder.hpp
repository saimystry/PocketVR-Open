// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "d3d.hpp"
#include <pocketvr/video.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace pocketvr::driver {
struct EncoderConfig {
    Codec codec{Codec::h264};
    uint32_t width{2160}, height{1080}, fps{60}, bitrate{60'000'000}, slices{2};
    std::string preset{"balanced"};
};
struct EncodedFrame {
    uint64_t frame_id{};
    bool idr{};
    wire::Bytes bytes;
    double encode_ms{};
};
class IVideoEncoder {
  public:
    virtual ~IVideoEncoder() = default;
    virtual bool submit(ID3D11Texture2D *texture, uint64_t frame_id, bool force_idr) = 0;
    virtual std::optional<EncodedFrame> poll() = 0;
    virtual void bitrate(uint32_t bits_per_second) = 0;
    // Highest bitrate this encoder accepts. Asking for more fails, so callers stay below it.
    virtual uint32_t max_bitrate() const { return UINT32_MAX; }
    virtual std::string name() const = 0;
    virtual bool hardware() const = 0;
};
std::unique_ptr<IVideoEncoder> create_encoder(Gpu &gpu, const EncoderConfig &config,
                                              bool software_only = false);
std::unique_ptr<IVideoEncoder> scale_encoder(Gpu &gpu, const EncoderConfig &config,
                                             std::unique_ptr<IVideoEncoder> encoder);
std::unique_ptr<IVideoEncoder> create_amf_encoder(Gpu &gpu, const EncoderConfig &config);
std::unique_ptr<IVideoEncoder> create_nvenc_encoder(Gpu &gpu, const EncoderConfig &config);
std::unique_ptr<IVideoEncoder> create_mf_encoder(Gpu &gpu, const EncoderConfig &config,
                                                 bool hardware);
class Nv12Converter {
  public:
    Nv12Converter(Gpu &gpu, uint32_t width, uint32_t height);
    Com<ID3D11Texture2D> create_output();
    void convert(ID3D11Texture2D *source, ID3D11Texture2D *destination);

  private:
    Gpu &gpu_;
    uint32_t width_, height_;
    Com<ID3D11VideoDevice> video_;
    Com<ID3D11VideoContext> context_;
    Com<ID3D11VideoProcessorEnumerator> enumerator_;
    Com<ID3D11VideoProcessor> processor_;
    std::unordered_map<ID3D11Texture2D *, Com<ID3D11VideoProcessorInputView>> input_views_;
    std::unordered_map<ID3D11Texture2D *, Com<ID3D11VideoProcessorOutputView>> output_views_;
};
} // namespace pocketvr::driver
