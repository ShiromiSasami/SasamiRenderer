#include "AppFramework/Debug/RemoteControl/DebugEditorCommands.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/Editor/ContentAssetCatalog.h"
#include "AppFramework/Editor/EditorGizmo.h"
#include "AppFramework/Editor/EditorPicking.h"
#include "AppFramework/Editor/EditorPlaySession.h"
#include "AppFramework/Editor/EditorTransformMath.h"
#include "AppFramework/Editor/EditorViewMode.h"
#include "AppFramework/Editor/EditorWindows.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SkinnedModel.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Object/StaticModel.h"
#include "AppFramework/UI/ImGuiCoordinator.h"
#include "AppFramework/UI/UIPanelRegistry.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        template <typename T>
        SObject* PickObject(const std::vector<T*>& list, size_t index)
        {
            return index < list.size() ? static_cast<SObject*>(list[index]) : nullptr;
        }

        bool ParseKind(const std::string& text, EditorObjectKind& kind)
        {
            const EditorObjectKind kinds[] = { EditorObjectKind::StaticModel, EditorObjectKind::SkinnedModel,
                EditorObjectKind::PointLight, EditorObjectKind::SpotLight, EditorObjectKind::Camera,
                EditorObjectKind::DirectionalLight };
            for (EditorObjectKind k : kinds)
            {
                if (text == EditorObjectKindName(k))
                {
                    kind = k;
                    return true;
                }
            }
            return false;
        }

        std::string Select(ApplicationCore& app, const std::string& kindText, const std::string& indexText)
        {
            EditorObjectKind kind = EditorObjectKind::None;
            if (!ParseKind(kindText, kind))
                return "ERR kind must be static|skinned|point|spot|camera|directional";

            size_t index = 0;
            try {
                index = std::stoul(indexText);
            }
            catch (...) {
                return "ERR index must be an integer";
            }

            SObject* object = nullptr;
            switch (kind)
            {
            case EditorObjectKind::StaticModel: object = PickObject(app.GetStaticModels(), index); break;
            case EditorObjectKind::SkinnedModel: object = PickObject(app.GetSkinnedModels(), index); break;
            case EditorObjectKind::PointLight: object = PickObject(app.GetPointLightObjects(), index); break;
            case EditorObjectKind::SpotLight: object = PickObject(app.GetSpotLightObjects(), index); break;
            case EditorObjectKind::Camera: object = PickObject(app.GetCameraObjects(), index); break;
            default: break;
            }
            if (kind != EditorObjectKind::DirectionalLight && object == nullptr)
                return "ERR index out of range";

            app.GetEditorWindows().Selection().Select(kind, object, app.GetSceneGeneration());
            return std::string("OK selected=") + kindText + " index=" + std::to_string(index);
        }

        template <typename T>
        long long IndexOf(const std::vector<T*>& list, const SObject* object)
        {
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (static_cast<const SObject*>(list[i]) == object)
                    return static_cast<long long>(i);
            }
            return -1;
        }

        // Same path as a left click in the 3D view; x, y are main-viewport pixels.
        std::string Pick(ApplicationCore& app, const std::string& xText, const std::string& yText)
        {
            float x = 0.0f;
            float y = 0.0f;
            try {
                x = std::stof(xText);
                y = std::stof(yText);
            }
            catch (...) {
                return "ERR x y must be numbers";
            }

            EditorWindows& editor = app.GetEditorWindows();
            const EditorObjectKind kind = editor.PickAt(app, x, y);
            const SObject* object = editor.Selection().object;
            long long index = -1;
            switch (kind)
            {
            case EditorObjectKind::StaticModel: index = IndexOf(app.GetStaticModels(), object); break;
            case EditorObjectKind::SkinnedModel: index = IndexOf(app.GetSkinnedModels(), object); break;
            case EditorObjectKind::PointLight: index = IndexOf(app.GetPointLightObjects(), object); break;
            case EditorObjectKind::SpotLight: index = IndexOf(app.GetSpotLightObjects(), object); break;
            default: break;
            }

            std::ostringstream out;
            out << "OK hit=" << EditorObjectKindName(kind) << " index=" << index;
            float boxMin[3], boxMax[3];
            const float* model = nullptr;
            if (GetSelectionBox(editor.Selection(), boxMin, boxMax, model))
            {
                out << " localMin=" << boxMin[0] << "," << boxMin[1] << "," << boxMin[2]
                    << " localMax=" << boxMax[0] << "," << boxMax[1] << "," << boxMax[2];
            }
            return out.str();
        }

        std::string ListPanels()
        {
            std::ostringstream out;
            const std::vector<UI::PanelInfo> infos = ImGuiCoordinator::Instance().Panels().List();
            out << "OK panels=" << infos.size();
            for (const UI::PanelInfo& info : infos)
            {
                out << " [" << info.category << "/" << info.name << " tabs=" << info.tabCount
                    << " open=" << (info.open ? 1 : 0) << "]";
            }
            return out.str();
        }

        // args: panel <name words...> on|off (names such as "PBR Controls" contain spaces).
        std::string SetPanelOpen(const std::vector<std::string>& args)
        {
            std::string name;
            for (size_t i = 1; i + 1 < args.size(); ++i)
            {
                name += (i > 1 ? " " : "") + args[i];
            }
            const std::string& state = args.back();
            if (state != "on" && state != "off")
                return "ERR last argument must be on or off";
            if (!ImGuiCoordinator::Instance().Panels().SetOpen(name, state == "on"))
                return "ERR no panel named '" + name + "' (see editor panels)";
            return "OK panel=" + name + " open=" + (state == "on" ? "1" : "0");
        }

        std::string Status(ApplicationCore& app)
        {
            EditorWindows& editor = app.GetEditorWindows();
            SceneHierarchyPanel::ValidateSelection(app, editor.Selection());
            const EditorSelection& selection = editor.Selection();

            std::ostringstream out;
            out << "OK visible=" << (editor.IsVisible() ? 1 : 0)
                << " selected=" << EditorObjectKindName(selection.kind)
                << " static=" << app.GetStaticModels().size() << " skinned=" << app.GetSkinnedModels().size()
                << " point=" << app.GetPointLightObjects().size() << " spot=" << app.GetSpotLightObjects().size()
                << " cameras=" << app.GetCameraObjects().size();

            const float* matrix = nullptr;
            if (selection.kind == EditorObjectKind::StaticModel)
                matrix = static_cast<StaticModel*>(selection.object)->MeshComponentRef().GetModelMatrix();
            else if (selection.kind == EditorObjectKind::SkinnedModel)
                matrix = static_cast<SkinnedModel*>(selection.object)->SkinnedMeshComponentRef().GetModelMatrix();
            if (matrix != nullptr)
            {
                const EditorTrs trs = DecomposeTrs(matrix);
                out << " pos=" << trs.translation.x << "," << trs.translation.y << "," << trs.translation.z
                    << " scale=" << trs.scale.x << "," << trs.scale.y << "," << trs.scale.z;
            }
            return out.str();
        }

        // args: content list | refresh | open on|off | add <relpath>
        // Every value the snapshot is meant to restore, flattened in a fixed order (compared bit for bit).
        std::vector<float> SceneFingerprint(const ApplicationCore& app)
        {
            std::vector<float> values;
            for (StaticModel* model : app.GetStaticModels())
            {
                const float* m = model->MeshComponentRef().GetModelMatrix();
                values.insert(values.end(), m, m + 16);
            }
            for (SkinnedModel* model : app.GetSkinnedModels())
            {
                const float* m = model->SkinnedMeshComponentRef().GetModelMatrix();
                values.insert(values.end(), m, m + 16);
            }
            for (PointLight* light : app.GetPointLightObjects())
            {
                const Vector3& p = light->Transform().position;
                values.insert(values.end(), { p.x, p.y, p.z, light->Intensity(), light->Range() });
                values.insert(values.end(), light->ColorData(), light->ColorData() + 4);
            }
            for (SpotLight* light : app.GetSpotLightObjects())
            {
                const Vector3& p = light->Transform().position;
                values.insert(values.end(), { p.x, p.y, p.z, light->Intensity(), light->Range(),
                                              light->InnerAngle(), light->OuterAngle() });
            }
            for (Camera* camera : app.GetCameraObjects())
            {
                const Vector3& t = camera->Transform().position;
                values.insert(values.end(), { t.x, t.y, t.z, camera->Yaw(), camera->Pitch(), camera->Distance() });
            }
            const DirectionalLight& sun = app.GetDirectionalLight();
            values.insert(values.end(), { sun.yaw, sun.pitch, sun.intensity, sun.distance });
            return values;
        }

        // Play, disturb every kind of object the snapshot covers, Stop, and compare against the state before Play.
        std::string PlayCheck(ApplicationCore& app)
        {
            EditorPlaySession& session = app.GetEditorWindows().PlaySession();
            if (session.IsPlaying())
            {
                return "ERR already playing (editor stop first)";
            }
            const std::vector<float> before = SceneFingerprint(app);
            session.Play(app);

            uint32_t disturbed = 0;
            for (StaticModel* model : app.GetStaticModels())
            {
                float m[16];
                std::memcpy(m, model->MeshComponentRef().GetModelMatrix(), sizeof(m));
                m[12] += 1.0f;
                model->MeshComponentRef().SetModelMatrix(m);
                ++disturbed;
            }
            for (SkinnedModel* model : app.GetSkinnedModels())
            {
                float m[16];
                std::memcpy(m, model->SkinnedMeshComponentRef().GetModelMatrix(), sizeof(m));
                m[12] += 1.0f;
                model->SkinnedMeshComponentRef().SetModelMatrix(m);
                ++disturbed;
            }
            for (PointLight* light : app.GetPointLightObjects())
            {
                light->Transform().position.y += 1.0f;
                light->SetIntensity(light->Intensity() * 2.0f + 1.0f);
                ++disturbed;
            }
            for (SpotLight* light : app.GetSpotLightObjects())
            {
                light->Transform().position.y += 1.0f;
                ++disturbed;
            }
            for (Camera* camera : app.GetCameraObjects())
            {
                camera->SetDistance(camera->Distance() + 1.0f);
                camera->SetYawPitch(camera->Yaw() + 0.5f, camera->Pitch());
                ++disturbed;
            }
            DirectionalLight sun = app.GetDirectionalLight();
            sun.yaw += 0.5f;
            sun.intensity += 1.0f;
            app.SetDirectionalLight(sun);
            app.InvalidateRenderObjects();
            const bool changed = SceneFingerprint(app) != before;

            EditorSceneRestoreReport report;
            session.Stop(app, &report);
            const std::vector<float> after = SceneFingerprint(app);
            uint32_t mismatch = after.size() == before.size() ? 0u : 1u;
            for (size_t i = 0; after.size() == before.size() && i < before.size(); ++i)
            {
                mismatch += std::memcmp(&before[i], &after[i], sizeof(float)) != 0 ? 1u : 0u;
            }
            std::ostringstream out;
            out << (mismatch == 0 && changed ? "OK" : "FAIL") << " values=" << before.size()
                << " disturbed=" << disturbed << " changed=" << (changed ? 1 : 0) << " mismatch=" << mismatch
                << " restored=" << report.restored << " missing=" << report.missing << " added=" << report.added;
            return out.str();
        }

        std::string Play(ApplicationCore& app, const std::vector<std::string>& args)
        {
            EditorPlaySession& session = app.GetEditorWindows().PlaySession();
            if (args[0] == "play" && args.size() == 2 && args[1] == "check")
            {
                return PlayCheck(app);
            }
            if (args[0] == "play")
            {
                return session.Play(app) ? "OK playing=1" : "ERR already playing";
            }
            if (args[0] == "stop")
            {
                EditorSceneRestoreReport report;
                if (!session.Stop(app, &report))
                {
                    return "ERR not playing";
                }
                return "OK playing=0 restored=" + std::to_string(report.restored) + " missing="
                     + std::to_string(report.missing) + " added=" + std::to_string(report.added);
            }
            return std::string("OK playing=") + (session.IsPlaying() ? "1" : "0");
        }

        std::string Content(ApplicationCore& app, const std::vector<std::string>& args)
        {
            EditorWindows& editor = app.GetEditorWindows();
            ContentBrowserPanel& browser = editor.ContentBrowser();
            if (args.size() == 2 && (args[1] == "list" || args[1] == "refresh"))
            {
                if (args[1] == "refresh" || !browser.Catalog().HasScanned())
                    browser.Catalog().Scan();
                std::ostringstream out;
                out << "OK models=" << browser.Catalog().Entries().size();
                for (const ContentAssetEntry& entry : browser.Catalog().Entries())
                    out << " " << entry.relPath;
                return out.str();
            }
            if (args.size() == 3 && args[1] == "open" && (args[2] == "on" || args[2] == "off"))
            {
                editor.SetContentBrowserOpen(args[2] == "on");
                return std::string("OK content=") + (args[2] == "on" ? "1" : "0");
            }
            if (args.size() == 3 && args[1] == "add")
                return browser.AddToScene(app, editor.Selection(), args[2]);
            return "ERR usage: editor content list | refresh | open on|off | add <Assets-relative path>";
        }
        std::string Gizmo(ApplicationCore& app, const std::vector<std::string>& args)
        {
            EditorGizmo& gizmo = app.GetEditorWindows().Gizmo();
            if (args.size() == 2)
            {
                if (args[1] == "translate")
                    gizmo.SetOperation(EditorGizmoOperation::Translate);
                else if (args[1] == "rotate")
                    gizmo.SetOperation(EditorGizmoOperation::Rotate);
                else if (args[1] == "scale")
                    gizmo.SetOperation(EditorGizmoOperation::Scale);
                else if (args[1] == "local")
                    gizmo.SetSpace(EditorGizmoSpace::Local);
                else if (args[1] == "world")
                    gizmo.SetSpace(EditorGizmoSpace::World);
                else if (args[1] != "status")
                    return "ERR usage: editor gizmo translate|rotate|scale|local|world|status";
            }
            return std::string("OK op=") + EditorGizmoOperationName(gizmo.Operation()) + " space="
                 + EditorGizmoSpaceName(gizmo.Space()) + " using=" + (gizmo.IsUsing() ? "1" : "0")
                 + " edits=" + std::to_string(gizmo.EditCount())
                 + " input=" + (gizmo.LastInputEnabled() ? "1" : "0")
                 + " wantMouse=" + (ImGui::GetIO().WantCaptureMouse ? "1" : "0")
                 + " mouse=" + std::to_string(static_cast<int>(ImGui::GetIO().MousePos.x)) + ","
                 + std::to_string(static_cast<int>(ImGui::GetIO().MousePos.y))
                 + " hovered=" + (GImGui->HoveredWindow != nullptr ? GImGui->HoveredWindow->Name : "none");
        }
    }

    void RegisterEditorCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("editor", "on | off | status | select <static|skinned|point|spot|camera|directional> <index> | pick <x> <y> | panels | panel <name> on|off | view [lit|unlit|wireframe] | content list|refresh|open on|off|add <relpath> | gizmo translate|rotate|scale|local|world|status | layout reset | play [check] | stop | playstate | selftest - UE-style editor: WorldObjects/Details/Content Browser docked (Ctrl+E toggles, left click in the view picks, Window menu opens panels and resets the layout, View menu sets the view mode, the Play/Stop toolbar restores the edited scene, the ImGuizmo gizmo moves the selection; Space cycles T/R/S)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
                    app.GetEditorWindows().SetVisible(args[0] == "on");
                    return std::string("OK visible=") + (args[0] == "on" ? "1" : "0");
                }

                if (args.size() == 1 && args[0] == "status")
                    return Status(app);

                if (args.size() == 3 && args[0] == "select")
                    return Select(app, args[1], args[2]);

                if (args.size() == 3 && args[0] == "pick")
                    return Pick(app, args[1], args[2]);

                if (args.size() == 1 && args[0] == "panels")
                    return ListPanels();

                if (args.size() >= 3 && args[0] == "panel")
                    return SetPanelOpen(args);

                if (args.size() >= 2 && args[0] == "content")
                    return Content(app, args);

                if (args.size() == 2 && args[0] == "layout" && args[1] == "reset") {
                    app.GetEditorWindows().RequestLayoutReset();
                    return "OK layout=reset";
                }

                if ((args.size() == 1 || args.size() == 2) && args[0] == "gizmo")
                    return Gizmo(app, args);

                if (!args.empty() && (args[0] == "play" || args[0] == "stop" || args[0] == "playstate"))
                    return Play(app, args);

                if ((args.size() == 1 || args.size() == 2) && args[0] == "view") {
                    if (args.size() == 2) {
                        EditorViewMode mode = EditorViewMode::Lit;
                        if (!ParseEditorViewMode(args[1].c_str(), mode))
                            return "ERR view must be lit, unlit or wireframe";
                        ApplyEditorViewMode(app, mode);
                    }
                    return std::string("OK view=") + EditorViewModeName(CurrentEditorViewMode(app));
                }

                if (args.size() == 1 && args[0] == "selftest") {
                    int checks = 0;
                    int failures = RunEditorTransformSelfTest(checks);
                    failures += RunEditorPickingSelfTest(checks);
                    failures += UI::RunPanelRegistrySelfTest(checks);
                    failures += RunEditorViewModeSelfTest(checks);
                    failures += RunContentAssetCatalogSelfTest(checks);
                    failures += RunEditorGizmoSelfTest(checks);
                    return std::string(failures == 0 ? "OK" : "FAIL") + " checks=" + std::to_string(checks)
                        + " failures=" + std::to_string(failures);
                }

                return "ERR usage: editor on | off | status | select <kind> <index> | pick <x> <y> | panels | panel <name> on|off | view [lit|unlit|wireframe] | content ... | gizmo <op|space|status> | layout reset | play [check] | stop | playstate | selftest";
            });
    }
}
