// SPDX-License-Identifier: Apache-2.0
#include "encoder.hpp"
#include <pocketvr/transport.hpp>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>

namespace pocketvr::driver {
namespace {
constexpr char shader[] = R"(
Texture2D image:register(t0); SamplerState filter:register(s0);
struct V { float4 p:SV_Position; float2 uv:TEXCOORD0; };
V vs(uint id:SV_VertexID) { V o; float2 p=float2(id==1?3:-1,id==2?3:-1);
o.p=float4(p,0,1);o.uv=float2((p.x+1)*.5,(1-p.y)*.5);return o; }
float4 ps(V i):SV_Target { return image.Sample(filter,i.uv); }
)";
class ScaledEncoder final : public IVideoEncoder {
    struct Slot {
        Com<ID3D11Texture2D> texture;
        Com<ID3D11RenderTargetView> target;
        uint64_t id{}, started{};
        bool active{};
    };
    Gpu &gpu_;
    EncoderConfig config_;
    std::unique_ptr<IVideoEncoder> encoder_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11VertexShader> vertex_;
    Com<ID3D11PixelShader> pixel_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11RasterizerState> raster_;
    std::array<Slot,3> slots_;
    std::unordered_map<ID3D11Texture2D*,Com<ID3D11ShaderResourceView>> views_;
    void initialize() {
        check(gpu_.device->CreateDeferredContext(0,&context_),"Scale context failed");
        const auto compile = [](const char *entry,const char *target) {
            Com<ID3DBlob> code,error;
            check(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,entry,target,
                D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error),"Scale shader failed");
            return code;
        };
        const auto vs=compile("vs","vs_5_0"), ps=compile("ps","ps_5_0");
        check(gpu_.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_),"Scale vertex shader failed");
        check(gpu_.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_),"Scale pixel shader failed");
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD=D3D11_FLOAT32_MAX;
        check(gpu_.device->CreateSamplerState(&sampler,&sampler_),"Scale sampler failed");
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
        raster.DepthClipEnable=TRUE;
        check(gpu_.device->CreateRasterizerState(&raster,&raster_),"Scale raster state failed");
        for (auto &slot:slots_) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=config_.width;desc.Height=config_.height;
            desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            check(gpu_.device->CreateTexture2D(&desc,nullptr,&slot.texture),"Scale target failed");
            check(gpu_.device->CreateRenderTargetView(slot.texture.Get(),nullptr,&slot.target),"Scale view failed");
        }
    }
  public:
    ScaledEncoder(Gpu &gpu,EncoderConfig config,std::unique_ptr<IVideoEncoder> encoder)
        :gpu_(gpu),config_(std::move(config)),encoder_(std::move(encoder)) {}
    ~ScaledEncoder() override { encoder_.reset(); } // Release codec references before targets.
    bool submit(ID3D11Texture2D *source,uint64_t id,bool force) override {
        if (!source) return false;
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if (desc.Width==config_.width&&desc.Height==config_.height) return encoder_->submit(source,id,force);
        if (!context_) initialize();
        auto slot=std::find_if(slots_.begin(),slots_.end(),[](const auto &s){return !s.active;});
        if (slot==slots_.end()) return false;
        const auto started=monotonic_ns();
        auto view=views_.find(source);
        if (view==views_.end()) {
            Com<ID3D11ShaderResourceView> resource;
            check(gpu_.device->CreateShaderResourceView(source,nullptr,&resource),"Scale input view failed");
            if (views_.size()>=16) views_.clear();
            view=views_.emplace(source,std::move(resource)).first;
        }
        context_->ClearState();
        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(config_.width),static_cast<float>(config_.height),0,1};
        context_->RSSetViewports(1,&viewport);context_->RSSetState(raster_.Get());
        auto *target=slot->target.Get();context_->OMSetRenderTargets(1,&target,nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);
        auto *srv=view->second.Get();auto *sampler=sampler_.Get();
        context_->PSSetShaderResources(0,1,&srv);context_->PSSetSamplers(0,1,&sampler);context_->Draw(3,0);
        context_->ClearState();
        Com<ID3D11CommandList> commands;check(context_->FinishCommandList(FALSE,&commands),"Scale commands failed");
        gpu_.context->ExecuteCommandList(commands.Get(),TRUE);
        if (!encoder_->submit(slot->texture.Get(),id,force)) return false;
        slot->active=true;slot->id=id;slot->started=started;return true;
    }
    std::optional<EncodedFrame> poll() override {
        auto frame=encoder_->poll();
        if (frame) for (auto &slot:slots_) if (slot.active&&slot.id==frame->frame_id) {
            frame->encode_ms=static_cast<double>(monotonic_ns()-slot.started)*1e-6;slot.active=false;break;
        }
        return frame;
    }
    void bitrate(uint32_t value) override {encoder_->bitrate(value);}
    uint32_t max_bitrate() const override {return encoder_->max_bitrate();}
    std::string name() const override {return encoder_->name();}
    bool hardware() const override {return encoder_->hardware();}
};
}
std::unique_ptr<IVideoEncoder> scale_encoder(Gpu &gpu,const EncoderConfig &config,std::unique_ptr<IVideoEncoder> encoder) {
    return std::make_unique<ScaledEncoder>(gpu,config,std::move(encoder));
}
} // namespace pocketvr::driver
