// Included inside MhwSrBridge.cpp's anonymous namespace after QuadData.
// These callbacks observe the original MHWSS callback ABI, not D3D12 replacements.
using PsoCallback=bool(__fastcall*)(void*,ID3D12GraphicsCommandList*,ID3D12PipelineState*);
using DispatchCallback=bool(__fastcall*)(void*,ID3D12GraphicsCommandList*,UINT,UINT,UINT);
using ResetCallback=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
PsoCallback g_psoCallback{};DispatchCallback g_dispatchCallback{};ResetCallback g_listReset{};
unsigned char* g_mhwss{};
mhwsr::Dispatch g_ngx{};
std::mutex g_bridgeMutex;
struct ListTrace {ID3D12PipelineState* pso{};D3D12_VIEWPORT view{};bool hasView=false;uint64_t generation=0;};
std::unordered_map<ID3D12GraphicsCommandList*,ListTrace> g_bridgeLists;
struct TaaTrace {
    uintptr_t list=0,pso=0;UINT groups[3]{};D3D12_VIEWPORT view{};
    bool hasView=false,handled=false;uint64_t serial=0,tick=0,engineUpdate=0,generation=0;
    unsigned size[2]{};float scales[3]{};uint64_t sceneInvocation=0;
};
thread_local TaaTrace g_threadTaa{};
using ScenePass=uintptr_t(__fastcall*)(void*,void*,unsigned,uintptr_t);
ScenePass g_scenePass{};
struct SceneInvocation {uint64_t serial=0;uintptr_t context=0,scene=0;int outputRect[4]{},inputRect[4]{};float scale=0;};
thread_local SceneInvocation g_sceneInvocation{};
std::atomic<uint64_t> g_scenePasses{0},g_matchingInvocations{0};
bool ReadSceneInvocation(void* context,void* scene,SceneInvocation* out) noexcept {
    __try {
        auto* renderer=static_cast<unsigned char*>(Renderer());if(!renderer||!scene||!context)return false;
        if(scene!=*reinterpret_cast<void**>(renderer+0x1f450))return false;
        memcpy(out->outputRect,static_cast<unsigned char*>(context)+0x5c,16);
        memcpy(&out->scale,static_cast<unsigned char*>(scene)+0x6b8,4);
        out->context=reinterpret_cast<uintptr_t>(context);out->scene=reinterpret_cast<uintptr_t>(scene);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uintptr_t __fastcall OnScenePass(void* context,void* scene,unsigned flags,uintptr_t tag) {
    struct RestoreScope {
        SceneInvocation invocation;TaaTrace taa;
        ~RestoreScope(){g_sceneInvocation=invocation;g_threadTaa=taa;}
    } restore{g_sceneInvocation,g_threadTaa};
    g_sceneInvocation={};g_threadTaa={};
    if(g_ready.load()&&!g_done.load()) {
        SceneInvocation candidate{};
        if(ReadSceneInvocation(context,scene,&candidate)&&std::isfinite(candidate.scale)&&candidate.scale>=0.5f&&candidate.scale<=1.0f&&
           candidate.outputRect[0]==0&&candidate.outputRect[1]==0&&candidate.outputRect[2]>=1280&&candidate.outputRect[2]<=16384&&
           candidate.outputRect[3]>=720&&candidate.outputRect[3]<=16384) {
            // Match 0x23c38a8..0x23c3908: single-precision multiply followed
            // by cvttss2si (truncate), then preserve that source rectangle on stack.
            for(unsigned i=0;i<4;++i)candidate.inputRect[i]=int(float(candidate.outputRect[i])*candidate.scale);
            candidate.serial=++g_scenePasses;g_sceneInvocation=candidate;
        }
    }
    return g_scenePass(context,scene,flags,tag);
}
std::atomic<uint64_t> g_taaDispatches{0},g_correlations{0},g_missingTaa{0},g_traceDrops{0},g_nativeBypasses{0};
std::atomic<bool> g_queried{false};
std::vector<std::string> g_bridgeRecords;

bool MainState(TaaTrace* trace) noexcept {
    __try {
        auto* renderer=static_cast<unsigned char*>(Renderer());if(!renderer)return false;
        memcpy(trace->size,renderer+0x198,8);
        auto* scene=*reinterpret_cast<unsigned char**>(renderer+0x1f450);
        if(!scene)return false;memcpy(trace->scales,scene+0x6b0,12);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeTaaPso(ID3D12PipelineState* pso) noexcept {
    __try {return pso&&g_mhwss&&(pso==*reinterpret_cast<ID3D12PipelineState**>(g_mhwss+0x54ec00)||
        pso==*reinterpret_cast<ID3D12PipelineState**>(g_mhwss+0x54ec08));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void SaveBridge(const std::string& row) {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    if(g_bridgeRecords.size()<768)g_bridgeRecords.push_back(row);else ++g_traceDrops;
}
void QueryBridgePlans(const TaaTrace& t) {
    if(!t.size[0]||!t.size[1]||t.size[0]>16384||t.size[1]>16384)return;
    if(g_queried.exchange(true))return;
    // Called from MHWSS's existing render callback after its NGX initialization.
    // Only capability parameters are allocated and destroyed, never a feature.
    for(const int quality:{2,1,0,3}) {
        mhwsr::Plan p{};const bool accepted=mhwsr::QueryPlan(g_ngx,{t.size[0],t.size[1]},quality,p);
        std::ostringstream out;
        out<<"{\"event\":\"bridge_plan\",\"quality\":"<<quality<<",\"engine_compatible\":"<<(accepted?"true":"false")
           <<",\"output\":["<<t.size[0]<<','<<t.size[1]<<"],\"optimal\":["<<p.optimal.width<<','<<p.optimal.height
           <<"],\"minimum\":["<<p.minimum.width<<','<<p.minimum.height<<"],\"maximum\":["<<p.maximum.width<<','<<p.maximum.height
           <<"],\"aligned_candidate\":["<<p.aligned.width<<','<<p.aligned.height<<"],\"creates_feature\":false}";
        SaveBridge(out.str());
    }
}
bool __fastcall OnMhwPso(void* self,ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso) {
    if(g_ready.load()&&!g_done.load())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list))g_bridgeLists[list].pso=pso;else ++g_traceDrops;
    }catch(...){++g_traceDrops;}
    return g_psoCallback(self,list,pso);
}
bool __fastcall OnMhwDispatch(void* self,ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z) {
    TaaTrace trace{};bool matched=false;
    if(g_ready.load()&&!g_done.load())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        const auto found=g_bridgeLists.find(list);
        if(found!=g_bridgeLists.end()&&NativeTaaPso(found->second.pso)) {
            trace.list=reinterpret_cast<uintptr_t>(list);trace.pso=reinterpret_cast<uintptr_t>(found->second.pso);
            trace.view=found->second.view;trace.hasView=found->second.hasView;trace.generation=found->second.generation;matched=true;
        }
    }catch(...){++g_traceDrops;}
    const auto result=g_dispatchCallback(self,list,x,y,z);
    if(matched)try {
        trace.groups[0]=x;trace.groups[1]=y;trace.groups[2]=z;trace.handled=result;
        trace.serial=++g_taaDispatches;trace.tick=GetTickCount64();trace.engineUpdate=g_engineUpdates.load();
        trace.sceneInvocation=g_sceneInvocation.serial;
        MainState(&trace);g_threadTaa=trace;if(result)++g_nativeBypasses;
        QueryBridgePlans(trace);
    }catch(...){++g_traceDrops;}
    return result;
}
HRESULT STDMETHODCALLTYPE OnBridgeReset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pso) {
    const auto result=g_listReset(list,allocator,pso);
    if(SUCCEEDED(result)&&g_ready.load()&&!g_done.load())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        const auto found=g_bridgeLists.find(list);
        if(found!=g_bridgeLists.end()){const auto generation=found->second.generation+1;found->second={};found->second.generation=generation;}
        if(g_threadTaa.list==reinterpret_cast<uintptr_t>(list))g_threadTaa={};
    }catch(...){++g_traceDrops;}
    return result;
}
void RecordBridgeViewport(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* view) {
    if(!g_ready.load()||g_done.load()||count!=1||!view)return;
    try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){auto& trace=g_bridgeLists[list];trace.view=view[0];trace.hasView=true;}
    }catch(...){++g_traceDrops;}
}
void RecordBridgeQuad(uintptr_t caller,void* context,const QuadData& d) {
    if(caller!=0x23c7b08||!d.contextScene||d.contextScene!=d.mainScene)return;
    const auto trace=g_threadTaa;g_threadTaa={};
    if(!trace.serial){++g_missingTaa;return;}
    ++g_correlations;
    const bool sameInvocation=trace.sceneInvocation&&trace.sceneInvocation==g_sceneInvocation.serial&&
        g_sceneInvocation.context==reinterpret_cast<uintptr_t>(context)&&g_sceneInvocation.scene==d.contextScene&&
        !memcmp(g_sceneInvocation.inputRect,d.rect,16)&&!memcmp(g_sceneInvocation.outputRect,d.contextRect,16);
    if(sameInvocation)++g_matchingInvocations;
    const auto age=GetTickCount64()-trace.tick;
    std::ostringstream out;out<<std::setprecision(9);
    out<<"{\"event\":\"taa_quad_order\",\"phase\":"<<g_phase.load()<<",\"thread\":"<<GetCurrentThreadId()
       <<",\"taa_serial\":"<<trace.serial<<",\"age_ms\":"<<age<<",\"taa_engine_update\":"<<trace.engineUpdate
       <<",\"quad_engine_update\":"<<g_engineUpdates.load()<<",\"list\":\"0x"<<std::hex<<trace.list<<"\",\"pso\":\"0x"<<trace.pso<<std::dec
       <<"\",\"list_generation\":"<<trace.generation<<",\"taa_handled_by_mhwss\":"<<(trace.handled?"true":"false")
       <<",\"dispatch\":["<<trace.groups[0]<<','<<trace.groups[1]<<','<<trace.groups[2]<<"],\"source_rect\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';out<<d.rect[i];}
    out<<"],\"texture_size\":["<<d.texture[0]<<','<<d.texture[1]<<"],\"taa_scene_scale\":[";
    for(unsigned i=0;i<3;++i){if(i)out<<',';if(std::isfinite(trace.scales[i]))out<<trace.scales[i];else out<<"null";}
    out<<"],\"quad_scene_scale\":[";
    for(unsigned i=0;i<3;++i){if(i)out<<',';if(std::isfinite(d.scale[i]))out<<d.scale[i];else out<<"null";}
    out<<"],\"last_viewport\":";
    if(trace.hasView)out<<'['<<trace.view.TopLeftX<<','<<trace.view.TopLeftY<<','<<trace.view.Width<<','<<trace.view.Height<<']';else out<<"null";
    out<<",\"taa_scene_invocation\":"<<trace.sceneInvocation<<",\"quad_scene_invocation\":"<<g_sceneInvocation.serial
       <<",\"same_scene_invocation_and_rect\":"<<(sameInvocation?"true":"false")
       <<",\"scope_input_rect\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';out<<g_sceneInvocation.inputRect[i];}
    out<<"],\"frame_association_proven\":false,\"changes_copy\":false,\"evaluates_sr\":false}";
    SaveBridge(out.str());
}
template<class Fn> void BridgeHook(size_t index,void* target,const std::array<unsigned char,16>& signature,void* detour,Fn& original) {
    auto& hook=g_hooks[index];hook.address=target;hook.original=signature;
    if(memcmp(target,signature.data(),16))throw std::runtime_error("Bridge hook target signature differs");
    const auto status=MH_CreateHook(target,detour,reinterpret_cast<void**>(&original));
    if(status!=MH_OK)throw std::runtime_error("Bridge hook reservation failed");
}
void PrepareSceneScopeHook() {
    // Followed the PE chained unwind metadata to the real prologue, not a middle fragment.
    const std::array<unsigned char,16> signature{0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x41,0x54,0x41,0x55};
    BridgeHook(6,reinterpret_cast<unsigned char*>(g_game)+0x23c3790,signature,reinterpret_cast<void*>(&OnScenePass),g_scenePass);
}
bool KnownNgxPointer(void* pointer,bool allowProbe) {
    HMODULE module{};
    if(!pointer||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(pointer),&module))return false;
    const auto path=ModulePath(module);const auto name=path.filename().wstring();bool ok=false;
    // Preserve the known parameter observer chain; never overwrite any dispatch slot.
    if(allowProbe&&_wcsicmp(name.c_str(),L"MhwSrProbe.dll")==0)
        ok=FileSha256(path)=="42de0846e46a593fd1b3743abd92e1e0b098760b575322867977127a750618c7";
    else if(_wcsicmp(name.c_str(),L"d3d12.dll")==0)
        ok=FileSha256(path)=="73cf97e5c1a3db2be778df25d21b2999e664a06c6e0f64e67741987a21228a24";
    else ok=_wcsicmp(name.c_str(),L"_nvngx.dll")==0||_wcsicmp(name.c_str(),L"nvngx.dll")==0||module==reinterpret_cast<HMODULE>(g_mhwss);
    FreeLibrary(module);return ok;
}
void PrepareBridgeHooks() {
    const auto module=GetModuleHandleW(L"MHWSS.dll");
    if(!module||FileSha256(ModulePath(module))!="55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a")
        throw std::runtime_error("MHWSS build mismatch");
    g_mhwss=reinterpret_cast<unsigned char*>(module);
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module),&pinned))throw std::runtime_error("MHWSS module pin failed");
    const std::array<unsigned char,16> pso{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x02,0x4c,0x8d,0x4c};
    const std::array<unsigned char,16> dispatch{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57};
    const std::array<unsigned char,16> reset{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57};
    BridgeHook(3,g_mhwss+0xf0b40,pso,reinterpret_cast<void*>(&OnMhwPso),g_psoCallback);
    BridgeHook(4,g_mhwss+0xf1360,dispatch,reinterpret_cast<void*>(&OnMhwDispatch),g_dispatchCallback);
    auto* core=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"D3D12Core.dll"));
    BridgeHook(5,core+0x131010,reset,reinterpret_cast<void*>(&OnBridgeReset),g_listReset);
    // Exact rip-relative loads, verified separately from the lifecycle loader.
    const std::array<unsigned char,7> caps{0x48,0x8b,0x1d,0x2c,0x80,0x25,0x00};
    const std::array<unsigned char,7> destroy{0x48,0x8b,0x1d,0x5e,0x81,0x25,0x00};
    if(memcmp(g_mhwss+0x301aed,caps.data(),7)||memcmp(g_mhwss+0x3019c3,destroy.data(),7))
        throw std::runtime_error("NGX parameter dispatch signatures differ");
    const auto capPointer=*reinterpret_cast<void**>(g_mhwss+0x559b20);
    const auto destroyPointer=*reinterpret_cast<void**>(g_mhwss+0x559b28);
    if(!KnownNgxPointer(capPointer,false)||!KnownNgxPointer(destroyPointer,false))
        throw std::runtime_error("NGX parameter dispatch owner differs");
    g_ngx.capabilities=reinterpret_cast<mhwsr::Parameters>(capPointer);g_ngx.destroy=reinterpret_cast<mhwsr::Destroy>(destroyPointer);
    Log("{\"event\":\"bridge_preflight_prepared\",\"version\":1,\"modifies_ngx_dispatch\":false,\"changes_copy\":false,\"queries_capabilities\":true,\"evaluates_sr\":false}");
}
void FinishBridge() {
    try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        for(const auto& row:g_bridgeRecords)Log(row);
        Log("{\"event\":\"bridge_order_summary\",\"taa_dispatches\":"+std::to_string(g_taaDispatches.load())+
            ",\"same_thread_taa_quad_pairs\":"+std::to_string(g_correlations.load())+",\"quads_without_thread_taa\":"+std::to_string(g_missingTaa.load())+
            ",\"scene_invocations\":"+std::to_string(g_scenePasses.load())+",\"same_invocation_and_rect_pairs\":"+std::to_string(g_matchingInvocations.load())+
            ",\"mhwss_handled_taa\":"+std::to_string(g_nativeBypasses.load())+",\"trace_drops\":"+std::to_string(g_traceDrops.load())+",\"evaluates_sr\":false}");
    }catch(...){}
}
