#pragma once
#include "SrParameterAdapter.h"
#include <wrl/client.h>
#include <cstdint>

namespace mhwsr {
// One bounded Quality session on the host's already initialized NGX dispatch.
// No Init/Shutdown, no MHWSS mode change, no DLAA or frame-generation feature.
struct QualityFrame {
    ID3D12GraphicsCommandList* list{};
    ID3D12Resource *color{},*depth{},*packedMotion{},*taaOutput{};
    D3D12_RESOURCE_STATES colorState{},depthState{},motionState{},outputState{};
    Size render{};
    float jitter[2]{},previousJitter[2]{};
    bool associated=false,reset=true;
};
class QualityGpu {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Dispatch api_{};
    Plan plan_{};
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12Resource> motion_,output_,vertices_,packed_;
    Ptr<ID3D12DescriptorHeap> heap_;
    Ptr<ID3D12RootSignature> decodeRoot_;
    Ptr<ID3D12PipelineState> decodePso_;
    Ptr<ID3D12Fence> fence_;
    NVSDK_NGX_Parameter* params_{};
    NVSDK_NGX_Handle* feature_{};
    bool referenced_=false;
    UINT stride_=0;
    D3D12_VERTEX_BUFFER_VIEW quad_{},triangle_{};
public:
    QualityGpu()=default;
    QualityGpu(const QualityGpu&)=delete;
    QualityGpu& operator=(const QualityGpu&)=delete;
    ~QualityGpu();
    // Preparation only creates CPU objects/resources. Prime/Evaluate mark them as
    // potentially GPU-referenced BEFORE CreateFeature, including failed calls.
    bool Prepare(const Dispatch&,const Plan&,ID3D12Device*,ID3D12Resource* packed);
    NVSDK_NGX_Result Prime(ID3D12GraphicsCommandList*);
    NVSDK_NGX_Result Evaluate(const QualityFrame&);
    bool Matches(ID3D12Resource* packed)const{return packed_.Get()==packed;}
    bool Referenced()const{return referenced_;}
    ID3D12Fence* Fence()const{return fence_.Get();}
    ID3D12Device* Device()const{return device_.Get();}
    const Plan& Settings()const{return plan_;}
    const D3D12_VERTEX_BUFFER_VIEW& Quad()const{return quad_;}
    const D3D12_VERTEX_BUFFER_VIEW& Triangle()const{return triangle_;}
    // Call only after all recorded uses are submitted and their fence completes.
    // On uncertainty retain this object until process exit, never a timed release.
    bool ReleaseAfterGpu();
};
}
