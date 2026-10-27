// RendererNativeFrame.cpp
// Renderer::RenderNativeBackendFrame -- the whole-frame path taken by non-D3D12 backends.
//
// Split out of RendererFrameGraph.cpp: this path never touches the render-pass graph that
// the rest of Renderer::Render builds, so it is a separate responsibility from that function.
#include "Renderer/Runtime/NativeBackendFrame.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Passes/RayMarch/RayMarchRenderPass.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"

using namespace std;

namespace SasamiEngine::Render
{
    namespace
    {
        // Mirrors the ToUtf8 helper in RendererScreenshot.cpp so the native-backend capture
        // path reports its OK result in the exact same string format as the D3D12 path.
        std::string ToUtf8(const std::wstring& wide)
        {
            if (wide.empty()) {
                return {};
            }

            const int required = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                                        nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                return {};
            }

            std::string utf8(static_cast<size_t>(required), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                  utf8.data(), required, nullptr, nullptr);
            return utf8;
        }

        const RayMarchRenderPass* FindRayMarchRenderPass(const std::vector<std::shared_ptr<IRenderPass>>& passes)
        {
            for (const auto& pass : passes) {
                if (!pass || pass->Tag() != "RayMarch") {
                    continue;
                }
                if (const auto* rayMarch = dynamic_cast<const RayMarchRenderPass*>(pass.get())) {
                    return rayMarch;
                }
            }
            return nullptr;
        }

        // Shadow map resolution for the native backend's directional shadow. The native path has
        // no dynamic-resolution shadow allocator like the DX12 CSM path, so a single fixed size is
        // used for every frame.
        constexpr uint32_t kNativeShadowMapResolution = 2048u;

        void FillNativeDirectionalShadowParams(const RenderDirectionalLight& light,
                                               const float cameraPos[3],
                                               RhiBackendMeshFrameDesc& meshDesc)
        {
            // The stored nearZ/farZ are a degenerate 2cm slab in the current scene (nearZ = 0.1,
            // farZ = 0.12) and the DX12 CSM path ignores them for the same reason.
            //
            // The slab must begin at the LIGHT PLANE, not at the scene. LightSystemShadow.cpp:252
            // states the rule for the DX12 CSM after the 2026-09-01 fix: a wall, a roof or the
            // upper storeys of a building sitting between the sun and the street still cast into
            // the view, so pulling the near plane forward to hug the scene throws away exactly the
            // casters that matter. maxZ mirrors that code's lightOffset + radius + margin so both
            // planes derive from the same quantities instead of two unrelated ones -- the
            // disagreement between those two floors was the confirmed root cause there.
            const float depthMargin = (std::max)(2.0f, light.orthoHalf * 0.1f);
            const float shadowNear = 0.001f;
            const float shadowFar = light.distance + light.orthoHalf + depthMargin;

            float lightVP[16] = {};
            float lightForward[3] = {};
            Math::BuildDirectionalLightViewProjection(light.yaw, light.pitch, light.distance, light.orthoHalf,
                                                       shadowNear, shadowFar, lightVP, lightForward);

            // World size of one shadow-map texel: the ortho box spans `orthoHalf * 2` world units
            // across `kNativeShadowMapResolution` texels.
            const float texelWorld = (light.orthoHalf * 2.0f) / static_cast<float>(kNativeShadowMapResolution);

            // BuildDirectionalLightViewProjection centres the ortho box on the world origin, so on
            // its own it only shadows an orthoHalf-sized island around (0,0,0) -- measured on
            // Bistro, a street 31 units out saw the shadow toggle change exactly 0.000% of the
            // frame. Follow the box to the camera instead, the way the DX12 CSM centres each
            // cascade on its frustum, by pre-translating world space by -centre.
            //
            // The centre is snapped to whole texels first. Without that, sub-texel drift as the
            // camera moves re-rasterises every shadow edge each frame and they crawl.
            const float center[3] = {
                std::floor(cameraPos[0] / texelWorld) * texelWorld,
                std::floor(cameraPos[1] / texelWorld) * texelWorld,
                std::floor(cameraPos[2] / texelWorld) * texelWorld,
            };
            const float recentre[16] = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                -center[0], -center[1], -center[2], 1.0f,
            };
            float centeredVP[16] = {};
            Math::Mul4x4(recentre, lightVP, centeredVP);
            std::memcpy(meshDesc.lightViewProjection, centeredVP, sizeof(meshDesc.lightViewProjection));

