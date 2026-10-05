// Bounded, one-frame D3D12 texture readback at the existing MHWSS NGX dispatch.
// Does not modify NGX parameters, render dimensions, or input pixels.
#include "CaptureCommon.h"
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace {
constexpr char kMhwssHash[] = "55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a";
constexpr size_t kEvaluateSlotRva = 0x559b00;
constexpr std::array<unsigned char, 7> kEvaluateLoad{0x48, 0x8b, 0x0d, 0x82, 0x80, 0x25, 0x00};
using EvaluateFn = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
std::atomic<EvaluateFn> g_evaluate{};
std::atomic<ResetFn> g_reset{};
std::atomic<BarrierFn> g_barrier{};
std::atomic<ExecuteFn> g_execute{};
std::atomic<bool> g_tracking{false}, g_claim{false}, g_accept{false};
std::atomic<unsigned> g_evaluators{0};
std::atomic<uint64_t> g_evaluationCount{0}, g_request{0}, g_captured{0};
HMODULE g_self = nullptr;
std::filesystem::path g_folder;
std::ofstream g_log;
std::mutex g_logMutex, g_stateMutex, g_jobMutex, g_controlMutex;
void* volatile* g_evaluateSlot = nullptr;
std::array<void*, 3> g_targets{};
std::array<std::array<unsigned char, 16>, 3> g_ourPatches{};
bool g_hooksEnabled = false;
LUID g_expectedLuid{};
std::string g_setting;
uint64_t g_requestTick = 0;

void Log(const std::string& line) noexcept {
    try { std::lock_guard<std::mutex> lock(g_logMutex); if (g_log) { g_log << line << '\n'; g_log.flush(); } } catch (...) {}
}
void Event(const char* name, const char* reason) noexcept {
    try { Log(std::string("{\"event\":\"") + name + "\",\"reason\":\"" + reason + "\",\"tick_ms\":" + std::to_string(GetTickCount64()) + "}"); } catch (...) {}
}

struct State { D3D12_RESOURCE_STATES value{}; bool known = false; };
struct ListState {
    bool seenReset = false, overflow = false;
    std::unordered_map<ID3D12Resource*, State> resources;
};
std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_states;

enum Phase { Recording, Recorded, Submitting, Submitted, Saved, FailedRetained };
struct Texture {
    const char* name = nullptr;
    ComPtr<ID3D12Resource> source, readback;
    D3D12_RESOURCE_DESC desc{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows = 0;
    UINT64 rowBytes = 0, size = 0;
    D3D12_RESOURCE_STATES observedState{};
    bool copied = false;
};
struct Job {
    uint64_t request = 0, tick = 0, call = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    std::string setting, parameters;
    std::array<Texture, 4> textures{};
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    std::atomic<Phase> phase{Recording};
    unsigned result = 0;
};
// The active job is deliberately retained on any uncertain submission/fence failure.
// GPU-referenced resources must never be released just because a CPU timeout elapsed.
std::shared_ptr<Job> g_job;

HRESULT STDMETHODCALLTYPE OnReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline) {
    const auto result = g_reset.load()(list, allocator, pipeline);
    if (SUCCEEDED(result) && g_tracking.load(std::memory_order_relaxed)) {
        try {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (g_states.size() < 512 || g_states.count(list)) {
                auto& state = g_states[list]; state.resources.clear(); state.seenReset = true; state.overflow = false;
            }
        } catch (...) { Event("tracking_error", "reset_state_allocation_failed"); }
    }
    return result;
}

void STDMETHODCALLTYPE OnBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (g_tracking.load(std::memory_order_relaxed) && barriers && count <= 16384) {
        try {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto found = g_states.find(list);
            if (found != g_states.end() && found->second.seenReset && !found->second.overflow) {
                auto& state = found->second;
                for (UINT i = 0; i < count; ++i) {
                    const auto& barrier = barriers[i];
                    if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                        const auto& transition = barrier.Transition;
                        if (!transition.pResource || (transition.Subresource != 0 && transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) continue;
                        if (state.resources.size() >= 4096 && !state.resources.count(transition.pResource)) { state.overflow = true; break; }
                        auto& resourceState = state.resources[transition.pResource];
                        resourceState.value = transition.StateAfter;
                        resourceState.known = barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE || barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
                    } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                        if (!barrier.Aliasing.pResourceBefore || !barrier.Aliasing.pResourceAfter) state.resources.clear();
                        else { state.resources.erase(barrier.Aliasing.pResourceBefore); state.resources.erase(barrier.Aliasing.pResourceAfter); }
                    }
                }
            }
        } catch (...) {
            // A partial tracking failure disarms this request; never use a guessed StateBefore.
            g_tracking.store(false); Event("tracking_error", "barrier_tracking_failed_request_disarmed");
        }
    }
    g_barrier.load()(list, count, barriers);
}

