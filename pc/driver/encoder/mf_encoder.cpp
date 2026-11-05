// SPDX-License-Identifier: Apache-2.0
#include "mf.hpp"
#include <pocketvr/transport.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <bit>
#include <cstring>

namespace pocketvr::driver {
Com<IMFTransform> find_mft(GUID category, GUID input, GUID output, bool hardware, uint64_t luid) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, input}, out{MFMediaType_Video, output};
    IMFActivate **raw = nullptr;
    UINT32 count{};
    const UINT32 flags =
        (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
    using Enum2 = HRESULT(WINAPI *)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
                                    const MFT_REGISTER_TYPE_INFO *, IMFAttributes *,
                                    IMFActivate ***, UINT32 *);
    const auto symbol = GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2");
    HRESULT status{};
    if (hardware && luid && symbol) {
        Com<IMFAttributes> attributes;
        check(MFCreateAttributes(&attributes, 1), "MFT enumeration attributes failed");
        check(attributes->SetUINT64(MFT_ENUM_ADAPTER_LUID, luid), "MFT adapter filter failed");
        const auto enumerate = std::bit_cast<Enum2>(symbol);
        status = enumerate(category, flags, &in, &out, attributes.Get(), &raw, &count);
    } else
        status = MFTEnumEx(category, flags, &in, &out, &raw, &count);
    check(status, "MFT enumeration failed");
    struct Cleanup {
        IMFActivate **objects;
        UINT32 count;
        ~Cleanup() {
            for (UINT32 i = 0; i < count; ++i)
                objects[i]->Release();
            CoTaskMemFree(objects);
        }
    } cleanup{raw, count};
    for (UINT32 i = 0; i < count; ++i) {
        Com<IMFTransform> transform;
        if (SUCCEEDED(raw[i]->ActivateObject(IID_PPV_ARGS(&transform))))
            return transform;
    }
    throw std::runtime_error(hardware ? "No matching hardware Media Foundation transform"
                                      : "No matching software Media Foundation transform");
}
Com<IMFMediaType> video_type(GUID subtype, uint32_t width, uint32_t height, uint32_t fps) {
    Com<IMFMediaType> type;
    check(MFCreateMediaType(&type), "Media type creation failed");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Video major type failed");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Video subtype failed");
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height),
          "Video frame size failed");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1), "Video frame rate failed");
    check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
          "Pixel aspect ratio failed");
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}
bool set_codec_uint(ICodecAPI *api, const GUID &key, uint32_t value) {
    if (!api || api->IsSupported(&key) != S_OK)
        return false;
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    const auto status = api->SetValue(&key, &v);
    if (FAILED(status))
        spdlog::debug("MFT optional integer property rejected: {}", static_cast<uint32_t>(status));
    return SUCCEEDED(status);
}
void set_codec_bool(ICodecAPI *api, const GUID &key, bool value) {
    if (!api || api->IsSupported(&key) != S_OK)
        return;
    VARIANT v{};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const auto status = api->SetValue(&key, &v);
    if (FAILED(status))
        spdlog::debug("MFT optional boolean property rejected: {}", static_cast<uint32_t>(status));
}
namespace {
class MfEncoder final : public IVideoEncoder {
    MfRuntime runtime_;
    Gpu &gpu_;
    EncoderConfig config_;
    bool hardware_, async_{};
    int64_t period_;
    Com<IMFTransform> transform_;
    Com<ICodecAPI> codec_;
    Com<IMFMediaEventGenerator> events_;
    Com<IMFDXGIDeviceManager> manager_;
    std::unique_ptr<Nv12Converter> converter_;
    Com<ID3D11Texture2D> staging_;
    Com<IMFSample> output_sample_;
    MFT_OUTPUT_STREAM_INFO output_info_{};
    unsigned need_input_{}, have_output_{};
    wire::Bytes headers_;
    struct Slot {
        bool active{};
        uint64_t id{}, start{};
        Com<ID3D11Texture2D> nv12;
        Com<IMFMediaBuffer> cpu;
    };
    std::array<Slot, 3> slots_;
    void events() {
        if (!async_)
            return;
        for (int i = 0; i < 64; ++i) {
            Com<IMFMediaEvent> event;
            const auto status = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (status == MF_E_NO_EVENTS_AVAILABLE)
                break;
            check(status, "MFT event query failed");
            HRESULT error{};
            check(event->GetStatus(&error), "MFT event status failed");
            check(error, "Hardware MFT reported an asynchronous error");
            MediaEventType type{};
            event->GetType(&type);
            if (type == METransformNeedInput)
                need_input_ = std::min(need_input_ + 1, 16U);
            if (type == METransformHaveOutput)
                have_output_ = std::min(have_output_ + 1, 16U);
        }
    }
    void allocate_output() {
        check(transform_->GetOutputStreamInfo(0, &output_info_),
              "MFT output stream information failed");
        if (!(output_info_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            Com<IMFMediaBuffer> buffer;
            const auto size =
                std::max<DWORD>(output_info_.cbSize, config_.width * config_.height * 3);
            check(MFCreateAlignedMemoryBuffer(
                      size, output_info_.cbAlignment ? output_info_.cbAlignment - 1 : 0, &buffer),
                  "MFT output buffer allocation failed");
            check(MFCreateSample(&output_sample_), "MFT output sample allocation failed");
            check(output_sample_->AddBuffer(buffer.Get()), "MFT output sample buffer failed");
        }
        Com<IMFMediaType> type;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &type))) {
            UINT32 size{};
            if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size &&
                size <= 65536) {
                wire::Bytes config(size);
                if (SUCCEEDED(
                        type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, config.data(), size, nullptr))) {
                    auto parsed = codec_headers(config, config_.codec);
                    if (parsed)
                        headers_ = std::move(*parsed);
                }
            }
        }
    }

  public:
    MfEncoder(Gpu &gpu, const EncoderConfig &config, bool hardware)
        : gpu_(gpu), config_(config), hardware_(hardware), period_(10'000'000 / config.fps) {
        transform_ = find_mft(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12,
                              config.codec == Codec::h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC,
                              hardware, gpu.luid());
        Com<IMFAttributes> attributes;
        check(transform_->GetAttributes(&attributes), "MFT attributes unavailable");
        UINT32 asynchronous{};
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
        async_ = asynchronous != 0;
        if (async_) {
            check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE),
                  "Hardware MFT unlock failed");
            check(transform_.As(&events_), "Hardware MFT event interface failed");
        }
        if (hardware_) {
            UINT32 aware{};
            attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
            if (!aware)
                throw std::runtime_error("Hardware MFT does not support GPU-only D3D11 input");
            UINT reset{};
            check(MFCreateDXGIDeviceManager(&reset, &manager_), "MFT D3D manager creation failed");
            check(manager_->ResetDevice(gpu.device.Get(), reset), "MFT D3D manager device failed");
            check(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                             reinterpret_cast<ULONG_PTR>(manager_.Get())),
                  "MFT D3D manager setup failed");
            converter_ = std::make_unique<Nv12Converter>(gpu, config.width, config.height);
            for (auto &slot : slots_)
                slot.nv12 = converter_->create_output();
        } else {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = config.width;
            description.Height = config.height;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(gpu.device->CreateTexture2D(&description, nullptr, &staging_),
                  "Software fallback staging texture failed");
            for (auto &slot : slots_)
                check(MFCreateMemoryBuffer(config.width * config.height * 3 / 2, &slot.cpu),
                      "Software fallback NV12 buffer failed");
        }
        transform_.As(&codec_);
        set_codec_bool(codec_.Get(), CODECAPI_AVLowLatencyMode, true);
        set_codec_bool(codec_.Get(), CODECAPI_AVEncCommonLowLatency, true);
        set_codec_uint(codec_.Get(), CODECAPI_AVEncCommonRateControlMode,
                       eAVEncCommonRateControlMode_CBR);
        set_codec_uint(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, config.bitrate);
        set_codec_uint(
            codec_.Get(), CODECAPI_AVEncCommonBufferSize,
            static_cast<uint32_t>(static_cast<uint64_t>(config.bitrate) * 12 / (config.fps * 10)));
        set_codec_uint(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        set_codec_uint(codec_.Get(), CODECAPI_AVEncMPVGOPSize, 1'000'000);
        set_codec_uint(codec_.Get(), CODECAPI_AVEncCommonQualityVsSpeed,
                       config.preset == "fast"      ? 0
                       : config.preset == "quality" ? 100
                                                    : 50);
        auto output =
            video_type(config.codec == Codec::h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC,
                       config.width, config.height, config.fps);
        output->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate);
        HRESULT status = E_FAIL;
        for (UINT32 profile : config.codec == Codec::h264 ? std::array<UINT32, 3>{100, 77, 66}
                                                          : std::array<UINT32, 3>{1, 1, 1}) {
            output->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
            status = transform_->SetOutputType(0, output.Get(), 0);
            if (SUCCEEDED(status))
                break;
        }
        check(status, "MFT output type rejected");
        auto input = video_type(MFVideoFormat_NV12, config.width, config.height, config.fps);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, config.width);
        check(transform_->SetInputType(0, input.Get(), 0), "MFT NV12 input type rejected");
        allocate_output();
        check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
              "MFT begin streaming failed");
        check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
              "MFT start stream failed");
    }
    ~MfEncoder() override {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
    }
    bool submit(ID3D11Texture2D *texture, uint64_t id, bool force) override {
        events();
        if (!texture || (async_ && !need_input_) || id > static_cast<uint64_t>(INT64_MAX / period_))
            return false;
        auto slot = std::find_if(slots_.begin(), slots_.end(),
                                 [](const auto &item) { return !item.active; });
        if (slot == slots_.end())
            return false;
        const auto started = monotonic_ns();
        Com<IMFMediaBuffer> buffer;
        if (hardware_) {
            converter_->convert(texture, slot->nv12.Get());
            check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot->nv12.Get(), 0, FALSE,
                                            &buffer),
                  "MFT GPU buffer wrapping failed");
        } else {
            gpu_.context->CopyResource(staging_.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(gpu_.context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                  "Software fallback GPU readback failed");
            BYTE *bytes{};
            const auto status = slot->cpu->Lock(&bytes, nullptr, nullptr);
            if (FAILED(status)) {
                gpu_.context->Unmap(staging_.Get(), 0);
                check(status, "Software NV12 lock failed");
            }
            const auto size = static_cast<size_t>(config_.width) * config_.height * 3 / 2;
            const bool converted = bgra_to_nv12(
                std::span(static_cast<const uint8_t *>(mapped.pData),
                          static_cast<size_t>(mapped.RowPitch) * config_.height),
                mapped.RowPitch, config_.width, config_.height, std::span(bytes, size));
            slot->cpu->Unlock();
            gpu_.context->Unmap(staging_.Get(), 0);
            if (!converted)
                throw std::runtime_error("Software NV12 conversion failed");
            check(slot->cpu->SetCurrentLength(static_cast<DWORD>(size)),
                  "Software NV12 length failed");
            buffer = slot->cpu;
        }
        Com<IMFSample> sample;
        check(MFCreateSample(&sample), "MFT input sample failed");
        check(sample->AddBuffer(buffer.Get()), "MFT input buffer failed");
        check(sample->SetSampleTime(static_cast<LONGLONG>(id) * period_),
              "MFT input timestamp failed");
        sample->SetSampleDuration(period_);
        if (force && !set_codec_uint(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1))
            throw std::runtime_error("MFT rejected forced keyframe request");
        const auto status = transform_->ProcessInput(0, sample.Get(), 0);
        if (status == MF_E_NOTACCEPTING)
            return false;
        check(status, "MFT input failed");
        if (async_)
            --need_input_;
        slot->active = true;
        slot->id = id;
        slot->start = started;
        return true;
    }
    std::optional<EncodedFrame> poll() override {
        events();
        if (async_ && !have_output_)
            return {};
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = output_sample_.Get();
        DWORD status{};
        const auto result = transform_->ProcessOutput(0, 1, &output, &status);
        Com<IMFCollection> collection;
        collection.Attach(output.pEvents);
        if (async_)
            --have_output_;
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return {};
        if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
            auto type =
                video_type(config_.codec == Codec::h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC,
                           config_.width, config_.height, config_.fps);
            type->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate);
            check(transform_->SetOutputType(0, type.Get(), 0), "MFT output renegotiation failed");
            output_sample_.Reset();
            allocate_output();
            if (async_)
                ++have_output_;
            return {};
        }
        check(result, "MFT output failed");
        if (!output.pSample)
            return {};
        Com<IMFSample> sample;
        if (output.pSample == output_sample_.Get())
            sample = output_sample_;
        else
            sample.Attach(output.pSample);
        LONGLONG time{};
        check(sample->GetSampleTime(&time), "MFT output timestamp missing");
        if (time < 0 || time % period_)
            throw std::runtime_error("MFT output timestamp not aligned");
        const auto id = static_cast<uint64_t>(time / period_);
        auto slot = std::find_if(slots_.begin(), slots_.end(),
                                 [&](const auto &item) { return item.active && item.id == id; });
        if (slot == slots_.end())
            throw std::runtime_error("MFT returned an unrecognized frame");
        Com<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "MFT output buffer failed");
        BYTE *bytes{};
        DWORD length{};
        check(buffer->Lock(&bytes, nullptr, &length), "MFT output buffer lock failed");
        std::optional<wire::Bytes> normalized;
        try {
            normalized = to_annex_b(std::span(bytes, length));
        } catch (...) {
            buffer->Unlock();
            throw;
        }
        buffer->Unlock();
        if (!normalized)
            throw std::runtime_error("MFT produced an invalid H.264/HEVC access unit");
        const bool idr = contains_idr(*normalized, config_.codec);
        if (idr && !contains_parameter_sets(*normalized, config_.codec) && !headers_.empty())
            normalized->insert(normalized->begin(), headers_.begin(), headers_.end());
        const auto milliseconds = static_cast<double>(monotonic_ns() - slot->start) * 1e-6;
        slot->active = false;
        return EncodedFrame{id, idr, std::move(*normalized), milliseconds};
    }
    void bitrate(uint32_t value) override {
        if (!set_codec_uint(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, value))
            throw std::runtime_error("MFT rejected live bitrate update");
        config_.bitrate = value;
    }
    std::string name() const override {
        return hardware_ ? "Media Foundation hardware" : "Media Foundation software H.264";
    }
    bool hardware() const override {
        return hardware_;
    }
};
} // namespace
std::unique_ptr<IVideoEncoder> create_mf_encoder(Gpu &gpu, const EncoderConfig &config,
                                                 bool hardware) {
    return std::make_unique<MfEncoder>(gpu, config, hardware);
}
} // namespace pocketvr::driver
