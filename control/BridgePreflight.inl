// Included inside MhwSrBridge.cpp's anonymous namespace after QuadData.
// These callbacks observe the original MHWSS callback ABI, not D3D12 replacements.
using PsoCallback=bool(__fastcall*)(void*,ID3D12GraphicsCommandList*,ID3D12PipelineState*);
using DispatchCallback=bool(__fastcall*)(void*,ID3D12GraphicsCommandList*,UINT,UINT,UINT);
using ResetCallback=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
PsoCallback g_psoCallback{};DispatchCallback g_dispatchCallback{};ResetCallback g_listReset{};
unsigned char* g_mhwss{};
#include "BridgeJitterPulse.inl"
mhwsr::Dispatch g_ngx{};
std::mutex g_bridgeMutex;
struct VertexCopyRange {UINT64 destination=0,source=0,bytes=0;};
struct TextureHeap {UINT64 gpu=0,cpu=0;unsigned count=0,stride=0;};
struct TextureRoots {uintptr_t signature=0;std::array<UINT64,32> tables{},cbvs{};};
struct TextureList {TextureHeap heap{};TextureRoots compute{},graphics{};std::array<UINT64,8> targets{};unsigned targetCount=0;
    std::array<ID3D12DescriptorHeap*,2> heaps{};unsigned heapCount=0;};
struct QualityMark {uintptr_t color=0,taa=0,copy=0,tone=0;unsigned stage=0;bool sr=false;};
struct PreparedDepthTrace {uintptr_t source=0,prepared=0;unsigned width=0,height=0,format=0;uint64_t generation=0,sequence=0;};
struct ListTrace {
    ID3D12PipelineState* pso{};D3D12_VIEWPORT view{};bool hasView=false;uint64_t generation=0;
    std::array<UINT64,16> cbvs{};std::array<uint64_t,16> cbvEpochs{};uint64_t bindEpoch=0,lastTaaEpoch=0;
    D3D12_VERTEX_BUFFER_VIEW vertex{};uint64_t taaSerial=0;unsigned postTaaOps=0;mhwsr::Size taaInput{},taaOutput{};
    uintptr_t vertexCaller=0;
    std::array<VertexCopyRange,16> vertexCopies{};unsigned vertexCopyCount=0;
    TextureList textures{};
    uint64_t texturePassId=0;
    unsigned postTaaDispatches=0;bool detailedTextureFrame=false;
    D3D12_RECT scissor{};bool hasScissor=false;
    PreparedDepthTrace preparedDepth{};
    QualityMark quality{};
    D3D_PRIMITIVE_TOPOLOGY topology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};