bool KnownState(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES* value) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    const auto found = g_states.find(list);
    if (found == g_states.end() || !found->second.seenReset || found->second.overflow) return false;
    const auto state = found->second.resources.find(resource);
    if (state == found->second.resources.end() || !state->second.known) return false;
    *value = state->second.value; return true;
}

void STDMETHODCALLTYPE OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    std::shared_ptr<Job> job;
    try {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        if (g_job && g_job->phase.load() == Recorded && lists && count <= 4096) {
            for (UINT i = 0; i < count; ++i) {
                if (lists[i] != static_cast<ID3D12CommandList*>(g_job->list)) continue;
                Phase expected = Recorded;
                if (g_job->phase.compare_exchange_strong(expected, Submitting)) job = g_job;
                break;
            }
        }
    } catch (...) { Event("queue_observation_error", "job_lookup_failed"); }
    // All application arguments, ordering and the original submission are preserved.
    g_execute.load()(queue, count, lists);
    if (job) {
        ComPtr<ID3D12Device> device;
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != job->device.Get()) {
            job->phase.store(FailedRetained); Event("capture_retained", "queue_device_or_type_mismatch"); return;
        }
        job->queue = queue;
        if (FAILED(queue->Signal(job->fence.Get(), 1))) {
            job->phase.store(FailedRetained); Event("capture_retained", "queue_fence_signal_failed"); return;
        }
        job->phase.store(Submitted, std::memory_order_release);
        Event("submitted", "copy_fence_signaled_after_original_queue_submission");
    }
}

