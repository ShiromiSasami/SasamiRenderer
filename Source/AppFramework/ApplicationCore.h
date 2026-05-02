#pragma once

#include <wtypes.h>
#include "IApplication.h"
#include "Light/DirectionalLight.h"
#include "Object/SObject.h"
#include "Object/StaticModel.h"
#include "Object/PointLight.h"
#include "Object/SpotLight.h"
#include "ECS/EcsRegistry.h"
#include "Renderer/Core/Renderer.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Structures/RendererEnums.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SasamiRenderer
{
    class Camera;
    class IRenderNode;
    struct RenderCameraProxy;

	class ApplicationCore
	{
    public:
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;
        using RenderNodeType = RendererEnums::RenderNodeType;
        using RenderPathMode = RendererEnums::RenderPathMode;

        ApplicationCore(UINT width, UINT height, const wchar_t* title, IApplication* game = nullptr);
        ~ApplicationCore();

        int Run();
        void SetGame(IApplication* game) { m_game = game; }
        HWND GetHwnd() const { return m_hwnd; }
        UINT GetWidth() const { return m_width; }
        UINT GetHeight() const { return m_height; }
        float GetDeltaTime() const { return m_deltaTime; }
        void RequestQuit() { m_running = false; }
        bool IsRendererReady() const;
        Renderer& GetRenderer() { return *m_renderer; }
        void RenderFrame();
        void RenderFrame(const Camera& camera);
        void ResizeRenderer(UINT width, UINT height);

        DirectionalLight GetDirectionalLight() const;
        void SetDirectionalLight(const DirectionalLight& light);
        bool GetShowDirectionalLightOnSkybox() const;
        void SetShowDirectionalLightOnSkybox(bool enabled);
        float GetDirectionalLightOnSkyboxAngularRadius() const;
        void SetDirectionalLightOnSkyboxAngularRadius(float radians);

        float GetIblIntensity() const;
        void SetIblIntensity(float intensity);
        bool GetUseTessellation() const;
        void SetUseTessellation(bool enabled);
        bool GetTessWireframeEnabled() const;
        void SetTessWireframeEnabled(bool enabled);
        bool GetTessDebugColorsEnabled() const;
        void SetTessDebugColorsEnabled(bool enabled);
        bool  GetVolumetricCloudEnabled() const;
        void  SetVolumetricCloudEnabled(bool enabled);
        float GetCloudCover() const;
        void  SetCloudCover(float v);
        float GetCloudDensity() const;
        void  SetCloudDensity(float v);
        float GetCloudWindSpeed() const;
        void  SetCloudWindSpeed(float v);
        float GetCloudBaseAlt() const;
        void  SetCloudBaseAlt(float v);
        float GetCloudTopAlt() const;
        void  SetCloudTopAlt(float v);
        bool GetDebugProbeGridEnabled() const;
        void SetDebugProbeGridEnabled(bool enabled);
        float GetDebugProbeRadius() const;
        void SetDebugProbeRadius(float radius);
        void FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                  float bMaxX, float bMaxY, float bMaxZ,
                                  float margin = 1.0f);
        void ReinsertDebugProbeGrid();
        bool GetMeshletDebugViewEnabled() const;
        void SetMeshletDebugViewEnabled(bool enabled);
        bool GetUseMeshShader() const;
        void SetUseMeshShader(bool enabled);
        int GetRasterShaderModeIndex() const;
        void SetRasterShaderModeIndex(int modeIndex);
        int GetRenderPathModeIndex() const;
        void SetRenderPathModeIndex(int modeIndex);
        int GetRayTracingPerformancePresetIndex() const;
        void SetRayTracingPerformancePresetIndex(int presetIndex);
        bool GetRayTracingDynamicResolutionEnabled() const;
        void SetRayTracingDynamicResolutionEnabled(bool enabled);
        int GetRayTracingMaxBounceCount() const;
        void SetRayTracingMaxBounceCount(int count);
        bool GetRasterSoftwareRayTracedDirectionalShadowEnabled() const;
        void SetRasterSoftwareRayTracedDirectionalShadowEnabled(bool enabled);
        bool GetRasterSoftwareRayTracedReflectionEnabled() const;
        void SetRasterSoftwareRayTracedReflectionEnabled(bool enabled);
        bool GetRasterSoftwareRayTracedAmbientOcclusionEnabled() const;
        void SetRasterSoftwareRayTracedAmbientOcclusionEnabled(bool enabled);
        int GetAmbientOcclusionModeIndex() const;
        void SetAmbientOcclusionModeIndex(int modeIndex);
        bool GetSSAOEnabled() const;
        void SetSSAOEnabled(bool enabled);
        float GetSSAORadius() const;
        void SetSSAORadius(float radius);
        float GetSSAOBias() const;
        void SetSSAOBias(float bias);
        float GetSSAOIntensity() const;
        void SetSSAOIntensity(float intensity);
        float GetSSAOThickness() const;
        void SetSSAOThickness(float thickness);
        int GetSSAOQualityIndex() const;
        void SetSSAOQualityIndex(int qualityIndex);
        int GetSwrtAoSampleCount() const;
        void SetSwrtAoSampleCount(int count);
        float GetAoMinOcclusion() const;
        void  SetAoMinOcclusion(float v);
        bool GetSwrtUseReSTIR() const;
        void SetSwrtUseReSTIR(bool useReSTIR);
        int  GetSwrtSamplingMode() const;
        void SetSwrtSamplingMode(int mode);
        int  GetSwrtSamplesPerPixel() const;
        void SetSwrtSamplesPerPixel(int n);
        int  GetSwrtMaxBounces() const;
        void SetSwrtMaxBounces(int n);
        bool IsHardwareRayTracingSupported() const;
        Renderer::RayTracingStats GetRayTracingStats() const;
        int GetGBufferDebugViewIndex() const;
        void SetGBufferDebugViewIndex(int modeIndex);
        void CycleGBufferDebugView(int delta = 1);

        bool LoadSkybox(const std::string& resourcePath, SkyboxLoadFormat format = SkyboxLoadFormat::Auto);
        std::vector<RenderNodeType> GetRenderNodeSequence() const;
        void SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence);
        bool AddRenderPass(const std::shared_ptr<IRenderNode>& renderPass);
        bool AddRenderPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        bool AddRenderPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        bool ReplaceRenderPass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        void ClearRenderPasses();

        template<typename TObject, typename... TArgs>
        TObject* CreateObject(TArgs&&... args)
        {
            static_assert(std::is_base_of_v<SObject, TObject>, "TObject must derive from SObject.");

            std::unique_ptr<TObject> object = std::make_unique<TObject>(std::forward<TArgs>(args)...);
            if (!object) {
                return nullptr;
            }

            TObject* created = object.get();
            m_objects.push_back(std::move(object));
            RegisterObjectInEcs(created);
            m_objectsDirty = true;
            return created;
        }

        StaticModel* CreateStaticModel()
        {
            return CreateObject<StaticModel>();
        }
        PointLight* CreatePointLightObject() { return CreateObject<PointLight>(); }
        SpotLight* CreateSpotLightObject() { return CreateObject<SpotLight>(); }
        std::vector<PointLight*> GetPointLightObjects() const;
        std::vector<SpotLight*> GetSpotLightObjects() const;
        Camera* CreateCameraObject();
        bool SetMainCamera(Camera* camera);
        Camera* GetMainCamera() const { return m_activeCamera; }
        const RenderCameraProxy& GetMainCameraProxy() const { return m_activeCameraProxy; }
        bool SetActiveCamera(Camera* camera);
        Camera* GetActiveCamera() const { return m_activeCamera; }
        bool DeleteObject(SObject* object);
        void ClearObjects();
        void InvalidateRenderObjects() { m_objectsDirty = true; }

    private:
        using EntityId = EcsRegistry::EntityId;
        struct ObjectRefComponent
        {
            SObject* object = nullptr;
        };
        struct StaticModelTag {};
        struct PointLightTag {};
        struct SpotLightTag {};
        struct CameraTag {};

        bool InitializeRenderer();
        void ShutdownRenderer();
        void RenderFrameInternal(const RenderCameraProxy& cameraProxy);
        bool UpdateMainCameraProxy();
        void SyncModelsToRenderer(Renderer& renderer);
        void SyncLightObjectsToRenderer(Renderer& renderer) const;
        void RegisterObjectInEcs(SObject* object);
        void UnregisterObjectInEcs(SObject* object);
        void OnInit();
        void OnUpdate(float deltaTime);
        void OnRender();
        void OnDestroy();

    private:
        static LRESULT CALLBACK WindowProccessStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT WindowProccess(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND m_hwnd;
        UINT m_width;
        UINT m_height;
        const wchar_t* m_title;
        bool m_running;

        IApplication* m_game;
        std::unique_ptr<Renderer> m_renderer;
        float m_deltaTime = 0.0f;
        std::vector<std::unique_ptr<SObject>> m_objects;
        EcsRegistry m_ecsRegistry;
        std::unordered_map<SObject*, EntityId> m_objectEntityMap;
        Camera* m_activeCamera = nullptr;
        RenderCameraProxy m_activeCameraProxy{};
        bool m_objectsDirty = false;
		};
}