            meshDesc.shadowMapResolution = kNativeShadowMapResolution;
            meshDesc.shadowTexelWorld = texelWorld;

            // `light.depthBias` is a D3D12 rasterizer integer depth bias (units of the UNORM
            // depth buffer's least significant bit) and is many orders of magnitude too large to
            // add directly to an NDC depth in the shader, so it is not used here.
            //
            // Sign matters and is easy to get backwards. Reverse-Z means near = 1.0, so the shadow
            // map keeps the LARGEST depth (the caster nearest the light) and a receiver is lit when
            // its own depth is >= the stored one. A surface that is its own closest caster compares
            // equal, so the tolerance has to make it look slightly NEARER the light -- a positive
            // bias. A negative one shadows every lit surface and leaves only ambient in the frame.
            meshDesc.shadowDepthBias = 0.0015f;
            meshDesc.shadowNormalBias = light.normalBias > 0.0f ? light.normalBias : 0.09f;
        }
    }

    // Non-DX12 backends draw their whole frame through ExecuteBackendFrame instead of the
    // render-pass graph Renderer::Render otherwise builds, so this returns true when it has
    // fully handled the frame (the caller should return immediately) and false when the
    // caller should fall through to the render-pass graph path.
    bool Renderer::RenderNativeBackendFrame(const OverlayRenderCallback& overlay)
    {
        if (!UsesNativeBackendFrame(*m_device)) {
            return false;
        }

        if (m_deltaTime > 0.0f) {
            m_sceneTime += m_deltaTime;
        }
        RhiBackendFrameDesc frameDesc{};
        frameDesc.clearColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        if (const RayMarchRenderPass* rayMarch = FindRayMarchRenderPass(m_passRegistry.GetPasses())) {
            auto& rayMarchDesc = frameDesc.rayMarch;
            rayMarchDesc.enabled = true;
            std::memcpy(rayMarchDesc.invViewProjection, m_cameraState.GetInvPV(), sizeof(rayMarchDesc.invViewProjection));
            const float* cameraPos = m_cameraState.GetPos();
            rayMarchDesc.cameraPos[0] = cameraPos[0];
            rayMarchDesc.cameraPos[1] = cameraPos[1];
            rayMarchDesc.cameraPos[2] = cameraPos[2];
            rayMarchDesc.sceneTimeSec = m_sceneTime;

            const auto light = m_lightSystem.GetDirectionalLightSettings();
            float lightFwd[3] = {};
            Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);
            rayMarchDesc.sunDir[0] = -lightFwd[0];
            rayMarchDesc.sunDir[1] = -lightFwd[1];
            rayMarchDesc.sunDir[2] = -lightFwd[2];
            rayMarchDesc.sunIntensity = light.intensity;
            rayMarchDesc.sunColor[0] = light.color[0];
            rayMarchDesc.sunColor[1] = light.color[1];
            rayMarchDesc.sunColor[2] = light.color[2];

            rayMarchDesc.cloudCover = rayMarch->GetCloudCover();
            rayMarchDesc.cloudDensity = rayMarch->GetCloudDensity();
            rayMarchDesc.debugMode = rayMarch->GetDebugMode();
            rayMarchDesc.renderWidth = m_viewport.Width;
            rayMarchDesc.renderHeight = m_viewport.Height;
            rayMarchDesc.tanHalfFovY = m_cameraState.GetTanHalfFovY();
            rayMarchDesc.aspectRatio = m_cameraState.GetAspectRatio();
            const float* right = m_cameraState.GetRight();
            const float* up = m_cameraState.GetUp();
            rayMarchDesc.cameraRight[0] = right[0];
            rayMarchDesc.cameraRight[1] = right[1];
            rayMarchDesc.cameraRight[2] = right[2];
            rayMarchDesc.cameraUp[0] = up[0];
            rayMarchDesc.cameraUp[1] = up[1];
            rayMarchDesc.cameraUp[2] = up[2];
            rayMarchDesc.explicitCameraBasis = (m_cameraState.GetCameraMode() == RenderCameraMode::RayMarch);
        } else if (!m_sceneSubmitter.GetDrawItems().empty() || !m_sceneSubmitter.GetSkinnedDrawItems().empty()) {
            std::vector<RhiBackendMeshDrawDesc> meshDraws;
            const auto& gpuItems = m_meshBuffer.Items();
            meshDraws.reserve(m_sceneSubmitter.GetDrawItems().size());
            // Both skips below drop a mesh from the frame without a trace, which reads as
            // "some geometry is just missing" when comparing against the D3D12 path. Counted
            // and reported once per change so the gap is visible instead of guessed at.
            size_t skippedMissingGpuItem = 0;
            size_t skippedNoRhiVertexBuffer = 0;
            for (const auto& item : m_sceneSubmitter.GetDrawItems()) {
                if (item.meshIndex >= gpuItems.size()) {
                    ++skippedMissingGpuItem;
                    continue;
                }
                const auto& gpuItem = gpuItems[item.meshIndex];
                if (!gpuItem.rhiVb.IsValid()) {
                    ++skippedNoRhiVertexBuffer;
                    continue;
                }

                RhiBackendMeshDrawDesc draw{};
                draw.vertexBufferHandle = gpuItem.rhiVb.id;
                draw.indexBufferHandle = gpuItem.rhiIb.id;
                draw.vertexCount = gpuItem.vertexCount;
                draw.indexCount = gpuItem.indexCount;
                draw.vertexStride = gpuItem.vbv.StrideInBytes;
                draw.indexSizeInBytes = gpuItem.ibv.SizeInBytes;
                draw.index32Bit = (gpuItem.ibv.Format == DXGI_FORMAT_R32_UINT);
                draw.transparent = item.transparent;
                draw.albedoSrv = item.texture ? item.texture->srv.ptr : 0;
                draw.occlusionSrv = item.occlusionTexture ? item.occlusionTexture->srv.ptr : 0;
                // occlusionStrength is the upstream record of whether the occlusion slot
                // still holds ambient occlusion: MeshComponent zeroes it when glTF used
                // separate occlusion and metallic-roughness images, because the single
                // material texture slot is then given the metallic-roughness image instead.
                // A packed ORM image, a standalone occlusion image, and an asset-authored
                // strength of 0 are all handled by the same test.
                draw.occlusionSlotHasAo = (item.material.occlusionStrength > 0.0f);
                std::memcpy(draw.model, item.model, sizeof(draw.model));
                std::memcpy(draw.baseColor, item.material.baseColor, sizeof(draw.baseColor));
                std::memcpy(draw.emissive, item.material.emissive, sizeof(draw.emissive));
                draw.roughness = item.material.roughness;
                draw.metallic = item.material.metallic;
                meshDraws.push_back(draw);
            }


            if (skippedMissingGpuItem != 0 || skippedNoRhiVertexBuffer != 0) {
                static size_t s_lastReportedSkipSignature = static_cast<size_t>(-1);
                const size_t signature = skippedMissingGpuItem * 1000003u + skippedNoRhiVertexBuffer;
                if (signature != s_lastReportedSkipSignature) {
                    s_lastReportedSkipSignature = signature;
                    char message[192];
                    snprintf(message,
                             sizeof(message),
                             "Renderer::Render: native mesh frame dropped %zu/%zu static draws "
                             "(%zu without a GPU item, %zu without an RHI vertex buffer).\n",
                             skippedMissingGpuItem + skippedNoRhiVertexBuffer,
                             m_sceneSubmitter.GetDrawItems().size(),
                             skippedMissingGpuItem,
                             skippedNoRhiVertexBuffer);
                    DebugLog(message);
                }
            }

            std::vector<RhiBackendSkinnedMeshDrawDesc> skinnedDraws;
            const auto& skinnedGpuItems = m_skinnedMeshBuffer.Items();
            skinnedDraws.reserve(m_sceneSubmitter.GetSkinnedDrawItems().size());
            for (const auto& item : m_sceneSubmitter.GetSkinnedDrawItems()) {
                if (item.meshIndex >= skinnedGpuItems.size()) {
                    continue;
                }
                const auto& gpuItem = skinnedGpuItems[item.meshIndex];
                if (!gpuItem.rhiVb.IsValid() || item.boneMatricesNative.empty()) {
                    continue;
                }

                RhiBackendSkinnedMeshDrawDesc draw{};
                draw.vertexBufferHandle = gpuItem.rhiVb.id;
                draw.indexBufferHandle = gpuItem.rhiIb.id;
                draw.vertexCount = gpuItem.vertexCount;
                draw.indexCount = gpuItem.indexCount;
                draw.vertexStride = gpuItem.vbv.StrideInBytes;
                draw.indexSizeInBytes = gpuItem.ibv.SizeInBytes;
                draw.index32Bit = (gpuItem.ibv.Format == DXGI_FORMAT_R32_UINT);
                draw.transparent = item.transparent;
                draw.albedoSrv = item.texture ? item.texture->srv.ptr : 0;
                draw.occlusionSrv = item.occlusionTexture ? item.occlusionTexture->srv.ptr : 0;
                // ModelLoaderSkinned.cpp only ever fills occlusionTexture from glTF's
                // dedicated occlusionTexture slot and never substitutes metallic-roughness
                // into it, so the slot always holds real ambient occlusion here. The same
                // test as the static path is used so an asset-authored strength of 0 still
                // disables AO.
                draw.occlusionSlotHasAo = (item.material.occlusionStrength > 0.0f);
                std::memcpy(draw.model, item.model, sizeof(draw.model));
                std::memcpy(draw.baseColor, item.material.baseColor, sizeof(draw.baseColor));
                std::memcpy(draw.emissive, item.material.emissive, sizeof(draw.emissive));
                draw.roughness = item.material.roughness;
                draw.metallic = item.material.metallic;
                draw.boneMatrices = item.boneMatricesNative.data();
                skinnedDraws.push_back(draw);
            }

            {
                // "Some meshes look missing" on this path is usually not a dropped draw but an
                // untextured one, which reads as flat white next to the D3D12 image. Count the
                // draws that reach the backend without an albedo texture so the two failure
                // modes can be told apart from the log instead of from a screenshot.
                size_t untextured = 0;
                for (const auto& draw : meshDraws) {
                    if (draw.albedoSrv == 0) {
                        ++untextured;
                    }
                }
                // Skinned draws are reported too: they go through a separate shader
                // (NativeMeshSkinned.hlsl) whose lighting has drifted from the static one
                // before, and without a count there is no way to tell "skinned looks wrong"
                // from "no skinned draw ever reached the backend".
                static size_t s_lastReportedUntextured = static_cast<size_t>(-1);
                static size_t s_lastReportedSkinned = static_cast<size_t>(-1);
                if (untextured != s_lastReportedUntextured || skinnedDraws.size() != s_lastReportedSkinned) {
                    s_lastReportedUntextured = untextured;
                    s_lastReportedSkinned = skinnedDraws.size();
                    char message[200];
                    snprintf(message,
                             sizeof(message),
                             "Renderer::Render: native mesh frame submits %zu static draws (%zu without an albedo texture) and %zu skinned draws.\n",
                             meshDraws.size(),
                             untextured,
                             skinnedDraws.size());
                    DebugLog(message);
                }
            }
            if (!meshDraws.empty() || !skinnedDraws.empty()) {
                auto& meshDesc = frameDesc.mesh;
                meshDesc.enabled = true;
                meshDesc.draws = meshDraws.data();
                meshDesc.drawCount = static_cast<uint32_t>(meshDraws.size());
                meshDesc.skinnedDraws = skinnedDraws.data();
                meshDesc.skinnedDrawCount = static_cast<uint32_t>(skinnedDraws.size());
                std::memcpy(meshDesc.viewProjection, m_cameraState.GetPV(), sizeof(meshDesc.viewProjection));
                std::memcpy(meshDesc.invViewProjection, m_cameraState.GetInvPV(), sizeof(meshDesc.invViewProjection));
                const float* cameraPos = m_cameraState.GetPos();
                meshDesc.cameraPos[0] = cameraPos[0];
                meshDesc.cameraPos[1] = cameraPos[1];
                meshDesc.cameraPos[2] = cameraPos[2];
                const auto light = m_lightSystem.GetDirectionalLightSettings();
                float lightFwd[3] = {};
                Math::DirectionFromYawPitch(light.yaw, light.pitch, lightFwd);
                meshDesc.sunDir[0] = -lightFwd[0];
                meshDesc.sunDir[1] = -lightFwd[1];
                meshDesc.sunDir[2] = -lightFwd[2];
                meshDesc.sunIntensity = light.intensity;
                meshDesc.sunColor[0] = light.color[0];
                meshDesc.sunColor[1] = light.color[1];
                meshDesc.sunColor[2] = light.color[2];
                meshDesc.renderWidth = m_viewport.Width;
                meshDesc.renderHeight = m_viewport.Height;
                meshDesc.enableIbl = true;
                // Same latch as UploadHdrSkyboxTexture on the D3D12 path: calling this every
                // frame is free once the cubemap (or the "no HDR source" verdict) is cached.
                meshDesc.environmentCubeSrv = m_skybox.EnsureNativeEnvironmentCubemap().IsValid()
                    ? m_skybox.GetNativeEnvironmentCubemapSrv()
                    : 0ull;
                // Same call, same latch: EnsureNativeEnvironmentCubemap builds both cubes in one
                // pass, so its validity also gates the low-resolution ambient cube below.
                meshDesc.ambientCubeSrv = m_skybox.EnsureNativeEnvironmentCubemap().IsValid()
                    ? m_skybox.GetNativeAmbientCubemapSrv()
                    : 0ull;
                // Off: the shadow map renders correct geometry (visualising the map shows
                // the buildings and roofs from the light's side), but the comparison
                // over-occludes -- a roof top surface stays dark with the sun at yaw 40 AND at
                // yaw 220, which no real occluder can do. The direct sun term is then
                // multiplied out and only ambient survives, which is worse than no shadows at
                // all, so the PBR shading ships on its own. See TODO.md for the evidence
                // gathered so far and the next step.
                // Opt-in via `render.nativeshadow 1`. The shadow pass runs and the map holds
                // correct geometry, but the comparison has not been shown to shadow anything
                // yet, and a wrong one multiplies the direct sun term out entirely -- worse
                // than no shadows. Kept behind a runtime switch so on/off can be A/B'd in one
                // session without a rebuild. See TODO.md.
                meshDesc.enableShadow = m_settings.nativeShadowEnabled;
                FillNativeDirectionalShadowParams(light, m_cameraState.GetPos(), meshDesc);
                // AND, not OR: runtimeAoEnabled is the master switch, so `render.runtimeao 0` has to
                // kill the pass whatever aoMode says. The old OR meant the flag only ever mattered at
                // aoMode == MaterialOnly, where there is no runtime AO to begin with -- i.e. it never
                // mattered, and a "runtime AO off" capture was silently still AO-on. All three native
                // backends clear their own ready flag when enableSsao is false (Dx11GraphicsDeviceMesh
                // RenderNativeAmbientOcclusion, OpenGLGraphicsDeviceSsao, VulkanGraphicsDevice), so the
                // shaders fall back to runtimeAo = 1.0 instead of sampling a stale texture.
                meshDesc.enableSsao =
                    GetEffectiveAmbientOcclusionMode() != RendererEnums::AmbientOcclusionMode::MaterialOnly;
                // The three native backends used to each hard-code the RenderSettings defaults
                // here, which left the UI sliders (and `render.runtimeao*` commands) without
                // effect outside DirectX 12. Carried on the description instead so one source of
                // truth reaches all of them.
                meshDesc.exposure = m_settings.exposure;
                meshDesc.runtimeAoRadius = m_settings.runtimeAoRadius;
                meshDesc.runtimeAoBias = m_settings.runtimeAoBias;
                meshDesc.runtimeAoIntensity = m_settings.runtimeAoIntensity;
                meshDesc.runtimeAoThickness = m_settings.runtimeAoThickness;
                meshDesc.runtimeAoQuality = m_settings.runtimeAoQuality;
                meshDesc.enablePostProcess = true;
                frameDesc.present = true;
                // The D3D12 overlay callback has no meaning here (there is no command list to
                // hand out); the backend calls m_frameOverlay back from inside its own frame.
                (void)overlay;
                frameDesc.overlay = m_frameOverlay;
                RhiBackendCaptureRequest meshCapture{};
                if (m_screenshotRequested) {
                    if (m_captureTargetName == "backbuffer") {
                        meshCapture.path = m_screenshotPath.c_str();
                        frameDesc.capture = &meshCapture;
                    } else {
                        // Native backends have no intermediate targets (GBuffer, shadow maps, ...)
                        // to dump; close the request now instead of leaving the poller waiting.
                        m_screenshotResult = "ERR capture target unavailable on this backend";
                        m_screenshotResultReady = true;
                        m_screenshotRequested = false;
                    }
                }
                if (!m_device->ExecuteBackendFrame(frameDesc)) {
                    DebugLog("Renderer::Render: native backend mesh frame execution failed.\n");
                }
                if (frameDesc.capture) {
                    if (meshCapture.completed && meshCapture.succeeded) {
                        m_screenshotResult = "OK " + ToUtf8(m_screenshotPath);
                        m_screenshotResultReady = true;
                        m_screenshotRequested = false;
                    } else if (meshCapture.completed && !meshCapture.succeeded) {
                        m_screenshotResult = "ERR native backend capture failed";
                        m_screenshotResultReady = true;
                        m_screenshotRequested = false;
                    } else {
                        // The backend did not attempt the capture at all (unimplemented for
                        // this API). Leaving the request open would make the poller wait
                        // forever, so close it as failed instead.
                        m_screenshotResult = "ERR this backend cannot capture the back buffer yet";
                        m_screenshotResultReady = true;
                        m_screenshotRequested = false;
                    }
                }
                return true;
            }
        }
        frameDesc.present = true;
        (void)overlay;
        frameDesc.overlay = m_frameOverlay;
        RhiBackendCaptureRequest genericCapture{};
        if (m_screenshotRequested) {
            if (m_captureTargetName == "backbuffer") {
                genericCapture.path = m_screenshotPath.c_str();
                frameDesc.capture = &genericCapture;
            } else {
                // Native backends have no intermediate targets (GBuffer, shadow maps, ...)
                // to dump; close the request now instead of leaving the poller waiting.
                m_screenshotResult = "ERR capture target unavailable on this backend";
                m_screenshotResultReady = true;
                m_screenshotRequested = false;
            }
        }
        if (!m_device->ExecuteBackendFrame(frameDesc)) {
            DebugLog("Renderer::Render: native backend frame execution failed.\n");
        }
        if (frameDesc.capture) {
            if (genericCapture.completed && genericCapture.succeeded) {
                m_screenshotResult = "OK " + ToUtf8(m_screenshotPath);
                m_screenshotResultReady = true;
                m_screenshotRequested = false;
            } else if (genericCapture.completed && !genericCapture.succeeded) {
                m_screenshotResult = "ERR native backend capture failed";
                m_screenshotResultReady = true;
                m_screenshotRequested = false;
            } else {
                // The backend did not attempt the capture at all (unimplemented for this
                // API). Leaving the request open would make the poller wait forever, so
                // close it as failed instead.
                m_screenshotResult = "ERR this backend cannot capture the back buffer yet";
                m_screenshotResultReady = true;
                m_screenshotRequested = false;
            }
        }
        return true;
    }
}