bool GetTexture(NVSDK_NGX_Parameter* params, const char* name, ID3D12Resource** resource, D3D12_RESOURCE_DESC* desc) noexcept {
    __try {
        if (!params || NVSDK_NGX_FAILED(params->Get(name, resource)) || !*resource) return false;
        *desc = (*resource)->GetDesc(); return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GetUInt(NVSDK_NGX_Parameter* params, const char* name, unsigned* value) noexcept {
    __try { return NVSDK_NGX_SUCCEED(params->Get(name, value)); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GetFloat(NVSDK_NGX_Parameter* params, const char* name, float* value) noexcept {
    __try { return NVSDK_NGX_SUCCEED(params->Get(name, value)); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Parameters(NVSDK_NGX_Parameter* params) {
    std::ostringstream out; out << '{'; bool first = true;
    for (const char* name : {"Width", "Height", "OutWidth", "OutHeight", "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height"}) {
        unsigned value = 0; if (!first) out << ','; first = false;
        out << '"' << name << "\":"; if (GetUInt(params, name, &value)) out << value; else out << "null";
    }
    for (const char* name : {"Jitter.Offset.X", "Jitter.Offset.Y", "MV.Scale.X", "MV.Scale.Y"}) {
        float value = 0; out << ",\"" << name << "\":";
        if (GetFloat(params, name, &value) && std::isfinite(value)) out << std::setprecision(9) << value; else out << "null";
    }
    out << '}'; return out.str();
}

bool SupportedTexture(const D3D12_RESOURCE_DESC& desc, unsigned index) {
    const DXGI_FORMAT format = index == 1 ? DXGI_FORMAT_R32_FLOAT : index == 2 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R11G11B10_FLOAT;
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == format &&
        desc.Width >= 32 && desc.Width <= 8192 && desc.Height >= 32 && desc.Height <= 8192 &&
        desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
        !(desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS));
}

bool PrepareTexture(ID3D12Device* device, Texture& texture) {
    device->GetCopyableFootprints(&texture.desc, 0, 1, 0, &texture.layout, &texture.rows, &texture.rowBytes, &texture.size);
    if (!texture.size || texture.size == UINT64_MAX || texture.size > 256ULL * 1024 * 1024) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = texture.size; buffer.Height = 1;
    buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texture.readback)));
}

void CopyTexture(ID3D12GraphicsCommandList* list, Texture& texture, D3D12_RESOURCE_STATES previous) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.source.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = previous; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    // Bypass only our observer; all other layers on the actual list's copy method remain in place.
    g_barrier.load()(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
    destination.pResource = texture.readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = texture.layout;
    source.pResource = texture.source.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = previous;
    g_barrier.load()(list, 1, &barrier);
    texture.observedState = previous; texture.copied = true;
}

std::shared_ptr<Job> BeginCapture(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params, uint64_t call) {
    if (!g_accept.load() || !g_tracking.load() || g_request.load() == g_captured.load() || g_claim.exchange(true)) return {};
    struct ReleaseClaim { ~ReleaseClaim() { g_claim.store(false); } } release;
    std::lock_guard<std::mutex> control(g_controlMutex);
    if (!g_accept.load() || !g_tracking.load()) return {};
    {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        if (g_job && g_job->phase.load() != Saved) return {};
    }
    const auto vtable = *reinterpret_cast<void***>(list);
    if (vtable[10] != g_targets[0] || vtable[26] != g_targets[1] || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return {};
    auto job = std::make_shared<Job>();
    job->request = g_request.load(); job->list = list; job->call = call; job->tick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        job->setting = g_setting;
        if (job->tick - g_requestTick < 3000) return {};
    }
    constexpr std::array<const char*, 4> names{"Color", "Depth", "MotionVectors", "Output"};
    bool ready = true;
    std::ostringstream status; status << "{\"event\":\"awaiting_states\",\"call\":" << call << ",\"states\":[";
    for (unsigned i = 0; i < names.size(); ++i) {
        auto& texture = job->textures[i]; texture.name = names[i];
        ID3D12Resource* resource = nullptr;
        if (i) status << ',';
        if (!GetTexture(params, names[i], &resource, &texture.desc) || !SupportedTexture(texture.desc, i)) { ready = false; status << "\"unsupported_resource\""; continue; }
        texture.source = resource;
        if (!KnownState(list, resource, &texture.observedState)) { ready = false; status << "null"; continue; }
        status << unsigned(texture.observedState);
        const auto state = texture.observedState;
        if (i == 3 ? state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
            (state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
             state != (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))) ready = false;
    }
    status << "]}";
    if (!ready) { if (call % 127 == 0) Log(status.str()); return {}; }
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&job->device)))) return {};
    const auto luid = job->device->GetAdapterLuid();
    if (luid.LowPart != g_expectedLuid.LowPart || luid.HighPart != g_expectedLuid.HighPart) { Event("capture_skipped", "adapter_luid_mismatch"); return {}; }
    if (FAILED(job->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&job->fence)))) return {};
    for (auto& texture : job->textures) if (!PrepareTexture(job->device.Get(), texture)) { Event("capture_skipped", "readback_allocation_failed"); return {}; }
    job->parameters = Parameters(params);
    // Publish ownership before recording any GPU command. It persists even if no queue submission is observed.
    {
        std::lock_guard<std::mutex> lock(g_jobMutex); g_job = job;
    }
    g_captured.store(job->request);
    for (unsigned i = 0; i < 3; ++i) CopyTexture(list, job->textures[i], job->textures[i].observedState);
    return job;
}

