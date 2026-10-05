// Observe the actual D3D12 compute CBV bindings even when MHWSS tracking is off.
// The buffer and layout were independently identified by the screen observer.
std::mutex g_screenReadMutex;
ID3D12Resource* g_screenArena{};
const unsigned char* g_screenMapped{};
UINT64 g_screenGpuBase=0,g_screenBytes=0;
std::atomic<uint64_t> g_taaScreenValid{0},g_taaScreenInvalid{0};
using ComputeCbv=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT64);
using ClearState=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
ComputeCbv g_computeCbv{};ClearState g_clearState{};
std::atomic<uint64_t> g_computeCbvCalls{0};
struct ExtraTaaConstant {unsigned root=0;UINT64 address=0;std::array<uint32_t,16> words{};bool copied=false;};
struct TaaScreenInput {
    UINT64 camera=0,screen=0;std::array<uint32_t,27> words{};unsigned error=0;
    float jitter[2]{},previousJitter[2]{};bool cameraValid=false;
    std::array<ExtraTaaConstant,4> extra{};
};
float ScreenFloat(const TaaScreenInput& s,unsigned byteOffset) {float value;memcpy(&value,&s.words[byteOffset/4],4);return value;}
ID3D12Resource* ReferenceScreenArena() noexcept {
    __try {auto* resource=*reinterpret_cast<ID3D12Resource**>(g_mhwss+0x54ebf8);if(resource)resource->AddRef();return resource;}
    __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
void STDMETHODCALLTYPE OnBridgeComputeCbv(ID3D12GraphicsCommandList* list,UINT root,UINT64 address) {
    if(g_ready.load()&&!g_done.load()&&root<16)try {
        ++g_computeCbvCalls;std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)) {
            auto& state=g_bridgeLists[list];state.cbvs[root]=address;state.cbvEpochs[root]=++state.bindEpoch;
        }else ++g_traceDrops;
    }catch(...){++g_traceDrops;}
    g_computeCbv(list,root,address);
}
void STDMETHODCALLTYPE OnBridgeClearState(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso) {
    g_clearState(list,pso);
    if(g_ready.load()&&!g_done.load())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(list);
        if(found!=g_bridgeLists.end()){const auto generation=found->second.generation+1;found->second={};found->second.generation=generation;found->second.pso=pso;}
    }catch(...){++g_traceDrops;}
}
bool CopyConstantBytes(const void* source,void* destination,size_t bytes) noexcept {
    __try {memcpy(destination,source,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void OpenScreenReader() {
    std::lock_guard<std::mutex> lock(g_screenReadMutex);
    g_screenArena=ReferenceScreenArena();if(!g_screenArena)throw std::runtime_error("MHWSS constant arena unavailable");
    const auto desc=g_screenArena->GetDesc();D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||desc.Width!=0x3000000||FAILED(g_screenArena->GetHeapProperties(&heap,&flags))||
       (heap.Type!=D3D12_HEAP_TYPE_UPLOAD&&!(heap.Type==D3D12_HEAP_TYPE_CUSTOM&&heap.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)))
        throw std::runtime_error("MHWSS constant arena layout or heap differs");
    g_screenBytes=desc.Width;g_screenGpuBase=g_screenArena->GetGPUVirtualAddress();
    D3D12_RANGE range{0,static_cast<SIZE_T>(g_screenBytes)};void* mapped=nullptr;
    if(!g_screenGpuBase||FAILED(g_screenArena->Map(0,&range,&mapped))||!mapped)throw std::runtime_error("MHWSS constant arena map failed");
    g_screenMapped=static_cast<const unsigned char*>(mapped);
    Log("{\"event\":\"taa_screen_reader_ready\",\"bytes\":"+std::to_string(g_screenBytes)+",\"writes_buffer\":false}");
}
TaaScreenInput ReadTaaScreen(ID3D12GraphicsCommandList* list) {
    TaaScreenInput out{};bool fresh=false;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(list);
        if(found==g_bridgeLists.end()){out.error=1;return out;}
        auto& state=found->second;out.camera=state.cbvs[2];out.screen=state.cbvs[3];
        fresh=state.cbvEpochs[2]>state.lastTaaEpoch&&state.cbvEpochs[3]>state.lastTaaEpoch;
        state.lastTaaEpoch=state.bindEpoch;
        unsigned extra=0;
        for(unsigned root=0;root<16&&extra<out.extra.size();++root)if(root!=2&&root!=3&&state.cbvs[root]) {
            out.extra[extra].root=root;out.extra[extra].address=state.cbvs[root];++extra;
        }
    }
    if(!out.camera||!out.screen){out.error=2;return out;}
    std::lock_guard<std::mutex> lock(g_screenReadMutex);
    if(!g_screenMapped||out.screen<g_screenGpuBase||out.screen-g_screenGpuBase>g_screenBytes-sizeof(out.words)||(out.screen&255))
        {out.error=3;return out;}
    if(!CopyConstantBytes(g_screenMapped+(out.screen-g_screenGpuBase),out.words.data(),sizeof(out.words))){out.error=4;return out;}
    if(out.camera>=g_screenGpuBase&&out.camera-g_screenGpuBase<=g_screenBytes-712&&(out.camera&255)==0) {
        const auto* camera=g_screenMapped+(out.camera-g_screenGpuBase);
        out.cameraValid=CopyConstantBytes(camera+160,out.jitter,8)&&CopyConstantBytes(camera+704,out.previousJitter,8);
        for(unsigned i=0;i<2;++i)out.cameraValid=out.cameraValid&&std::isfinite(out.jitter[i])&&std::isfinite(out.previousJitter[i])&&
            std::fabs(out.jitter[i])<0.1f&&std::fabs(out.previousJitter[i])<0.1f;
    }
    for(auto& extra:out.extra)if(extra.address>=g_screenGpuBase&&extra.address-g_screenGpuBase<=g_screenBytes-sizeof(extra.words)&&(extra.address&255)==0)
        extra.copied=CopyConstantBytes(g_screenMapped+(extra.address-g_screenGpuBase),extra.words.data(),sizeof(extra.words));
    const auto width=ScreenFloat(out,16),height=ScreenFloat(out,20),scale=ScreenFloat(out,84);
    const auto rw=out.words[10],rh=out.words[11];
    if(!std::isfinite(width)||!std::isfinite(height)||width<1280||height<720||width>16384||height>16384||
       !rw||!rh||rw>width||rh>height||out.words[8]||out.words[9]||!std::isfinite(scale)||scale<0.5f||scale>1.0f||
       !std::isfinite(ScreenFloat(out,56))||!std::isfinite(ScreenFloat(out,60))||
       std::fabs(ScreenFloat(out,56)-float(rw))>0.01f||std::fabs(ScreenFloat(out,60)-float(rh))>0.01f||
       !std::isfinite(ScreenFloat(out,24))||!std::isfinite(ScreenFloat(out,28))||
       std::fabs(ScreenFloat(out,24)*width-1)>0.001f||std::fabs(ScreenFloat(out,28)*height-1)>0.001f)
        out.error=5;
    if(!out.error&&!fresh)out.error=6;
    return out;
}
void RecordTaaScreen(const TaaTrace& trace,const TaaScreenInput& s) {
    if(s.error)++g_taaScreenInvalid;else ++g_taaScreenValid;
    if(trace.serial>32&&trace.serial%17!=0&&s.error)return;
    std::ostringstream out;out<<std::setprecision(9);
    out<<"{\"event\":\"taa_screen_input\",\"phase\":"<<g_phase.load()<<",\"taa_serial\":"<<trace.serial
       <<",\"thread\":"<<GetCurrentThreadId()<<",\"scene_invocation\":"<<trace.sceneInvocation<<",\"error\":"<<s.error
       <<",\"list\":\"0x"<<std::hex<<trace.list<<"\",\"camera_cbv\":\"0x"<<s.camera<<"\",\"screen_cbv\":\"0x"<<s.screen<<std::dec
       <<"\",\"dispatch\":["<<trace.groups[0]<<','<<trace.groups[1]<<','<<trace.groups[2]<<']';
    if(!s.error)out<<",\"output\":["<<ScreenFloat(s,16)<<','<<ScreenFloat(s,20)<<"],\"active_input\":["<<s.words[10]<<','<<s.words[11]
        <<"],\"actual_scale\":"<<ScreenFloat(s,84);
    out<<",\"camera_valid\":"<<(s.cameraValid?"true":"false");
    if(s.cameraValid){out<<",\"projection_jitter\":["<<s.jitter[0]<<','<<s.jitter[1]<<"],\"previous_projection_jitter\":["
        <<s.previousJitter[0]<<','<<s.previousJitter[1]<<']';
        if(!s.error)out<<",\"candidate_view_pixel_jitter\":["<<s.jitter[0]*float(s.words[10])*-0.5f<<','<<s.jitter[1]*float(s.words[11])*0.5f<<']';}
    out<<",\"additional_constants\":[";bool first=true;
    for(const auto& extra:s.extra)if(extra.address){if(!first)out<<',';first=false;out<<"{\"root\":"<<extra.root<<",\"address\":\"0x"<<std::hex
        <<extra.address<<std::dec<<"\",\"copied\":"<<(extra.copied?"true":"false")<<",\"raw_words\":[";
        for(unsigned i=0;i<extra.words.size();++i){if(i)out<<',';out<<extra.words[i];}out<<"]}";}
    out<<']';
    out<<",\"raw_words\":[";for(unsigned i=0;i<s.words.size();++i){if(i)out<<',';out<<s.words[i];}out<<"]}";
    SaveBridge(out.str());
}
void CloseScreenReader() noexcept {
    std::lock_guard<std::mutex> lock(g_screenReadMutex);
    if(g_screenArena){if(g_screenMapped){D3D12_RANGE noWrite{0,0};g_screenArena->Unmap(0,&noWrite);}g_screenArena->Release();}
    g_screenArena=nullptr;g_screenMapped=nullptr;g_screenBytes=g_screenGpuBase=0;
}
