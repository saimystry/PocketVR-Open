// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "encoder.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>

namespace pocketvr::driver {
class MfRuntime {
  public:
    MfRuntime() {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            check(hr, "COM initialization failed");
        com_ = SUCCEEDED(hr);
        try {
            check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "Media Foundation initialization failed");
        } catch (...) {
            if (com_)
                CoUninitialize();
            throw;
        }
    }
    ~MfRuntime() {
        MFShutdown();
        if (com_)
            CoUninitialize();
    }

  private:
    bool com_{};
};
struct DecodedFrame {
    uint64_t frame_id{};
    uint32_t width{}, height{};
    wire::Bytes nv12;
};
class MfDecoder {
  public:
    MfDecoder(Codec codec, uint32_t width, uint32_t height);
    ~MfDecoder();
    bool submit(std::span<const uint8_t> bytes, uint64_t frame_id);
    std::optional<DecodedFrame> poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Com<IMFTransform> find_mft(GUID category, GUID input, GUID output, bool hardware,
                           uint64_t luid = 0);
Com<IMFMediaType> video_type(GUID subtype, uint32_t width, uint32_t height, uint32_t fps = 60);
bool set_codec_uint(ICodecAPI *api, const GUID &key, uint32_t value);
void set_codec_bool(ICodecAPI *api, const GUID &key, bool value);
} // namespace pocketvr::driver