NVSDK_NGX_Result __cdecl OnEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params, void* callback) {
    g_evaluators.fetch_add(1);
    const auto call = g_evaluationCount.fetch_add(1) + 1;
    std::shared_ptr<Job> job;
    try { job = BeginCapture(list, params, call); } catch (...) { Event("capture_skipped", "preparation_failed"); }
    const auto result = g_evaluate.load()(list, handle, params, callback);
    if (job) {
        job->result = unsigned(result);
        try {
            D3D12_RESOURCE_STATES state{};
            if (g_tracking.load() && NVSDK_NGX_SUCCEED(result) && KnownState(list, job->textures[3].source.Get(), &state) && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                CopyTexture(list, job->textures[3], state);
            else Event("output_copy_skipped", "post_evaluate_state_or_result_not_confirmed");
        } catch (...) { Event("output_copy_skipped", "post_evaluate_inspection_failed"); }
        job->phase.store(Recorded, std::memory_order_release);
        g_tracking.store(false);
        Event("recorded", "input_states_restored_and_original_evaluate_forwarded");
    }
    g_evaluators.fetch_sub(1);
    return result;
}

bool SaveJob(Job& job) {
    const auto prefix = "capture-" + std::to_string(job.request) + "-" + job.setting + "-" + std::to_string(job.tick);
    const auto directory = g_folder / ("textures-" + std::to_string(GetCurrentProcessId())) / prefix;
    std::filesystem::create_directories(directory);
    std::ostringstream metadata;
    metadata << "{\"version\":1,\"pid\":" << GetCurrentProcessId() << ",\"tick_ms\":" << job.tick
        << ",\"evaluate_call\":" << job.call << ",\"evaluate_result\":" << job.result << ",\"ResolutionScaling\":\"" << job.setting
        << "\",\"gpu_fence_completed\":true,\"command_list\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(job.list) << std::dec
        << "\",\"parameters\":" << job.parameters << ",\"textures\":[";
    bool first = true;
    for (auto& texture : job.textures) {
        if (!texture.copied) continue;
        const std::string filename = std::string(texture.name) + ".bin";
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, static_cast<SIZE_T>(texture.size)};
        if (FAILED(texture.readback->Map(0, &readRange, &mapped)) || !mapped) return false;
        std::ofstream output(directory / filename, std::ios::binary | std::ios::trunc);
        if (output) output.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(texture.size));
        output.close();
        D3D12_RANGE noWrite{0, 0}; texture.readback->Unmap(0, &noWrite);
        if (!output) return false;
        if (!first) metadata << ','; first = false;
        metadata << "{\"name\":\"" << texture.name << "\",\"file\":\"" << filename << "\",\"identity\":\"0x" << std::hex
            << reinterpret_cast<uintptr_t>(texture.source.Get()) << std::dec << "\",\"width\":" << texture.desc.Width
            << ",\"height\":" << texture.desc.Height << ",\"format\":" << unsigned(texture.desc.Format)
            << ",\"flags\":" << unsigned(texture.desc.Flags) << ",\"offset\":" << texture.layout.Offset
            << ",\"row_pitch\":" << texture.layout.Footprint.RowPitch << ",\"rows\":" << texture.rows
            << ",\"row_bytes\":" << texture.rowBytes << ",\"total_bytes\":" << texture.size
            << ",\"observed_and_restored_state\":" << unsigned(texture.observedState) << '}';
    }
    metadata << "]}\n";
    std::ofstream report(directory / "metadata.json"); report << metadata.str(); report.close();
    if (!report) return false;
    Log("{\"event\":\"saved\",\"directory\":\"" + prefix + "\",\"request\":" + std::to_string(job.request) + ",\"gpu_fence_completed\":true}");
    // No other thread may consume these resources after phase Submitted except this worker.
    for (auto& texture : job.textures) { texture.readback.Reset(); texture.source.Reset(); }
    job.queue.Reset(); job.fence.Reset(); job.device.Reset();
    job.phase.store(Saved, std::memory_order_release);
    return true;
}

void PollJob() noexcept {
    try {
        std::shared_ptr<Job> job;
        { std::lock_guard<std::mutex> lock(g_jobMutex); job = g_job; }
        if (!job) return;
        if (job->phase.load(std::memory_order_acquire) != Submitted) return;
        const auto completed = job->fence->GetCompletedValue();
        if (completed == UINT64_MAX) { job->phase.store(FailedRetained); Event("capture_retained", "device_removed"); return; }
        if (completed >= 1 && !SaveJob(*job)) { job->phase.store(FailedRetained); Event("capture_retained", "file_save_failed"); }
    } catch (...) { Event("save_error", "worker_exception_resources_retained"); }
}

