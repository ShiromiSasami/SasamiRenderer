#pragma once
#include <functional>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::App
{
    class Camera;

    // Forwards InputSystem key/mouse events to whichever camera `getCamera` returns at event time
    // (null = ignored). The returned connections disconnect when destroyed.
    std::vector<boost::signals2::scoped_connection> ConnectCameraInput(std::function<Camera*()> getCamera);
}
