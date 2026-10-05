#pragma once
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <cmath>
#include <algorithm>

// No NGX initialization, DLL loading, resource allocation, or rendering here.
// Dispatch must come from the already initialized, build-verified MHWSS context.
namespace mhwsr {
using Create = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Evaluate = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    NVSDK_NGX_Parameter*, void*);
using Parameters = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using Destroy = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter*);
using Release = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);
struct Dispatch { Create create{}; Evaluate evaluate{}; Parameters capabilities{},allocate{}; Destroy destroy{}; Release release{}; };
struct Size { unsigned width=0,height=0; };
struct Plan {
    Size output{},optimal{},minimum{},maximum{},aligned{};
    int quality=2;
    float requestedScale=0;
};
inline bool Ok(NVSDK_NGX_Result r) { return NVSDK_NGX_SUCCEED(r); }
inline bool Inside(Size a,Size low,Size high) {
    return a.width>=low.width&&a.width<=high.width&&a.height>=low.height&&a.height<=high.height;
}
inline bool QualityPlanValid(const Plan& p) {
    return p.quality==2&&p.output.width>=1280&&p.output.height>=720&&p.output.width<=16384&&p.output.height<=16384&&
        p.optimal.width&&p.optimal.height&&p.optimal.width<p.output.width&&p.optimal.height<p.output.height&&
        p.minimum.width&&p.minimum.height&&Inside(p.optimal,p.minimum,p.maximum)&&
        p.maximum.width<=p.output.width&&p.maximum.height<=p.output.height;
}

// The callback is present on capability parameters, not necessarily on AllocateParameters.
inline bool QueryPlan(const Dispatch& api,Size output,int quality,Plan& plan) {
    if(!api.capabilities||!api.destroy||!output.width||!output.height||
       (quality!=0&&quality!=1&&quality!=2&&quality!=3))return false;
    NVSDK_NGX_Parameter* caps=nullptr;
    if(!Ok(api.capabilities(&caps))||!caps)return false;
    struct Owner { NVSDK_NGX_Parameter* p; Destroy destroy; ~Owner(){destroy(p);} } owner{caps,api.destroy};
    void* callback=nullptr;
    if(!Ok(caps->Get("DLSSOptimalSettingsCallback",&callback))||!callback)return false;
    caps->Set("Width",output.width);caps->Set("Height",output.height);
    caps->Set("PerfQualityValue",quality);caps->Set("RTXValue",0);
    using Optimal=NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter*);
    if(!Ok(reinterpret_cast<Optimal>(callback)(caps)))return false;
    Plan p{};p.output=output;p.quality=quality;
    if(!Ok(caps->Get("OutWidth",&p.optimal.width))||!Ok(caps->Get("OutHeight",&p.optimal.height)))return false;
    p.minimum=p.maximum=p.optimal;
    caps->Get("DLSS.Get.Dynamic.Min.Render.Width",&p.minimum.width);
    caps->Get("DLSS.Get.Dynamic.Min.Render.Height",&p.minimum.height);
    caps->Get("DLSS.Get.Dynamic.Max.Render.Width",&p.maximum.width);
    caps->Get("DLSS.Get.Dynamic.Max.Render.Height",&p.maximum.height);
    if(!p.optimal.width||!p.optimal.height||!Inside(p.optimal,p.minimum,p.maximum))return false;
    p.requestedScale=float(p.optimal.width)/float(output.width);
    // The existing engine setter clamps at 0.5 and aligns width down to 16 pixels.
    // Refuse unsupported modes instead of displaying a mode the engine cannot produce.
    plan=p;
    if(p.requestedScale<0.5f||p.requestedScale>=1.0f)return false;
    p.aligned.width=p.optimal.width&~15u;
    const float alignedScale=float(p.aligned.width)/float(output.width);
    p.aligned.height=unsigned(float(output.height)*alignedScale);
    if(!Inside(p.aligned,p.minimum,p.maximum)||!p.aligned.width||!p.aligned.height)return false;
    plan=p;return true;
}

// RAII only covers changed CPU parameters. Feature ownership stays with MHWSS;
// a future caller must retain its original release/synchronization path.
class CreateOverride {
    NVSDK_NGX_Parameter* p_{};
    unsigned w_=0,h_=0,preset_=0;
    int quality_=0;
public:
    CreateOverride(NVSDK_NGX_Parameter* p,const Plan& plan) {
        unsigned ow=0,oh=0;int flags=0;
        if(!p||!QualityPlanValid(plan)||!Ok(p->Get("Width",&w_))||!Ok(p->Get("Height",&h_))||
           !Ok(p->Get("OutWidth",&ow))||!Ok(p->Get("OutHeight",&oh))||
           !Ok(p->Get("PerfQualityValue",&quality_))||!Ok(p->Get("DLSS.Feature.Create.Flags",&flags))||
           !Ok(p->Get("DLSS.Hint.Render.Preset.Quality",&preset_))||
           ow!=plan.output.width||oh!=plan.output.height||flags!=0x4b||plan.quality!=2)return;
        // Preset K: initialize at the queried optimal size and evaluate the aligned
        // input subrect within the returned dynamic range. L/M are not enabled.
        p_=p;p->Set("Width",plan.optimal.width);p->Set("Height",plan.optimal.height);
        p->Set("PerfQualityValue",2);p->Set("DLSS.Hint.Render.Preset.Quality",11u);
    }
    CreateOverride(const CreateOverride&)=delete;
    CreateOverride& operator=(const CreateOverride&)=delete;
    ~CreateOverride(){if(p_){p_->Set("Width",w_);p_->Set("Height",h_);p_->Set("PerfQualityValue",quality_);p_->Set("DLSS.Hint.Render.Preset.Quality",preset_);}}
    explicit operator bool()const{return p_!=nullptr;}
};

