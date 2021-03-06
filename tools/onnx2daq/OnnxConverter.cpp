#include "OnnxConverter.h"

#include <string>
#include <fstream>
#include <numeric>
#include <map>

#include <glog/logging.h>
#include <onnx/onnx.pb.h>
#include <onnx/optimizer/optimize.h>
#include <common/StrKeyMap.h>
#include <common/Shaper.h>
#include "NodeAttrHelper.h"

using std::string; using std::vector;
using Shape = Shaper::Shape;

std::string OnnxConverter::m(const std::string &str) {
    if (name_map_.find(str) != name_map_.end()) {
        return name_map_[str];
    }
    
    return str;
}

DNN::FuseCode OnnxConverter::ConvertFuseCodeType(FuseCode fuse_code) {
    switch (fuse_code) {
        case FuseCode::FUSED_NONE:
            return DNN::FuseCode::None;
        case FuseCode::FUSED_RELU:
            return DNN::FuseCode::Relu;
        case FuseCode::FUSED_RELU1:
            return DNN::FuseCode::Relu1;
        case FuseCode::FUSED_RELU6:
            return DNN::FuseCode::Relu6;
    }
    throw std::invalid_argument("Invalid FuseCode");
}

std::pair<std::optional<std::string>, OnnxConverter::FuseCode> OnnxConverter::FindActivation(const ONNX_NAMESPACE::ModelProto &model_proto, const ONNX_NAMESPACE::NodeProto &node) {
    std::pair<std::optional<string>, FuseCode> activation{{}, FuseCode::FUSED_NONE};
    for (const auto &_node : model_proto.graph().node()) {
        if (!node.output().empty() && !_node.input().empty() && node.output(0) == _node.input(0) && _node.op_type() == "Relu") {
            // If there are two branches after a conv/pool and both branches has a relu on the top, we have to add two normal relu layers
            if (activation.second != FuseCode::FUSED_NONE) {
                return {{}, FuseCode::FUSED_NONE};
            }
            activation = std::make_pair(std::make_optional(_node.name()), FuseCode::FUSED_RELU);
        }
    }
    return activation;
}