std::wstring Ini(const std::filesystem::path& ini, const std::wstring& key) {
    std::array<wchar_t, 1024> value{};
    GetPrivateProfileStringW(L"Capture", key.c_str(), L"", value.data(), static_cast<DWORD>(value.size()), ini.c_str());
    return value.data();
}
std::array<unsigned char, 16> ParseBytes(const std::wstring& value) {
    if (value.size() != 32) throw std::runtime_error("signature_length");
    std::array<unsigned char, 16> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        size_t used = 0; const auto byte = std::stoul(value.substr(i * 2, 2), &used, 16);
        if (used != 2 || byte > 255) throw std::runtime_error("signature_parse");
        bytes[i] = static_cast<unsigned char>(byte);
    }
    return bytes;
}
bool Pin(HMODULE module) {
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned) != FALSE;
}

bool ResolveExistingJump(void* address, void** target) noexcept {
    __try {
        const auto* entry = static_cast<const unsigned char*>(address);
        if (entry[0] != 0xe9) return false;
        const auto* relay = entry + 5 + *reinterpret_cast<const int32_t*>(entry + 1);
        if (relay[0] != 0xff || relay[1] != 0x25) return false;
        const auto* slot = relay + 6 + *reinterpret_cast<const int32_t*>(relay + 2);
        *target = *reinterpret_cast<void* const*>(slot);
        return *target != nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool AcceptExistingQueueHook(void* address, const std::array<unsigned char, 16>& originalBytes, const std::filesystem::path& ini) {
    // This machine's RTSS replaces exactly the first five bytes with a relay jump.
    // Validate its owner, on-disk hash, target RVA and live bytes before chaining it.
    const auto* entry = static_cast<const unsigned char*>(address);
    if (memcmp(entry + 5, originalBytes.data() + 5, originalBytes.size() - 5) != 0) return false;
    void* target = nullptr;
    if (!ResolveExistingJump(address, &target)) return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(target), &owner) || owner != GetModuleHandleW(L"RTSSHooks64.dll")) return false;
    const auto digest = FileSha256(ModulePath(owner));
    if (digest.empty() || std::wstring(digest.begin(), digest.end()) != Ini(ini, L"ExistingQueueHookOwnerSha256")) return false;
    const auto rva = std::stoull(Ini(ini, L"ExistingQueueHookRva"), nullptr, 0);
    if (reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(owner) != rva) return false;
    const auto bytes = ParseBytes(Ini(ini, L"ExistingQueueHookBytes"));
    if (memcmp(target, bytes.data(), bytes.size()) != 0 || !Pin(owner)) return false;
    Event("existing_queue_hook", "validated_RTSS_hook_will_be_chained_and_restored");
    return true;
}

