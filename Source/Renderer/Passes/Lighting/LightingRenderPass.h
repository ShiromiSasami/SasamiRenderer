#pragma once

// =============================================================================
// LightingRenderPass.h
// 繝ｩ繧ｹ繧ｿ繝ｩ繧､繧ｺ繝代う繝励Λ繧､繝ｳ縺ｮ繝ｩ繧､繝・ぅ繝ｳ繧ｰ繝代せ繧定｡ｨ縺吶Ξ繝ｳ繝繝ｼ繧ｰ繝ｩ繝輔ヮ繝ｼ繝峨・//
// 縲仙ｽｹ蜑ｲ縲・//  - GBuffer 縺ｮ豕慕ｷ壹・豺ｱ蠎ｦ繝ｻ繧ｷ繝｣繝峨え繝槭ャ繝励ｒ隱ｭ縺ｿ霎ｼ繧
//  - PBR 繝ｩ繧､繝・ぅ繝ｳ繧ｰ・医ョ繧｣繝ｬ繧ｯ繧ｷ繝ｧ繝翫Ν繝ｻ繝昴う繝ｳ繝医・繧ｹ繝昴ャ繝医・IBL・峨ｒ險育ｮ励＠縺ｦ SceneColor 縺ｫ譖ｸ縺・//  - 繝昴せ繝医・繝ｭ繧ｻ繧ｹ蜑阪・譛邨ゅす繧ｧ繝ｼ繝・ぅ繝ｳ繧ｰ邨先棡繧貞・蜉帙☆繧・//
// 縲舌Μ繧ｽ繝ｼ繧ｹ萓晏ｭ倥・//  Read:  "ShadowMap"・医す繝｣繝峨え繝槭ャ繝玲ｷｱ蠎ｦ SRV・・//  Read:  "SceneDepth"・・Buffer 豺ｱ蠎ｦ・・//  Write: "SceneColor"・医Λ繧､繝・ぅ繝ｳ繧ｰ邨先棡蜃ｺ蜉帛・・・//
// 縲舌す繧ｧ繝ｼ繝繝ｼ縲・//  - 讓呎ｺ悶ヱ繧ｹ: PBR_PS.hlsl / BasicShader.hlsl・・SO 縺ｫ萓晏ｭ假ｼ・//  - 繝・ャ繧ｻ繝ｬ繝ｼ繧ｷ繝ｧ繝ｳ繝代せ: 蟆ら畑縺ｮ PSO 繧剃ｽｿ逕ｨ
//
// 縲舌Ν繝ｼ繝医す繧ｰ繝阪メ繝｣縺ｮ繝・ぅ繧ｹ繧ｯ繝ｪ繝励ち繝・・繝悶Ν蜑ｲ繧雁ｽ薙※縲・//  b3: LightCB・医Λ繧､繝・ぅ繝ｳ繧ｰ螳壽焚繝舌ャ繝輔ぃ・・//  t1: ShadowMap SRV
//  t4: Point/SpotLight StructuredBuffer・医Λ繧､繝医ユ繝ｼ繝悶Ν・・//  t5: IBL 繧ｭ繝･繝ｼ繝悶・繝・・ SRV 繝・・繝悶Ν
//  t6: Runtime AO 繝・け繧ｹ繝√Ε SRV・・SAO/RTAO 縺ｪ縺ｩ・・//  t7: Reflection 繝・け繧ｹ繝√Ε SRV
// =============================================================================

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    // =========================================================================
    // LightingRenderPass
    // =========================================================================
    class LightingRenderPass : public IRenderPass
    {
    public:
        // Render graph identification.
        std::string_view Tag() const override { return "Lighting"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        // Declares resources and services used by this pass.
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;

        // Declares render graph resource dependencies.
        void Setup(RenderGraphBuilder& builder) const override;

        // Executes through the render graph context.
        bool Execute(const RenderPassContextView& context) const override;

        // Issues the lighting draw commands.
        void Execute(IRhiCommandEncoder* enc,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     bool useTessellation,
                     bool useTessellationWireframe,
                     bool useTessellationDebugColors,
                     bool useMeshletDebugView,
                     GpuDescriptorHandle shadowSrv,
                     GpuDescriptorHandle spotShadowSrv,
                     GpuDescriptorHandle vsmSrv,
                     GpuDescriptorHandle lightSrvTable,
                     GpuDescriptorHandle iblSrvTable,
                     GpuDescriptorHandle aoSrv,
                     GpuDescriptorHandle reflectionSrv,
                     GpuDescriptorHandle depthSrv,
                     GpuDescriptorHandle transparentBackfaceDistanceSrv,
                     D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                     const std::function<void()>& drawCallback) const;
    };
}
