// Observe the existing MHWSS depth-copy stage, separate from native TAA's
// placeholder t2. Its implementation stores the prepared R32 texture at +0x78.
using PreparedDepthStage=void(__fastcall*)(void*,ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned);
PreparedDepthStage g_preparedDepthStage{};
std::atomic<uint64_t> g_depthPreparations{0},g_taaDepthMatches{0};
std::set<std::tuple<uintptr_t,uintptr_t,unsigned,unsigned>> g_preparedDepthKinds;
std::set<std::tuple<uintptr_t,uintptr_t,unsigned,unsigned,bool>> g_taaDepthKinds;
ID3D12Resource* ReferencePreparedDepth(void* context) noexcept {
    __try {
        if(!context)return nullptr;
        auto* resource=*reinterpret_cast<ID3D12Resource**>(static_cast<unsigned char*>(context)+0x78);
        TextureCopySource reference{};return ReferenceTexture(resource,&reference)?reference.reference:nullptr;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
void __fastcall OnPreparedDepth(void* context,ID3D12GraphicsCommandList* list,ID3D12Resource* source,unsigned sourceState) {
    g_preparedDepthStage(context,list,source,sourceState);
    if(!(g_ready.load()&&!g_qualityCommands)||g_done.load()||!source)return;
    auto* prepared=ReferencePreparedDepth(context);if(!prepared)return;
    try {
        const auto a=source->GetDesc(),b=prepared->GetDesc();
        const auto sequence=++g_depthPreparations;
        const bool valid=a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&b.Dimension==a.Dimension&&
            a.Format==DXGI_FORMAT_R32_FLOAT&&b.Format==a.Format&&a.Width==b.Width&&a.Height==b.Height&&a.Width>=1280&&a.Height>=720;
        PreparedDepthTrace trace{reinterpret_cast<uintptr_t>(source),reinterpret_cast<uintptr_t>(prepared),unsigned(b.Width),b.Height,unsigned(b.Format),0,sequence};
        bool save=false;
        {std::lock_guard<std::mutex> lock(g_bridgeMutex);
            if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){auto& state=g_bridgeLists[list];trace.generation=state.generation;if(valid)state.preparedDepth=trace;else state.preparedDepth={};}
            const auto key=std::make_tuple(trace.source,trace.prepared,trace.width,trace.height);save=g_preparedDepthKinds.size()<32&&g_preparedDepthKinds.insert(key).second;}
        if(save){std::ostringstream out;out<<"{\"event\":\"mhwss_depth_preparation\",\"sequence\":"<<sequence<<",\"list\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(list)
            <<"\",\"source\":\"0x"<<trace.source<<"\",\"prepared\":\"0x"<<trace.prepared<<std::dec<<"\",\"size\":["<<b.Width<<','<<b.Height<<"],\"format\":"<<b.Format
            <<",\"source_state\":"<<sourceState<<",\"valid\":"<<(valid?"true":"false")<<",\"adds_gpu_commands\":false}";SaveBridge(out.str());}
    }catch(...){++g_traceDrops;}
    prepared->Release();
}
void RecordPreparedDepth(ID3D12GraphicsCommandList* list,uint64_t serial) {
    if(!VerboseDiagnostics())return;
    if(!g_traceTextures)return;
    PreparedDepthTrace trace{};mhwsr::Size input{},output{};bool matched=false,save=false;
    {std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(list);if(found==g_bridgeLists.end())return;
        const auto& state=found->second;trace=state.preparedDepth;input=state.taaInput;output=state.taaOutput;
        matched=trace.sequence&&trace.generation==state.generation&&state.generation&&trace.width==output.width&&trace.height==output.height;
        if(matched)++g_taaDepthMatches;
        const auto key=std::make_tuple(trace.source,trace.prepared,input.width,input.height,matched);
        save=state.detailedTextureFrame||(g_taaDepthKinds.size()<32&&g_taaDepthKinds.insert(key).second);}
    if(save){std::ostringstream out;out<<"{\"event\":\"taa_prepared_depth\",\"taa_serial\":"<<serial<<",\"depth_sequence\":"<<trace.sequence<<",\"generation\":"<<trace.generation
        <<",\"source\":\"0x"<<std::hex<<trace.source<<"\",\"prepared\":\"0x"<<trace.prepared<<std::dec<<"\",\"input\":["<<input.width<<','<<input.height
        <<"],\"depth_size\":["<<trace.width<<','<<trace.height<<"],\"same_command_list_generation\":"<<(matched?"true":"false")<<",\"pixel_contents_verified\":false}";SaveBridge(out.str());}
}
