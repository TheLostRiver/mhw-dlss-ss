// Research implementation derived from the user's MHWSS 1.0.2 shader bytecode.
// Source evidence: shader_004c70a0.dxil.txt, SHA-256 in evidence/shaders/manifest.json.
// This is a standalone decode component, NOT a complete game integration.
// Actual resource bindings, per-frame identity, and motion direction require a live capture.
// The output reproduces MHWSS's intermediate convention. Its default NGX MV.Scale
// is (-0.5, +0.5); do not feed this texture with a unity MV.Scale without verification.

cbuffer GameCamera : register(b0)
{
    // Original camera constant buffer: 1068 bytes plus alignment padding.
    // Byte offset 160: current projection's jitter row xy.
    // Byte offset 704: previous projection's corresponding row xy.
    float4 CameraWords[67];
};

cbuffer DecodeSettings : register(b2)
{
    // The inspected MHWSS constructor initializes the consumed xy pair to (1, 1).
    float4 UnJitterScale;
};

Texture2D<uint> PackedMotion : register(t0);
RWTexture2D<float2> DecodedMotion : register(u0);

[numthreads(8, 8, 1)]
void DecodeMotion(uint3 dispatchId : SV_DispatchThreadID)
{
    uint width, height, outputWidth, outputHeight;
    PackedMotion.GetDimensions(width, height);
    DecodedMotion.GetDimensions(outputWidth, outputHeight);
    if (dispatchId.x >= min(width, outputWidth) || dispatchId.y >= min(height, outputHeight))
        return;
    uint packed = PackedMotion.Load(int3(dispatchId.xy, 0));
    float2 encoded = float2(f16tof32(packed >> 16), f16tof32(packed & 0xfffeu));
    float2 projectionDifference = CameraWords[10].xy - CameraWords[44].xy;
    float2 corrected = encoded + projectionDifference * UnJitterScale.xy;
    DecodedMotion[dispatchId.xy] = corrected * float2(width, height);
}
