// Observes raster state at sampled direct draw calls. Does not change GPU arguments.
#include <Windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(void*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using ClearFn = void(STDMETHODCALLTYPE*)(void*, ID3D12PipelineState*);
using DrawFn = void(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT, UINT);
using IndexedFn = void(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT, INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT);
using ViewportFn = void(STDMETHODCALLTYPE*)(void*, UINT, const D3D12_VIEWPORT*);
using ScissorFn = void(STDMETHODCALLTYPE*)(void*, UINT, const D3D12_RECT*);
using PipelineFn = void(STDMETHODCALLTYPE*)(void*, ID3D12PipelineState*);
std::atomic<ResetFn> g_reset{};
std::atomic<ClearFn> g_clear{};
std::atomic<DrawFn> g_draw{};
std::atomic<IndexedFn> g_indexed{};
std::atomic<DispatchFn> g_dispatch{};
std::atomic<ViewportFn> g_viewport{};
std::atomic<ScissorFn> g_scissor{};
std::atomic<PipelineFn> g_pipeline{};
std::atomic<bool> g_collect{false};
std::atomic<unsigned long long> g_dropped{0};
HMODULE g_self = nullptr;
std::ofstream g_log;
std::mutex g_mutex;

struct RasterState {
    UINT viewports = 0, scissors = 0;
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    uintptr_t pipeline = 0;
};
struct Count {
    uint64_t samples = 0;
    UINT minimum = UINT_MAX, maximum = 0, maxInstances = 0;
    uintptr_t pipelineExample = 0, listExample = 0;
};
using Key = std::array<uint32_t, 16>;
std::unordered_map<void*, RasterState> g_states;
std::map<Key, Count> g_counts;

void Log(const std::string& value) noexcept {
    try { if (g_log) { g_log << value << '\n'; g_log.flush(); } } catch (...) {}
}
uint32_t Bits(float value) { uint32_t bits; memcpy(&bits, &value, sizeof(bits)); return bits; }
float Float(uint32_t value) { float number; memcpy(&number, &value, sizeof(number)); return number; }
void Number(std::ostream& out, float value) { if (std::isfinite(value)) out << value; else out << "null"; }

bool ReadViewport(UINT count, const D3D12_VIEWPORT* input, D3D12_VIEWPORT* out) noexcept {
    __try { if (count && count <= 16 && input) { *out = *input; return true; } }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
bool ReadScissor(UINT count, const D3D12_RECT* input, D3D12_RECT* out) noexcept {
    __try { if (count && count <= 16 && input) { *out = *input; return true; } }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
RasterState* State(void* list) {
    auto found = g_states.find(list);
    if (found != g_states.end()) return &found->second;
    if (g_states.size() >= 2048) { ++g_dropped; return nullptr; }
    return &g_states.try_emplace(list).first->second;
}
void ResetState(void* list, ID3D12PipelineState* pipeline) noexcept {
    if (!g_collect.load(std::memory_order_relaxed)) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto* state = State(list)) { *state = {}; state->pipeline = reinterpret_cast<uintptr_t>(pipeline); }
    } catch (...) { ++g_dropped; }
}

void Record(unsigned kind, void* list, UINT count, UINT instances, UINT z = 0) noexcept {
    if (!g_collect.load(std::memory_order_relaxed)) return;
    thread_local std::array<uint64_t, 3> calls{};
    const auto call = ++calls[kind];
    if (call > 64 && call % 31 != 0) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        Key key{};
        key[0] = kind;
        auto* state = State(list);
        if (kind == 2) { key[13] = count; key[14] = instances; key[15] = z; }
        else {
            if (state) {
                key[1] = state->viewports;
                key[2] = Bits(state->viewport.TopLeftX); key[3] = Bits(state->viewport.TopLeftY);
                key[4] = Bits(state->viewport.Width); key[5] = Bits(state->viewport.Height);
                key[6] = Bits(state->viewport.MinDepth); key[7] = Bits(state->viewport.MaxDepth);
                key[8] = state->scissors;
                key[9] = static_cast<uint32_t>(state->scissor.left); key[10] = static_cast<uint32_t>(state->scissor.top);
                key[11] = static_cast<uint32_t>(state->scissor.right); key[12] = static_cast<uint32_t>(state->scissor.bottom);
            }
            key[13] = count <= 6 ? 0 : count <= 256 ? 1 : count <= 4096 ? 2 : 3;
        }
        auto found = g_counts.find(key);
        if (found == g_counts.end() && g_counts.size() >= 2048) { ++g_dropped; return; }
        auto& values = g_counts[key];
        ++values.samples;
        values.minimum = (std::min)(values.minimum, count); values.maximum = (std::max)(values.maximum, count);
        values.maxInstances = (std::max)(values.maxInstances, instances);
        values.pipelineExample = state ? state->pipeline : 0;
        values.listExample = reinterpret_cast<uintptr_t>(list);
    } catch (...) { ++g_dropped; }
}

