// SPDX-License-Identifier: Apache-2.0
#include "mf.hpp"
#include <algorithm>
#include <cstring>

namespace pocketvr::driver {
struct MfDecoder::Impl {
    MfRuntime runtime;
    Codec codec;
    uint32_t width, height, coded_width{}, coded_height{};
    int32_t stride{};
    Com<IMFTransform> transform;
    Com<IMFSample> output_sample;
    MFT_OUTPUT_STREAM_INFO info{};
    Impl(Codec c, uint32_t w, uint32_t h) : codec(c), width(w), height(h) {
        if (!w || !h || (w & 1) || (h & 1) || w > 4096 || h > 4096)
            throw std::invalid_argument("Decoder dimensions");
        transform = find_mft(MFT_CATEGORY_VIDEO_DECODER,
                             c == Codec::h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC,
                             MFVideoFormat_NV12, false);
        Com<ICodecAPI> api;
        transform.As(&api);
        // Microsoft's H.264 decoder uniquely requires VT_UI4 for this property.
        // https://learn.microsoft.com/windows/win32/medfound/codecapi-avlowlatencymode
        if (c == Codec::h264)
            set_codec_uint(api.Get(), CODECAPI_AVLowLatencyMode, 1);
        else
            set_codec_bool(api.Get(), CODECAPI_AVLowLatencyMode, true);
        Com<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes)))
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        auto input = video_type(c == Codec::h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC, w, h);
        check(transform->SetInputType(0, input.Get(), 0), "Decoder input type failed");
        // Let the SPS establish the decoder's complete output type (including
        // stride, aperture and allocation flags) before selecting NV12.
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
              "Decoder begin stream failed");
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
              "Decoder start stream failed");
    }
    ~Impl() {
        if (transform) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
    }
    void allocate_output() {
        check(transform->GetOutputStreamInfo(0, &info), "Decoder output information failed");
        Com<IMFMediaType> type;
        check(transform->GetOutputCurrentType(0, &type), "Decoder current type failed");
        check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &coded_width, &coded_height),
              "Decoder output size failed");
        if (coded_width < width || coded_height < height || coded_width > 8192 ||
            coded_height > 8192)
            throw std::runtime_error("Unexpected decoder output dimensions");
        UINT32 raw_stride{};
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride)))
            stride = static_cast<int32_t>(raw_stride);
        else {
            LONG pitch{};
            check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_NV12.Data1, coded_width, &pitch),
                  "Decoder stride query failed");
            stride = pitch;
        }
        if (stride <= 0 || static_cast<uint32_t>(stride) < coded_width || stride > 65536)
            throw std::runtime_error("Invalid decoder stride");
        output_sample.Reset();
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            Com<IMFMediaBuffer> buffer;
            check(MFCreate2DMediaBuffer(coded_width, coded_height, MFVideoFormat_NV12.Data1, FALSE,
                                        &buffer),
                  "Decoder 2D output buffer failed");
            check(MFCreateSample(&output_sample), "Decoder output sample failed");
            check(output_sample->AddBuffer(buffer.Get()), "Decoder output sample buffer failed");
        }
    }
    void renegotiate() {
        for (DWORD index = 0; index < 128; ++index) {
            Com<IMFMediaType> type;
            const auto status = transform->GetOutputAvailableType(0, index, &type);
            if (status == MF_E_NO_MORE_TYPES)
                break;
            check(status, "Decoder output type enumeration failed");
            GUID subtype{};
            if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12 &&
                SUCCEEDED(transform->SetOutputType(0, type.Get(), 0))) {
                allocate_output();
                return;
            }
        }
        throw std::runtime_error("Decoder NV12 output renegotiation failed");
    }
};
MfDecoder::MfDecoder(Codec codec, uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>(codec, width, height)) {}
MfDecoder::~MfDecoder() = default;
bool MfDecoder::submit(std::span<const uint8_t> bytes, uint64_t id) {
    if (bytes.empty() || bytes.size() > 8 * 1024 * 1024 || id > INT64_MAX)
        return false;
    Com<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer),
          "Decoder input buffer failed");
    BYTE *data{};
    check(buffer->Lock(&data, nullptr, nullptr), "Decoder input lock failed");
    std::memcpy(data, bytes.data(), bytes.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
    Com<IMFSample> sample;
    check(MFCreateSample(&sample), "Decoder input sample failed");
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(id));
    const auto status = impl_->transform->ProcessInput(0, sample.Get(), 0);
    if (status == MF_E_NOTACCEPTING)
        return false;
    check(status, "Decoder input failed");
    return true;
}
std::optional<DecodedFrame> MfDecoder::poll() {
    auto &p = *impl_;
    MFT_OUTPUT_DATA_BUFFER output{};
    output.pSample = p.output_sample.Get();
    DWORD flags{};
    const auto status = p.transform->ProcessOutput(0, 1, &output, &flags);
    Com<IMFCollection> events;
    events.Attach(output.pEvents);
    if (status == MF_E_TRANSFORM_NEED_MORE_INPUT)
        return {};
    if (status == MF_E_TRANSFORM_STREAM_CHANGE || status == MF_E_TRANSFORM_TYPE_NOT_SET) {
        p.renegotiate();
        return {};
    }
    if (FAILED(status))
        throw std::runtime_error(
            std::format("Decoder output failed (HRESULT 0x{:08x}, flags 0x{:x}, cbSize {}, coded "
                        "{}x{}, stride {}, supplied sample {})",
                        static_cast<uint32_t>(status), p.info.dwFlags, p.info.cbSize, p.coded_width,
                        p.coded_height, p.stride, output.pSample != nullptr));
    if (!output.pSample)
        return {};
    Com<IMFSample> sample;
    if (output.pSample == p.output_sample.Get())
        sample = p.output_sample;
    else
        sample.Attach(output.pSample);
    LONGLONG time{};
    check(sample->GetSampleTime(&time), "Decoder output timestamp missing");
    if (time < 0)
        throw std::runtime_error("Negative decoder frame identifier");
    Com<IMFMediaBuffer> buffer;
    check(sample->ConvertToContiguousBuffer(&buffer), "Decoder output buffer access failed");
    Com<IMF2DBuffer> two_d;
    BYTE *data{};
    DWORD length{};
    LONG pitch = p.stride;
    const bool planar = SUCCEEDED(buffer.As(&two_d));
    if (planar) {
        check(two_d->Lock2D(&data, &pitch), "Decoder 2D buffer lock failed");
        buffer->GetMaxLength(&length);
    } else
        check(buffer->Lock(&data, nullptr, &length), "Decoder CPU buffer lock failed");
    auto unlock = [&] {
        if (planar)
            two_d->Unlock2D();
        else
            buffer->Unlock();
    };
    try {
        if (pitch <= 0 || static_cast<uint32_t>(pitch) < p.width ||
            static_cast<uint64_t>(pitch) * p.coded_height * 3 / 2 > length)
            throw std::runtime_error("Decoder output buffer has invalid plane bounds");
        DecodedFrame frame{static_cast<uint64_t>(time), p.width, p.height,
                           wire::Bytes(static_cast<size_t>(p.width) * p.height * 3 / 2)};
        for (size_t row = 0; row < p.height; ++row)
            std::memcpy(frame.nv12.data() + row * p.width, data + row * static_cast<size_t>(pitch),
                        p.width);
        const auto offset = static_cast<size_t>(p.width) * p.height;
        for (size_t row = 0; row < p.height / 2; ++row)
            std::memcpy(frame.nv12.data() + offset + row * p.width,
                        data + (static_cast<size_t>(p.coded_height) + row) *
                                   static_cast<size_t>(pitch),
                        p.width);
        unlock();
        return frame;
    } catch (...) {
        unlock();
        throw;
    }
}
} // namespace pocketvr::driver
