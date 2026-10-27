// CrowdVat_VS.hlsl
// Instanced Vertex-Animation-Texture crowd draw into the GBuffer. No input layout:
// SV_VertexID is the VAT vertex index, SV_InstanceID indexes the agent buffer (or g_visible when GPU culled).
// LOD index ranges come from CrowdVatGpuAsset; its indices already include the vertex offset,
// so SV_VertexID stays the VAT vertex index.
//
// Root signature (CrowdVat):
//   [0] b0 : CrowdCB — VS + PS visibility
//            g_extra0 = (modelScale, asuint vertexCount, asuint mode (0 Vertex, 1 Bone), asuint boneCount)
//            g_extra1 = (cameraPos.xyz, modelYOffset)
//            g_extra2 = (albedo.rgb linear, roughness)
//   [1] t0 : g_tableA — raw. Vertex mode: positions (float4 per frame*vertexCount+vertex).
//                       Bone mode: 3 float4 rows per (frame*boneCount+bone).
//   [2] t1 : g_tableB — raw. Vertex mode: normals (float4 per frame*vertexCount+vertex).
//                       Bone mode: 48-byte skin vertex per vertex (pos+boneIndices, normal, weights).
//   [3] t2 : g_agents — CrowdAgentGpu per instance
//   [4] t3 : g_clips  — CrowdClipGpu per clip
//   [5] t4 : g_visible — per-LOD compacted agent indices written by CrowdCull_CS; bound at the LOD's list start

cbuffer CrowdCB : register(b0)
{
    row_major float4x4 g_viewProj;      // Camera view * projection
    row_major float4x4 g_drawParams;    // [0].x = asuint 1 -> instance index goes through g_visible (GPU culling, TODO 8d)
    float4             g_extra0;        // .x = modelScale, .y = vertexCount, .z = mode, .w = boneCount (uint bits)
    float4             g_extra1;        // .xyz = cameraPos, .w = modelYOffset
    float4             g_extra2;        // .rgb = albedo, .w = roughness
}

ByteAddressBuffer g_tableA : register(t0);
ByteAddressBuffer g_tableB : register(t1);

struct CrowdAgentGpu
{
    float posX;
    float posY;
    float posZ;
    float animTime;
    float velX;
    float velZ;
    float animRate;
    uint  clip;
};

struct CrowdClipGpu
{
    uint  firstFrame;
    uint  frameCount;
    float durationSec;
    uint  pad;
};

StructuredBuffer<CrowdAgentGpu> g_agents : register(t2);
StructuredBuffer<CrowdClipGpu>  g_clips  : register(t3);
StructuredBuffer<uint>          g_visible : register(t4);

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float3 worldPos : TEXCOORD0;
};

struct VatFrameBlend
{
    uint  frameA;
    uint  frameB;
    float alpha;
};

// Must match ComputeVatFrameBlend in Source/Renderer/Crowd/VatAnimation.cpp.
VatFrameBlend ComputeVatFrameBlend(CrowdClipGpu clip, float timeSec)
{
    VatFrameBlend blend;
    blend.frameA = clip.firstFrame;
    blend.frameB = clip.firstFrame;
    blend.alpha  = 0.0f;
    if (clip.frameCount <= 1u || clip.durationSec <= 0.0f)
    {
        return blend;
    }

    float d = clip.durationSec;
    float t = fmod(timeSec, d);
    if (t < 0.0f)
    {
        t += d;
    }

    float framePos = t / d * (float)clip.frameCount;
    uint  i        = min((uint)floor(framePos), clip.frameCount - 1u);
    blend.alpha    = saturate(framePos - (float)i);
    blend.frameA   = clip.firstFrame + i;
    blend.frameB   = clip.firstFrame + (i + 1u) % clip.frameCount;
    return blend;
}