bool Install() {
    const auto ini = g_folder / L"MhwTextureProbe.ini";
    if (GetPrivateProfileIntW(L"Capture", L"Pid", 0, ini.c_str()) != GetCurrentProcessId()) { Event("refused", "pid_mismatch"); return false; }
    const auto core = GetModuleHandleW(L"D3D12Core.dll"), mhwss = GetModuleHandleW(L"MHWSS.dll");
    const auto coreHash = core ? FileSha256(ModulePath(core)) : std::string();
    if (!core || coreHash.empty() || std::wstring(coreHash.begin(), coreHash.end()) != Ini(ini, L"CoreSha256") ||
        !mhwss || FileSha256(ModulePath(mhwss)) != kMhwssHash) { Event("refused", "module_hash_mismatch"); return false; }
    g_expectedLuid.LowPart = std::stoul(Ini(ini, L"AdapterLuidLow"));
    g_expectedLuid.HighPart = std::stol(Ini(ini, L"AdapterLuidHigh"));
    auto* base = reinterpret_cast<unsigned char*>(core);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    const std::array<const wchar_t*, 3> names{L"Reset", L"ResourceBarrier", L"ExecuteCommandLists"};
    const std::array<void*, 3> replacements{reinterpret_cast<void*>(&OnReset), reinterpret_cast<void*>(&OnBarrier), reinterpret_cast<void*>(&OnExecute)};
    for (size_t i = 0; i < names.size(); ++i) {
        const auto rva = std::stoull(Ini(ini, std::wstring(names[i]) + L"Rva"), nullptr, 0);
        const auto bytes = ParseBytes(Ini(ini, std::wstring(names[i]) + L"Bytes"));
        if (rva >= size || size - rva < bytes.size()) { Event("refused", "api_rva_out_of_range"); return false; }
        if (memcmp(base + rva, bytes.data(), bytes.size()) != 0 &&
            !(i == 2 && AcceptExistingQueueHook(base + rva, bytes, ini))) { Event("refused", "live_api_signature_mismatch"); return false; }
        g_targets[i] = base + rva;
    }
    auto* mhwssBase = reinterpret_cast<unsigned char*>(mhwss);
    if (memcmp(mhwssBase + 0x301a77, kEvaluateLoad.data(), kEvaluateLoad.size()) != 0) { Event("refused", "ngx_stub_mismatch"); return false; }
    g_evaluateSlot = reinterpret_cast<void* volatile*>(mhwssBase + kEvaluateSlotRva);
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(mhwssBase + kEvaluateSlotRva, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) return false;
    void* original = InterlockedCompareExchangePointer(g_evaluateSlot, nullptr, nullptr);
    HMODULE owner = nullptr;
    if (!original || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(original), &owner)) return false;
    const auto filename = ModulePath(owner).filename().wstring();
    if (_wcsicmp(filename.c_str(), L"MhwSrProbe.dll") != 0 && _wcsicmp(filename.c_str(), L"nvngx_dlss.dll") != 0) {
        Event("refused", "unexpected_evaluate_dispatch_owner"); return false;
    }
    if (!Pin(g_self) || !Pin(core) || !Pin(mhwss) || !Pin(owner)) return false;
    if (MH_Initialize() != MH_OK) return false;
    std::array<void*, 3> originals{};
    for (size_t i = 0; i < names.size(); ++i) {
        if (MH_CreateHook(g_targets[i], replacements[i], &originals[i]) != MH_OK) { MH_Uninitialize(); Event("refused", "hook_creation_failed"); return false; }
    }
    g_reset.store(reinterpret_cast<ResetFn>(originals[0])); g_barrier.store(reinterpret_cast<BarrierFn>(originals[1]));
    g_execute.store(reinterpret_cast<ExecuteFn>(originals[2])); g_evaluate.store(reinterpret_cast<EvaluateFn>(original));
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { MH_DisableHook(MH_ALL_HOOKS); Event("refused", "api_enable_failed"); return false; }
    g_hooksEnabled = true;
    for (size_t i = 0; i < names.size(); ++i) memcpy(g_ourPatches[i].data(), g_targets[i], g_ourPatches[i].size());
    if (InterlockedCompareExchangePointer(g_evaluateSlot, reinterpret_cast<void*>(&OnEvaluate), original) != original) { Event("refused", "evaluate_slot_changed"); return false; }
    g_accept.store(true);
    Event("attached", "existing_ngx_dispatch_chained_without_parameter_changes"); return true;
}

std::string ReadSetting(const std::filesystem::path& graphics) {
    std::ifstream input(graphics, std::ios::binary); std::string line;
    while (std::getline(input, line)) {
        constexpr char prefix[] = "ResolutionScaling=";
        if (line.compare(0, sizeof(prefix) - 1, prefix) != 0) continue;
        auto value = line.substr(sizeof(prefix) - 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (value == "High" || value == "Low") return value;
        if (value == "Mid" || value == "Medium") return "Medium";
    }
    return {};
}
bool Arm(const std::string& setting) {
    std::lock_guard<std::mutex> control(g_controlMutex);
    if (g_request.load() >= 12 || setting.empty()) return false;
    { std::lock_guard<std::mutex> lock(g_jobMutex); if (g_job && g_job->phase.load() != Saved) return false; }
    g_tracking.store(false);
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_states.clear(); g_setting = setting; g_requestTick = GetTickCount64(); g_request.fetch_add(1);
    }
    g_tracking.store(true);
    Log("{\"event\":\"armed\",\"request\":" + std::to_string(g_request.load()) + ",\"ResolutionScaling\":\"" + setting +
        "\",\"tick_ms\":" + std::to_string(GetTickCount64()) + ",\"expires\":false}");
    return true;
}

