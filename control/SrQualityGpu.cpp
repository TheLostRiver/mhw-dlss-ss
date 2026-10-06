#include "SrQualityGpu.h"
#include <d3dcompiler.h>
#include <cstring>

namespace mhwsr {
namespace {
using Microsoft::WRL::ComPtr;
// Same unpacking and allocation-pixel convention as MhwPackedMotionDecode.hlsl.
// Constants come from the camera bound to this TAA, never a renderer global.
constexpr char kDecode[]=R"(
cbuffer Frame : register(b0) {
    uint2 activeSize; uint2 allocationSize;
    float2 currentJitter; float2 previousJitter;
};
Texture2D<uint> packedMotion : register(t0);
RWTexture2D<float2> decoded : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
    if (any(p.xy >= activeSize)) return;
    uint v = packedMotion.Load(int3(p.xy,0));
    float2 mv = float2(f16tof32(v >> 16), f16tof32(v & 0xfffeu));
    decoded[p.xy] = (mv + currentJitter - previousJitter) * float2(allocationSize);
})";
void Transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&barrier);
}
bool Texture(ID3D12Device* device,Size s,DXGI_FORMAT format,ID3D12Resource** output,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=s.width;d.Height=s.height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=flags;
    return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(output)));
}
bool FrameTexture(ID3D12Resource* r,Size s,DXGI_FORMAT format) {
    if(!r)return false;
    const auto d=r->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==s.width&&d.Height==s.height&&
        d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&d.Format==format;
}
}
QualityGpu::~QualityGpu() {
    // The bridge deliberately retains the entire session on uncertain GPU work.
    // Destruction is allowed only before recording or after ReleaseAfterGpu.
    if(!referenced_&&params_&&api_.destroy)api_.destroy(params_);
    if(nativeConstantsMapped_)nativeConstants_->Unmap(0,nullptr);
}
bool QualityGpu::Prepare(const Dispatch& api,const Plan& plan,ID3D12Device* device,ID3D12Resource* packed,bool timings) {
    if(device_||!api.create||!api.evaluate||!api.allocate||!api.destroy||!api.release||!device||
       !QualityPlanValid(plan)||!FrameTexture(packed,plan.output,DXGI_FORMAT_R32_UINT))return false;
    api_=api;plan_=plan;device_=device;packed_=packed;
    if(timings) {
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=kTimingSamples*2;
        if(FAILED(device->CreateQueryHeap(&q,IID_PPV_ARGS(&timingQueries_))))return false;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=UINT64(kTimingSamples)*2*sizeof(UINT64);d.Height=1;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&timingReadback_))))return false;
    }
    const auto preparationFlags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(!Texture(device,plan.output,DXGI_FORMAT_R16G16_FLOAT,&motion_,preparationFlags)||
       !Texture(device,plan.output,DXGI_FORMAT_R11G11B10_FLOAT,&output_)||!Texture(device,plan.output,DXGI_FORMAT_R32_FLOAT,&depth_,preparationFlags)||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_))))return false;
    stride_=device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_UINT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();device->CreateShaderResourceView(packed,&srv,cpu);cpu.ptr+=stride_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(motion_.Get(),nullptr,&uav,cpu);
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
    D3D12_ROOT_PARAMETER roots[2]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[0].DescriptorTable={2,ranges};
    roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[1].Constants={0,0,8};
    D3D12_ROOT_SIGNATURE_DESC rd{2,roots,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||
       FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&decodeRoot_))))return false;
    ComPtr<ID3DBlob> shader;
    if(FAILED(D3DCompile(kDecode,sizeof(kDecode)-1,"MhwSrQualityDecode",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error)))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=decodeRoot_.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&decodePso_))))return false;
    // Immutable geometry; never edit a shared game upload buffer or reuse its bytes.
    const float data[]={-1,1,0,0, 1,1,1,0, -1,-1,0,1, 1,-1,1,1,
                        -1,1,0,0, -1,-3,0,2, 3,1,2,0};
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC vb{};vb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;vb.Width=sizeof(data);vb.Height=1;
    vb.DepthOrArraySize=1;vb.MipLevels=1;vb.SampleDesc.Count=1;vb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&vb,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&vertices_))))return false;
    void* mapped=nullptr;D3D12_RANGE noRead{0,0};
    if(FAILED(vertices_->Map(0,&noRead,&mapped))||!mapped)return false;
    memcpy(mapped,data,sizeof(data));D3D12_RANGE written{0,sizeof(data)};vertices_->Unmap(0,&written);
    quad_={vertices_->GetGPUVirtualAddress(),64,16};triangle_={quad_.BufferLocation+64,48,16};
    if(!Ok(api_.allocate(&params_))||!params_)return false;
    params_->Set("CreationNodeMask",1u);params_->Set("VisibilityNodeMask",1u);
    params_->Set("Width",plan.optimal.width);params_->Set("Height",plan.optimal.height);
    params_->Set("OutWidth",plan.output.width);params_->Set("OutHeight",plan.output.height);
    params_->Set("PerfQualityValue",2);params_->Set("DLSS.Feature.Create.Flags",0x4b);
    params_->Set("DLSS.Hint.Render.Preset.Quality",11u);params_->Set("DLSS.Enable.Output.Subrects",0);
    return true;
}
NVSDK_NGX_Result QualityGpu::Prime(ID3D12GraphicsCommandList* list) {
    if(!params_||!list)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if(feature_)return NVSDK_NGX_Result_Success;
    referenced_=true;
    const auto result=api_.create(list,NVSDK_NGX_Feature_SuperSampling,params_,&feature_);
    return Ok(result)&&!feature_?NVSDK_NGX_Result_FAIL_FeatureNotFound:result;
}
bool QualityGpu::PrepareNativeInputs(ID3D12RootSignature* root) {
    if(nativePreparePso_)return nativeRoot_.Get()==root;
    if(!device_||!root)return false;
    constexpr char source[]=R"(
cbuffer Frame : register(b0) { float2 currentJitter; float2 previousJitter; };
Texture2D<float> sceneDepth : register(t1);
Texture2D<uint> packedMotion : register(t2);
float4 vertex(uint id : SV_VertexID) : SV_POSITION {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
struct Result {float2 motion : SV_Target0; float depth : SV_Target1;};
Result pixel(float4 position : SV_POSITION) {
    uint2 p=uint2(position.xy); uint w,h; packedMotion.GetDimensions(w,h);
    uint v=packedMotion.Load(int3(p,0));
    Result r;
    r.motion=(float2(f16tof32(v>>16),f16tof32(v&0xfffeu))+currentJitter-previousJitter)*float2(w,h);
    r.depth=sceneDepth.Load(int3(p,0)); return r;
})";
    ComPtr<ID3DBlob> vs,ps,error;
    if(FAILED(D3DCompile(source,sizeof(source)-1,"MhwSrNativeInputs",nullptr,nullptr,"vertex","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error))||
       FAILED(D3DCompile(source,sizeof(source)-1,"MhwSrNativeInputs",nullptr,nullptr,"pixel","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error)))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};d.pRootSignature=root;d.VS={vs->GetBufferPointer(),vs->GetBufferSize()};d.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    d.SampleMask=UINT_MAX;d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;d.RasterizerState.DepthClipEnable=TRUE;
    d.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;d.DepthStencilState.StencilReadMask=d.DepthStencilState.StencilWriteMask=255;
    d.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};d.DepthStencilState.BackFace=d.DepthStencilState.FrontFace;
    for(auto& blend:d.BlendState.RenderTarget){blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;}
    d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;d.NumRenderTargets=2;
    d.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;d.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;d.SampleDesc.Count=1;
    if(FAILED(device_->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&nativePreparePso_))))return false;
    D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;h.NumDescriptors=2;
    if(FAILED(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&nativeRtvs_)))){nativePreparePso_.Reset();return false;}
    nativeRtvStride_=device_->GetDescriptorHandleIncrementSize(h.Type);
    auto at=nativeRtvs_->GetCPUDescriptorHandleForHeapStart();device_->CreateRenderTargetView(motion_.Get(),nullptr,at);at.ptr+=nativeRtvStride_;
    device_->CreateRenderTargetView(depth_.Get(),nullptr,at);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=UINT64(kNativeConstantSlotCount)*256;buffer.Height=1;
    buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device_->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&nativeConstants_))))return false;
    D3D12_RANGE noRead{0,0};void* mapped=nullptr;
    if(FAILED(nativeConstants_->Map(0,&noRead,&mapped))||!mapped)return false;
    nativeConstantsMapped_=static_cast<unsigned char*>(mapped);nativeRoot_=root;return true;
}
bool QualityGpu::RecordNativeInputs(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root,const QualityFrame& frame) {
    const auto active=frame.render;
    if(!nativePreparePso_||nativeRoot_.Get()!=root||!Inside(active,plan_.minimum,plan_.maximum)||!nativeConstantsMapped_||!NativeInputBudgetAvailable())return false;
    referenced_=true;
    // Every evaluation receives a fresh 256-byte slot. None is overwritten
    // while this bounded session's GPU commands may still reference it.
    const auto offset=UINT64(nativeConstantSlots_++)*256;
    const float jitter[]={frame.jitter[0],frame.jitter[1],frame.previousJitter[0],frame.previousJitter[1]};
    memcpy(nativeConstantsMapped_+offset,jitter,sizeof(jitter));
    list->SetGraphicsRootConstantBufferView(2,nativeConstants_->GetGPUVirtualAddress()+offset);
    // Native t1/t2 are already legally pixel-readable at this exact draw. Sample
    // them with the existing descriptors; no guessed source-state transition.
    Transition(list,motion_.Get(),motionState_,D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(list,depth_.Get(),depthState_,D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtvs=nativeRtvs_->GetCPUDescriptorHandleForHeapStart();list->OMSetRenderTargets(2,&rtvs,TRUE,nullptr);
    list->SetPipelineState(nativePreparePso_.Get());list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0,0,float(active.width),float(active.height),0,1};const D3D12_RECT scissor{0,0,LONG(active.width),LONG(active.height)};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->DrawInstanced(3,1,0,0);
    Transition(list,motion_.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list,depth_.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    motionState_=depthState_=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;return true;
}
bool QualityGpu::CaptureRasterDepth(ID3D12GraphicsCommandList* list,ID3D12Resource* source,D3D12_RESOURCE_STATES state) {
    if(!list||!depth_||!FrameTexture(source,plan_.output,DXGI_FORMAT_R32_TYPELESS)||
       !(source->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))return false;
    referenced_=true;
    // Copy the whole depth/stencil subresource, before the engine can reuse it.
    Transition(list,source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list,depth_.Get(),depthState_,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(depth_.Get(),source);
    Transition(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
    Transition(list,depth_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depthState_=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;return true;
}
bool QualityGpu::StageRawTaaFallback(const QualityFrame& f) {
    if(!f.list||f.color==f.taaOutput||f.render.width!=plan_.aligned.width||f.render.height!=plan_.aligned.height||
       !FrameTexture(f.color,plan_.output,DXGI_FORMAT_R11G11B10_FLOAT)||!FrameTexture(f.taaOutput,plan_.output,DXGI_FORMAT_R11G11B10_FLOAT))return false;
    referenced_=true;
    // Preserve this frame's raw ROI for the existing T -> B copy. If the later
    // SR pass fails, native postprocessing gets current color, never stale TAA.
    Transition(f.list,f.color,f.colorState,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(f.list,f.taaOutput,f.outputState,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=f.color;dst.pResource=f.taaOutput;
    src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX roi{0,0,0,f.render.width,f.render.height,1};f.list->CopyTextureRegion(&dst,0,0,0,&src,&roi);
    Transition(f.list,f.taaOutput,D3D12_RESOURCE_STATE_COPY_DEST,f.outputState);
    Transition(f.list,f.color,D3D12_RESOURCE_STATE_COPY_SOURCE,f.colorState);
    return true;
}
NVSDK_NGX_Result QualityGpu::Evaluate(const QualityFrame& f) {
    if(!params_||!f.list||!f.associated||!Matches(f.packedMotion)||((f.depthIsRaster||f.nativeInputs)&&depthState_!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)||
       (f.nativeInputs&&motionState_!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)||
       !Inside(f.render,plan_.minimum,plan_.maximum)||f.render.width!=plan_.aligned.width||f.render.height!=plan_.aligned.height||
       !FrameTexture(f.color,plan_.output,DXGI_FORMAT_R11G11B10_FLOAT)||
       !FrameTexture(f.depth,plan_.output,f.depthIsRaster?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32_FLOAT)||
       !FrameTexture(f.taaOutput,plan_.output,DXGI_FORMAT_R11G11B10_FLOAT))
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    for(unsigned i=0;i<2;++i)if(!std::isfinite(f.jitter[i])||!std::isfinite(f.previousJitter[i]))return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto creation=Prime(f.list);if(!Ok(creation))return creation;
    auto* list=f.list;
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if(!f.nativeInputs) {
    Transition(list,f.packedMotion,f.motionState,read);
    ID3D12DescriptorHeap* heaps[]={heap_.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(decodeRoot_.Get());list->SetPipelineState(decodePso_.Get());
    list->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());
    struct Constants {unsigned w,h,ow,oh;float x,y,px,py;} values{f.render.width,f.render.height,plan_.output.width,plan_.output.height,
        f.jitter[0],f.jitter[1],f.previousJitter[0],f.previousJitter[1]};
    list->SetComputeRoot32BitConstants(1,8,&values,0);list->Dispatch((f.render.width+7)/8,(f.render.height+7)/8,1);
    Transition(list,motion_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);
    Transition(list,f.packedMotion,read,f.motionState);
    }
    Transition(list,f.color,f.colorState,read);
    auto* depthInput=f.depth;
    if(f.depthIsRaster||f.nativeInputs) {
        depthInput=depth_.Get();
    }else Transition(list,f.depth,f.depthState,read);
    params_->Set("Color",f.color);params_->Set("Depth",depthInput);params_->Set("MotionVectors",motion_.Get());params_->Set("Output",output_.Get());
    params_->Set("Jitter.Offset.X",f.jitter[0]*float(f.render.width)*-0.5f);
    params_->Set("Jitter.Offset.Y",f.jitter[1]*float(f.render.height)*0.5f);
    params_->Set("MV.Scale.X",-0.5f*float(f.render.width)/float(plan_.output.width));
    params_->Set("MV.Scale.Y",0.5f*float(f.render.height)/float(plan_.output.height));
    params_->Set("DLSS.Render.Subrect.Dimensions.Width",f.render.width);params_->Set("DLSS.Render.Subrect.Dimensions.Height",f.render.height);
    const char* bases[]={"DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y","DLSS.Input.Depth.Subrect.Base.X","DLSS.Input.Depth.Subrect.Base.Y",
        "DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y","DLSS.Output.Subrect.Base.X","DLSS.Output.Subrect.Base.Y"};
    for(auto* key:bases)params_->Set(key,0u);
    params_->Set("Reset",f.reset?1:0);params_->Set("Sharpness",0.0f);
    params_->Set("DLSS.Pre.Exposure",1.0f);params_->Set("DLSS.Exposure.Scale",1.0f);
    params_->Set("ExposureTexture",static_cast<ID3D12Resource*>(nullptr));
    const auto result=api_.evaluate(list,feature_,params_,nullptr);
    Transition(list,f.color,read,f.colorState);
    if(!f.depthIsRaster&&!f.nativeInputs)Transition(list,f.depth,read,f.depthState);
    if(!f.nativeInputs)Transition(list,motion_.Get(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // A failed evaluation NEVER writes stale/uninitialized output into the game.
    if(Ok(result)) {
        Transition(list,output_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list,f.taaOutput,f.outputState,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=output_.Get();dst.pResource=f.taaOutput;
        src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Transition(list,f.taaOutput,D3D12_RESOURCE_STATE_COPY_DEST,f.outputState);
        Transition(list,output_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return result;
}
unsigned QualityGpu::BeginTiming(ID3D12GraphicsCommandList* list,unsigned kind,unsigned phase,Size input) {
    if(!timingQueries_||!list||timingCount_>=kTimingSamples)return UINT_MAX;
    const auto index=timingCount_++;timingRecords_[index]={kind,phase,input,false};referenced_=true;
    // Timestamp queries use EndQuery at both endpoints, not BeginQuery.
    list->EndQuery(timingQueries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2);return index;
}
void QualityGpu::EndTiming(ID3D12GraphicsCommandList* list,unsigned token) {
    if(!list||token>=timingCount_||timingRecords_[token].resolved)return;
    list->EndQuery(timingQueries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token*2+1);
    list->ResolveQueryData(timingQueries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token*2,2,timingReadback_.Get(),UINT64(token)*2*sizeof(UINT64));
    timingRecords_[token].resolved=true;
}
bool QualityGpu::ReadTimings(UINT64 frequency,std::vector<QualityTiming>& out) {
    if(!frequency||!timingReadback_||!timingCount_)return false;
    const D3D12_RANGE range{0,SIZE_T(timingCount_)*2*sizeof(UINT64)};void* mapped=nullptr;
    if(FAILED(timingReadback_->Map(0,&range,&mapped))||!mapped)return false;
    struct Unmap {ID3D12Resource* resource;~Unmap(){const D3D12_RANGE noWrite{0,0};resource->Unmap(0,&noWrite);}} unmap{timingReadback_.Get()};
    const auto* values=static_cast<const UINT64*>(mapped);
    for(unsigned i=0;i<timingCount_;++i) {
        const auto& s=timingRecords_[i];if(!s.resolved)continue;
        const auto begin=values[2*i],end=values[2*i+1];if(!begin||end<begin)continue;
        out.push_back({s.kind,s.phase,s.input,double(end-begin)*1000.0/double(frequency)});
    }
    return true;
}
bool QualityGpu::ReleaseAfterGpu() {
    if(feature_&&!Ok(api_.release(feature_)))return false;
    feature_=nullptr;referenced_=false;
    if(params_){api_.destroy(params_);params_=nullptr;}
    return true;
}
}
