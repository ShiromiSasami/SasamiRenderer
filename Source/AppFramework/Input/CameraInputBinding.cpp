#include "CameraInputBinding.h"
#include "InputSystem.h"
#include "Object/Camera.h"

namespace SasamiEngine::App
{
    std::vector<boost::signals2::scoped_connection> ConnectCameraInput(std::function<Camera*()> getCamera)
    {
        std::vector<boost::signals2::scoped_connection> connections;
        auto& input = InputSystem::Instance();

        connections.emplace_back(input.ConnectOnKeyDown([getCamera](WPARAM k) {
            if (Camera* camera = getCamera()) camera->OnKeyDown(k);
        }));
        connections.emplace_back(input.ConnectOnKeyUp([getCamera](WPARAM k) {
            if (Camera* camera = getCamera()) camera->OnKeyUp(k);
        }));
        connections.emplace_back(input.ConnectOnMouseDown([getCamera](int x, int y) {
            if (Camera* camera = getCamera()) camera->OnMouseDown(x, y);
        }));
        connections.emplace_back(input.ConnectOnMouseUp([getCamera]() {
            if (Camera* camera = getCamera()) camera->OnMouseUp();
        }));
        connections.emplace_back(input.ConnectOnMouseMove([getCamera](int x, int y, bool held) {
            if (Camera* camera = getCamera()) camera->OnMouseMove(x, y, held);
        }));
        connections.emplace_back(input.ConnectOnMouseWheel([getCamera](int d) {
            if (Camera* camera = getCamera()) camera->OnMouseWheel(d);
        }));
        return connections;
    }
}
