// Reads known CBScreen blocks from MHWSS's existing CPU-visible buffer reference.
// Never writes to the buffer, never evaluates DLSS/DLAA, and adds no GPU commands.
#include "../readback/CaptureCommon.h"
#include <d3d12.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace {
constexpr char kMhwssHash[] = "55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a";
constexpr char kGameHash[] = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";
constexpr size_t kMainBufferRva = 0x54ebf8;
constexpr UINT64 kBufferBytes = 0x3000000;
constexpr UINT64 kCbAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr UINT64 kSamplePeriodMs = 100;
constexpr unsigned kReadsPerPeriod = 2048;
using List = ID3D12GraphicsCommandList;
using CbvFn = void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using ViewportFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
CbvFn g_compute{}, g_graphics{}; ViewportFn g_viewport{}; ResetFn g_reset{};
HMODULE g_self{}, g_mhwss{};
volatile LONG g_started = 0;
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_bindings{0}, g_inRange{0}, g_matches{0}, g_lockMisses{0};
std::ofstream g_log;
std::mutex g_mutex, g_logMutex;
ID3D12Resource* g_buffer = nullptr; // Explicitly released by worker, not under process-detach loader lock.
const unsigned char* g_mapped = nullptr;
UINT64 g_gpuBase = 0;
unsigned g_session = 0, g_dropped = 0;
uint64_t g_sampleEpoch = 0, g_reads = 0, g_repeatedAddresses = 0, g_budgetSkips = 0, g_readFailures = 0, g_unaligned = 0;
unsigned g_periodReads = 0, g_rawDiagnostics = 0, g_nearDiagnostics = 0;
unsigned g_stackSamples = 0;
std::array<uint64_t, kBufferBytes / kCbAlignment> g_addressEpochs{};
std::set<uint64_t> g_rejectedKinds;
std::string g_armFailure = "not_attempted";
std::string g_lastArenaDescription;
std::string g_setting;
std::string g_upscaler;
std::map<List*, D3D12_VIEWPORT> g_viewports;
std::set<std::string> g_seen;
std::vector<std::string> g_records;
struct Hook { void* target{}; std::array<unsigned char, 16> patch{}; bool enabled = false; };
std::array<Hook, 4> g_hooks{};

void Log(const std::string& line) noexcept {
    try { std::lock_guard<std::mutex> lock(g_logMutex); if (g_log) { g_log << line << '\n'; g_log.flush(); } } catch (...) {}
}
std::string Hex(UINT64 value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }
bool Pin(HMODULE module) { HMODULE pinned{}; return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned) != FALSE; }

