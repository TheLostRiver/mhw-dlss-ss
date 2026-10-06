// Standalone input host, first stage: capture native camera/screen bindings.
// No SR commands, scale changes, projection writes, or third-party host callbacks.
#include "../readback/CaptureCommon.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <atomic>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

namespace {
using Microsoft::WRL::ComPtr;
constexpr char kGameHash[]="c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";
constexpr char kTaaShader[]="2d7b26c742c27db2c83546c6cf6a1dfda47d7cfd4336e69f8c2302de5802b0d1";
HMODULE g_self{};
std::atomic<bool> g_observing{false},g_capture{false};
std::atomic<unsigned> g_busy{0};
std::atomic<uint64_t> g_dispatches{0},g_taaSamples{0},g_valid{0},g_nonzeroJitter{0},g_fresh{0};
std::atomic<unsigned> g_arenaCaptures{0},g_computePipelines{0};
std::mutex g_mutex,g_logMutex;
std::ofstream g_log;
thread_local bool g_internal=false;
struct Busy {Busy(){++g_busy;}~Busy(){--g_busy;}};
struct Internal {bool previous=g_internal;Internal(){g_internal=true;}~Internal(){g_internal=previous;}};
void Log(const std::string& line) noexcept {try{std::lock_guard<std::mutex> lock(g_logMutex);if(g_log){g_log<<line<<'\n';g_log.flush();}}catch(...) {}}
bool Active(){return g_observing.load()&&!g_internal;}
bool CopyBytes(void* to,const void* from,size_t count) noexcept {__try{memcpy(to,from,count);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
std::string Digest(const void* data,size_t bytes) {
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};DWORD size=0,used=0;std::string result;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&used,0)>=0) {
        std::vector<unsigned char> storage(size);std::array<unsigned char,32> out{};
        if(BCryptCreateHash(alg,&hash,storage.data(),size,nullptr,0,0)>=0) {
            if(bytes<=1048576&&BCryptHashData(hash,static_cast<PUCHAR>(const_cast<void*>(data)),ULONG(bytes),0)>=0&&
               BCryptFinishHash(hash,out.data(),DWORD(out.size()),0)>=0)result=HexBytes(out.data(),out.size());
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(alg,0);return result;
}
struct Arena {ComPtr<ID3D12Resource> resource;UINT64 gpu=0,bytes=0;const unsigned char* cpu=nullptr;};
std::array<Arena,4> g_arenas{};
std::set<ID3D12PipelineState*> g_taaPsos;
std::vector<ComPtr<ID3D12PipelineState>> g_psoReferences;
struct ListState {ID3D12PipelineState* pso=nullptr;UINT64 camera=0,screen=0;uint64_t epoch=0,cameraEpoch=0,screenEpoch=0,lastTaa=0,generation=0;};
std::unordered_map<ID3D12GraphicsCommandList*,ListState> g_lists;
using ResourceCreate=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
using GpuAddress=UINT64(STDMETHODCALLTYPE*)(ID3D12Resource*);
using ComputeCreate=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*,REFIID,void**);
using Pipeline=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
using Dispatch=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
using Cbv=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT64);
using Reset=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
ResourceCreate g_resourceCreate{};GpuAddress g_gpuAddress{};ComputeCreate g_computeCreate{};
Pipeline g_pipeline{},g_clear{};Dispatch g_dispatch{};Cbv g_cbv{};Reset g_reset{};
struct Hook {void* address=nullptr;std::array<unsigned char,16> original{},patch{};bool enabled=false;};
std::vector<Hook> g_hooks;

