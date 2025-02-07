// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "listconstruct_replacer.hpp"

#include "openvino/core/rt_info.hpp"
#include "openvino/op/adaptive_avg_pool.hpp"
#include "openvino/op/broadcast.hpp"
#include "openvino/op/concat.hpp"
#include "openvino/op/constant.hpp"
#include "openvino/op/equal.hpp"
#include "openvino/op/reshape.hpp"
#include "openvino/op/roll.hpp"
#include "openvino/op/select.hpp"
#include "openvino/op/shape_of.hpp"
#include "openvino/op/tile.hpp"
#include "openvino/op/transpose.hpp"
#include "openvino/op/util/framework_node.hpp"
#include "openvino/pass/pattern/matcher.hpp"
#include "openvino/pass/pattern/op/or.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "utils.hpp"

namespace ov {
namespace frontend {
namespace pytorch {
namespace pass {

using namespace ov::pass;
using namespace ov::op;

ListConstructReplacer::ListConstructReplacer() {
    // Transformation for torch operators for cases where prim::ListConstruct can be replaced with Concat.
    auto list = pattern::wrap_type<ov::op::util::FrameworkNode>();

    // Both aten::view and aten::reshape are using same translation returning Reshape operator.
    auto reshape_op = pattern::wrap_type<v1::Reshape>({pattern::any_input(), list});
    auto roll_op = pattern::wrap_type<v7::Roll>({pattern::any_input(), list, pattern::any_input()});
    auto broadcast_op = pattern::wrap_type<v3::Broadcast>({pattern::any_input(), list});
    auto adapool_op = pattern::wrap_type<v8::AdaptiveAvgPool>({pattern::any_input(), list});
    // replace list construct for aten::expand(tensor, prim::ListConstruct(shapes)) decomposition
    //  shape_of + broadcast + equal + select
    auto shape_of_op = pattern::wrap_type<v3::ShapeOf>({list});
    auto equal_op = pattern::wrap_type<v1::Equal>({list, pattern::any_input()});
    auto select_op = pattern::wrap_type<v1::Select>({pattern::any_input(), pattern::any_input(), list});
    // replace list construct for aten::repeat(tensor,  prim::ListConstruct(shapes)))
    // shape_of + broadcast + tile
    auto tile_op = pattern::wrap_type<v0::Tile>({pattern::any_input(), list});
    // replace aten::permute(tensor, prim::ListConstruct)
    auto transpose_op = pattern::wrap_type<v1::Transpose>({pattern::any_input(), list});
    auto lc_pattern = std::make_shared<pattern::op::Or>(OutputVector{reshape_op,
                                                                     roll_op,
                                                                     broadcast_op,
                                                                     adapool_op,
                                                                     shape_of_op,
                                                                     equal_op,
                                                                     select_op,
                                                                     tile_op,
                                                                     transpose_op});

    ov::matcher_pass_callback callback = [=](pattern::Matcher& m) {
        auto& pattern_map = m.get_pattern_value_map();

        auto list_node = pattern_map.at(list).get_node_shared_ptr();
        // Concatenation is possible because all elements in list should be scalar or 1D tensors,
        // result should be 1D tensor.
        OutputVector inputs;
        auto neg_1 = v0::Constant::create(element::i32, Shape{1}, {-1});
        const auto& start_output = list_node->output(0);
        for (const auto& input : get_list_as_outputs(start_output)) {
            if (input == start_output) {
                // Start output exist in list elements, it might mean we have only 1 element in list inputs and it is
                // already a list, we do not need to concat it
                return false;
            }
            auto rank = input.get_partial_shape().rank();
            if (rank.is_static() && rank.get_length() > 1) {
                // if list elements of rank higher then 1D we cannot resolve it
                return false;
            }
            // reshape all elements to 1D
            auto reshape = std::make_shared<v1::Reshape>(input, neg_1, false);
            inputs.push_back(reshape);
        }
        auto concat = std::make_shared<v0::Concat>(inputs, 0);
        copy_runtime_info({list_node}, concat);
        replace_node(list_node, concat);
        concat->set_friendly_name(list_node->get_friendly_name());
        return true;
    };
    auto m = std::make_shared<pattern::Matcher>(lc_pattern, "ov::frontend::pytorch::pass::ListConstructReplacer");
    this->register_matcher(m, callback);
};

}  // namespace pass
}  // namespace pytorch
}  // namespace frontend
}  // namespace ov
