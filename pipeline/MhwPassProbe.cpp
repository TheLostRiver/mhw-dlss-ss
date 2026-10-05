// Read-only CPU tracing of draws that bind MHWSS's returned scene resource.
// Does not inject GPU commands or change any graphics arguments.
#include "../readback/CaptureCommon.h"
#include <d3d12.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace {
using List = ID3D12GraphicsCommandList;
using Device = ID3D12Device;
using PsoFn = HRESULT(STDMETHODCALLTYPE*)(Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateSignatureFn = HRESULT(STDMETHODCALLTYPE*)(Device*, UINT, const void*, SIZE_T, REFIID, void**);
using ComputePsoFn = HRESULT(STDMETHODCALLTYPE*)(Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using SrvFn = void(STDMETHODCALLTYPE*)(Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using ConstantViewFn = void(STDMETHODCALLTYPE*)(Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using UavFn = void(STDMETHODCALLTYPE*)(Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using RtvFn = void(STDMETHODCALLTYPE*)(Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyFn = void(STDMETHODCALLTYPE*)(Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopySimpleFn = void(STDMETHODCALLTYPE*)(Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT);
using IndexedFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using ViewportFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);
using PipelineFn = void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
using HeapsFn = void(STDMETHODCALLTYPE*)(List*, UINT, ID3D12DescriptorHeap* const*);
using SignatureFn = void(STDMETHODCALLTYPE*)(List*, ID3D12RootSignature*);
using TableFn = void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using CbvFn = void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using ConstantFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using ConstantsFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const void*, UINT);
using TargetsFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using UpscaleFn = void(__cdecl*)(void*, List*, ID3D12Resource*, unsigned, uint64_t);
PsoFn g_pso{}; ComputePsoFn g_computePso{}; SrvFn g_srv{}; RtvFn g_rtv{}; CopyFn g_copy{}; CopySimpleFn g_copySimple{};
ConstantViewFn g_constantView{}; UavFn g_uav{};
CreateSignatureFn g_createSignature{};
ConstantFn g_computeConstant{}, g_graphicsConstant{}; ConstantsFn g_computeConstants{}, g_graphicsConstants{};
ResetFn g_reset{}; DrawFn g_draw{}; IndexedFn g_indexed{}; DispatchFn g_dispatch{}; ViewportFn g_viewport{};
PipelineFn g_pipeline{}; HeapsFn g_heaps{}; SignatureFn g_computeSig{}, g_graphicsSig{};
PipelineFn g_clear{};
TableFn g_computeTable{}, g_graphicsTable{}; CbvFn g_computeCbv{}, g_graphicsCbv{}; TargetsFn g_targets{}; UpscaleFn g_upscale{};
HMODULE g_self{};
volatile LONG g_started = 0;
bool g_partialEnable = false;
std::filesystem::path g_folder;
std::ofstream g_log;
std::mutex g_mutex, g_logMutex;
std::atomic<bool> g_enabled{false}, g_active{false};
std::atomic<unsigned> g_armFrames{0};
std::atomic<uint64_t> g_frames{0};
thread_local bool g_insideUpscale = false;
constexpr char kMhwssHash[] = "55d52caf2e7bba5220ec721149bc067679bf9391bbf55d62bed3ec9522ccd09a";
constexpr char kGameHash[] = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";
constexpr char kParameterProbeHash[] = "42de0846e46a593fd1b3743abd92e1e0b098760b575322867977127a750618c7";

void Log(const std::string& line) noexcept {
    try { std::lock_guard<std::mutex> lock(g_logMutex); if (g_log) { g_log << line << '\n'; g_log.flush(); } } catch (...) {}
}
void Problem(const char* reason) noexcept {
    try { Log(std::string("{\"event\":\"trace_notice\",\"reason\":\"") + reason + "\"}"); } catch (...) {}
}
std::string Hex(uintptr_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }
struct View { uintptr_t resource{}; UINT64 width{}; UINT height{}, format{}, dimension{}, flags{}; unsigned type = 0; };
std::unordered_map<SIZE_T, View> g_views;
struct Heap { UINT64 gpu{}; SIZE_T cpu{}; UINT count{}, increment{}; };
struct Roots {
    std::array<UINT64, 32> tables{}, cbvs{}, constantMasks{};
    std::array<std::array<UINT, 64>, 32> constants{};
    uintptr_t signature{};
};
struct State {
    bool resetObserved = false;
    uintptr_t pso{}; UINT viewportCount{}; D3D12_VIEWPORT viewport{};
    std::array<Heap, 2> heaps{}; UINT heapCount{};
    std::array<SIZE_T, 8> rtvs{}; UINT rtvCount{};
    Roots graphics{}, compute{};
};
std::unordered_map<List*, State> g_states;
struct Pipeline {
    std::vector<unsigned char> shader;
    std::vector<unsigned char> vertex;
    bool compute = false;
    uintptr_t rootSignature{};
    UINT renderTargets{};
    uint64_t generation{};
};
std::unordered_map<uintptr_t, Pipeline> g_pipelines;
std::unordered_map<uintptr_t, std::vector<unsigned char>> g_rootSignatures;
size_t g_shaderBytes = 0;
uint64_t g_pipelineGeneration = 0;
std::map<uintptr_t, unsigned> g_frontier;
std::vector<std::string> g_records;
std::set<uintptr_t> g_neededShaders, g_savedShaders;
std::set<uintptr_t> g_neededSignatures, g_savedSignatures;
bool g_exportAllShaders = false;
unsigned g_dropped = 0;
UINT g_rtvIncrement = 0;

bool Tracking() { return g_enabled.load(std::memory_order_relaxed) && (g_active.load(std::memory_order_relaxed) || g_armFrames.load(std::memory_order_relaxed)); }
View Describe(ID3D12Resource* resource, unsigned type = 0) {
    if (!resource) return {};
    const auto d = resource->GetDesc(); return {reinterpret_cast<uintptr_t>(resource), d.Width, d.Height, UINT(d.Format), UINT(d.Dimension), UINT(d.Flags), type};
}
void StoreView(SIZE_T handle, const View& view) {
    // Follow renderable color images, not material assets or structured lighting buffers.
    // A non-matching overwrite must erase an old entry so reused slots cannot look like old scene inputs.
    if (!view.resource || view.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || view.width < 64 || view.height < 64 ||
        !(view.flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
        (view.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) { g_views.erase(handle); return; }
    if (g_views.size() >= 262144 && !g_views.count(handle)) { ++g_dropped; return; }
    g_views[handle] = view;
}
void ViewJson(std::ostream& out, const View& view) {
    out << "{\"resource\":\"" << Hex(view.resource) << "\",\"width\":" << view.width << ",\"height\":" << view.height
        << ",\"format\":" << view.format << ",\"dimension\":" << view.dimension << ",\"flags\":" << view.flags << ",\"view_type\":" << view.type << '}';
}
void STDMETHODCALLTYPE OnSrv(Device* device, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_srv(device, resource, desc, handle);
    if (!g_enabled.load()) return;
    try { const auto view = Describe(resource, 1); std::lock_guard<std::mutex> lock(g_mutex); StoreView(handle.ptr, view); } catch (...) { Problem("srv_tracking_failed"); }
}
void STDMETHODCALLTYPE OnConstantView(Device* device, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_constantView(device, desc, handle);
    if (g_enabled.load()) try { std::lock_guard<std::mutex> lock(g_mutex); g_views.erase(handle.ptr); } catch (...) {}
}
void STDMETHODCALLTYPE OnUav(Device* device, ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_uav(device, resource, counter, desc, handle);
    if (g_enabled.load()) try { const auto view = Describe(resource, 3); std::lock_guard<std::mutex> lock(g_mutex); StoreView(handle.ptr, view); } catch (...) {}
}
void STDMETHODCALLTYPE OnRtv(Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_rtv(device, resource, desc, handle);
    if (!g_enabled.load()) return;
    try { const auto view = Describe(resource, 2); const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        std::lock_guard<std::mutex> lock(g_mutex); g_rtvIncrement = increment; StoreView(handle.ptr, view); } catch (...) { Problem("rtv_tracking_failed"); }
}
void CopyView(SIZE_T destination, SIZE_T source) {
    const auto found = g_views.find(source); if (found != g_views.end()) StoreView(destination, found->second); else g_views.erase(destination);
}
void STDMETHODCALLTYPE OnCopySimple(Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (g_enabled.load() && (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) {
        try { const auto stride = device->GetDescriptorHandleIncrementSize(type); std::lock_guard<std::mutex> lock(g_mutex);
            if (count <= 65536) for (UINT i = 0; i < count; ++i) CopyView(destination.ptr + SIZE_T(i) * stride, source.ptr + SIZE_T(i) * stride);
            else ++g_dropped;
        } catch (...) { Problem("descriptor_copy_failed"); }
    }
    g_copySimple(device, count, destination, source, type);
}
void STDMETHODCALLTYPE OnCopy(Device* device, UINT destinationCount, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations, const UINT* destinationSizes,
    UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (g_enabled.load() && (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) {
        try {
            const auto stride = device->GetDescriptorHandleIncrementSize(type); std::lock_guard<std::mutex> lock(g_mutex);
            UINT si = 0, offset = 0, budget = 65536;
            if (destinationCount <= 4096 && sourceCount <= 4096) {
                for (UINT di = 0; di < destinationCount && budget; ++di) {
                    const UINT size = destinationSizes ? destinationSizes[di] : 1;
                    for (UINT index = 0; index < size && budget; ++index, --budget) {
                        while (si < sourceCount && offset >= (sourceSizes ? sourceSizes[si] : 1)) { ++si; offset = 0; }
                        if (si == sourceCount) break;
                        CopyView(destinations[di].ptr + SIZE_T(index) * stride, sources[si].ptr + SIZE_T(offset++) * stride);
                    }
                }
            } else ++g_dropped;
        } catch (...) { Problem("descriptor_ranges_failed"); }
    }
    g_copy(device, destinationCount, destinations, destinationSizes, sourceCount, sources, sourceSizes, type);
}
void RememberPipeline(void* pso, ID3D12RootSignature* root, const D3D12_SHADER_BYTECODE& code, bool compute, UINT targets, const D3D12_SHADER_BYTECODE* vertex = nullptr) {
    if (!pso || !code.pShaderBytecode || !code.BytecodeLength || code.BytecodeLength > 1024 * 1024) return;
    Pipeline pipeline; pipeline.compute = compute; pipeline.rootSignature = reinterpret_cast<uintptr_t>(root); pipeline.renderTargets = targets;
    const auto* bytes = static_cast<const unsigned char*>(code.pShaderBytecode); pipeline.shader.assign(bytes, bytes + code.BytecodeLength);
    if (vertex && vertex->pShaderBytecode && vertex->BytecodeLength <= 1024 * 1024) {
        const auto* data = static_cast<const unsigned char*>(vertex->pShaderBytecode); pipeline.vertex.assign(data, data + vertex->BytecodeLength);
    }
    const auto size = pipeline.shader.size() + pipeline.vertex.size();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shaderBytes + size > 64 * 1024 * 1024 || g_pipelines.size() >= 16384) { ++g_dropped; return; }
    const auto key = reinterpret_cast<uintptr_t>(pso); const auto old = g_pipelines.find(key);
    if (old != g_pipelines.end()) g_shaderBytes -= old->second.shader.size() + old->second.vertex.size();
    pipeline.generation = ++g_pipelineGeneration;
    g_savedShaders.erase(key); // A COM object address may be reused for a different PSO.
    g_shaderBytes += size; g_pipelines[key] = std::move(pipeline); g_exportAllShaders = true;
}
HRESULT STDMETHODCALLTYPE OnPso(Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** result) {
    const auto hr = g_pso(device, desc, iid, result);
    if (g_enabled.load() && SUCCEEDED(hr) && desc && result && !desc->DepthStencilState.DepthEnable && desc->NumRenderTargets <= 2)
        try { RememberPipeline(*result, desc->pRootSignature, desc->PS, false, desc->NumRenderTargets, &desc->VS); } catch (...) { Problem("pso_capture_failed"); }
    return hr;
}
HRESULT STDMETHODCALLTYPE OnComputePso(Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** result) {
    const auto hr = g_computePso(device, desc, iid, result);
    if (g_enabled.load() && SUCCEEDED(hr) && desc && result)
        try { RememberPipeline(*result, desc->pRootSignature, desc->CS, true, 0); } catch (...) { Problem("compute_pso_capture_failed"); }
    return hr;
}
HRESULT STDMETHODCALLTYPE OnCreateSignature(Device* device, UINT node, const void* blob, SIZE_T size, REFIID iid, void** result) {
    const auto hr = g_createSignature(device, node, blob, size, iid, result);
    if (g_enabled.load() && SUCCEEDED(hr) && result && *result && blob && size && size <= 65536) try {
        const auto* bytes = static_cast<const unsigned char*>(blob);
        std::vector<unsigned char> copy(bytes, bytes + size);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_rootSignatures.size() < 1024) { g_rootSignatures[reinterpret_cast<uintptr_t>(*result)] = std::move(copy); g_exportAllShaders = true; } else ++g_dropped;
    } catch (...) { Problem("root_signature_capture_failed"); }
    return hr;
}
State* GetState(List* list) {
    if (g_states.size() >= 1024 && !g_states.count(list)) { ++g_dropped; return nullptr; }
    return &g_states[list];
}
HRESULT STDMETHODCALLTYPE OnReset(List* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    const auto hr = g_reset(list, allocator, pso);
    if (SUCCEEDED(hr) && Tracking()) try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = GetState(list)) { *state = {}; state->resetObserved = true; state->pso = reinterpret_cast<uintptr_t>(pso); } } catch (...) {}
    return hr;
}
void STDMETHODCALLTYPE OnClear(List* list, ID3D12PipelineState* pso) {
    g_clear(list, pso);
    if (Tracking()) try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = GetState(list)) { *state = {}; state->resetObserved = true; state->pso = reinterpret_cast<uintptr_t>(pso); } } catch (...) {}
}
void STDMETHODCALLTYPE OnViewport(List* list, UINT count, const D3D12_VIEWPORT* viewports) {
    if (Tracking()) try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = GetState(list)) { state->viewportCount = count; if (count && viewports) state->viewport = viewports[0]; } } catch (...) {}
    g_viewport(list, count, viewports);
}
void STDMETHODCALLTYPE OnPipeline(List* list, ID3D12PipelineState* pso) {
    if (Tracking()) try { std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = GetState(list)) state->pso = reinterpret_cast<uintptr_t>(pso); } catch (...) {}
    g_pipeline(list, pso);
}
void STDMETHODCALLTYPE OnHeaps(List* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
    if (Tracking() && heaps && count <= 2) try {
        std::array<Heap, 2> info{};
        for (UINT i = 0; i < count; ++i) {
            const auto desc = heaps[i]->GetDesc(); Device* device = nullptr;
            if (SUCCEEDED(heaps[i]->GetDevice(IID_PPV_ARGS(&device)))) {
                info[i] = {heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr, heaps[i]->GetCPUDescriptorHandleForHeapStart().ptr,
                    desc.NumDescriptors, device->GetDescriptorHandleIncrementSize(desc.Type)}; device->Release();
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex); if (auto* state = GetState(list)) { state->heaps = info; state->heapCount = count; }
    } catch (...) {}
    g_heaps(list, count, heaps);
}
void Signature(List* list, ID3D12RootSignature* signature, bool compute) {
    if (!Tracking()) return; std::lock_guard<std::mutex> lock(g_mutex);
    if (auto* state = GetState(list)) {
        auto& roots = compute ? state->compute : state->graphics;
        if (roots.signature != reinterpret_cast<uintptr_t>(signature)) { roots = {}; roots.signature = reinterpret_cast<uintptr_t>(signature); }
    }
}
void STDMETHODCALLTYPE OnComputeSig(List* list, ID3D12RootSignature* signature) { try { Signature(list, signature, true); } catch (...) {} g_computeSig(list, signature); }
void STDMETHODCALLTYPE OnGraphicsSig(List* list, ID3D12RootSignature* signature) { try { Signature(list, signature, false); } catch (...) {} g_graphicsSig(list, signature); }
void RootBinding(List* list, UINT index, UINT64 value, bool compute, bool table) {
    if (!Tracking() || index >= 32) return; std::lock_guard<std::mutex> lock(g_mutex);
    if (auto* state = GetState(list)) { auto& roots = compute ? state->compute : state->graphics; (table ? roots.tables : roots.cbvs)[index] = value; }
}
void STDMETHODCALLTYPE OnComputeTable(List* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) { try { RootBinding(list, index, handle.ptr, true, true); } catch (...) {} g_computeTable(list, index, handle); }
void STDMETHODCALLTYPE OnGraphicsTable(List* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) { try { RootBinding(list, index, handle.ptr, false, true); } catch (...) {} g_graphicsTable(list, index, handle); }
void STDMETHODCALLTYPE OnComputeCbv(List* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) { try { RootBinding(list, index, address, true, false); } catch (...) {} g_computeCbv(list, index, address); }
void STDMETHODCALLTYPE OnGraphicsCbv(List* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) { try { RootBinding(list, index, address, false, false); } catch (...) {} g_graphicsCbv(list, index, address); }
void Constants(List* list, UINT root, UINT count, const void* values, UINT offset, bool compute) {
    if (!Tracking() || root >= 32 || !values || offset >= 64) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto* state = GetState(list)) {
        auto& roots = compute ? state->compute : state->graphics;
        const auto bounded = (std::min)(count, 64 - offset);
        for (UINT i = 0; i < bounded; ++i) { roots.constants[root][offset + i] = static_cast<const UINT*>(values)[i]; roots.constantMasks[root] |= UINT64(1) << (offset + i); }
    }
}
void STDMETHODCALLTYPE OnComputeConstant(List* list, UINT root, UINT value, UINT offset) { try { Constants(list, root, 1, &value, offset, true); } catch (...) {} g_computeConstant(list, root, value, offset); }
void STDMETHODCALLTYPE OnGraphicsConstant(List* list, UINT root, UINT value, UINT offset) { try { Constants(list, root, 1, &value, offset, false); } catch (...) {} g_graphicsConstant(list, root, value, offset); }
void STDMETHODCALLTYPE OnComputeConstants(List* list, UINT root, UINT count, const void* values, UINT offset) { try { Constants(list, root, count, values, offset, true); } catch (...) {} g_computeConstants(list, root, count, values, offset); }
void STDMETHODCALLTYPE OnGraphicsConstants(List* list, UINT root, UINT count, const void* values, UINT offset) { try { Constants(list, root, count, values, offset, false); } catch (...) {} g_graphicsConstants(list, root, count, values, offset); }
void STDMETHODCALLTYPE OnTargets(List* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL consecutive, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    if (Tracking()) try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto* state = GetState(list)) {
            state->rtvCount = count <= 8 && targets ? count : 0;
            for (UINT i = 0; i < state->rtvCount; ++i) state->rtvs[i] = consecutive ? targets[0].ptr + SIZE_T(i) * g_rtvIncrement : targets[i].ptr;
        }
    } catch (...) {}
    g_targets(list, count, targets, consecutive, depth);
}

