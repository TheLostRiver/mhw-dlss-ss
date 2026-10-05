// Diagnostic only: observes MHWSS's existing NGX calls; never changes render parameters.
// Only the locally inspected MHWSS 1.0.2 SHA-256 is supported.
#include <Windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr char kExpectedHash[] = "55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a";
// RIP-relative dispatch slots recovered from the pinned MHWSS NGX SDK stubs.
constexpr size_t kCreateSlotRva = 0x559af8;
constexpr size_t kEvaluateSlotRva = 0x559b00;
constexpr size_t kCreateLoadRva = 0x30195e;
constexpr size_t kEvaluateLoadRva = 0x301a77;
constexpr std::array<unsigned char, 7> kCreateLoad{0x48, 0x8b, 0x1d, 0x93, 0x81, 0x25, 0x00};
constexpr std::array<unsigned char, 7> kEvaluateLoad{0x48, 0x8b, 0x0d, 0x82, 0x80, 0x25, 0x00};

using CreateFunction = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFunction = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    NVSDK_NGX_Parameter*, void*);
std::atomic<CreateFunction> g_create{nullptr};
std::atomic<EvaluateFunction> g_evaluate{nullptr};
std::atomic<unsigned long long> g_evaluations{0};
std::atomic<unsigned> g_samples{0};
using EncodedMotionFunction = void(__cdecl*)(void*, ID3D12GraphicsCommandList*, ID3D12Resource*, uintptr_t, uint64_t, uint64_t);
using ColorStageFunction = void(__cdecl*)(void*, ID3D12GraphicsCommandList*, ID3D12Resource*, unsigned);
using UpscaleEntryFunction = void(__cdecl*)(void*, ID3D12GraphicsCommandList*, ID3D12Resource*, unsigned, uint64_t);
std::atomic<EncodedMotionFunction> g_encodedMotion{nullptr};
std::atomic<ColorStageFunction> g_colorStage{nullptr};
std::atomic<UpscaleEntryFunction> g_upscaleEntry{nullptr};
std::array<std::atomic<unsigned long long>, 3> g_stageCalls{};
std::mutex g_logMutex;
std::ofstream g_log;
HMODULE g_self = nullptr;
volatile LONG g_started = 0;

void Log(const std::string& line) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_log) { g_log << line << '\n'; g_log.flush(); }
    } catch (...) {}
}

std::string HashFile(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string output;
    std::ifstream input(path, std::ios::binary);
    if (!input || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    DWORD objectSize = 0, written = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
        sizeof(objectSize), &written, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0); return {};
    }
    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0) {
        std::array<char, 65536> buffer{};
        bool valid = true;
        while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(input.gcount()), 0) < 0) { valid = false; break; }
        }
        valid = valid && input.eof();
        std::array<unsigned char, 32> digest{};
        if (valid && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
            std::ostringstream value;
            for (auto byte : digest) value << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
            output = value.str();
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return output;
}

