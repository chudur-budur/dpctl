//=== boolean_reductions.hpp - Implementation of boolean reduction kernels
//---*-C++-*--/===//
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
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines kernels for dpctl.tensor.any and dpctl.tensor.all
//===----------------------------------------------------------------------===//

#pragma once

#include <CL/sycl.hpp>

#include <complex>
#include <cstdint>
#include <utility>
#include <vector>

#include "pybind11/numpy.h"
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"

#include "utils/offset_utils.hpp"
#include "utils/type_dispatch.hpp"
#include "utils/type_utils.hpp"

namespace py = pybind11;

namespace dpctl
{
namespace tensor
{
namespace kernels
{

template <typename T> struct boolean_predicate
{
    bool operator()(const T &v) const
    {
        using dpctl::tensor::type_utils::convert_impl;
        return convert_impl<bool, T>(v);
    }
};

template <typename inpT, typename outT, typename PredicateT, size_t wg_dim>
struct all_reduce_wg_contig
{
    outT operator()(sycl::group<wg_dim> &wg,
                    const inpT *start,
                    const inpT *end) const
    {
        PredicateT pred{};
        return static_cast<outT>(sycl::joint_all_of(wg, start, end, pred));
    }
};

template <typename inpT, typename outT, typename PredicateT, size_t wg_dim>
struct any_reduce_wg_contig
{
    outT operator()(sycl::group<wg_dim> &wg,
                    const inpT *start,
                    const inpT *end) const
    {
        PredicateT pred{};
        return static_cast<outT>(sycl::joint_any_of(wg, start, end, pred));
    }
};

template <typename T, typename PredicateT, size_t wg_dim>
struct all_reduce_wg_strided
{
    T operator()(sycl::group<wg_dim> &wg, const T &local_val) const
    {
        PredicateT pred{};
        return static_cast<T>(sycl::all_of_group(wg, local_val, pred));
    }
};

template <typename T, typename PredicateT, size_t wg_dim>
struct any_reduce_wg_strided
{
    T operator()(sycl::group<wg_dim> &wg, const T &local_val) const
    {
        PredicateT pred{};
        return static_cast<T>(sycl::any_of_group(wg, local_val, pred));
    }
};

template <typename argT,
          typename outT,
          typename ReductionOp,
          typename InputOutputIterIndexerT,
          typename InputRedIndexerT>
struct SequentialBooleanReduction
{
private:
    const argT *inp_ = nullptr;
    outT *out_ = nullptr;
    ReductionOp reduction_op_;
    outT identity_;
    InputOutputIterIndexerT inp_out_iter_indexer_;
    InputRedIndexerT inp_reduced_dims_indexer_;
    size_t reduction_max_gid_ = 0;

public:
    SequentialBooleanReduction(const argT *inp,
                               outT *res,
                               ReductionOp reduction_op,
                               const outT &identity_val,
                               InputOutputIterIndexerT arg_res_iter_indexer,
                               InputRedIndexerT arg_reduced_dims_indexer,
                               size_t reduction_size)
        : inp_(inp), out_(res), reduction_op_(reduction_op),
          identity_(identity_val), inp_out_iter_indexer_(arg_res_iter_indexer),
          inp_reduced_dims_indexer_(arg_reduced_dims_indexer),
          reduction_max_gid_(reduction_size)
    {
    }

    void operator()(sycl::id<1> id) const
    {

        auto inp_out_iter_offsets_ = inp_out_iter_indexer_(id[0]);
        const auto &inp_iter_offset = inp_out_iter_offsets_.get_first_offset();
        const auto &out_iter_offset = inp_out_iter_offsets_.get_second_offset();

        outT red_val(identity_);
        for (size_t m = 0; m < reduction_max_gid_; ++m) {
            auto inp_reduction_offset = inp_reduced_dims_indexer_(m);
            auto inp_offset = inp_iter_offset + inp_reduction_offset;

            // must convert to boolean first to handle nans
            using dpctl::tensor::type_utils::convert_impl;
            outT val = convert_impl<bool, argT>(inp_[inp_offset]);

            red_val = reduction_op_(red_val, val);
        }

        out_[out_iter_offset] = red_val;
    }
};

template <typename argT, typename outT, typename ReductionOp, typename GroupOp>
struct ContigBooleanReduction
{
private:
    const argT *inp_ = nullptr;
    outT *out_ = nullptr;
    ReductionOp reduction_op_;
    GroupOp group_op_;
    size_t reduction_max_gid_ = 0;
    size_t reductions_per_wi = 16;

public:
    ContigBooleanReduction(const argT *inp,
                           outT *res,
                           ReductionOp reduction_op,
                           GroupOp group_op,
                           size_t reduction_size,
                           size_t reduction_size_per_wi)
        : inp_(inp), out_(res), reduction_op_(reduction_op),
          group_op_(group_op), reduction_max_gid_(reduction_size),
          reductions_per_wi(reduction_size_per_wi)
    {
    }