std::unordered_map<ID3D12GraphicsCommandList*,ListTrace> g_bridgeLists;
struct TaaTrace {
    uintptr_t list=0,pso=0;UINT groups[3]{};D3D12_VIEWPORT view{};
    bool hasView=false,handled=false;uint64_t serial=0,tick=0,engineUpdate=0,generation=0;
    unsigned size[2]{};float scales[3]{};uint64_t sceneInvocation=0;
};
thread_local TaaTrace g_threadTaa{};
using ScenePass=uintptr_t(__fastcall*)(void*,void*,unsigned,uintptr_t);
ScenePass g_scenePass{};
using SceneViewport=uintptr_t(__fastcall*)(void*,const int*);
SceneViewport g_sceneViewport{};
struct SceneInvocation {uint64_t serial=0;uintptr_t context=0,scene=0;int outputRect[4]{},inputRect[4]{},predictedRect[4]{};float scale=0;bool inputObserved=false;};
thread_local SceneInvocation g_sceneInvocation{};
std::atomic<uint64_t> g_scenePasses{0},g_matchingInvocations{0},g_sceneRectsObserved{0};
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
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()) {
        SceneInvocation candidate{};
        if(ReadSceneInvocation(context,scene,&candidate)&&std::isfinite(candidate.scale)&&candidate.scale>=0.5f&&candidate.scale<=1.0f) {
            // Match 0x23c38a8..0x23c3908: single-precision multiply followed
            // by cvttss2si (truncate), then preserve that source rectangle on stack.
            for(unsigned i=0;i<4;++i)if(candidate.outputRect[i]>=0&&candidate.outputRect[i]<=16384)
                candidate.predictedRect[i]=int(float(candidate.outputRect[i])*candidate.scale);
            candidate.serial=++g_scenePasses;g_sceneInvocation=candidate;
        }
    }
    return g_scenePass(context,scene,flags,tag);
}
bool ReadActualSceneRect(void* context,const int* rect,SceneInvocation* scope) noexcept {
    __try {
        if(*reinterpret_cast<uintptr_t*>(static_cast<unsigned char*>(context)+0x1d0)!=scope->scene)return false;
        memcpy(scope->inputRect,rect,16);memcpy(scope->outputRect,static_cast<unsigned char*>(context)+0x5c,16);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uintptr_t __fastcall OnSceneViewport(void* context,const int* rect) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-reinterpret_cast<uintptr_t>(g_game);
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&caller==0x23c391f&&g_sceneInvocation.serial&&
       g_sceneInvocation.context==reinterpret_cast<uintptr_t>(context)&&ReadActualSceneRect(context,rect,&g_sceneInvocation)) {
        const auto& d=g_sceneInvocation;
        g_sceneInvocation.inputObserved=d.inputRect[0]==0&&d.inputRect[1]==0&&d.outputRect[0]==0&&d.outputRect[1]==0&&
            d.inputRect[2]>0&&d.inputRect[3]>0&&d.inputRect[2]<=d.outputRect[2]&&d.inputRect[3]<=d.outputRect[3]&&
            d.outputRect[2]<=16384&&d.outputRect[3]<=16384;
        if(g_sceneInvocation.inputObserved)++g_sceneRectsObserved;
    }
    return g_sceneViewport(context,rect);
}
std::atomic<uint64_t> g_taaDispatches{0},g_correlations{0},g_missingTaa{0},g_traceDrops{0},g_nativeBypasses{0};
std::atomic<uint64_t> g_psoCallbacks{0},g_dispatchCallbacks{0};
std::atomic<uint64_t> g_taaPsoBindings{0},g_dispatchWithoutPso{0};
std::set<std::tuple<uintptr_t,UINT,UINT,UINT>> g_dispatchKinds;
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
void InvalidateQualityBindings(ID3D12GraphicsCommandList*);
#include "BridgeScreenInputs.inl"
bool RunQualityTaa(ID3D12GraphicsCommandList*,const TaaScreenInput&);
bool RunQualityDraw(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
bool RunQualityCopy(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
void ResetQualityList(ID3D12GraphicsCommandList*);
#include "BridgeVertexReader.inl"
#include "BridgeEngineVertices.inl"
#include "BridgeTextureBindings.inl"
#include "BridgePreparedDepth.inl"
#include "BridgePostTaaTrace.inl"
template<class Fn> void BridgeHook(size_t,void*,const std::array<unsigned char,16>&,void*,Fn&);
bool KnownNgxPointer(void*,bool);
#include "BridgeQuality.inl"
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
void RecordDispatchCandidate(const TaaTrace& t) {
    if(!g_sceneInvocation.serial||!g_sceneInvocation.inputObserved)return;
    const auto key=std::make_tuple(t.pso,t.groups[0],t.groups[1],t.groups[2]);
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_dispatchKinds.count(key)||g_dispatchKinds.size()>=160)return;
        g_dispatchKinds.insert(key);
    }
    uintptr_t known[2]{};memcpy(known,g_mhwss+0x54ec00,sizeof(known));
    std::ostringstream out;
    out<<"{\"event\":\"scene_dispatch_candidate\",\"phase\":"<<g_phase.load()<<",\"scope\":"<<g_sceneInvocation.serial
       <<",\"thread\":"<<GetCurrentThreadId()<<",\"list\":\"0x"<<std::hex<<t.list<<"\",\"bound_pso\":\"0x"<<t.pso
       <<"\",\"known_taa_psos\":[\"0x"<<known[0]<<"\",\"0x"<<known[1]<<"\"]"<<std::dec
       <<",\"dispatch\":["<<t.groups[0]<<','<<t.groups[1]<<','<<t.groups[2]<<"],\"actual_input\":["
       <<g_sceneInvocation.inputRect[2]<<','<<g_sceneInvocation.inputRect[3]<<"],\"handled\":"<<(t.handled?"true":"false")<<'}';
    SaveBridge(out.str());
}
bool __fastcall OnMhwPso(void* self,ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso) {
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        const auto pass=++g_psoCallbacks;
        if(NativeTaaPso(pso))++g_taaPsoBindings;
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){auto& state=g_bridgeLists[list];state.pso=pso;state.texturePassId=pass;}else ++g_traceDrops;
    }catch(...){++g_traceDrops;}
    return g_psoCallback(self,list,pso);
}
bool __fastcall OnMhwDispatch(void* self,ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z) {
    TaaTrace trace{};bool matched=false;
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        ++g_dispatchCallbacks;
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        const auto found=g_bridgeLists.find(list);
        if(found!=g_bridgeLists.end()) {
            trace.list=reinterpret_cast<uintptr_t>(list);trace.pso=reinterpret_cast<uintptr_t>(found->second.pso);
            trace.view=found->second.view;trace.hasView=found->second.hasView;trace.generation=found->second.generation;
            matched=NativeTaaPso(found->second.pso);
        }
        if(!trace.pso)++g_dispatchWithoutPso;
    }catch(...){++g_traceDrops;}
    TaaScreenInput screen{};
    if(matched)screen=ReadTaaScreen(list);
    const bool quality=matched&&g_qualityMode&&RunQualityTaa(list,screen);
    const auto mhwssResult=g_dispatchCallback(self,list,x,y,z);
    const bool result=mhwssResult||quality;
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&g_sceneInvocation.serial&&g_sceneInvocation.inputObserved)try {
        trace.groups[0]=x;trace.groups[1]=y;trace.groups[2]=z;trace.handled=mhwssResult;trace.sceneInvocation=g_sceneInvocation.serial;
        MainState(&trace);QueryBridgePlans(trace);RecordDispatchCandidate(trace);
    }catch(...){++g_traceDrops;}
    if(matched)try {
        trace.groups[0]=x;trace.groups[1]=y;trace.groups[2]=z;trace.handled=mhwssResult;
        trace.serial=++g_taaDispatches;trace.tick=GetTickCount64();trace.engineUpdate=g_engineUpdates.load();
        trace.sceneInvocation=g_sceneInvocation.serial;
        MainState(&trace);g_threadTaa=trace;if(mhwssResult)++g_nativeBypasses;
        {
            std::lock_guard<std::mutex> lock(g_bridgeMutex);auto& state=g_bridgeLists[list];
            state.taaSerial=screen.error?0:trace.serial;state.postTaaOps=0;
            state.postTaaDispatches=0;state.detailedTextureFrame=false;
            if(!screen.error){state.taaInput={screen.words[10],screen.words[11]};
                state.taaOutput={static_cast<unsigned>(ScreenFloat(screen,16)),static_cast<unsigned>(ScreenFloat(screen,20))};
                if(g_traceTextures){static std::map<std::pair<unsigned,unsigned>,unsigned> samples;const auto key=std::make_pair(screen.words[10],screen.words[11]);
                    if(samples.size()<8||samples.count(key)){auto& count=samples[key];state.detailedTextureFrame=count<4;if(count<4)++count;}}}
        }
        RecordTaaScreen(trace,screen);
        if(!screen.error)RecordPreparedDepth(list,trace.serial);
        if(!screen.error)RecordTextureSnapshot(list,true,trace.serial,0,0);
        QueryBridgePlans(trace);
    }catch(...){++g_traceDrops;}
    if(!matched&&g_traceTextures&&(g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        uint64_t serial=0;unsigned index=0;
        {std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(list);
            if(found!=g_bridgeLists.end()&&found->second.taaSerial&&found->second.postTaaOps<3&&found->second.postTaaDispatches<16){
                serial=found->second.taaSerial;index=++found->second.postTaaDispatches;}}
        if(serial)RecordTextureSnapshot(list,true,serial,index,0,"post_taa_dispatch",x,y,z);
    }catch(...){++g_traceDrops;}
    return result;
}
HRESULT STDMETHODCALLTYPE OnBridgeReset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pso) {
    const auto result=g_listReset(list,allocator,pso);
    if(SUCCEEDED(result)&&g_qualityMode&&!g_qualityCommands)ResetQualityList(list);
    if(SUCCEEDED(result)&&(g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        const auto found=g_bridgeLists.find(list);
        if(found!=g_bridgeLists.end()){const auto generation=found->second.generation+1;found->second={};found->second.generation=generation;found->second.pso=pso;}
        if(g_threadTaa.list==reinterpret_cast<uintptr_t>(list))g_threadTaa={};
    }catch(...){++g_traceDrops;}
    return result;
}
void RecordBridgeViewport(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* view) {
    if(!(g_ready.load()&&!g_qualityCommands)||g_done.load())return;
    try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){auto& trace=g_bridgeLists[list];trace.hasView=count==1&&view;if(trace.hasView)trace.view=view[0];}
    }catch(...){++g_traceDrops;}
}
void RecordBridgeQuad(uintptr_t caller,void* context,const QuadData& d) {
    if(caller!=0x23c7b08||!d.contextScene||d.contextScene!=d.mainScene)return;
    const auto trace=g_threadTaa;g_threadTaa={};
    if(!trace.serial){++g_missingTaa;return;}
    ++g_correlations;
    const bool sameInvocation=g_sceneInvocation.inputObserved&&trace.sceneInvocation&&trace.sceneInvocation==g_sceneInvocation.serial&&
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
       <<",\"input_rect_observed_at_engine_call\":"<<(g_sceneInvocation.inputObserved?"true":"false")
       <<",\"scope_input_rect\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';out<<g_sceneInvocation.inputRect[i];}
    out<<"],\"frame_association_proven\":false,\"changes_copy\":false,\"evaluates_sr\":false}";
    SaveBridge(out.str());
}
template<class Fn> void BridgeHook(size_t index,void* target,const std::array<unsigned char,16>& signature,void* detour,Fn& original) {
    auto& hook=g_hooks[index];hook.address=target;hook.original=signature;
    if(memcmp(target,signature.data(),16)) {
        Log("{\"event\":\"bridge_hook_signature_mismatch\",\"index\":"+std::to_string(index)+
            ",\"expected\":\""+HexBytes(signature.data(),16)+"\",\"actual\":\""+HexBytes(target,16)+"\"}");
        throw std::runtime_error("Bridge hook target signature differs");
    }
    const auto status=MH_CreateHook(target,detour,reinterpret_cast<void**>(&original));
    Log("{\"event\":\"bridge_hook_created\",\"index\":"+std::to_string(index)+",\"status\":\""+MH_StatusToString(status)+"\"}");
    if(status!=MH_OK)throw std::runtime_error("Bridge hook reservation failed");
}
void PrepareSceneScopeHook() {
    // Followed the PE chained unwind metadata to the real prologue, not a middle fragment.
    const std::array<unsigned char,16> signature{0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x41,0x54,0x41,0x55};
    BridgeHook(6,reinterpret_cast<unsigned char*>(g_game)+0x23c3790,signature,reinterpret_cast<void*>(&OnScenePass),g_scenePass);
    const std::array<unsigned char,16> viewport{0x40,0x55,0x53,0x48,0x8d,0x6c,0x24,0xe8,0x48,0x81,0xec,0x18,0x01,0x00,0x00,0x8b};
    BridgeHook(7,reinterpret_cast<unsigned char*>(g_game)+0x23d0330,viewport,reinterpret_cast<void*>(&OnSceneViewport),g_sceneViewport);
}
void PrepareEngineVertexHook() {
    const std::array<unsigned char,16> signature{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x55,0x56,0x41,0x56,0x48,0x8b,0xec,0x48,0x83};
    BridgeHook(15,reinterpret_cast<unsigned char*>(g_game)+0x259e470,signature,reinterpret_cast<void*>(&OnEngineVertices),g_engineVertices);
}
void PrepareEngineTextureHooks() {
    const std::array<unsigned char,16> bind{0x4c,0x8b,0xdc,0x4d,0x89,0x43,0x18,0x55,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41};
    const std::array<unsigned char,16> targets{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x48,0x89,0x50,0x10,0x55,0x53,0x57,0x41,0x55};
    auto* game=reinterpret_cast<unsigned char*>(g_game);
    BridgeHook(16,game+0x259f070,bind,reinterpret_cast<void*>(&OnEngineTextureBind0),g_engineTextureBind0);
    BridgeHook(17,game+0x259f330,bind,reinterpret_cast<void*>(&OnEngineTextureBind1),g_engineTextureBind1);
    BridgeHook(24,game+0x259f9b0,targets,reinterpret_cast<void*>(&OnEngineTargets),g_engineTargets);
    const std::array<unsigned char,16> compute{0x4c,0x8b,0xdc,0x49,0x89,0x6b,0x20,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    BridgeHook(26,game+0x259f610,compute,reinterpret_cast<void*>(&OnEngineTextureComputeBind),g_engineTextureComputeBind);
    const std::array<unsigned char,16> resourceGetter{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x59,0x60,0x48,0x8b};
    if(memcmp(game+0x259ffb0,resourceGetter.data(),16))throw std::runtime_error("Texture resource getter signature differs");
    g_textureAllocation=reinterpret_cast<TextureAllocation>(game+0x259ffb0);
}
bool KnownNgxPointer(void* pointer,bool allowProbe) {
    HMODULE module{};
    if(!pointer||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(pointer),&module))return false;
    const auto path=ModulePath(module);const auto name=path.filename().wstring();bool ok=false;
    // Preserve the known parameter observer chain; never overwrite any dispatch slot.
    if(allowProbe&&_wcsicmp(name.c_str(),L"MhwSrProbe.dll")==0)
        ok=FileSha256(path)=="42de0846e46a593fd1b3743abd92e1e0b098760b575322867977127a750618c7";
    else if(_wcsicmp(name.c_str(),L"d3d12.dll")==0) {
        const auto digest=FileSha256(path);ok=!g_expectedProxyHash.empty()&&digest==g_expectedProxyHash;
        Log("{\"event\":\"ngx_proxy_identity\",\"sha256\":\""+digest+"\",\"matches_expected\":"+(ok?"true":"false")+"}");
    }
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
    const std::array<unsigned char,16> cbv{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd9,0x48,0x8b,0x41,0x58,0x4c,0x8b,0x15,0x4e,0xa7};
    const std::array<unsigned char,16> clear{0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x8d,0x99,0xe0,0xfe,0xff,0xff,0xb9,0x03,0x10};
    BridgeHook(8,core+0x12a8a0,cbv,reinterpret_cast<void*>(&OnBridgeComputeCbv),g_computeCbv);
    BridgeHook(9,core+0x130f80,clear,reinterpret_cast<void*>(&OnBridgeClearState),g_clearState);
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
    if(g_tracePostTaa) {
        HMODULE proxy{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(capPointer),&proxy)||
           FileSha256(ModulePath(proxy))!="e78c757c483631364985efe79806fe81ca7f8fc1b9d2b6451b4736052fa72011")
            throw std::runtime_error("Post-TAA observation requires the inspected proxy build");
        // The live Core DrawInstanced relay was independently followed to this
        // proxy entry. Chain the owner function instead of overwriting its relay.
        const std::array<unsigned char,16> draw{0x40,0x55,0x53,0x56,0x57,0x48,0x8d,0x6c,0x24,0xe8,0x48,0x81,0xec,0x18,0x01,0x00};
        const std::array<unsigned char,16> vertices{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x41,0x58,0x4c,0x8b,0x1d};
        BridgeHook(10,reinterpret_cast<unsigned char*>(proxy)+0x178930,draw,reinterpret_cast<void*>(&OnBridgeDraw),g_draw);
        BridgeHook(11,core+0x12a660,vertices,reinterpret_cast<void*>(&OnBridgeVertices),g_vertexBuffers);
        const std::array<unsigned char,16> gpuva{0x48,0x8b,0x81,0x68,0x01,0x00,0x00,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
        BridgeHook(12,core+0x10d230,gpuva,reinterpret_cast<void*>(&OnBridgeGpuAddress),g_gpuAddress);
        const std::array<unsigned char,16> copy{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c};
        BridgeHook(13,core+0x130c80,copy,reinterpret_cast<void*>(&OnBridgeBufferCopy),g_bufferCopy);
        const std::array<unsigned char,16> map{0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x80,0x00};
        BridgeHook(14,core+0x10d010,map,reinterpret_cast<void*>(&OnBridgeResourceMap),g_resourceMap);
        if(g_traceTextures) {
            auto* owner=reinterpret_cast<unsigned char*>(proxy);
            const std::array<unsigned char,16> copyDescriptor{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c};
            const std::array<unsigned char,16> heaps{0x48,0x81,0xc1,0xe0,0xfe,0xff,0xff,0xe9,0x3c,0x75,0xff,0xff,0xcc,0xcc,0xcc,0xcc};
            const std::array<unsigned char,16> table{0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24};
            const std::array<unsigned char,16> csig{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x60,0x48};
            const std::array<unsigned char,16> gsig{0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x60,0x48,0x8b,0x05,0xbf,0x5e,0x7e};
            const std::array<unsigned char,16> om{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac};
            BridgeHook(18,owner+0x177470,copyDescriptor,reinterpret_cast<void*>(&OnTextureCopy),g_textureCopy);
            BridgeHook(19,core+0x12ac90,heaps,reinterpret_cast<void*>(&OnTextureHeaps),g_textureHeaps);
            BridgeHook(20,owner+0x1783b0,table,reinterpret_cast<void*>(&OnTextureComputeTable),g_textureComputeTable);
            BridgeHook(21,owner+0x177730,table,reinterpret_cast<void*>(&OnTextureGraphicsTable),g_textureGraphicsTable);
            BridgeHook(22,owner+0x61900,csig,reinterpret_cast<void*>(&OnTextureComputeSignature),g_textureComputeSignature);
            BridgeHook(23,owner+0x62170,gsig,reinterpret_cast<void*>(&OnTextureGraphicsSignature),g_textureGraphicsSignature);
            BridgeHook(25,owner+0x177ca0,om,reinterpret_cast<void*>(&OnTextureTargets),g_textureTargets);
            const std::array<unsigned char,16> textureCopy{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c};
            const std::array<unsigned char,16> resourceCopy{0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x48,0x83,0xec,0x30,0x49,0x8b,0xf0,0x48};
            BridgeHook(27,core+0x1308e0,textureCopy,reinterpret_cast<void*>(&OnTrackedTextureCopy),g_trackedTextureCopy);
            BridgeHook(28,core+0x1306d0,resourceCopy,reinterpret_cast<void*>(&OnTrackedResourceCopy),g_trackedResourceCopy);
            const std::array<unsigned char,16> scissor{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd9,0x48,0x8b,0x41,0x58,0x4c,0x8b,0x15,0xfe,0xa1};
            BridgeHook(29,core+0x12adf0,scissor,reinterpret_cast<void*>(&OnTextureScissor),g_textureScissor);
            const std::array<unsigned char,16> graphicsCbv{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd9,0x48,0x8b,0x41,0x58,0x4c,0x8b,0x15,0x9e,0xa7};
            BridgeHook(30,core+0x12a850,graphicsCbv,reinterpret_cast<void*>(&OnTextureGraphicsCbv),g_textureGraphicsCbv);
            const std::array<unsigned char,16> depthStage{0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
            BridgeHook(31,g_mhwss+0x115940,depthStage,reinterpret_cast<void*>(&OnPreparedDepth),g_preparedDepthStage);
        }
    }
    OpenScreenReader();
    if(g_traceTextures) {
        ID3D12Device* device=nullptr;
        if(FAILED(g_screenArena->GetDevice(IID_PPV_ARGS(&device))))throw std::runtime_error("Texture trace device unavailable");
        g_textureRtvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);device->Release();
        if(!g_textureRtvStride)throw std::runtime_error("RTV descriptor stride unavailable");
    }
    Log("{\"event\":\"bridge_preflight_prepared\",\"version\":3,\"modifies_ngx_dispatch\":false,\"changes_copy\":false,\"queries_capabilities\":true,\"evaluates_sr\":false}");
}
void FinishBridge() {
    CloseScreenReader();
    CloseVertexReaders();
    SaveTextureShaders();
    size_t textureKinds=0;{std::lock_guard<std::mutex> lock(g_textureMutex);textureKinds=g_textureSnapshotKinds.size();}
    try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        for(const auto& row:g_bridgeRecords)Log(row);
        Log("{\"event\":\"bridge_order_summary\",\"taa_dispatches\":"+std::to_string(g_taaDispatches.load())+
            ",\"pso_callbacks\":"+std::to_string(g_psoCallbacks.load())+",\"dispatch_callbacks\":"+std::to_string(g_dispatchCallbacks.load())+
            ",\"known_taa_pso_bindings\":"+std::to_string(g_taaPsoBindings.load())+",\"dispatch_without_pso\":"+std::to_string(g_dispatchWithoutPso.load())+
            ",\"same_thread_taa_quad_pairs\":"+std::to_string(g_correlations.load())+",\"quads_without_thread_taa\":"+std::to_string(g_missingTaa.load())+
            ",\"scene_invocations\":"+std::to_string(g_scenePasses.load())+",\"same_invocation_and_rect_pairs\":"+std::to_string(g_matchingInvocations.load())+
            ",\"engine_input_rects\":"+std::to_string(g_sceneRectsObserved.load())+
            ",\"valid_taa_screen_inputs\":"+std::to_string(g_taaScreenValid.load())+",\"invalid_taa_screen_inputs\":"+std::to_string(g_taaScreenInvalid.load())+
            ",\"compute_cbv_bindings\":"+std::to_string(g_computeCbvCalls.load())+
            ",\"post_taa_draws\":"+std::to_string(g_postTaaDraws.load())+",\"matching_post_taa_triangles\":"+std::to_string(g_matchingPostTaaTriangles.load())+
            ",\"gpu_address_calls\":"+std::to_string(g_gpuAddressCalls.load())+",\"mapped_vertex_buffers\":"+std::to_string(g_vertexArenaCount)+
            ",\"vertex_read_hits\":"+std::to_string(g_vertexReadHits.load())+",\"vertex_read_misses\":"+std::to_string(g_vertexReadMisses.load())+
            ",\"buffer_copy_calls\":"+std::to_string(g_bufferCopyCalls.load())+",\"vertex_buffer_copies\":"+std::to_string(g_vertexBufferCopies.load())+
            ",\"resource_map_calls\":"+std::to_string(g_resourceMapCalls.load())+
            ",\"engine_vertex_candidates\":"+std::to_string(g_engineVertexCandidates.load())+",\"engine_vertex_references\":"+std::to_string(g_engineVertexReferences.load())+
            ",\"texture_binder_calls\":"+std::to_string(g_textureBinderCalls.load())+",\"texture_packet_failures\":"+std::to_string(g_texturePacketFailures.load())+
            ",\"confirmed_texture_copies\":"+std::to_string(g_confirmedTextureCopies.load())+",\"texture_snapshot_kinds\":"+std::to_string(textureKinds)+
            ",\"post_taa_texture_copies\":"+std::to_string(g_postTaaTextureCopies.load())+
            ",\"observed_depth_preparations\":"+std::to_string(g_depthPreparations.load())+",\"taa_with_same_generation_depth\":"+std::to_string(g_taaDepthMatches.load())+
            ",\"mhwss_handled_taa\":"+std::to_string(g_nativeBypasses.load())+",\"trace_drops\":"+std::to_string(g_traceDrops.load())+
            ",\"quality_evaluations\":"+std::to_string(g_qualityEvaluated.load())+"}");
    }catch(...){}
}