// These leaf functions contain no C++ objects that require stack unwinding.
bool GetUnsigned(NVSDK_NGX_Parameter* params, const char* name, unsigned* value) noexcept {
    __try { return params && NVSDK_NGX_SUCCEED(params->Get(name, value)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GetInteger(NVSDK_NGX_Parameter* params, const char* name, int* value) noexcept {
    __try { return params && NVSDK_NGX_SUCCEED(params->Get(name, value)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GetFloat(NVSDK_NGX_Parameter* params, const char* name, float* value) noexcept {
    __try { return params && NVSDK_NGX_SUCCEED(params->Get(name, value)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GetTexture(NVSDK_NGX_Parameter* params, const char* name,
    ID3D12Resource** resource, D3D12_RESOURCE_DESC* description) noexcept {
    __try {
        if (!params || NVSDK_NGX_FAILED(params->Get(name, resource)) || !*resource) return false;
        *description = (*resource)->GetDesc();
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool DescribeResource(ID3D12Resource* resource, D3D12_RESOURCE_DESC* description) noexcept {
    __try {
        if (!resource) return false;
        *description = resource->GetDesc();
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadStageMetadata(void* context, ID3D12GraphicsCommandList* list,
    unsigned* widthCandidate, unsigned* heightCandidate, uintptr_t* viewportFunction) noexcept {
    __try {
        *widthCandidate = *reinterpret_cast<unsigned*>(static_cast<unsigned char*>(context) + 0x18);
        *heightCandidate = *reinterpret_cast<unsigned*>(static_cast<unsigned char*>(context) + 0x1c);
        // Only records the address, not a viewport or a hook on this method.
        *viewportFunction = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(list))[21]);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RecordStage(unsigned index, const char* name, void* context, ID3D12GraphicsCommandList* list,
    ID3D12Resource* source, uint64_t auxiliary0 = 0, uint64_t auxiliary1 = 0) noexcept {
    const auto call = g_stageCalls[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (call > 61000 || (call > 16 && call % 127 != 0)) return;
    try {
        D3D12_RESOURCE_DESC description{};
        unsigned widthCandidate = 0, heightCandidate = 0;
        uintptr_t viewportFunction = 0;
        const bool metadata = ReadStageMetadata(context, list, &widthCandidate, &heightCandidate, &viewportFunction);
        std::ostringstream out;
        out << "{\"event\":\"upstream_stage\",\"stage\":\"" << name << "\",\"stage_call\":" << call
            << ",\"tick_ms\":" << GetTickCount64() << ",\"thread_id\":" << GetCurrentThreadId()
            << ",\"completed_evaluates\":" << g_evaluations.load(std::memory_order_relaxed)
            << ",\"context\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(context)
            << "\",\"command_list\":\"0x" << reinterpret_cast<uintptr_t>(list)
            << "\",\"viewport_method_address\":\"0x" << viewportFunction
            << "\",\"auxiliary0\":\"0x" << auxiliary0 << "\",\"auxiliary1\":\"0x" << auxiliary1 << std::dec << '"'
            << ",\"context_size_candidate\":";
        if (metadata) out << '[' << widthCandidate << ',' << heightCandidate << ']'; else out << "null";
        out << ",\"source\":";
        if (DescribeResource(source, &description)) {
            out << "{\"identity\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(source) << std::dec
                << "\",\"width\":" << description.Width << ",\"height\":" << description.Height
                << ",\"format\":" << unsigned(description.Format) << ",\"flags\":" << unsigned(description.Flags) << '}';
        } else { out << "null"; }
        out << '}';
        Log(out.str());
    } catch (...) {}
}

void __cdecl ObserveEncodedMotion(void* context, ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    uintptr_t opaque4, uint64_t argument5, uint64_t argument6) {
    RecordStage(0, "encoded_motion_before_decode", context, list, source, argument5, argument6);
    g_encodedMotion.load(std::memory_order_acquire)(context, list, source, opaque4, argument5, argument6);
}
void __cdecl ObserveColorStage(void* context, ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    unsigned state) {
    RecordStage(1, "pre_upscale_color_candidate", context, list, source, state);
    g_colorStage.load(std::memory_order_acquire)(context, list, source, state);
}
void __cdecl ObserveUpscaleEntry(void* context, ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    unsigned state, uint64_t jitterBits) {
    RecordStage(2, "upscale_entry_resource", context, list, source, state, jitterBits);
    g_upscaleEntry.load(std::memory_order_acquire)(context, list, source, state, jitterBits);
}

void InstallStageHooks(unsigned char* base) noexcept {
    constexpr std::array<size_t, 3> rvas{0x114d60, 0x116710, 0x117a20};
    constexpr std::array<unsigned char, 16> prologue{
        0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    for (auto rva : rvas) {
        if (memcmp(base + rva, prologue.data(), prologue.size()) != 0) {
            Log("{\"event\":\"stage_hooks_refused\",\"reason\":\"stage_prologue_changed\"}"); return;
        }
    }
    if (MH_Initialize() != MH_OK) {
        Log("{\"event\":\"stage_hooks_refused\",\"reason\":\"minhook_initialization_failed\"}"); return;
    }
    const std::array<void*, 3> detours{reinterpret_cast<void*>(&ObserveEncodedMotion),
        reinterpret_cast<void*>(&ObserveColorStage), reinterpret_cast<void*>(&ObserveUpscaleEntry)};
    std::array<void*, 3> original{};
    for (size_t i = 0; i < rvas.size(); ++i) {
        if (MH_CreateHook(base + rvas[i], detours[i], &original[i]) != MH_OK) {
            MH_Uninitialize(); // No hook has been enabled yet.
            Log("{\"event\":\"stage_hooks_refused\",\"reason\":\"stage_hook_creation_failed\"}"); return;
        }
    }
    g_encodedMotion.store(reinterpret_cast<EncodedMotionFunction>(original[0]), std::memory_order_release);
    g_colorStage.store(reinterpret_cast<ColorStageFunction>(original[1]), std::memory_order_release);
    g_upscaleEntry.store(reinterpret_cast<UpscaleEntryFunction>(original[2]), std::memory_order_release);
    const auto status = MH_EnableHook(MH_ALL_HOOKS);
    if (status == MH_OK) {
        Log("{\"event\":\"stage_hooks_attached\",\"rvas\":[\"0x114d60\",\"0x116710\",\"0x117a20\"]}");
    } else {
        // An enable failure can be partial. Keep the module pinned and trampolines alive.
        Log("{\"event\":\"stage_hooks_enable_error\",\"partial_activation_possible\":true}");
    }
}

void UnsignedField(std::ostringstream& out, NVSDK_NGX_Parameter* params, const char* key, const char* name) {
    unsigned value = 0;
    out << ",\"" << key << "\":";
    if (GetUnsigned(params, name, &value)) out << value; else out << "null";
}
void IntegerField(std::ostringstream& out, NVSDK_NGX_Parameter* params, const char* key, const char* name) {
    int value = 0;
    out << ",\"" << key << "\":";
    if (GetInteger(params, name, &value)) out << value; else out << "null";
}
void FloatField(std::ostringstream& out, NVSDK_NGX_Parameter* params, const char* key, const char* name) {
    float value = 0;
    out << ",\"" << key << "\":";
    if (GetFloat(params, name, &value) && std::isfinite(value)) out << std::setprecision(9) << value;
    else out << "null";
}
void TextureField(std::ostringstream& out, NVSDK_NGX_Parameter* params, const char* key, const char* name) {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC desc{};
    out << ",\"" << key << "\":";
    if (!GetTexture(params, name, &resource, &desc)) { out << "null"; return; }
    out << "{\"identity\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(resource) << std::dec
        << "\",\"width\":" << desc.Width << ",\"height\":" << desc.Height
        << ",\"format\":" << unsigned(desc.Format) << ",\"dimension\":" << unsigned(desc.Dimension)
        << ",\"flags\":" << unsigned(desc.Flags) << ",\"samples\":" << desc.SampleDesc.Count << '}';
}

std::string Snapshot(NVSDK_NGX_Parameter* params) {
    std::ostringstream out;
    UnsignedField(out, params, "render_width", "Width");
    UnsignedField(out, params, "render_height", "Height");
    UnsignedField(out, params, "output_width", "OutWidth");
    UnsignedField(out, params, "output_height", "OutHeight");
    UnsignedField(out, params, "active_width", "DLSS.Render.Subrect.Dimensions.Width");
    UnsignedField(out, params, "active_height", "DLSS.Render.Subrect.Dimensions.Height");
    IntegerField(out, params, "quality", "PerfQualityValue");
    IntegerField(out, params, "create_flags", "DLSS.Feature.Create.Flags");
    IntegerField(out, params, "reset", "Reset");
    FloatField(out, params, "jitter_x", "Jitter.Offset.X");
    FloatField(out, params, "jitter_y", "Jitter.Offset.Y");
    FloatField(out, params, "mv_scale_x", "MV.Scale.X");
    FloatField(out, params, "mv_scale_y", "MV.Scale.Y");
    FloatField(out, params, "pre_exposure", "DLSS.Pre.Exposure");
    FloatField(out, params, "exposure_scale", "DLSS.Exposure.Scale");
    TextureField(out, params, "color", "Color");
    TextureField(out, params, "depth", "Depth");
    TextureField(out, params, "motion_vectors", "MotionVectors");
    TextureField(out, params, "output", "Output");
    TextureField(out, params, "exposure", "ExposureTexture");
    return out.str();
}

NVSDK_NGX_Result __cdecl ObserveCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle) {
    std::string snapshot;
    try { snapshot = Snapshot(params); } catch (...) {}
    const auto result = g_create.load(std::memory_order_acquire)(list, feature, params, handle);
    try {
        Log("{\"event\":\"create\",\"feature\":" + std::to_string(unsigned(feature))
            + ",\"result\":" + std::to_string(unsigned(result)) + snapshot + "}");
    } catch (...) {}
    return result;
}

NVSDK_NGX_Result __cdecl ObserveEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* params, void* callback) {
    const auto frame = g_evaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    // A prime stride avoids repeatedly sampling the same phase of common 8/16/32-frame jitter cycles.
    const bool sample = g_samples.load(std::memory_order_relaxed) < 512 && (frame <= 32 || frame % 127 == 0);
    std::string snapshot;
    if (sample) { try { snapshot = Snapshot(params); } catch (...) {} }
    const auto result = g_evaluate.load(std::memory_order_acquire)(list, handle, params, callback);
    if (sample) {
        g_samples.fetch_add(1, std::memory_order_relaxed);
        try {
            Log("{\"event\":\"evaluate\",\"call\":" + std::to_string(frame)
                + ",\"tick_ms\":" + std::to_string(GetTickCount64())
                + ",\"result\":" + std::to_string(unsigned(result)) + snapshot + "}");
        } catch (...) {}
    }
    return result;
}

bool Executable(void* pointer) {
    MEMORY_BASIC_INFORMATION info{};
    if (!pointer || !VirtualQuery(pointer, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
bool WritableSlot(void* pointer) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

DWORD WINAPI Worker(void*) {
    try {
        std::array<wchar_t, 32768> path{};
        if (!GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()))) return 0;
        if (_wcsicmp(std::filesystem::path(path.data()).filename().c_str(), L"MonsterHunterWorld.exe") != 0) return 0;
        if (!GetModuleFileNameW(g_self, path.data(), static_cast<DWORD>(path.size()))) return 0;
        const auto directory = std::filesystem::path(path.data()).parent_path();
        g_log.open(directory / (L"MhwSrProbe-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl"), std::ios::app);
        Log("{\"event\":\"probe_start\",\"version\":3,\"changes_render_parameters\":false,\"captures_texture_pixels\":false}");
        HMODULE mhwss = nullptr;
        for (unsigned tries = 0; tries < 1800; ++tries) {
            mhwss = GetModuleHandleW(L"MHWSS.dll");
            if (mhwss) break;
            Sleep(100);
        }
        if (!mhwss) { Log("{\"event\":\"refused\",\"reason\":\"MHWSS_not_loaded_within_180_seconds\"}"); return 0; }
        if (!GetModuleFileNameW(mhwss, path.data(), static_cast<DWORD>(path.size()))) return 0;
        const auto digest = HashFile(path.data());
        if (digest != kExpectedHash) {
            Log("{\"event\":\"refused\",\"reason\":\"unsupported_MHWSS_hash\",\"sha256\":\"" + digest + "\"}"); return 0;
        }
        auto* base = reinterpret_cast<unsigned char*>(mhwss);
        if (memcmp(base + kCreateLoadRva, kCreateLoad.data(), kCreateLoad.size()) != 0 ||
            memcmp(base + kEvaluateLoadRva, kEvaluateLoad.data(), kEvaluateLoad.size()) != 0) {
            Log("{\"event\":\"refused\",\"reason\":\"loaded_SDK_stub_bytes_differ\"}"); return 0;
        }
        auto* createSlot = reinterpret_cast<void* volatile*>(base + kCreateSlotRva);
        auto* evaluateSlot = reinterpret_cast<void* volatile*>(base + kEvaluateSlotRva);
        if (!WritableSlot(base + kCreateSlotRva) || !WritableSlot(base + kEvaluateSlotRva)) {
            Log("{\"event\":\"refused\",\"reason\":\"dispatch_slots_not_writable\"}"); return 0;
        }
        void* originalCreate = nullptr;
        void* originalEvaluate = nullptr;
        for (unsigned tries = 0; tries < 1800; ++tries) {
            originalCreate = InterlockedCompareExchangePointer(createSlot, nullptr, nullptr);
            originalEvaluate = InterlockedCompareExchangePointer(evaluateSlot, nullptr, nullptr);
            if (Executable(originalCreate) && Executable(originalEvaluate)) break;
            Sleep(100);
        }
        if (!Executable(originalCreate) || !Executable(originalEvaluate)) {
            Log("{\"event\":\"refused\",\"reason\":\"NGX_dispatch_not_initialized\"}"); return 0;
        }
        // Keep both modules alive until process exit; callbacks must never point to an unloaded DLL.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(mhwss), &pinned) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(g_self), &pinned)) {
            Log("{\"event\":\"refused\",\"reason\":\"module_pin_failed\"}"); return 0;
        }
        g_create.store(reinterpret_cast<CreateFunction>(originalCreate), std::memory_order_release);
        g_evaluate.store(reinterpret_cast<EvaluateFunction>(originalEvaluate), std::memory_order_release);
        if (InterlockedCompareExchangePointer(createSlot, reinterpret_cast<void*>(&ObserveCreate), originalCreate) != originalCreate) {
            Log("{\"event\":\"refused\",\"reason\":\"create_slot_changed\"}"); return 0;
        }
        if (InterlockedCompareExchangePointer(evaluateSlot, reinterpret_cast<void*>(&ObserveEvaluate), originalEvaluate) != originalEvaluate) {
            InterlockedCompareExchangePointer(createSlot, originalCreate, reinterpret_cast<void*>(&ObserveCreate));
            Log("{\"event\":\"refused\",\"reason\":\"evaluate_slot_changed\"}"); return 0;
        }
        Log("{\"event\":\"attached\",\"sha256\":\"" + digest +
            "\",\"create_may_have_preceded_attachment\":true,\"maximum_evaluate_samples\":512}");
        InstallStageHooks(base);
    } catch (...) { Log("{\"event\":\"probe_error\"}"); }
    return 0;
}

void StartWorker() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread); else InterlockedExchange(&g_started, 0);
}
} // namespace

extern "C" __declspec(dllexport) void Initialize() { StartWorker(); }
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = module; StartWorker(); }
    return TRUE;
}
