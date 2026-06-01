// ModelLoader_OBJ.cpp
// Wavefront OBJ file loader.
// Extracted from ModelLoader.cpp to separate from glTF2 loader.
#include "ModelLoader.h"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "Foundation/Math/MathUtil.h"

namespace SasamiRenderer
{
    using Math::Mul4x4;

    static float Clamp01(float v)
    {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    static float DefaultReflectionStrength(float roughness, float metallic)
    {
        return Clamp01(Clamp01(metallic) * (1.0f - Clamp01(roughness)));
    }

	static inline std::string Trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) { return ""; }
		size_t b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	bool LoadOBJ(const std::string& path, std::vector<Vertex>& outVertices)
	{
		outVertices.clear();

		std::filesystem::path p(path);
		std::ifstream ifs(p);
		if (!ifs.is_open()) { return false; }

		std::vector<std::array<float, 3>> positions;
		std::vector<std::array<float, 2>> texcoords;
		struct Face { std::vector<std::string> tokens; };
		std::vector<Face> faces;

		std::string line;
		while (std::getline(ifs, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') { continue; }

			std::stringstream ss(line);
			std::string head;
			ss >> head;

			if (head == "v") {
				float x = 0, y = 0, z = 0;
				ss >> x >> y >> z;
				positions.push_back({ x, y, z });
			}
			else if (head == "vt") {
				float u = 0, v = 0;
				ss >> u >> v;
				texcoords.push_back({ u, v });
			}
			else if (head == "f") {
				Face f;
				std::string tok;
				while (ss >> tok) { f.tokens.push_back(tok); }
				if (f.tokens.size() >= 3) { faces.push_back(std::move(f)); }
			}
		}

		ifs.close();

		if (positions.empty() || faces.empty()) { return false; }

		auto parseTok = [&](const std::string& vtkn, int& vi, int& ti){
			vi = 0; ti = 0; std::string a,b,c; size_t slash = vtkn.find('/');
			if (slash == std::string::npos) { a = vtkn; }
			else {
				a = vtkn.substr(0, slash);
				size_t slash2 = vtkn.find('/', slash + 1);
				if (slash2 == std::string::npos) { b = vtkn.substr(slash + 1); }
				else { b = vtkn.substr(slash + 1, slash2 - slash - 1); c = vtkn.substr(slash2 + 1); }
			}
			if (!a.empty()) { vi = std::stoi(a); }
			if (!b.empty()) { ti = std::stoi(b); }
		};

		auto getPos = [&](int idx, float out[3]){
			if (idx < 0) idx = static_cast<int>(positions.size()) + idx + 1;
			if (idx > 0 && idx <= (int)positions.size()) {
				auto& p = positions[idx - 1];
				out[0] = p[0];
				out[1] = p[1];
				out[2] = p[2];
			}
		};

		for (auto& f : faces) {
				auto emitTri = [&](const std::string& a, const std::string& b, const std::string& c){
					int ia, ib, ic, ita, itb, itc; parseTok(a, ia, ita); parseTok(b, ib, itb); parseTok(c, ic, itc);
					float p0[3]{}, p1[3]{}, p2[3]{}; getPos(ia, p0); getPos(ib, p1); getPos(ic, p2);
					// Face normal from cross product:
					// n = normalize((p1 - p0) x (p2 - p0))
					float e1[3]{ p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
					float e2[3]{ p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
					float n[3]{ e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
				float len = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (len>0){ n[0]/=len; n[1]/=len; n[2]/=len; }
				auto pushV = [&](int vi, int ti, const float pos[3]){
					Vertex v{}; v.position[0]=pos[0]; v.position[1]=pos[1]; v.position[2]=pos[2]; v.normal[0]=n[0]; v.normal[1]=n[1]; v.normal[2]=n[2];
					v.color[0]=1; v.color[1]=1; v.color[2]=1; v.color[3]=1;
					if (ti < 0) ti = static_cast<int>(texcoords.size()) + ti + 1;
					if (ti > 0 && ti <= (int)texcoords.size()) { v.uv[0]=texcoords[ti-1][0]; v.uv[1]=texcoords[ti-1][1]; } else { v.uv[0]=0; v.uv[1]=0; }
					outVertices.push_back(v);
				};
				pushV(ia, ita, p0); pushV(ib, itb, p1); pushV(ic, itc, p2);
			};

			if (f.tokens.size() == 3) {
				emitTri(f.tokens[0], f.tokens[1], f.tokens[2]);
			}
			else {
				for (size_t i = 2; i < f.tokens.size(); ++i) {
					emitTri(f.tokens[0], f.tokens[i - 1], f.tokens[i]);
				}
			}
		}

		return !outVertices.empty();
	}

} // namespace SasamiRenderer
