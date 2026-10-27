#pragma once
#include <string>

namespace SasamiEngine::App::Gameplay
{
    // CPU checks for CharacterMovement, FollowCamera and CameraShake. Same output format as
    // Render::Anim::RunAnimSelfTest ("ok ..." / "FAIL ..." lines and an N/M summary).
    std::string RunGameplaySelfTest();
}

namespace SasamiEngine { using namespace App; }
