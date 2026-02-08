#include <windows.h>
#include "Application.h"
#include "RenderingApp.h"

using namespace SasamiRenderer;

int WINAPI wmain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    RenderingApp appImpl;
    Application app(1280, 720, L"PBR App", &appImpl);
    return app.Run();
}
