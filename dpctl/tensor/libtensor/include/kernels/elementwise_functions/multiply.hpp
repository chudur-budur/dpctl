//=== multiply.hpp -   Binary function MUL               ------  *-C++-*--/===//
//
//                      Data Parallel Control (dpctl)
//
// Copyright 2020-2023 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===---------------------------------------------------------------------===//
///
/// \file
/// This file defines kernels for elementwise evaluation of MUL(x1, x2)
/// function.
//===---------------------------------------------------------------------===//

#pragma once
#include <CL/sycl.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "utils/offset_utils.hpp"
#include "utils/type_dispatch.hpp"
#include "utils/type_utils.hpp"

#include "kernels/elementwise_functions/common.hpp"
#include <pybind11/pybind11.h>

namespace dpctl
{
namespace tensor
{
namespace kernels
{
namespace multiply
{

namespace py = pybind11;
namespace td_ns = dpctl::tensor::type_dispatch;
namespace tu_ns = dpctl::tensor::type_utils;

template <typename argT1, typename argT2, typename resT> struct MultiplyFunctor
{

    using supports_sg_loadstore = std::negation<
        std::disjunction<tu_ns::is_complex<argT1>, tu_ns::is_complex<argT2>>>;
    using supports_vec = std::negation<
        std::disjunction<tu_ns::is_complex<argT1>, tu_ns::is_complex<argT2>>>;

    resT operator()(const argT1 &in1, const argT2 &in2)
    {
        return in1 * in2;
    }

    template <int vec_sz>
    sycl::vec<resT, vec_sz> operator()(const sycl::vec<argT1, vec_sz> &in1,
                                       const sycl::vec<argT2, vec_sz> &in2)
    {
        auto tmp = in1 * in2;
        if constexpr (std::is_same_v<resT,
                                     typename decltype(tmp)::element_type>) {
            return tmp;
        }
        else {
            using dpctl::tensor::type_utils::vec_cast;

            return vec_cast<resT, typename decltype(tmp)::element_type, vec_sz>(
                tmp);
        }
    }
};

template <typename argT1,
          typename argT2,
          typename resT,
          unsigned int vec_sz = 4,
          unsigned int n_vecs = 2>
using MultiplyContigFunctor =
    elementwise_common::BinaryContigFunctor<argT1,
                                            argT2,
                                            resT,
                                            MultiplyFunctor<argT1, argT2, resT>,
                                            vec_sz,
                                            n_vecs>;

template <typename argT1, typename argT2, typename resT, typename IndexerT>
using MultiplyStridedFunctor = elementwise_common::BinaryStridedFunctor<
    argT1,
    argT2,
    resT,
    IndexerT,
    MultiplyFunctor<argT1, argT2, resT>>;

template <typename T1, typename T2> struct MultiplyOutputType
{
    using value_type = typename std::disjunction< // disjunction is C++17
                                                  // feature, supported by DPC++
        td_ns::BinaryTypeMapResultEntry<T1, bool, T2, bool, bool>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::uint8_t,
                                        T2,
                                        std::uint8_t,
                                        std::uint8_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::int8_t,
                                        T2,
                                        std::int8_t,
                                        std::int8_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::uint16_t,
                                        T2,
                                        std::uint16_t,
                                        std::uint16_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::int16_t,
                                        T2,
                                        std::int16_t,
                                        std::int16_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::uint32_t,
                                        T2,
                                        std::uint32_t,
                                        std::uint32_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::int32_t,
                                        T2,
                                        std::int32_t,
                                        std::int32_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::uint64_t,
                                        T2,
                                        std::uint64_t,
                                        std::uint64_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::int64_t,
                                        T2,
                                        std::int64_t,
                                        std::int64_t>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        sycl::half,
                                        T2,
                                        sycl::half,
                                        sycl::half>,
        td_ns::BinaryTypeMapResultEntry<T1, float, T2, float, float>,
        td_ns::BinaryTypeMapResultEntry<T1, double, T2, double, double>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::complex<float>,
                                        T2,
                                        std::complex<float>,
                                        std::complex<float>>,
        td_ns::BinaryTypeMapResultEntry<T1,
                                        std::complex<double>,
                                        T2,
                                        std::complex<double>,
                                        std::complex<double>>,
        td_ns::DefaultResultEntry<void>>::result_type;
};

template <typename argT1,
          typename argT2,
          typename resT,
          unsigned int vec_sz,
          unsigned int n_vecs>
class multiply_contig_kernel;

template <typename argTy1, typename argTy2>
sycl::event multiply_contig_impl(sycl::queue exec_q,
                                 size_t nelems,
                                 const char *arg1_p,
                                 py::ssize_t arg1_offset,
                                 const char *arg2_p,
                                 py::ssize_t arg2_offset,
                                 char *res_p,
                                 py::ssize_t res_offset,
                                 const std::vector<sycl::event> &depends = {})
{
    sycl::event comp_ev = exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(depends);

        size_t lws = 64;
        constexpr unsigned int vec_sz = 4;
        constexpr unsigned int n_vecs = 2;
        const size_t n_groups =
            ((nelems + lws * n_vecs * vec_sz - 1) / (lws * n_vecs * vec_sz));
        const auto gws_range = sycl::range<1>(n_groups * lws);
        const auto lws_range = sycl::range<1>(lws);

        using resTy = typename MultiplyOutputType<argTy1, argTy2>::value_type;

        const argTy1 *arg1_tp =
            reinterpret_cast<const argTy1 *>(arg1_p) + arg1_offset;
        const argTy2 *arg2_tp =
            reinterpret_cast<const argTy2 *>(arg2_p) + arg2_offset;
        resTy *res_tp = reinterpret_cast<resTy *>(res_p) + res_offset;

        cgh.parallel_for<
            multiply_contig_kernel<argTy1, argTy2, resTy, vec_sz, n_vecs>>(
            sycl::nd_range<1>(gws_range, lws_range),
            MultiplyContigFunctor<argTy1, argTy2, resTy, vec_sz, n_vecs>(
                arg1_tp, arg2_tp, res_tp, nelems));
    });
    return comp_ev;
}

template <typename fnT, typename T1, typename T2> struct MultiplyContigFactory
{
    fnT get()
    {
        if constexpr (std::is_same_v<
                          typename MultiplyOutputType<T1, T2>::value_type,
                          void>)
        {
            fnT fn = nullptr;
            return fn;
        }
        else {
            fnT fn = multiply_contig_impl<T1, T2>;
            return fn;
        }
    }
};

template <typename fnT, typename T1, typename T2> struct MultiplyTypeMapFactory
{
    /*! @brief get typeid for output type of multiply(T1 x, T2 y) */
    std::enable_if_t<std::is_same<fnT, int>::value, int> get()
    {
        using rT = typename MultiplyOutputType<T1, T2>::value_type;
        ;
        return td_ns::GetTypeid<rT>{}.get();
    }
};

template <typename T1, typename T2, typename resT, typename IndexerT>
class multiply_strided_strided_kernel;

template <typename argTy1, typename argTy2>
sycl::event
multiply_strided_impl(sycl::queue exec_q,
                      size_t nelems,
                      int nd,
                      const py::ssize_t *shape_and_strides,
                      const char *arg1_p,
                      py::ssize_t arg1_offset,
                      const char *arg2_p,
                      py::ssize_t arg2_offset,
                      char *res_p,
                      py::ssize_t res_offset,
                      const std::vector<sycl::event> &depends,
                      const std::vector<sycl::event> &additional_depends)
{
    sycl::event comp_ev = exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(depends);
        cgh.depends_on(additional_depends);

        using resTy = typename MultiplyOutputType<argTy1, argTy2>::value_type;

        using IndexerT =
            typename dpctl::tensor::offset_utils::ThreeOffsets_StridedIndexer;

        IndexerT indexer{nd, arg1_offset, arg2_offset, res_offset,
                         shape_and_strides};

        const argTy1 *arg1_tp = reinterpret_cast<const argTy1 *>(arg1_p);
        const argTy2 *arg2_tp = reinterpret_cast<const argTy2 *>(arg2_p);
        resTy *res_tp = reinterpret_cast<resTy *>(res_p);

        cgh.parallel_for<
            multiply_strided_strided_kernel<argTy1, argTy2, resTy, IndexerT>>(
            {nelems}, MultiplyStridedFunctor<argTy1, argTy2, resTy, IndexerT>(
                          arg1_tp, arg2_tp, res_tp, indexer));
    });
    return comp_ev;
}

