// Resources come from observed D3D12 calls or the verified engine binding.
// Never infer COM objects from memory scans. Keep a reference and a nested Map
// for each accepted CPU-visible 4 MiB buffer until the bounded capture ends.
using GpuAddress=UINT64(STDMETHODCALLTYPE*)(ID3D12Resource*);
GpuAddress g_gpuAddress{};
std::mutex g_vertexReadMutex;
struct VertexArena {
    ID3D12Resource* resource{};const unsigned char* mapped{};UINT64 base=0,bytes=0;
};
std::array<VertexArena,16> g_vertexArenas{};
size_t g_vertexArenaCount=0;
std::set<ID3D12Resource*> g_seenAddressResources;
std::set<ID3D12Resource*> g_seenCopySources;
std::atomic<uint64_t> g_gpuAddressCalls{0},g_vertexReadHits{0},g_vertexReadMisses{0};
using BufferCopy=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64);
BufferCopy g_bufferCopy{};
using ResourceMap=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
ResourceMap g_resourceMap{};
std::atomic<uint64_t> g_resourceMapCalls{0};
std::atomic<uint64_t> g_bufferCopyCalls{0},g_vertexBufferCopies{0};
thread_local bool g_insideVertexDiscovery=false;

void ObserveVertexResource(ID3D12Resource* resource,UINT64 base,bool copySource=false) {
    std::lock_guard<std::mutex> lock(g_vertexReadMutex);
    // Recheck after locking: Stop may have started while this call was pending.
    if(!g_ready.load()||g_done.load()||!base||g_vertexArenaCount>=g_vertexArenas.size())return;
    for(size_t i=0;i<g_vertexArenaCount;++i)if(g_vertexArenas[i].resource==resource)return;
    auto& seen=copySource?g_seenCopySources:g_seenAddressResources;
    if(seen.count(resource)||seen.size()>=1024)return;
    seen.insert(resource);
    const auto desc=resource->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||(!copySource&&desc.Width!=0x400000)||desc.Width<48||desc.Width>0x4000000)return;
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    const auto properties=resource->GetHeapProperties(&heap,&flags);
    // Reads from upload/write-combine memory are slow. Limit this diagnostic to
    // the observed buffer size and 48 bytes per matching triangle draw.
    const bool readable=SUCCEEDED(properties)&&(heap.Type==D3D12_HEAP_TYPE_UPLOAD||
        (heap.Type==D3D12_HEAP_TYPE_CUSTOM&&(heap.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_BACK||
         heap.CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)));
    HRESULT mappedResult=E_ACCESSDENIED;void* mapped=nullptr;
    if(readable) {
        resource->AddRef();D3D12_RANGE range{0,static_cast<SIZE_T>(desc.Width)};
        mappedResult=resource->Map(0,&range,&mapped);
        if(SUCCEEDED(mappedResult)&&mapped)g_vertexArenas[g_vertexArenaCount++]={resource,static_cast<const unsigned char*>(mapped),base,desc.Width};
        else {if(SUCCEEDED(mappedResult)){D3D12_RANGE noWrite{0,0};resource->Unmap(0,&noWrite);}resource->Release();}
    }
    std::ostringstream out;
    out<<"{\"event\":\"vertex_buffer_observed\",\"resource\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(resource)
       <<"\",\"gpu_base\":\"0x"<<base<<std::dec<<"\",\"bytes\":"<<desc.Width<<",\"heap_type\":"<<heap.Type
       <<",\"cpu_page_property\":"<<heap.CPUPageProperty<<",\"heap_result\":"<<static_cast<int32_t>(properties)
       <<",\"map_result\":"<<static_cast<int32_t>(mappedResult)<<",\"mapped\":"<<(mapped?"true":"false")<<",\"writes_buffer\":false}";
    SaveBridge(out.str());
}
UINT64 STDMETHODCALLTYPE OnBridgeGpuAddress(ID3D12Resource* resource) {
    const auto base=g_gpuAddress(resource);
    if(g_ready.load()&&!g_done.load()&&!g_insideVertexDiscovery) {
        ++g_gpuAddressCalls;
        struct Guard {Guard(){g_insideVertexDiscovery=true;}~Guard(){g_insideVertexDiscovery=false;}} guard;
        try{ObserveVertexResource(resource,base);}catch(...){++g_traceDrops;}
    }
    return base;
}
HRESULT STDMETHODCALLTYPE OnBridgeResourceMap(ID3D12Resource* resource,UINT subresource,const D3D12_RANGE* range,void** data) {
    const auto result=g_resourceMap(resource,subresource,range,data);
    if(SUCCEEDED(result)&&g_ready.load()&&!g_done.load()&&!g_insideVertexDiscovery) {
        ++g_resourceMapCalls;
        struct Guard {Guard(){g_insideVertexDiscovery=true;}~Guard(){g_insideVertexDiscovery=false;}} guard;
        try{ObserveVertexResource(resource,resource->GetGPUVirtualAddress());}catch(...){++g_traceDrops;}
    }
    return result;
}
void STDMETHODCALLTYPE OnBridgeBufferCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,UINT64 destinationOffset,
    ID3D12Resource* source,UINT64 sourceOffset,UINT64 bytes) {
    if(g_ready.load()&&!g_done.load()&&!g_insideVertexDiscovery) {
        ++g_bufferCopyCalls;
        struct Guard {Guard(){g_insideVertexDiscovery=true;}~Guard(){g_insideVertexDiscovery=false;}} guard;
        try {
            const auto desc=destination->GetDesc();
            if(desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&desc.Width==0x400000&&bytes<=desc.Width&&destinationOffset<=desc.Width-bytes) {
                const auto from=source->GetDesc();
                if(from.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&bytes<=from.Width&&sourceOffset<=from.Width-bytes) {
                    ++g_vertexBufferCopies;
                    const auto dst=destination->GetGPUVirtualAddress(),src=source->GetGPUVirtualAddress();
                    ObserveVertexResource(destination,dst);ObserveVertexResource(source,src,true);
                    if(dst&&src) {
                        std::lock_guard<std::mutex> lock(g_bridgeMutex);
                        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)) {
                            auto& state=g_bridgeLists[list];
                            if(state.vertexCopyCount<state.vertexCopies.size())state.vertexCopies[state.vertexCopyCount++]={dst+destinationOffset,src+sourceOffset,bytes};
                            else {state.vertexCopyCount=0;state.taaSerial=0;++g_traceDrops;}
                        }
                    }
                }
            }
        }catch(...){++g_traceDrops;}
    }
    g_bufferCopy(list,destination,destinationOffset,source,sourceOffset,bytes);
}
bool CopyVertexBytes(UINT64 address,void* destination,size_t bytes,UINT64& arenaBase) {
    std::lock_guard<std::mutex> lock(g_vertexReadMutex);
    for(size_t i=0;i<g_vertexArenaCount;++i) {
        const auto& arena=g_vertexArenas[i];
        if(arena.mapped&&bytes<=arena.bytes&&address>=arena.base&&address-arena.base<=arena.bytes-bytes) {
            arenaBase=arena.base;const bool copied=CopyConstantBytes(arena.mapped+(address-arena.base),destination,bytes);
            if(copied)++g_vertexReadHits;else ++g_vertexReadMisses;return copied;
        }
    }
    ++g_vertexReadMisses;return false;
}
void CloseVertexReaders() noexcept {
    std::lock_guard<std::mutex> lock(g_vertexReadMutex);
    for(size_t i=0;i<g_vertexArenaCount;++i) {
        auto& arena=g_vertexArenas[i];D3D12_RANGE noWrite{0,0};
        if(arena.mapped)arena.resource->Unmap(0,&noWrite);
        arena.resource->Release();arena={};
    }
}
