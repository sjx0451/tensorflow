/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/delegates/gpu/cl/kernels/special/depthwise_conv_plus_1x1_conv.h"

#include <string>
#include <utility>
#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/cl_device.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"

namespace tflite {
namespace gpu {
namespace cl {

DepthwiseConvPlus1x1Conv::DepthwiseConvPlus1x1Conv(
    const OperationDef& definition,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr)
    : GPUOperation(definition), dw_attr_(dw_attr) {
  work_group_size_ = int3(8, 8, 1);
  code_ = GenerateCode(definition_, dw_attr_,
                       DivideRoundUp(conv_attr.weights.shape.o, 4));
}

DepthwiseConvPlus1x1Conv::DepthwiseConvPlus1x1Conv(
    DepthwiseConvPlus1x1Conv&& operation)
    : GPUOperation(std::move(operation)),
      dw_attr_(std::move(operation.dw_attr_)) {}

DepthwiseConvPlus1x1Conv& DepthwiseConvPlus1x1Conv::operator=(
    DepthwiseConvPlus1x1Conv&& operation) {
  if (this != &operation) {
    dw_attr_ = std::move(operation.dw_attr_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

absl::Status DepthwiseConvPlus1x1Conv::UploadWeights(
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr, CLContext* context) {
  int dw_dst_ch_aligned = AlignByN(dw_attr.weights.shape.i, 4);
  int dw_weights_count =
      dw_dst_ch_aligned * dw_attr.weights.shape.h * dw_attr.weights.shape.w;
  int conv_src_ch_aligned = AlignByN(conv_attr.weights.shape.i, 4);
  int conv_dst_ch_aligned = AlignByN(conv_attr.weights.shape.o, 4);
  int conv_weights_count = conv_src_ch_aligned * conv_dst_ch_aligned;
  std::vector<float> gpu_data;
  gpu_data.reserve(dw_dst_ch_aligned + dw_weights_count + conv_dst_ch_aligned +
                   conv_weights_count);
  // dw bias loading
  for (int i = 0; i < dw_dst_ch_aligned; ++i) {
    if (i < dw_attr.bias.shape.v) {
      gpu_data.push_back(dw_attr.bias.data[i]);
    } else {
      gpu_data.push_back(0.0f);
    }
  }
  // dw weights loading
  for (int y = 0; y < dw_attr.weights.shape.h; ++y) {
    for (int x = 0; x < dw_attr.weights.shape.w; ++x) {
      for (int d = 0; d < dw_dst_ch_aligned / 4; ++d) {
        for (int i = 0; i < 4; ++i) {
          const int d_ch = d * 4 + i;
          if (d_ch < dw_attr.weights.shape.i) {
            const int f_index =
                dw_attr.weights.shape.LinearIndex({0, y, x, d_ch});
            gpu_data.push_back(dw_attr.weights.data[f_index]);
          } else {
            gpu_data.push_back(0.0f);
          }
        }
      }
    }
  }
  // conv bias loading
  for (int i = 0; i < conv_dst_ch_aligned; ++i) {
    if (i < conv_attr.bias.shape.v) {
      gpu_data.push_back(conv_attr.bias.data[i]);
    } else {
      gpu_data.push_back(0.0f);
    }
  }
  // conv weights loading
  for (int d = 0; d < conv_dst_ch_aligned / 4; ++d) {
    for (int s = 0; s < conv_src_ch_aligned / 4; ++s) {
      for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
          const int s_ch = s * 4 + j;
          const int d_ch = d * 4 + i;
          if (s_ch < conv_attr.weights.shape.i &&
              d_ch < conv_attr.weights.shape.o) {
            const int f_index =
                conv_attr.weights.shape.LinearIndex({d_ch, 0, 0, s_ch});
            gpu_data.push_back(conv_attr.weights.data[f_index]);
          } else {
            gpu_data.push_back(0.0f);
          }
        }
      }
    }
  }

  Buffer constants_buf;
  const bool fp32_weights = definition_.precision == CalculationsPrecision::F32;
  const int float_size = fp32_weights ? 4 : 2;
  if (fp32_weights) {
    RETURN_IF_ERROR(CreateReadOnlyBuffer(float_size * gpu_data.size(),
                                         gpu_data.data(), context,
                                         &constants_buf));
  } else {
    std::vector<half> gpu_data_half(gpu_data.size());
    for (int i = 0; i < gpu_data.size(); ++i) {
      gpu_data_half[i] = gpu_data[i];
    }
    RETURN_IF_ERROR(CreateReadOnlyBuffer(float_size * gpu_data_half.size(),
                                         gpu_data_half.data(), context,
                                         &constants_buf));
  }

  BufferDescriptor desc;
  desc.element_type = fp32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
  desc.element_size = 4;
  desc.memory_type = MemoryType::CONSTANT;
  args_.AddObject("constants", AccessType::READ,
                  absl::make_unique<Buffer>(std::move(constants_buf)),
                  absl::make_unique<BufferDescriptor>(desc));
  return absl::OkStatus();
}

std::string DepthwiseConvPlus1x1Conv::GenerateCode(
    const OperationDef& op_def, const DepthwiseConvolution2DAttributes& dw_attr,
    int result_depth) {
  auto src_desc = op_def.src_tensors[0];
  src_desc.SetTextureAddressMode(TextureAddressMode::ZERO);
  AddSrcTensor("src_tensor", src_desc);
  AddDstTensor("dst_tensor", op_def.dst_tensors[0]);

  args_.AddInt("stride_x", dw_attr.strides.w);
  args_.AddInt("padding_x", -dw_attr.padding.prepended.w);
  args_.AddInt("dilation_x", dw_attr.dilations.w);
  args_.AddInt("stride_y", dw_attr.strides.h);
  args_.AddInt("padding_y", -dw_attr.padding.prepended.h);
  args_.AddInt("dilation_y", dw_attr.dilations.h);

  const auto src_tensor_type = op_def.src_tensors[0].storage_type;

  const bool manual_clamp = src_tensor_type == TensorStorageType::BUFFER ||
                            src_tensor_type == TensorStorageType::IMAGE_BUFFER;

  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  if (op_def.dst_tensors[0].HasAxis(Axis::BATCH)) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int X = linear_id / args.dst_tensor.Batch();\n";
    c += "  int B = linear_id % args.dst_tensor.Batch();\n";
    c += "  args.dst_tensor.SetBatchRef(B);\n";
    c += "  args.src_tensor.SetBatchRef(B);\n";
  } else {
    c += "  int X = get_global_id(0);\n";
  }
  c += "  int Y = get_global_id(1);\n";
  c += "  if (X >= args.dst_tensor.Width() || Y >= args.dst_tensor.Height()) { "
       "\n";
  c += "    return; \n";
  c += "  } \n";
  c += "  __constant FLT4* constants = args.constants.GetPtr();\n";
  int intermediate_depth = DivideRoundUp(dw_attr.weights.shape.i, 4);
  int weights_counter = 0;
  for (int d = 0; d < intermediate_depth; ++d) {
    c += "  FLT4 dw_res_" + std::to_string(d) + " = constants[" +
         std::to_string(weights_counter++) + "];\n";
  }
  c += "  int x_offseted = X * args.stride_x + args.padding_x;\n";
  c += "  int y_offseted = Y * args.stride_y + args.padding_y;\n";
  c += "  int x_c, y_c;\n";
  if (manual_clamp) {
    c += "  bool x_in, y_in;\n";
  }
  c += "  FLT4 src;\n";
  for (int ky = 0; ky < dw_attr.weights.shape.h; ++ky) {
    c += "  y_c = y_offseted + " + std::to_string(ky) + " * args.dilation_y;\n";
    if (manual_clamp) {
      c += "  y_in = y_c >= 0 && y_c < args.src_tensor.Height();\n";
      c += "  y_c = clamp(y_c, 0, args.src_tensor.Height() - 1);\n";
    }
    for (int kx = 0; kx < dw_attr.weights.shape.w; ++kx) {
      c += "  x_c = x_offseted + " + std::to_string(kx) +
           " * args.dilation_x;\n";
      if (manual_clamp) {
        c += "  x_in = x_c >= 0 && x_c < args.src_tensor.Width();\n";
        c += "  x_c = clamp(x_c, 0, args.src_tensor.Width() - 1);\n";
      }
      for (int d = 0; d < intermediate_depth; ++d) {
        std::string multiplier = manual_clamp ? "* (FLT)(x_in && y_in)" : "";
        c += "  src = args.src_tensor.Read(x_c, y_c, " + std::to_string(d) +
             ")" + multiplier + ";\n";
        c += "  dw_res_" + std::to_string(d) + " += src * constants[" +
             std::to_string(weights_counter++) + "];\n";
      }
    }
  }
  for (int d = 0; d < result_depth; ++d) {
    c += "  FLT4 conv_res_" + std::to_string(d) + " = constants[" +
         std::to_string(weights_counter++) + "];\n";
  }
  for (int d = 0; d < result_depth; ++d) {
    for (int s = 0; s < intermediate_depth; ++s) {
      std::string src = "dw_res_" + std::to_string(s);
      std::string dst = "conv_res_" + std::to_string(d);
      c += "  " + dst + " += " + src + ".x * constants[" +
           std::to_string(weights_counter++) + "];\n";
      c += "  " + dst + " += " + src + ".y * constants[" +
           std::to_string(weights_counter++) + "];\n";
      c += "  " + dst + " += " + src + ".z * constants[" +
           std::to_string(weights_counter++) + "];\n";
      c += "  " + dst + " += " + src + ".w * constants[" +
           std::to_string(weights_counter++) + "];\n";
    }
    c += "  args.dst_tensor.Write(conv_res_" + std::to_string(d) + ", X, Y, " +
         std::to_string(d) + ");\n";
  }
  c += "}\n";

  return c;
}

int3 DepthwiseConvPlus1x1Conv::GetGridSize() const {
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = dst_[0]->Height();
  return int3(grid_x, grid_y, 1);
}

bool IsDepthwiseConvPlus1x1ConvSupported(
    const CLDevice& device, const OperationDef& definition,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr) {
  const auto dw_shape = dw_attr.weights.shape;
  const auto conv_shape = conv_attr.weights.shape;
  bool good_dw = dw_shape.o == 1;
  bool good_conv =
      conv_shape.w == 1 && conv_shape.h == 1 && conv_attr.dilations.w == 1 &&
      conv_attr.dilations.h == 1 && conv_attr.strides.w == 1 &&
      conv_attr.strides.h == 1 && conv_attr.padding.prepended.w == 0 &&
      conv_attr.padding.prepended.h == 0 && conv_attr.padding.appended.w == 0 &&
      conv_attr.padding.appended.h == 0;
  bool recommended_dw =
      dw_shape.i <= 16 && dw_shape.i * dw_shape.h * dw_shape.w <= 3 * 3 * 16;
  bool recommended_conv =
      conv_shape.o <= 32 && conv_shape.i * conv_shape.o <= 16 * 32;
  return good_dw && good_conv && recommended_dw && recommended_conv;
}

absl::Status CreateDepthwiseConvPlus1x1Conv(
    const CreationContext& creation_context, const OperationDef& definition,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr,
    DepthwiseConvPlus1x1Conv* result) {
  *result = DepthwiseConvPlus1x1Conv(definition, dw_attr, conv_attr);
  RETURN_IF_ERROR(
      result->UploadWeights(dw_attr, conv_attr, creation_context.context));
  return absl::OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