void ObserveArena(ID3D12Resource* resource,UINT64 knownAddress=0) {
    if(!resource||!Active())return;Busy busy;Internal internal;
    const auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||desc.Width!=0x3000000)return;
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    if(FAILED(resource->GetHeapProperties(&heap,&flags))||
       (heap.Type!=D3D12_HEAP_TYPE_UPLOAD&&!(heap.Type==D3D12_HEAP_TYPE_CUSTOM&&heap.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)))return;
    {std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto& arena:g_arenas)if(arena.resource.Get()==resource)return;
        if(std::none_of(g_arenas.begin(),g_arenas.end(),[](const Arena& a){return !a.resource;}))return;}
    const auto address=knownAddress?knownAddress:resource->GetGPUVirtualAddress();if(!address)return;
    const D3D12_RANGE read{0,SIZE_T(desc.Width)};void* mapped=nullptr;
    if(FAILED(resource->Map(0,&read,&mapped))||!mapped)return;
    bool retained=false;
    {std::lock_guard<std::mutex> lock(g_mutex);
        const bool duplicate=std::any_of(g_arenas.begin(),g_arenas.end(),[&](const Arena& a){return a.resource.Get()==resource;});
        auto at=std::find_if(g_arenas.begin(),g_arenas.end(),[](const Arena& a){return !a.resource;});
        if(g_observing.load()&&!duplicate&&at!=g_arenas.end()){
            at->resource=resource;at->gpu=address;at->bytes=desc.Width;at->cpu=static_cast<const unsigned char*>(mapped);retained=true;}}
    if(!retained){const D3D12_RANGE noWrite{0,0};resource->Unmap(0,&noWrite);return;}
    ++g_arenaCaptures;
    Log("{\"event\":\"native_arena_captured\",\"bytes\":"+std::to_string(desc.Width)+",\"writes_resource\":false}");
}
HRESULT STDMETHODCALLTYPE OnResourceCreate(ID3D12Device* d,const D3D12_HEAP_PROPERTIES* h,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    const auto hr=g_resourceCreate(d,h,flags,desc,state,clear,iid,out);
    if(SUCCEEDED(hr)&&out&&*out&&desc&&desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&desc->Width==0x3000000&&Active())try {
        ComPtr<ID3D12Resource> resource;if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&resource))))ObserveArena(resource.Get());
    }catch(...){Log("{\"event\":\"arena_capture_exception\"}");}
    return hr;
}
UINT64 STDMETHODCALLTYPE OnGpuAddress(ID3D12Resource* resource) {
    const auto address=g_gpuAddress(resource);if(Active())try{ObserveArena(resource,address);}catch(...){}return address;
}
HRESULT STDMETHODCALLTYPE OnComputeCreate(ID3D12Device* d,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,REFIID iid,void** out) {
    const auto hr=g_computeCreate(d,desc,iid,out);
    if(SUCCEEDED(hr)&&Active()&&desc&&out&&*out&&desc->CS.pShaderBytecode&&desc->CS.BytecodeLength>=32&&desc->CS.BytecodeLength<=1048576)try {
        Busy busy;Internal internal;
        const auto digest=Digest(desc->CS.pShaderBytecode,desc->CS.BytecodeLength);
        if(++g_computePipelines<=128)Log("{\"event\":\"native_compute_pipeline\",\"bytes\":"+std::to_string(desc->CS.BytecodeLength)+",\"sha256\":\""+digest+"\"}");
        if(digest==kTaaShader) {
            ComPtr<ID3D12PipelineState> pso;
            if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&pso)))) {
                std::lock_guard<std::mutex> lock(g_mutex);
                if(g_taaPsos.size()<8&&g_taaPsos.insert(pso.Get()).second){g_psoReferences.push_back(pso);Log("{\"event\":\"native_taa_pso_identified\",\"source\":\"shader_bytecode_sha256\"}");}
            }
        }
    }catch(...){Log("{\"event\":\"pso_capture_exception\"}");}
    return hr;
}
void SetPipeline(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso,bool clear) {
    if(!Active())return;Busy busy;std::lock_guard<std::mutex> lock(g_mutex);
    if(g_lists.size()>=512&&!g_lists.count(list))return;
    auto& s=g_lists[list];if(clear){const auto next=s.generation+1;s={};s.generation=next;}s.pso=pso;
}
void STDMETHODCALLTYPE OnPipeline(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso) {try{SetPipeline(list,pso,false);}catch(...){}g_pipeline(list,pso);}
void STDMETHODCALLTYPE OnClear(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso) {g_clear(list,pso);try{SetPipeline(list,pso,true);}catch(...) {}}
HRESULT STDMETHODCALLTYPE OnReset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pso) {
    const auto hr=g_reset(list,allocator,pso);if(SUCCEEDED(hr))try{SetPipeline(list,pso,true);}catch(...){}return hr;
}
void STDMETHODCALLTYPE OnCbv(ID3D12GraphicsCommandList* list,UINT root,UINT64 address) {
    if(Active()&&(root==2||root==3))try {
        Busy busy;std::lock_guard<std::mutex> lock(g_mutex);
        if(g_lists.size()<512||g_lists.count(list)){auto& s=g_lists[list];++s.epoch;
            if(root==2){s.camera=address;s.cameraEpoch=s.epoch;}else{s.screen=address;s.screenEpoch=s.epoch;}}
    }catch(...){}g_cbv(list,root,address);
}
const unsigned char* BufferAddress(UINT64 address,size_t bytes) {
    for(const auto& a:g_arenas)if(a.cpu&&bytes<=a.bytes&&address>=a.gpu&&address-a.gpu<=a.bytes-bytes)return a.cpu+(address-a.gpu);
    return nullptr;
}
void ObserveDispatch(ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z) {
    if(!Active()||!g_capture.load())return;Busy busy;
    ++g_dispatches;std::array<unsigned,27> screen{};float jitter[2]{},previous[2]{};bool fresh=false,copied=false;
    uint64_t generation=0;unsigned psoCount=0,arenaCount=0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);const auto found=g_lists.find(list);
        if(found==g_lists.end()||!g_taaPsos.count(found->second.pso))return;
        auto& s=found->second;generation=s.generation;fresh=s.cameraEpoch>s.lastTaa&&s.screenEpoch>s.lastTaa;s.lastTaa=s.epoch;
        const auto* camera=BufferAddress(s.camera,712);const auto* data=BufferAddress(s.screen,sizeof(screen));
        copied=!(s.camera&255)&&!(s.screen&255)&&camera&&data&&CopyBytes(screen.data(),data,sizeof(screen))&&
            CopyBytes(jitter,camera+160,8)&&CopyBytes(previous,camera+704,8);
        psoCount=unsigned(g_taaPsos.size());for(const auto& a:g_arenas)if(a.resource)++arenaCount;
    }
    const auto serial=++g_taaSamples;float width=0,height=0,scale=0;memcpy(&width,&screen[4],4);memcpy(&height,&screen[5],4);memcpy(&scale,&screen[21],4);
    bool finite=true;for(float v:{jitter[0],jitter[1],previous[0],previous[1]})finite=finite&&std::isfinite(v)&&std::fabs(v)<0.1f;
    const bool valid=copied&&fresh&&generation&&finite&&std::isfinite(width)&&std::isfinite(height)&&
        width>=1280&&height>=720&&width<=16384&&height<=16384&&screen[10]&&screen[11]&&screen[10]<=width&&screen[11]<=height&&
        std::isfinite(scale)&&scale>=0.5f&&scale<=1.0f;
    if(fresh)++g_fresh;if(valid)++g_valid;
    const bool nonzero=valid&&(std::fabs(jitter[0])+std::fabs(jitter[1])>0.00000001f);if(nonzero)++g_nonzeroJitter;
    if(serial>16&&serial%120!=0&&valid)return;
    std::ostringstream out;out<<std::setprecision(9)<<"{\"event\":\"native_taa_input\",\"serial\":"<<serial
        <<",\"valid\":"<<(valid?"true":"false")<<",\"copied\":"<<(copied?"true":"false")<<",\"fresh\":"<<(fresh?"true":"false")
        <<",\"known_psos\":"<<psoCount<<",\"known_arenas\":"<<arenaCount<<",\"dispatch\":["<<x<<','<<y<<','<<z<<']';
    if(valid)out<<",\"output\":["<<width<<','<<height<<"],\"input\":["<<screen[10]<<','<<screen[11]<<"],\"scale\":"<<scale
        <<",\"projection_jitter\":["<<jitter[0]<<','<<jitter[1]<<"],\"previous_projection_jitter\":["<<previous[0]<<','<<previous[1]<<']';
    out<<",\"modifies_rendering\":false}";Log(out.str());
}
void STDMETHODCALLTYPE OnDispatch(ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z) {try{ObserveDispatch(list,x,y,z);}catch(...){Log("{\"event\":\"native_input_exception\"}");}g_dispatch(list,x,y,z);}

