// SPDX-License-Identifier: Apache-2.0
#include "graphics.hpp"
#include "input.hpp"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <stdexcept>
#include <format>

namespace pocketvr::dashboard {
void check(HRESULT result, const char *operation) {
    if (FAILED(result)) throw std::runtime_error(std::format("{} (0x{:08x})", operation, static_cast<unsigned>(result)));
}
Graphics::Graphics(int adapter, bool warp, HWND preview) {
    Com<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    Com<IDXGIAdapter1> gpu;
    if (adapter >= 0) check(factory->EnumAdapters1(static_cast<UINT>(adapter), &gpu), "Find SteamVR GPU");
    const auto type = gpu ? D3D_DRIVER_TYPE_UNKNOWN : warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    auto created = D3D11CreateDevice(gpu.Get(), type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        &level, 1, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    // The desktop preview still works on a PC without a DirectX 11 graphics card.
    if (FAILED(created) && type == D3D_DRIVER_TYPE_HARDWARE)
        created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &level, 1, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    check(created, "Create overlay GPU device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    check(device_->CreateTexture2D(&desc, nullptr, &texture_), "Create dashboard texture");
    check(device_->CreateRenderTargetView(texture_.Get(), nullptr, &target_), "Create dashboard render target");
    if (preview) {
        DXGI_SWAP_CHAIN_DESC swap{};
        swap.BufferDesc.Width = width; swap.BufferDesc.Height = height;
        swap.BufferDesc.Format = desc.Format; swap.SampleDesc.Count = 1;
        swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; swap.BufferCount = 2;
        swap.OutputWindow = preview; swap.Windowed = TRUE; swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        check(factory->CreateSwapChain(device_.Get(), &swap, &swap_), "Create preview swap chain");
        check(factory->MakeWindowAssociation(preview, DXGI_MWA_NO_ALT_ENTER), "Disable preview fullscreen switching");
        check(swap_->GetBuffer(0, IID_PPV_ARGS(&back_)), "Get preview buffer");
    }
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    try {
        auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImFontConfig font; font.SizePixels = 23.f; io.Fonts->AddFontDefault(&font);
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle(); style.WindowPadding = {24, 16}; style.FramePadding = {10, 7};
        style.ItemSpacing = {10, 8}; style.WindowRounding = 0;
        style.Colors[ImGuiCol_WindowBg] = {.035f, .055f, .09f, 1};
        style.Colors[ImGuiCol_Header] = {.1f, .3f, .45f, 1};
        style.Colors[ImGuiCol_Button] = {.11f, .24f, .35f, 1};
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) throw std::runtime_error("Initialize ImGui D3D11 renderer");
        initialized_ = true;
        if (preview) {
            if (!ImGui_ImplWin32_Init(preview)) throw std::runtime_error("Initialize preview input");
            platform_ = true;
        }
    } catch (...) {
        if (initialized_) ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(); throw;
    }
}
Graphics::~Graphics() {
    if (platform_) ImGui_ImplWin32_Shutdown();
    if (initialized_) ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    context_->ClearState(); context_->Flush();
}
void Graphics::begin(float elapsed) {
    ImGui_ImplDX11_NewFrame();
    if (platform_) ImGui_ImplWin32_NewFrame();
    auto &io = ImGui::GetIO(); io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
    io.DeltaTime = std::clamp(elapsed, .001f, .1f);
    ImGui::NewFrame();
}
void Graphics::render() {
    ImGui::Render();
    auto *target = target_.Get(); context_->OMSetRenderTargets(1, &target, nullptr);
    constexpr float clear[]{.035f, .055f, .09f, 1}; context_->ClearRenderTargetView(target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (FAILED(device_->GetDeviceRemovedReason())) throw std::runtime_error("Dashboard GPU device was lost; restart the dashboard");
    if (swap_) { context_->CopyResource(back_.Get(), texture_.Get()); check(swap_->Present(1, 0), "Present preview"); }
    else context_->Flush();
}
void Graphics::capture(const std::filesystem::path &path) {
    D3D11_TEXTURE2D_DESC desc{}; texture_->GetDesc(&desc);
    desc.BindFlags = 0; desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging; check(device_->CreateTexture2D(&desc, nullptr, &staging), "Create test capture");
    context_->CopyResource(staging.Get(), texture_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read test capture");
    try {
        Com<IWICImagingFactory> factory;
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create PNG factory");
        Com<IStream> stream;
        check(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
            FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream), "Create test PNG");
        Com<IWICBitmapEncoder> encoder; check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        Com<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
        check(frame->Initialize(nullptr), "Initialize PNG frame"); check(frame->SetSize(width, height), "Set PNG size");
        auto format = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format), "Set PNG pixel format");
        if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder changed pixel format");
        check(frame->WritePixels(height, mapped.RowPitch, mapped.RowPitch * height, static_cast<BYTE *>(mapped.pData)), "Encode PNG pixels");
        check(frame->Commit(), "Commit PNG frame"); check(encoder->Commit(), "Commit test PNG");
    } catch (...) { context_->Unmap(staging.Get(), 0); throw; }
    context_->Unmap(staging.Get(), 0);
}
} // namespace pocketvr::dashboard