HRESULT STDMETHODCALLTYPE OnReset(void* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline) {
    const auto result = g_reset.load()(list, allocator, pipeline);
    if (SUCCEEDED(result)) ResetState(list, pipeline);
    return result;
}
void STDMETHODCALLTYPE OnClear(void* list, ID3D12PipelineState* pipeline) {
    g_clear.load()(list, pipeline); ResetState(list, pipeline);
}
void STDMETHODCALLTYPE OnDraw(void* list, UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance) {
    Record(0, list, vertices, instances); g_draw.load()(list, vertices, instances, firstVertex, firstInstance);
}
void STDMETHODCALLTYPE OnIndexed(void* list, UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance) {
    Record(1, list, indices, instances); g_indexed.load()(list, indices, instances, firstIndex, baseVertex, firstInstance);
}
void STDMETHODCALLTYPE OnDispatch(void* list, UINT x, UINT y, UINT z) {
    Record(2, list, x, y, z); g_dispatch.load()(list, x, y, z);
}
void STDMETHODCALLTYPE OnViewport(void* list, UINT count, const D3D12_VIEWPORT* viewports) {
    if (g_collect.load(std::memory_order_relaxed)) {
        D3D12_VIEWPORT value{};
        const bool valid = ReadViewport(count, viewports, &value);
        try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = State(list)) { state->viewports = valid ? count : 0; state->viewport = value; } }
        catch (...) { ++g_dropped; }
    }
    g_viewport.load()(list, count, viewports);
}
void STDMETHODCALLTYPE OnScissor(void* list, UINT count, const D3D12_RECT* rectangles) {
    if (g_collect.load(std::memory_order_relaxed)) {
        D3D12_RECT value{};
        const bool valid = ReadScissor(count, rectangles, &value);
        try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = State(list)) { state->scissors = valid ? count : 0; state->scissor = value; } }
        catch (...) { ++g_dropped; }
    }
    g_scissor.load()(list, count, rectangles);
}
void STDMETHODCALLTYPE OnPipeline(void* list, ID3D12PipelineState* pipeline) {
    if (g_collect.load(std::memory_order_relaxed)) {
        try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = State(list)) state->pipeline = reinterpret_cast<uintptr_t>(pipeline); }
        catch (...) { ++g_dropped; }
    }
    g_pipeline.load()(list, pipeline);
}