void RecordDraw(List* list, const char* kind, UINT x, UINT y, UINT z, bool compute) noexcept {
    if (!g_enabled.load() || !g_active.load() || g_insideUpscale) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_states.find(list); if (found == g_states.end()) return;
        const auto& state = found->second; if (!state.resetObserved) return; const auto& roots = compute ? state.compute : state.graphics;
        struct Match { UINT root, offset; View view; unsigned depth; };
        std::vector<Match> matches;
        // Candidate bindings only: inspecting the shader/root layout is still needed to prove an actual read.
        for (UINT root = 0; root < roots.tables.size(); ++root) {
            const auto gpu = roots.tables[root]; if (!gpu) continue;
            for (UINT heapIndex = 0; heapIndex < state.heapCount; ++heapIndex) {
                const auto& heap = state.heaps[heapIndex];
                if (!heap.gpu || !heap.increment || gpu < heap.gpu || (gpu - heap.gpu) % heap.increment) continue;
                const auto baseIndex = (gpu - heap.gpu) / heap.increment; if (baseIndex >= heap.count) continue;
                const auto maximum = (std::min)(UINT64(32), UINT64(heap.count) - baseIndex);
                for (UINT offset = 0; offset < maximum; ++offset) {
                    const auto view = g_views.find(heap.cpu + SIZE_T(baseIndex + offset) * heap.increment);
                    if (view == g_views.end() || view->second.type != 1) continue;
                    const auto watched = g_frontier.find(view->second.resource);
                    if (watched != g_frontier.end() && watched->second < 8) matches.push_back({root, offset, view->second, watched->second});
                }
            }
        }
        if (matches.empty()) return;
        if (g_records.size() >= 512) { ++g_dropped; return; }
        std::ostringstream out;
        out << "{\"event\":\"candidate_pass\",\"frame\":" << g_frames.load() << ",\"kind\":\"" << kind << "\",\"count\":[" << x << ',' << y << ',' << z
            << "],\"list\":\"" << Hex(reinterpret_cast<uintptr_t>(list)) << "\",\"pso\":\"" << Hex(state.pso) << "\",\"root_signature\":\"" << Hex(roots.signature)
            << "\",\"viewport\":[" << state.viewport.TopLeftX << ',' << state.viewport.TopLeftY << ',' << state.viewport.Width << ',' << state.viewport.Height << "],\"candidate_srvs\":[";
        unsigned depth = 8; bool first = true;
        for (const auto& match : matches) {
            depth = (std::min)(depth, match.depth);
            if (!first) out << ','; first = false;
            out << "{\"root\":" << match.root << ",\"table_offset\":" << match.offset << ",\"chain_depth\":" << match.depth << ",\"view\":";
            ViewJson(out, match.view); out << '}';
        }
        out << "],\"render_targets\":["; first = true;
        for (UINT i = 0; !compute && i < state.rtvCount; ++i) {
            const auto view = g_views.find(state.rtvs[i]); if (view == g_views.end()) continue;
            if (!first) out << ','; first = false; ViewJson(out, view->second);
            if (!compute && view->second.resource && g_frontier.size() < 64) {
                auto inserted = g_frontier.emplace(view->second.resource, depth + 1);
                if (!inserted.second) inserted.first->second = (std::min)(inserted.first->second, depth + 1);
            }
        }
        out << "],\"root_cbvs\":{"; first = true;
        for (UINT i = 0; i < roots.cbvs.size(); ++i) if (roots.cbvs[i]) {
            if (!first) out << ','; first = false; out << '"' << i << "\":\"" << Hex(roots.cbvs[i]) << '"';
        }
        out << "},\"root_constants\":["; first = true;
        for (UINT root = 0; root < roots.constantMasks.size(); ++root) for (UINT i = 0; i < 64; ++i) {
            if (!(roots.constantMasks[root] & (UINT64(1) << i))) continue;
            if (!first) out << ','; first = false;
            const auto bits = roots.constants[root][i]; float value; memcpy(&value, &bits, sizeof(value));
            out << "{\"root\":" << root << ",\"word\":" << i << ",\"bits\":" << bits << ",\"as_float\":";
            if (std::isfinite(value)) out << value; else out << "null"; out << '}';
        }
        out << "],\"call_stack\":[";
        void* stack[12]{}; const auto count = CaptureStackBackTrace(1, 12, stack, nullptr);
        for (USHORT i = 0; i < count; ++i) { if (i) out << ','; out << '"' << Hex(reinterpret_cast<uintptr_t>(stack[i])) << '"'; }
        const auto pipeline = g_pipelines.find(state.pso);
        out << "],\"shader_available\":" << (pipeline != g_pipelines.end() ? "true" : "false")
            << ",\"pso_generation\":" << (pipeline != g_pipelines.end() ? pipeline->second.generation : 0) << '}';
        g_records.push_back(out.str()); g_neededShaders.insert(state.pso); g_neededSignatures.insert(roots.signature);
    } catch (...) { Problem("draw_record_failed"); }
}
void STDMETHODCALLTYPE OnDraw(List* list, UINT count, UINT instances, UINT first, UINT firstInstance) { RecordDraw(list, "draw", count, instances, 0, false); g_draw(list, count, instances, first, firstInstance); }
void STDMETHODCALLTYPE OnIndexed(List* list, UINT count, UINT instances, UINT first, INT vertex, UINT firstInstance) { RecordDraw(list, "indexed", count, instances, 0, false); g_indexed(list, count, instances, first, vertex, firstInstance); }
void STDMETHODCALLTYPE OnDispatch(List* list, UINT x, UINT y, UINT z) { RecordDraw(list, "dispatch", x, y, z, true); g_dispatch(list, x, y, z); }

