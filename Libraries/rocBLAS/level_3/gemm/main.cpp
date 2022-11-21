// MIT License
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "cmdparser.hpp"
#include "example_utils.hpp"
#include "rocblas_utils.hpp"

#include <rocblas/rocblas.h>

#include <hip/hip_runtime.h>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <vector>

int main(const int argc, const char** argv)
{
    // Parse user inputs.
    cli::Parser parser(argc, argv);
    parser.set_optional<float>("a", "alpha", 1.f, "Alpha scalar");
    parser.set_optional<float>("b", "beta", 1.f, "Beta scalar");
    parser.set_optional<int>("m", "m", 5, "Number of rows of matrices f(A) and C");
    parser.set_optional<int>("n", "n", 5, "Number of columns of matrices f(B) and C");
    parser.set_optional<int>("k", "k", 5, "Number of columns of matrix f(A) and rows of f(B)");
    parser.run_and_exit_if_error();

    // Set sizes of matrices.
    const rocblas_int m = parser.get<int>("m");
    const rocblas_int n = parser.get<int>("n");
    const rocblas_int k = parser.get<int>("k");

    // Check input values validity.
    if(m <= 0)
    {
        std::cout << "Value of 'm' should be greater than 0" << std::endl;
        return 0;
    }

    if(n <= 0)
    {
        std::cout << "Value of 'n' should be greater than 0" << std::endl;
        return 0;
    }

    if(k <= 0)
    {
        std::cout << "Value of 'k' should be greater than 0" << std::endl;
        return 0;
    }

    // Set scalar values used for multiplication.
    const rocblas_float h_alpha = parser.get<float>("a");
    const rocblas_float h_beta  = parser.get<float>("b");

    // Set GEMM operation as identity operation: $f(X) = X$
    const rocblas_operation trans_a = rocblas_operation_none;
    const rocblas_operation trans_b = rocblas_operation_none;

    rocblas_int lda, ldb, ldc, size_a, size_b, size_c;
    int         stride1_a, stride2_a, stride1_b, stride2_b;

    // Set up matrix dimension variables
    if(trans_a == rocblas_operation_none)
    {
        lda       = m;
        size_a    = k * lda;
        stride1_a = 1;
        stride2_a = lda;
    }
    else
    {
        lda       = k;
        size_a    = m * lda;
        stride1_a = lda;
        stride2_a = 1;
    }
    if(trans_b == rocblas_operation_none)
    {
        ldb       = k;
        size_b    = n * ldb;
        stride1_b = 1;
        stride2_b = ldb;
    }
    else
    {
        ldb       = n;
        size_b    = k * ldb;
        stride1_b = ldb;
        stride2_b = 1;
    }
    ldc    = m;
    size_c = n * ldc;

    // Allocate host data.
    std::vector<float> h_a(size_a, 1);
    std::vector<float> h_b(size_b);
    std::vector<float> h_c(size_c, 1);
    std::vector<float> h_gold(size_c);

    // Set B matrix to an identity matrix.
    generate_identity_matrix(h_b.data(), k, n, ldb);

    // Initialize gold standard matrix.
    h_gold = h_c;

    // Calculate gold standard on CPU.
    multiply_matrices<float>(h_alpha,
                             h_beta,
                             m,
                             n,
                             k,
                             h_a.data(),
                             stride1_a,
                             stride2_a,
                             h_b.data(),
                             stride1_b,
                             stride2_b,
                             h_gold.data(),
                             ldc);

    // Allocate device memory.
    float* d_a{};
    float* d_b{};
    float* d_c{};
    HIP_CHECK(hipMalloc(&d_a, size_a * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_b, size_b * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_c, size_c * sizeof(float)));

    // Copy data from CPU to device.
    HIP_CHECK(hipMemcpy(d_a,
                        static_cast<void*>(h_a.data()),
                        sizeof(float) * size_a,
                        hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_b,
                        static_cast<void*>(h_b.data()),
                        sizeof(float) * size_b,
                        hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_c,
                        static_cast<void*>(h_c.data()),
                        sizeof(float) * size_c,
                        hipMemcpyHostToDevice));

    // Use rocBLAS API.
    rocblas_handle handle;
    ROCBLAS_CHECK(rocblas_create_handle(&handle));

    // Enable passing alpha and beta parameters from pointer to host memory.
    ROCBLAS_CHECK(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));

    // Asynchronous matrix multiplication calculation on device.
    ROCBLAS_CHECK(rocblas_sgemm(handle,
                                trans_a,
                                trans_b,
                                m,
                                n,
                                k,
                                &h_alpha,
                                d_a,
                                lda,
                                d_b,
                                ldb,
                                &h_beta,
                                d_c,
                                ldc));

    // Fetch device memory results, automatically blocked until results ready
    HIP_CHECK(hipMemcpy(h_c.data(), d_c, sizeof(float) * size_c, hipMemcpyDeviceToHost));

    // Destroy the rocBLAS handle.
    ROCBLAS_CHECK(rocblas_destroy_handle(handle));

    // Free device memory as it is no longer required.
    HIP_CHECK(hipFree(d_a));
    HIP_CHECK(hipFree(d_b));
    HIP_CHECK(hipFree(d_c));

    // Check the relative error between output generated by the rocBLAS API and the CPU.
    const float  eps    = 10.f * std::numeric_limits<float>::epsilon();
    unsigned int errors = 0;
    for(rocblas_int i = 0; i < ldc; ++i)
    {
        errors += std::fabs(h_c[i] - h_gold[i]) > eps;
    }
    return report_validation_result(errors);
}
