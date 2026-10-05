// Observe the game's descriptor-binding packet, then confirm each copy at the
// existing D3D12 API chain. No descriptor bytes or rendering arguments change.
struct TextureIdentity {uintptr_t resource=0;UINT64 width=0;unsigned height=0,format=0,flags=0;};
struct TextureCopySource {
    ID3D12Resource* reference{};TextureIdentity texture{};UINT64 source=0,destination=0;
    unsigned kind=0,slot=0;
    uintptr_t list=0;uint64_t pass=0;UINT64 tableBase=0;
};
struct TextureUnresolved {unsigned kind=0,slot=0;uintptr_t wrapper=0,allocation=0,view=0,resource=0,method=0;std::array<UINT64,6> allocationWords{};};
struct TexturePacket {ID3D12GraphicsCommandList* list{};uint64_t pass=0;uintptr_t pso=0,program=0;std::array<TextureCopySource,64> entries{};unsigned count=0;
    std::array<TextureUnresolved,16> unresolved{};unsigned unresolvedCount=0;};
using TextureAllocation=unsigned char*(__fastcall*)(void*,void*);
TextureAllocation g_textureAllocation{};
using EngineTextureBind=uintptr_t(__fastcall*)(void*,void*,void*);
using EngineTargets=uintptr_t(__fastcall*)(void*,void*,void*,void*);
using TextureCopy=void(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
using TextureHeaps=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*);
using TextureTable=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE);
using TextureSignature=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
using TextureTargets=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
using TextureScissor=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*);
TextureScissor g_textureScissor{};
ComputeCbv g_textureGraphicsCbv{};
EngineTextureBind g_engineTextureBind0{},g_engineTextureBind1{},g_engineTextureComputeBind{};EngineTargets g_engineTargets{};
TextureCopy g_textureCopy{};TextureHeaps g_textureHeaps{};TextureTargets g_textureTargets{};
TextureTable g_textureComputeTable{},g_textureGraphicsTable{};
TextureSignature g_textureComputeSignature{},g_textureGraphicsSignature{};
std::mutex g_textureMutex;
std::unordered_map<UINT64,TextureCopySource> g_textureDescriptors;
std::unordered_map<UINT64,TextureIdentity> g_textureRtvs;
std::unordered_map<UINT64,TextureIdentity> g_textureDsvs;
std::set<std::string> g_textureSnapshotKinds;
std::set<std::tuple<uintptr_t,unsigned,unsigned>> g_unresolvedTextureKinds;
std::atomic<uint64_t> g_textureBinderCalls{0},g_texturePacketFailures{0},g_confirmedTextureCopies{0};
thread_local const TexturePacket* g_texturePacket{};
UINT g_textureRtvStride=0;
std::map<std::pair<uintptr_t,unsigned>,std::vector<unsigned char>> g_textureShaders;
size_t g_textureShaderBytes=0;
bool ReadShaderSlice(uintptr_t program,unsigned stage,const void** data,unsigned* bytes) noexcept {
    __try {
        const unsigned offsets[]={0x50,0x58,0x78};if(stage>=3||!program)return false;
        auto* shader=*reinterpret_cast<unsigned char**>(program+offsets[stage]);if(!shader)return false;
        auto* blob=*reinterpret_cast<unsigned char**>(shader+0x10);if(!blob)return false;
        const auto size=*reinterpret_cast<unsigned*>(blob);const auto* code=*reinterpret_cast<unsigned char**>(blob+8);
        if(!code||size<32||size>1048576||memcmp(code,"DXBC",4)||*reinterpret_cast<const unsigned*>(code+24)!=size)return false;
        *data=code;*bytes=size;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void CaptureTextureShaders(const TexturePacket& packet) {
    std::lock_guard<std::mutex> lock(g_textureMutex);
    if(!(g_ready.load()&&!g_qualityCommands)||g_done.load())return;
    for(unsigned stage=0;stage<3;++stage) {
        const auto key=std::make_pair(packet.pso,stage);
        if(g_textureShaders.count(key)||g_textureShaders.size()>=64)continue;
        const void* data=nullptr;unsigned bytes=0;
        if(ReadShaderSlice(packet.program,stage,&data,&bytes)&&g_textureShaderBytes+bytes<=4194304) {
            std::vector<unsigned char> copy(bytes);
            if(CopyConstantBytes(data,copy.data(),bytes)){g_textureShaderBytes+=bytes;g_textureShaders.emplace(key,std::move(copy));}
        }
    }
}
void SaveTextureShaders() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_textureMutex);if(g_textureShaders.empty())return;
        const auto folder=ModulePath(g_self).parent_path()/L"shaders";std::filesystem::create_directories(folder);
        const char* stages[]={"vs","ps","cs"};
        for(const auto& entry:g_textureShaders) {
            std::ostringstream name;name<<"pso-"<<std::hex<<entry.first.first<<'-'<<stages[entry.first.second]<<".dxbc";
            const auto path=folder/name.str();{std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(entry.second.data()),entry.second.size());if(!file)continue;}
            std::ostringstream line;line<<"{\"event\":\"texture_shader_saved\",\"pso\":\"0x"<<std::hex<<entry.first.first<<std::dec<<"\",\"stage\":\""
                <<stages[entry.first.second]<<"\",\"file\":\""<<name.str()<<"\",\"bytes\":"<<entry.second.size()<<",\"sha256\":\""<<FileSha256(path)<<"\"}";Log(line.str());
        }
    }catch(...){}
}