void Stop() noexcept {
    g_accept.store(false); g_tracking.store(false);
    bool slotRestored = true;
    if (g_evaluateSlot && g_evaluate.load()) {
        const auto original = reinterpret_cast<void*>(g_evaluate.load());
        const auto actual = InterlockedCompareExchangePointer(g_evaluateSlot, original, reinterpret_cast<void*>(&OnEvaluate));
        slotRestored = actual == original || actual == reinterpret_cast<void*>(&OnEvaluate);
    }
    // Wait only on the observer worker; never wait for GPU completion on the rendering thread.
    const auto deadline = GetTickCount64() + 3000;
    while (g_evaluators.load() && GetTickCount64() < deadline) Sleep(10);
    PollJob();
    bool restored = !g_hooksEnabled;
    if (g_hooksEnabled && !g_evaluators.load()) {
        bool owned = true;
        for (size_t i = 0; i < g_targets.size(); ++i)
            if (memcmp(g_targets[i], g_ourPatches[i].data(), g_ourPatches[i].size()) != 0) owned = false;
        if (owned && MH_DisableHook(MH_ALL_HOOKS) == MH_OK) { restored = true; g_hooksEnabled = false; }
    }
    try { Log(std::string("{\"event\":\"stopped\",\"api_hooks_restored\":") + (restored ? "true" : "false") +
        ",\"evaluate_slot_restored\":" + (slotRestored ? "true" : "false") + ",\"module_pinned_until_process_exit\":true}"); } catch (...) {}
    // Never uninitialize hook trampolines, unload the DLL, or free uncertain GPU work.
}

DWORD WINAPI Worker(void*) {
    HANDLE controls[2]{};
    try {
        if (_wcsicmp(ModulePath(nullptr).filename().c_str(), L"MonsterHunterWorld.exe") != 0) return 0;
        g_folder = ModulePath(g_self).parent_path();
        g_log.open(g_folder / ("MhwTextureProbe-" + std::to_string(GetCurrentProcessId()) + ".jsonl"), std::ios::app);
        Event("start", "one_frame_texture_readback_no_ngx_parameter_changes");
        if (!Install()) { Stop(); return 0; }
        const auto prefix = L"Local\\MhwTextureProbe." + std::to_wstring(GetCurrentProcessId());
        controls[0] = CreateEventW(nullptr, TRUE, FALSE, (prefix + L".Shutdown").c_str());
        controls[1] = CreateEventW(nullptr, FALSE, FALSE, (prefix + L".Capture").c_str());
        if (!controls[0] || !controls[1]) throw std::runtime_error("events");
        const auto graphics = ModulePath(nullptr).parent_path() / L"graphics_option.ini";
        std::string observed; bool pending = true;
        uint64_t changed = GetTickCount64(), nextStatus = changed + 10000;
        while (true) {
            const auto wait = WaitForMultipleObjects(2, controls, FALSE, 250);
            if (wait == WAIT_OBJECT_0) break;
            PollJob();
            const auto now = GetTickCount64();
            const auto setting = ReadSetting(graphics);
            if (!setting.empty() && setting != observed) {
                observed = setting; changed = now; pending = true;
                // Disarm during setting transitions so stale geometry is not attributed to the new marker.
                g_tracking.store(false);
                Log("{\"event\":\"setting_marker\",\"ResolutionScaling\":\"" + setting + "\",\"tick_ms\":" + std::to_string(now) + "}");
            }
            if (wait == WAIT_OBJECT_0 + 1) { pending = true; changed = now; }
            if (pending && now - changed >= 1500 && Arm(observed)) pending = false;
            if (now >= nextStatus) {
                Log("{\"event\":\"status\",\"evaluate_calls\":" + std::to_string(g_evaluationCount.load()) +
                    ",\"tracking\":" + (g_tracking.load() ? "true" : "false") + ",\"request\":" + std::to_string(g_request.load()) + "}");
                nextStatus = now + 10000;
            }
        }
    } catch (...) { Event("controller_error", "exception"); }
    Stop();
    for (auto control : controls) if (control) CloseHandle(control);
    Event("controller_exit", "finished");
    return 0;
}
} // namespace
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