std::wstring Ini(const std::filesystem::path& path,const std::wstring& name) {
    std::array<wchar_t,256> value{};GetPrivateProfileStringW(L"Capture",name.c_str(),L"",value.data(),DWORD(value.size()),path.c_str());return value.data();
}
template<class Fn> void HookMethod(HMODULE core,const std::filesystem::path& methods,const wchar_t* name,void* callback,Fn& original) {
    const auto rva=Ini(methods,std::wstring(name)+L"Rva"),bytes=Ini(methods,std::wstring(name)+L"Bytes");
    if(rva.empty()||bytes.size()!=32)throw std::runtime_error("Native method calibration missing");
    const auto offset=std::stoull(rva,nullptr,0);const auto* image=reinterpret_cast<const unsigned char*>(core);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    if(offset>=nt->OptionalHeader.SizeOfImage||nt->OptionalHeader.SizeOfImage-offset<16)throw std::runtime_error("Native method RVA outside runtime image");
    Hook hook;hook.address=reinterpret_cast<unsigned char*>(core)+offset;
    for(unsigned i=0;i<16;++i)hook.original[i]=static_cast<unsigned char>(std::stoul(bytes.substr(i*2,2),nullptr,16));
    if(memcmp(hook.address,hook.original.data(),16))throw std::runtime_error("Native API entry is already patched or differs from calibration");
    if(MH_CreateHook(hook.address,callback,reinterpret_cast<void**>(&original))!=MH_OK)throw std::runtime_error("Native API hook creation failed");
    g_hooks.push_back(hook);
}
void Stop() {
    g_capture.store(false);g_observing.store(false);bool restored=true;
    for(auto& h:g_hooks)if(h.enabled){if(memcmp(h.address,h.patch.data(),16)||MH_DisableHook(h.address)!=MH_OK)restored=false;else h.enabled=false;}
    const auto deadline=GetTickCount64()+2000;while(g_busy.load()&&GetTickCount64()<deadline)Sleep(10);
    if(!g_busy.load()) {
        std::lock_guard<std::mutex> lock(g_mutex);const D3D12_RANGE noWrite{0,0};
        for(auto& a:g_arenas)if(a.resource){a.resource->Unmap(0,&noWrite);a={};}g_psoReferences.clear();
    }
    Log(std::string("{\"event\":\"native_host_stopped\",\"owned_hooks_restored\":")+(restored?"true":"false")+",\"modifies_rendering\":false}");
}
DWORD WINAPI Worker(void*) {
    HANDLE start=nullptr,cancel=nullptr;const auto folder=ModulePath(g_self).parent_path(),ini=folder/L"MhwNativeHost.ini";
    try {
        if(_wcsicmp(ModulePath(nullptr).filename().c_str(),L"MonsterHunterWorld.exe")||GetPrivateProfileIntW(L"Standalone",L"Enabled",0,ini.c_str())!=1)return 0;
        g_log.open(folder/(L"MhwNativeHost-"+std::to_wstring(GetCurrentProcessId())+L".jsonl"),std::ios::app);
        if(GetModuleHandleW(L"MHWSS.dll"))throw std::runtime_error("Standalone capture requires a process without MHWSS loaded");
        HMODULE pinned{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(g_self),&pinned))throw std::runtime_error("Host pin failed");
        HMODULE core=nullptr;const auto deadline=GetTickCount64()+60000;
        while(!core&&GetTickCount64()<deadline){core=GetModuleHandleW(L"D3D12Core.dll");if(!core)Sleep(10);}
        const auto methods=folder/L"MhwNativeMethods.ini";const auto expected=Ini(methods,L"CoreSha256");
        const auto actual=core?FileSha256(ModulePath(core)):std::string();
        if(!core||std::wstring(actual.begin(),actual.end())!=expected)throw std::runtime_error("Native D3D12 runtime differs from calibration");
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(core),&pinned))throw std::runtime_error("Native runtime pin failed");
        if(MH_Initialize()!=MH_OK)throw std::runtime_error("Native hook initialization failed");
        HookMethod(core,methods,L"CreateCommittedResource",reinterpret_cast<void*>(&OnResourceCreate),g_resourceCreate);
        HookMethod(core,methods,L"GetGPUVirtualAddress",reinterpret_cast<void*>(&OnGpuAddress),g_gpuAddress);
        HookMethod(core,methods,L"CreateComputePipelineState",reinterpret_cast<void*>(&OnComputeCreate),g_computeCreate);
        HookMethod(core,methods,L"SetPipelineState",reinterpret_cast<void*>(&OnPipeline),g_pipeline);
        HookMethod(core,methods,L"ClearState",reinterpret_cast<void*>(&OnClear),g_clear);
        HookMethod(core,methods,L"Reset",reinterpret_cast<void*>(&OnReset),g_reset);
        HookMethod(core,methods,L"SetComputeRootConstantBufferView",reinterpret_cast<void*>(&OnCbv),g_cbv);
        HookMethod(core,methods,L"Dispatch",reinterpret_cast<void*>(&OnDispatch),g_dispatch);
        g_observing.store(true);
        for(auto& h:g_hooks){if(MH_EnableHook(h.address)!=MH_OK)throw std::runtime_error("Native hook enable failed");h.enabled=true;memcpy(h.patch.data(),h.address,16);}
        // Capture startup-created buffers before the potentially large EXE hash.
        // These hooks only observe API objects; no rendering is changed.
        if(FileSha256(ModulePath(nullptr))!=kGameHash)throw std::runtime_error("Game build differs");
        Log("{\"event\":\"native_host_ready\",\"independent_host\":true,\"sr_execution\":false,\"modifies_rendering\":false}");
        const auto prefix=L"Local\\MhwNativeHost."+std::to_wstring(GetCurrentProcessId());
        start=CreateEventW(nullptr,FALSE,FALSE,(prefix+L".Start").c_str());cancel=CreateEventW(nullptr,TRUE,FALSE,(prefix+L".Cancel").c_str());
        if(!start||!cancel)throw std::runtime_error("Native control events unavailable");
        HANDLE events[]{cancel,start};
        bool keyWasDown=true;uint64_t nextConfigCheck=0;
        while(true){const auto wait=WaitForMultipleObjects(2,events,FALSE,25);if(wait==WAIT_OBJECT_0+1)break;
            if(wait==WAIT_OBJECT_0)throw std::runtime_error("Native capture cancelled");
            if(GetTickCount64()>=nextConfigCheck){nextConfigCheck=GetTickCount64()+1000;
                if(GetPrivateProfileIntW(L"Standalone",L"Enabled",0,ini.c_str())!=1)throw std::runtime_error("Native capture disabled");}
            DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            const bool down=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
            if(foreground==GetCurrentProcessId()){if(down&&!keyWasDown)break;keyWasDown=down;}else keyWasDown=true;
        }
        if(GetModuleHandleW(L"MHWSS.dll"))throw std::runtime_error("A third-party host was loaded after startup");
        const auto end=GetTickCount64()+10000;
        Log("{\"event\":\"native_capture_started\",\"duration_ms\":10000,\"mhwss_loaded\":false,\"sr_execution\":false}");g_capture.store(true);Beep(880,120);
        while(GetTickCount64()<end&&WaitForSingleObject(cancel,100)!=WAIT_OBJECT_0){}
        g_capture.store(false);Beep(440,200);
        Log("{\"event\":\"native_input_summary\",\"dispatches\":"+std::to_string(g_dispatches.load())+",\"taa_samples\":"+std::to_string(g_taaSamples.load())+
            ",\"valid_inputs\":"+std::to_string(g_valid.load())+",\"fresh_cbvs\":"+std::to_string(g_fresh.load())+",\"nonzero_jitter\":"+std::to_string(g_nonzeroJitter.load())+
            ",\"arena_captures\":"+std::to_string(g_arenaCaptures.load())+",\"compute_pipelines\":"+std::to_string(g_computePipelines.load())+",\"sr_execution\":false}");
    }catch(const std::exception& e){Log(std::string("{\"event\":\"native_host_refused\",\"reason\":\"")+e.what()+"\"}");}
    Stop();if(start)CloseHandle(start);if(cancel)CloseHandle(cancel);WritePrivateProfileStringW(L"Standalone",L"Enabled",L"0",ini.c_str());return 0;
}
volatile LONG g_started=0;
void Start(){if(InterlockedCompareExchange(&g_started,1,0))return;const auto thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
}
extern "C" __declspec(dllexport) void Initialize(){Start();}
BOOL APIENTRY DllMain(HMODULE self,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){g_self=self;Start();}return TRUE;}