ID3D12GraphicsCommandList* EngineTextureList(void* backend) noexcept {
    __try {
        if(!backend)return nullptr;
        const auto* state=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(backend)+8);
        if(!state)return nullptr;
        auto* list=*reinterpret_cast<ID3D12GraphicsCommandList* const*>(state+0x20);
        if(list&&(*reinterpret_cast<void***>(list))[44]==g_mhwss+0x1578d0)
            list=*reinterpret_cast<ID3D12GraphicsCommandList**>(reinterpret_cast<unsigned char*>(list)+8);
        return list;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
bool TexturePassRelevant(ID3D12GraphicsCommandList* list) {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto found=g_bridgeLists.find(list);
    return found!=g_bridgeLists.end()&&(NativeTaaPso(found->second.pso)||found->second.qualityDepthRenderPending||(found->second.taaSerial&&found->second.postTaaOps<3));
}
bool ReferenceTexture(ID3D12Resource* resource,TextureCopySource* out) noexcept {
    __try {
        if(!resource)return false;
        auto* core=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"D3D12Core.dll"));
        if((*reinterpret_cast<void***>(resource))[11]!=core+0x10d230)return false;
        resource->AddRef();out->reference=resource;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void UnresolvedTexture(TexturePacket* packet,unsigned kind,unsigned slot,void* wrapper,void* allocation,void* view,ID3D12Resource* resource) noexcept {
    if(packet->unresolvedCount>=packet->unresolved.size())return;
    auto& record=packet->unresolved[packet->unresolvedCount++];record.kind=kind;record.slot=slot;
    record.wrapper=reinterpret_cast<uintptr_t>(wrapper);record.allocation=reinterpret_cast<uintptr_t>(allocation);
    record.view=reinterpret_cast<uintptr_t>(view);record.resource=reinterpret_cast<uintptr_t>(resource);
    __try {if(allocation)memcpy(record.allocationWords.data(),allocation,sizeof(record.allocationWords));
        if(resource)record.method=reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(resource))[11]);}__except(EXCEPTION_EXECUTE_HANDLER){}
}
bool ReadTexturePacket(void* backend,void* request,TexturePacket* out) noexcept {
    __try {
        auto* p=static_cast<unsigned char*>(request);if(!p||!backend)return false;
        auto* bindings=*reinterpret_cast<unsigned char**>(p+0x10);
        auto* range=*reinterpret_cast<unsigned short**>(p+0x18);
        if(!bindings||!range||range[0]>128||range[1]>8192)return false;
        auto* header=*reinterpret_cast<unsigned char**>(bindings+8);if(!header)return false;
        auto** block=*reinterpret_cast<unsigned char***>(header);if(!block||!*block)return false;
        out->program=reinterpret_cast<uintptr_t>(block);
        auto* tokens=reinterpret_cast<uint32_t*>(*block);
        auto* context=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(backend)+8);if(!context)return false;
        const auto cpu=*reinterpret_cast<UINT64*>(context+8);
        const auto stride=*reinterpret_cast<unsigned*>(context+0x40),used=*reinterpret_cast<unsigned*>(context+0x120);
        if(!cpu||!stride||stride>128||used>1048576)return false;
        for(unsigned i=0;i<range[0];++i) {
            const auto token=tokens[range[1]+i],kind=token&15,slot=token>>24,index=(token>>4)&255;
            if((kind!=2&&kind!=3)||slot>=32)continue;
            if(out->count>=out->entries.size())return false;
            auto* wrapper=*reinterpret_cast<unsigned char**>(bindings+0x30+index*8);if(!wrapper)continue;
            auto* view=*reinterpret_cast<unsigned char**>(wrapper+(kind==2?0x78:0x80));
            auto* allocation=*reinterpret_cast<unsigned char**>(wrapper+0x60);
            if(!allocation&&g_textureAllocation)allocation=g_textureAllocation(wrapper,*reinterpret_cast<void**>(p));
            if(!view||!allocation){UnresolvedTexture(out,kind,slot,wrapper,allocation,view,nullptr);continue;}
            const auto handle=*reinterpret_cast<UINT64*>(view+0x10);
            const auto descriptor=int64_t(used)+int64_t(*reinterpret_cast<int*>(p+(kind==2?0x24:0x28)))+slot;
            if(!handle||descriptor<0||descriptor>1048576)continue;
            auto& entry=out->entries[out->count];
            auto* resource=*reinterpret_cast<ID3D12Resource**>(allocation+0x20);
            if(!ReferenceTexture(resource,&entry)){UnresolvedTexture(out,kind,slot,wrapper,allocation,view,resource);continue;}
            entry.source=handle;entry.destination=cpu+UINT64(descriptor)*stride;entry.kind=kind;entry.slot=slot;++out->count;
            entry.list=reinterpret_cast<uintptr_t>(out->list);entry.pass=out->pass;entry.tableBase=cpu+UINT64(used)*stride;
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void DescribeTexturePacket(TexturePacket& packet) {
    for(unsigned i=0;i<packet.count;++i) {
        auto& entry=packet.entries[i];const auto d=entry.reference->GetDesc();
        if(d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width>=1&&d.Width<=16384&&d.Height>=1&&d.Height<=16384)
            entry.texture={reinterpret_cast<uintptr_t>(entry.reference),d.Width,d.Height,unsigned(d.Format),unsigned(d.Flags)};
    }
    for(unsigned i=0;i<packet.unresolvedCount;++i) {
        const auto& e=packet.unresolved[i];const auto key=std::make_tuple(packet.pso,e.kind,e.slot);
        {std::lock_guard<std::mutex> lock(g_textureMutex);if(g_unresolvedTextureKinds.count(key)||g_unresolvedTextureKinds.size()>=32)continue;g_unresolvedTextureKinds.insert(key);}
        std::ostringstream out;out<<"{\"event\":\"unresolved_texture_binding\",\"pso\":\"0x"<<std::hex<<packet.pso<<std::dec<<"\",\"kind\":"<<e.kind<<",\"slot\":"<<e.slot
            <<",\"wrapper\":\"0x"<<std::hex<<e.wrapper<<"\",\"allocation\":\"0x"<<e.allocation<<"\",\"view\":\"0x"<<e.view<<"\",\"resource_candidate\":\"0x"<<e.resource
            <<"\",\"gpuva_method_candidate\":\"0x"<<e.method<<"\",\"allocation_words\":[";
        for(unsigned j=0;j<e.allocationWords.size();++j){if(j)out<<',';out<<"\"0x"<<e.allocationWords[j]<<"\"";}out<<"],\"invokes_unverified_resource\":false}";SaveBridge(out.str());
    }
}
struct TexturePacketOwner {
    TexturePacket packet{};const TexturePacket* previous=g_texturePacket;
    ~TexturePacketOwner(){g_texturePacket=previous;for(unsigned i=0;i<packet.count;++i)if(packet.entries[i].reference)packet.entries[i].reference->Release();}
};
uintptr_t ObserveEngineTextureBind(EngineTextureBind next,void* backend,void* request,void* result) {
    TexturePacketOwner scope;
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        auto* list=EngineTextureList(backend);
        if(list&&TexturePassRelevant(list)) {
            ++g_textureBinderCalls;scope.packet.list=list;
            {std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto& state=g_bridgeLists.at(list);scope.packet.pass=state.texturePassId;scope.packet.pso=reinterpret_cast<uintptr_t>(state.pso);}
            if(ReadTexturePacket(backend,request,&scope.packet)){DescribeTexturePacket(scope.packet);CaptureTextureShaders(scope.packet);g_texturePacket=&scope.packet;}
            else ++g_texturePacketFailures;
        }
    }catch(...){++g_traceDrops;}
    return next(backend,request,result);
}
uintptr_t __fastcall OnEngineTextureBind0(void* a,void* b,void* c){return ObserveEngineTextureBind(g_engineTextureBind0,a,b,c);}
uintptr_t __fastcall OnEngineTextureBind1(void* a,void* b,void* c){return ObserveEngineTextureBind(g_engineTextureBind1,a,b,c);}
uintptr_t __fastcall OnEngineTextureComputeBind(void* a,void* b,void* c){return ObserveEngineTextureBind(g_engineTextureComputeBind,a,b,c);}
void STDMETHODCALLTYPE OnTextureCopy(ID3D12Device* device,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE destination,D3D12_CPU_DESCRIPTOR_HANDLE source,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    g_textureCopy(device,count,destination,source,type);
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)try {
        const auto stride=device->GetDescriptorHandleIncrementSize(type);
        std::lock_guard<std::mutex> lock(g_textureMutex);
        if(count>4096){g_textureDescriptors.clear();++g_traceDrops;return;}
        for(unsigned i=0;i<count;++i)g_textureDescriptors.erase(destination.ptr+UINT64(i)*stride);
        if(count!=1||!g_texturePacket)return;
        for(unsigned i=0;i<g_texturePacket->count;++i) {
            const auto& e=g_texturePacket->entries[i];
            if(e.texture.resource&&e.source==source.ptr&&e.destination==destination.ptr) {
                if(g_textureDescriptors.size()<32768) {
                    auto copy=e;copy.reference=nullptr;g_textureDescriptors[destination.ptr]=copy;++g_confirmedTextureCopies;
                }else ++g_traceDrops;
                break;
            }
        }
    }catch(...){++g_traceDrops;}
}
void STDMETHODCALLTYPE OnTextureHeaps(ID3D12GraphicsCommandList* list,UINT count,ID3D12DescriptorHeap* const* heaps) {
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&count<=2)try {
        TextureHeap info{};
        for(unsigned i=0;heaps&&i<count;++i)if(heaps[i]) {
            const auto d=heaps[i]->GetDesc();
            if(d.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)continue;
            ID3D12Device* device=nullptr;if(FAILED(heaps[i]->GetDevice(IID_PPV_ARGS(&device))))continue;
            const auto stride=device->GetDescriptorHandleIncrementSize(d.Type);device->Release();
            info={heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr,heaps[i]->GetCPUDescriptorHandleForHeapStart().ptr,d.NumDescriptors,stride};
        }
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)) {
            auto& t=g_bridgeLists[list].textures;
            if(t.heap.gpu!=info.gpu||t.heap.cpu!=info.cpu){t.compute.tables={};t.graphics.tables={};}
            t.heap=info;
            t.heapCount=count;t.heaps={};for(unsigned i=0;heaps&&i<count;++i)t.heaps[i]=heaps[i];
        }
    }catch(...){++g_traceDrops;}
    g_textureHeaps(list,count,heaps);
}
void TrackTextureTable(ID3D12GraphicsCommandList* list,UINT root,UINT64 value,bool compute) {
    if(!(g_ready.load()&&!g_qualityCommands)||g_done.load()||root>=32)return;
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    if(g_bridgeLists.size()<512||g_bridgeLists.count(list)) {
        auto& t=g_bridgeLists[list].textures;(compute?t.compute:t.graphics).tables[root]=value;
    }
}
void STDMETHODCALLTYPE OnTextureComputeTable(ID3D12GraphicsCommandList* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){try{TrackTextureTable(l,i,h.ptr,true);}catch(...){++g_traceDrops;}g_textureComputeTable(l,i,h);}
void STDMETHODCALLTYPE OnTextureGraphicsTable(ID3D12GraphicsCommandList* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){try{TrackTextureTable(l,i,h.ptr,false);}catch(...){++g_traceDrops;}g_textureGraphicsTable(l,i,h);}
void TrackTextureSignature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* signature,bool compute) {
    if(!(g_ready.load()&&!g_qualityCommands)||g_done.load())return;
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    if(g_bridgeLists.size()<512||g_bridgeLists.count(list)) {
        auto& t=g_bridgeLists[list].textures;auto& roots=compute?t.compute:t.graphics;
        if(roots.signature!=reinterpret_cast<uintptr_t>(signature)){roots.tables={};roots.cbvs={};}
        roots.signature=reinterpret_cast<uintptr_t>(signature);
    }
}
void STDMETHODCALLTYPE OnTextureComputeSignature(ID3D12GraphicsCommandList* l,ID3D12RootSignature* s){try{TrackTextureSignature(l,s,true);}catch(...){++g_traceDrops;}g_textureComputeSignature(l,s);}
void STDMETHODCALLTYPE OnTextureGraphicsSignature(ID3D12GraphicsCommandList* l,ID3D12RootSignature* s){try{TrackTextureSignature(l,s,false);}catch(...){++g_traceDrops;}g_textureGraphicsSignature(l,s);}
void STDMETHODCALLTYPE OnTextureGraphicsCbv(ID3D12GraphicsCommandList* list,UINT root,UINT64 address) {
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&root<32)try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list))g_bridgeLists[list].textures.graphics.cbvs[root]=address;
    }catch(...){++g_traceDrops;}
    g_textureGraphicsCbv(list,root,address);
}