struct FrameInput {
    Size render{};
    // These dimensions must be associated with this command list/frame, not read
    // from a later renderer-global scale. Observer v1 does not yet supply this token.
    bool frameAssociationVerified=false,reset=false;
};
class EvaluateOverride {
    NVSDK_NGX_Parameter* p_{};
    unsigned w_=0,h_=0;int reset_=0;
    float jx_=0,jy_=0,mx_=0,my_=0;
public:
    EvaluateOverride(NVSDK_NGX_Parameter* p,const Plan& plan,const FrameInput& frame) {
        if(!p||!QualityPlanValid(plan)||!frame.frameAssociationVerified||!Inside(frame.render,plan.minimum,plan.maximum)||
           frame.render.width>plan.optimal.width||frame.render.height>plan.optimal.height)return;
        ID3D12Resource *color=nullptr,*depth=nullptr,*motion=nullptr,*output=nullptr;
        if(!Ok(p->Get("Color",&color))||!Ok(p->Get("Depth",&depth))||
           !Ok(p->Get("MotionVectors",&motion))||!Ok(p->Get("Output",&output))||!color||!depth||!motion||!output)return;
        const auto c=color->GetDesc(),d=depth->GetDesc(),m=motion->GetDesc(),o=output->GetDesc();
        if(color==output||c.Format!=DXGI_FORMAT_R11G11B10_FLOAT||d.Format!=DXGI_FORMAT_R32_FLOAT||
           m.Format!=DXGI_FORMAT_R16G16_FLOAT||o.Format!=c.Format||o.Width!=plan.output.width||o.Height!=plan.output.height)return;
        for(const auto& desc:{c,d,m,o})if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.SampleDesc.Count!=1||
            desc.Width!=plan.output.width||desc.Height!=plan.output.height||desc.DepthOrArraySize!=1)return;
        // MHWSS uses origin zero for these full-sized allocations. Do not silently
        // reinterpret an unexpected base offset as the upper-left active rectangle.
        const char* baseKeys[]={"DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y",
            "DLSS.Input.Depth.Subrect.Base.X","DLSS.Input.Depth.Subrect.Base.Y",
            "DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y",
            "DLSS.Output.Subrect.Base.X","DLSS.Output.Subrect.Base.Y"};
        for(const auto* key:baseKeys){unsigned n=0;if(Ok(p->Get(key,&n))&&n!=0)return;}
        if(!Ok(p->Get("DLSS.Render.Subrect.Dimensions.Width",&w_))||!Ok(p->Get("DLSS.Render.Subrect.Dimensions.Height",&h_))||
           !Ok(p->Get("Jitter.Offset.X",&jx_))||!Ok(p->Get("Jitter.Offset.Y",&jy_))||
           !Ok(p->Get("MV.Scale.X",&mx_))||!Ok(p->Get("MV.Scale.Y",&my_))||!Ok(p->Get("Reset",&reset_))||
           !std::isfinite(jx_)||!std::isfinite(jy_)||std::fabs(mx_+0.5f)>0.00001f||std::fabs(my_-0.5f)>0.00001f)return;
        // MHWSS already multiplies NDC motion/jitter by the allocated dimensions.
        // Convert those units to active input pixels. Temporal validity still needs
        // actual in-game image inspection before this adapter is activated.
        const float sx=float(frame.render.width)/float(c.Width),sy=float(frame.render.height)/float(c.Height);
        p_=p;p->Set("DLSS.Render.Subrect.Dimensions.Width",frame.render.width);
        p->Set("DLSS.Render.Subrect.Dimensions.Height",frame.render.height);
        p->Set("Jitter.Offset.X",jx_*sx);p->Set("Jitter.Offset.Y",jy_*sy);
        p->Set("MV.Scale.X",mx_*sx);p->Set("MV.Scale.Y",my_*sy);
        p->Set("Reset",reset_||frame.reset?1:0);
    }
    EvaluateOverride(const EvaluateOverride&)=delete;
    EvaluateOverride& operator=(const EvaluateOverride&)=delete;
    ~EvaluateOverride(){if(p_){p_->Set("DLSS.Render.Subrect.Dimensions.Width",w_);p_->Set("DLSS.Render.Subrect.Dimensions.Height",h_);
        p_->Set("Jitter.Offset.X",jx_);p_->Set("Jitter.Offset.Y",jy_);p_->Set("MV.Scale.X",mx_);p_->Set("MV.Scale.Y",my_);p_->Set("Reset",reset_);}}
    explicit operator bool()const{return p_!=nullptr;}
};
}
