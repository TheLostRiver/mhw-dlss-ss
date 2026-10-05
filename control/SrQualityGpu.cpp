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
bool Texture(ID3D12Device* device,Size s,DXGI_FORMAT format,ID3D12Resource** output) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=s.width;d.Height=s.height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
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
}
bool QualityGpu::Prepare(const Dispatch& api,const Plan& plan,ID3D12Device* device,ID3D12Resource* packed) {
    if(device_||!api.create||!api.evaluate||!api.allocate||!api.destroy||!api.release||!device||
       !QualityPlanValid(plan)||!FrameTexture(packed,plan.output,DXGI_FORMAT_R32_UINT))return false;
    api_=api;plan_=plan;device_=device;packed_=packed;
    if(!Texture(device,plan.output,DXGI_FORMAT_R16G16_FLOAT,&motion_)||
       !Texture(device,plan.output,DXGI_FORMAT_R11G11B10_FLOAT,&output_)||
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
NVSDK_NGX_Result QualityGpu::Evaluate(const QualityFrame& f) {
    if(!params_||!f.list||!f.associated||!Matches(f.packedMotion)||
       !Inside(f.render,plan_.minimum,plan_.maximum)||f.render.width!=plan_.aligned.width||f.render.height!=plan_.aligned.height||
       !FrameTexture(f.color,plan_.output,DXGI_FORMAT_R11G11B10_FLOAT)||
       !FrameTexture(f.depth,plan_.output,DXGI_FORMAT_R32_FLOAT)||
       !FrameTexture(f.taaOutput,plan_.output,DXGI_FORMAT_R11G11B10_FLOAT)||f.color==f.taaOutput)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    for(unsigned i=0;i<2;++i)if(!std::isfinite(f.jitter[i])||!std::isfinite(f.previousJitter[i]))return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto creation=Prime(f.list);if(!Ok(creation))return creation;
    auto* list=f.list;
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list,f.packedMotion,f.motionState,read);
    ID3D12DescriptorHeap* heaps[]={heap_.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(decodeRoot_.Get());list->SetPipelineState(decodePso_.Get());
    list->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());
    struct Constants {unsigned w,h,ow,oh;float x,y,px,py;} values{f.render.width,f.render.height,plan_.output.width,plan_.output.height,
        f.jitter[0],f.jitter[1],f.previousJitter[0],f.previousJitter[1]};
    list->SetComputeRoot32BitConstants(1,8,&values,0);list->Dispatch((f.render.width+7)/8,(f.render.height+7)/8,1);
    Transition(list,motion_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);
    Transition(list,f.packedMotion,read,f.motionState);
    Transition(list,f.color,f.colorState,read);Transition(list,f.depth,f.depthState,read);
    params_->Set("Color",f.color);params_->Set("Depth",f.depth);params_->Set("MotionVectors",motion_.Get());params_->Set("Output",output_.Get());
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
    Transition(list,f.color,read,f.colorState);Transition(list,f.depth,read,f.depthState);
    Transition(list,motion_.Get(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
bool QualityGpu::ReleaseAfterGpu() {
    if(feature_&&!Ok(api_.release(feature_)))return false;
    feature_=nullptr;referenced_=false;
    if(params_){api_.destroy(params_);params_=nullptr;}
    return true;
}
}
