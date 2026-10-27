#include "Foundation/Math/SymmetricEigen3.h"

#include <cmath>
#include <utility>

namespace SasamiEngine::Foundation
{
    void SymmetricEigen3(const double a[6], double values[3], double R[3][3])
    {
        double m[3][3] = {
            { a[0], a[1], a[2] },
            { a[1], a[3], a[4] },
            { a[2], a[4], a[5] },
        };
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                R[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }

        constexpr int MAX_SWEEPS = 32;
        for (int sweep = 0; sweep < MAX_SWEEPS; ++sweep) {
            const double off = m[0][1] * m[0][1] + m[0][2] * m[0][2] + m[1][2] * m[1][2];
            const double diag = m[0][0] * m[0][0] + m[1][1] * m[1][1] + m[2][2] * m[2][2];
            if (off <= 1e-30 * diag || off == 0.0) {
                break;
            }
            for (int p = 0; p < 2; ++p) {
                for (int q = p + 1; q < 3; ++q) {
                    if (m[p][q] == 0.0) {
                        continue;
                    }
                    // Symmetric Schur 2x2 (Golub & Van Loan Alg. 8.5.1).
                    const double tau = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
                    const double t = (tau >= 0.0 ? 1.0 : -1.0) / (std::fabs(tau) + std::sqrt(1.0 + tau * tau));
                    const double c = 1.0 / std::sqrt(1.0 + t * t);
                    const double s = t * c;
                    // m = J^T m J with J the (p, q) rotation.
                    for (int k = 0; k < 3; ++k) {
                        const double mkp = m[k][p];
                        const double mkq = m[k][q];
                        m[k][p] = c * mkp - s * mkq;
                        m[k][q] = s * mkp + c * mkq;
                    }
                    for (int k = 0; k < 3; ++k) {
                        const double mpk = m[p][k];
                        const double mqk = m[q][k];
                        m[p][k] = c * mpk - s * mqk;
                        m[q][k] = s * mpk + c * mqk;
                    }
                    for (int k = 0; k < 3; ++k) {
                        const double rkp = R[k][p];
                        const double rkq = R[k][q];
                        R[k][p] = c * rkp - s * rkq;
                        R[k][q] = s * rkp + c * rkq;
                    }
                }
            }
        }

        for (int k = 0; k < 3; ++k) {
            values[k] = m[k][k];
        }
        // Sort descending, swapping the matching columns of R.
        for (int i = 0; i < 2; ++i) {
            int best = i;
            for (int j = i + 1; j < 3; ++j) {
                if (values[j] > values[best]) {
                    best = j;
                }
            }
            if (best != i) {
                std::swap(values[i], values[best]);
                for (int k = 0; k < 3; ++k) {
                    std::swap(R[k][i], R[k][best]);
                }
            }
        }
    }
}
