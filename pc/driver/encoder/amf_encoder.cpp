// SPDX-License-Identifier: Apache-2.0
#include "encoder.hpp"
#include <core/Factory.h>
#include <core/Buffer.h>
#include <components/VideoEncoderVCE.h>
#include <components/VideoEncoderHEVC.h>
#include <spdlog/spdlog.h>
#include <bit>
#include <array>
#include <pocketvr/transport.hpp>

namespace pocketvr::driver {
namespace {
class BgraInitError final : public std::runtime_error {
  public:
    explicit BgraInitError(const std::string &message) : std::runtime_error(message) {}
};
void amf_check(AMF_RESULT result, const char *message) {
    if (result != AMF_OK)
        throw std::runtime_error(std::string(message) + " (AMF " + std::to_string(result) + ")");
}
class AmfEncoder final : public IVideoEncoder {
    struct Impl {
        HMODULE module{};
        amf::AMFFactory *factory{};
        amf::AMFContextPtr context;
        amf::AMFComponentPtr component;
        EncoderConfig config;
        std::unique_ptr<Nv12Converter> converter;
        struct Pending {
            uint64_t id{}, start{};
            Com<ID3D11Texture2D> texture;
            bool active{};
        };
        std::array<Pending, 3> pending;
        uint32_t bitrate_limit{UINT32_MAX};
        int64_t period{};
        bool bgra{};
        ~Impl() {
            if (component) {
                component->Terminate();
                component = nullptr;
            }
            if (context) {
                context->Terminate();
                context = nullptr;
            }
            if (module)
                FreeLibrary(module);
        }
        template <class T>
        bool property(const wchar_t *key, const T &value, bool required = false) {
            const amf::AMFPropertyInfo *info = nullptr;
            const auto support = component->GetPropertyInfo(key, &info);
            const auto result = support == AMF_OK ? component->SetProperty(key, value) : support;
            if (result != AMF_OK) {
                if (required)
                    amf_check(result, "Required AMF property unavailable");
                std::string name;
                for (auto at = key; *at; ++at)
                    name.push_back(static_cast<char>(*at));
                spdlog::debug("AMF property {} unsupported; keeping runtime default: {}", name,
                              static_cast<int>(result));
                return false;
            }
            return true;
        }
        // About 1.2 frames, so one frame can run a little over its share without the
        // buffer adding latency. It must follow the bitrate: a fixed size starves the
        // encoder as bitrate rises and allows large spikes as it falls.
        amf_int64 vbv_bits(uint32_t bitrate) const {
            return static_cast<amf_int64>(static_cast<uint64_t>(bitrate) * 12 / (config.fps * 10));
        }
        // The RX 590 refuses anything above its limit, and refusing at startup would drop
        // the stream to a much slower encoder.
        void read_bitrate_limit(bool hevc) {
            const amf::AMFPropertyInfo *info = nullptr;
            if (component->GetPropertyInfo(hevc ? AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE
                                                : AMF_VIDEO_ENCODER_TARGET_BITRATE, &info) == AMF_OK &&
                info && info->maxValue.type == amf::AMF_VARIANT_INT64 && info->maxValue.int64Value > 0 &&
                info->maxValue.int64Value < UINT32_MAX)
                bitrate_limit = static_cast<uint32_t>(info->maxValue.int64Value);
            spdlog::info("AMF bitrate limit: {} bps", bitrate_limit);
        }
        // Picture quality options. Unsupported ones are skipped by property(), and the
        // constructor retries without them if the runtime then refuses to initialize.
        void quality_options(bool hevc) {
            // Spends more bits on flat areas such as sky, where blocks are most visible.
            const auto vbaq = property(hevc ? AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ
                                            : AMF_VIDEO_ENCODER_ENABLE_VBAQ, true);
            // A starting point, not a tuned value. The default allows 51, which is
            // badly blocky; hard scenes may now run over budget instead.
            constexpr amf_int64 max_qp = 42;
            bool capped{};
            if (hevc) {
                capped = property(AMF_VIDEO_ENCODER_HEVC_MAX_QP_I, max_qp);
                capped = property(AMF_VIDEO_ENCODER_HEVC_MAX_QP_P, max_qp) && capped;
            } else
                capped = property(AMF_VIDEO_ENCODER_MAX_QP, max_qp);
            // Matches the Media Foundation path and the NV12 converter: BT.709, studio
            // range. The stream then tells the phone how to convert it back.
            const auto profile = static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
            if (!bgra)
                property(hevc ? AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE
                              : AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, profile);
            bool color = property(hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE
                                       : AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, profile);
            color = property(hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC
                                  : AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                             static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709)) && color;
            color = property(hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES
                                  : AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
                             static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709)) && color;
            if (hevc)
                color = property(AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE,
                                 static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO)) && color;
            else
                color = property(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false) && color;
            spdlog::info("AMF quality options accepted: VBAQ {}, QP limit {}, colour {}", vbaq, capped, color);
        }
        void initialize(Gpu &gpu, const EncoderConfig &settings, bool force_nv12, bool quality) {
            config = settings;
            period = 10'000'000 / config.fps;
            module = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
                throw std::runtime_error("AMD AMF runtime not installed in System32");
            const auto symbol = GetProcAddress(module, AMF_INIT_FUNCTION_NAME);
            if (!symbol)
                throw std::runtime_error("AMFInit entry point unavailable");
            const auto initialize = std::bit_cast<AMFInit_Fn>(symbol);
            amf_check(initialize(AMF_FULL_VERSION, &factory), "AMF initialization failed");
            amf_check(factory->CreateContext(&context), "AMF context failed");
            amf_check(context->InitDX11(gpu.device.Get()), "AMF D3D11 interop failed");
            const bool hevc = config.codec == Codec::hevc;
            amf_check(factory->CreateComponent(context,
                                               hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC,
                                               &component),
                      "AMF codec unavailable");
            amf::AMFCapsPtr caps;
            amf_check(component->GetCaps(&caps), "AMF capability query failed");
            amf::AMFIOCapsPtr inputs;
            amf_check(caps->GetInputCaps(&inputs), "AMF input capabilities unavailable");
            for (amf_int32 i = 0; i < inputs->GetNumOfFormats(); ++i) {
                amf::AMF_SURFACE_FORMAT format{};
                amf_bool native{};
                if (inputs->GetFormatAt(i, &format, &native) == AMF_OK &&
                    format == amf::AMF_SURFACE_BGRA && !force_nv12)
                    bgra = true;
            }
            if (!bgra) {
                converter = std::make_unique<Nv12Converter>(gpu, config.width, config.height);
                for (auto &slot : pending)
                    slot.texture = converter->create_output();
            }
            if (hevc) {
                property(AMF_VIDEO_ENCODER_HEVC_USAGE,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY),
                         true);
                property(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
                property(AMF_VIDEO_ENCODER_HEVC_PROFILE,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN));
                property(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                         static_cast<amf_int64>(
                             config.preset == "fast" ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED
                             : config.preset == "quality"
                                 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY
                                 : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED));
                property(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
                property(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                         AMFConstructSize(static_cast<amf_int32>(config.width),
                                          static_cast<amf_int32>(config.height)),
                         true);
                property(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(config.fps, 1), true);
                property(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_bits(config.bitrate));
                property(AMF_VIDEO_ENCODER_HEVC_INITIAL_VBV_BUFFER_FULLNESS,
                         static_cast<amf_int64>(41));
                property(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, true);
                property(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
                property(AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES, static_cast<amf_int64>(1));
                property(AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, false);
                property(AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE, false);
                property(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, static_cast<amf_int64>(1'000'000));
                property(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, static_cast<amf_int64>(1));
                property(AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME,
                         static_cast<amf_int64>(config.slices));
            } else {
                property(AMF_VIDEO_ENCODER_USAGE,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY), true);
                // AMD AMF #410: Polaris runtimes can reject ULL when the
                // preencode property cannot be queried. Set usage first, then
                // use AMD's compatibility workaround before all other options.
                amf_int64 preencode{};
                if (component->GetProperty(AMF_VIDEO_ENCODER_PREENCODE_ENABLE, &preencode) != AMF_OK) {
                    spdlog::warn("AMF ultra-low-latency preset unavailable; applying AMD legacy usage workaround");
                    property(AMF_VIDEO_ENCODER_USAGE,
                             static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_TRANSCODING), true);
                }
                property(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
                if (!property(AMF_VIDEO_ENCODER_PROFILE,
                              static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH)))
                    property(AMF_VIDEO_ENCODER_PROFILE,
                             static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_MAIN));
                property(AMF_VIDEO_ENCODER_PROFILE_LEVEL,
                         static_cast<amf_int64>(AMF_H264_LEVEL__5_1));
                property(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                         static_cast<amf_int64>(config.preset == "fast"
                                                    ? AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED
                                                : config.preset == "quality"
                                                    ? AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY
                                                    : AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED));
                property(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
                property(AMF_VIDEO_ENCODER_FRAMESIZE,
                         AMFConstructSize(static_cast<amf_int32>(config.width),
                                          static_cast<amf_int32>(config.height)),
                         true);
                property(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(config.fps, 1), true);
                property(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_bits(config.bitrate));
                property(AMF_VIDEO_ENCODER_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(41));
                property(AMF_VIDEO_ENCODER_ENFORCE_HRD, true);
                property(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
                property(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
                property(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, static_cast<amf_int64>(1));
                property(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, false);
                property(AMF_VIDEO_ENCODER_PREENCODE_ENABLE,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_PREENCODE_DISABLED));
                property(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(1'000'000));
                property(AMF_VIDEO_ENCODER_SLICES_PER_FRAME, static_cast<amf_int64>(config.slices));
                property(AMF_VIDEO_ENCODER_CABAC_ENABLE,
                         static_cast<amf_int64>(AMF_VIDEO_ENCODER_CABAC));
            }
            read_bitrate_limit(hevc);
            const auto bitrate = std::min(config.bitrate, bitrate_limit);
            property(hevc ? AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE
                          : AMF_VIDEO_ENCODER_TARGET_BITRATE,
                     static_cast<amf_int64>(bitrate), true);
            property(hevc ? AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE : AMF_VIDEO_ENCODER_PEAK_BITRATE,
                     static_cast<amf_int64>(bitrate));
            property(hevc ? AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE : AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
                     vbv_bits(bitrate));
            if (quality)
                quality_options(hevc);
            const auto result = component->Init(
                bgra ? amf::AMF_SURFACE_BGRA : amf::AMF_SURFACE_NV12,
                static_cast<amf_int32>(config.width), static_cast<amf_int32>(config.height));
            if (result != AMF_OK && bgra)
                throw BgraInitError("AMF BGRA initialization failed (AMF " +
                                    std::to_string(result) + ")");
            amf_check(result, "AMF NV12 encoder initialization failed");
        }
    };
    std::unique_ptr<Impl> impl_;

  public:
    AmfEncoder(Gpu &gpu, const EncoderConfig &settings) {
        const auto start = [&](bool quality) {
            impl_ = std::make_unique<Impl>();
            try {
                impl_->initialize(gpu, settings, false, quality);
            } catch (const BgraInitError &error) {
                spdlog::warn("{}; retrying a fresh AMF context with GPU NV12 input", error.what());
                impl_.reset();
                impl_ = std::make_unique<Impl>();
                impl_->initialize(gpu, settings, true, quality);
            }
        };
        // Older runtimes such as Polaris can list an option and still fail at Init.
        // AMF without these options beats falling back to a slower encoder.
        try {
            start(true);
        } catch (const std::exception &error) {
            spdlog::warn("{}; retrying AMF without the quality options", error.what());
            impl_.reset();
            start(false);
        }
    }
    bool submit(ID3D11Texture2D *texture, uint64_t id, bool force) override {
        auto &p = *impl_;
        auto slot = std::find_if(p.pending.begin(), p.pending.end(),
                                 [](const auto &item) { return !item.active; });
        if (slot == p.pending.end() || !texture || id > static_cast<uint64_t>(INT64_MAX / p.period))
            return false;
        const auto started = monotonic_ns();
        if (p.converter)
            p.converter->convert(texture, slot->texture.Get());
        amf::AMFSurfacePtr surface;
        amf_check(p.context->CreateSurfaceFromDX11Native(
                      p.converter ? slot->texture.Get() : texture, &surface, nullptr),
                  "AMF surface wrapping failed");
        surface->SetPts(static_cast<amf_pts>(id) * p.period);
        surface->SetDuration(p.period);
        if (force) {
            amf_check(surface->SetProperty(
                          p.config.codec == Codec::hevc ? AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE
                                                        : AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                          p.config.codec == Codec::hevc
                              ? static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR)
                              : static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR)),
                      "AMF IDR request failed");
            if (p.config.codec == Codec::h264) {
                surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
                surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
            } else
                surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        }
        const auto status = p.component->SubmitInput(surface);
        if (status == AMF_INPUT_FULL)
            return false;
        amf_check(status, "AMF submit failed");
        slot->id = id;
        slot->start = started;
        slot->active = true;
        return true;
    }
    std::optional<EncodedFrame> poll() override {
        auto &p = *impl_;
        amf::AMFDataPtr data;
        const auto status = p.component->QueryOutput(&data);
        if (status == AMF_REPEAT || status == AMF_EOF)
            return {};
        amf_check(status, "AMF query failed");
        if (!data)
            return {};
        amf::AMFBufferPtr buffer(data);
        if (!buffer || buffer->GetSize() > 8 * 1024 * 1024)
            throw std::runtime_error("Invalid AMF bitstream output");
        if (buffer->GetMemoryType() != amf::AMF_MEMORY_HOST)
            amf_check(buffer->Convert(amf::AMF_MEMORY_HOST),
                      "AMF compressed buffer host transfer failed");
        const auto id = static_cast<uint64_t>(data->GetPts() / p.period);
        auto slot = std::find_if(p.pending.begin(), p.pending.end(),
                                 [&](const auto &item) { return item.active && item.id == id; });
        if (slot == p.pending.end())
            throw std::runtime_error("AMF returned unrecognized frame timestamp");
        auto bytes = to_annex_b(
            std::span(static_cast<const uint8_t *>(buffer->GetNative()), buffer->GetSize()));
        if (!bytes)
            throw std::runtime_error("Malformed AMF output bitstream");
        EncodedFrame result{id, contains_idr(*bytes, p.config.codec), std::move(*bytes),
                            static_cast<double>(monotonic_ns() - slot->start) * 1e-6};
        slot->active = false;
        return result;
    }
    void bitrate(uint32_t value) override {
        auto &p = *impl_;
        p.property(p.config.codec == Codec::hevc ? AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE
                                                 : AMF_VIDEO_ENCODER_TARGET_BITRATE,
                   static_cast<amf_int64>(value), true);
        p.property(p.config.codec == Codec::hevc ? AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE
                                                 : AMF_VIDEO_ENCODER_PEAK_BITRATE,
                   static_cast<amf_int64>(value));
        p.property(p.config.codec == Codec::hevc ? AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE
                                                 : AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
                   p.vbv_bits(value));
    }
    uint32_t max_bitrate() const override {
        return impl_->bitrate_limit;
    }
    std::string name() const override {
        return "AMD AMF";
    }
    bool hardware() const override {
        return true;
    }
};
} // namespace
std::unique_ptr<IVideoEncoder> create_amf_encoder(Gpu &gpu, const EncoderConfig &config) {
    return std::make_unique<AmfEncoder>(gpu, config);
}
} // namespace pocketvr::driver
