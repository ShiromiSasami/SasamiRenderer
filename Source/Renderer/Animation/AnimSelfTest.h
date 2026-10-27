#pragma once
#include <string>

namespace SasamiEngine::Render::Anim
{
    // CPU checks for the animation runtime: math identities, clip sampling against
    // AnimationController, blending, springs, inertialization continuity, root motion, IK reach.
    // Returns one line per check ("ok name ..." / "FAIL name ...") and a final "anim selftest: N/M passed".
    std::string RunAnimSelfTest();
}

namespace SasamiEngine { using namespace Render; }
