// Included inside the native host's anonymous namespace.
// Only the fresh stack result of the verified projection getter can be written.
// Dimensions and the target view are learned from untouched TAA inputs first.
enum class ProjectionPhase { Idle, Baseline, Pulse, Recovery };
const char* PhaseName(ProjectionPhase phase) {
    switch(phase){case ProjectionPhase::Baseline:return "baseline";case ProjectionPhase::Pulse:return "pulse";
        case ProjectionPhase::Recovery:return "recovery";default:return "idle";}
}
struct ProjectionView {
    uintptr_t context=0;
    unsigned slot=0,type=0;
    int rect[4]{};
    float matrix[16]{};
    uint64_t seenAt=0,calls=0,matches=0;
    bool scratch=false,perspective=false;
};
struct ProjectionCapture {
    ProjectionPhase phase=ProjectionPhase::Idle;
    bool requested=false,havePrevious=false,faulted=false;
    unsigned width=0,height=0,targetSlot=8,patternMask=0;
    uintptr_t targetContext=0;
    uint64_t deadline=0,lastInputAt=0,lastSerial=0,phaseIndex=0;
    uint64_t baselineInputs=0,baselineZero=0,ambiguousInputs=0,pulseInputs=0,pulseNonzero=0;
    uint64_t patternMatches=0,historyPairs=0,historyMatches=0,pulseHistoryPairs=0,pulseHistoryMatches=0;
    uint64_t recoveryInputs=0,recoveryZero=0,recoveryZeroStreak=0;
    uint64_t writes=0,rejected=0,duplicates=0,viewOverflow=0,lastWritePhase=UINT64_MAX;
    float lastCurrent[2]{};
    std::array<ProjectionView,32> views{};
    const char* reason="none";
};
std::mutex g_projectionMutex;
ProjectionCapture g_projection;
std::atomic<bool> g_projectionCapturing{false},g_projectionFaulted{false};

