// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/transpose_sinking_interpolate.hpp"

#include <openvino/pass/pattern/op/or.hpp>

#include "itt.hpp"
#include "openvino/op/util/op_types.hpp"
#include "openvino/opsets/opset10.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "openvino/util/common_util.hpp"
#include "transformations/common_optimizations/transpose_sinking_utils.hpp"
#include "transformations/rt_info/transpose_sinking_attr.hpp"

using namespace ov;
using namespace ov::opset10;
using namespace ov::pass::pattern;
using namespace transpose_sinking;

ov::pass::TransposeSinkingInterpolateForward::TransposeSinkingInterpolateForward() {
    MATCHER_SCOPE(TransposeSinkingInterpolateForward);
    auto const_label = wrap_type<Constant>();
    auto transpose_label = wrap_type<Transpose>({any_input(), const_label});
    auto main_node_label = wrap_type<Interpolate>({transpose_label, any_input(), any_input(), any_input()});

    matcher_pass_callback matcher_pass_callback = [=](Matcher& m) {
        const auto& pattern_to_node = m.get_pattern_map();

        auto& main_node = pattern_to_node.at(main_node_label);
        auto transpose = std::dynamic_pointer_cast<Transpose>(pattern_to_node.at(transpose_label));
        if (!transpose) {
            return false;
        }

        auto transpose_const = as_type_ptr<Constant>(pattern_to_node.at(const_label));
        if (!transpose_const) {
            return false;
        }

        // remove Transpose on 1st input:
        auto transpose_parent = main_node->input_value(0).get_node()->input_value(0);
        main_node->input(0).replace_source_output(transpose_parent);

        const auto transpose_axis_order = transpose_const->get_axis_vector_val();
        auto axis = std::make_shared<Constant>(element::i32, Shape{}, std::vector<int32_t>{0});

        const auto& interpolate = std::dynamic_pointer_cast<Interpolate>(main_node);

        auto data = std::make_shared<Constant>(element::i32, Shape{transpose_axis_order.size()}, transpose_axis_order);
        const auto& indices = main_node->input_value(3);
        auto new_axis = std::make_shared<Gather>(data, indices, axis);

        main_node->input(3).replace_source_output(new_axis);
        if (interpolate) {
            op::v4::Interpolate::InterpolateAttrs attrs = interpolate->get_attrs();
            if (!attrs.pads_begin.empty() || !attrs.pads_end.empty()) {
                const auto& order_size = transpose_axis_order.size();
                attrs.pads_begin.resize(order_size);
                attrs.pads_end.resize(order_size);
                std::vector<size_t> new_pads_begin(order_size), new_pads_end(order_size);
                for (size_t i = 0; i < order_size; ++i) {
                    new_pads_begin[i] = attrs.pads_begin[transpose_axis_order[i]];
                    new_pads_end[i] = attrs.pads_end[transpose_axis_order[i]];
                }
                std::swap(attrs.pads_begin, new_pads_begin);
                std::swap(attrs.pads_end, new_pads_end);
                interpolate->set_attrs(attrs);
            }
        }

        main_node->validate_and_infer_types();
        TransposeInputsInfo transpose_input_info = {transpose, transpose_const, 0};
        for (auto& new_node : sink_forward::InsertOutputTransposes(main_node, transpose_input_info)) {
            register_new_node(new_node);
            transpose_sinking::UpdateForwardSinkingAbility(new_node);
        }
        return true;
    };

    auto m = std::make_shared<Matcher>(main_node_label, matcher_name);
    register_matcher(m, matcher_pass_callback);
}

ov::pass::TransposeSinkingInterpolateBackward::TransposeSinkingInterpolateBackward() {
    MATCHER_SCOPE(TransposeSinkingInterpolateBackward);

    auto main_node_label = wrap_type<Interpolate>([](const Output<Node>& output) -> bool {
        return has_static_rank()(output) && HasSameOutputTransposeNodes(output);
    });

    auto transpose_const_label = wrap_type<Constant>();

    auto transpose_label =
        wrap_type<Transpose>({main_node_label, transpose_const_label}, [](const Output<Node>& output) -> bool {
            return has_static_rank()(output) && is_sinking_node(output);
        });

    matcher_pass_callback matcher_pass_callback = [=](Matcher& m) {
        const auto& pattern_to_output = m.get_pattern_value_map();
        auto transpose_const = as_type_ptr<Constant>(pattern_to_output.at(transpose_const_label).get_node_shared_ptr());
        auto transpose = pattern_to_output.at(transpose_label).get_node_shared_ptr();
        auto main_node = pattern_to_output.at(main_node_label).get_node_shared_ptr();

        for (auto& new_node : sink_backward::InsertTransposeBeforeNode(main_node,
                                                                       transpose_const,
                                                                       /* input_indexes= */ {0})) {
            register_new_node(new_node);
        }

        // remove output transposes
        RemoveSingleOutputConsumers(main_node);
        SwapNames(main_node, transpose);
        const auto transpose_axis_order = transpose_const->get_axis_vector_val();
        const auto reversed_transpose_order = ReverseTransposeOrder(transpose_axis_order);
        auto axis = std::make_shared<Constant>(element::i32, Shape{}, std::vector<int32_t>{0});
        auto data =
            std::make_shared<Constant>(element::i32, Shape{reversed_transpose_order.size()}, reversed_transpose_order);
        const auto& indices = main_node->input_value(3);
        auto new_axis = std::make_shared<Gather>(data, indices, axis);
        main_node->input(3).replace_source_output(new_axis);

        const auto& interpolate = std::dynamic_pointer_cast<Interpolate>(main_node);
        if (interpolate) {
            op::v4::Interpolate::InterpolateAttrs attrs = interpolate->get_attrs();
            if (!attrs.pads_begin.empty() || !attrs.pads_end.empty()) {
                const auto& order_size = reversed_transpose_order.size();
                attrs.pads_begin.resize(order_size);
                attrs.pads_end.resize(order_size);
                std::vector<size_t> new_pads_begin(order_size), new_pads_end(order_size);
                for (size_t i = 0; i < order_size; ++i) {
                    new_pads_begin[i] = attrs.pads_begin[reversed_transpose_order[i]];
                    new_pads_end[i] = attrs.pads_end[reversed_transpose_order[i]];
                }
                std::swap(attrs.pads_begin, new_pads_begin);
                std::swap(attrs.pads_end, new_pads_end);
                interpolate->set_attrs(attrs);
            }
        }
        main_node->validate_and_infer_types();
        return true;
    };

    auto m = std::make_shared<Matcher>(transpose_label, matcher_name);
    register_matcher(m, matcher_pass_callback);
}
