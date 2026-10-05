// Standalone GPU replay of captured MHW scene buffers. Never opens the game process.
// This software contains source code provided by NVIDIA Corporation (SDK inline helpers).
#include "../readback/CaptureCommon.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <array>
#include <iostream>
#include <mutex>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
std::mutex logMutex;
std::ofstream ngxLog;
void NVSDK_CONV LogCallback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (ngxLog) { ngxLog << message << '\n'; ngxLog.flush(); }
}
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) { std::ostringstream out; out << operation << " HRESULT=0x" << std::hex << unsigned(result); throw std::runtime_error(out.str()); }
}
void Ngx(NVSDK_NGX_Result result, const char* operation) {
    std::cout << operation << ": 0x" << std::hex << unsigned(result) << std::dec << '\n' << std::flush;
    if (NVSDK_NGX_FAILED(result)) { std::ostringstream out; out << operation << " NGX=0x" << std::hex << unsigned(result); throw std::runtime_error(out.str()); }
}
std::wstring ReadIni(const std::filesystem::path& ini, const wchar_t* name) {
    std::array<wchar_t, 32768> value{};
    GetPrivateProfileStringW(L"Replay", name, L"", value.data(), static_cast<DWORD>(value.size()), ini.c_str());
    if (!value[0]) throw std::runtime_error("Missing replay setting");
    return value.data();
}
UINT Dimension(const std::filesystem::path& ini, const wchar_t* name) {
    const auto value = std::stoul(ReadIni(ini, name));
    if (value < 32 || value > 8192) throw std::runtime_error("Invalid dimension");
    return static_cast<UINT>(value);
}
D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1; return heap;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size; desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto heap = Heap(type); ComPtr<ID3D12Resource> buffer;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)), "Create buffer"); return buffer;
}
ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, bool output) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = format;
    if (output) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT); ComPtr<ID3D12Resource> texture;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "Create texture");
    return texture;
}
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after; list->ResourceBarrier(1, &barrier);
}
struct Upload { ComPtr<ID3D12Resource> texture, buffer; };
Upload LoadInput(ID3D12Device* device, ID3D12GraphicsCommandList* list, const std::filesystem::path& source,
    UINT sourceWidth, UINT sourceHeight, UINT renderWidth, UINT renderHeight, DXGI_FORMAT format) {
    Upload result; result.texture = Texture(device, renderWidth, renderHeight, format, false);
    const auto desc = result.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT rows = 0; UINT64 rowBytes = 0, bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowBytes, &bytes);
    result.buffer = Buffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    std::ifstream input(source, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(uint64_t(sourceWidth) * sourceHeight * 4)) throw std::runtime_error("Input size mismatch");
    void* mapped = nullptr; D3D12_RANGE noRead{0, 0}; Check(result.buffer->Map(0, &noRead, &mapped), "Map upload");
    auto* destination = static_cast<unsigned char*>(mapped) + layout.Offset;
    for (UINT y = 0; y < renderHeight; ++y) {
        input.seekg(static_cast<std::streamoff>(uint64_t(y) * sourceWidth * 4));
        input.read(reinterpret_cast<char*>(destination + size_t(y) * layout.Footprint.RowPitch), size_t(renderWidth) * 4);
        if (!input) { result.buffer->Unmap(0, nullptr); throw std::runtime_error("Input read failed"); }
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(bytes)}; result.buffer->Unmap(0, &written);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = result.buffer.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = layout;
    to.pResource = result.texture.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(list, result.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::wcerr << L"Usage: MhwSrReplay <replay.ini> <output directory>\n"; return 2; }
    const auto ini = std::filesystem::absolute(argv[1]), outputDirectory = std::filesystem::absolute(argv[2]);
    try {
        std::filesystem::create_directories(outputDirectory);
        ngxLog.open(outputDirectory / "ngx.log", std::ios::trunc);
        const std::filesystem::path inputDirectory = ReadIni(ini, L"InputDirectory"), runtimeDirectory = ReadIni(ini, L"RuntimeDirectory");
        const auto expectedHash = ReadIni(ini, L"ExpectedRuntimeSha256");
        const auto diskHash = FileSha256(runtimeDirectory / L"nvngx_dlss.dll");
        if (std::wstring(diskHash.begin(), diskHash.end()) != expectedHash) throw std::runtime_error("Runtime hash mismatch");
        const UINT sourceWidth = Dimension(ini, L"SourceWidth"), sourceHeight = Dimension(ini, L"SourceHeight");
        const UINT renderWidth = Dimension(ini, L"RenderWidth"), renderHeight = Dimension(ini, L"RenderHeight");
        const UINT outputWidth = Dimension(ini, L"OutputWidth"), outputHeight = Dimension(ini, L"OutputHeight");
        if (renderWidth > sourceWidth || renderHeight > sourceHeight || renderWidth >= outputWidth || renderHeight >= outputHeight)
            throw std::runtime_error("Replay requires a smaller real input and a larger output");
        const auto quality = static_cast<NVSDK_NGX_PerfQuality_Value>(std::stoi(ReadIni(ini, L"Quality")));
        const int preset = std::stoi(ReadIni(ini, L"Preset"));
        const float jitterX = std::stof(ReadIni(ini, L"JitterX")), jitterY = std::stof(ReadIni(ini, L"JitterY"));
        const float mvX = std::stof(ReadIni(ini, L"MVScaleX")), mvY = std::stof(ReadIni(ini, L"MVScaleY"));
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<ID3D12Device> device; DXGI_ADAPTER_DESC1 adapterDesc{};
        for (UINT i = 0; !device; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            adapter->GetDesc1(&adapterDesc);
            if (adapterDesc.VendorId == 0x10de && !(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        }
        if (!device || device->GetNodeCount() != 1) throw std::runtime_error("Expected one-node NVIDIA D3D12 device");
        std::wcout << L"GPU: " << adapterDesc.Description << L"\n";
        ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create queue");
        ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create allocator");
        ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create list");
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
        const HANDLE complete = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!complete) throw std::runtime_error("Create event failed");
        UINT64 submission = 0;
        auto flush = [&]() {
            Check(list->Close(), "Close list"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++submission), "Signal fence");
            Check(fence->SetEventOnCompletion(submission, complete), "Fence event");
            if (WaitForSingleObject(complete, 30000) != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX)
                throw std::runtime_error("GPU completion unavailable");
        };
        const auto runtimePath = runtimeDirectory.wstring(); const wchar_t* paths[]{runtimePath.c_str()};
        NVSDK_NGX_FeatureCommonInfo common{}; common.PathListInfo.Path = paths; common.PathListInfo.Length = 1;
        common.LoggingInfo.LoggingCallback = &LogCallback; common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        common.LoggingInfo.DisableOtherLoggingSinks = true;
        // Identifier for this standalone research application; not a borrowed game AppID.
        Ngx(NVSDK_NGX_D3D12_Init_with_ProjectID("F92B42F5-4D13-4A44-B676-7DA8323E9120", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "MhwSrReplay-0.1", outputDirectory.c_str(), device.Get(), &common), "NGX Init");
        NVSDK_NGX_Parameter* caps = nullptr;
        Ngx(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps), "GetCapabilityParameters");
        int available = 0; Ngx(caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available), "SuperSampling available query");
        if (!available) throw std::runtime_error("SuperSampling is unavailable; inspect ngx.log");
        const auto runtimeModule = GetModuleHandleW(L"nvngx_dlss.dll");
        const auto loadedPath = ModulePath(runtimeModule);
        if (!runtimeModule || FileSha256(loadedPath) != diskHash) throw std::runtime_error("Loaded DLSS module differs from specified runtime");
        std::wcout << L"Loaded DLSS: " << loadedPath.c_str() << L"\n";
        std::ofstream settings(outputDirectory / "optimal-settings.json"); settings << "{\"output\":[" << outputWidth << ',' << outputHeight << "],\"modes\":[";
        bool first = true; bool allowed = false;
        for (int mode : {2, 1, 0, 3}) {
            unsigned width = 0, height = 0, maximumWidth = 0, maximumHeight = 0, minimumWidth = 0, minimumHeight = 0; float sharpness = 0;
            const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(caps, outputWidth, outputHeight, static_cast<NVSDK_NGX_PerfQuality_Value>(mode),
                &width, &height, &maximumWidth, &maximumHeight, &minimumWidth, &minimumHeight, &sharpness);
            if (!first) settings << ','; first = false;
            settings << "{\"quality\":" << mode << ",\"result\":" << unsigned(result) << ",\"optimal\":[" << width << ',' << height
                << "],\"minimum\":[" << minimumWidth << ',' << minimumHeight << "],\"maximum\":[" << maximumWidth << ',' << maximumHeight << "]}";
            if (mode == int(quality) && NVSDK_NGX_SUCCEED(result)) allowed = renderWidth >= minimumWidth && renderWidth <= maximumWidth && renderHeight >= minimumHeight && renderHeight <= maximumHeight;
        }
        settings << "]}\n"; settings.close();
        if (!allowed) throw std::runtime_error("Captured render size is outside the runtime's supported interval for this mode");
        NVSDK_NGX_Parameter* params = nullptr; Ngx(NVSDK_NGX_D3D12_AllocateParameters(&params), "AllocateParameters");
        params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
        params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
        params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
        params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = renderWidth; create.Feature.InHeight = renderHeight;
        create.Feature.InTargetWidth = outputWidth; create.Feature.InTargetHeight = outputHeight; create.Feature.InPerfQualityValue = quality;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        NVSDK_NGX_Handle* feature = nullptr;
        Ngx(NGX_D3D12_CREATE_DLSS_EXT(list.Get(), 1, 1, &feature, params, &create), "Create DLSS SR");
        flush(); Check(allocator->Reset(), "Reset allocator"); Check(list->Reset(allocator.Get(), nullptr), "Reset list");
        std::array<Upload, 3> inputs{
            LoadInput(device.Get(), list.Get(), inputDirectory / "Color.bin", sourceWidth, sourceHeight, renderWidth, renderHeight, DXGI_FORMAT_R11G11B10_FLOAT),
            LoadInput(device.Get(), list.Get(), inputDirectory / "Depth.bin", sourceWidth, sourceHeight, renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT),
            LoadInput(device.Get(), list.Get(), inputDirectory / "MotionVectors.bin", sourceWidth, sourceHeight, renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT)};
        auto output = Texture(device.Get(), outputWidth, outputHeight, DXGI_FORMAT_R11G11B10_FLOAT, true);
        NVSDK_NGX_D3D12_DLSS_Eval_Params evaluate{};
        evaluate.Feature.pInColor = inputs[0].texture.Get(); evaluate.pInDepth = inputs[1].texture.Get(); evaluate.pInMotionVectors = inputs[2].texture.Get();
        evaluate.Feature.pInOutput = output.Get(); evaluate.InRenderSubrectDimensions = {renderWidth, renderHeight}; evaluate.InReset = 1;
        evaluate.InJitterOffsetX = jitterX; evaluate.InJitterOffsetY = jitterY; evaluate.InMVScaleX = mvX; evaluate.InMVScaleY = mvY;
        evaluate.InPreExposure = evaluate.InExposureScale = 1;
        const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(list.Get(), feature, params, &evaluate); Ngx(result, "Evaluate DLSS SR");
        Barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto outputDesc = output->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT rows = 0; UINT64 rowBytes = 0, bytes = 0;
        device->GetCopyableFootprints(&outputDesc, 0, 1, 0, &layout, &rows, &rowBytes, &bytes);
        auto readback = Buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = output.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = layout;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr); flush();
        void* mapped = nullptr; D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)}; Check(readback->Map(0, &readRange, &mapped), "Map result");
        std::ofstream binary(outputDirectory / "Output.bin", std::ios::binary); binary.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(bytes)); binary.close();
        D3D12_RANGE noWrite{0, 0}; readback->Unmap(0, &noWrite); if (!binary) throw std::runtime_error("Output save failed");
        std::ofstream metadata(outputDirectory / "metadata.json");
        metadata << "{\"standalone_replay\":true,\"single_reset_frame\":true,\"gpu_fence_completed\":true,\"game_modified\":false,\"fps_measured\":false,"
            << "\"ResolutionScaling\":\"LowReplay\",\"evaluate_result\":" << unsigned(result) << ",\"quality\":" << int(quality) << ",\"requested_preset\":" << preset
            << ",\"runtime_sha256\":\"" << diskHash << "\",\"render_size\":[" << renderWidth << ',' << renderHeight << "],\"output_size\":[" << outputWidth << ',' << outputHeight
            << "],\"jitter\":[" << jitterX << ',' << jitterY << "],\"mv_scale\":[" << mvX << ',' << mvY << "],\"textures\":[{\"name\":\"Output\",\"file\":\"Output.bin\",\"width\":"
            << outputWidth << ",\"height\":" << outputHeight << ",\"format\":26,\"offset\":" << layout.Offset << ",\"row_pitch\":" << layout.Footprint.RowPitch
            << ",\"rows\":" << rows << ",\"row_bytes\":" << rowBytes << ",\"total_bytes\":" << bytes << "}]}\n";
        metadata.close(); if (!metadata) throw std::runtime_error("Metadata save failed");
        Ngx(NVSDK_NGX_D3D12_ReleaseFeature(feature), "ReleaseFeature");
        Ngx(NVSDK_NGX_D3D12_DestroyParameters(params), "DestroyParameters");
        Ngx(NVSDK_NGX_D3D12_DestroyParameters(caps), "DestroyCapabilities");
        Ngx(NVSDK_NGX_D3D12_Shutdown1(device.Get()), "Shutdown");
        CloseHandle(complete);
        std::cout << "Saved one real-low-input SR replay. This is not an in-game integration or a temporal/performance validation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::ofstream failure(outputDirectory / "failure.txt"); failure << error.what() << '\n';
        return 1;
    }
}
