// RendererGIBakeStep.cpp
// Single responsibility: advance a requested GI probe bake by one frame from Render()'s tail.
// This covers the two waiting states (probe grid, SWRT BVH), the SWRT BVH update the probe rays
// trace against, the probe dispatch itself, the progress logging, and the continuous-mode
// restart once a full pass completes. It is a state machine stepped once per frame rather than a
// render stage: it decides what the bake does next from the bake's own state, not from the frame
// being drawn, which is why it lives apart from RendererFrameGraph.cpp. It is also separate from
// RendererGI.cpp, which owns the request/status/cache half that runs only when asked and never
// during a frame.
#include "Renderer/Runtime/Renderer.h"

#include <cstdint>
#include <string>

#include "Foundation/Math/MathUtil.h"

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros. Same
// guard as RendererFrameGraph.cpp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiEngine::Render
{
    namespace
    {
        std::string FormatGIBvhMissingMask(uint32_t mask)
        {
            if (mask == 0u) {
                return "none";
            }

            std::string result;
            auto append = [&result](const char* name) {
                if (!result.empty()) {
                    result += ", ";
                }
                result += name;
            };

            if ((mask & Renderer::GI_BVH_MISSING_SWRT_NOT_INITIALIZED) != 0u) append("SWRT not initialized");
            if ((mask & Renderer::GI_BVH_MISSING_BVH_NODES) != 0u) append("bvhNodes");
            if ((mask & Renderer::GI_BVH_MISSING_TRIANGLES) != 0u) append("triangles");
            if ((mask & Renderer::GI_BVH_MISSING_MESH_INFO) != 0u) append("meshInfo");
            if ((mask & Renderer::GI_BVH_MISSING_INSTANCES) != 0u) append("instances");
            if ((mask & Renderer::GI_BVH_MISSING_TLAS_NODES) != 0u) append("tlasNodes");
            if ((mask & Renderer::GI_BVH_MISSING_MATERIALS) != 0u) append("materials");
            return result;
        }
    }

    void Renderer::StepGIBake(CommandList* cmdList)
    {
        // GI probe bake in Raster mode (or explicit bake request in any mode).
        if (m_giBakeRequested) {
            if (!m_readyState.IsFeatureReady(m_readyState.giReady) || !m_probeGrid.IsInitialized()) {
                SetGIBakePhase("Render tail: waiting for probe grid");
                if (m_giBakeStatus.stalledFrames == 0u ||
                    ((m_giBakeStatus.stalledFrames + 1u) % 60u) == 0u) {
                    AddGIBakeLog("ProbeGrid", "Waiting for GI probe grid initialization. stalledFrames=%u",
                                 m_giBakeStatus.stalledFrames + 1u);
                }
                RefreshGIBakeStatus(GIBakeState::WaitingForProbeGrid);
            } else if (m_probeGrid.IsBaked()) {
                if (m_giContinuousMode) {
                    const uint32_t total = m_probeGrid.GetTotalProbeCount();
                    m_probeGrid.ResetBakeState();
                    m_probeGrid.FlushGridCB();
                    SetGIBakePhase("Render tail: continuous refresh");
                    RefreshGIBakeStatus(GIBakeState::Continuous);
                    AddGIBakeLog("Continuous", "Full pass complete (%u probes); restarting for continuous refresh.", total);
                } else {
                    m_giBakeRequested = false;
                    SetGIBakePhase("Render tail: completed");
                    m_probeGrid.FlushGridCB();
                    RefreshGIBakeStatus(GIBakeState::Completed);
                    AddGIBakeLog("Completed", "Bake already completed before dispatch. probes=%u/%u",
                                 m_probeGrid.GetBakedProbeCount(),
                                 m_probeGrid.GetTotalProbeCount());
                }
            } else {
                if (m_giBakeClearPending) {
                    m_giBakeClearPending = false;
                }

                if (m_giBakeRequested) {
                    SetGIBakePhase("Render tail: updating SWRT BVH");
                    if (m_giBakeFrameIndex == 0u && m_probeGrid.GetBakedProbeCount() == 0u) {
                        AddGIBakeLog("BVH Update", "Begin SWRT BVH update. scene=%u instances / %u triangles, versions=%llu/%llu/%llu",
                                     static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                                     m_rayTracingScene.TriangleCount(),
                                     static_cast<unsigned long long>(m_rayTracingScene.geometryVersion),
                                     static_cast<unsigned long long>(m_rayTracingScene.materialVersion),
                                     static_cast<unsigned long long>(m_rayTracingScene.instanceVersion));
                    }
                    const bool swrtReadyForBvhUpdate = m_readyState.IsFeatureReady(m_readyState.swrtReady);
                    if (swrtReadyForBvhUpdate) {
                        m_gpuSoftwareRayTracer.UpdateScene(m_rayTracingScene, *m_device, *cmdList);
                    }
                    const auto bvhAddrs = swrtReadyForBvhUpdate
                        ? m_gpuSoftwareRayTracer.GetBvhGpuAddresses()
                        : GpuSoftwareRayTracer::BvhGpuAddresses{};
                    if (bvhAddrs.valid) {
                        SetGIBakePhase("Render tail: dispatching GI probes");
                        const auto& bvhDiag = m_gpuSoftwareRayTracer.GetBvhBuildDiagnostics();
                        if (m_giBakeLastLoggedMissingMask != 0u) {
                            AddGIBakeLog("BVH Ready", "SWRT BVH buffers ready. phase=%s, scene=%u instances / %u triangles, meshBVH=%u, tlasNodes=%u",
                                         bvhDiag.lastPhase,
                                         static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                                         m_rayTracingScene.TriangleCount(),
                                         bvhDiag.meshBvhCount,
                                         bvhDiag.tlasNodeCount);
                        }
                        m_giBakeLastLoggedMissingMask = 0u;
                        const auto dirLight = m_lightSystem.GetDirectionalLightSettings();
                        float fwd[3];
                        Math::DirectionFromYawPitch(dirLight.yaw, dirLight.pitch, fwd);
                        IrradianceProbeGrid::UpdateDesc giDesc{};
                        giDesc.dirLightDir[0]     = -fwd[0];
                        giDesc.dirLightDir[1]     = -fwd[1];
                        giDesc.dirLightDir[2]     = -fwd[2];
                        giDesc.dirLightIntensity  = dirLight.intensity;
                        // Probe rays evaluate point/spot lights too, so indirect light
                        // picks up local sources instead of only the sun. Null here means
                        // zero lights, which is exactly the previous behaviour.
                        giDesc.pointLights        = &m_lightSystem.GetPointLights();
                        giDesc.spotLights         = &m_lightSystem.GetSpotLights();
                        giDesc.dirLightColor[0]   = dirLight.color[0];
                        giDesc.dirLightColor[1]   = dirLight.color[1];
                        giDesc.dirLightColor[2]   = dirLight.color[2];
                        giDesc.ambientColor[0]    = 0.1f;
                        giDesc.ambientColor[1]    = 0.1f;
                        giDesc.ambientColor[2]    = 0.1f;
                        giDesc.ambientIntensity   = 1.0f;
                        giDesc.shadowBias         = 0.005f;
                        giDesc.punctualLightFalloff = static_cast<uint32_t>(m_settings.punctualLightFalloff);
                        giDesc.frameIndex         = m_giBakeFrameIndex++;
                        giDesc.blendWithHistory   = m_giContinuousMode && m_probeGrid.HasEverBaked();

                        const uint32_t beforeProbes = m_probeGrid.GetBakedProbeCount();
                        if (!m_probeGrid.UpdateProbes(giDesc, bvhAddrs, *m_device, *cmdList)) {
                            m_giBakeRequested = false;
                            SetGIBakePhase("Render tail: probe dispatch failed");
                            RefreshGIBakeStatus(GIBakeState::Failed);
                            AddGIBakeLog("ProbeDispatch", "UpdateProbes failed after BVH was ready. probes=%u/%u",
                                         beforeProbes,
                                         m_probeGrid.GetTotalProbeCount());
                        } else if (m_probeGrid.IsBaked()) {
                            if (m_giContinuousMode) {
                                const uint32_t total = m_probeGrid.GetTotalProbeCount();
                                m_probeGrid.ResetBakeState();
                                SetGIBakePhase("Render tail: continuous refresh");
                                RefreshGIBakeStatus(GIBakeState::Continuous);
                                AddGIBakeLog("Continuous", "Full pass complete (%u probes); restarting for continuous refresh.", total);
                            } else {
                                m_giBakeRequested = false;
                                SetGIBakePhase("Render tail: completed");
                                RefreshGIBakeStatus(GIBakeState::Completed);
                                AddGIBakeLog("Completed", "Final probe dispatch succeeded. probes=%u/%u",
                                             m_probeGrid.GetBakedProbeCount(),
                                             m_probeGrid.GetTotalProbeCount());
                            }
                        } else {
                            const uint32_t afterProbes = m_probeGrid.GetBakedProbeCount();
                            if (afterProbes != m_giBakeLastLoggedCompletedProbes) {
                                AddGIBakeLog("ProbeDispatch", "Dispatched probes %u-%u. progress=%u/%u",
                                             beforeProbes,
                                             afterProbes > 0u ? afterProbes - 1u : 0u,
                                             afterProbes,
                                             m_probeGrid.GetTotalProbeCount());
                                m_giBakeLastLoggedCompletedProbes = afterProbes;

                                if (afterProbes == 0u && !m_probeGrid.GetLastPipelineError().empty()) {
                                    AddGIBakeLog("PipelineError", "%s",
                                                 m_probeGrid.GetLastPipelineError().c_str());
                                }
                            }
                            RefreshGIBakeStatus(GIBakeState::Baking);
                        }
                    } else {
                        SetGIBakePhase("Render tail: waiting for SWRT BVH");
                        const auto& bvhDiag = m_gpuSoftwareRayTracer.GetBvhBuildDiagnostics();
                        const std::string missing = FormatGIBvhMissingMask(bvhAddrs.missingMask);
                        if (bvhAddrs.missingMask & GI_BVH_MISSING_SWRT_NOT_INITIALIZED) {
                            m_giBakeRequested = false;
                            SetGIBakePhase("Render tail: SWRT unavailable");
                            RefreshGIBakeStatus(GIBakeState::Failed);
                            AddGIBakeLog("BVH Failed", "SWRT is not initialized; BakeGI cannot build BVH.");
                        } else {
                            const bool shouldLogWait =
                                bvhAddrs.missingMask != m_giBakeLastLoggedMissingMask ||
                                m_giBakeStatus.stalledFrames == 0u ||
                                ((m_giBakeStatus.stalledFrames + 1u) % 60u) == 0u;
                            if (shouldLogWait) {
                                AddGIBakeLog("WaitingForBvh", "Retrying SWRT BVH upload; missing=%s, phase=%s, lastFailure=%s, stalledFrames=%u, scene=%u instances / %u triangles",
                                             missing.c_str(),
                                             bvhDiag.lastPhase,
                                             bvhDiag.lastFailure[0] ? bvhDiag.lastFailure : "none",
                                             m_giBakeStatus.stalledFrames + 1u,
                                             static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                                             m_rayTracingScene.TriangleCount());
                                m_giBakeLastLoggedMissingMask = bvhAddrs.missingMask;
                            }
                            RefreshGIBakeStatus(GIBakeState::WaitingForBvh, bvhAddrs.missingMask);
                        }
                    }
                }
            }
        }
    }
}
