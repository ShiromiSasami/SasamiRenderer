#pragma once

#include <wtypes.h>
#include "IApplication.h"
#include "Light/DirectionalLight.h"
#include "Object/SObject.h"
#include "Object/StaticModel.h"
#include "Object/PointLight.h"
#include "Object/SpotLight.h"
#include "ECS/EcsRegistry.h"
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
    class Renderer;
    struct RenderCameraProxy;

	class ApplicationCore
	{
    public:
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;
        using RenderNodeType = RendererEnums::RenderNodeType;

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
        void RenderFrame();
        void RenderFrame(const Camera& camera);
        void ResizeRenderer(UINT width, UINT height);

        DirectionalLight GetDirectionalLight() const;
        void SetDirectionalLight(const DirectionalLight& light);

        float GetIblIntensity() const;
        void SetIblIntensity(float intensity);
        bool GetUseTessellation() const;
        void SetUseTessellation(bool enabled);
        int GetRasterShaderModeIndex() const;
        void SetRasterShaderModeIndex(int modeIndex);
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
