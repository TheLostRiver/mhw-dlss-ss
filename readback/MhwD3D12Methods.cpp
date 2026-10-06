// External helper: creates its own NVIDIA device, never enters or modifies the game.
#include "CaptureCommon.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <iostream>
using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::wcerr << L"Usage: MhwD3D12Methods <game PID> <output ini>\n"; return 2; }
    wchar_t* end = nullptr;
    const auto processId = wcstoul(argv[1], &end, 10);
    if (!end || *end) return 2; // Zero is offline calibration for the next matching game process.
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    DXGI_ADAPTER_DESC1 description{};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapter->GetDesc1(&description);
        if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) break;
        adapter.Reset();
    }
    if (!device) { std::wcerr << L"No supported NVIDIA adapter.\n"; return 1; }
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return 1;
    list->Close();
    const auto core = GetModuleHandleW(L"D3D12Core.dll");
    if (!core) return 1;
    std::ofstream output(std::filesystem::path(argv[2]), std::ios::trunc);
    output << "[Capture]\nPid=" << processId << "\nCoreSha256=" << FileSha256(ModulePath(core))
        << "\nAdapterLuidLow=" << description.AdapterLuid.LowPart
        << "\nAdapterLuidHigh=" << description.AdapterLuid.HighPart << '\n';
    auto method = [&](const char* name, void* object, size_t index) {
        const auto address = (*static_cast<void***>(object))[index];
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &owner) || owner != core) return false;
        output << name << "Rva=0x" << std::hex << reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(core)
            << std::dec << '\n' << name << "Bytes=" << HexBytes(address, 16) << '\n';
        return true;
    };
    if (!method("Reset", list.Get(), 10) || !method("ResourceBarrier", list.Get(), 26) ||
        !method("ExecuteCommandLists", queue.Get(), 10)) return 1;
    if (!method("CreateCommittedResource", device.Get(), 27) ||
        !method("CreateGraphicsPipelineState", device.Get(), 10) || !method("CreateComputePipelineState", device.Get(), 11) ||
        !method("CreateRootSignature", device.Get(), 16) || !method("CreateConstantBufferView", device.Get(), 17) || !method("CreateShaderResourceView", device.Get(), 18) ||
        !method("CreateUnorderedAccessView", device.Get(), 19) || !method("CreateRenderTargetView", device.Get(), 20) ||
        !method("CopyDescriptors", device.Get(), 23) || !method("CopyDescriptorsSimple", device.Get(), 24) ||
        !method("ClearState", list.Get(), 11) || !method("DrawInstanced", list.Get(), 12) || !method("DrawIndexedInstanced", list.Get(), 13) ||
        !method("Dispatch", list.Get(), 14) || !method("RSSetViewports", list.Get(), 21) ||
        !method("SetPipelineState", list.Get(), 25) || !method("SetDescriptorHeaps", list.Get(), 28) ||
        !method("SetComputeRootSignature", list.Get(), 29) || !method("SetGraphicsRootSignature", list.Get(), 30) ||
        !method("SetComputeRootDescriptorTable", list.Get(), 31) || !method("SetGraphicsRootDescriptorTable", list.Get(), 32) ||
        !method("SetComputeRoot32BitConstant", list.Get(), 33) || !method("SetGraphicsRoot32BitConstant", list.Get(), 34) ||
        !method("SetComputeRoot32BitConstants", list.Get(), 35) || !method("SetGraphicsRoot32BitConstants", list.Get(), 36) ||
        !method("SetComputeRootConstantBufferView", list.Get(), 37) || !method("SetGraphicsRootConstantBufferView", list.Get(), 38) ||
        !method("OMSetRenderTargets", list.Get(), 46)) return 1;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=4096;buffer.Height=1;
    buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&resource)))||
        !method("GetGPUVirtualAddress",resource.Get(),11))return 1;
    output.close();
    if (!output) return 1;
    std::wcout << L"Adapter: " << description.Description << L"\nWritten: " << argv[2] << L"\n";
    return 0;
}
