// The measured draw stack leads to game 0x259d670 -> 0x259e470.
// This latter function constructs the vertex view from input[0]->backend:
// backend +0x10 size, +0x20 ID3D12Resource, +0x28 pool offset, +0x40 GPU VA.
// Resource creation at 0x24e89a5/0x24e89ef establishes the COM pointer field.
using EngineVertices=uintptr_t(__fastcall*)(void*,void*,void*);
EngineVertices g_engineVertices{};
std::atomic<uint64_t> g_engineVertexCandidates{0},g_engineVertexReferences{0};
struct EngineVertexData {ID3D12GraphicsCommandList* list{};ID3D12Resource* resource{};UINT64 base=0;unsigned bytes=0,offset=0;};
bool ReferenceEngineVertex(void* backend,void* input,EngineVertexData* out) noexcept {
    __try {
        if(!backend||!input)return false;
        const auto* view=static_cast<unsigned char*>(input);
        if(view[0x20]!=16)return false;
        auto* wrapper=*reinterpret_cast<unsigned char* const*>(view);if(!wrapper)return false;
        auto* buffer=*reinterpret_cast<unsigned char**>(wrapper+0x60);if(!buffer)return false;
        memcpy(&out->bytes,buffer+0x10,4);memcpy(&out->base,buffer+0x40,8);memcpy(&out->offset,view+0x24,4);
        // Exclude pooled suballocations: +0x20 may be null in that other path.
        if(out->bytes!=0x400000||out->offset>out->bytes-48||*reinterpret_cast<UINT64*>(buffer+0x28))return false;
        auto* state=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(backend)+8);
        if(!state)return false;out->list=*reinterpret_cast<ID3D12GraphicsCommandList**>(state+0x20);
        // MHWSS's command-list wrapper forwards IASetVertexBuffers through +8.
        // Normalize only its exact, build-verified vtable entry; the bridge API
        // callbacks observe the underlying Core command list instead.
        if(out->list&&(*reinterpret_cast<void***>(out->list))[44]==g_mhwss+0x1578d0)
            out->list=*reinterpret_cast<ID3D12GraphicsCommandList**>(reinterpret_cast<unsigned char*>(out->list)+8);
        auto* resource=*reinterpret_cast<ID3D12Resource**>(buffer+0x20);if(!resource)return false;
        const auto* table=*reinterpret_cast<void***>(resource);
        const auto* core=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"D3D12Core.dll"));
        if(table[11]!=core+0x10d230)return false;
        resource->AddRef();out->resource=resource;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uintptr_t __fastcall OnEngineVertices(void* backend,void* state,void* input) {
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()) {
        EngineVertexData data{};
        if(ReferenceEngineVertex(backend,input,&data)) {
            ++g_engineVertexCandidates;
            try {
                bool relevant=false;
                {std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(data.list);
                    relevant=found!=g_bridgeLists.end()&&found->second.taaSerial&&found->second.postTaaOps<32;}
                if(relevant) {
                    ++g_engineVertexReferences;
                    struct Guard {Guard(){g_insideVertexDiscovery=true;}~Guard(){g_insideVertexDiscovery=false;}} guard;
                    const auto base=data.resource->GetGPUVirtualAddress();const auto desc=data.resource->GetDesc();
                    if(base==data.base&&desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&desc.Width==data.bytes)
                        ObserveVertexResource(data.resource,base);
                }
            }catch(...){++g_traceDrops;}
            data.resource->Release();
        }
    }
    return g_engineVertices(backend,state,input);
}