// Mirrors SkinVertexWithBoneRows in Source/Renderer/Crowd/VatSkinning.cpp.
void SkinWithBoneRows(float3 p, float3 n, uint4 boneIdx, float4 weights, uint frame, uint boneCount,
                      out float3 outPos, out float3 outNrm)
{
    float3 pos = float3(0.0f, 0.0f, 0.0f);
    float3 nrm = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        float w    = weights[k];
        uint  base = (frame * boneCount + boneIdx[k]) * 3u;
        [unroll]
        for (int r = 0; r < 3; ++r)
        {
            float4 row = asfloat(g_tableA.Load4((base + (uint)r) * 16u));
            pos[r] += w * (dot(row.xyz, p) + row.w);
            nrm[r] += w * dot(row.xyz, n);
        }
    }

    float lenSq = dot(nrm, nrm);
    outPos = pos;
    outNrm = lenSq > 0.0f ? nrm * rsqrt(lenSq) : float3(0.0f, 0.0f, 0.0f);
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint agentIndex = (asuint(g_drawParams[0].x) == 1u) ? g_visible[instanceId] : instanceId;
    CrowdAgentGpu agent = g_agents[agentIndex];
    CrowdClipGpu  clip  = g_clips[agent.clip];
    VatFrameBlend blend = ComputeVatFrameBlend(clip, agent.animTime);

    uint vertexCount = asuint(g_extra0.y);
    uint mode        = asuint(g_extra0.z);
    uint boneCount   = asuint(g_extra0.w);

    float3 p;
    float3 n;
    if (mode == 0u)
    {
        uint idxA = blend.frameA * vertexCount + vertexId;
        uint idxB = blend.frameB * vertexCount + vertexId;
        float3 posA = asfloat(g_tableA.Load4(idxA * 16u)).xyz;
        float3 posB = asfloat(g_tableA.Load4(idxB * 16u)).xyz;
        float3 nrmA = asfloat(g_tableB.Load4(idxA * 16u)).xyz;
        float3 nrmB = asfloat(g_tableB.Load4(idxB * 16u)).xyz;
        p = lerp(posA, posB, blend.alpha);
        n = lerp(nrmA, nrmB, blend.alpha);
    }
    else
    {
        uint   vbase    = vertexId * 48u;
        uint4  v0       = g_tableB.Load4(vbase);
        float3 bindPos  = asfloat(v0.xyz);
        uint   packed   = v0.w;
        float3 bindNrm  = asfloat(g_tableB.Load4(vbase + 16u)).xyz;
        float4 weights  = asfloat(g_tableB.Load4(vbase + 32u));
        uint4  boneIdx  = uint4(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu, (packed >> 24u) & 0xffu);

        float3 posA, nrmA, posB, nrmB;
        SkinWithBoneRows(bindPos, bindNrm, boneIdx, weights, blend.frameA, boneCount, posA, nrmA);
        SkinWithBoneRows(bindPos, bindNrm, boneIdx, weights, blend.frameB, boneCount, posB, nrmB);
        p = lerp(posA, posB, blend.alpha);
        n = lerp(nrmA, nrmB, blend.alpha);
    }

    float nLenSq = dot(n, n);
    n = nLenSq > 0.0f ? n * rsqrt(nLenSq) : float3(0.0f, 0.0f, 0.0f);

    p *= g_extra0.x;
    p.y += g_extra1.w;

    // Model forward is +Z.
    float heading = (agent.velX == 0.0f && agent.velZ == 0.0f) ? 0.0f : atan2(agent.velX, agent.velZ);
    float s = sin(heading);
    float c = cos(heading);

    float3 worldPos = float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c)
                    + float3(agent.posX, agent.posY, agent.posZ);
    float3 worldN   = float3(n.x * c + n.z * s, n.y, -n.x * s + n.z * c);

    VSOutput o;
    o.position = mul(float4(worldPos, 1.0f), g_viewProj);
    o.worldN   = worldN;
    o.worldPos = worldPos;
    return o;
}