    void operator()(sycl::nd_item<2> it) const
    {

        size_t reduction_id = it.get_group(0);
        size_t reduction_batch_id = it.get_group(1);

        auto work_group = it.get_group();
        size_t wg_size = it.get_local_range(1);

        size_t base = reduction_id * reduction_max_gid_;
        size_t start = base + reduction_batch_id * wg_size * reductions_per_wi;
        size_t end = std::min((start + (reductions_per_wi * wg_size)),
                              base + reduction_max_gid_);

        // reduction to the work group level is performed
        // inside group_op
        outT red_val_over_wg = group_op_(work_group, inp_ + start, inp_ + end);

        if (work_group.leader()) {
            sycl::atomic_ref<outT, sycl::memory_order::relaxed,
                             sycl::memory_scope::device,
                             sycl::access::address_space::global_space>
                res_ref(out_[reduction_id]);
            outT read_val = res_ref.load();
            outT new_val{};
            do {
                new_val = reduction_op_(read_val, red_val_over_wg);
            } while (!res_ref.compare_exchange_strong(read_val, new_val));
        }
    }
};

typedef sycl::event (*boolean_reduction_contig_impl_fn_ptr)(
    sycl::queue,
    size_t,
    size_t,
    const char *,
    char *,
    py::ssize_t,
    py::ssize_t,
    py::ssize_t,
    const std::vector<sycl::event> &);

template <typename T1, typename T2, typename T3, typename T4>
class boolean_reduction_contig_krn;

template <typename T1, typename T2, typename T3, typename T4, typename T5>
class boolean_reduction_seq_contig_krn;

template <typename argTy, typename resTy, typename RedOpT, typename GroupOpT>
sycl::event
boolean_reduction_contig_impl(sycl::queue exec_q,
                              size_t iter_nelems,
                              size_t reduction_nelems,
                              const char *arg_cp,
                              char *res_cp,
                              py::ssize_t iter_arg_offset,
                              py::ssize_t iter_res_offset,
                              py::ssize_t red_arg_offset,
                              const std::vector<sycl::event> &depends)
{
    const argTy *arg_tp = reinterpret_cast<const argTy *>(arg_cp) +
                          iter_arg_offset + red_arg_offset;
    resTy *res_tp = reinterpret_cast<resTy *>(res_cp) + iter_res_offset;

    constexpr resTy identity_val = sycl::known_identity<RedOpT, resTy>::value;

    const sycl::device &d = exec_q.get_device();
    const auto &sg_sizes = d.get_info<sycl::info::device::sub_group_sizes>();
    size_t wg =
        4 * (*std::max_element(std::begin(sg_sizes), std::end(sg_sizes)));

    sycl::event red_ev;
    if (reduction_nelems < wg) {
        red_ev = exec_q.submit([&](sycl::handler &cgh) {
            cgh.depends_on(depends);

            using InputIterIndexerT =
                dpctl::tensor::offset_utils::Strided1DIndexer;
            using NoOpIndexerT = dpctl::tensor::offset_utils::NoOpIndexer;
            using InputOutputIterIndexerT =
                dpctl::tensor::offset_utils::TwoOffsets_CombinedIndexer<
                    InputIterIndexerT, NoOpIndexerT>;
            using ReductionIndexerT = NoOpIndexerT;

            InputOutputIterIndexerT in_out_iter_indexer{
                InputIterIndexerT{0, static_cast<py::ssize_t>(iter_nelems),
                                  static_cast<py::ssize_t>(reduction_nelems)},
                NoOpIndexerT{}};
            ReductionIndexerT reduction_indexer{};

            cgh.parallel_for<class boolean_reduction_seq_contig_krn<
                argTy, resTy, RedOpT, InputOutputIterIndexerT,
                ReductionIndexerT>>(
                sycl::range<1>(iter_nelems),
                SequentialBooleanReduction<argTy, resTy, RedOpT,
                                           InputOutputIterIndexerT,
                                           ReductionIndexerT>(
                    arg_tp, res_tp, RedOpT(), identity_val, in_out_iter_indexer,
                    reduction_indexer, reduction_nelems));
        });
    }
    else {
        sycl::event init_ev = exec_q.submit([&](sycl::handler &cgh) {
            using IndexerT = dpctl::tensor::offset_utils::NoOpIndexer;

            IndexerT res_indexer{};

            cgh.depends_on(depends);

            cgh.parallel_for(sycl::range<1>(iter_nelems), [=](sycl::id<1> id) {
                auto res_offset = res_indexer(id[0]);
                res_tp[res_offset] = identity_val;
            });
        });
        red_ev = exec_q.submit([&](sycl::handler &cgh) {
            cgh.depends_on(init_ev);

            constexpr size_t group_dim = 2;

            constexpr size_t preferred_reductions_per_wi = 4;
            size_t reductions_per_wi =
                (reduction_nelems < preferred_reductions_per_wi * wg)
                    ? ((reduction_nelems + wg - 1) / wg)
                    : preferred_reductions_per_wi;

            size_t reduction_groups =
                (reduction_nelems + reductions_per_wi * wg - 1) /
                (reductions_per_wi * wg);

            auto gws =
                sycl::range<group_dim>{iter_nelems, reduction_groups * wg};
            auto lws = sycl::range<group_dim>{1, wg};

            cgh.parallel_for<class boolean_reduction_contig_krn<
                argTy, resTy, RedOpT, GroupOpT>>(
                sycl::nd_range<group_dim>(gws, lws),
                ContigBooleanReduction<argTy, resTy, RedOpT, GroupOpT>(
                    arg_tp, res_tp, RedOpT(), GroupOpT(), reduction_nelems,
                    reductions_per_wi));
        });
    }
    return red_ev;
}

template <typename fnT, typename srcTy> struct AllContigFactory
{
    fnT get() const
    {
        using resTy = std::int32_t;
        using RedOpT = sycl::logical_and<resTy>;
        using GroupOpT =
            all_reduce_wg_contig<srcTy, resTy, boolean_predicate<srcTy>, 2>;

        return dpctl::tensor::kernels::boolean_reduction_contig_impl<
            srcTy, resTy, RedOpT, GroupOpT>;
    }
};

template <typename fnT, typename srcTy> struct AnyContigFactory
{
    fnT get() const
    {
        using resTy = std::int32_t;
        using RedOpT = sycl::logical_or<resTy>;
        using GroupOpT =
            any_reduce_wg_contig<srcTy, resTy, boolean_predicate<srcTy>, 2>;

        return dpctl::tensor::kernels::boolean_reduction_contig_impl<
            srcTy, resTy, RedOpT, GroupOpT>;
    }
};

template <typename argT,
          typename outT,
          typename ReductionOp,
          typename GroupOp,
          typename InputOutputIterIndexerT,
          typename InputRedIndexerT>
struct StridedBooleanReduction
{
private:
    const argT *inp_ = nullptr;
    outT *out_ = nullptr;
    ReductionOp reduction_op_;
    GroupOp group_op_;
    outT identity_;
    InputOutputIterIndexerT inp_out_iter_indexer_;
    InputRedIndexerT inp_reduced_dims_indexer_;
    size_t reduction_max_gid_ = 0;
    size_t reductions_per_wi = 16;

public:
    StridedBooleanReduction(const argT *inp,
                            outT *res,
                            ReductionOp reduction_op,
                            GroupOp group_op,
                            const outT &identity_val,
                            InputOutputIterIndexerT arg_res_iter_indexer,
                            InputRedIndexerT arg_reduced_dims_indexer,
                            size_t reduction_size,
                            size_t reduction_size_per_wi)
        : inp_(inp), out_(res), reduction_op_(reduction_op),
          group_op_(group_op), identity_(identity_val),
          inp_out_iter_indexer_(arg_res_iter_indexer),
          inp_reduced_dims_indexer_(arg_reduced_dims_indexer),
          reduction_max_gid_(reduction_size),
          reductions_per_wi(reduction_size_per_wi)
    {
    }

