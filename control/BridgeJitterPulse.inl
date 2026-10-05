// A bounded diagnostic of MHWSS's independent projection-jitter gate.
// It changes neither the TAA bypass flags nor the NGX/upscaler selection.
std::array<unsigned char,4> g_originalRenderFlags{};
bool EnableJitterPulse() {
    if(!g_pulseJitter)return true;
    const std::array<unsigned char,6> configure{0x88,0x05,0x08,0xe6,0x52,0x00};
    const std::array<unsigned char,6> producer{0x38,0x05,0x1b,0x00,0x47,0x00};
    if(!g_mhwss||memcmp(g_mhwss+0x2056a,configure.data(),configure.size())||
       memcmp(g_mhwss+0xdeb57,producer.data(),producer.size()))return false;
    memcpy(g_originalRenderFlags.data(),g_mhwss+0x54ebe0,g_originalRenderFlags.size());
    float denominator[2]{};memcpy(denominator,g_mhwss+0x54eb68,sizeof(denominator));
    if(g_mhwss[0x54eb78]||g_originalRenderFlags!=std::array<unsigned char,4>{}||
       !Near(denominator[0],float(g_width))||!Near(denominator[1],float(g_height)))return false;
    *reinterpret_cast<volatile unsigned char*>(g_mhwss+0x54eb78)=1;
    g_jitterChanged.store(true);
    Log("{\"event\":\"projection_jitter_enabled\",\"gate_rva\":\"0x54eb78\",\"original\":0,\"temporary\":1,\"mhwss_render_flags\":[0,0,0,0],\"enables_upscaler\":false}");
    return true;
}
bool JitterPulseStillOwned() {
    return !g_jitterChanged.load()||(g_mhwss[0x54eb78]==1&&
        !memcmp(g_mhwss+0x54ebe0,g_originalRenderFlags.data(),g_originalRenderFlags.size()));
}
void RestoreJitterPulse() noexcept {
    if(!g_jitterChanged.load()||!g_mhwss)return;
    const bool sameMode=!memcmp(g_mhwss+0x54ebe0,g_originalRenderFlags.data(),g_originalRenderFlags.size());
    const auto observed=g_mhwss[0x54eb78];
    // Preserve a newer MHWSS mode selected by the user instead of disabling its jitter.
    if(sameMode&&observed==1)*reinterpret_cast<volatile unsigned char*>(g_mhwss+0x54eb78)=0;
    g_jitterChanged.store(false);
    Log(std::string("{\"event\":\"projection_jitter_restored\",\"current_gate\":")+std::to_string(g_mhwss[0x54eb78])+
        ",\"external_mode_preserved\":"+(sameMode?"false":"true")+"}");
}