bool ReadEngineTargets(void* input,TexturePacket* out) noexcept {
    __try {
        if(!input)return false;auto* p=static_cast<unsigned char*>(input);const auto count=*reinterpret_cast<unsigned*>(p+0x48)&7;
        for(unsigned i=0;i<count;++i) {
            auto* wrapper=*reinterpret_cast<unsigned char**>(p+i*8);if(!wrapper)continue;
            auto* view=*reinterpret_cast<unsigned char**>(wrapper+0x60);if(!view||view[0x28])continue;
            const auto handle=*reinterpret_cast<UINT64*>(view+0x10);
            auto& e=out->entries[out->count];
            if(!handle||!ReferenceTexture(*reinterpret_cast<ID3D12Resource**>(view+0x18),&e))continue;
            e.source=handle;e.slot=i;++out->count;
        }
        // Candidate DSV slot is separately confirmed against the actual API DSV
        // handle. Never invoke an unknown COM pointer: require the depth identity
        // already observed as the source of a main-depth copy.
        if(g_qualityMode&&g_qualityRasterDepthAnchor.load()) {
            auto* wrapper=*reinterpret_cast<unsigned char**>(p+0x40);
            auto* view=wrapper?*reinterpret_cast<unsigned char**>(wrapper+0x60):nullptr;
            if(view) {
                const auto handle=*reinterpret_cast<UINT64*>(view+0x10);
                auto* resource=*reinterpret_cast<ID3D12Resource**>(view+0x18);
                if(handle&&reinterpret_cast<uintptr_t>(resource)==g_qualityRasterDepthAnchor.load()) {
                    auto& e=out->entries[out->count];
                    if(ReferenceTexture(resource,&e)){e.source=handle;e.kind=4;++out->count;}
                }
            }
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uintptr_t __fastcall OnEngineTargets(void* a,void* b,void* c,void* d) {
    TexturePacketOwner owner;
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        const auto list=EngineTextureList(a);
        if(list&&TexturePassRelevant(list)&&ReadEngineTargets(c,&owner.packet)) {
            DescribeTexturePacket(owner.packet);std::lock_guard<std::mutex> lock(g_textureMutex);
            for(unsigned i=0;i<owner.packet.count;++i){const auto& e=owner.packet.entries[i];
                auto& views=e.kind==4?g_textureDsvs:g_textureRtvs;if(e.texture.resource&&views.size()<4096)views[e.source]=e.texture;}
        }
    }catch(...){++g_traceDrops;}
    return g_engineTargets(a,b,c,d);
}
void STDMETHODCALLTYPE OnTextureTargets(ID3D12GraphicsCommandList* list,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL consecutive,const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    if(g_qualityMode)CaptureQualitySceneDepth(list,count,handles,consecutive);
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load()&&count<=8)try {
        std::array<UINT64,8> values{};
        if(handles)for(unsigned i=0;i<count;++i)values[i]=consecutive?handles[0].ptr+UINT64(i)*g_textureRtvStride:handles[i].ptr;
        uintptr_t depthResource=0;
        if(depth){std::lock_guard<std::mutex> lock(g_textureMutex);const auto f=g_textureDsvs.find(depth->ptr);if(f!=g_textureDsvs.end())depthResource=f->second.resource;}
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){auto& t=g_bridgeLists[list].textures;t.targets=values;t.targetCount=count;t.depthResource=depthResource;t.depthHandle=depth?depth->ptr:0;}
    }catch(...){++g_traceDrops;}
    g_textureTargets(list,count,handles,consecutive,depth);
}
void STDMETHODCALLTYPE OnTextureScissor(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* rects) {
    if((g_ready.load()&&!g_qualityCommands)&&!g_done.load())try {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if(g_bridgeLists.size()<512||g_bridgeLists.count(list)){auto& s=g_bridgeLists[list];s.hasScissor=count==1&&rects;if(s.hasScissor)s.scissor=rects[0];}
    }catch(...){++g_traceDrops;}
    g_textureScissor(list,count,rects);
}
void WriteTextureIdentity(std::ostream& out,const TextureIdentity& t) {
    out<<"\"resource\":\"0x"<<std::hex<<t.resource<<std::dec<<"\",\"size\":["<<t.width<<','<<t.height<<"],\"format\":"<<t.format<<",\"flags\":"<<t.flags;
}
void RecordTextureSnapshot(ID3D12GraphicsCommandList* list,bool compute,uint64_t serial,unsigned drawIndex,unsigned vertices,const char* stage=nullptr,UINT x=0,UINT y=0,UINT z=0) {
    if(!g_traceTextures||!(g_ready.load()&&!g_qualityCommands)||g_done.load())return;
    ListTrace state{};
    {std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto f=g_bridgeLists.find(list);if(f==g_bridgeLists.end())return;state=f->second;}
    const auto& heap=state.textures.heap;const auto& roots=compute?state.textures.compute:state.textures.graphics;
    struct Bound {TextureCopySource copy;unsigned root=0,offset=0;};
    std::vector<Bound> bound;std::array<TextureIdentity,8> targets{};
    {
        std::lock_guard<std::mutex> lock(g_textureMutex);
        if(heap.gpu&&heap.cpu&&heap.stride&&heap.count<=1048576)for(unsigned root=0;root<roots.tables.size();++root) {
            const auto table=roots.tables[root];
            if(!table||table<heap.gpu||(table-heap.gpu)%heap.stride)continue;
            const auto index=(table-heap.gpu)/heap.stride;if(index>=heap.count)continue;
            // Compute uses a mixed table: UAVs precede SRVs. Keep shader register
            // and table offset distinct, and require the engine packet's table
            // base plus a copy recorded for THIS pipeline binding and list.
            for(unsigned slot=0;slot<64&&index+slot<heap.count;++slot) {
                const auto cpu=heap.cpu+(index+slot)*heap.stride;const auto f=g_textureDescriptors.find(cpu);
                if(f!=g_textureDescriptors.end()&&f->second.tableBase==heap.cpu+index*heap.stride&&
                   f->second.list==reinterpret_cast<uintptr_t>(list)&&state.texturePassId&&f->second.pass==state.texturePassId)
                    bound.push_back({f->second,root,slot});
            }
        }
        if(!compute)for(unsigned i=0;i<state.textures.targetCount;++i){const auto f=g_textureRtvs.find(state.textures.targets[i]);if(f!=g_textureRtvs.end())targets[i]=f->second;}
    }
    const auto* at=stage?stage:(compute?"taa":"post_taa_draw");
    std::ostringstream key;key<<at<<':'<<drawIndex<<':'<<state.pso<<':'<<state.taaInput.width<<':'<<state.taaInput.height<<':'<<vertices;
    for(const auto& b:bound)key<<':'<<b.root<<':'<<b.offset<<':'<<b.copy.kind<<':'<<b.copy.texture.resource;
    for(const auto& t:targets)key<<':'<<t.resource;
    {std::lock_guard<std::mutex> lock(g_textureMutex);if(!state.detailedTextureFrame&&(g_textureSnapshotKinds.count(key.str())||g_textureSnapshotKinds.size()>=128))return;g_textureSnapshotKinds.insert(key.str());}
    std::ostringstream out;out<<std::setprecision(9)<<"{\"event\":\"texture_binding_snapshot\",\"at\":\""<<at
       <<"\",\"taa_serial\":"<<serial<<",\"draw_index\":"<<drawIndex<<",\"vertices\":"<<vertices<<",\"list\":\"0x"<<std::hex<<reinterpret_cast<uintptr_t>(list)
       <<"\",\"pso\":\"0x"<<reinterpret_cast<uintptr_t>(state.pso)<<"\",\"root_signature\":\"0x"<<roots.signature<<std::dec<<"\",\"input\":["
       <<state.taaInput.width<<','<<state.taaInput.height<<"],\"output\":["<<state.taaOutput.width<<','<<state.taaOutput.height<<"],\"viewport\":["
       <<state.view.TopLeftX<<','<<state.view.TopLeftY<<','<<state.view.Width<<','<<state.view.Height<<"],\"bindings\":[";
    bool first=true;for(const auto& b:bound){if(!first)out<<',';first=false;out<<"{\"root\":"<<b.root<<",\"table_offset\":"<<b.offset<<",\"slot\":"<<b.copy.slot<<",\"kind\":\""<<(b.copy.kind==2?"srv":"uav")<<"\",";
        WriteTextureIdentity(out,b.copy.texture);out<<",\"copied_source_cpu\":\"0x"<<std::hex<<b.copy.source<<"\",\"copied_destination_cpu\":\"0x"<<b.copy.destination<<std::dec<<"\"}";}
    out<<"],\"targets\":[";first=true;for(unsigned i=0;!compute&&i<state.textures.targetCount;++i){if(!first)out<<',';first=false;out<<"{\"slot\":"<<i<<',';WriteTextureIdentity(out,targets[i]);out<<'}';}
    out<<"],\"bound_tables\":[";for(unsigned i=0;i<roots.tables.size();++i){if(i)out<<',';out<<"\"0x"<<std::hex<<roots.tables[i]<<std::dec<<"\"";}
    out<<"],\"scissor\":";if(state.hasScissor)out<<'['<<state.scissor.left<<','<<state.scissor.top<<','<<state.scissor.right<<','<<state.scissor.bottom<<']';else out<<"null";
    if(!compute&&state.detailedTextureFrame) {
        std::array<uint32_t,16> words{};bool copied=false;const auto address=roots.cbvs[5];
        {std::lock_guard<std::mutex> lock(g_screenReadMutex);
            if(g_screenMapped&&address>=g_screenGpuBase&&address-g_screenGpuBase<=g_screenBytes-sizeof(words))
                copied=CopyConstantBytes(g_screenMapped+(address-g_screenGpuBase),words.data(),sizeof(words));}
        out<<",\"graphics_cbv5_copied\":"<<(copied?"true":"false")<<",\"graphics_cbv5_raw_words\":[";
        if(copied)for(unsigned i=0;i<words.size();++i){if(i)out<<',';out<<words[i];}out<<']';
    }
    out<<",\"dispatch\":["<<x<<','<<y<<','<<z<<"],\"detailed_frame\":"<<(state.detailedTextureFrame?"true":"false")
        <<",\"pipeline_binding_serial\":"<<state.texturePassId<<",\"requires_same_pipeline_binding_copy\":true,\"original_game_bindings\":true}";SaveBridge(out.str());
}
#include "BridgeTextureCopies.inl"
