#include "AppFramework/Editor/EditorModelSpawner.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/StaticModel.h"

namespace SasamiEngine::App
{
    StaticModel* EditorModelSpawner::Spawn(ApplicationCore& app, const std::string& relPath,
                                           MeshComponent::ModelFormat format, const float position[3],
                                           std::string& error)
    {
        StaticModel* model = app.CreateStaticModel();
        if (model == nullptr)
        {
            error = "static model creation failed";
            return nullptr;
        }
        if (!model->LoadModel(relPath, format, 1.0f))
        {
            app.DeleteObject(model);
            error = "load failed: Assets/" + relPath;
            return nullptr;
        }
        model->SetTranslation(position[0], position[1], position[2]);
        return model;
    }

    void EditorModelSpawner::PointInFrontOfCamera(ApplicationCore& app, float distance, float outPosition[3])
    {
        const Render::RenderCameraProxy& proxy = app.GetMainCameraProxy();
        for (int i = 0; i < 3; ++i)
        {
            outPosition[i] = app.GetMainCamera() != nullptr
                ? proxy.cameraPosition[i] + proxy.cameraForward[i] * distance
                : 0.0f;
        }
    }
}