    void operator()(sycl::nd_item<2> it) const
    {

        size_t reduction_id = it.get_group(0);
        size_t reduction_batch_id = it.get_group(1);
        size_t reduction_lid = it.get_local_id(1);
        size_t wg_size = it.get_local_range(1);

        auto inp_out_iter_offsets_ = inp_out_iter_indexer_(reduction_id);
        const auto &inp_iter_offset = inp_out_iter_offsets_.get_first_offset();
        const auto &out_iter_offset = inp_out_iter_offsets_.get_second_offset();

        outT local_red_val(identity_);
        size_t arg_reduce_gid0 =
            reduction_lid + reduction_batch_id * wg_size * reductions_per_wi;
        for (size_t m = 0; m < reductions_per_wi; ++m) {
            size_t arg_reduce_gid = arg_reduce_gid0 + m * wg_size;

            if (arg_reduce_gid < reduction_max_gid_) {
                auto inp_reduction_offset =
                    inp_reduced_dims_indexer_(arg_reduce_gid);
                auto inp_offset = inp_iter_offset + inp_reduction_offset;

                // must convert to boolean first to handle nans
                using dpctl::tensor::type_utils::convert_impl;
                outT val = convert_impl<bool, argT>(inp_[inp_offset]);

                local_red_val = reduction_op_(local_red_val, val);
            }
        }

        // reduction to the work group level is performed
        // inside group_op
        auto work_group = it.get_group();
        outT red_val_over_wg = group_op_(work_group, local_red_val);

        if (work_group.leader()) {
            sycl::atomic_ref<outT, sycl::memory_order::relaxed,
                             sycl::memory_scope::device,
                             sycl::access::address_space::global_space>
                res_ref(out_[out_iter_offset]);
            outT read_val = res_ref.load();
            outT new_val{};
            do {
                new_val = reduction_op_(read_val, red_val_over_wg);
            } while (!res_ref.compare_exchange_strong(read_val, new_val));
        }
    }
};

template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6>
class boolean_reduction_strided_krn;

template <typename T1, typename T2, typename T3, typename T4, typename T5>
class boolean_reduction_seq_strided_krn;

typedef sycl::event (*boolean_reduction_strided_impl_fn_ptr)(
    sycl::queue,
    size_t,
    size_t,
    const char *,
    char *,
    int,
    const py::ssize_t *,
    py::ssize_t,
    py::ssize_t,
    int,
    const py::ssize_t *,
    py::ssize_t,
    const std::vector<sycl::event> &);

template <typename argTy, typename resTy, typename RedOpT, typename GroupOpT>
sycl::event
boolean_reduction_strided_impl(sycl::queue exec_q,
                               size_t iter_nelems,
                               size_t reduction_nelems,
                               const char *arg_cp,
                               char *res_cp,
                               int iter_nd,
                               const py::ssize_t *iter_shape_and_strides,
                               py::ssize_t iter_arg_offset,
                               py::ssize_t iter_res_offset,
                               int red_nd,
                               const py::ssize_t *reduction_shape_stride,
                               py::ssize_t reduction_arg_offset,
                               const std::vector<sycl::event> &depends)
{
    const argTy *arg_tp = reinterpret_cast<const argTy *>(arg_cp);
    resTy *res_tp = reinterpret_cast<resTy *>(res_cp);

    constexpr resTy identity_val = sycl::known_identity<RedOpT, resTy>::value;

    const sycl::device &d = exec_q.get_device();
    const auto &sg_sizes = d.get_info<sycl::info::device::sub_group_sizes>();
    size_t wg =
        4 * (*std::max_element(std::begin(sg_sizes), std::end(sg_sizes)));

    sycl::event red_ev;
    if (reduction_nelems < wg) {
        red_ev = exec_q.submit([&](sycl::handler &cgh) {
            cgh.depends_on(depends);

            using InputOutputIterIndexerT =
                dpctl::tensor::offset_utils::TwoOffsets_StridedIndexer;
            using ReductionIndexerT =
                dpctl::tensor::offset_utils::StridedIndexer;

            InputOutputIterIndexerT in_out_iter_indexer{
                iter_nd, iter_arg_offset, iter_res_offset,
                iter_shape_and_strides};
            ReductionIndexerT reduction_indexer{red_nd, reduction_arg_offset,
                                                reduction_shape_stride};

            cgh.parallel_for<class boolean_reduction_seq_strided_krn<
                argTy, resTy, RedOpT, InputOutputIterIndexerT,
                ReductionIndexerT>>(
                sycl::range<1>(iter_nelems),
                SequentialBooleanReduction<argTy, resTy, RedOpT,
                                           InputOutputIterIndexerT,
                                           ReductionIndexerT>(
                    arg_tp, res_tp, RedOpT(), identity_val, in_out_iter_indexer,
                    reduction_indexer, reduction_nelems));
        });
    }
    else {
        sycl::event res_init_ev = exec_q.submit([&](sycl::handler &cgh) {
            using IndexerT =
                dpctl::tensor::offset_utils::UnpackedStridedIndexer;

            const py::ssize_t *const &res_shape = iter_shape_and_strides;
            const py::ssize_t *const &res_strides =
                iter_shape_and_strides + 2 * iter_nd;
            IndexerT res_indexer(iter_nd, iter_res_offset, res_shape,
                                 res_strides);

            cgh.depends_on(depends);

            cgh.parallel_for(sycl::range<1>(iter_nelems), [=](sycl::id<1> id) {
                auto res_offset = res_indexer(id[0]);
                res_tp[res_offset] = identity_val;
            });
        });
        red_ev = exec_q.submit([&](sycl::handler &cgh) {
            cgh.depends_on(res_init_ev);

            constexpr size_t group_dim = 2;

            using InputOutputIterIndexerT =
                dpctl::tensor::offset_utils::TwoOffsets_StridedIndexer;
            using ReductionIndexerT =
                dpctl::tensor::offset_utils::StridedIndexer;

            InputOutputIterIndexerT in_out_iter_indexer{
                iter_nd, iter_arg_offset, iter_res_offset,
                iter_shape_and_strides};
            ReductionIndexerT reduction_indexer{red_nd, reduction_arg_offset,
                                                reduction_shape_stride};

            constexpr size_t preferred_reductions_per_wi = 4;
            size_t reductions_per_wi =
                (reduction_nelems < preferred_reductions_per_wi * wg)
                    ? ((reduction_nelems + wg - 1) / wg)
                    : preferred_reductions_per_wi;

            size_t reduction_groups =
                (reduction_nelems + reductions_per_wi * wg - 1) /
                (reductions_per_wi * wg);

            auto gws =
                sycl::range<group_dim>{iter_nelems, reduction_groups * wg};
            auto lws = sycl::range<group_dim>{1, wg};

            cgh.parallel_for<class boolean_reduction_strided_krn<
                argTy, resTy, RedOpT, GroupOpT, InputOutputIterIndexerT,
                ReductionIndexerT>>(
                sycl::nd_range<group_dim>(gws, lws),
                StridedBooleanReduction<argTy, resTy, RedOpT, GroupOpT,
                                        InputOutputIterIndexerT,
                                        ReductionIndexerT>(
                    arg_tp, res_tp, RedOpT(), GroupOpT(), identity_val,
                    in_out_iter_indexer, reduction_indexer, reduction_nelems,
                    reductions_per_wi));
        });
    }
    return red_ev;
}

template <typename fnT, typename srcTy> struct AllStridedFactory
{
    fnT get() const
    {
        using resTy = std::int32_t;
        using RedOpT = sycl::logical_and<resTy>;
        using GroupOpT =
            all_reduce_wg_strided<resTy, boolean_predicate<srcTy>, 2>;

        return dpctl::tensor::kernels::boolean_reduction_strided_impl<
            srcTy, resTy, RedOpT, GroupOpT>;
    }
};

template <typename fnT, typename srcTy> struct AnyStridedFactory
{
    fnT get() const
    {
        using resTy = std::int32_t;
        using RedOpT = sycl::logical_or<resTy>;
        using GroupOpT =
            any_reduce_wg_strided<resTy, boolean_predicate<srcTy>, 2>;

        return dpctl::tensor::kernels::boolean_reduction_strided_impl<
            srcTy, resTy, RedOpT, GroupOpT>;
    }
};

} // namespace kernels
} // namespace tensor
} // namespace dpctl