// Only the independently recovered MHWSS graphics-buffer pointer is read.
ID3D12Resource* AcquireMainBuffer() noexcept {
    __try {
        auto* resource = *reinterpret_cast<ID3D12Resource**>(reinterpret_cast<unsigned char*>(g_mhwss) + kMainBufferRva);
        if (resource) resource->AddRef();
        return resource;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CopyBytes(const void* source, void* destination, size_t count) noexcept {
    __try { memcpy(destination, source, count); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float Float(const std::array<uint32_t, 27>& words, size_t byteOffset) {
    float value; memcpy(&value, &words[byteOffset / 4], 4); return value;
}
bool Close(float a, float b, float tolerance) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance; }
// A nonzero result identifies the first failed invariant, retained in bounded diagnostics.
unsigned ScreenBlockFailure(const std::array<uint32_t, 27>& words) {
    const float width = Float(words, 16), height = Float(words, 20);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 32 || height < 32 || width > 16384 || height > 16384) return 1;
    if (!Close(width, std::round(width), 0.01f) || !Close(height, std::round(height), 0.01f)) return 1;
    if (!Close(width * Float(words, 24), 1, 0.002f) || !Close(height * Float(words, 28), 1, 0.002f)) return 2;
    const auto viewWidth = words[10], viewHeight = words[11];
    if (!viewWidth || !viewHeight || viewWidth > 16384 || viewHeight > 16384 || words[26] > 1) return 3;
    // Do not assume the float view representation is identical to the integer viewport.
    // Observe both representations and only require internally consistent reciprocals.
    if (Float(words, 56) <= 0 || Float(words, 60) <= 0 || Float(words, 56) > 16384 || Float(words, 60) > 16384) return 4;
    if (!Close(Float(words, 64) * Float(words, 56), 1, 0.002f) || !Close(Float(words, 68) * Float(words, 60), 1, 0.002f)) return 4;
    for (size_t offset = 72; offset <= 100; offset += 4) {
        const auto value = Float(words, offset); if (!std::isfinite(value) || value < 0 || value > 64) return 5;
    }
    // Ring-buffer reuse can leave CBScreen's tail behind a different CB's integer header.
    // Denormal float headers observed in v2 were tiny bit-cast draw counters, not screen transforms.
    for (size_t offset = 0; offset < 16; offset += 4) {
        const auto value = Float(words, offset);
        if (!std::isfinite(value) || (value != 0 && !std::isnormal(value)) || std::fabs(value) > 16384) return 6;
    }
    return 0;
}
void Number(std::ostream& out, float value) { if (std::isfinite(value)) out << std::setprecision(9) << value; else out << "null"; }
void Pair(std::ostream& out, const std::array<uint32_t, 27>& words, size_t offset) { out << '['; Number(out, Float(words, offset)); out << ','; Number(out, Float(words, offset + 4)); out << ']'; }
void Stack(std::ostream& out) {
    std::array<void*, 20> addresses{};
    const auto count = CaptureStackBackTrace(1, static_cast<DWORD>(addresses.size()), addresses.data(), nullptr);
    out << '[';
    for (USHORT i = 0; i < count; ++i) {
        if (i) out << ',';
        out << '"' << Hex(reinterpret_cast<uintptr_t>(addresses[i])) << '"';
    }
    out << ']';
}

void Observe(List* list, UINT root, UINT64 address, bool compute) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) return;
    g_bindings.fetch_add(1, std::memory_order_relaxed);
    try {
        // Never wait behind another render thread or the log-draining worker.
        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock()) { g_lockMisses.fetch_add(1, std::memory_order_relaxed); return; }
        if (!g_active.load() || !g_mapped || address < g_gpuBase || address - g_gpuBase > kBufferBytes - 108) return;
        g_inRange.fetch_add(1, std::memory_order_relaxed);
        const auto offset = address - g_gpuBase;
        if (offset % kCbAlignment) { ++g_unaligned; return; }
        const auto tick = GetTickCount64(), epoch = tick / kSamplePeriodMs + 1;
        auto& lastEpoch = g_addressEpochs[static_cast<size_t>(offset / kCbAlignment)];
        if (lastEpoch == epoch) { ++g_repeatedAddresses; return; }
        if (g_sampleEpoch != epoch) { g_sampleEpoch = epoch; g_periodReads = 0; }
        if (g_periodReads >= kReadsPerPeriod) { ++g_budgetSkips; return; }
        lastEpoch = epoch; ++g_periodReads; ++g_reads;
        std::array<uint32_t, 27> words{};
        if (!CopyBytes(g_mapped + offset, words.data(), sizeof(words))) { ++g_readFailures; return; }
        const auto failure = ScreenBlockFailure(words);
        if (failure) {
            // Reserve separate space for almost-valid blocks so unrelated CBs cannot hide a layout mismatch.
            auto& diagnostics = failure >= 3 ? g_nearDiagnostics : g_rawDiagnostics;
            const unsigned limit = failure >= 3 ? 24 : 8;
            const uint64_t kind = (static_cast<uint64_t>(failure) << 33) | (static_cast<uint64_t>(compute) << 32) | root;
            if (diagnostics >= limit || !g_rejectedKinds.insert(kind).second) return;
            ++diagnostics;
        } else {
            g_matches.fetch_add(1, std::memory_order_relaxed);
            const auto key = std::string(compute ? "c" : "g") + std::to_string(root) + HexBytes(words.data(), sizeof(words));
            if (g_seen.count(key)) return;
            if (g_seen.size() >= 512) { ++g_dropped; return; }
            g_seen.insert(key);
        }
        std::ostringstream out;
        out << "{\"event\":\"" << (failure ? "rejected_block" : "screen_constants") << "\",\"first_failed_invariant\":" << failure
            << ",\"observation\":\"cpu_at_cbv_bind\",\"session\":" << g_session << ",\"tick_ms\":" << tick
            << ",\"setting\":\"" << g_setting << "\",\"mhwss_upscaler_config\":\"" << g_upscaler
            << "\",\"stage\":\"" << (compute ? "compute" : "graphics") << "\",\"root\":" << root
            << ",\"list\":\"" << Hex(reinterpret_cast<uintptr_t>(list)) << "\",\"gpu_address\":\"" << Hex(address)
            << "\",\"arena_offset\":" << address - g_gpuBase << ",\"screen_size\":"; Pair(out, words, 16);
        out << ",\"screen_inverse\":"; Pair(out, words, 24);
        out << ",\"view_offset_uint\":[" << words[8] << ',' << words[9] << "],\"view_size_uint\":[" << words[10] << ',' << words[11] << ']';
        out << ",\"view_size_float\":"; Pair(out, words, 56);
        out << ",\"view_inverse\":"; Pair(out, words, 64);
        out << ",\"screen_offset\":"; Pair(out, words, 0);
        out << ",\"screen_scale\":"; Pair(out, words, 8);
        out << ",\"view_offset_float\":"; Pair(out, words, 48);
        out << ",\"scales\":{";
        constexpr std::array<const char*, 8> names{"content", "pf", "base", "actual", "inverse", "base_inverse", "actual_inverse", "pass_screen"};
        for (size_t i = 0; i < names.size(); ++i) { if (i) out << ','; out << '"' << names[i] << "\":"; Number(out, Float(words, 72 + i * 4)); }
        out << "},\"checkerboard\":" << words[26] << ",\"last_observed_viewport\":";
        const auto viewport = g_viewports.find(list);
        if (viewport == g_viewports.end()) out << "null";
        else { out << '['; Number(out, viewport->second.TopLeftX); out << ','; Number(out, viewport->second.TopLeftY); out << ','; Number(out, viewport->second.Width); out << ','; Number(out, viewport->second.Height); out << ']'; }
        out << ",\"raw_words\":["; for (size_t i = 0; i < words.size(); ++i) { if (i) out << ','; out << words[i]; } out << ']';
        if (!failure && g_stackSamples < 64) { ++g_stackSamples; out << ",\"binding_call_stack\":"; Stack(out); }
        out << '}';
        g_records.push_back(out.str());
    } catch (...) {}
}
void STDMETHODCALLTYPE OnCompute(List* list, UINT root, UINT64 address) { Observe(list, root, address, true); g_compute(list, root, address); }
void STDMETHODCALLTYPE OnGraphics(List* list, UINT root, UINT64 address) { Observe(list, root, address, false); g_graphics(list, root, address); }
void STDMETHODCALLTYPE OnViewport(List* list, UINT count, const D3D12_VIEWPORT* viewports) {
    if (g_active.load() && count == 1 && viewports) try { std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock); if (lock.owns_lock() && g_active.load() && (g_viewports.size() < 1024 || g_viewports.count(list))) g_viewports[list] = viewports[0]; } catch (...) {}
    g_viewport(list, count, viewports);
}
HRESULT STDMETHODCALLTYPE OnReset(List* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    const auto hr = g_reset(list, allocator, pso);
    if (SUCCEEDED(hr) && g_active.load()) try { std::lock_guard<std::mutex> lock(g_mutex); g_viewports.erase(list); } catch (...) {}
    return hr;
}
void Drain() {
    std::vector<std::string> records;
    { std::lock_guard<std::mutex> lock(g_mutex); records.swap(g_records); }
    for (const auto& line : records) Log(line);
}
void UnmapBuffer() {
    g_active.store(false);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buffer) {
        if (g_mapped) { D3D12_RANGE noWrite{0, 0}; g_buffer->Unmap(0, &noWrite); }
        g_buffer->Release();
    }
    g_buffer = nullptr; g_mapped = nullptr; g_gpuBase = 0;
}
bool Arm(const std::string& setting, const std::string& upscaler) {
    auto* resource = AcquireMainBuffer(); if (!resource) { g_armFailure = "main_buffer_unavailable"; return false; }
    const auto desc = resource->GetDesc(); D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS heapFlags{};
    const auto hr = resource->GetHeapProperties(&heap, &heapFlags);
    std::ostringstream description;
    description << "{\"event\":\"arena_description\",\"resource\":\"" << Hex(reinterpret_cast<uintptr_t>(resource))
        << "\",\"dimension\":" << desc.Dimension << ",\"width\":" << desc.Width << ",\"height\":" << desc.Height
        << ",\"heap_result\":" << static_cast<unsigned long>(hr) << ",\"heap_type\":" << heap.Type
        << ",\"cpu_page_property\":" << heap.CPUPageProperty << ",\"memory_pool\":" << heap.MemoryPoolPreference << '}';
    if (description.str() != g_lastArenaDescription) { g_lastArenaDescription = description.str(); Log(g_lastArenaDescription); }
    const bool cpuVisible = heap.Type == D3D12_HEAP_TYPE_UPLOAD ||
        (heap.Type == D3D12_HEAP_TYPE_CUSTOM && (heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE || heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK));
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width != kBufferBytes || FAILED(hr) || !cpuVisible) {
        g_armFailure = "buffer_description_or_cpu_visibility_mismatch"; resource->Release(); return false;
    }
    const auto gpu = resource->GetGPUVirtualAddress(); void* mapped{};
    D3D12_RANGE range{0, static_cast<SIZE_T>(kBufferBytes)};
    if (!gpu) { g_armFailure = "gpu_address_unavailable"; resource->Release(); return false; }
    const auto mapResult = resource->Map(0, &range, &mapped);
    if (FAILED(mapResult) || !mapped) {
        if (SUCCEEDED(mapResult)) { D3D12_RANGE noWrite{0,0}; resource->Unmap(0,&noWrite); }
        g_armFailure = "map_failed"; resource->Release(); return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_buffer = resource; g_mapped = static_cast<const unsigned char*>(mapped); g_gpuBase = gpu;
        g_seen.clear(); g_viewports.clear(); g_dropped = 0; g_setting = setting; g_upscaler = upscaler; ++g_session;
        g_addressEpochs.fill(0); g_rejectedKinds.clear(); g_sampleEpoch = 0; g_periodReads = 0;
        g_reads = g_repeatedAddresses = g_budgetSkips = g_readFailures = g_unaligned = 0;
        g_rawDiagnostics = g_nearDiagnostics = g_stackSamples = 0; g_armFailure = "none";
        g_bindings.store(0); g_inRange.store(0); g_matches.store(0); g_lockMisses.store(0); g_active.store(true);
    }
    Log("{\"event\":\"armed\",\"session\":" + std::to_string(g_session) + ",\"setting\":\"" + setting + "\",\"gpu_base\":\"" + Hex(gpu) +
        "\",\"buffer_bytes\":" + std::to_string(kBufferBytes) + ",\"duration_seconds\":6,\"writes_buffer\":false,\"read_limit_per_100ms\":" + std::to_string(kReadsPerPeriod) +
        ",\"mhwss_upscaler_config\":\"" + upscaler + "\"}");
    return true;
}
std::wstring Ini(const std::filesystem::path& ini, const std::wstring& key) {
    std::array<wchar_t, 1024> value{}; GetPrivateProfileStringW(L"Capture", key.c_str(), L"", value.data(), static_cast<DWORD>(value.size()), ini.c_str()); return value.data();
}
std::array<unsigned char, 16> Bytes(const std::wstring& text) {
    if (text.size() != 32) throw std::runtime_error("signature length"); std::array<unsigned char, 16> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<unsigned char>(std::stoul(text.substr(i * 2, 2), nullptr, 16)); return out;
}
void Install(const std::filesystem::path& ini) {
    const auto core = GetModuleHandleW(L"D3D12Core.dll"); const auto digest = FileSha256(ModulePath(core));
    if (!core || digest.empty() || std::wstring(digest.begin(), digest.end()) != Ini(ini, L"CoreSha256")) throw std::runtime_error("Core hash mismatch");
    if (FileSha256(ModulePath(g_mhwss)) != kMhwssHash) throw std::runtime_error("MHWSS hash mismatch");
    // Validate the exact instructions referencing the recovered arena pointer.
    auto* mhwss = reinterpret_cast<unsigned char*>(g_mhwss);
    const std::array<unsigned char, 7> read{0x48,0x8b,0x0d,0x33,0xfa,0x45,0x00};
    const std::array<unsigned char, 7> write{0x48,0x89,0x05,0x72,0xbd,0x44,0x00};
    if (memcmp(mhwss + 0xef1be, read.data(), 7) || memcmp(mhwss + 0x102e7f, write.data(), 7)) throw std::runtime_error("Arena reference signatures differ");
    auto* base = reinterpret_cast<unsigned char*>(core);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base); const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    const std::array<const wchar_t*, 4> names{L"SetComputeRootConstantBufferView",L"SetGraphicsRootConstantBufferView",L"RSSetViewports",L"Reset"};
    const std::array<void*, 4> detours{reinterpret_cast<void*>(&OnCompute),reinterpret_cast<void*>(&OnGraphics),reinterpret_cast<void*>(&OnViewport),reinterpret_cast<void*>(&OnReset)};
    for (size_t i = 0; i < names.size(); ++i) {
        const auto rva = std::stoull(Ini(ini, std::wstring(names[i]) + L"Rva"), nullptr, 0); const auto signature = Bytes(Ini(ini, std::wstring(names[i]) + L"Bytes"));
        if (rva >= imageSize || imageSize - rva < 16 || memcmp(base + rva, signature.data(), 16)) throw std::runtime_error("Live API signature mismatch; no unknown hook is overwritten");
        g_hooks[i].target = base + rva;
    }
    if (!Pin(core) || !Pin(g_self) || !Pin(g_mhwss) || MH_Initialize() != MH_OK) throw std::runtime_error("Initialization failed");
    std::array<void*,4> originals{};
    for (size_t i = 0; i < g_hooks.size(); ++i) if (MH_CreateHook(g_hooks[i].target,detours[i],&originals[i]) != MH_OK) throw std::runtime_error("Create hook failed");
    g_compute = reinterpret_cast<CbvFn>(originals[0]); g_graphics = reinterpret_cast<CbvFn>(originals[1]);
    g_viewport = reinterpret_cast<ViewportFn>(originals[2]); g_reset = reinterpret_cast<ResetFn>(originals[3]);
    for (auto& hook : g_hooks) {
        if (MH_EnableHook(hook.target) != MH_OK) throw std::runtime_error("Enable hook failed");
        hook.enabled = true; memcpy(hook.patch.data(), hook.target, 16);
    }
    Log("{\"event\":\"attached\",\"version\":4,\"requires_dlaa\":false,\"changes_gpu_commands\":false,\"changes_ngx_parameters\":false,\"game_base\":\"" +
        Hex(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) + "\",\"mhwss_base\":\"" + Hex(reinterpret_cast<uintptr_t>(g_mhwss)) + "\"}");
}
std::string ReadSetting(const std::filesystem::path& path) {
    std::ifstream file(path); std::string line;
    while (std::getline(file,line)) if (line.rfind("ResolutionScaling=",0)==0) {
        auto value=line.substr(18); while(!value.empty()&&(value.back()=='\r'||value.back()==' ')) value.pop_back();
        if(value=="High"||value=="Low"||value=="Mid"||value=="Medium") return value;
    }
    return {};
}
std::string ReadUpscaler(const std::filesystem::path& path) {
    std::ifstream file(path); std::string line;
    while (std::getline(file, line)) {
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 8, "Upscaler") != 0) continue;
        const auto equal = line.find('=', start + 8);
        if (equal == std::string::npos) continue;
        const auto quote = line.find('"', equal + 1);
        if (quote == std::string::npos) continue;
        const auto end = line.find('"', quote + 1);
        if (end == std::string::npos || end - quote > 64) continue;
        const auto value = line.substr(quote + 1, end - quote - 1);
        if (value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _-") == std::string::npos) return value;
    }
    return {};
}
void Stop() noexcept {
    try {
        UnmapBuffer(); Drain(); bool restored=true;
        for(auto& hook:g_hooks) if(hook.enabled) {
            if(memcmp(hook.target,hook.patch.data(),16)||MH_DisableHook(hook.target)!=MH_OK) restored=false;
            else hook.enabled=false;
        }
        Log(std::string("{\"event\":\"stopped\",\"owned_hooks_restored\":")+(restored?"true":"false")+"}");
    } catch (...) {}
}
DWORD WINAPI Worker(void*) {
    HANDLE events[2]{};
    try {
        const auto executable=ModulePath(nullptr); if(_wcsicmp(executable.filename().c_str(),L"MonsterHunterWorld.exe")) return 0;
        const auto module=ModulePath(g_self), folder=module.parent_path(), ini=folder/(module.stem().wstring()+L".ini");
        if(GetPrivateProfileIntW(L"Capture",L"Enabled",0,ini.c_str())!=1) return 0;
        const bool stopAfterCapture=GetPrivateProfileIntW(L"Capture",L"StopAfterCapture",0,ini.c_str())!=0;
        g_log.open(folder/(L"MhwScreenProbe-"+std::to_wstring(GetCurrentProcessId())+L".jsonl"),std::ios::app);
        if(FileSha256(executable)!=kGameHash) throw std::runtime_error("Game build mismatch");
        for(unsigned i=0;i<3600;++i) {
            g_mhwss=GetModuleHandleW(L"MHWSS.dll"); if(g_mhwss&&GetModuleHandleW(L"D3D12Core.dll")) break; Sleep(50);
        }
        if(!g_mhwss||!GetModuleHandleW(L"D3D12Core.dll")) throw std::runtime_error("Graphics modules unavailable");
        Install(ini);
        const auto prefix=L"Local\\MhwScreenProbe."+std::to_wstring(GetCurrentProcessId());
        events[0]=CreateEventW(nullptr,TRUE,FALSE,(prefix+L".Shutdown").c_str());
        events[1]=CreateEventW(nullptr,FALSE,FALSE,(prefix+L".Capture").c_str());
        if(!events[0]||!events[1]) throw std::runtime_error("Control events unavailable");
        const auto graphics=executable.parent_path()/L"graphics_option.ini";
        const auto upscaleConfig=executable.parent_path()/L"MHWSS"/L"MHWSS_config.toml";
        std::string setting,upscaler; bool pending=true; uint64_t changed=GetTickCount64(),deadline=0,nextStatus=changed+10000;
        while(true) {
            const auto wait=WaitForMultipleObjects(2,events,FALSE,250); if(wait==WAIT_OBJECT_0) break;
            Drain(); const auto now=GetTickCount64(); const auto current=ReadSetting(graphics);
            if(!current.empty()&&current!=setting) {
                if(g_active.load()) UnmapBuffer(); setting=current; changed=now; pending=true;
                Log("{\"event\":\"setting_marker\",\"setting\":\""+setting+"\",\"tick_ms\":"+std::to_string(now)+"}");
            }
            const auto currentUpscaler=ReadUpscaler(upscaleConfig);
            if(!currentUpscaler.empty()&&currentUpscaler!=upscaler) {
                if(g_active.load()) UnmapBuffer(); upscaler=currentUpscaler; changed=now; pending=true;
                Log("{\"event\":\"upscaler_config_marker\",\"value\":\""+upscaler+"\",\"tick_ms\":"+std::to_string(now)+"}");
            }
            if(wait==WAIT_OBJECT_0+1) { pending=true; changed=now; }
            if(g_active.load()&&now>=deadline) {
                UnmapBuffer(); Drain();
                Log("{\"event\":\"capture_complete\",\"session\":"+std::to_string(g_session)+",\"bindings\":"+std::to_string(g_bindings.load())+
                    ",\"addresses_in_arena\":"+std::to_string(g_inRange.load())+",\"matching_blocks\":"+std::to_string(g_matches.load())+
                    ",\"unique_blocks\":"+std::to_string(g_seen.size())+",\"dropped\":"+std::to_string(g_dropped)+
                    ",\"sampled_blocks\":"+std::to_string(g_reads)+",\"repeat_address_skips\":"+std::to_string(g_repeatedAddresses)+
                    ",\"read_budget_skips\":"+std::to_string(g_budgetSkips)+",\"read_failures\":"+std::to_string(g_readFailures)+
                    ",\"unaligned_addresses\":"+std::to_string(g_unaligned)+",\"lock_contention_skips\":"+std::to_string(g_lockMisses.load())+
                    ",\"rejected_diagnostics\":"+std::to_string(g_rawDiagnostics+g_nearDiagnostics)+",\"stack_samples\":"+std::to_string(g_stackSamples)+"}");
                if(stopAfterCapture) break;
            }
            if(pending&&!g_active.load()&&!setting.empty()&&now-changed>=3000&&g_session<12) {
                if(Arm(setting,upscaler)) { pending=false; deadline=now+6000; }
            }
            if(now>=nextStatus) {
                Log("{\"event\":\"status\",\"waiting_for_arena\":"+std::string(pending&&!g_active.load()?"true":"false")+
                    ",\"active\":"+(g_active.load()?"true":"false")+",\"sessions\":"+std::to_string(g_session)+
                    ",\"arm_state\":\""+g_armFailure+"\"}"); nextStatus=now+10000;
            }
        }
    } catch(const std::exception& error) { Log(std::string("{\"event\":\"refused\",\"reason\":\"")+error.what()+"\"}"); }
    Stop(); for(auto event:events) if(event) CloseHandle(event); return 0;
}
void Start() {
    if(InterlockedCompareExchange(&g_started,1,0)) return;
    const auto thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr); if(thread) CloseHandle(thread); else InterlockedExchange(&g_started,0);
}
}
extern "C" __declspec(dllexport) void Initialize() { Start(); }
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) { if(reason==DLL_PROCESS_ATTACH) {g_self=module;Start();} return TRUE; }