void OnnxConverter::AddConv(const string &input_name, const std::vector<int> &strides, const std::vector<int> &pads, 
        const std::vector<int> &dilations, int group, 
        const std::pair<std::optional<std::string>, FuseCode>& activation,
        const string &ori_weight_name, const std::optional<std::string> &bias_name, const string &output_name) {
    flatbuffers::Offset<DNN::Layer> layer;
    if (dilations != vector<int>{1, 1}) {
        if (strides != vector<int>{1, 1}) {
            throw std::invalid_argument("Both dilations and strides > 1 is not supported for now");
        }
        LOG(INFO) << "Dilations of conv: " << dilations << ", converting..";
        const auto s2b_name = input_name + "_s2b";
        const auto im_name = input_name + "_conv_imm";
        const auto b2s_name = input_name + "_b2s";
        std::vector<int> new_pads = pads;
        auto input_shape = shaper_[input_name];
        new_pads[1] = (input_shape[1] + pads[1] + (dilations[0] - 1)) / dilations[0] * dilations[0] - input_shape[1];
        new_pads[3] = (input_shape[2] + pads[3] + (dilations[1] - 1)) / dilations[1] * dilations[1] - input_shape[2];
        LOG(INFO) << input_shape << ", " << pads << ", " << dilations << ", " << new_pads;
        {
            shaper_.SpaceToBatch(input_name, dilations, new_pads, s2b_name);
            auto param = DNN::CreateSpaceToBatchDirect(builder_, input_name.c_str(), &dilations, &new_pads, s2b_name.c_str());
            layer = DNN::CreateLayer(builder_, DNN::LayerType::SpaceToBatch, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, param);
            layers_.push_back(layer);
        }
        {
            // paddings are applied in spacetobatch
            AddConv(s2b_name, strides, vector<int>{0, 0, 0, 0}, vector<int>{1, 1}, group, activation, ori_weight_name, bias_name, im_name);
        }
        {
            shaper_.BatchToSpace(im_name, dilations, b2s_name);
            auto param = DNN::CreateBatchToSpaceDirect(builder_, im_name.c_str(), &dilations, b2s_name.c_str());
            layer = DNN::CreateLayer(builder_, DNN::LayerType::BatchToSpace, 0, 0, 0, 0, 0, 0, 0, 0, 0, param);
            layers_.push_back(layer);
        }
        {
            auto b2s_shape = shaper_[b2s_name];
            std::vector<int32_t> starts{0, 0, 0, 0};
            std::vector<int32_t> ends{static_cast<int32_t>(b2s_shape[0]), 
                static_cast<int32_t>(b2s_shape[1]) - (new_pads[1] - pads[0]), 
                static_cast<int32_t>(b2s_shape[2]) - (new_pads[3] - pads[3]), 
                static_cast<int32_t>(b2s_shape[3])};
            std::vector<int32_t> strides_in_ss{1, 1, 1, 1};
            int32_t begin_mask = 0;
            int32_t end_mask = 0;
            int32_t shrink_axis_mask = 0;
            shaper_.StridedSlice(b2s_name, starts, ends, strides_in_ss, begin_mask, end_mask, shrink_axis_mask, output_name);
            auto param = DNN::CreateStridedSliceDirect(builder_, b2s_name.c_str(), &starts, &ends, &strides_in_ss,
                    begin_mask, end_mask, shrink_axis_mask, output_name.c_str());
            layer = DNN::CreateLayer(builder_, DNN::LayerType::StridedSlice, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, param);
            layers_.push_back(layer);
        }
        return;
    }

    const auto &onnx_weight = onnx_tensors_.at(ori_weight_name);
    string weight_name;
    FTensor weight_tensor;
    if (group == 1) {
        LOG(INFO) << "Vanilla conv";
        weight_name = ori_weight_name + "_conv_w";
        weight_tensor = OnnxToNnapiVanilla(onnx_weight);
        shaper_.AddShape(weight_name, weight_tensor.shape);
        shaper_.Conv(input_name, strides[1], strides[0], 1, 1, pads[2], pads[3], pads[0], pads[1], weight_name, output_name);
        nnapi_tensors_[weight_name] = weight_tensor;

        auto param = DNN::CreateConv2DDirect(builder_, input_name.c_str(), weight_name.c_str(),
                bias_name ? bias_name.value().c_str() : nullptr,
                &pads, &strides, ConvertFuseCodeType(activation.second), output_name.c_str());
        layer = DNN::CreateLayer(builder_, DNN::LayerType::Conv2D, param);
    } else if (onnx_weight.shape[1] == 1) {    // depthwise
        LOG(INFO) << "Depthwise conv";
        weight_name = ori_weight_name + "_dwconv_w";
        weight_tensor = OnnxToNnapiDw(onnx_weight);
        shaper_.AddShape(weight_name, weight_tensor.shape);
        shaper_.DepthwiseConv(input_name, strides[1], strides[0], 1, 1, pads[2], pads[3], pads[0], pads[1], weight_name, output_name);
        nnapi_tensors_[weight_name] = weight_tensor;
        auto multiplier = nnapi_tensors_.at(weight_name).shape[3] / group;
        auto param = DNN::CreateDepthwiseConv2DDirect(builder_, input_name.c_str(), weight_name.c_str(),
                bias_name ? bias_name.value().c_str() : nullptr,
                &pads, &strides, multiplier, ConvertFuseCodeType(activation.second), output_name.c_str());
        layer = DNN::CreateLayer(builder_, DNN::LayerType::DepthwiseConv2D, 0, 0, 0, 0, 0, 0, 0, 0, param);
    } else {
        // TODO: Support it
        throw std::invalid_argument("group != 1 is not supported");
    }
    auto flat_tensor = DNN::CreateTensorDirect(builder_, DNN::DataType::Float32, nullptr, 
            &weight_tensor.data, &weight_tensor.shape, weight_name.c_str());
    tensors_.push_back(flat_tensor);
    layers_.push_back(layer);
}

