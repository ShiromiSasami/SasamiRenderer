#include "Renderer/ParticleCharacter/ParticleCharacterSelfTest.h"

#include "Renderer/ParticleCharacter/MolecularBody.h"
#include "Renderer/ParticleCharacter/SurfaceSampleSet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::Render::ParticleCharacter
{
    namespace
    {
        SkinnedVertex MakeVertex(float x, float y)
        {
            SkinnedVertex v = {};
            v.position[0] = x;
            v.position[1] = y;
            v.position[2] = 0.0f;
            v.normal[2] = 2.0f;
            v.uv[0] = x;
            v.uv[1] = y;
            v.boneWeights[0] = 1.0f;
            return v;
        }

        SkinnedMesh MakeMeshA(bool indexed)
        {
            SkinnedMesh m;
            m.vertices = {
                MakeVertex(0, 0), MakeVertex(1, 0), MakeVertex(0, 1),
                MakeVertex(2, 0), MakeVertex(5, 0), MakeVertex(2, 1),
            };
            if (indexed) {
                m.indices = { 0, 1, 2, 3, 4, 5 };
            }
            return m;
        }

        bool SameSample(const SurfaceSample& a, const SurfaceSample& b)
        {
            return std::memcmp(a.vertex, b.vertex, sizeof(a.vertex)) == 0
                && a.b1 == b.b1 && a.b2 == b.b2 && a.uv[0] == b.uv[0] && a.uv[1] == b.uv[1] && a.mesh == b.mesh;
        }

        bool SameSamples(const std::vector<SurfaceSample>& a, const std::vector<SurfaceSample>& b)
        {
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (!SameSample(a[i], b[i])) {
                    return false;
                }
            }
            return true;
        }

        template <typename T>
        bool SameVector(const std::vector<T>& a, const std::vector<T>& b)
        {
            return a.size() == b.size()
                && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
        }

        void MeshAPositions(const SkinnedMesh& mesh, uint32_t stride, std::vector<float>& pos, std::vector<float>& nrm)
        {
            const size_t n = mesh.vertices.size();
            pos.assign(n * stride, 0.0f);
            nrm.assign(n * stride, 0.0f);
            for (size_t i = 0; i < n; ++i) {
                for (uint32_t k = 0; k < 3; ++k) {
                    pos[i * stride + k] = mesh.vertices[i].position[k];
                    nrm[i * stride + k] = mesh.vertices[i].normal[k];
                }
            }
        }

        bool HasNaN(const std::vector<float>& v)
        {
            for (float f : v) {
                if (!std::isfinite(f)) {
                    return true;
                }
            }
            return false;
        }

        float MeanY(const std::vector<float>& xyz)
        {
            const size_t n = xyz.size() / 3;
            double sum = 0.0;
            for (size_t i = 0; i < n; ++i) {
                sum += xyz[i * 3 + 1];
            }
            return n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
        }

        float MinY(const std::vector<float>& xyz)
        {
            float m = 1e30f;
            for (size_t i = 1; i < xyz.size(); i += 3) {
                m = (std::min)(m, xyz[i]);
            }
            return m;
        }

        float MaxBond(const std::vector<float>& bonds)
        {
            float m = 0.0f;
            for (float b : bonds) {
                m = (std::max)(m, b);
            }
            return m;
        }

        float MinBond(const std::vector<float>& bonds)
        {
            float m = 1e30f;
            for (float b : bonds) {
                m = (std::min)(m, b);
            }
            return m;
        }

        // Interpolated points of mesh A, lifted by +1 in y.
        struct Targets
        {
            std::vector<float> points;
            std::vector<float> normals;
            uint32_t count = 0;
        };

        bool MakeTargets(const SkinnedMesh& mesh, uint32_t count, uint32_t seed, Targets& out)
        {
            SurfaceSampleSet set;
            std::string err;
            if (!SampleSkinnedSurface({ mesh }, count, seed, set, err)) {
                return false;
            }
            std::vector<float> pos, nrm;
            MeshAPositions(mesh, 3, pos, nrm);
            out.count = count;
            out.points.assign(static_cast<size_t>(count) * 3, 0.0f);
            InterpolateSamples(set, pos.data(), nrm.data(), 3, out.points.data(), nullptr);
            for (uint32_t i = 0; i < count; ++i) {
                out.points[i * 3 + 1] += 1.0f;
            }
            out.normals.assign(static_cast<size_t>(count) * 3, 0.0f);
            for (uint32_t i = 0; i < count; ++i) {
                out.normals[i * 3 + 2] = 1.0f;
            }
            return true;
        }
    }

    std::string RunParticleCharacterSelfTest()
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

        const SkinnedMesh meshA = MakeMeshA(true);
        float tri1Fraction = 0.0f;

        // 1. Sampling
        {
            SurfaceSampleSet a, b, c;
            std::string err;
            const bool okA = SampleSkinnedSurface({ meshA }, 20000, 7, a, err);
            if (!check(okA, "sample.ok")) {
                return error + " " + err;
            }
            check(a.samples.size() == 20000, "sample.count");
            check(a.vertexCount == 6, "sample.vertexCount");
            check(a.triangleCount == 2, "sample.triangleCount");
            check(std::fabs(a.totalArea - 2.0) < 1e-9, "sample.totalArea");

            bool baryOk = true;
            size_t inTri1 = 0;
            for (const SurfaceSample& s : a.samples) {
                if (!(s.b1 >= 0.0f && s.b2 >= 0.0f && s.b1 + s.b2 <= 1.0f + 1e-6f)) {
                    baryOk = false;
                }
                if (s.vertex[0] >= 3 && s.vertex[0] <= 5) {
                    ++inTri1;
                }
            }
            check(baryOk, "sample.barycentric");
            tri1Fraction = static_cast<float>(inTri1) / static_cast<float>(a.samples.size());
            check(std::fabs(tri1Fraction - 0.75f) <= 0.02f, "sample.areaWeight");

            SampleSkinnedSurface({ meshA }, 20000, 7, b, err);
            check(SameSamples(a.samples, b.samples), "sample.deterministic");
            SampleSkinnedSurface({ meshA }, 20000, 8, c, err);
            check(!SameSamples(a.samples, c.samples), "sample.seedDiffers");
        }

        // 2. Interpolation
        {
            SurfaceSampleSet set;
            std::string err;
            SampleSkinnedSurface({ meshA }, 5000, 11, set, err);
            std::vector<float> pos3, nrm3, pos4, nrm4;
            MeshAPositions(meshA, 3, pos3, nrm3);
            MeshAPositions(meshA, 4, pos4, nrm4);
            const size_t n = set.samples.size();
            std::vector<float> out3(n * 3), outN3(n * 3), out4(n * 3), outN4(n * 3);
            InterpolateSamples(set, pos3.data(), nrm3.data(), 3, out3.data(), outN3.data());
            InterpolateSamples(set, pos4.data(), nrm4.data(), 4, out4.data(), outN4.data());

            bool planeOk = true;
            bool boxOk = true;
            bool normalOk = true;
            const float eps = 1e-5f;
            for (size_t i = 0; i < n; ++i) {
                const SurfaceSample& s = set.samples[i];
                const float x = out3[i * 3 + 0];
                const float y = out3[i * 3 + 1];
                const float z = out3[i * 3 + 2];
                if (z != 0.0f) {
                    planeOk = false;
                }
                float lo[2] = { 1e30f, 1e30f };
                float hi[2] = { -1e30f, -1e30f };
                for (int k = 0; k < 3; ++k) {
                    const SkinnedVertex& v = meshA.vertices[s.vertex[k]];
                    for (int d = 0; d < 2; ++d) {
                        lo[d] = (std::min)(lo[d], v.position[d]);
                        hi[d] = (std::max)(hi[d], v.position[d]);
                    }
                }
                if (x < lo[0] - eps || x > hi[0] + eps || y < lo[1] - eps || y > hi[1] + eps) {
                    boxOk = false;
                }
                if (std::fabs(outN3[i * 3 + 0]) > 1e-6f || std::fabs(outN3[i * 3 + 1]) > 1e-6f
                    || std::fabs(outN3[i * 3 + 2] - 1.0f) > 1e-6f) {
                    normalOk = false;
                }
            }
            check(planeOk, "interp.plane");
            check(boxOk, "interp.boundingBox");
            check(normalOk, "interp.normal");
            check(SameVector(out3, out4) && SameVector(outN3, outN4), "interp.stride4Identical");
        }

        // 3. Non-indexed and failure cases
        {
            const SkinnedMesh flat = MakeMeshA(false);
            SurfaceSampleSet set;
            std::string err;
            const bool ok = SampleSkinnedSurface({ flat }, 100, 3, set, err);
            check(ok, "flat.ok");
            check(set.triangleCount == 2, "flat.triangleCount");
            check(std::fabs(set.totalArea - 2.0) < 1e-9, "flat.totalArea");

            SurfaceSampleSet zero;
            std::string zeroErr;
            check(!SampleSkinnedSurface({ meshA }, 0, 3, zero, zeroErr) && !zeroErr.empty(), "fail.zeroCount");

            SkinnedMesh degenerate;
            degenerate.vertices = { MakeVertex(0, 0), MakeVertex(1, 1), MakeVertex(2, 2) };
            degenerate.indices = { 0, 1, 2 };
            SurfaceSampleSet deg;
            std::string degErr;
            check(!SampleSkinnedSurface({ degenerate }, 10, 3, deg, degErr) && !degErr.empty(), "fail.zeroArea");
        }

        // 4-6. MolecularBody
        float releaseMaxDist = 0.0f;
        float reformMaxDist = 0.0f;
        float meltMeanY = 0.0f;
        {
            Targets t;
            if (!check(MakeTargets(meshA, 2000, 5, t), "body.targets")) {
                return error;
            }
            const float dt = 1.0f / 60.0f;
            const uint32_t n = t.count;

            // 4. Hold
            {
                MolecularBody body;
                body.Reset(t.points.data(), n, 3);
                MolecularBodySettings s;
                for (int i = 0; i < 120; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, false);
                }
                check(body.MaxTargetDistance(t.points.data()) < 1e-6f, "hold.maxDist");
            }

            // 5. Release and re-form
            {
                MolecularBody body;
                body.Reset(t.points.data(), n, 3);
                MolecularBodySettings s;
                s.force = MolecularForce::Scatter;
                body.SetBondGoal(0.0f);
                bool floorOk = true;
                for (int i = 0; i < 90; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, false);
                    if (MinY(body.Positions()) < s.floorY) {
                        floorOk = false;
                    }
                }
                releaseMaxDist = body.MaxTargetDistance(t.points.data());
                check(releaseMaxDist > 0.1f, "release.flewAway");
                check(MaxBond(body.Bonds()) < 0.01f, "release.bonds");

                body.SetBondGoal(1.0f);
                for (int i = 0; i < 240; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, false);
                    if (MinY(body.Positions()) < s.floorY) {
                        floorOk = false;
                    }
                }
                check(floorOk, "release.floor");
                check(MinBond(body.Bonds()) == 1.0f, "reform.bonds");
                reformMaxDist = body.MaxTargetDistance(t.points.data());
                check(reformMaxDist < 1e-3f, "reform.maxDist");
            }

            // 6. Swirl and Melt
            {
                MolecularBody body;
                body.Reset(t.points.data(), n, 3);
                MolecularBodySettings s;
                s.force = MolecularForce::Swirl;
                body.SetBondGoal(0.0f);
                for (int i = 0; i < 60; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, false);
                }
                check(!HasNaN(body.Positions()) && !HasNaN(body.Velocities()), "swirl.finite");
            }
            {
                MolecularBody body;
                body.Reset(t.points.data(), n, 3);
                MolecularBodySettings s;
                s.force = MolecularForce::Melt;
                body.SetBondGoal(0.0f);
                for (int i = 0; i < 180; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, false);
                }
                meltMeanY = MeanY(body.Positions());
                const float targetMeanY = MeanY(t.points);
                check(!HasNaN(body.Positions()), "melt.finite");
                check(meltMeanY < 0.5f * targetMeanY, "melt.meanY");
                check(MinY(body.Positions()) >= s.floorY, "melt.floor");
            }
        }

        // 7. Serial vs parallel
        {
            Targets t;
            if (!check(MakeTargets(meshA, 10000, 9, t), "par.targets")) {
                return error;
            }
            const float dt = 1.0f / 60.0f;
            MolecularBody serial, parallel;
            serial.Reset(t.points.data(), t.count, 3);
            parallel.Reset(t.points.data(), t.count, 3);
            MolecularBodySettings s;
            s.force = MolecularForce::Scatter;
            auto run = [&](MolecularBody& body, bool par) {
                body.SetBondGoal(0.0f);
                for (int i = 0; i < 60; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, par);
                }
                body.SetBondGoal(1.0f);
                for (int i = 0; i < 60; ++i) {
                    body.Step(t.points.data(), t.normals.data(), dt, s, par);
                }
            };
            run(serial, false);
            run(parallel, true);
            check(SameVector(serial.Positions(), parallel.Positions()), "par.positions");
            check(SameVector(serial.Velocities(), parallel.Velocities()), "par.velocities");
            check(SameVector(serial.Bonds(), parallel.Bonds()), "par.bonds");
            check(serial.FloorHits() == parallel.FloorHits(), "par.floorHits");
        }

        if (!error.empty()) {
            return error;
        }
        std::ostringstream o;
        o << "OK checks=" << checks << " tri1Fraction=" << tri1Fraction << " releaseMaxDist=" << releaseMaxDist
          << " reformMaxDist=" << reformMaxDist << " meltMeanY=" << meltMeanY;
        return o.str();
    }
}