void Drain(uint64_t begin, uint64_t end) {
    std::map<Key, Count> counts;
    { std::lock_guard<std::mutex> lock(g_mutex); counts.swap(g_counts); }
    std::ostringstream out;
    out << "{\"event\":\"raster_window\",\"begin_tick_ms\":" << begin << ",\"end_tick_ms\":" << end
        << ",\"dropped_total\":" << g_dropped.load() << ",\"groups\":[";
    bool first = true;
    for (const auto& pair : counts) {
        const auto& k = pair.first; const auto& value = pair.second;
        if (!first) out << ','; first = false;
        out << "{\"kind\":\"" << (k[0] == 0 ? "draw" : k[0] == 1 ? "indexed" : "dispatch")
            << "\",\"samples\":" << value.samples << ",\"pipeline_example\":\"0x" << std::hex << value.pipelineExample
            << "\",\"command_list_example\":\"0x" << value.listExample << std::dec << '"';
        if (k[0] == 2) out << ",\"thread_groups\":[" << k[13] << ',' << k[14] << ',' << k[15] << ']';
        else {
            out << ",\"viewport_count\":" << k[1] << ",\"first_viewport\":[";
            for (size_t i = 2; i <= 7; ++i) { if (i > 2) out << ','; Number(out, Float(k[i])); }
            out << "],\"scissor_count\":" << k[8] << ",\"first_scissor\":[";
            for (size_t i = 9; i <= 12; ++i) { if (i > 9) out << ','; out << static_cast<int32_t>(k[i]); }
            out << "],\"geometry_bucket\":" << k[13] << ",\"min_count\":" << value.minimum
                << ",\"max_count\":" << value.maximum << ",\"max_instances\":" << value.maxInstances;
        }
        out << '}';
    }
    out << "]}"; Log(out.str());
}

std::string HashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty() || bytes.size() > ULONG_MAX) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<unsigned char, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) return {};
    std::ostringstream text;
    for (auto byte : digest) text << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return text.str();
}

std::wstring Setting(const std::filesystem::path& ini, const std::wstring& name) {
    std::array<wchar_t, 1024> text{};
    GetPrivateProfileStringW(L"Capture", name.c_str(), L"", text.data(), static_cast<DWORD>(text.size()), ini.c_str());
    return text.data();
}
std::array<unsigned char, 16> ParseBytes(const std::wstring& text) {
    if (text.size() != 32) throw std::runtime_error("A 16-byte signature is required.");
    std::array<unsigned char, 16> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        size_t used = 0;
        const auto value = std::stoul(text.substr(i * 2, 2), &used, 16);
        if (used != 2 || value > 255) throw std::runtime_error("Invalid byte signature.");
        bytes[i] = static_cast<unsigned char>(value);
    }
    return bytes;
}

std::array<void*, 8> g_targets{};
std::array<std::array<unsigned char, 16>, 8> g_ourPatches{};
std::array<std::array<unsigned char, 16>, 8> g_expectedOriginals{};
bool g_hooksEnabled = false;
bool g_canRearm = true;

void StopHooks() noexcept {
    g_collect.store(false, std::memory_order_release);
    if (!g_hooksEnabled) return;
    for (size_t i = 0; i < g_targets.size(); ++i) {
        if (memcmp(g_targets[i], g_ourPatches[i].data(), g_ourPatches[i].size()) != 0) {
            g_canRearm = false;
            Log("{\"event\":\"stop\",\"recording\":false,\"hooks_restored\":false,\"reason\":\"another_patch_changed_a_target\"}");
            return;
        }
    }
    const auto status = MH_DisableHook(MH_ALL_HOOKS);
    if (status == MH_OK) g_hooksEnabled = false; else g_canRearm = false;
    Log(status == MH_OK ? "{\"event\":\"stop\",\"recording\":false,\"hooks_restored\":true}" :
        "{\"event\":\"stop\",\"recording\":false,\"hooks_restored\":false,\"reason\":\"disable_failed\"}");
    // Keep trampolines and modules alive until process exit, including on partial disable failure.
}

