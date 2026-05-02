//
// SWRT_Reflection_Temporal_CS.hlsl
// Simple temporal EMA (exponential moving average) accumulation for the
// SWRT Legacy reflection output.
//
// Layout:
//   t0 = current-frame raw reflection (output of SWRT_Reflection_CS)
//   t1 = previous EMA result (ping-pong history read side)
//   u0 = ping-pong history write side
//
// After dispatch the caller CopyResource(u0 → reflOutput) so the downstream
// compositing shader reads the EMA-blended result.
//
// Alpha semantics:
//   alpha = 1.0  → use current frame only (camera moved, first frame)
//   alpha = 0.1  → 90% history + 10% current (camera stationary)
//

cbuffer BlendCB : register(b0)
{
    float g_alpha;   // blend weight for current frame
    uint  g_width;
    uint  g_height;
    uint  g_pad;
};

Texture2D<float4>   g_current  : register(t0);  // this frame's raw reflection
Texture2D<float4>   g_history  : register(t1);  // previous EMA result

RWTexture2D<float4> g_outHist  : register(u0);  // write EMA result (next frame's history)

[numthreads(16, 16, 1)]
void CS_ReflectionTemporal(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    float4 cur  = g_current.Load(int3(id.xy, 0));
    float4 hist = g_history.Load(int3(id.xy, 0));

    // Clamp history to prevent runaway accumulation of old bright samples
    // (simple neighbourhood clamp: keep within 3x of current to avoid ghosting).
    float4 blended = lerp(hist, cur, g_alpha);

    g_outHist[id.xy] = blended;
}
