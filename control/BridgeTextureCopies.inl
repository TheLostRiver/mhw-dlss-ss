using TrackedTextureCopy=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
using TrackedResourceCopy=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*);
TrackedTextureCopy g_trackedTextureCopy{};TrackedResourceCopy g_trackedResourceCopy{};
std::atomic<uint64_t> g_postTaaTextureCopies{0};std::set<std::string> g_textureCopyKinds;
void RecordPostTaaCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,ID3D12Resource* source,
    const D3D12_TEXTURE_COPY_LOCATION* dst,const D3D12_TEXTURE_COPY_LOCATION* src,UINT x,UINT y,UINT z,const D3D12_BOX* box) {
    ListTrace state{};
    {std::lock_guard<std::mutex> lock(g_bridgeMutex);const auto f=g_bridgeLists.find(list);
        if(f==g_bridgeLists.end()||!f->second.taaSerial||f->second.postTaaOps>=3)return;state=f->second;}
    if(!destination||!source)return;
    const auto a=source->GetDesc(),b=destination->GetDesc();
    if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||b.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)return;
    ++g_postTaaTextureCopies;
    std::ostringstream key;key<<source<<':'<<destination<<':'<<state.taaInput.width<<':'<<state.taaInput.height<<':'<<x<<':'<<y<<':'<<z;
    if(box)key<<':'<<box->left<<':'<<box->top<<':'<<box->front<<':'<<box->right<<':'<<box->bottom<<':'<<box->back;
    {std::lock_guard<std::mutex> lock(g_textureMutex);if(!state.detailedTextureFrame&&(g_textureCopyKinds.count(key.str())||g_textureCopyKinds.size()>=64))return;g_textureCopyKinds.insert(key.str());}
    std::ostringstream out;out<<"{\"event\":\"post_taa_texture_copy\",\"taa_serial\":"<<state.taaSerial<<",\"after_draw\":"<<state.postTaaOps
        <<",\"after_dispatch\":"<<state.postTaaDispatches<<",\"whole_resource\":"<<(dst?"false":"true")<<",\"input\":["<<state.taaInput.width<<','<<state.taaInput.height
        <<"],\"source\":{";WriteTextureIdentity(out,{reinterpret_cast<uintptr_t>(source),a.Width,a.Height,unsigned(a.Format),unsigned(a.Flags)});
    out<<"},\"destination\":{";WriteTextureIdentity(out,{reinterpret_cast<uintptr_t>(destination),b.Width,b.Height,unsigned(b.Format),unsigned(b.Flags)});
    out<<"},\"destination_xyz\":["<<x<<','<<y<<','<<z<<"],\"source_box\":";
    if(box)out<<'['<<box->left<<','<<box->top<<','<<box->front<<','<<box->right<<','<<box->bottom<<','<<box->back<<']';else out<<"null";
    if(dst&&src)out<<",\"copy_location_types\":["<<src->Type<<','<<dst->Type<<"],\"source_subresource\":"<<(src->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX?src->SubresourceIndex:UINT_MAX)
        <<",\"destination_subresource\":"<<(dst->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX?dst->SubresourceIndex:UINT_MAX);
    out<<",\"detailed_frame\":"<<(state.detailedTextureFrame?"true":"false")<<",\"changes_copy\":false}";SaveBridge(out.str());
}
void STDMETHODCALLTYPE OnTrackedTextureCopy(ID3D12GraphicsCommandList* list,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box) {
    if(g_ready.load()&&!g_done.load()&&dst&&src)try{RecordPostTaaCopy(list,dst->pResource,src->pResource,dst,src,x,y,z,box);}catch(...){++g_traceDrops;}
    g_trackedTextureCopy(list,dst,x,y,z,src,box);
}
void STDMETHODCALLTYPE OnTrackedResourceCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* dst,ID3D12Resource* src) {
    if(g_ready.load()&&!g_done.load())try{RecordPostTaaCopy(list,dst,src,nullptr,nullptr,0,0,0,nullptr);}catch(...){++g_traceDrops;}
    g_trackedResourceCopy(list,dst,src);
}