template <typename fnT, typename T1, typename T2> struct MultiplyStridedFactory
{
    fnT get()
    {
        if constexpr (std::is_same_v<
                          typename MultiplyOutputType<T1, T2>::value_type,
                          void>)
        {
            fnT fn = nullptr;
            return fn;
        }
        else {
            fnT fn = multiply_strided_impl<T1, T2>;
            return fn;
        }
    }
};

template <typename argT1, typename argT2, typename resT>
class multiply_matrix_row_broadcast_sg_krn;

template <typename argT1, typename argT2, typename resT>
using MultiplyContigMatrixContigRowBroadcastingFunctor =
    elementwise_common::BinaryContigMatrixContigRowBroadcastingFunctor<
        argT1,
        argT2,
        resT,
        MultiplyFunctor<argT1, argT2, resT>>;

template <typename argT1, typename argT2, typename resT>
sycl::event multiply_contig_matrix_contig_row_broadcast_impl(
    sycl::queue exec_q,
    std::vector<sycl::event> &host_tasks,
    size_t n0,
    size_t n1,
    const char *mat_p, // typeless pointer to (n0, n1) C-contiguous matrix
    py::ssize_t mat_offset,
    const char *vec_p, // typeless pointer to (n1,) contiguous row
    py::ssize_t vec_offset,
    char *res_p, // typeless pointer to (n0, n1) result C-contig. matrix,
                 //    res[i,j] = mat[i,j] * vec[j]
    py::ssize_t res_offset,
    const std::vector<sycl::event> &depends = {})
{
    const argT1 *mat = reinterpret_cast<const argT1 *>(mat_p) + mat_offset;
    const argT2 *vec = reinterpret_cast<const argT2 *>(vec_p) + vec_offset;
    resT *res = reinterpret_cast<resT *>(res_p) + res_offset;

    const auto &dev = exec_q.get_device();
    const auto &sg_sizes = dev.get_info<sycl::info::device::sub_group_sizes>();
    // Get device-specific kernel info max_sub_group_size
    size_t max_sgSize =
        *(std::max_element(std::begin(sg_sizes), std::end(sg_sizes)));

    size_t n1_padded = n1 + max_sgSize;
    argT2 *padded_vec = sycl::malloc_device<argT2>(n1_padded, exec_q);

    if (padded_vec == nullptr) {
        throw std::runtime_error("Could not allocate memory on the device");
    }
    sycl::event make_padded_vec_ev = exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(depends); // ensure vec contains actual data
        cgh.parallel_for({n1_padded}, [=](sycl::id<1> id) {
            auto i = id[0];
            padded_vec[i] = vec[i % n1];
        });
    });

    // sub-group spans work-items [I, I + sgSize)
    // base = ndit.get_global_linear_id() - sg.get_local_id()[0]
    // Generically, sg.load( &mat[base]) may load arrays from
    // different rows of mat. The start corresponds to row (base / n0)
    // We read sg.load(&padded_vec[(base / n0)]). The vector is padded to
    // ensure that reads are accessible

    size_t lws = 64;

    sycl::event comp_ev = exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(make_padded_vec_ev);

        auto lwsRange = sycl::range<1>(lws);
        size_t n_elems = n0 * n1;
        size_t n_groups = (n_elems + lws - 1) / lws;
        auto gwsRange = sycl::range<1>(n_groups * lws);

        cgh.parallel_for<
            class multiply_matrix_row_broadcast_sg_krn<argT1, argT2, resT>>(
            sycl::nd_range<1>(gwsRange, lwsRange),
            MultiplyContigMatrixContigRowBroadcastingFunctor<argT1, argT2,
                                                             resT>(
                mat, padded_vec, res, n_elems, n1));
    });

    sycl::event tmp_cleanup_ev = exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(comp_ev);
        sycl::context ctx = exec_q.get_context();
        cgh.host_task([ctx, padded_vec]() { sycl::free(padded_vec, ctx); });
    });
    host_tasks.push_back(tmp_cleanup_ev);

    return comp_ev;
}