DWORD WINAPI Worker(void*) {
    try {
        std::array<wchar_t, 32768> text{};
        if (!GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()))) return 0;
        if (_wcsicmp(std::filesystem::path(text.data()).filename().c_str(), L"MonsterHunterWorld.exe") != 0) return 0;
        if (!GetModuleFileNameW(g_self, text.data(), static_cast<DWORD>(text.size()))) return 0;
        const auto folder = std::filesystem::path(text.data()).parent_path();
        const auto ini = folder / L"MhwViewportProbe.ini";
        g_log.open(folder / (L"MhwViewportProbe-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl"), std::ios::app);
        Log("{\"event\":\"start\",\"version\":2,\"changes_gpu_parameters\":false,\"captures_gpu_pixels\":false,\"auto_capture_on_setting_change\":true}");
        if (GetPrivateProfileIntW(L"Capture", L"Pid", 0, ini.c_str()) != GetCurrentProcessId()) {
            Log("{\"event\":\"refused\",\"reason\":\"process_id_mismatch\"}"); return 0;
        }
        const HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
        if (!core || !GetModuleFileNameW(core, text.data(), static_cast<DWORD>(text.size()))) return 0;
        const auto digest = HashFile(text.data());
        if (digest.empty() || std::wstring(digest.begin(), digest.end()) != Setting(ini, L"CoreSha256")) {
            Log("{\"event\":\"refused\",\"reason\":\"D3D12Core_hash_mismatch\"}"); return 0;
        }
        auto* base = reinterpret_cast<unsigned char*>(core);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const size_t size = nt->OptionalHeader.SizeOfImage;
        const std::array<const wchar_t*, 8> names{L"Reset", L"ClearState", L"DrawInstanced", L"DrawIndexedInstanced",
            L"Dispatch", L"RSSetViewports", L"RSSetScissorRects", L"SetPipelineState"};
        const std::array<void*, 8> replacements{reinterpret_cast<void*>(&OnReset), reinterpret_cast<void*>(&OnClear),
            reinterpret_cast<void*>(&OnDraw), reinterpret_cast<void*>(&OnIndexed), reinterpret_cast<void*>(&OnDispatch),
            reinterpret_cast<void*>(&OnViewport), reinterpret_cast<void*>(&OnScissor), reinterpret_cast<void*>(&OnPipeline)};
        for (size_t i = 0; i < names.size(); ++i) {
            const size_t rva = std::stoull(Setting(ini, std::wstring(names[i]) + L"Rva"), nullptr, 0);
            const auto expected = ParseBytes(Setting(ini, std::wstring(names[i]) + L"Bytes"));
            if (rva >= size || size - rva < expected.size() || memcmp(base + rva, expected.data(), expected.size()) != 0) {
                Log("{\"event\":\"refused\",\"reason\":\"live_method_signature_mismatch\"}"); return 0;
            }
            g_targets[i] = base + rva;
            g_expectedOriginals[i] = expected;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(core), &pinned) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(g_self), &pinned)) return 0;
        if (MH_Initialize() != MH_OK) { Log("{\"event\":\"refused\",\"reason\":\"minhook_init_failed\"}"); return 0; }
        std::array<void*, 8> originals{};
        for (size_t i = 0; i < g_targets.size(); ++i) {
            if (MH_CreateHook(g_targets[i], replacements[i], &originals[i]) != MH_OK) {
                MH_Uninitialize(); Log("{\"event\":\"refused\",\"reason\":\"hook_creation_failed\"}"); return 0;
            }
        }
        g_reset.store(reinterpret_cast<ResetFn>(originals[0]));
        g_clear.store(reinterpret_cast<ClearFn>(originals[1]));
        g_draw.store(reinterpret_cast<DrawFn>(originals[2]));
        g_indexed.store(reinterpret_cast<IndexedFn>(originals[3]));
        g_dispatch.store(reinterpret_cast<DispatchFn>(originals[4]));
        g_viewport.store(reinterpret_cast<ViewportFn>(originals[5]));
        g_scissor.store(reinterpret_cast<ScissorFn>(originals[6]));
        g_pipeline.store(reinterpret_cast<PipelineFn>(originals[7]));
        std::array<wchar_t, 32768> executable{};
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        const auto graphics = std::filesystem::path(executable.data()).parent_path() / L"graphics_option.ini";
        auto currentSetting = [&]() {
            // Read the file directly instead of relying on Windows INI profile caching.
            std::ifstream input(graphics, std::ios::binary);
            std::string line;
            while (std::getline(input, line)) {
                constexpr char prefix[] = "ResolutionScaling=";
                if (line.compare(0, sizeof(prefix) - 1, prefix) != 0) continue;
                std::string value = line.substr(sizeof(prefix) - 1);
                while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
                return value == "High" ? std::string("High") : value == "Low" ? std::string("Low") :
                    value == "Mid" || value == "Medium" ? std::string("Medium") : std::string("other");
            }
            return std::string(); // A write may be in progress; do not invent a setting change.
        };
        auto marker = [&](const std::string& value) {
            Log("{\"event\":\"setting_marker\",\"tick_ms\":" + std::to_string(GetTickCount64()) +
                ",\"ResolutionScaling\":\"" + value + "\",\"source\":\"configuration_file\"}");
        };
        const auto prefix = L"Local\\MhwViewportProbe." + std::to_wstring(GetCurrentProcessId());
        HANDLE controls[2]{CreateEventW(nullptr, TRUE, FALSE, (prefix + L".Shutdown").c_str()),
            CreateEventW(nullptr, FALSE, FALSE, (prefix + L".Start").c_str())};
        if (!controls[0] || !controls[1]) {
            if (controls[0]) CloseHandle(controls[0]); if (controls[1]) CloseHandle(controls[1]);
            Log("{\"event\":\"refused\",\"reason\":\"control_event_creation_failed\"}"); return 0;
        }
        std::string setting = currentSetting(); marker(setting);
        UINT duration = GetPrivateProfileIntW(L"Capture", L"DurationSeconds", 120, ini.c_str());
        if (duration < 30 || duration > 600) duration = 120;
        bool capture = true;
        bool shutdown = false;
        unsigned session = 0;
        while (!shutdown && g_canRearm && session < 12) {
            if (!capture) {
                const auto wait = WaitForMultipleObjects(2, controls, FALSE, 500);
                if (wait == WAIT_OBJECT_0) break;
                const auto next = currentSetting();
                capture = wait == WAIT_OBJECT_0 + 1 || (!next.empty() && next != setting);
                if (!next.empty() && next != setting) { setting = next; marker(setting); }
                if (!capture) continue;
            }
            for (size_t i = 0; i < g_targets.size(); ++i)
                if (memcmp(g_targets[i], g_expectedOriginals[i].data(), g_expectedOriginals[i].size()) != 0) g_canRearm = false;
            if (!g_canRearm) { Log("{\"event\":\"refused\",\"reason\":\"method_changed_before_rearm\"}"); break; }
            { std::lock_guard<std::mutex> lock(g_mutex); g_states.clear(); g_counts.clear(); }
            if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
                MH_DisableHook(MH_ALL_HOOKS); Log("{\"event\":\"enable_failed\",\"recording\":false}"); break;
            }
            for (size_t i = 0; i < g_targets.size(); ++i) memcpy(g_ourPatches[i].data(), g_targets[i], g_ourPatches[i].size());
            g_hooksEnabled = true;
            ++session;
            uint64_t previous = GetTickCount64();
            const uint64_t hardDeadline = previous + 600000;
            uint64_t deadline = previous + uint64_t(duration) * 1000;
            Log("{\"event\":\"attached\",\"session\":" + std::to_string(session) + ",\"tick_ms\":" + std::to_string(previous) +
                ",\"duration_seconds\":" + std::to_string(duration) + ",\"sample_stride\":31,\"only_first_viewport_and_scissor\":true}");
            g_collect.store(true, std::memory_order_release);
            while (GetTickCount64() < deadline) {
                if (WaitForMultipleObjects(2, controls, FALSE, 2000) == WAIT_OBJECT_0) { shutdown = true; break; }
                const auto now = GetTickCount64(); Drain(previous, now); previous = now;
                const auto next = currentSetting();
                if (!next.empty() && next != setting) {
                    setting = next; marker(setting);
                    deadline = (std::min)(hardDeadline, (std::max)(deadline, now + 45000));
                }
            }
            g_collect.store(false, std::memory_order_release);
            Drain(previous, GetTickCount64());
            StopHooks();
            capture = false;
            Log("{\"event\":\"idle\",\"auto_trigger_on_setting_change\":true}");
        }
        CloseHandle(controls[0]); CloseHandle(controls[1]);
        Log("{\"event\":\"controller_exit\"}");
    } catch (...) { Log("{\"event\":\"observer_error\"}"); }
    StopHooks();
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        HANDLE worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
