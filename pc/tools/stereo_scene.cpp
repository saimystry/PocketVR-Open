// SPDX-License-Identifier: Apache-2.0
#include "d3d.hpp"
#include <openvr.h>
#include <charconv>
#include <iostream>
#include <thread>
#include <array>
#include <vector>
#include <nlohmann/json.hpp>

// Original, matching directional patterns isolate a real SteamVR submission from
// game/controller setup. CPU image generation exists only in this diagnostic.
int main(int argc, char **argv) {
    unsigned seconds = 30;
    const bool inspect = argc == 2 && std::string_view(argv[1]) == "--inspect";
    if (argc != 1 && !inspect) {
        if (argc != 3 || std::string_view(argv[1]) != "--seconds") {
            std::cerr << "Usage: pocketvr_stereo_scene [--seconds 1..300 | --inspect]\n";
            return 2;
        }
        const std::string_view argument(argv[2]);
        const auto parsed =
            std::from_chars(argument.data(), argument.data() + argument.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
            !seconds || seconds > 300)
            return 2;
    }
    bool initialized = false;
    try {
        vr::EVRInitError error{};
        auto *system = vr::VR_Init(&error, inspect ? vr::VRApplication_Background : vr::VRApplication_Scene);
        if (error != vr::VRInitError_None || !system)
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        initialized = true;
        if (inspect) {
            auto eyes = nlohmann::json::array();
            for (const auto eye : {vr::Eye_Left, vr::Eye_Right}) {
                const auto transform = system->GetEyeToHeadTransform(eye);
                auto matrix = nlohmann::json::array();
                for (const auto &row : transform.m)
                    for (const auto v : row) matrix.push_back(v);
                float left{}, right{}, top{}, bottom{};
                system->GetProjectionRaw(eye, &left, &right, &top, &bottom);
                eyes.push_back({{"eye_to_head", matrix}, {"projection", {left, right, top, bottom}}});
            }
            nlohmann::json origins = nlohmann::json::object();
            nlohmann::json controllers = nlohmann::json::object();
            for (const auto origin : {vr::TrackingUniverseRawAndUncalibrated, vr::TrackingUniverseStanding, vr::TrackingUniverseSeated}) {
                std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> tracked{};
                system->GetDeviceToAbsoluteTrackingPose(origin, 0, tracked.data(), static_cast<uint32_t>(tracked.size()));
                auto matrix = nlohmann::json::array();
                for (const auto &row : tracked[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m)
                    for (const auto value : row) matrix.push_back(value);
                origins[std::to_string(origin)] = matrix;
                auto hands = nlohmann::json::array();
                for (uint32_t index = 1; index < tracked.size(); ++index) {
                    if (system->GetTrackedDeviceClass(index) != vr::TrackedDeviceClass_Controller) continue;
                    std::array<char, 128> serial{};
                    vr::ETrackedPropertyError property_error{};
                    system->GetStringTrackedDeviceProperty(index, vr::Prop_SerialNumber_String,
                        serial.data(), static_cast<uint32_t>(serial.size()), &property_error);
                    if (property_error != vr::TrackedProp_Success) continue;
                    const std::string_view name(serial.data());
                    if (name != "PocketVR-Left" && name != "PocketVR-Right") continue;
                    auto transform = nlohmann::json::array();
                    for (const auto &row : tracked[index].mDeviceToAbsoluteTracking.m)
                        for (const auto value : row) transform.push_back(value);
                    hands.push_back({{"role", name == "PocketVR-Left" ? "left" : "right"},
                        {"valid", tracked[index].bPoseIsValid}, {"connected", tracked[index].bDeviceIsConnected},
                        {"transform", transform},
                        {"velocity", {tracked[index].vVelocity.v[0], tracked[index].vVelocity.v[1], tracked[index].vVelocity.v[2]}},
                        {"angular_velocity", {tracked[index].vAngularVelocity.v[0], tracked[index].vAngularVelocity.v[1], tracked[index].vAngularVelocity.v[2]}}});
                }
                controllers[std::to_string(origin)] = hands;
            }
            std::cout << nlohmann::json{{"eyes", eyes}, {"origins", origins}, {"controllers", controllers}}.dump(2) << '\n';
            vr::VR_Shutdown();
            return 0;
        }
        auto *compositor = vr::VRCompositor();
        if (!compositor)
            throw std::runtime_error("SteamVR compositor unavailable");
        pocketvr::driver::Gpu gpu;
        std::array<pocketvr::driver::Com<ID3D11Texture2D>, 2> textures;
        constexpr unsigned width = 512, height = 512;
        std::vector<uint8_t> pixels(width * height * 4);
        for (unsigned eye = 0; eye < 2; ++eye) {
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x) {
                    const auto at = (y * width + x) * 4;
                    const bool bottom = y >= height / 2;
                    pixels[at] = 0;
                    pixels[at + 1] = bottom ? 255 : 0;
                    pixels[at + 2] = !bottom ? 255 : 0;
                    pixels[at + 3] = 255;
                    if ((x > 244 && x < 268) || (y > 244 && y < 268))
                        pixels[at] = pixels[at + 1] = pixels[at + 2] = 255;
                    // Identical UP arrow in each eye makes a vertical flip
                    // unambiguous without remembering different eye colours.
                    const auto dx = static_cast<int>(x) - 256;
                    if ((y >= 60 && y < 140 && std::abs(dx) <= static_cast<int>(y - 60)) ||
                        (y >= 130 && y < 220 && std::abs(dx) < 18))
                        pixels[at] = pixels[at + 1] = pixels[at + 2] = 255;
                    constexpr std::array<unsigned, 7> u{17, 17, 17, 17, 17, 17, 14};
                    constexpr std::array<unsigned, 7> p{30, 17, 17, 30, 16, 16, 16};
                    if (y >= 290 && y < 360 && x >= 196 && x < 316) {
                        const auto row = (y - 290) / 10, column = (x - 196) / 10;
                        if ((column < 5 && (u[row] & (16U >> column))) ||
                            (column >= 7 && (p[row] & (16U >> (column - 7)))))
                            pixels[at] = pixels[at + 1] = pixels[at + 2] = 255;
                    }
                }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
            pocketvr::driver::check(gpu.device->CreateTexture2D(&desc, &initial, &textures[eye]),
                                    "Diagnostic texture creation failed");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        uint64_t submitted{};
        std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses;
        while (std::chrono::steady_clock::now() < deadline) {
            vr::VREvent_t event{};
            bool quit = false;
            while (system->PollNextEvent(&event, sizeof(event)))
                if (event.eventType == vr::VREvent_Quit)
                    quit = true;
            if (quit)
                break;
            if (compositor->WaitGetPoses(poses.data(), static_cast<uint32_t>(poses.size()), nullptr,
                                         0) != vr::VRCompositorError_None)
                throw std::runtime_error("SteamVR pose wait failed");
            for (unsigned eye = 0; eye < 2; ++eye) {
                const vr::Texture_t texture{textures[eye].Get(), vr::TextureType_DirectX,
                                            vr::ColorSpace_Gamma};
                const auto result =
                    compositor->Submit(eye ? vr::Eye_Right : vr::Eye_Left, &texture);
                if (result != vr::VRCompositorError_None)
                    throw std::runtime_error("SteamVR texture submission failed: " +
                                             std::to_string(result));
            }
            compositor->PostPresentHandoff();
            ++submitted;
        }
        vr::VR_Shutdown();
        initialized = false;
        std::cout << "Submitted " << submitted << " original stereo diagnostic frames.\n";
        return 0;
    } catch (const std::exception &error) {
        if (initialized)
            vr::VR_Shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