template <typename fnT, typename T1, typename T2>
struct MultiplyContigMatrixContigRowBroadcastFactory
{
    fnT get()
    {
        using resT = typename MultiplyOutputType<T1, T2>::value_type;
        if constexpr (std::is_same_v<resT, void>) {
            fnT fn = nullptr;
            return fn;
        }
        else {
            if constexpr (dpctl::tensor::type_utils::is_complex<T1>::value ||
                          dpctl::tensor::type_utils::is_complex<T2>::value ||
                          dpctl::tensor::type_utils::is_complex<resT>::value)
            {
                fnT fn = nullptr;
                return fn;
            }
            else {
                fnT fn =
                    multiply_contig_matrix_contig_row_broadcast_impl<T1, T2,
                                                                     resT>;
                return fn;
            }
        }
    }
};

template <typename argT1, typename argT2, typename resT>
sycl::event multiply_contig_row_contig_matrix_broadcast_impl(
    sycl::queue exec_q,
    std::vector<sycl::event> &host_tasks,
    size_t n0,
    size_t n1,
    const char *vec_p, // typeless pointer to (n1,) contiguous row
    py::ssize_t vec_offset,
    const char *mat_p, // typeless pointer to (n0, n1) C-contiguous matrix
    py::ssize_t mat_offset,
    char *res_p, // typeless pointer to (n0, n1) result C-contig. matrix,
                 //    res[i,j] = mat[i,j] * vec[j]
    py::ssize_t res_offset,
    const std::vector<sycl::event> &depends = {})
{
    return multiply_contig_matrix_contig_row_broadcast_impl<argT2, argT1, resT>(
        exec_q, host_tasks, n0, n1, mat_p, mat_offset, vec_p, vec_offset, res_p,
        res_offset, depends);
};

template <typename fnT, typename T1, typename T2>
struct MultiplyContigRowContigMatrixBroadcastFactory
{
    fnT get()
    {
        using resT = typename MultiplyOutputType<T1, T2>::value_type;
        if constexpr (std::is_same_v<resT, void>) {
            fnT fn = nullptr;
            return fn;
        }
        else {
            if constexpr (dpctl::tensor::type_utils::is_complex<T1>::value ||
                          dpctl::tensor::type_utils::is_complex<T2>::value ||
                          dpctl::tensor::type_utils::is_complex<resT>::value)
            {
                fnT fn = nullptr;
                return fn;
            }
            else {
                fnT fn =
                    multiply_contig_row_contig_matrix_broadcast_impl<T1, T2,
                                                                     resT>;
                return fn;
            }
        }
    }
};

} // namespace multiply
} // namespace kernels
} // namespace tensor
} // namespace dpctl