void __cdecl OnUpscale(void* context, List* list, ID3D12Resource* resource, unsigned state, uint64_t jitter) {
    const bool previous = g_insideUpscale; g_insideUpscale = true;
    g_upscale(context, list, resource, state, jitter);
    g_insideUpscale = previous;
    g_frames.fetch_add(1);
    if (!g_enabled.load()) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto remaining = g_armFrames.load();
        const bool active = remaining > 0;
        if (active) g_armFrames.fetch_sub(1);
        g_active.store(active); g_frontier.clear();
        if (active) {
            const auto view = Describe(resource); g_frontier.emplace(view.resource, 0);
            std::ostringstream out; out << "{\"event\":\"upscale_return\",\"frame\":" << g_frames.load() << ",\"tick_ms\":" << GetTickCount64()
                << ",\"list\":\"" << Hex(reinterpret_cast<uintptr_t>(list)) << "\",\"output\":"; ViewJson(out, view); out << '}'; g_records.push_back(out.str());
        }
    } catch (...) { Problem("upscale_record_failed"); }
}

std::wstring Ini(const std::filesystem::path& ini, const std::wstring& key) {
    std::array<wchar_t, 1024> value{}; GetPrivateProfileStringW(L"Capture", key.c_str(), L"", value.data(), static_cast<DWORD>(value.size()), ini.c_str()); return value.data();
}
std::array<unsigned char, 16> Bytes(const std::wstring& value) {
    if (value.size() != 32) throw std::runtime_error("signature length"); std::array<unsigned char, 16> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<unsigned char>(std::stoul(value.substr(i * 2, 2), nullptr, 16)); return out;
}
struct Hook { void* address{}; std::array<unsigned char, 16> patch{}; };
std::vector<Hook> g_hooks;
bool Pin(HMODULE module) { HMODULE pinned{}; return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned) != FALSE; }
bool ExistingProbeJump(void* address, void** target) noexcept;
bool AcceptChainedHook(void* address, const wchar_t* name, const std::filesystem::path& ini) {
    const std::wstring prefix = std::wstring(L"Existing") + name;
    const auto hash = Ini(ini, prefix + L"OwnerSha256");
    if (hash.empty()) return false;
    void* destination{};
    if (!ExistingProbeJump(address, &destination)) return false;
    HMODULE owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(destination), &owner)) return false;
    const auto ownerPath = ModulePath(owner);
    if (_wcsicmp(ownerPath.c_str(), Ini(ini, prefix + L"OwnerPath").c_str())) return false;
    const auto digest = FileSha256(ownerPath);
    if (std::wstring(digest.begin(), digest.end()) != hash) return false;
    if (reinterpret_cast<uintptr_t>(destination) - reinterpret_cast<uintptr_t>(owner) != std::stoull(Ini(ini, prefix + L"TargetRva"), nullptr, 0)) return false;
    const auto expected = Bytes(Ini(ini, prefix + L"TargetBytes"));
    if (memcmp(destination, expected.data(), expected.size()) != 0 || !Pin(owner)) return false;
    return true;
}
template<class Function> void AddHook(void* address, void* replacement, Function& original) {
    void* trampoline{};
    if (MH_CreateHook(address, replacement, &trampoline) != MH_OK) throw std::runtime_error("hook creation failed");
    original = reinterpret_cast<Function>(trampoline);
    g_hooks.push_back({address, {}});
}
void InstallCore(HMODULE core, const std::filesystem::path& ini) {
    const auto digest = FileSha256(ModulePath(core));
    if (digest.empty() || std::wstring(digest.begin(), digest.end()) != Ini(ini, L"CoreSha256")) throw std::runtime_error("Core hash mismatch");
    const auto* base = reinterpret_cast<unsigned char*>(core);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    auto address = [&](const wchar_t* name) -> void* {
        const auto rva = std::stoull(Ini(ini, std::wstring(name) + L"Rva"), nullptr, 0); const auto bytes = Bytes(Ini(ini, std::wstring(name) + L"Bytes"));
        if (rva >= imageSize || imageSize - rva < bytes.size()) throw std::runtime_error("API RVA out of range");
        if (memcmp(base + rva, bytes.data(), bytes.size()) && !AcceptChainedHook(const_cast<unsigned char*>(base + rva), name, ini)) {
            std::string field; for (auto p = name; *p; ++p) field += char(*p); Problem(field.c_str()); throw std::runtime_error("Live API signature mismatch");
        }
        return const_cast<unsigned char*>(base + rva);
    };
    if (!Pin(core) || !Pin(g_self) || MH_Initialize() != MH_OK) throw std::runtime_error("Hook initialization failed");
#define ADD(name, callback, original) AddHook(address(L##name), reinterpret_cast<void*>(&callback), original)
    ADD("CreateGraphicsPipelineState", OnPso, g_pso); ADD("CreateComputePipelineState", OnComputePso, g_computePso);
    ADD("CreateRootSignature", OnCreateSignature, g_createSignature);
    ADD("CreateConstantBufferView", OnConstantView, g_constantView); ADD("CreateShaderResourceView", OnSrv, g_srv);
    ADD("CreateUnorderedAccessView", OnUav, g_uav); ADD("CreateRenderTargetView", OnRtv, g_rtv);
    ADD("CopyDescriptors", OnCopy, g_copy); ADD("CopyDescriptorsSimple", OnCopySimple, g_copySimple);
    ADD("Reset", OnReset, g_reset); ADD("ClearState", OnClear, g_clear); ADD("DrawInstanced", OnDraw, g_draw); ADD("DrawIndexedInstanced", OnIndexed, g_indexed);
    ADD("Dispatch", OnDispatch, g_dispatch); ADD("RSSetViewports", OnViewport, g_viewport); ADD("SetPipelineState", OnPipeline, g_pipeline);
    ADD("SetDescriptorHeaps", OnHeaps, g_heaps); ADD("SetComputeRootSignature", OnComputeSig, g_computeSig);
    ADD("SetGraphicsRootSignature", OnGraphicsSig, g_graphicsSig); ADD("SetComputeRootDescriptorTable", OnComputeTable, g_computeTable);
    ADD("SetGraphicsRootDescriptorTable", OnGraphicsTable, g_graphicsTable); ADD("SetComputeRootConstantBufferView", OnComputeCbv, g_computeCbv);
    ADD("SetGraphicsRootConstantBufferView", OnGraphicsCbv, g_graphicsCbv); ADD("OMSetRenderTargets", OnTargets, g_targets);
    ADD("SetComputeRoot32BitConstant", OnComputeConstant, g_computeConstant); ADD("SetGraphicsRoot32BitConstant", OnGraphicsConstant, g_graphicsConstant);
    ADD("SetComputeRoot32BitConstants", OnComputeConstants, g_computeConstants); ADD("SetGraphicsRoot32BitConstants", OnGraphicsConstants, g_graphicsConstants);
#undef ADD
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { g_partialEnable = true; throw std::runtime_error("Hook enable failed; pass-through callbacks may remain pinned"); }
    for (auto& hook : g_hooks) memcpy(hook.patch.data(), hook.address, hook.patch.size());
    g_enabled.store(true); Log("{\"event\":\"core_attached\",\"version\":4,\"changes_gpu_parameters\":false,\"validated_existing_hooks_preserved\":true,\"color_render_target_views_only\":true}");
}
bool ExistingProbeJump(void* address, void** target) noexcept {
    __try {
        const auto* entry = static_cast<unsigned char*>(address); if (entry[0] != 0xe9) return false;
        const auto* relay = entry + 5 + *reinterpret_cast<const int32_t*>(entry + 1);
        if (relay[0] != 0xff || relay[1] != 0x25) return false;
        *target = *reinterpret_cast<void* const*>(relay + 6 + *reinterpret_cast<const int32_t*>(relay + 2)); return *target != nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool InstallStage(HMODULE mhwss) {
    auto* address = reinterpret_cast<unsigned char*>(mhwss) + 0x117a20; void* destination{};
    if (!ExistingProbeJump(address, &destination)) return false;
    HMODULE owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(destination), &owner) || _wcsicmp(ModulePath(owner).filename().c_str(), L"MhwSrProbe.dll")) return false;
    if (FileSha256(ModulePath(owner)) != kParameterProbeHash || FileSha256(ModulePath(mhwss)) != kMhwssHash || !Pin(owner) || !Pin(mhwss))
        throw std::runtime_error("MHWSS/probe hash mismatch");
    AddHook(address, reinterpret_cast<void*>(&OnUpscale), g_upscale);
    if (MH_EnableHook(address) != MH_OK) throw std::runtime_error("Stage hook enable failed");
    memcpy(g_hooks.back().patch.data(), address, 16);
    Log("{\"event\":\"stage_attached\",\"existing_parameter_probe_preserved\":true}"); return true;
}
void Drain() {
    std::vector<std::string> records; std::map<uintptr_t, Pipeline> shaders; std::map<uintptr_t, std::vector<unsigned char>> signatures;
    {
        std::lock_guard<std::mutex> lock(g_mutex); records.swap(g_records);
        if (g_exportAllShaders) {
            for (const auto& entry : g_pipelines) g_neededShaders.insert(entry.first);
            for (const auto& entry : g_rootSignatures) g_neededSignatures.insert(entry.first);
            g_exportAllShaders = false;
        }
        for (const auto pso : g_neededShaders) {
            const auto found = g_pipelines.find(pso);
            if (found != g_pipelines.end() && !g_savedShaders.count(pso)) { shaders.emplace(pso, found->second); g_savedShaders.insert(pso); }
        }
        g_neededShaders.clear();
        for (const auto root : g_neededSignatures) {
            const auto found = g_rootSignatures.find(root);
            if (found != g_rootSignatures.end() && !g_savedSignatures.count(root)) { signatures.emplace(root, found->second); g_savedSignatures.insert(root); }
        }
        g_neededSignatures.clear();
    }
    for (const auto& record : records) Log(record);
    const auto shaderFolder = g_folder / (L"MhwPassShaders-" + std::to_wstring(GetCurrentProcessId()));
    for (const auto& [address, pipeline] : shaders) {
        std::filesystem::create_directories(shaderFolder);
        const auto prefix = Hex(address) + "-g" + std::to_string(pipeline.generation);
        const auto name = prefix + (pipeline.compute ? "-cs.dxbc" : "-ps.dxbc");
        std::ofstream file(shaderFolder / name, std::ios::binary); file.write(reinterpret_cast<const char*>(pipeline.shader.data()), pipeline.shader.size()); file.close();
        if (!pipeline.vertex.empty()) {
            std::ofstream vertex(shaderFolder / (prefix + "-vs.dxbc"), std::ios::binary);
            vertex.write(reinterpret_cast<const char*>(pipeline.vertex.data()), pipeline.vertex.size()); vertex.close();
        }
        Log("{\"event\":\"shader_saved\",\"pso\":\"" + Hex(address) + "\",\"file\":\"" + name + "\",\"bytes\":" + std::to_string(pipeline.shader.size()) +
            ",\"vertex_bytes\":" + std::to_string(pipeline.vertex.size()) + ",\"pso_generation\":" + std::to_string(pipeline.generation) +
            ",\"root_signature\":\"" + Hex(pipeline.rootSignature) + "\"}");
    }
    for (const auto& [address, signature] : signatures) {
        std::filesystem::create_directories(shaderFolder); const auto name = "root-" + Hex(address) + ".dxbc";
        std::ofstream file(shaderFolder / name, std::ios::binary); file.write(reinterpret_cast<const char*>(signature.data()), signature.size()); file.close();
        Log("{\"event\":\"root_signature_saved\",\"identity\":\"" + Hex(address) + "\",\"file\":\"" + name + "\"}");
    }
}
std::string ReadSetting(const std::filesystem::path& graphics) {
    std::ifstream file(graphics); std::string line;
    while (std::getline(file, line)) if (line.rfind("ResolutionScaling=", 0) == 0) {
        auto value = line.substr(18); while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (value == "High" || value == "Low" || value == "Mid" || value == "Medium") return value;
    }
    return {};
}
void Stop() noexcept {
    g_enabled.store(false); g_active.store(false); g_armFrames.store(0);
    bool restored = !g_partialEnable;
    for (auto& hook : g_hooks) {
        if (hook.patch[0] == 0) continue; // Created but never enabled.
        if (memcmp(hook.address, hook.patch.data(), 16) != 0 || MH_DisableHook(hook.address) != MH_OK) restored = false;
    }
    try { Drain(); Log(std::string("{\"event\":\"stopped\",\"owned_hooks_restored\":") + (restored ? "true" : "false") + "}"); } catch (...) {}
}
DWORD WINAPI Worker(void*) {
    HANDLE controls[2]{};
    try {
        const auto executable = ModulePath(nullptr);
        if (_wcsicmp(executable.filename().c_str(), L"MonsterHunterWorld.exe")) return 0;
        g_folder = ModulePath(g_self).parent_path();
        g_log.open(g_folder / (L"MhwPassProbe-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl"), std::ios::app);
        const auto ini = g_folder / L"MhwPassProbe.ini";
        if (GetPrivateProfileIntW(L"Capture", L"Enabled", 0, ini.c_str()) != 1 || FileSha256(executable) != kGameHash) return 0;
        HMODULE core{};
        for (unsigned i = 0; i < 3600; ++i) { core = GetModuleHandleW(L"D3D12Core.dll"); if (core) break; Sleep(50); }
        if (!core) throw std::runtime_error("D3D12Core was not loaded");
        InstallCore(core, ini);
        bool stage = false;
        for (unsigned i = 0; i < 1800 && !stage; ++i) {
            const auto mhwss = GetModuleHandleW(L"MHWSS.dll"); if (mhwss) stage = InstallStage(mhwss); if (!stage) Sleep(100);
        }
        if (!stage) throw std::runtime_error("Existing parameter probe stage was not ready");
        const auto prefix = L"Local\\MhwPassProbe." + std::to_wstring(GetCurrentProcessId());
        controls[0] = CreateEventW(nullptr, TRUE, FALSE, (prefix + L".Shutdown").c_str());
        controls[1] = CreateEventW(nullptr, FALSE, FALSE, (prefix + L".Capture").c_str());
        if (!controls[0] || !controls[1]) throw std::runtime_error("Control events unavailable");
        const auto graphics = executable.parent_path() / L"graphics_option.ini";
        std::string setting; uint64_t changed = GetTickCount64(), nextStatus = changed + 10000; bool pending = true; unsigned sessions = 0;
        while (true) {
            const auto wait = WaitForMultipleObjects(2, controls, FALSE, 250); if (wait == WAIT_OBJECT_0) break;
            Drain(); const auto now = GetTickCount64(); const auto current = ReadSetting(graphics);
            if (!current.empty() && current != setting) { setting = current; changed = now; pending = true;
                Log("{\"event\":\"setting_marker\",\"setting\":\"" + setting + "\",\"tick_ms\":" + std::to_string(now) + "}"); }
            if (wait == WAIT_OBJECT_0 + 1) { changed = now; pending = true; }
            if (pending && now - changed >= 4000 && sessions < 12 && !g_active.load() && !g_armFrames.load()) {
                { std::lock_guard<std::mutex> lock(g_mutex); g_states.clear(); g_exportAllShaders = true; }
                g_armFrames.store(4); pending = false; ++sessions;
                Log("{\"event\":\"armed\",\"session\":" + std::to_string(sessions) + ",\"setting\":\"" + setting + "\"}");
            }
            if (now >= nextStatus) {
                std::lock_guard<std::mutex> lock(g_mutex);
                Log("{\"event\":\"status\",\"upscale_calls\":" + std::to_string(g_frames.load()) + ",\"views\":" + std::to_string(g_views.size()) +
                    ",\"pipelines\":" + std::to_string(g_pipelines.size()) + ",\"root_signatures\":" + std::to_string(g_rootSignatures.size()) + ",\"dropped\":" + std::to_string(g_dropped) + "}"); nextStatus = now + 10000;
            }
        }
    } catch (const std::exception& error) { Problem(error.what()); }
    Stop(); for (auto event : controls) if (event) CloseHandle(event); return 0;
}
void StartWorker() {
    if (InterlockedCompareExchange(&g_started, 1, 0)) return;
    const auto thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread); else InterlockedExchange(&g_started, 0);
}
}
extern "C" __declspec(dllexport) void Initialize() { StartWorker(); }
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = module; StartWorker(); }
    return TRUE;
}
