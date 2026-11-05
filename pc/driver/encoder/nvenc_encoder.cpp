// SPDX-License-Identifier: Apache-2.0
// Direct NVIDIA encoding. The runtime ships with the NVIDIA driver and is loaded on
// demand, so nothing is bundled and every failure falls back to Media Foundation.
#include "encoder.hpp"
#include <nvEncodeAPI.h>
#include <spdlog/spdlog.h>
#include <pocketvr/transport.hpp>
#include <array>

namespace pocketvr::driver {
namespace {
void nv_check(NVENCSTATUS status, const char *message) {
    if (status != NV_ENC_SUCCESS)
        throw std::runtime_error(std::string(message) + " (NVENC " + std::to_string(status) + ")");
}
GUID preset_guid(const std::string &preset) {
    if (preset == "fast")
        return NV_ENC_PRESET_P1_GUID;
    return preset == "quality" ? NV_ENC_PRESET_P4_GUID : NV_ENC_PRESET_P2_GUID;
}
class NvencEncoder final : public IVideoEncoder {
    struct Slot {
        Com<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered{};
        NV_ENC_INPUT_PTR mapped{};
        NV_ENC_OUTPUT_PTR bitstream{};
        uint64_t id{}, start{};
        bool active{};
    };
    Gpu &gpu_;
    EncoderConfig config_;
    HMODULE module_{};
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void *encoder_{};
    NV_ENC_CONFIG encode_config_{};
    NV_ENC_INITIALIZE_PARAMS initialize_{};
    std::array<Slot, 3> slots_;
    size_t next_submit_{}, next_poll_{};

    void configure_rate(uint32_t bitrate) {
        auto &rate = encode_config_.rcParams;
        rate.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        rate.averageBitRate = bitrate;
        rate.maxBitRate = bitrate;
        // One frame of buffer keeps each frame close to its share of the bitrate.
        rate.vbvBufferSize = bitrate / config_.fps;
        rate.vbvInitialDelay = rate.vbvBufferSize;
    }
    void release() noexcept {
        if (!encoder_)
            return;
        for (auto &slot : slots_) {
            if (slot.mapped)
                api_.nvEncUnmapInputResource(encoder_, slot.mapped);
            if (slot.registered)
                api_.nvEncUnregisterResource(encoder_, slot.registered);
            if (slot.bitstream)
                api_.nvEncDestroyBitstreamBuffer(encoder_, slot.bitstream);
            slot = {};
        }
        api_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }

  public:
    NvencEncoder(Gpu &gpu, const EncoderConfig &config) : gpu_(gpu), config_(config) {
        module_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!module_)
            throw std::runtime_error("NVIDIA encoder runtime is not installed");
        try {
            using Create = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
            using Version = NVENCSTATUS(NVENCAPI *)(uint32_t *);
            const auto create = reinterpret_cast<Create>(
                reinterpret_cast<void *>(GetProcAddress(module_, "NvEncodeAPICreateInstance")));
            const auto version = reinterpret_cast<Version>(
                reinterpret_cast<void *>(GetProcAddress(module_, "NvEncodeAPIGetMaxSupportedVersion")));
            if (!create || !version)
                throw std::runtime_error("NVIDIA encoder runtime is incomplete");
            uint32_t supported{};
            nv_check(version(&supported), "NVENC version query failed");
            if (supported < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION))
                throw std::runtime_error("NVIDIA driver is too old for this encoder");
            api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            nv_check(create(&api_), "NVENC function table failed");

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
            session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            session.device = gpu.device.Get();
            session.apiVersion = NVENCAPI_VERSION;
            nv_check(api_.nvEncOpenEncodeSessionEx(&session, &encoder_), "NVENC session failed");

            const auto codec = config.codec == Codec::hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
            const auto preset = preset_guid(config.preset);
            NV_ENC_PRESET_CONFIG defaults{};
            defaults.version = NV_ENC_PRESET_CONFIG_VER;
            defaults.presetCfg.version = NV_ENC_CONFIG_VER;
            nv_check(api_.nvEncGetEncodePresetConfigEx(encoder_, codec, preset,
                                                       NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &defaults),
                     "NVENC preset query failed");
            encode_config_ = defaults.presetCfg;
            encode_config_.version = NV_ENC_CONFIG_VER;
            encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
            encode_config_.frameIntervalP = 1; // No B-frames.
            configure_rate(config.bitrate);
            if (config.codec == Codec::hevc) {
                auto &hevc = encode_config_.encodeCodecConfig.hevcConfig;
                hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                hevc.repeatSPSPPS = 1;
                hevc.sliceMode = 3;
                hevc.sliceModeData = config.slices;
            } else {
                auto &h264 = encode_config_.encodeCodecConfig.h264Config;
                h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h264.repeatSPSPPS = 1;
                h264.sliceMode = 3;
                h264.sliceModeData = config.slices;
            }
            initialize_.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initialize_.encodeGUID = codec;
            initialize_.presetGUID = preset;
            initialize_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initialize_.encodeWidth = initialize_.darWidth = config.width;
            initialize_.encodeHeight = initialize_.darHeight = config.height;
            initialize_.frameRateNum = config.fps;
            initialize_.frameRateDen = 1;
            initialize_.enablePTD = 1;
            initialize_.encodeConfig = &encode_config_;
            nv_check(api_.nvEncInitializeEncoder(encoder_, &initialize_), "NVENC initialization failed");

