// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include "openvino/frontend/pytorch/node_context.hpp"
#include "openvino/opsets/opset10.hpp"
#include "utils.hpp"
namespace ov {
namespace frontend {
namespace pytorch {
namespace op {

OutputVector translate_sort(NodeContext& context) {
    num_inputs_check(context, 3, 4);
    const auto input_tensor = context.get_input(0);
    bool stable, descending;
    int64_t dim;

    if (context.get_input_size() == 4) {
        stable = context.const_input<bool>(1);
        dim = context.const_input<int64_t>(2);
        descending = context.const_input<bool>(3);
        FRONT_END_OP_CONVERSION_CHECK(stable == false, "Stable sorting in aten::sort is not yet supported.");
    } else {
        dim = context.const_input<int64_t>(1);
        descending = context.const_input<bool>(2);
    }

    auto mode = descending ? ov::op::TopKMode::MAX : ov::op::TopKMode::MIN;
    auto zero_axis = context.mark_node(opset10::Constant::create(element::i32, Shape{1}, {0}));
    auto dim_axis = context.mark_node(opset10::Constant::create(element::i64, Shape{1}, {dim}));
    auto shape = context.mark_node(std::make_shared<opset10::ShapeOf>(input_tensor));
    auto k_values_node = context.mark_node(std::make_shared<opset10::Gather>(shape, dim_axis, zero_axis));
    auto k_values = context.mark_node(std::make_shared<opset10::Squeeze>(k_values_node));
    auto topk = context.mark_node(std::make_shared<opset10::TopK>(input_tensor,
                                                                  k_values,
                                                                  dim,
                                                                  mode,
                                                                  ov::op::TopKSortType::SORT_VALUES,
                                                                  element::i64));
    return topk->outputs();
};

OutputVector translate_argsort(NodeContext& context) {
    auto sort = translate_sort(context);
    return {sort[1]};
};

}  // namespace op
}  // namespace pytorch
}  // namespace frontend
}  // namespace ov
