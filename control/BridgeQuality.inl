// Experimental, bounded rendering integration. Included after all observer types.
// The observer's original path remains the default; QualityPrototype is opt-in.
constexpr bool kQualityAtNativeInputs=true;
using QualityBarriers=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
using QualityExecute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
using QualityTopology=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D_PRIMITIVE_TOPOLOGY);
using QualityIndexed=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
QualityBarriers g_qualityBarriers{};QualityExecute g_qualityExecute{};
QualityTopology g_qualityTopology{};
QualityIndexed g_qualityIndexed{};
std::mutex g_qualityMutex,g_qualitySubmitMutex;
struct QualityResourceState {D3D12_RESOURCE_STATES value{};bool valid=false;};
struct QualityStates {bool reset=false,overflow=false;std::unordered_map<ID3D12Resource*,QualityResourceState> resources;};
std::unordered_map<ID3D12GraphicsCommandList*,QualityStates> g_qualityStates;
std::unordered_map<ID3D12CommandList*,Microsoft::WRL::ComPtr<ID3D12CommandQueue>> g_qualityQueues;
std::map<uintptr_t,unsigned> g_qualityRoles;
mhwsr::QualityGpu* g_qualityGpu{};
mhwsr::Plan g_qualityPlan{};
ID3D12GraphicsCommandList* g_qualityList{};
Microsoft::WRL::ComPtr<ID3D12CommandQueue> g_qualityQueue;
uint64_t g_qualityRecorded=0,g_qualitySubmitted=0,g_qualityFenceValue=0;
uint64_t g_qualityDepthSequence=0,g_qualityDepthCopyAssociations=0,g_qualityPreparedDepthAssociations=0;
uint64_t g_qualitySceneDepthSequence=0,g_qualityDepthSnapshots=0,g_qualityRasterAssociations=0;
std::set<std::tuple<uintptr_t,unsigned,unsigned>> g_qualitySceneDepthKinds;
std::set<std::tuple<uintptr_t,unsigned,unsigned,bool>> g_qualityDepthKinds;
std::set<std::tuple<uintptr_t,unsigned,unsigned>> g_qualityDepthDrawKinds;
bool g_qualityRetain=false,g_qualityLowChain=false,g_qualityPrimed=false;
uintptr_t g_qualityColor=0,g_qualityCopy=0,g_qualityTone=0,g_qualityFinal=0,g_qualityDepth=0;
unsigned g_qualityToneBaselines=0;
float g_qualityLastJitter[2]{};bool g_qualityHadHistory=false;
std::atomic<uint64_t> g_qualityEvaluated{0},g_qualitySucceeded{0},g_qualityCopies{0},g_qualityBlurBypasses{0},g_qualityToneDraws{0},g_qualityFinalDraws{0};
std::atomic<unsigned> g_qualityBusy{0};
std::map<std::string,uint64_t> g_qualityWaits;
bool QualityWait(const char* reason){if(g_qualityWaits.size()<32||g_qualityWaits.count(reason))++g_qualityWaits[reason];return false;}
struct QualityBusy {QualityBusy(){++g_qualityBusy;}~QualityBusy(){--g_qualityBusy;}};
struct QualityCommands {QualityCommands(){++g_qualityCommands;}~QualityCommands(){--g_qualityCommands;}};
bool QualityObserving(){return g_qualityMode&&g_ready.load()&&!g_done.load()&&!g_qualityCommands;}
void STDMETHODCALLTYPE OnQualityTopology(ID3D12GraphicsCommandList* list,D3D_PRIMITIVE_TOPOLOGY topology) {
    if(QualityObserving())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list))g_bridgeLists[list].topology=topology;
    }catch(...){++g_traceDrops;}
    g_qualityTopology(list,topology);
}
void QualityFault(const char* reason) {
    if(!g_abort.exchange(true))Log(std::string("{\"event\":\"quality_abort\",\"reason\":\"")+reason+"\",\"restores_engine_scale\":true}");
}
void InvalidateQualityBindings(ID3D12GraphicsCommandList* list) {
    if(!QualityObserving())return;
    std::lock_guard<std::mutex> lock(g_qualityMutex);
    std::lock_guard<std::mutex> guard(g_bridgeMutex);
    const auto found=g_bridgeLists.find(list);
    if(found!=g_bridgeLists.end()&&found->second.quality.sr&&found->second.quality.stage!=4)
        QualityFault("sr_chain_interrupted_by_state_clear");
}
void ResetQualityList(ID3D12GraphicsCommandList* list) {
    if(!QualityObserving())return;
    InvalidateQualityBindings(list);
    std::lock_guard<std::mutex> lock(g_qualityMutex);
    if(list==g_qualityList&&g_qualityRecorded>g_qualitySubmitted){g_qualityRetain=true;QualityFault("command_list_reset_before_observed_submission");}
    if(g_qualityStates.size()<512||g_qualityStates.count(list)){auto& s=g_qualityStates[list];s={};s.reset=true;}
}
void STDMETHODCALLTYPE OnQualityBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers) {
    if(QualityObserving()&&barriers)try {
        std::lock_guard<std::mutex> lock(g_qualityMutex);auto f=g_qualityStates.find(list);
        if(f!=g_qualityStates.end()&&f->second.reset&&!f->second.overflow) {
            auto& s=f->second;
            if(count>16384)s.overflow=true;
            else for(UINT i=0;i<count;++i) {
                const auto& b=barriers[i];
                if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                    const auto& t=b.Transition;
                    if(!t.pResource||(t.Subresource&&t.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))continue;
                    if(s.resources.size()>=4096&&!s.resources.count(t.pResource)){s.overflow=true;break;}
                    s.resources[t.pResource]={t.StateAfter,b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY};
                    if(reinterpret_cast<uintptr_t>(t.pResource)==g_qualityDepthAnchor.load()) {
                        std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto tracked=g_bridgeLists.find(list);
                        if(tracked!=g_bridgeLists.end())tracked->second.qualityDepthRenderPending=(t.StateAfter&D3D12_RESOURCE_STATE_RENDER_TARGET)!=0;
                    }
                }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                    if(!b.Aliasing.pResourceBefore||!b.Aliasing.pResourceAfter)s.resources.clear();
                    else {s.resources.erase(b.Aliasing.pResourceBefore);s.resources.erase(b.Aliasing.pResourceAfter);}
                }
            }
        }
    }catch(...){QualityFault("barrier_tracking_exception");}
    g_qualityBarriers(list,count,barriers);
}
bool QualityState(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES& out) {
    // Caller holds g_qualityMutex. Never guess StateBefore from a shader binding.
    const auto l=g_qualityStates.find(list);if(l==g_qualityStates.end()||!l->second.reset||l->second.overflow)return false;
    const auto r=l->second.resources.find(resource);if(r==l->second.resources.end()||!r->second.valid)return false;
    out=r->second.value;return true;
}
void STDMETHODCALLTYPE OnQualityExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    // Keep our monotonically increasing Signal values in original submission order.
    std::lock_guard<std::mutex> submit(g_qualitySubmitMutex);
    uint64_t serial=0;mhwsr::QualityGpu* gpu=nullptr;
    try {
        std::lock_guard<std::mutex> lock(g_qualityMutex);
        if(lists&&count<=4096)for(UINT i=0;i<count;++i) {
            if(g_qualityQueues.size()<512||g_qualityQueues.count(lists[i]))g_qualityQueues[lists[i]]=queue;
            if(lists[i]==static_cast<ID3D12CommandList*>(g_qualityList)&&g_qualityGpu&&g_qualityRecorded>g_qualitySubmitted) {
                serial=g_qualityRecorded;gpu=g_qualityGpu;
                if(queue!=g_qualityQueue.Get()){g_qualityRetain=true;QualityFault("command_queue_changed");}
            }
        }
    }catch(...){QualityFault("submission_tracking_exception");}
    g_qualityExecute(queue,count,lists);
    if(!serial||!gpu)return;
    std::lock_guard<std::mutex> lock(g_qualityMutex);
    if(g_qualityRetain)return;
    const auto value=++g_qualityFenceValue;
    if(FAILED(queue->Signal(gpu->Fence(),value))){g_qualityRetain=true;QualityFault("queue_signal_failed");return;}
    g_qualitySubmitted=serial;
}
std::string QualityShaderHash(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};DWORD size=0,n=0;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    std::string result;
    if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&n,0)>=0) {
        std::vector<unsigned char> object(size);std::array<unsigned char,32> digest{};
        if(BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0) {
            if(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0&&
               BCryptFinishHash(hash,digest.data(),32,0)>=0)result=HexBytes(digest.data(),32);
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(alg,0);return result;
}
unsigned QualityRole(uintptr_t pso) {
    const auto found=g_qualityRoles.find(pso);if(found!=g_qualityRoles.end())return found->second;
    std::lock_guard<std::mutex> lock(g_textureMutex);
    const auto ps=g_textureShaders.find({pso,1});if(ps==g_textureShaders.end())return 0;
    const auto digest=QualityShaderHash(ps->second);unsigned role=0;
    if(digest=="3eed9b57bbbdf38d14d9a94596fc93a107aa99eab275af7118b95febbbc1ba18")role=1;
    if(digest=="174a09e5b23c7e01fb6c47ce86b61250d695113109abb4ffae7219117c31a8cb")role=2;
    if(digest=="be6127ba48008790fd0e93558b8248cb8acd7c0577899bf50d28e5618ae37b79")role=3;
    if(role==1||role==2) {
        const auto vs=g_textureShaders.find({pso,0});
        if(vs==g_textureShaders.end()||QualityShaderHash(vs->second)!="abddc634c053c157f295842964b023610ff64a7505d4b253edbe1612680ea937")role=0;
    }
    if(g_qualityRoles.size()<64)g_qualityRoles.emplace(pso,role);return role;
}
uintptr_t QualityBinding(const ListTrace& state,ID3D12GraphicsCommandList* list,bool compute,unsigned offset,unsigned kind,unsigned slot) {
    const auto& h=state.textures.heap;const auto table=(compute?state.textures.compute:state.textures.graphics).tables[0];
    if(!h.gpu||!h.cpu||!h.stride||table<h.gpu||(table-h.gpu)%h.stride)return 0;
    const auto index=(table-h.gpu)/h.stride;if(index+offset>=h.count)return 0;
    std::lock_guard<std::mutex> lock(g_textureMutex);const auto f=g_textureDescriptors.find(h.cpu+(index+offset)*h.stride);
    if(f==g_textureDescriptors.end())return 0;
    const auto& b=f->second;
    return b.list==reinterpret_cast<uintptr_t>(list)&&b.pass==state.texturePassId&&state.texturePassId&&b.tableBase==h.cpu+index*h.stride&&
        b.kind==kind&&b.slot==slot?b.texture.resource:0;
}
uintptr_t QualityTarget(const ListTrace& state) {
    if(state.textures.targetCount!=1)return 0;
    std::lock_guard<std::mutex> lock(g_textureMutex);const auto f=g_textureRtvs.find(state.textures.targets[0]);
    return f==g_textureRtvs.end()?0:f->second.resource;
}
bool QualityTexture(uintptr_t address,mhwsr::Size size,DXGI_FORMAT format) {
    TextureCopySource ref{};if(!ReferenceTexture(reinterpret_cast<ID3D12Resource*>(address),&ref))return false;
    const auto d=ref.reference->GetDesc();ref.reference->Release();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==size.width&&d.Height==size.height&&d.Format==format&&
        d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1;
}
void RecordQualityDepthCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,ID3D12Resource* source,
    const D3D12_TEXTURE_COPY_LOCATION* dst,const D3D12_TEXTURE_COPY_LOCATION* src,UINT x,UINT y,UINT z,const D3D12_BOX* box) {
    if(!QualityObserving()||!destination||!source||x||y||z)return;
    try {
        std::lock_guard<std::mutex> lock(g_qualityMutex);
        // The baseline MHWSS depth stage identifies the main depth resource.
        // After that, only an ACTUAL game copy to this identity can associate it.
        if(!g_qualityDepth||reinterpret_cast<uintptr_t>(destination)!=g_qualityDepth)return;
        if(dst&&(!src||dst->Type!=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX||src->Type!=dst->Type||dst->SubresourceIndex||src->SubresourceIndex))return;
        if(box&&(box->left||box->top||box->front||box->back!=1))return;
        const auto d=destination->GetDesc(),s=source->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||s.Dimension!=d.Dimension||d.Format!=DXGI_FORMAT_R32_FLOAT||
           d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1||s.SampleDesc.Count!=1)return;
        if(s.Format==DXGI_FORMAT_R32_TYPELESS&&(s.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)&&s.Width==d.Width&&s.Height==d.Height) {
            const auto prior=g_qualityRasterDepthAnchor.load();
            if(!prior)g_qualityRasterDepthAnchor.store(reinterpret_cast<uintptr_t>(source));
            else if(prior!=reinterpret_cast<uintptr_t>(source))QualityFault("raster_depth_source_changed");
        }
        const auto w=box?box->right:unsigned(s.Width),h=box?box->bottom:s.Height;
        if(!w||!h||w>d.Width||h>d.Height||w>s.Width||h>s.Height)return;
        QualityDepthWrite record{reinterpret_cast<uintptr_t>(source),g_qualityDepth,w,h,0,++g_qualityDepthSequence};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto found=g_bridgeLists.find(list);
            if(found==g_bridgeLists.end()||!found->second.generation)return;
            record.generation=found->second.generation;found->second.qualityDepth=record;}
        if(g_qualityDepthKinds.insert({reinterpret_cast<uintptr_t>(list),w,h,dst!=nullptr}).second) {
            std::ostringstream out;out<<"{\"event\":\"quality_depth_write\",\"list\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(list)
                <<"\",\"source\":\"0x"<<record.source<<"\",\"destination\":\"0x"<<record.destination<<std::dec
                <<"\",\"written_size\":["<<w<<','<<h<<"],\"generation\":"<<record.generation<<",\"sequence\":"<<record.sequence
                <<",\"source_format\":"<<unsigned(s.Format)<<",\"texture_region\":"<<(dst?"true":"false")<<",\"adds_gpu_commands\":false}";Log(out.str());
        }
    }catch(...){QualityFault("depth_copy_association_exception");}
}
void RecordQualitySceneDepth(ID3D12GraphicsCommandList* list) {
    if(kQualityAtNativeInputs)return;
    if(!QualityObserving()||!g_qualityRasterDepthAnchor.load()||!g_qualityGbufferAnchor.load())return;
    try {
        std::lock_guard<std::mutex> lock(g_qualityMutex);
        ListTrace state{};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto f=g_bridgeLists.find(list);if(f==g_bridgeLists.end())return;state=f->second;}
        if(state.textures.depthResource!=g_qualityRasterDepthAnchor.load()||!state.hasView||!state.generation||
           !Near(state.view.TopLeftX,0)||!Near(state.view.TopLeftY,0)||state.view.Width<640||state.view.Height<360)return;
        bool bound=false;
        {std::lock_guard<std::mutex> guard(g_textureMutex);for(unsigned i=0;i<state.textures.targetCount;++i){
            const auto t=g_textureRtvs.find(state.textures.targets[i]);if(t!=g_textureRtvs.end()&&t->second.resource==g_qualityGbufferAnchor.load())bound=true;}}
        if(!bound)return;
        QualitySceneDepth record{state.textures.depthResource,g_qualityGbufferAnchor.load(),unsigned(state.view.Width),unsigned(state.view.Height),state.generation,++g_qualitySceneDepthSequence};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);g_bridgeLists.at(list).qualitySceneDepth=record;}
        if(g_qualitySceneDepthKinds.insert({reinterpret_cast<uintptr_t>(list),record.width,record.height}).second){
            std::ostringstream out;out<<"{\"event\":\"quality_scene_depth_binding\",\"list\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(list)
                <<"\",\"depth\":\"0x"<<record.resource<<"\",\"gbuffer\":\"0x"<<record.gbuffer<<std::dec<<"\",\"viewport\":["<<record.width<<','<<record.height
                <<"],\"generation\":"<<record.generation<<",\"sequence\":"<<record.sequence<<'}';Log(out.str());}
    }catch(...){QualityFault("scene_depth_binding_exception");}
}
void STDMETHODCALLTYPE OnQualityIndexed(ID3D12GraphicsCommandList* list,UINT indices,UINT instances,UINT first,INT base,UINT firstInstance) {
    g_qualityIndexed(list,indices,instances,first,base,firstInstance);
    if(indices&&instances)RecordQualitySceneDepth(list);
}
void CaptureQualitySceneDepth(ID3D12GraphicsCommandList* list,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL consecutive) {
    if(kQualityAtNativeInputs)return;
    if(!QualityObserving()||g_phase.load()!=2||g_abort.load()||(count&&!handles)||count>8)return;QualityBusy busy;
    try {
        std::unique_lock<std::mutex> lock(g_qualityMutex);if(!g_qualityGpu||!g_qualityPrimed||list!=g_qualityList)return;
        ListTrace state{};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto f=g_bridgeLists.find(list);if(f==g_bridgeLists.end())return;state=f->second;}
        const auto scope=state.qualitySceneDepth;
        if(!scope.sequence||scope.sequence==state.qualityDepthSnapshot.sequence||scope.generation!=state.generation)return;
        {std::lock_guard<std::mutex> guard(g_textureMutex);for(unsigned i=0;i<count;++i){
            const auto handle=consecutive?handles[0].ptr+UINT64(i)*g_textureRtvStride:handles[i].ptr;
            const auto t=g_textureRtvs.find(handle);if(t!=g_textureRtvs.end()&&t->second.resource==scope.gbuffer)return;}}
        D3D12_RESOURCE_STATES value{};auto* source=reinterpret_cast<ID3D12Resource*>(scope.resource);
        if(!QualityState(list,source,value)){QualityWait("raster_depth_copy_state");return;}
        auto* gpu=g_qualityGpu;++g_qualityRecorded;lock.unlock();
        bool captured=false;{QualityCommands internal;captured=gpu->CaptureRasterDepth(list,source,value);}
        lock.lock();
        if(!captured){QualityFault("raster_depth_copy_failed");return;}
        ++g_qualityDepthSnapshots;
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);g_bridgeLists.at(list).qualityDepthSnapshot=scope;}
    }catch(...){QualityFault("raster_depth_snapshot_exception");}
}
void RecordQualityDepthDraw(ID3D12GraphicsCommandList* list,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance) {
    if(!QualityObserving())return;
    try {
        ListTrace state{};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto f=g_bridgeLists.find(list);
            if(f==g_bridgeLists.end()||!f->second.qualityDepthRenderPending)return;state=f->second;}
        if(QualityTarget(state)!=g_qualityDepthAnchor.load())return;
        const auto source=QualityBinding(state,list,false,0,2,0);
        bool save=false;
        {std::lock_guard<std::mutex> guard(g_qualityMutex);save=g_qualityDepthDrawKinds.insert({reinterpret_cast<uintptr_t>(state.pso),unsigned(state.view.Width),unsigned(state.view.Height)}).second;}
        if(!save)return;
        RecordTextureSnapshot(list,false,0,0,vertices,"depth_producer");
        float data[16]{};UINT64 base=0;const auto bytes=vertices*16;
        const bool copied=vertices<=4&&state.vertex.StrideInBytes==16&&UINT64(firstVertex)*16+bytes<=state.vertex.SizeInBytes&&
            CopyVertexBytes(state.vertex.BufferLocation+UINT64(firstVertex)*16,data,bytes,base);
        std::ostringstream out;out<<std::setprecision(9)<<"{\"event\":\"quality_depth_draw\",\"list\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(list)
            <<"\",\"pso\":\"0x"<<reinterpret_cast<uintptr_t>(state.pso)<<"\",\"source\":\"0x"<<source<<std::dec
            <<"\",\"generation\":"<<state.generation<<",\"vertex_count\":"<<vertices<<",\"instances\":"<<instances<<",\"first_instance\":"<<firstInstance
            <<",\"viewport\":["<<state.view.Width<<','<<state.view.Height<<"],\"vertices_copied\":"<<(copied?"true":"false")<<",\"vertices\":[";
        if(copied)for(unsigned i=0;i<vertices*4;++i){if(i)out<<',';if(std::isfinite(data[i]))out<<data[i];else out<<"null";}
        out<<"],\"changes_rendering\":false}";Log(out.str());
    }catch(...){QualityFault("depth_draw_observation_exception");}
}
void QualityRestoreBindings(ID3D12GraphicsCommandList* list,const ListTrace& s) {
    list->SetDescriptorHeaps(s.textures.heapCount,s.textures.heaps.data());
    const auto restore=[&](const TextureRoots& r,bool compute) {
        if(!r.signature)return;
        auto* signature=reinterpret_cast<ID3D12RootSignature*>(r.signature);
        if(compute)list->SetComputeRootSignature(signature);else list->SetGraphicsRootSignature(signature);
        for(unsigned i=0;i<32;++i) {
            if(r.tables[i]){D3D12_GPU_DESCRIPTOR_HANDLE h{r.tables[i]};if(compute)list->SetComputeRootDescriptorTable(i,h);else list->SetGraphicsRootDescriptorTable(i,h);}
            if(r.cbvs[i]){if(compute)list->SetComputeRootConstantBufferView(i,r.cbvs[i]);else list->SetGraphicsRootConstantBufferView(i,r.cbvs[i]);}
        }
    };
    restore(s.textures.compute,true);restore(s.textures.graphics,false);
    list->SetPipelineState(s.pso);
    if(s.hasView)list->RSSetViewports(1,&s.view);
    if(s.hasScissor)list->RSSetScissorRects(1,&s.scissor);
    if(s.vertex.BufferLocation)list->IASetVertexBuffers(0,1,&s.vertex);
    if(s.topology!=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)list->IASetPrimitiveTopology(s.topology);
    D3D12_CPU_DESCRIPTOR_HANDLE targets[8]{};for(unsigned i=0;i<s.textures.targetCount;++i)targets[i].ptr=s.textures.targets[i];
    D3D12_CPU_DESCRIPTOR_HANDLE depth{s.textures.depthHandle};
    list->OMSetRenderTargets(s.textures.targetCount,targets,FALSE,depth.ptr?&depth:nullptr);
}
bool RunQualityTaa(ID3D12GraphicsCommandList* list,const TaaScreenInput& screen) {
    if(!QualityObserving())return false;QualityBusy busy;
    try {
        std::unique_lock<std::mutex> lock(g_qualityMutex);
        if(Uint(g_mhwss,0x54eb70)||Uint(g_mhwss,0x574230)||memcmp(g_mhwss+0x54ebe0,std::array<unsigned char,4>{}.data(),4)) {
            QualityFault("mhwss_mode_changed");return false;
        }
        ListTrace state{};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);state=g_bridgeLists.at(list);g_bridgeLists.at(list).quality={};}
        if(state.quality.sr&&state.quality.stage!=4)QualityFault("previous_sr_postprocess_chain_incomplete");
        if(screen.error||!screen.cameraValid||!state.generation||!state.textures.heapCount||!state.textures.compute.signature)return QualityWait("frame_association");
        const mhwsr::Size output{unsigned(ScreenFloat(screen,16)),unsigned(ScreenFloat(screen,20))},input{screen.words[10],screen.words[11]};
        if(kQualityAtNativeInputs) {
            const auto c=QualityBinding(state,list,true,9,2,0),mv=QualityBinding(state,list,true,13,2,4),out=QualityBinding(state,list,true,0,3,0);
            if(!QualityTexture(c,output,DXGI_FORMAT_R11G11B10_FLOAT)||!QualityTexture(mv,output,DXGI_FORMAT_R32_UINT)||!QualityTexture(out,output,DXGI_FORMAT_R11G11B10_FLOAT))return QualityWait("taa_candidate_resources");
            mhwsr::QualityFrame candidate{};candidate.list=list;candidate.color=reinterpret_cast<ID3D12Resource*>(c);
            candidate.packedMotion=reinterpret_cast<ID3D12Resource*>(mv);candidate.taaOutput=reinterpret_cast<ID3D12Resource*>(out);candidate.render=input;
            memcpy(candidate.jitter,screen.jitter,8);memcpy(candidate.previousJitter,screen.previousJitter,8);
            {std::lock_guard<std::mutex> guard(g_bridgeMutex);auto& s=g_bridgeLists.at(list);s.quality={c,out,0,0,0,false};s.qualityCandidate=candidate;s.qualityCamera=screen.camera;s.qualityScreen=screen.screen;}
            // Native TAA is retained as an immediate fallback. Its output is not
            // fed into SR: the raw color remains intact until the first blur draw.
            if(g_qualityGpu)return false;
        }
        const bool preparedDepth=state.preparedDepth.sequence&&state.preparedDepth.generation==state.generation;
        const bool copied=state.qualityDepth.sequence>state.lastQualityDepth&&state.qualityDepth.generation==state.generation&&
            state.qualityDepth.destination==g_qualityDepth&&g_qualityDepth&&state.qualityDepth.width>=input.width&&state.qualityDepth.height>=input.height;
        const auto& snapshot=state.qualityDepthSnapshot;
        const bool raster=snapshot.sequence>state.lastQualitySceneDepth&&snapshot.generation==state.generation&&
            snapshot.resource==g_qualityRasterDepthAnchor.load()&&snapshot.width==input.width&&snapshot.height==input.height;
        if(!preparedDepth&&!copied&&!raster)return QualityWait("depth_frame_association");
        const auto color=QualityBinding(state,list,true,9,2,0),motion=QualityBinding(state,list,true,13,2,4),taa=QualityBinding(state,list,true,0,3,0);
        const auto gbuffer=QualityBinding(state,list,true,12,2,3);
        const auto depth=raster?snapshot.resource:copied?state.qualityDepth.destination:state.preparedDepth.source;
        if(raster&&snapshot.gbuffer!=gbuffer)return QualityWait("scene_depth_gbuffer_identity");
        if(!QualityTexture(color,output,DXGI_FORMAT_R11G11B10_FLOAT)||!QualityTexture(taa,output,DXGI_FORMAT_R11G11B10_FLOAT)||
           !QualityTexture(motion,output,DXGI_FORMAT_R32_UINT)||!QualityTexture(depth,output,raster?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32_FLOAT))return QualityWait("input_resource_identity");
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);auto& target=g_bridgeLists.at(list);target.quality={color,taa,0,0,0,false};target.lastQualityDepth=state.qualityDepth.sequence;target.lastQualitySceneDepth=snapshot.sequence;}
        if(raster)++g_qualityRasterAssociations;
        if(copied)++g_qualityDepthCopyAssociations;else ++g_qualityPreparedDepthAssociations;
        if(g_qualityList&&g_qualityList!=list)return false;
        mhwsr::QualityFrame f{};f.list=list;f.color=reinterpret_cast<ID3D12Resource*>(color);f.depth=reinterpret_cast<ID3D12Resource*>(depth);
        f.packedMotion=reinterpret_cast<ID3D12Resource*>(motion);f.taaOutput=reinterpret_cast<ID3D12Resource*>(taa);f.render=input;
        f.depthIsRaster=raster;
        if(!QualityState(list,f.color,f.colorState))return QualityWait("color_barrier_state");
        if(!raster&&!QualityState(list,f.depth,f.depthState))return QualityWait("depth_barrier_state");
        if(!QualityState(list,f.packedMotion,f.motionState))return QualityWait("motion_barrier_state");
        if(!QualityState(list,f.taaOutput,f.outputState))return QualityWait("output_barrier_state");
        const auto q=g_qualityQueues.find(list);
        if(q==g_qualityQueues.end()||q->second->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return QualityWait("direct_queue_association");
        const auto queue=q->second;
        if(!g_qualityGpu&&!g_abort.load()) {
            if(input.width!=output.width||input.height!=output.height)return QualityWait("quality_plan");
            g_qualityList=list;g_qualityQueue=queue;g_qualityColor=color;g_qualityDepth=depth;
            g_qualityDepthAnchor.store(depth);
            g_qualityGbufferAnchor.store(gbuffer);
            mhwsr::Plan plan{};
            // Foreign driver/NGX calls must not run under the tracking mutex:
            // their workers can enter unrelated command-list hooks concurrently.
            lock.unlock();const bool planned=mhwsr::QueryPlan(g_ngx,output,2,plan);lock.lock();
            if(!planned||!mhwsr::QualityPlanValid(plan))return QualityWait("quality_plan");
            if(!g_ready.load()||g_done.load()||g_abort.load())return false;
            g_qualityPlan=plan;
            Microsoft::WRL::ComPtr<ID3D12Device> device,queueDevice;
            if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))||device.Get()!=queueDevice.Get())return false;
            QualityCommands internal;
            auto candidate=std::make_unique<mhwsr::QualityGpu>();
            lock.unlock();const bool prepared=candidate->Prepare(g_ngx,plan,device.Get(),f.packedMotion,g_gpuTimings);lock.lock();
            if(!prepared){QualityFault("gpu_preparation_failed");return false;}
            if(!g_ready.load()||g_done.load()||g_abort.load())return false;
            g_qualityGpu=candidate.release();
            Log("{\"event\":\"quality_gpu_prepared\",\"mode\":2,\"preset\":11,\"output\":["+std::to_string(output.width)+","+std::to_string(output.height)+
                "],\"optimal\":["+std::to_string(g_qualityPlan.optimal.width)+","+std::to_string(g_qualityPlan.optimal.height)+"],\"aligned\":["+
                std::to_string(g_qualityPlan.aligned.width)+","+std::to_string(g_qualityPlan.aligned.height)+"],\"creates_feature\":false}");
            ++g_qualityRecorded;
            auto* gpu=g_qualityGpu;NVSDK_NGX_Result created{};
            lock.unlock();
            {struct RestoreHost {ID3D12GraphicsCommandList* list;const ListTrace& state;~RestoreHost(){QualityRestoreBindings(list,state);}} restore{list,state};
                created=gpu->Prime(list);}
            lock.lock();
            Log("{\"event\":\"quality_feature_creation\",\"result\":"+std::to_string(unsigned(created))+",\"evaluates_native_resolution\":false}");
            const auto modern=ModulePath(nullptr).parent_path()/L"OptiScaler"/L"nvngx_dlss.dll";
            const auto loaded=GetModuleHandleW(modern.c_str());
            if(!mhwsr::Ok(created)||!loaded||!std::filesystem::equivalent(ModulePath(loaded),modern)){
                QualityFault("quality_feature_creation_or_modern_library_load_failed");return false;}
            g_qualityPrimed=true;
        }
        if(!g_qualityGpu||g_abort.load()||g_phase.load()!=2)return false;
        if(!g_qualityLowChain)return QualityWait("low_postprocess_chain");
        if(input.width!=g_qualityPlan.aligned.width||input.height!=g_qualityPlan.aligned.height)return QualityWait("aligned_render_size");
        if(std::fabs(screen.jitter[0])+std::fabs(screen.jitter[1])<0.00000001f)return QualityWait("nonzero_projection_jitter");
        if(output.width!=g_qualityPlan.output.width||output.height!=g_qualityPlan.output.height||color!=g_qualityColor||(!raster&&depth!=g_qualityDepth)||
           !g_qualityGpu->Matches(f.packedMotion)||queue.Get()!=g_qualityQueue.Get()||!JitterPulseStillOwned()) {
            QualityFault("frame_resources_or_output_changed");return false;
        }
        // Directly consume the real source depth in THIS command-list generation,
        // not the shared MHWSS prepared texture that another list can overwrite.
        f.associated=true;memcpy(f.jitter,screen.jitter,8);memcpy(f.previousJitter,screen.previousJitter,8);
        f.reset=!g_qualityHadHistory||!Near(f.previousJitter[0],g_qualityLastJitter[0])||!Near(f.previousJitter[1],g_qualityLastJitter[1]);
        ++g_qualityRecorded;++g_qualityEvaluated;
        auto* gpu=g_qualityGpu;NVSDK_NGX_Result result{};
        lock.unlock();
        {QualityCommands internal;
            struct RestoreHost {ID3D12GraphicsCommandList* list;const ListTrace& state;~RestoreHost(){QualityRestoreBindings(list,state);}} restore{list,state};
            result=gpu->Evaluate(f);}
        lock.lock();
        if(!mhwsr::Ok(result)) {
            Log("{\"event\":\"quality_evaluate_failed\",\"result\":"+std::to_string(unsigned(result))+",\"writes_game_output\":false}");
            g_qualityHadHistory=false;QualityFault("ngx_evaluation_failed_native_taa_fallback");return false;
        }
        ++g_qualitySucceeded;g_qualityHadHistory=true;memcpy(g_qualityLastJitter,f.jitter,8);
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);g_bridgeLists.at(list).quality.sr=true;}
        if(g_qualitySucceeded.load()==1)Log("{\"event\":\"quality_first_output_written\",\"ngx_result\":"+std::to_string(unsigned(result))+",\"native_taa_bypassed\":true,\"framegen\":false}");
        return true;
    }catch(...){QualityFault("taa_integration_exception");return false;}
}
void BeginQualityTaaTiming(ID3D12GraphicsCommandList* list,const TaaScreenInput& screen) {
    if(!g_gpuTimings||!QualityObserving()||screen.error)return;QualityBusy busy;
    try {
        std::unique_lock<std::mutex> lock(g_qualityMutex);
        if(!g_qualityGpu||list!=g_qualityList||g_abort.load())return;
        auto* gpu=g_qualityGpu;const auto phase=g_phase.load();++g_qualityRecorded;
        lock.unlock();unsigned token=UINT_MAX;
        {QualityCommands internal;token=gpu->BeginTiming(list,1,phase,{screen.words[10],screen.words[11]});}
        lock.lock();std::lock_guard<std::mutex> guard(g_bridgeMutex);g_bridgeLists.at(list).qualityTaaTiming=token;
    }catch(...){QualityFault("taa_timing_exception");}
}
bool QualityScale(float& value) {
    std::lock_guard<std::mutex> lock(g_qualityMutex);
    if(!g_qualityGpu||!g_qualityPrimed||g_qualityToneBaselines<3||(kQualityAtNativeInputs&&!g_qualityGpu->NativeInputsReady()))return false;
    value=g_qualityPlan.requestedScale;return true;
}
bool RunQualityCopy(ID3D12GraphicsCommandList* list,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box) {
    if(!QualityObserving()||!dst||!src)return false;QualityBusy busy;
    try {
        std::unique_lock<std::mutex> lock(g_qualityMutex);ListTrace state{};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto f=g_bridgeLists.find(list);if(f==g_bridgeLists.end())return false;state=f->second;}
        const auto mark=state.quality;
        if(!mark.taa||mark.stage||reinterpret_cast<uintptr_t>(src->pResource)!=mark.taa)return false;
        const bool valid=box&&!x&&!y&&!z&&dst->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&src->Type==dst->Type&&
            !dst->SubresourceIndex&&!src->SubresourceIndex&&!box->left&&!box->top&&!box->front&&box->back==1&&
            box->right==state.taaInput.width&&box->bottom==state.taaInput.height&&QualityTexture(reinterpret_cast<uintptr_t>(dst->pResource),state.taaOutput,DXGI_FORMAT_R11G11B10_FLOAT);
        if(!valid||(mark.sr&&reinterpret_cast<uintptr_t>(dst->pResource)!=g_qualityCopy)) {
            if(mark.sr)QualityFault("sr_handoff_copy_mismatch");return false;
        }
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);auto& q=g_bridgeLists.at(list).quality;q.copy=reinterpret_cast<uintptr_t>(dst->pResource);q.stage=1;}
        if(g_gpuTimings&&state.qualityTaaTiming!=UINT_MAX&&g_qualityGpu&&list==g_qualityList) {
            auto* gpu=g_qualityGpu;lock.unlock();
            {QualityCommands internal;gpu->EndTiming(list,state.qualityTaaTiming);}
            lock.lock();std::lock_guard<std::mutex> guard(g_bridgeMutex);g_bridgeLists.at(list).qualityTaaTiming=UINT_MAX;
        }
        if(!mark.sr)return false;
        D3D12_BOX full{0,0,0,state.taaOutput.width,state.taaOutput.height,1};
        g_trackedTextureCopy(list,dst,x,y,z,src,&full);++g_qualityCopies;return true;
    }catch(...){QualityFault("copy_integration_exception");return false;}
}
bool QualityVertices(const ListTrace& s,UINT vertices,UINT firstVertex,unsigned role) {
    const unsigned bytes=vertices*16;
    if(s.vertex.StrideInBytes!=16||UINT64(firstVertex)*16+bytes>s.vertex.SizeInBytes||bytes>64)return false;
    float v[16]{};UINT64 base=0;
    if(!CopyVertexBytes(s.vertex.BufferLocation+UINT64(firstVertex)*16,v,bytes,base))return false;
    for(unsigned i=0;i<bytes/4;++i)if(!std::isfinite(v[i]))return false;
    if(role==3) {
        const float ux=2*(float(s.taaInput.width)-0.5f)/float(s.taaOutput.width),uy=2*(float(s.taaInput.height)-0.5f)/float(s.taaOutput.height);
        const float expected[]={-1,1,0,0,-1,-3,0,uy,3,1,ux,0};
        for(unsigned i=0;i<12;++i)if(!Near(v[i],expected[i]))return false;
    }else if(role==1) {
        // The blur strip is column ordered (TL, BL, TR, BR); tone mapping is
        // row ordered. Both were observed, but their vertex sequences differ.
        const float expected[]={-1,1,0,0,-1,-1,0,1,1,1,1,0,1,-1,1,1};
        for(unsigned i=0;i<16;++i)if(!Near(v[i],expected[i]))return false;
    }else {
        const float ux=float(s.taaInput.width)/float(s.taaOutput.width),uy=float(s.taaInput.height)/float(s.taaOutput.height);
        const float expected[]={-1,1,0,0,1,1,ux,0,-1,-1,0,uy,1,-1,ux,uy};
        for(unsigned i=0;i<16;++i)if(!Near(v[i],expected[i]))return false;
    }
    return true;
}
void QualityTransition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    if(from==to)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};g_qualityBarriers(l,1,&b);
}
bool RunQualityDraw(ID3D12GraphicsCommandList* list,UINT vertices,UINT instances,UINT firstVertex,UINT firstInstance) {
    if(!QualityObserving())return false;QualityBusy busy;
    try {
        std::unique_lock<std::mutex> lock(g_qualityMutex);ListTrace state{};
        {std::lock_guard<std::mutex> guard(g_bridgeMutex);const auto f=g_bridgeLists.find(list);if(f==g_bridgeLists.end())return false;state=f->second;}
        if(!state.quality.taa||state.postTaaOps>3||list!=g_qualityList)return false;
        auto& q=state.quality;const auto role=QualityRole(reinterpret_cast<uintptr_t>(state.pso));
        const auto source=QualityBinding(state,list,false,0,2,0),target=QualityTarget(state);
        const bool low=state.taaInput.width<state.taaOutput.width;
        const bool sized=state.hasView&&state.hasScissor&&Near(state.view.TopLeftX,0)&&Near(state.view.TopLeftY,0)&&
            Near(state.view.Width,float(role==3?state.taaOutput.width:state.taaInput.width))&&
            Near(state.view.Height,float(role==3?state.taaOutput.height:state.taaInput.height))&&
            !state.scissor.left&&!state.scissor.top&&state.scissor.right==LONG(state.view.Width)&&state.scissor.bottom==LONG(state.view.Height);
        const bool topology=state.topology==D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP||(role==3&&state.topology==D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const bool geometry=role&&vertices==(role==3?3u:4u)&&QualityVertices(state,vertices,firstVertex,role);
        const bool textures=QualityTexture(source,state.taaOutput,DXGI_FORMAT_R11G11B10_FLOAT)&&QualityTexture(target,state.taaOutput,DXGI_FORMAT_R11G11B10_FLOAT);
        const bool chain=role&&role==q.stage&&role==state.postTaaOps&&instances==1&&!firstInstance&&sized&&topology&&textures&&geometry&&
            source==(role==1?q.copy:role==2?q.color:q.tone)&&target!=source&&
            (role!=1||target==q.color)&&(role!=3||low);
        if(!chain){
            if(q.sr)QualityFault("sr_postprocess_contract_mismatch");
            else if(role) {
                const auto reason=!geometry?"postprocess_vertices":!topology?"postprocess_topology":!sized?"postprocess_rect":!textures?"postprocess_texture":"postprocess_order";
                QualityWait(reason);
                if(g_qualityWaits[reason]==1)Log("{\"event\":\"quality_draw_guard\",\"reason\":\""+std::string(reason)+
                    "\",\"role\":"+std::to_string(role)+",\"stage\":"+std::to_string(q.stage)+",\"draw\":"+std::to_string(state.postTaaOps)+
                    ",\"topology\":"+std::to_string(unsigned(state.topology))+"}");
            }
            return false;
        }
        if(q.sr&&((role==1&&source!=g_qualityCopy)||(role==2&&target!=g_qualityTone)||(role==3&&target!=g_qualityFinal))) {
            QualityFault("sr_postprocess_resource_changed");return false;
        }
        const auto advance=[&] {std::lock_guard<std::mutex> guard(g_bridgeMutex);auto& m=g_bridgeLists.at(list).quality;m.stage=role+1;if(role==2)m.tone=target;};
        if(kQualityAtNativeInputs&&role==1&&!q.sr&&g_qualityGpu&&!g_abort.load()) {
            auto* gpu=g_qualityGpu;auto* root=reinterpret_cast<ID3D12RootSignature*>(state.textures.graphics.signature);
            if(!gpu->NativeInputsReady()) {
                lock.unlock();bool prepared=false;{QualityCommands internal;prepared=gpu->PrepareNativeInputs(root);}lock.lock();
                if(!prepared){QualityFault("native_input_preparation_pipeline_failed");return false;}
                Log("{\"event\":\"quality_native_input_pipeline_ready\",\"changes_mhwss_mode\":false}");
            }
            if(g_phase.load()==2&&g_qualityLowChain&&low) {
                auto f=state.qualityCandidate;
                const auto depth=QualityBinding(state,list,false,1,2,1),motion=QualityBinding(state,list,false,2,2,2);
                const bool scope=f.list==list&&f.color==reinterpret_cast<ID3D12Resource*>(target)&&f.color!=reinterpret_cast<ID3D12Resource*>(q.copy)&&
                    motion==reinterpret_cast<uintptr_t>(f.packedMotion)&&depth==g_qualityDepth&&
                    state.postTaaDispatches==0&&state.generation&&
                    QualityTexture(depth,state.taaOutput,DXGI_FORMAT_R32_FLOAT)&&gpu->Matches(f.packedMotion)&&
                    f.render.width==g_qualityPlan.aligned.width&&f.render.height==g_qualityPlan.aligned.height&&
                    state.taaOutput.width==g_qualityPlan.output.width&&state.taaOutput.height==g_qualityPlan.output.height;
                if(!scope){QualityWait("native_input_frame_scope");
                    if(g_qualityWaits["native_input_frame_scope"]==1){std::ostringstream out;out<<"{\"event\":\"quality_native_scope_rejected\",\"color_matches_target\":"<<(f.color==reinterpret_cast<ID3D12Resource*>(target)?"true":"false")
                        <<",\"motion_matches\":"<<(motion==reinterpret_cast<uintptr_t>(f.packedMotion)?"true":"false")<<",\"depth_matches\":"<<(depth==g_qualityDepth?"true":"false")
                        <<",\"intervening_dispatches\":"<<state.postTaaDispatches<<",\"input\":["<<f.render.width<<','<<f.render.height<<"]}";Log(out.str());}
                    return false;}
                if(std::fabs(f.jitter[0])+std::fabs(f.jitter[1])<0.00000001f){QualityWait("nonzero_projection_jitter");return false;}
                // Do not recycle upload slots or silently run beyond the bounded
                // session at unusually high frame rates. Restore before exhaustion.
                if(!gpu->NativeInputBudgetAvailable()){QualityFault("immutable_frame_budget_exhausted");return false;}
                // A is the verified native render target for this exact draw.
                // Its raw pre-TAA contents survive the T -> B copy, so sampling A
                // here does not stack TAA/DLAA onto the SR input.
                f.depth=reinterpret_cast<ID3D12Resource*>(depth);f.taaOutput=f.color;
                f.colorState=f.outputState=D3D12_RESOURCE_STATE_RENDER_TARGET;f.nativeInputs=true;f.associated=true;
                f.reset=!g_qualityHadHistory||!Near(f.previousJitter[0],g_qualityLastJitter[0])||!Near(f.previousJitter[1],g_qualityLastJitter[1]);
                ++g_qualityRecorded;++g_qualityEvaluated;
                lock.unlock();NVSDK_NGX_Result result=NVSDK_NGX_Result_FAIL_InvalidParameter;
                {QualityCommands internal;
                    struct RestoreHost {ID3D12GraphicsCommandList* list;const ListTrace& s;~RestoreHost(){QualityRestoreBindings(list,s);}} restore{list,state};
                    const auto timing=gpu->BeginTiming(list,2,2,f.render);
                    if(gpu->RecordNativeInputs(list,root,f))result=gpu->Evaluate(f);
                    gpu->EndTiming(list,timing);}
                lock.lock();
                if(!mhwsr::Ok(result)){g_qualityHadHistory=false;QualityFault("native_input_sr_evaluation_failed");
                    Log("{\"event\":\"quality_evaluate_failed\",\"result\":"+std::to_string(unsigned(result))+"}");return false;}
                ++g_qualitySucceeded;++g_qualityBlurBypasses;g_qualityHadHistory=true;memcpy(g_qualityLastJitter,f.jitter,8);
                q.sr=true;
                {std::lock_guard<std::mutex> guard(g_bridgeMutex);g_bridgeLists.at(list).quality.sr=true;}
                advance();
                if(g_qualitySucceeded.load()==1)Log("{\"event\":\"quality_first_output_written\",\"ngx_result\":"+std::to_string(unsigned(result))+
                    ",\"native_taa_bypassed\":false,\"source_color_before_taa\":true,\"native_motion_blur_bypassed\":true,\"framegen\":false}");
                return true;
            }
        }
        if(!q.sr) {
            advance();
            if(role==1)g_qualityCopy=source;
            if(role==2){g_qualityTone=target;++g_qualityToneBaselines;}
            if(role==3){g_qualityFinal=target;if(!g_qualityLowChain)Log("{\"event\":\"quality_low_chain_ready\",\"evaluates_sr\":false}");g_qualityLowChain=true;}
            return false;
        }
        QualityCommands internal;
        if(role==1) {
            auto* src=reinterpret_cast<ID3D12Resource*>(source);auto* dst=reinterpret_cast<ID3D12Resource*>(target);
            D3D12_RESOURCE_STATES a{},b{};
            if(!QualityState(list,src,a)||!QualityState(list,dst,b)){QualityFault("blur_copy_resource_state_unknown");return false;}
            lock.unlock();
            QualityTransition(list,src,a,D3D12_RESOURCE_STATE_COPY_SOURCE);QualityTransition(list,dst,b,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=src;to.pResource=dst;from.Type=to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g_trackedTextureCopy(list,&to,0,0,0,&from,nullptr);
            QualityTransition(list,src,D3D12_RESOURCE_STATE_COPY_SOURCE,a);QualityTransition(list,dst,D3D12_RESOURCE_STATE_COPY_DEST,b);
            advance();++g_qualityBlurBypasses;return true;
        }
        const D3D12_VIEWPORT full{0,0,float(state.taaOutput.width),float(state.taaOutput.height),state.view.MinDepth,state.view.MaxDepth};
        const D3D12_RECT rect{0,0,LONG(state.taaOutput.width),LONG(state.taaOutput.height)};
        const auto& vb=role==2?g_qualityGpu->Quad():g_qualityGpu->Triangle();
        lock.unlock();
        g_viewports(list,1,&full);g_textureScissor(list,1,&rect);g_vertexBuffers(list,0,1,&vb);
        g_draw(list,vertices,instances,0,firstInstance);
        g_vertexBuffers(list,0,1,&state.vertex);g_textureScissor(list,1,&state.scissor);g_viewports(list,1,&state.view);
        advance();
        if(role==2)++g_qualityToneDraws;else ++g_qualityFinalDraws;
        return true;
    }catch(...){QualityFault("draw_integration_exception");return false;}
}
void* QualityRelay(void* address) noexcept {
    __try {
        auto* p=static_cast<unsigned char*>(address);
        if(*p!=0xe9)return address;
        p+=5+*reinterpret_cast<int*>(p+1);
        if(p[0]!=0xff||p[1]!=0x25)return nullptr;
        return *reinterpret_cast<void**>(p+6+*reinterpret_cast<int*>(p+2));
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
void PrepareQualityHooks() {
    const auto root=ModulePath(nullptr).parent_path();
    const auto dll=root/L"OptiScaler"/L"nvngx_dlss.dll";
    if(FileSha256(dll)!="3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983")
        throw std::runtime_error("Quality prototype requires the inspected modern SR library");
    // A configuration/backend change requires a normal restart before this build.
    wchar_t backend[32]{};GetPrivateProfileStringW(L"Upscalers",L"Dx12Upscaler",L"",backend,32,(root/L"OptiScaler.ini").c_str());
    if(_wcsicmp(backend,L"dlss"))throw std::runtime_error("Quality prototype requires the DLSS backend, not native");
    const auto create=*reinterpret_cast<void**>(g_mhwss+0x559af8),evaluate=*reinterpret_cast<void**>(g_mhwss+0x559b00),
        release=*reinterpret_cast<void**>(g_mhwss+0x559b08),allocate=*reinterpret_cast<void**>(g_mhwss+0x559b18);
    if(!KnownNgxPointer(create,true)||!KnownNgxPointer(evaluate,true)||!KnownNgxPointer(release,false)||!KnownNgxPointer(allocate,false))
        throw std::runtime_error("Quality NGX dispatch chain differs");
    g_ngx.create=reinterpret_cast<mhwsr::Create>(create);g_ngx.evaluate=reinterpret_cast<mhwsr::Evaluate>(evaluate);
    g_ngx.release=reinterpret_cast<mhwsr::Release>(release);g_ngx.allocate=reinterpret_cast<mhwsr::Parameters>(allocate);
    auto* core=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"D3D12Core.dll"));
    HMODULE proxy{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(allocate),&proxy)||
       FileSha256(ModulePath(proxy))!=g_expectedProxyHash)throw std::runtime_error("Quality proxy owner differs");
    auto* execute=reinterpret_cast<unsigned char*>(proxy)+0x175a70;
    if(QualityRelay(core+0xb58f0)!=execute)throw std::runtime_error("Queue submission relay differs");
    const std::array<unsigned char,16> barrier{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83};
    const std::array<unsigned char,16> submit{0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    BridgeHook(32,core+0x1301d0,barrier,reinterpret_cast<void*>(&OnQualityBarriers),g_qualityBarriers);
    BridgeHook(33,execute,submit,reinterpret_cast<void*>(&OnQualityExecute),g_qualityExecute);
    const std::array<unsigned char,16> topology{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd1,0x48,0x8b,0x41,0x58,0x4c,0x8b,0x0d,0x5e,0xa1};
    BridgeHook(34,core+0x12ae90,topology,reinterpret_cast<void*>(&OnQualityTopology),g_qualityTopology);
    if(!kQualityAtNativeInputs) {
    auto* indexed=reinterpret_cast<unsigned char*>(proxy)+0x178f60;
    if(QualityRelay(core+0x12c2c0)!=indexed)throw std::runtime_error("Indexed draw relay differs");
    const std::array<unsigned char,16> indexedSignature{0x40,0x55,0x53,0x56,0x57,0x48,0x8d,0x6c,0x24,0xe8,0x48,0x81,0xec,0x18,0x01,0x00};
    BridgeHook(35,indexed,indexedSignature,reinterpret_cast<void*>(&OnQualityIndexed),g_qualityIndexed);
    }
    Log("{\"event\":\"quality_prototype_prepared\",\"new_ngx_context\":false,\"changes_mhwss_mode\":false,\"framegen\":false}");
}
void LogQualityTimings(mhwsr::QualityGpu* gpu,ID3D12CommandQueue* queue) {
    if(!g_gpuTimings)return;
    UINT64 frequency=0;std::vector<mhwsr::QualityTiming> samples;
    if(!queue||FAILED(queue->GetTimestampFrequency(&frequency))||!gpu->ReadTimings(frequency,samples)) {
        Log("{\"event\":\"quality_gpu_timings_unavailable\"}");return;
    }
    std::map<std::tuple<unsigned,unsigned,unsigned,unsigned>,std::vector<double>> groups;
    for(const auto& s:samples)groups[{s.kind,s.phase,s.input.width,s.input.height}].push_back(s.milliseconds);
    for(auto& pair:groups) {
        auto& values=pair.second;std::sort(values.begin(),values.end());double total=0;for(auto v:values)total+=v;
        const auto n=values.size();std::ostringstream out;out<<std::setprecision(9);
        out<<"{\"event\":\"quality_gpu_timing\",\"scope\":\""<<(std::get<0>(pair.first)==1?"native_taa_to_copy":"sr_inputs_evaluate_copy")
            <<"\",\"phase\":"<<std::get<1>(pair.first)<<",\"input\":["<<std::get<2>(pair.first)<<','<<std::get<3>(pair.first)
            <<"],\"samples\":"<<n<<",\"mean_ms\":"<<total/double(n)<<",\"median_ms\":"<<(values[(n-1)/2]+values[n/2])*0.5
            <<",\"p95_ms\":"<<values[(n-1)*95/100]<<",\"queue_frequency\":"<<frequency<<",\"whole_frame_gpu_time\":false}";
        Log(out.str());
    }
}
void FinishQuality() {
    // Hooks remain available while draining. No GPU timeout permits a release.
    const auto deadline=GetTickCount64()+5000;bool released=false;
    while(GetTickCount64()<deadline) {
        {
            std::unique_lock<std::mutex> lock(g_qualityMutex);
            if(!g_qualityGpu&&!g_qualityBusy.load()){released=true;break;}
            if(g_qualityGpu) {
                if(g_qualityRetain)break;
                const auto completed=g_qualityGpu->Fence()->GetCompletedValue();
                if(!g_qualityBusy.load()&&g_qualityRecorded==g_qualitySubmitted&&completed!=UINT64_MAX&&completed>=g_qualityFenceValue) {
                    auto* retiring=g_qualityGpu;const auto queue=g_qualityQueue;g_qualityGpu=nullptr;lock.unlock();
                    LogQualityTimings(retiring,queue.Get());
                    if(retiring->ReleaseAfterGpu()){delete retiring;released=true;}
                    else {lock.lock();g_qualityGpu=retiring;g_qualityRetain=true;}
                    break;
                }
            }
        }
        Sleep(25);
    }
    Log("{\"event\":\"quality_summary\",\"evaluated\":"+std::to_string(g_qualityEvaluated.load())+",\"successful\":"+std::to_string(g_qualitySucceeded.load())+
        ",\"expanded_copies\":"+std::to_string(g_qualityCopies.load())+",\"blur_bypasses\":"+std::to_string(g_qualityBlurBypasses.load())+
        ",\"full_tone_draws\":"+std::to_string(g_qualityToneDraws.load())+",\"full_final_draws\":"+std::to_string(g_qualityFinalDraws.load())+
        ",\"depth_copy_associations\":"+std::to_string(g_qualityDepthCopyAssociations)+",\"prepared_depth_associations\":"+std::to_string(g_qualityPreparedDepthAssociations)+
        ",\"raster_depth_snapshots\":"+std::to_string(g_qualityDepthSnapshots)+",\"raster_depth_associations\":"+std::to_string(g_qualityRasterAssociations)+
        ",\"gpu_objects_released\":"+(released?"true":"false")+",\"retained_until_exit\":"+(released?"false":"true")+",\"performance_validated\":false}");
    std::lock_guard<std::mutex> lock(g_qualityMutex);
    for(const auto& item:g_qualityWaits)Log("{\"event\":\"quality_wait_count\",\"reason\":\""+item.first+"\",\"count\":"+std::to_string(item.second)+"}");
}
