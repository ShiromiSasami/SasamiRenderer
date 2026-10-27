#pragma once

namespace SasamiEngine::Foundation
{
    // Eigen decomposition of a symmetric 3x3 matrix A = R diag(values) R^T by cyclic Jacobi rotations
    // (Golub & Van Loan, "Matrix Computations", 4th ed., sec. 8.5). a holds the upper triangle
    // { a00, a01, a02, a11, a12, a22 }. values come out sorted descending, and column k of R
    // (R[row][k]) is the unit eigenvector of values[k]; R is orthonormal. Deterministic.
    void SymmetricEigen3(const double a[6], double values[3], double R[3][3]);
}
