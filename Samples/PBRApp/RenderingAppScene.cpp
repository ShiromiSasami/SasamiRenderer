// What this sample puts in front of the camera: the view pose, the environment map, the four
// procedural materials, the models (three loaded from disk, four generated, one skinned) and
// the particle origin. Split out of the lifecycle hooks because this is the only part of the
// sample that names an asset path or a world coordinate - changing the scene never has to
// touch the hooks, and changing the hooks never has to read a coordinate.

#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "Object/StaticModel.h"
#include "Object/SampleFloorBox.h"
#include "Object/SkinnedModel.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"

namespace SasamiEngine::Sample::PBR
{
    namespace
    {
        constexpr const char* kSkyboxPanoramaPath = "Assets/Models/Bistro/san_giuseppe_bridge_4k.hdr";
        constexpr const char* kStanfordBunnyPath = "Models/stanford_bunny_pbr/scene.gltf";
        constexpr const char* kFoxPath = "Models/Fox/Fox.gltf";

        constexpr const char* kSponzaPath = "Models/Sponza/glTF/Sponza.gltf";

        SurfaceMaterial MakeMaterial(float r, float g, float b, float roughness, float metallic,
                                     float emissiveR = 0.0f, float emissiveG = 0.0f, float emissiveB = 0.0f)
        {
            SurfaceMaterial material{};
            material.baseColor[0] = r;
            material.baseColor[1] = g;
            material.baseColor[2] = b;
            material.baseColor[3] = 1.0f;
            material.emissive[0] = emissiveR;
            material.emissive[1] = emissiveG;
            material.emissive[2] = emissiveB;
            material.roughness = roughness;
            material.metallic = metallic;
            material.reflectionStrength = Math::DefaultReflectionStrength(roughness, metallic);
            material.occlusionStrength = 1.0f;
            return material;
        }

        SurfaceMaterial MakeTransparentMaterial(float r, float g, float b, float alpha,
                                                float roughness, float metallic,
                                                float transmission, float ior)
        {
            SurfaceMaterial material = MakeMaterial(r, g, b, roughness, metallic);
            material.baseColor[3] = alpha;
            material.transmission = transmission;
            material.ior = ior;
            return material;
        }
    }

