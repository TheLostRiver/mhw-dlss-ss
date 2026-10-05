#include "SrParameterAdapter.h"

// Compiled entry points for the next rendering integration. The current bridge
// preflight does not invoke these exports. No second NGX context or feature is made.
extern "C" __declspec(dllexport) NVSDK_NGX_Result MhwSrCreateQuality(
    mhwsr::Create next,ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** handle,const mhwsr::Plan* plan) {
    if(!next||!list||!params||!handle||!plan)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    mhwsr::CreateOverride override(params,*plan);
    if(!override)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    return next(list,NVSDK_NGX_Feature_SuperSampling,params,handle);
}
extern "C" __declspec(dllexport) NVSDK_NGX_Result MhwSrEvaluateQuality(
    mhwsr::Evaluate next,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* params,void* callback,const mhwsr::Plan* plan,const mhwsr::FrameInput* frame) {
    if(!next||!list||!handle||!params||!plan||!frame)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    mhwsr::EvaluateOverride override(params,*plan,*frame);
    if(!override)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    return next(list,handle,params,callback);
}
