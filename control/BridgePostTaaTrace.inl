// Passive observation of fullscreen triangles after an identified TAA dispatch.
// No vertex data, draw arguments, resource states or shader parameters are changed.
using VertexBuffers=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*);
using Draw=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
VertexBuffers g_vertexBuffers{};Draw g_draw{};
std::atomic<uint64_t> g_postTaaDraws{0},g_matchingPostTaaTriangles{0};
std::set<std::string> g_postDrawKinds;

void STDMETHODCALLTYPE OnBridgeVertices(ID3D12GraphicsCommandList* list,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW* views) {
    if(g_ready.load()&&!g_done.load()&&start==0&&count)try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){
            auto& state=g_bridgeLists[list];state.vertex=views?views[0]:D3D12_VERTEX_BUFFER_VIEW{};
            state.vertexCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());}
    }catch(...){++g_traceDrops;}
    g_vertexBuffers(list,start,count,views);
}
void ObservePostTaaDraw(ID3D12GraphicsCommandList* list,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance) {
    ListTrace state{};
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(list);
        if(found==g_bridgeLists.end()||!found->second.taaSerial||found->second.postTaaOps>=32)return;
        ++found->second.postTaaOps;state=found->second;
    }
    ++g_postTaaDraws;
    if(vertices!=3||instances!=1||firstInstance||state.vertex.StrideInBytes!=16)return;
    const UINT64 offset=UINT64(firstVertex)*16;
    if(offset+48>state.vertex.SizeInBytes)return;
    if(state.vertex.BufferLocation>UINT64_MAX-offset)return;
    const auto address=state.vertex.BufferLocation+offset;
    float data[12]{};bool mapped=false;UINT64 arenaBase=0,copySource=0;
    {
        std::lock_guard<std::mutex> lock(g_screenReadMutex);
        if(g_screenMapped&&address>=g_screenGpuBase&&address-g_screenGpuBase<=g_screenBytes-sizeof(data))
            {mapped=CopyConstantBytes(g_screenMapped+(address-g_screenGpuBase),data,sizeof(data));arenaBase=g_screenGpuBase;}
    }
    if(!mapped)mapped=CopyVertexBytes(address,data,sizeof(data),arenaBase);
    // A DEFAULT-heap vertex buffer may be populated by an earlier copy in this
    // same command-list generation. Observe its CPU-visible source, not the GPU
    // destination, and retain this distinction in the log.
    if(!mapped)for(unsigned i=state.vertexCopyCount;i>0;--i) {
        const auto& copy=state.vertexCopies[i-1];
        if(copy.bytes>=sizeof(data)&&address>=copy.destination&&address-copy.destination<=copy.bytes-sizeof(data)) {
            copySource=copy.source+(address-copy.destination);
            mapped=CopyVertexBytes(copySource,data,sizeof(data),arenaBase);break;
        }
    }
    for(float f:data)if(!std::isfinite(f))mapped=false;
    const auto closeFloat=[](float a,float b){return std::isfinite(a)&&std::fabs(a-b)<0.00002f;};
    const bool shape=mapped&&closeFloat(data[0],-1)&&closeFloat(data[1],1)&&closeFloat(data[4],-1)&&closeFloat(data[5],-3)&&closeFloat(data[8],3)&&closeFloat(data[9],1);
    const bool viewport=state.hasView&&closeFloat(state.view.TopLeftX,0)&&closeFloat(state.view.TopLeftY,0)&&
        closeFloat(state.view.Width,float(state.taaOutput.width))&&closeFloat(state.view.Height,float(state.taaOutput.height));
    const float u=state.taaOutput.width?2*(float(state.taaInput.width)-0.5f)/float(state.taaOutput.width):0;
    const float v=state.taaOutput.height?2*(float(state.taaInput.height)-0.5f)/float(state.taaOutput.height):0;
    const bool matches=shape&&viewport&&state.taaInput.width<state.taaOutput.width&&state.taaInput.height<state.taaOutput.height&&
        closeFloat(data[2],0)&&closeFloat(data[3],0)&&closeFloat(data[6],0)&&closeFloat(data[7],v)&&closeFloat(data[10],u)&&closeFloat(data[11],0);
    if(matches)++g_matchingPostTaaTriangles;
    std::ostringstream key;key<<state.pso<<':'<<state.taaInput.width<<':'<<state.taaInput.height<<':'<<shape<<':'<<matches<<':'<<mapped;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_postDrawKinds.count(key.str())||g_postDrawKinds.size()>=96)return;
        g_postDrawKinds.insert(key.str());
    }
    std::ostringstream out;out<<std::setprecision(9);
    out<<"{\"event\":\"post_taa_triangle\",\"taa_serial\":"<<state.taaSerial<<",\"post_taa_operation\":"<<state.postTaaOps
       <<",\"thread\":"<<GetCurrentThreadId()<<",\"list\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(list)
       <<"\",\"pso\":\"0x"<<reinterpret_cast<uintptr_t>(state.pso)<<"\",\"vertex_gpu_address\":\"0x"<<address<<std::dec
       <<"\",\"vertex_arena_base\":\"0x"<<std::hex<<arenaBase<<"\",\"copy_source_address\":\"0x"<<copySource<<std::dec
       <<"\",\"vertex_size\":"<<state.vertex.SizeInBytes<<",\"first_vertex\":"<<firstVertex
       <<",\"input\":["<<state.taaInput.width<<','<<state.taaInput.height<<"],\"output\":["<<state.taaOutput.width<<','<<state.taaOutput.height
       <<"],\"vertex_data_mapped\":"<<(mapped?"true":"false")<<",\"full_screen_shape\":"<<(shape?"true":"false")
       <<",\"matches_low_roi_sampling\":"<<(matches?"true":"false")<<",\"vertices\":[";
    for(unsigned i=0;i<12;++i){if(i)out<<',';if(mapped)out<<data[i];else out<<"null";}
    out<<"],\"viewport\":";
    if(state.hasView)out<<'['<<state.view.TopLeftX<<','<<state.view.TopLeftY<<','<<state.view.Width<<','<<state.view.Height<<']';else out<<"null";
    out<<",\"vertex_bind_caller\":\"0x"<<std::hex<<state.vertexCaller<<"\",\"draw_call_stack\":[";
    void* stack[20]{};const auto frames=CaptureStackBackTrace(1,20,stack,nullptr);
    for(unsigned i=0;i<frames;++i){if(i)out<<',';out<<"\"0x"<<reinterpret_cast<uintptr_t>(stack[i])<<"\"";}
    out<<"],\"changes_draw\":false}";SaveBridge(out.str());
}
void STDMETHODCALLTYPE OnBridgeDraw(ID3D12GraphicsCommandList* list,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance) {
    if(g_ready.load()&&!g_done.load())try{ObservePostTaaDraw(list,vertices,instances,firstVertex,firstInstance);}catch(...){++g_traceDrops;}
    g_draw(list,vertices,instances,firstVertex,firstInstance);
}