    bool RenderingApp::CreateSceneCamera(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("Camera object creation failed.\n");
            app.RequestQuit();
            return false;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("SetMainCamera failed.\n");
            app.RequestQuit();
            return false;
        }
        m_camera->SetTarget(0.0f, 1.5f, -7.5f);
        m_camera->SetYawPitch(0.0f, 0.12f);
        m_camera->SetMoveSpeed(4.0f);
        return true;
    }

    bool RenderingApp::BuildSampleScene(ApplicationCore& app)
    {
        // Loaded on every backend, not just D3D12: the native paths cannot run the render-graph
        // skybox pass, but they still build their own environment cubemap out of this same HDR
        // source (Skybox::EnsureNativeEnvironmentCubemap), so skipping the load left them with
        // nothing but the procedural sky.
        if (!app.LoadSkyboxAsync(kSkyboxPanoramaPath, ApplicationCore::SkyboxLoadFormat::HdrEquirect)) {
            DebugLog("Skybox load failed: invalid path for selected format.\n");
        }

        m_sphereMaterial = MakeMaterial(0.98f, 0.84f, 0.32f, 0.05f, 1.0f);
        m_boxMaterial = MakeMaterial(0.72f, 0.80f, 0.90f, 0.14f, 1.0f);
        m_transparentSphereMaterial = MakeTransparentMaterial(0.35f, 0.78f, 0.95f, 0.35f, 0.04f, 0.0f, 0.92f, 1.33f);
        m_transparentBoxMaterial = MakeTransparentMaterial(0.92f, 0.58f, 0.26f, 0.42f, 0.08f, 0.0f, 0.78f, 1.50f);

        m_bunnyModel = app.CreateStaticModel();
        m_sponzaModel = app.CreateStaticModel();
        m_bistroModel = app.CreateStaticModel();
        m_sphereModel = app.CreateStaticModel();
        m_boxModel = app.CreateStaticModel();
        m_transparentSphereModel = app.CreateStaticModel();
        m_transparentBoxModel = app.CreateStaticModel();
        StaticModel* floorModel = app.CreateStaticModel();

        if (!m_bunnyModel || !m_sponzaModel || !m_bistroModel || !m_sphereModel || !m_boxModel ||
            !m_transparentSphereModel || !m_transparentBoxModel || !floorModel) {
            DebugLog("Sample scene object creation failed.\n");
            app.RequestQuit();
            return false;
        }

        if (!m_bunnyModel->LoadModelAsync(app, kStanfordBunnyPath, StaticModel::ModelFormat::Gltf, 0.01f)) {
            app.DeleteObject(m_bunnyModel);
            m_bunnyModel = nullptr;
            DebugLog("Bunny model load failed: Assets/Models/stanford_bunny_pbr/scene.gltf\n");
        } else {
            m_bunnyModel->SetTranslation(0.0f, 0.0f, 0.8f);
        }

        if (!m_sponzaModel->LoadModelAsync(app, kSponzaPath, StaticModel::ModelFormat::Gltf, 1.f)) {
            app.DeleteObject(m_sponzaModel);
            m_sponzaModel = nullptr;
            DebugLog("Sponza model load failed: Assets/Models/Sponza/glTF/Sponza.gltf\n");
        } else {
            m_sponzaModel->SetTranslation(0.f, 0.f, 1.f);
        }

        // Loaded synchronously, unlike the glTF models: parsing this FBX on a worker
        // thread reliably kills the process a few seconds later (no exception reaches any
        // handler), while the identical parse on the main thread is stable. Tracked as a
        // follow-up in TODO.md; until it is understood, correctness beats overlap here.
        if (!m_bistroModel->LoadModel(BISTRO_EXTERIOR_PATH, StaticModel::ModelFormat::Fbx, 1.0f)) {
            app.DeleteObject(m_bistroModel);
            m_bistroModel = nullptr;
            DebugLog("Bistro model load failed: Assets/Models/Bistro/BistroExterior.fbx\n");
        } else {
            // +X offset keeps the Bistro exterior from overlapping Sponza, which stays at the origin.
            m_bistroModel->SetTranslation(60.0f, 0.0f, 0.0f);
        }

        // Skinned animated model (glTF sample "Fox", authored in centimeters).
        SkinnedModel* foxModel = app.CreateSkinnedModel();
        if (foxModel) {
            if (!foxModel->LoadModel(kFoxPath)) {
                app.DeleteObject(foxModel);
                foxModel = nullptr;
                DebugLog("Fox model load failed: Assets/Models/Fox/Fox.gltf\n");
            } else {
                foxModel->SetUniformScale(0.02f);
                foxModel->SetTranslation(1.2f, 0.0f, 0.3f);
                foxModel->PlayAnimation(2); // Run — clearly visible loop
            }
        }

        StaticModel::SphereDesc sphereDesc{};
        sphereDesc.radius = 0.72f;
        sphereDesc.slices = 48u;
        sphereDesc.stacks = 24u;
        sphereDesc.material = m_sphereMaterial;
        m_sphereModel->AddSphere(sphereDesc);
        m_sphereModel->SetTranslation(-1.8f, 0.72f, 1.0f);

        StaticModel::BoxDesc boxDesc{};
        boxDesc.width = 1.18f;
        boxDesc.height = 1.18f;
        boxDesc.depth = 1.18f;
        boxDesc.material = m_boxMaterial;
        m_boxModel->AddBox(boxDesc);
        m_boxModel->SetTranslation(1.9f, 0.59f, 0.35f);

        StaticModel::SphereDesc transparentSphereDesc{};
        transparentSphereDesc.radius = 0.72f;
        transparentSphereDesc.slices = 48u;
        transparentSphereDesc.stacks = 24u;
        transparentSphereDesc.material = m_transparentSphereMaterial;
        m_transparentSphereModel->AddSphere(transparentSphereDesc);
        m_transparentSphereModel->SetTranslation(-4.0f, 0.72f, 1.05f);

        StaticModel::BoxDesc transparentBoxDesc{};
        transparentBoxDesc.width = 1.18f;
        transparentBoxDesc.height = 1.18f;
        transparentBoxDesc.depth = 1.18f;
        transparentBoxDesc.material = m_transparentBoxMaterial;
        m_transparentBoxModel->AddBox(transparentBoxDesc);
        m_transparentBoxModel->SetTranslation(4.0f, 0.59f, 0.35f);

        if (!m_sponzaModel) {
            AddSampleFloorBox(*floorModel, MakeMaterial(0.52f, 0.54f, 0.58f, 0.92f, 0.0f));
        } else {
            app.DeleteObject(floorModel);
            floorModel = nullptr;
        }

        // Probe grid fit is deferred (see ApplyDeferredGiSessionState): scene bounds are
        // not final until the async Sponza/bunny loads settle.

        app.SetParticleEmitOrigin(0.0f, 1.5f, 0.0f);
        return true;
    }
}