void OnnxConverter::Convert(const ONNX_NAMESPACE::ModelProto &model_proto, const std::string &filepath) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    auto optimized = ONNX_NAMESPACE::optimization::Optimize(model_proto, vector<string>{"fuse_bn_into_conv"});

    for (const auto &tensor : optimized.graph().initializer()) {
        if (tensor.data_type() == ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
            const float *ptr = tensor.float_data().empty() ?
                reinterpret_cast<const float *>(tensor.raw_data().data()) : tensor.float_data().data();
            Shape shape;
            for (auto dim : tensor.dims()) {
                shape.push_back(static_cast<uint32_t>(dim));
            }
            auto data_vec = vector<float>(ptr, ptr + Product(shape));

            onnx_tensors_[tensor.name()] = {data_vec, shape};
        }
        operands_.push_back(tensor.name());
    }

    vector<flatbuffers::Offset<DNN::Input>> inputs;
    for (const auto &input : optimized.graph().input()) {
        if (std::find(operands_.begin(), operands_.end(), input.name()) != operands_.end()) {
            continue;
        }

        Shape shape;
        for (const auto &dim : input.type().tensor_type().shape().dim()) {
            if (dim.value_case() == ONNX_NAMESPACE::TensorShapeProto_Dimension::kDimValue) {
                shape.push_back(static_cast<uint32_t>(dim.dim_value()));
            } else {
                throw std::invalid_argument("The input of graph doesn't have dim_value");
            }
        }
        Shape nnapi_shape{shape[0], shape[2], shape[3], shape[1]};
        shaper_.AddShape(input.name(), nnapi_shape);
        auto flat_input = DNN::CreateInputDirect(builder_, &nnapi_shape, input.name().c_str());
        inputs.push_back(flat_input);
    }

    vector<string> skipped_act;
    bool has_reshape = false;
    for (const auto &node : optimized.graph().node()) {
        if (has_reshape) {
            throw std::invalid_argument("Reshape can only be the last layer for now");
        }
        NodeAttrHelper helper(node);
        const auto &op = node.op_type();
        LOG(INFO) << "Node " << node.name();
        if (op == "Conv") {
            LOG(INFO) << "Start converting Conv";
            auto strides = helper.get("strides", vector<int>{1, 1});
            auto pads = helper.get("pads", vector<int>{0, 0, 0, 0});
            auto dilations = helper.get("dilations", vector<int>{1, 1});
            CHECK_EQ(pads.size(), 4ul);
            CHECK_EQ(strides.size(), 2ul);
            CHECK_EQ(dilations.size(), 2ul);
            auto group = helper.get("group", 1);
            auto activation = FindActivation(optimized, node);
            if (activation.first.has_value()) {
                skipped_act.push_back(activation.first.value());
            }
            std::optional<string> bias_name;
            if (node.input_size() >= 3) {
                auto ori_bias_name = m(node.input(2));
                bias_name = ori_bias_name + "_conv_b";
                nnapi_tensors_[bias_name.value()] = onnx_tensors_.at(ori_bias_name);
                auto flat_tensor = DNN::CreateTensorDirect(builder_, DNN::DataType::Float32, nullptr, 
                        &nnapi_tensors_.at(bias_name.value()).data, &nnapi_tensors_.at(bias_name.value()).shape, 
                        bias_name.value().c_str());
                tensors_.push_back(flat_tensor);
            }

            auto ori_weight_name = m(node.input(1));
            AddConv(m(node.input(0)), strides, pads, dilations, group, activation, ori_weight_name, bias_name, m(node.output(0)));
            LOG(INFO) << "Converting Conv completed";
        } else if (op == "AveragePool" || op == "MaxPool" || op == "GlobalAveragePool" || op == "GlobalMaxPool") {
            LOG(INFO) << "Start converting Pool";
            auto input_name = m(node.input(0));
            auto output_name = m(node.output(0));
            vector<int> strides, pads, kernel_shape;
            if (op == "AveragePool" || op == "MaxPool") {
                strides = helper.get("strides", vector<int>{1, 1});
                pads = helper.get("pads", vector<int>{0, 0, 0, 0});
                kernel_shape = helper.get("kernel_shape", vector<int>{0, 0});
                auto count_include_pad = helper.get("count_include_pad", 0);
                if (count_include_pad == 1) {
                    throw std::invalid_argument("count_include_pad == 1 is not supported");
                }
                auto storage_order = helper.get("storage_order", 0);
                if (storage_order == 1) {
                    throw std::invalid_argument("storage_order == 1 is not supported");
                }
                if (helper.has_attr("auto_pad")) {
                    throw std::invalid_argument("auto_pad is not supported");
                }
            } else {
                strides = {0, 0};
                pads = {0, 0, 0, 0};
                kernel_shape = {-1, -1};    // -1 for global
            }
            CHECK_EQ(pads.size(), 4ul);
            CHECK_EQ(kernel_shape.size(), 2ul);
            CHECK_EQ(strides.size(), 2ul);
            auto activation = FindActivation(optimized, node);
            if (activation.first.has_value()) {
                skipped_act.push_back(activation.first.value());
            }
            shaper_.Pool(input_name, strides[1], strides[0], pads[2], pads[3], pads[0], pads[1], kernel_shape[0], kernel_shape[1], output_name);
            flatbuffers::Offset<DNN::Layer> layer;
            if (op == "AveragePool" || op == "GlobalAveragePool") {
                auto param = DNN::CreateAvePoolDirect(builder_, input_name.c_str(), &kernel_shape, &pads, &strides,
                        ConvertFuseCodeType(activation.second), output_name.c_str());
                layer = DNN::CreateLayer(builder_, DNN::LayerType::AvePool, 0, param);
            } else {
                auto param = DNN::CreateMaxPoolDirect(builder_, input_name.c_str(), &kernel_shape, &pads, &strides,
                        ConvertFuseCodeType(activation.second), output_name.c_str());
                layer = DNN::CreateLayer(builder_, DNN::LayerType::MaxPool, 0, 0, param);
            }
            layers_.push_back(layer);
            // operand_indexes[node.output(0)] = builder_.addPool(operand_indexes.at(node.input(0)), strides[1], strides[0],
            // pads[2], pads[3], pads[0], pads[1],
            // kernel_shape[0], kernel_shape[1], activation.second,
            // op == "AveragePool" ? ModelBuilder::AVE_POOL
            // : ModelBuilder::MAX_POOL);
            LOG(INFO) << "Converting Pool completed";
        } else if (op == "Relu") {
            LOG(INFO) << "Start converting Relu";
            auto input_name = m(node.input(0));
            auto output_name = m(node.output(0));
            shaper_.Relu(input_name, output_name);
            auto param = DNN::CreateReluDirect(builder_, input_name.c_str(), output_name.c_str());
            auto layer = DNN::CreateLayer(builder_, DNN::LayerType::Relu, 0, 0, 0, param);
            layers_.push_back(layer);
            LOG(INFO) << "Converting Relu completed";
            // operand_indexes[node.output(0)] = builder_.addReLU(operand_indexes.at(node.input(0)));
        } else if (op == "Add") {
            LOG(INFO) << "Start converting Add";
            auto input1_name = m(node.input(0));
            auto input2_name = m(node.input(1));
            auto output_name = m(node.output(0));
            shaper_.Eltwise(input1_name, input2_name, output_name);
            auto activation = FindActivation(optimized, node);
            if (activation.first.has_value()) {
                skipped_act.push_back(activation.first.value());
            }
            auto param = DNN::CreateAddDirect(builder_, input1_name.c_str(), input2_name.c_str(),
                    ConvertFuseCodeType(activation.second), output_name.c_str());
            auto layer = DNN::CreateLayer(builder_, DNN::LayerType::Add, 0, 0, 0, 0, 0, 0, param);
            layers_.push_back(layer);
            LOG(INFO) << "Converting Add completed";
            // auto input1 = operand_indexes.at(node.input(0));
            // auto input2 = operand_indexes.at(node.input(1));
            // operand_indexes[node.output(1)] = builder.addAddTensor(input1, input2);
        } else if (op == "Gemm") {
            LOG(INFO) << "Start converting Gemm";
            auto transA = helper.get("transA", 0);
            auto transB = helper.get("transB", 0);
            auto alpha = helper.get("alpha", 1.0f);
            auto beta = helper.get("beta", 1.0f);
            if (transA == 0 && transB == 1 && alpha == 1.f && beta == 1.f) {
                auto input_name = m(node.input(0));
                auto weight_name = m(node.input(1));
                {
                    nnapi_tensors_[weight_name] = onnx_tensors_.at(weight_name);
                    const auto &weight_tensor = nnapi_tensors_[weight_name];
                    shaper_.AddShape(weight_name, weight_tensor.shape);
                    auto flat_tensor = DNN::CreateTensorDirect(builder_, DNN::DataType::Float32, nullptr,
                            &weight_tensor.data, &weight_tensor.shape,
                            weight_name.c_str());
                    tensors_.push_back(flat_tensor);
                }
                string bias_name;
                if (node.input_size() >= 3) {
                    bias_name = m(node.input(2));
                    nnapi_tensors_[bias_name] = onnx_tensors_.at(bias_name);
                    const auto &bias_tensor = nnapi_tensors_[bias_name];
                    auto flat_tensor = DNN::CreateTensorDirect(builder_, DNN::DataType::Float32, nullptr,
                            &bias_tensor.data, &bias_tensor.shape, bias_name.c_str());
                    tensors_.push_back(flat_tensor);
                }
                auto activation = FindActivation(optimized, node);
                if (activation.first.has_value()) {
                    skipped_act.push_back(activation.first.value());
                }
                auto output_name = m(node.output(0));
                shaper_.FC(input_name, weight_name, output_name);
                auto param = DNN::CreateFCDirect(builder_, input_name.c_str(), weight_name.c_str(),
                        node.input_size() >= 3 ? bias_name.c_str() : nullptr,
                        ConvertFuseCodeType(activation.second), output_name.c_str()
                        );
                auto layer = DNN::CreateLayer(builder_, DNN::LayerType::FC, 0, 0, 0, 0, 0, param, 0);
                layers_.push_back(layer);
                // builder.addFC(operand_indexes.at(node.input(0)), activation.second,
                // operand_indexes.at(node.input(1)), operand_indexes.at(node.input(2)));
            } else {
                throw std::invalid_argument(
                        "Only transA == 0, transB == 1, alpha == 1.0 and beta == 1.0 is supported.");
            }
            LOG(INFO) << "Converting Gemm completed";
        } else if (op == "Softmax") {
            LOG(INFO) << "Start converting Softmax";
            auto input_name = m(node.input(0));
            auto output_name = m(node.output(0));
            shaper_.Softmax(input_name, output_name);
            // simply ignore attribute "axis", because nnapi softmax didn't has this attr, and we will check the equality of the two ops in DaqReader.cpp
            auto param = DNN::CreateSoftmaxDirect(builder_, input_name.c_str(), output_name.c_str());
            auto layer = DNN::CreateLayer(builder_, DNN::LayerType::Softmax, 0, 0, 0, 0, param);
            layers_.push_back(layer);
            LOG(INFO) << "Converting Softmax completed";
        } else if (op == "Concat") {
            LOG(INFO) << "Start converting Concat";
            vector<flatbuffers::Offset<flatbuffers::String>> concat_inputs;
            vector<std::string> concat_inputs_str;
            for (const auto &onnx_input : node.input()) {
                concat_inputs_str.push_back(onnx_input);
                auto flat_input = builder_.CreateString(m(onnx_input).c_str(), onnx_input.size());
                concat_inputs.push_back(flat_input);
            }
            auto axis = helper.get("axis", 1);
            uint32_t axis_nchw_to_nhwc[4]{0, 3, 1, 2};
            auto output_name = m(node.output(0));
            shaper_.Concat(concat_inputs_str, axis, output_name);
            auto param = DNN::CreateConcatDirect(builder_, &concat_inputs, axis_nchw_to_nhwc[axis], output_name.c_str());
            auto layer = DNN::CreateLayer(builder_, DNN::LayerType::Concat, 0, 0, 0, 0, 0, 0, 0, param);
            layers_.push_back(layer);
            LOG(INFO) << "Converting Concat completed";
        } else if (op == "Dropout") {
            LOG(INFO) << "Start converting Dropout";
            // Dropout does nothing, so the output is the same as the input
            name_map_[node.output(0)] = m(node.input(0));
            LOG(INFO) << "Converting Dropout completed";
        } else if (op == "Reshape") {
            LOG(INFO) << "Start converting Reshape";
            has_reshape = true;
            LOG(INFO) << "Converting Reshape completed";
        } else {
            throw std::invalid_argument("Unsupported operator " + op);
        }
    }
    auto flat_layers = builder_.CreateVector(layers_);
    auto flat_inputs = builder_.CreateVector(inputs);
    auto flat_tensors = builder_.CreateVector(tensors_);
    auto flat_model = DNN::CreateModel(builder_, flat_layers, flat_tensors, flat_inputs);

    builder_.Finish(flat_model);

    LOG(INFO) << "Shapes: ";
    LOG(INFO) << shaper_;

    std::ofstream ofs(filepath);
    ofs.write(reinterpret_cast<char *>(builder_.GetBufferPointer()), builder_.GetSize());
    ofs.close();
}
