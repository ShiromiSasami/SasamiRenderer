#include "Renderer/Crowd/CrowdSelfTest.h"

#include "Renderer/Crowd/CrowdSimulation.h"
#include "Renderer/Crowd/CrowdStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        template <typename T>
        bool SameVector(const std::vector<T>& a, const std::vector<T>& b)
        {
            return a.size() == b.size()
                && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
        }

        bool SameStore(const CrowdStore& a, const CrowdStore& b)
        {
            return SameVector(a.posX, b.posX) && SameVector(a.posY, b.posY) && SameVector(a.posZ, b.posZ)
                && SameVector(a.velX, b.velX) && SameVector(a.velZ, b.velZ)
                && SameVector(a.animTime, b.animTime) && SameVector(a.animRate, b.animRate)
                && SameVector(a.clip, b.clip);
        }

        CrowdStore MakeSingleAgent(float x, float z, float vx, float vz, float t, float rate)
        {
            CrowdStore s;
            s.Resize(1);
            s.posX[0] = x;
            s.posY[0] = 0.0f;
            s.posZ[0] = z;
            s.velX[0] = vx;
            s.velZ[0] = vz;
            s.animTime[0] = t;
            s.animRate[0] = rate;
            s.clip[0] = 0;
            return s;
        }
    }

    std::string RunCrowdSelfTest()
    {
        uint32_t checks = 0;
        std::string error;
        auto check = [&](bool ok, const char* what) {
            ++checks;
            if (!ok && error.empty()) {
                error = std::string("ERR ") + what;
            }
            return ok;
        };

        CrowdSimParams params;
        params.clipCount = 3;
        params.clipDurations[0] = 1.0f;
        params.clipDurations[1] = 0.7f;
        params.clipDurations[2] = 2.3f;
        const float minSpeed = 0.5f;
        const float maxSpeed = 3.0f;

        // a. Spawn determinism
        {
            CrowdStore a, b, c;
            SpawnCrowd(a, 10000, params, minSpeed, maxSpeed, 1234u);
            SpawnCrowd(b, 10000, params, minSpeed, maxSpeed, 1234u);
            SpawnCrowd(c, 10000, params, minSpeed, maxSpeed, 4321u);
            check(a.Count() == 10000, "spawn count");
            check(SameStore(a, b), "spawn not deterministic for same seed");
            check(!SameStore(a, c), "spawn identical for different seeds");
            if (!error.empty()) {
                return error;
            }

            // b. Spawn invariants
            const float tol = 1e-4f;
            bool inBounds = true, clipOk = true, timeOk = true, speedOk = true;
            for (uint32_t i = 0; i < a.Count(); ++i) {
                inBounds = inBounds && a.posX[i] >= params.boundsMinX && a.posX[i] <= params.boundsMaxX
                    && a.posZ[i] >= params.boundsMinZ && a.posZ[i] <= params.boundsMaxZ;
                clipOk = clipOk && a.clip[i] < params.clipCount;
                if (a.clip[i] < params.clipCount) {
                    timeOk = timeOk && a.animTime[i] >= 0.0f && a.animTime[i] < params.clipDurations[a.clip[i]];
                } else {
                    timeOk = false;
                }
                const float speed = std::sqrt(a.velX[i] * a.velX[i] + a.velZ[i] * a.velZ[i]);
                speedOk = speedOk && speed >= minSpeed - tol && speed <= maxSpeed + tol;
            }
            check(inBounds, "spawn position outside bounds");
            check(clipOk, "spawn clip >= clipCount");
            check(timeOk, "spawn animTime out of [0, duration)");
            check(speedOk, "spawn speed out of range");
            if (!error.empty()) {
                return error;
            }
        }

        // c. Serial vs parallel, plus post-run invariants (d, e)
        {
            CrowdStore serial;
            SpawnCrowd(serial, 20000, params, minSpeed, maxSpeed, 99u);
            CrowdStore parallel = serial;
            for (int step = 0; step < 300; ++step) {
                StepCrowdCpu(serial, params, CrowdUpdateBackend::CpuSerial);
                StepCrowdCpu(parallel, params, CrowdUpdateBackend::CpuParallel);
            }
            check(SameVector(serial.posX, parallel.posX), "serial/parallel posX differ");
            check(SameVector(serial.posY, parallel.posY), "serial/parallel posY differ");
            check(SameVector(serial.posZ, parallel.posZ), "serial/parallel posZ differ");
            check(SameVector(serial.velX, parallel.velX), "serial/parallel velX differ");
            check(SameVector(serial.velZ, parallel.velZ), "serial/parallel velZ differ");
            check(SameVector(serial.animTime, parallel.animTime), "serial/parallel animTime differ");
            check(SameVector(serial.animRate, parallel.animRate), "serial/parallel animRate differ");
            check(SameVector(serial.clip, parallel.clip), "serial/parallel clip differ");

            bool inBounds = true, timeOk = true;
            for (uint32_t i = 0; i < serial.Count(); ++i) {
                inBounds = inBounds && serial.posX[i] >= params.boundsMinX && serial.posX[i] <= params.boundsMaxX
                    && serial.posZ[i] >= params.boundsMinZ && serial.posZ[i] <= params.boundsMaxZ;
                timeOk = timeOk && serial.animTime[i] >= 0.0f && serial.animTime[i] < params.clipDurations[serial.clip[i]];
            }
            check(inBounds, "agent outside bounds after 300 steps");
            check(timeOk, "animTime out of [0, duration) after 300 steps");
            if (!error.empty()) {
                return error;
            }
        }

        // d. Reflection
        {
            CrowdSimParams p;
            p.deltaTime = 0.5f;
            CrowdStore s = MakeSingleAgent(p.boundsMaxX - 0.1f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
            StepCrowdRange(s, p, 0, 1);
            check(std::fabs(s.posX[0] - (p.boundsMaxX - 0.4f)) < 1e-5f, "reflect maxX position");
            check(s.velX[0] == -1.0f, "reflect maxX velocity");

            CrowdStore m = MakeSingleAgent(0.0f, p.boundsMinZ + 0.1f, 0.0f, -1.0f, 0.0f, 1.0f);
            StepCrowdRange(m, p, 0, 1);
            check(std::fabs(m.posZ[0] - (p.boundsMinZ + 0.4f)) < 1e-5f, "reflect minZ position");
            check(m.velZ[0] == 1.0f, "reflect minZ velocity");
            if (!error.empty()) {
                return error;
            }
        }

        // e. Anim wrap
        {
            CrowdSimParams p;
            p.deltaTime = 0.25f;
            p.clipCount = 1;
            p.clipDurations[0] = 1.0f;
            CrowdStore s = MakeSingleAgent(0.0f, 0.0f, 0.0f, 0.0f, 0.9f, 1.0f);
            StepCrowdRange(s, p, 0, 1);
            check(std::fabs(s.animTime[0] - 0.15f) < 1e-6f, "anim wrap result");
            if (!error.empty()) {
                return error;
            }
        }

        return "OK checks=" + std::to_string(checks);
    }
}