bool Near(float a,float b,float epsilon=0.00000001f) {return std::fabs(a-b)<=epsilon;}
bool ZeroJitter(const float* matrix) {return Near(matrix[8],0)&&Near(matrix[9],0);}
bool Perspective(const float* matrix) {
    for(unsigned i=0;i<16;++i)if(!std::isfinite(matrix[i])||std::fabs(matrix[i])>1000000)return false;
    return std::fabs(matrix[0])>0.0001f&&std::fabs(matrix[5])>0.0001f&&
        Near(std::fabs(matrix[11]),1,0.00001f)&&Near(matrix[15],0);
}
bool SameProjection(const float* a,const float* b) {
    for(unsigned i=0;i<16;++i)if(!Near(a[i],b[i],0.000001f))return false;
    return true;
}
float Halton(unsigned index,unsigned base) {
    float value=0,factor=1;
    while(index){factor/=float(base);value+=factor*float(index%base);index/=base;}
    return value-0.5f;
}
void ProjectionOffset(unsigned phase,unsigned width,unsigned height,float* out) {
    out[0]=-2.0f*Halton(phase+1,2)/float(width);
    out[1]= 2.0f*Halton(phase+1,3)/float(height);
}
bool FullView(const ProjectionView& view,unsigned width,unsigned height) {
    return view.scratch&&view.perspective&&view.rect[0]==0&&view.rect[1]==0&&
        view.rect[2]==int(width)&&view.rect[3]==int(height);
}
void ProjectionFault(const char* reason) {
    // Caller holds g_projectionMutex. A fault immediately makes subsequent calls read-only.
    g_projection.faulted=true;g_projection.reason=reason;
    g_projection.phase=ProjectionPhase::Recovery;g_projectionFaulted.store(true);
}
void BeginProjectionCapture(bool requested) {
    std::lock_guard<std::mutex> lock(g_projectionMutex);
    g_projection={};g_projection.requested=requested;g_projection.phase=ProjectionPhase::Baseline;
    g_projectionFaulted.store(false);g_projectionCapturing.store(true);
}
void ObserveProjection(float* result,const unsigned char* view,float* scratch,unsigned slot) {
    if(!g_projectionCapturing.load())return;Busy busy;
    ProjectionView observed;observed.context=reinterpret_cast<uintptr_t>(view);observed.slot=slot;
    observed.scratch=result==scratch;observed.seenAt=GetTickCount64();unsigned char type=0;
    const bool readable=slot<8&&view&&result&&CopyBytes(&type,view+0x74,1)&&
        CopyBytes(observed.rect,view+0x78,sizeof(observed.rect))&&CopyBytes(observed.matrix,result,sizeof(observed.matrix));
    observed.type=type;observed.perspective=readable&&Perspective(observed.matrix);
    std::lock_guard<std::mutex> lock(g_projectionMutex);
    auto& capture=g_projection;if(!g_projectionCapturing.load()||slot>=8)return;
    // A TAA callback on another thread may have advanced lastInputAt while we
    // copied the matrix. Compare timestamps obtained under the same lock.
    observed.seenAt=GetTickCount64();
    if(capture.phase==ProjectionPhase::Baseline){
        // The producer has eight slots per owner; auxiliary owners can reuse a slot.
        auto saved=std::find_if(capture.views.begin(),capture.views.end(),[&](const ProjectionView& v){
            return v.calls&&v.context==observed.context&&v.slot==slot;});
        if(saved==capture.views.end())saved=std::find_if(capture.views.begin(),capture.views.end(),[](const ProjectionView& v){return !v.calls;});
        if(saved==capture.views.end()){++capture.viewOverflow;return;}
        observed.calls=saved->calls+1;observed.matches=saved->matches;*saved=observed;return;
    }
    if(capture.phase!=ProjectionPhase::Pulse||!capture.requested)return;
    if(observed.seenAt>=capture.deadline){capture.phase=ProjectionPhase::Recovery;return;}
    if(slot!=capture.targetSlot||observed.context!=capture.targetContext)return;
    if(!readable||!FullView(observed,capture.width,capture.height)||
       !ZeroJitter(observed.matrix)||observed.seenAt-capture.lastInputAt>500) {
        ++capture.rejected;ProjectionFault("projection_layout_or_input_changed");return;
    }
    float offset[2]{};ProjectionOffset(unsigned(capture.phaseIndex%8),capture.width,capture.height,offset);
    // Require the unmodified result to contain exactly zero offsets. The getter
    // owns this stack result; the persistent camera object and uploaded CBs are untouched.
    if(observed.matrix[8]!=0||observed.matrix[9]!=0){++capture.rejected;ProjectionFault("nonzero_original_projection");return;}
    if(!CopyBytes(result+8,offset,sizeof(offset))){++capture.rejected;ProjectionFault("projection_write_failed");return;}
    if(capture.lastWritePhase==capture.phaseIndex)++capture.duplicates;
    capture.lastWritePhase=capture.phaseIndex;++capture.writes;
}
void ObserveProjectionInput(bool valid,unsigned width,unsigned height,unsigned inputWidth,unsigned inputHeight,
                            const float* current,const float* previous,uint64_t serial) {
    if(!g_projectionCapturing.load())return;
    std::lock_guard<std::mutex> lock(g_projectionMutex);auto& capture=g_projection;
    if(!g_projectionCapturing.load())return;
    const auto now=GetTickCount64();
    if(!valid||!Perspective(current)||!Perspective(previous)||width!=inputWidth||height!=inputHeight){
        capture.havePrevious=false;capture.recoveryZeroStreak=0;
        if(capture.phase==ProjectionPhase::Pulse)ProjectionFault("taa_input_invalid_or_dimensions_changed");
        return;
    }
    if(capture.phase==ProjectionPhase::Baseline){
        ++capture.baselineInputs;if(ZeroJitter(current)&&ZeroJitter(previous))++capture.baselineZero;
        if(capture.width&& (capture.width!=width||capture.height!=height)){
            capture.baselineInputs=1;capture.baselineZero=ZeroJitter(current)&&ZeroJitter(previous)?1:0;
            for(auto& view:capture.views)view.matches=0;
        }
        capture.width=width;capture.height=height;
        unsigned matches=0,index=0;
        for(unsigned i=0;i<capture.views.size();++i){const auto& view=capture.views[i];
            if(now-view.seenAt<250&&FullView(view,width,height)&&SameProjection(view.matrix,current)){++matches;index=i;}}
        if(matches==1)++capture.views[index].matches;else if(matches>1)++capture.ambiguousInputs;
    }else if(capture.phase==ProjectionPhase::Pulse){
        if(width!=capture.width||height!=capture.height){ProjectionFault("taa_dimensions_changed");return;}
        ++capture.pulseInputs;if(!ZeroJitter(current))++capture.pulseNonzero;
        for(unsigned i=0;i<8;++i){float offset[2]{};ProjectionOffset(i,width,height,offset);
            if(Near(current[8],offset[0])&&Near(current[9],offset[1])){
                ++capture.patternMatches;capture.patternMask|=1u<<i;break;}}
        // Advance only at the identified TAA dispatch, not once per auxiliary view.
        ++capture.phaseIndex;
    }else if(capture.phase==ProjectionPhase::Recovery){
        ++capture.recoveryInputs;
        if(ZeroJitter(current)&&ZeroJitter(previous)){++capture.recoveryZero;++capture.recoveryZeroStreak;}
        else capture.recoveryZeroStreak=0;
    }
    if(capture.havePrevious&&serial==capture.lastSerial+1){
        const bool matches=Near(previous[8],capture.lastCurrent[0])&&Near(previous[9],capture.lastCurrent[1]);
        ++capture.historyPairs;if(matches)++capture.historyMatches;
        if(capture.phase==ProjectionPhase::Pulse){++capture.pulseHistoryPairs;if(matches)++capture.pulseHistoryMatches;}
    }
    capture.lastCurrent[0]=current[8];capture.lastCurrent[1]=current[9];capture.lastSerial=serial;
    capture.havePrevious=true;capture.lastInputAt=now;
}
bool StartProjectionPulse() {
    std::lock_guard<std::mutex> lock(g_projectionMutex);auto& capture=g_projection;
    unsigned matched=0,slot=8;
    for(unsigned i=0;i<capture.views.size();++i)if(capture.views[i].matches>=16){++matched;slot=i;}
    if(!g_projectionCapturing.load()||g_projectionFaulted.load()||!capture.requested||
       capture.baselineInputs<16||capture.baselineZero!=capture.baselineInputs||
       capture.ambiguousInputs||capture.viewOverflow||matched!=1||GetTickCount64()-capture.lastInputAt>250){
        ProjectionFault("baseline_has_no_unique_unjittered_full_view");return false;
    }
    capture.targetSlot=capture.views[slot].slot;capture.targetContext=capture.views[slot].context;
    capture.phase=ProjectionPhase::Pulse;capture.deadline=GetTickCount64()+10000;return true;
}
void EndProjectionPulse() {
    std::lock_guard<std::mutex> lock(g_projectionMutex);
    if(g_projection.phase!=ProjectionPhase::Idle)g_projection.phase=ProjectionPhase::Recovery;
}
void EndProjectionCapture() {
    std::lock_guard<std::mutex> lock(g_projectionMutex);
    g_projectionCapturing.store(false);g_projection.phase=ProjectionPhase::Idle;
}
std::string ProjectionPhaseName() {
    std::lock_guard<std::mutex> lock(g_projectionMutex);return PhaseName(g_projection.phase);
}
void LogProjectionSummary(uint64_t session) {
    std::lock_guard<std::mutex> lock(g_projectionMutex);const auto& c=g_projection;
    std::ostringstream out;out<<"{\"event\":\"native_projection_summary\",\"session\":"<<session
        <<",\"requested\":"<<(c.requested?"true":"false")<<",\"faulted\":"<<((c.faulted||g_projectionFaulted.load())?"true":"false")
        <<",\"reason\":\""<<c.reason<<"\",\"input\":["<<c.width<<','<<c.height<<"],\"target_slot\":"<<c.targetSlot
        <<",\"baseline_inputs\":"<<c.baselineInputs<<",\"baseline_zero\":"<<c.baselineZero<<",\"ambiguous_inputs\":"<<c.ambiguousInputs
        <<",\"view_registry_overflow\":"<<c.viewOverflow
        <<",\"projection_writes\":"<<c.writes<<",\"rejected_writes\":"<<c.rejected<<",\"same_phase_extra_writes\":"<<c.duplicates
        <<",\"pulse_inputs\":"<<c.pulseInputs<<",\"pulse_nonzero\":"<<c.pulseNonzero<<",\"pattern_matches\":"<<c.patternMatches
        <<",\"pattern_mask\":"<<c.patternMask<<",\"history_pairs\":"<<c.historyPairs<<",\"history_matches\":"<<c.historyMatches
        <<",\"pulse_history_pairs\":"<<c.pulseHistoryPairs<<",\"pulse_history_matches\":"<<c.pulseHistoryMatches
        <<",\"recovery_inputs\":"<<c.recoveryInputs<<",\"recovery_zero\":"<<c.recoveryZero
        <<",\"recovery_confirmed\":"<<((c.recoveryZeroStreak>=2&&GetTickCount64()-c.lastInputAt<250)?"true":"false")
        <<",\"sr_execution\":false}";Log(out.str());
    for(const auto& view:c.views)if(view.calls){
        std::ostringstream item;item<<std::setprecision(9)<<"{\"event\":\"native_projection_view\",\"session\":"<<session
            <<",\"slot\":"<<view.slot<<",\"type\":"<<view.type<<",\"rect\":["<<view.rect[0]<<','<<view.rect[1]<<','<<view.rect[2]<<','<<view.rect[3]
            <<"],\"scratch_matches\":"<<(view.scratch?"true":"false")<<",\"perspective\":"<<(view.perspective?"true":"false")
            <<",\"calls\":"<<view.calls<<",\"taa_projection_matches\":"<<view.matches<<",\"matrix\":[";
        for(unsigned i=0;i<16;++i){if(i)item<<',';if(std::isfinite(view.matrix[i]))item<<view.matrix[i];else item<<"null";}
        item<<"]}";Log(item.str());
    }
}