            D3D11_TEXTURE2D_DESC description{};
            description.Width = config.width;
            description.Height = config.height;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BindFlags = D3D11_BIND_RENDER_TARGET;
            for (auto &slot : slots_) {
                check(gpu.device->CreateTexture2D(&description, nullptr, &slot.texture),
                      "NVENC input texture failed");
                NV_ENC_REGISTER_RESOURCE resource{};
                resource.version = NV_ENC_REGISTER_RESOURCE_VER;
                resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
                resource.width = config.width;
                resource.height = config.height;
                resource.resourceToRegister = slot.texture.Get();
                resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
                resource.bufferUsage = NV_ENC_INPUT_IMAGE;
                nv_check(api_.nvEncRegisterResource(encoder_, &resource), "NVENC texture registration failed");
                slot.registered = resource.registeredResource;
                NV_ENC_CREATE_BITSTREAM_BUFFER output{};
                output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                nv_check(api_.nvEncCreateBitstreamBuffer(encoder_, &output), "NVENC output buffer failed");
                slot.bitstream = output.bitstreamBuffer;
            }
        } catch (...) {
            release();
            FreeLibrary(module_);
            throw;
        }
    }
    ~NvencEncoder() override {
        release();
        if (module_)
            FreeLibrary(module_);
    }
    bool submit(ID3D11Texture2D *texture, uint64_t id, bool force) override {
        auto &slot = slots_[next_submit_];
        if (slot.active || !texture)
            return false;
        const auto started = monotonic_ns();
        gpu_.context->CopyResource(slot.texture.Get(), texture);
        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = slot.registered;
        nv_check(api_.nvEncMapInputResource(encoder_, &map), "NVENC input mapping failed");
        slot.mapped = map.mappedResource;
        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputWidth = config_.width;
        picture.inputHeight = config_.height;
        picture.inputBuffer = slot.mapped;
        picture.outputBitstream = slot.bitstream;
        picture.bufferFmt = map.mappedBufferFmt;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.inputTimeStamp = id;
        if (force)
            picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        nv_check(api_.nvEncEncodePicture(encoder_, &picture), "NVENC encode failed");
        slot.id = id;
        slot.start = started;
        slot.active = true;
        next_submit_ = (next_submit_ + 1) % slots_.size();
        return true;
    }
    std::optional<EncodedFrame> poll() override {
        auto &slot = slots_[next_poll_];
        if (!slot.active)
            return {};
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = slot.bitstream;
        nv_check(api_.nvEncLockBitstream(encoder_, &lock), "NVENC output failed");
        const auto *data = static_cast<const uint8_t *>(lock.bitstreamBufferPtr);
        wire::Bytes bytes(data, data + lock.bitstreamSizeInBytes);
        const bool idr = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
        api_.nvEncUnlockBitstream(encoder_, slot.bitstream);
        api_.nvEncUnmapInputResource(encoder_, slot.mapped);
        slot.mapped = nullptr;
        slot.active = false;
        next_poll_ = (next_poll_ + 1) % slots_.size();
        if (bytes.size() > 8 * 1024 * 1024)
            throw std::runtime_error("Invalid NVENC bitstream output");
        return EncodedFrame{slot.id, idr, std::move(bytes),
                            static_cast<double>(monotonic_ns() - slot.start) * 1e-6};
    }
    void bitrate(uint32_t value) override {
        configure_rate(value);
        NV_ENC_RECONFIGURE_PARAMS reconfigure{};
        reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        reconfigure.reInitEncodeParams = initialize_;
        nv_check(api_.nvEncReconfigureEncoder(encoder_, &reconfigure), "NVENC bitrate change failed");
    }
    std::string name() const override {
        return "NVIDIA NVENC";
    }
    bool hardware() const override {
        return true;
    }
};
} // namespace
std::unique_ptr<IVideoEncoder> create_nvenc_encoder(Gpu &gpu, const EncoderConfig &config) {
    return std::make_unique<NvencEncoder>(gpu, config);
}
} // namespace pocketvr::driver
