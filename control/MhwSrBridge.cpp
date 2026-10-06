// Bridge observer plus an opt-in, bounded Quality rendering prototype.
// Reserves disabled game hooks during startup; waits for an explicit scene signal.
#include "../readback/CaptureCommon.h"
#include <d3d12.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <set>
#include <intrin.h>
#include <unordered_map>
#include "SrParameterAdapter.h"
#include "SrQualityGpu.h"

namespace {
constexpr char kGameHash[] = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";
constexpr char kCoreHash[] = "64a537d9e927f1e41e550985b623e7636af50bcb758d3c79bf934f9acdbbb181";
constexpr size_t kUpdate = 0x229ed30, kSetter = 0x229bde0, kRenderPointer = 0x51c4480;
using Update = void(__fastcall*)(void*);
using Setter = void(__fastcall*)(void*, float, bool);
using Viewports = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
Update g_update{}; Setter g_setter{}; Viewports g_viewports{};
HMODULE g_self{}, g_game{};
using Quad = uintptr_t(__fastcall*)(void*,void*,const int*,const int*,const float*,bool);
Quad g_quad{};
std::mutex g_quadsMutex;
std::set<std::string> g_seenQuads;
std::vector<std::string> g_quadRecords;
std::atomic<uint64_t> g_quadCalls{0},g_focusedQuadCalls{0};
volatile LONG g_started = 0;
std::atomic<bool> g_ready{false}, g_abort{false}, g_done{false}, g_changed{false};
std::atomic<unsigned> g_phase{0};
std::mutex g_logMutex, g_viewsMutex;
std::ofstream g_log;
std::map<std::tuple<unsigned, unsigned, unsigned>, uint64_t> g_views;
struct Hook { void* address{}; std::array<unsigned char,16> patch{},original{}; bool enabled=false; };
std::array<Hook,36> g_hooks{};
bool g_qualityMode=false;
std::atomic<uintptr_t> g_qualityDepthAnchor{0};
std::atomic<uintptr_t> g_qualityRasterDepthAnchor{0},g_qualityGbufferAnchor{0};
thread_local unsigned g_qualityCommands=0;
bool QualityScale(float&);
void PrepareQualityHooks();
void FinishQuality();
std::string g_expectedProxyHash;
bool g_requireFrameGenOff=true;
bool g_applyScalePulse=true;
bool g_gameHooks=true;
bool g_tracePostTaa=false;
bool g_pulseJitter=false;
bool g_traceTextures=false;
unsigned g_windowMs=3000;
bool g_windowSounds=false;
std::atomic<uint64_t> g_windowDeadline{0};
std::atomic<bool> g_jitterChanged{false};
bool EnableJitterPulse();
bool JitterPulseStillOwned();
void RestoreJitterPulse() noexcept;
std::atomic<uint64_t> g_engineUpdates{0};
void PrepareBridgeHooks();
void PrepareSceneScopeHook();
void PrepareEngineVertexHook();
void PrepareEngineTextureHooks();
void RecordBridgeViewport(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*);
void FinishBridge();
uint64_t g_phaseStart=0;
float g_original=1, g_expected=1, g_restoreExpected=1;
unsigned g_width=0, g_height=0;
bool g_targetObserved=false;
std::atomic<bool> g_restoreObserved{false};

void Log(const std::string& line) noexcept {
    try { std::lock_guard<std::mutex> lock(g_logMutex); if(g_log){g_log<<line<<'\n';g_log.flush();} } catch(...){}
}
float Value(void* object,size_t offset) { float result;memcpy(&result,static_cast<unsigned char*>(object)+offset,4);return result; }
unsigned Uint(void* object,size_t offset) { unsigned result;memcpy(&result,static_cast<unsigned char*>(object)+offset,4);return result; }
bool Near(float a,float b) { return std::isfinite(a)&&std::isfinite(b)&&std::fabs(a-b)<0.00001f; }
void* Renderer() noexcept {
    __try {return *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(g_game)+kRenderPointer);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}
void Snapshot(const char* event,void* object) {
    std::ostringstream out;
    out<<std::setprecision(9)<<"{\"event\":\""<<event<<"\",\"tick_ms\":"<<GetTickCount64()
       <<",\"phase\":"<<g_phase.load()<<",\"screen_size\":["<<Uint(object,0x198)<<','<<Uint(object,0x19c)
       <<"],\"pending\":"<<Value(object,0x1f0)<<",\"active\":"<<Value(object,0x1f4)
       <<",\"previous_active\":"<<Value(object,0x1f8)<<",\"base_pending\":"<<Value(object,0x200)
       <<",\"base_active\":"<<Value(object,0x204)<<",\"actual\":"<<Value(object,0x208)<<'}';
    Log(out.str());
}
void Restore(void* object,const char* reason) {
    RestoreJitterPulse();
    // Preserve a newer value written by a user setting change instead of overwriting it.
    const auto pending=Value(object,0x1f0);
    g_restoreExpected=Near(pending,g_expected)?g_original:pending;
    if(Near(pending,g_expected)) g_setter(object,g_original,false);
    g_changed.store(false);g_phase.store(3);g_phaseStart=GetTickCount64();
    Log(std::string("{\"event\":\"restore_requested\",\"reason\":\"")+reason+"\"}");
    Snapshot("restore_pending",object);
}
void __fastcall OnUpdate(void* object) {
    g_update(object);
    if(!(g_ready.load()&&!g_qualityCommands)||g_done.load()||object!=Renderer()) return;
    ++g_engineUpdates;
    try {
        const auto now=GetTickCount64();auto phase=g_phase.load();
        if(phase==0) {
            g_width=Uint(object,0x198);g_height=Uint(object,0x19c);g_original=Value(object,0x1f0);
            if(g_width<1280||g_height<720||!Near(g_original,1)||!Near(Value(object,0x1f4),1)||
               !Near(Value(object,0x200),1)||!Near(Value(object,0x204),1)) {
                Snapshot("baseline_refused",object);g_done.store(true);return;
            }
            g_phase.store(1);g_phaseStart=now;Snapshot("baseline",object);return;
        }
        if(Uint(object,0x198)!=g_width||Uint(object,0x19c)!=g_height) g_abort.store(true);
        if(g_abort.load()&&phase<3) {
            if(g_changed.load()) Restore(object,"abort");
            else {RestoreJitterPulse();Log("{\"event\":\"aborted_before_change\"}");g_done.store(true);}
            return;
        }
        if(phase==1&&now-g_phaseStart>=1500) {
            if(!Near(Value(object,0x1f0),g_original)||!Near(Value(object,0x204),1)) {
                Log("{\"event\":\"baseline_changed_no_override\"}");g_done.store(true);return;
            }
            if(!g_applyScalePulse){g_restoreExpected=g_original;g_phase.store(3);g_phaseStart=now;Snapshot("scale_pulse_skipped",object);return;}
            float requested=2.0f/3.0f;
            if(g_qualityMode&&!QualityScale(requested))return;
            if(!EnableJitterPulse()){Log("{\"event\":\"jitter_pulse_refused\",\"changes_scale\":false}");g_done.store(true);return;}
            g_setter(object,requested,true);
            g_expected=Value(object,0x1f0);g_changed.store(true);g_windowDeadline.store(now+g_windowMs);g_phase.store(2);g_phaseStart=now;
            Snapshot("target_requested",object);return;
        }
        if(phase==2) {
            if(!JitterPulseStillOwned()){Restore(object,"external_jitter_mode_change");return;}
            if(!Near(Value(object,0x1f0),g_expected)) {Restore(object,"external_scale_change");return;}
            if(!g_targetObserved&&Near(Value(object,0x1f4),g_expected)) {g_targetObserved=true;Snapshot("target_active",object);}
            if(now>=g_windowDeadline.load()) Restore(object,"window_complete");
            return;
        }
        if(phase==3) {
            if(!g_restoreObserved&&Near(Value(object,0x1f4),g_restoreExpected)) {
                g_restoreObserved=true;g_phaseStart=now;Snapshot("restored_active",object);
            }
            if(g_restoreObserved&&now-g_phaseStart>=1000) {g_done.store(true);Snapshot("sequence_complete",object);}
        }
    } catch(...) {g_abort.store(true);}
}
void STDMETHODCALLTYPE OnViewports(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* views) {
    RecordBridgeViewport(list,count,views);
    const auto phase=g_phase.load();
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&phase>=1&&phase<=3&&count==1&&views&&
       std::isfinite(views[0].Width)&&std::isfinite(views[0].Height)&&views[0].Width>=320&&views[0].Height>=180&&
       views[0].Width<=16384&&views[0].Height<=16384) try {
        std::unique_lock<std::mutex> lock(g_viewsMutex,std::try_to_lock);
        if(lock.owns_lock()) {
            const auto key=std::make_tuple(phase,static_cast<unsigned>(std::lround(views[0].Width)),static_cast<unsigned>(std::lround(views[0].Height)));
            if(g_views.size()<128||g_views.count(key)) ++g_views[key];
        }
    } catch(...){}
    g_viewports(list,count,views);
}
struct QuadData {int rect[4]{},texture[2]{},contextRect[4]{};float offset[2]{},scale[3]{};uintptr_t contextScene=0,mainScene=0;};
bool ReadQuadData(void* context,const int* rect,const int* texture,const float* offset,void* renderer,QuadData* out) noexcept {
    __try {
        memcpy(out->rect,rect,16);memcpy(out->texture,texture,8);memcpy(out->offset,offset,8);
        memcpy(out->contextRect,static_cast<unsigned char*>(context)+0x5c,16);
        out->contextScene=*reinterpret_cast<uintptr_t*>(static_cast<unsigned char*>(context)+0x1d0);
        if(renderer)out->mainScene=*reinterpret_cast<uintptr_t*>(static_cast<unsigned char*>(renderer)+0x1f450);
        if(out->contextScene&&out->contextScene==out->mainScene)memcpy(out->scale,reinterpret_cast<void*>(out->mainScene+0x6b0),12);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
#include "BridgePreflight.inl"
uintptr_t __fastcall OnQuad(void* renderer,void* context,const int* rect,const int* texture,const float* offset,bool triangle) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto callerRva=caller-reinterpret_cast<uintptr_t>(g_game);
    const auto phase=g_phase.load();
    const bool active=(g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&phase>=1&&phase<=3;
    if(active)g_quadCalls.fetch_add(1,std::memory_order_relaxed);
    if(active&&(callerRva==0x23c6fee||callerRva==0x23c7b08))try {
        g_focusedQuadCalls.fetch_add(1,std::memory_order_relaxed);
        QuadData d{};
        if(ReadQuadData(context,rect,texture,offset,renderer,&d)&&d.texture[0]>0&&d.texture[1]>0&&d.texture[0]<=16384&&d.texture[1]<=16384&&
           std::isfinite(d.offset[0])&&std::isfinite(d.offset[1])) {
            RecordBridgeQuad(callerRva,context,d);
            std::ostringstream out;
            out<<std::setprecision(9)<<"{\"event\":\"quad\",\"phase\":"<<phase<<",\"caller_rva\":\"0x"<<std::hex
               <<callerRva<<std::dec<<"\",\"source_rect\":[";
            for(unsigned i=0;i<4;++i){if(i)out<<',';out<<d.rect[i];}
            out<<"],\"texture_size\":["<<d.texture[0]<<','<<d.texture[1]<<"],\"uv_offset\":["<<d.offset[0]<<','<<d.offset[1]
               <<"],\"triangle\":"<<(triangle?"true":"false")<<",\"candidate_context_rect\":[";
            for(unsigned i=0;i<4;++i){if(i)out<<',';out<<d.contextRect[i];}
            out<<"],\"context_matches_main_scene\":"<<(d.contextScene&&d.contextScene==d.mainScene?"true":"false")<<",\"scene_scales\":[";
            for(unsigned i=0;i<3;++i){if(i)out<<',';if(std::isfinite(d.scale[i]))out<<d.scale[i];else out<<"null";}
            out<<"]}";
            std::unique_lock<std::mutex> lock(g_quadsMutex,std::try_to_lock);
            if(lock.owns_lock()&&g_seenQuads.size()<256&&g_seenQuads.insert(out.str()).second)g_quadRecords.push_back(out.str());
        }
    }catch(...){}
    return g_quad(renderer,context,rect,texture,offset,triangle);
}
bool Pin(HMODULE module) {HMODULE pinned{};return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pinned)!=FALSE;}
void PrepareGameHook() {
    g_game=GetModuleHandleW(nullptr);
    if(FileSha256(ModulePath(g_game))!=kGameHash)throw std::runtime_error("Game build mismatch");
    auto* game=reinterpret_cast<unsigned char*>(g_game);
    const std::array<unsigned char,16> update{0x40,0x53,0x48,0x83,0xec,0x20,0x80,0xb9,0xf4,0xb3,0x07,0x00,0x00,0x48,0x8b,0xd9};
    const std::array<unsigned char,16> setter{0xf3,0x0f,0x10,0x05,0x70,0x40,0xbb,0x00,0x0f,0x2f,0xc8,0xf3,0x0f,0x10,0x15,0x61};
    const std::array<unsigned char,16> reference{0x48,0x8b,0x0d,0xab,0x54,0xd8,0x04,0x0f,0x28,0xce,0xe8,0x03,0xce,0xe5,0x01,0x0f};
    bool matched=false;
    for(unsigned attempt=0;attempt<2400;++attempt) {
        if(!memcmp(game+kUpdate,update.data(),16)&&!memcmp(game+kSetter,setter.data(),16)&&!memcmp(game+0x43efce,reference.data(),16)){matched=true;break;}
        Sleep(25);
    }
    if(!matched)throw std::runtime_error("Game live signatures differ");
    if(!Pin(g_self)||!Pin(g_game)||MH_Initialize()!=MH_OK)throw std::runtime_error("Hook initialization failed");
    g_setter=reinterpret_cast<Setter>(game+kSetter);g_hooks[0].address=game+kUpdate;
    g_hooks[0].original=update;
    const auto updateStatus=MH_CreateHook(g_hooks[0].address,reinterpret_cast<void*>(&OnUpdate),reinterpret_cast<void**>(&g_update));
    Log(std::string("{\"event\":\"hook_created\",\"target\":\"game_scale_update\",\"status\":\"")+MH_StatusToString(updateStatus)+"\"}");
    if(updateStatus!=MH_OK)throw std::runtime_error("Game update hook creation failed");
    const std::array<unsigned char,16> quad{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x55};
    g_hooks[2].address=game+0x2297e20;
    g_hooks[2].original=quad;
    if(memcmp(g_hooks[2].address,quad.data(),16))throw std::runtime_error("Quad signature differs");
    const auto status=MH_CreateHook(g_hooks[2].address,reinterpret_cast<void*>(&OnQuad),reinterpret_cast<void**>(&g_quad));
    Log(std::string("{\"event\":\"hook_created\",\"target\":\"quad\",\"status\":\"")+MH_StatusToString(status)+"\"}");
    if(status!=MH_OK)throw std::runtime_error("Quad hook creation failed");
    PrepareSceneScopeHook();
    if(g_tracePostTaa)PrepareEngineVertexHook();
    if(g_traceTextures)PrepareEngineTextureHooks();
    Log(std::string("{\"event\":\"game_hooks_prepared\",\"version\":3,\"game_hook_count\":")+std::to_string(4+(g_tracePostTaa?1:0)+(g_traceTextures?4:0))+",\"hook_enabled\":false,\"changes_scale\":false}");
}
void Install() {
    const auto core=GetModuleHandleW(L"D3D12Core.dll");
    if(!core||FileSha256(ModulePath(core))!=kCoreHash)throw std::runtime_error("Core build mismatch");
    auto* api=reinterpret_cast<unsigned char*>(core);
    const std::array<unsigned char,16> viewport{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd9,0x48,0x8b,0x41,0x58,0x4c,0x8b,0x15,0xae,0xa1};
    if(memcmp(api+0x12ae40,viewport.data(),16)||!Pin(core))throw std::runtime_error("Viewport live signature differs");
    g_hooks[1].address=api+0x12ae40;
    g_hooks[1].original=viewport;
    PrepareBridgeHooks();
    if(g_qualityMode)PrepareQualityHooks();
    for(const auto& hook:g_hooks)if(hook.address&&memcmp(hook.address,hook.original.data(),16))throw std::runtime_error("Hook target changed while waiting");
    const auto status=MH_CreateHook(g_hooks[1].address,reinterpret_cast<void*>(&OnViewports),reinterpret_cast<void**>(&g_viewports));
    if(status!=MH_OK)throw std::runtime_error("Viewport hook creation failed");
    for(auto& hook:g_hooks) {
        if(!hook.address)continue;
        if(MH_EnableHook(hook.address)!=MH_OK)throw std::runtime_error("Hook enable failed");
        hook.enabled=true;memcpy(hook.patch.data(),hook.address,16);
    }
    Log(std::string("{\"event\":\"attached\",\"version\":4,\"bridge_mode\":\"")+(g_qualityMode?"quality_prototype":"preflight_only")+"\",\"calls_engine_scale_setter\":"+
        (g_gameHooks&&g_applyScalePulse?"true":"false")+",\"queries_ngx_capabilities\":true,\"sr_execution_enabled\":"+(g_qualityMode?"true":"false")+"}");
    if(!g_gameHooks)g_phase.store(1);
    g_ready.store(true);
}
std::string Setting(const std::filesystem::path& path,const std::string& key) {
    std::ifstream input(path);std::string line;
    while(std::getline(input,line)) {
        const auto equal=line.find('=');if(equal==std::string::npos)continue;
        auto name=line.substr(0,equal);while(!name.empty()&&(name.back()==' '||name.back()=='\t'))name.pop_back();
        if(name!=key)continue;
        auto value=line.substr(equal+1);const auto first=value.find_first_not_of(" \t\"");
        if(first==std::string::npos)return {};value.erase(0,first);
        while(!value.empty()&&(value.back()=='\r'||value.back()==' '||value.back()=='\"'))value.pop_back();return value;
    }
    return {};
}
bool FrameGenDisabled(const std::filesystem::path& path) {
    if(!g_requireFrameGenOff)return true;
    if(!std::filesystem::exists(path))return true;
    std::array<wchar_t,32> value{};
    GetPrivateProfileStringW(L"FrameGen",L"Enabled",L"auto",value.data(),static_cast<DWORD>(value.size()),path.c_str());
    return _wcsicmp(value.data(),L"false")==0||_wcsicmp(value.data(),L"off")==0||!wcscmp(value.data(),L"0");
}
bool QualityConfiguration(const std::filesystem::path& graphics,const std::filesystem::path& optiscaler) {
    return !g_qualityMode||(Setting(graphics,"MotionBlur")=="Off"&&Setting(graphics,"Anti-Aliasing")=="TAA"&&
        Setting(graphics,"NVIDIA DLSS")=="Off"&&Setting(optiscaler,"Dx12Upscaler")=="dlss"&&
        Setting(optiscaler,"ColorResourceBarrier")=="auto"&&Setting(optiscaler,"MotionVectorResourceBarrier")=="auto"&&
        Setting(optiscaler,"DepthResourceBarrier")=="auto"&&Setting(optiscaler,"OutputResourceBarrier")=="auto");
}
void ReadBridgeConfiguration(const std::filesystem::path& ini) {
    std::array<wchar_t,128> value{};
    GetPrivateProfileStringW(L"Compatibility",L"NgxProxySha256",L"73cf97e5c1a3db2be778df25d21b2999e664a06c6e0f64e67741987a21228a24",
        value.data(),static_cast<DWORD>(value.size()),ini.c_str());
    const std::wstring raw=value.data();
    if(raw.size()!=64)throw std::runtime_error("Expected proxy SHA256 must contain 64 hex characters");
    for(auto c:raw){if(c>=L'A'&&c<=L'F')c+=L'a'-L'A';if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))
        throw std::runtime_error("Expected proxy SHA256 contains a non-hex character");g_expectedProxyHash.push_back(static_cast<char>(c));}
    g_requireFrameGenOff=GetPrivateProfileIntW(L"Experiment",L"RequireFrameGenOff",1,ini.c_str())!=0;
    g_applyScalePulse=GetPrivateProfileIntW(L"Experiment",L"ApplyScalePulse",1,ini.c_str())!=0;
    g_gameHooks=GetPrivateProfileIntW(L"Experiment",L"GameHooks",1,ini.c_str())!=0;
    g_tracePostTaa=GetPrivateProfileIntW(L"Experiment",L"TracePostTaa",0,ini.c_str())!=0;
    g_pulseJitter=GetPrivateProfileIntW(L"Experiment",L"PulseJitter",0,ini.c_str())!=0;
    g_traceTextures=GetPrivateProfileIntW(L"Experiment",L"TraceTextures",0,ini.c_str())!=0;
    g_qualityMode=GetPrivateProfileIntW(L"Experiment",L"QualityPrototype",0,ini.c_str())==1;
    g_windowMs=GetPrivateProfileIntW(L"Experiment",L"WindowMs",g_qualityMode?10000:3000,ini.c_str());
    if(g_windowMs<1000||g_windowMs>10000)throw std::runtime_error("WindowMs must be between 1000 and 10000; this is a bounded session");
    g_windowSounds=g_qualityMode&&GetPrivateProfileIntW(L"Experiment",L"WindowSounds",0,ini.c_str())==1;
    if(g_qualityMode&&(!g_traceTextures||!g_tracePostTaa||!g_gameHooks||!g_applyScalePulse||!g_pulseJitter||!g_requireFrameGenOff))
        throw std::runtime_error("Quality prototype requires all input/graph guards, jitter, scale pulse and FrameGen-off");
    if(g_traceTextures&&(!g_gameHooks||!g_tracePostTaa))throw std::runtime_error("Texture tracing requires game and post-TAA hooks");
    if(!g_gameHooks)g_applyScalePulse=false;
    if(g_pulseJitter&&(!g_gameHooks||!g_applyScalePulse))throw std::runtime_error("Jitter diagnostic requires the bounded engine scale pulse");
    Log("{\"event\":\"bridge_configuration\",\"version\":3,\"expected_proxy_sha256\":\""+g_expectedProxyHash+
        "\",\"requires_framegen_off\":"+(g_requireFrameGenOff?"true":"false")+",\"applies_scale_pulse\":"+(g_applyScalePulse?"true":"false")+
        ",\"game_hooks\":"+(g_gameHooks?"true":"false")+",\"trace_post_taa\":"+(g_tracePostTaa?"true":"false")+
        ",\"pulses_projection_jitter\":"+(g_pulseJitter?"true":"false")+",\"traces_textures\":"+(g_traceTextures?"true":"false")+
        ",\"target_window_ms\":"+std::to_string(g_windowMs)+",\"window_sounds\":"+(g_windowSounds?"true":"false")+"}");
}
void Stop() noexcept {
    g_ready.store(false);bool restored=true;
    if(g_qualityMode)try{FinishQuality();}catch(...){Log("{\"event\":\"quality_cleanup_retained\",\"reason\":\"cleanup_exception\"}");}
    RestoreJitterPulse();
    for(auto& hook:g_hooks)if(hook.enabled){
        if(memcmp(hook.address,hook.patch.data(),16)||MH_DisableHook(hook.address)!=MH_OK)restored=false;
        else hook.enabled=false;
    }
    FinishBridge();
    try {
        size_t uniqueQuads=0;
        {std::lock_guard<std::mutex> lock(g_quadsMutex);for(const auto& line:g_quadRecords)Log(line);uniqueQuads=g_seenQuads.size();}
        std::lock_guard<std::mutex> lock(g_viewsMutex);
        for(const auto& item:g_views)Log("{\"event\":\"viewport_count\",\"phase\":"+std::to_string(std::get<0>(item.first))+
            ",\"width\":"+std::to_string(std::get<1>(item.first))+",\"height\":"+std::to_string(std::get<2>(item.first))+",\"count\":"+std::to_string(item.second)+"}");
        Log("{\"event\":\"quad_summary\",\"all_quad_calls\":"+std::to_string(g_quadCalls.load())+",\"focused_quad_calls\":"+std::to_string(g_focusedQuadCalls.load())+",\"unique_quad_records\":"+std::to_string(uniqueQuads)+"}");
        Log(std::string("{\"event\":\"stopped\",\"owned_hooks_restored\":")+(restored?"true":"false")+",\"scale_override_pending\":"+(g_changed.load()?"true":"false")+
            ",\"jitter_override_pending\":"+(g_jitterChanged.load()?"true":"false")+"}");
    }catch(...){}
}
DWORD WINAPI Worker(void*) {
    HANDLE controls[2]{};
    try {
        const auto executable=ModulePath(nullptr);if(_wcsicmp(executable.filename().c_str(),L"MonsterHunterWorld.exe"))return 0;
        const auto folder=ModulePath(g_self).parent_path(),ini=folder/L"MhwSrBridge.ini";
        if(GetPrivateProfileIntW(L"Experiment",L"Enabled",0,ini.c_str())!=1)return 0;
        g_log.open(folder/(L"MhwSrBridge-"+std::to_wstring(GetCurrentProcessId())+L".jsonl"),std::ios::app);
        ReadBridgeConfiguration(ini);
        if(g_gameHooks)PrepareGameHook();
        else {
            g_game=GetModuleHandleW(nullptr);
            if(FileSha256(ModulePath(g_game))!=kGameHash||!Pin(g_self)||!Pin(g_game)||MH_Initialize()!=MH_OK)
                throw std::runtime_error("Input-only observer initialization failed");
        }
        const auto prefix=L"Local\\MhwSrBridge."+std::to_wstring(GetCurrentProcessId());
        controls[0]=CreateEventW(nullptr,TRUE,FALSE,(prefix+L".Cancel").c_str());
        controls[1]=CreateEventW(nullptr,FALSE,FALSE,(prefix+L".Start").c_str());
        if(!controls[0]||!controls[1])throw std::runtime_error("Control events unavailable");
        Log("{\"event\":\"waiting_for_scene_signal\",\"version\":3,\"changes_scale\":false}");
        while(true) {
            const auto wait=WaitForMultipleObjects(2,controls,FALSE,250);
            if(wait==WAIT_OBJECT_0+1)break;
            if(wait==WAIT_OBJECT_0||GetPrivateProfileIntW(L"Experiment",L"Enabled",0,ini.c_str())!=1)throw std::runtime_error("Cancelled before change");
        }
        const auto graphics=executable.parent_path()/L"graphics_option.ini",mhwss=executable.parent_path()/L"MHWSS"/L"MHWSS_config.toml",
            optiscaler=executable.parent_path()/L"OptiScaler.ini";
        uint64_t stableSince=0;
        Log("{\"event\":\"waiting_for_configuration\",\"version\":3,\"requires\":\"High, DX12, MHWSS None, configured FrameGen constraint, stable 5 seconds\",\"writes_while_waiting\":false}");
        while(true) {
            const auto now=GetTickCount64();
            const bool allowed=Setting(graphics,"ResolutionScaling")=="High"&&Setting(graphics,"DirectX12Enable")=="On"&&
                Setting(mhwss,"Upscaler")=="None"&&FrameGenDisabled(optiscaler)&&QualityConfiguration(graphics,optiscaler);
            if(!allowed)stableSince=0;else if(!stableSince)stableSince=now;
            if(stableSince&&now-stableSince>=5000)break;
            if(WaitForSingleObject(controls[0],0)==WAIT_OBJECT_0||GetPrivateProfileIntW(L"Experiment",L"Enabled",0,ini.c_str())!=1)throw std::runtime_error("Cancelled before change");
            Sleep(250);
        }
        Install();const auto started=GetTickCount64();bool timeoutReported=false,qualityAnnounced=false,restoreAnnounced=false;
        uint64_t lastWindowReport=0;
        while(!g_done.load()) {
            Sleep(100);
            if(WaitForSingleObject(controls[0],0)==WAIT_OBJECT_0)g_abort.store(true);
            if(Setting(graphics,"ResolutionScaling")!="High"||Setting(graphics,"DirectX12Enable")!="On"||
               Setting(mhwss,"Upscaler")!="None"||!FrameGenDisabled(optiscaler)||!QualityConfiguration(graphics,optiscaler)||
               GetPrivateProfileIntW(L"Experiment",L"Enabled",0,ini.c_str())!=1)g_abort.store(true);
            // Cues and progress follow completed SR output commands, never just
            // a requested scale change. Audio runs on this worker, not a render hook.
            const auto now=GetTickCount64();
            if(g_qualityMode) {
                const auto frames=g_qualityFinalDraws.load(),deadline=g_windowDeadline.load();
                if(frames&&g_phase.load()==2&&!g_abort.load()&&now<deadline) {
                    if(now-lastWindowReport>=1000) {
                        Log("{\"event\":\"quality_window_status\",\"state\":\"quality\",\"tick_ms\":"+std::to_string(now)+
                            ",\"remaining_ms\":"+std::to_string(deadline-now)+",\"full_final_draws\":"+std::to_string(frames)+"}");
                        lastWindowReport=now;
                    }
                    if(!qualityAnnounced){qualityAnnounced=true;if(g_windowSounds)Beep(880,120);}
                }
                if(g_restoreObserved.load()&&!restoreAnnounced) {
                    restoreAnnounced=true;
                    Log("{\"event\":\"quality_window_status\",\"state\":\"restored\",\"tick_ms\":"+std::to_string(now)+
                        ",\"active_scale\":"+std::to_string(g_restoreExpected)+",\"full_final_draws\":"+std::to_string(frames)+"}");
                    if(g_windowSounds&&qualityAnnounced)Beep(440,200);
                }
            }
            if(!g_gameHooks&&(g_abort.load()||GetTickCount64()-started>=5000)) {
                Log("{\"event\":\"input_capture_complete\",\"changes_scale\":false}");g_done.store(true);break;
            }
            if(GetTickCount64()-started>17000+g_windowMs) {
                g_abort.store(true);
                if(!g_changed.load()&&g_phase.load()<2)break;
                if(!timeoutReported){Log("{\"event\":\"awaiting_render_thread_restoration\"}");timeoutReported=true;}
            }
        }
    }catch(const std::exception& e){Log(std::string("{\"event\":\"refused\",\"reason\":\"")+e.what()+"\"}");}
    // When an override exists, leave the callback available to restore on render resume.
    if(g_changed.load()){g_abort.store(true);while(!g_done.load())Sleep(100);}
    Stop();for(auto event:controls)if(event)CloseHandle(event);
    const auto ini=ModulePath(g_self).parent_path()/L"MhwSrBridge.ini";
    WritePrivateProfileStringW(L"Experiment",L"Enabled",L"0",ini.c_str());
    return 0;
}
void Start(){if(InterlockedCompareExchange(&g_started,1,0))return;const auto t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);if(t)CloseHandle(t);}
}
extern "C" __declspec(dllexport) void Initialize(){Start();}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){g_self=module;Start();}return TRUE;}
