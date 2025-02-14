/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <unordered_map>
#include "paddle/fluid/framework/data_layout_transform.h"
#include "paddle/fluid/memory/malloc.h"
#include "paddle/fluid/operators/conv_op.h"
#include "paddle/fluid/platform/mkldnn_reuse.h"

namespace paddle {
namespace operators {

using framework::DataLayout;
using mkldnn::memory;
using mkldnn::primitive;
using mkldnn::reorder;
using mkldnn::stream;
using platform::to_void_cast;
using platform::GetMKLDNNFormat;

inline void GetWeightsTz(std::vector<int>& weights_tz, int groups,  // NOLINT
                         bool is_conv3d) {
  if (groups > 1) {
    if (is_conv3d) {
      int output = weights_tz[0];
      int input = weights_tz[1];
      int dimension = weights_tz[2];
      int height = weights_tz[3];
      int width = weights_tz[4];
      weights_tz.resize(6);
      weights_tz[0] = groups;
      weights_tz[1] = output / groups;
      weights_tz[2] = input;
      weights_tz[3] = dimension;
      weights_tz[4] = height;
      weights_tz[5] = width;
    } else {
      int output = weights_tz[0];
      int input = weights_tz[1];
      int height = weights_tz[2];
      int width = weights_tz[3];
      weights_tz.resize(5);
      weights_tz[0] = groups;
      weights_tz[1] = output / groups;
      weights_tz[2] = input;
      weights_tz[3] = height;
      weights_tz[4] = width;
    }
  }
}

inline MKLDNNMemoryFormat GetWeightsFormat(MKLDNNMemoryFormat format,
                                           int groups, bool is_conv3d) {
  if (is_conv3d) {
    return (groups == 1) ? format : MKLDNNMemoryFormat::goidhw;
  } else {
    return (groups == 1) ? format : MKLDNNMemoryFormat::goihw;
  }
}

static mkldnn::memory::data_type GetDstType(bool is_int8,
                                            bool force_fp32_output,
                                            std::string fuse_activation,
                                            bool fuse_residual_conn,
                                            const Tensor* residual_param) {
  auto dst_dt = mkldnn::memory::data_type::f32;  // uint8_t, int8_t, float
  if (is_int8) {
    dst_dt = (fuse_activation == "relu" || fuse_activation == "relu6")
                 ? mkldnn::memory::data_type::u8
                 : mkldnn::memory::data_type::s8;
    if (force_fp32_output) {
      dst_dt = mkldnn::memory::data_type::f32;
    }
    if (fuse_residual_conn && residual_param) {
      auto residual_dt = framework::ToMKLDNNDataType(residual_param->type());
      if (dst_dt != residual_dt) dst_dt = residual_dt;
    }
  }
  return dst_dt;
}

template <typename T, typename K>
class ConvMKLDNNOpKernel : public paddle::framework::OpKernel<T> {
 public:
  void Compute(const paddle::framework::ExecutionContext& ctx) const override {
    PADDLE_ENFORCE(paddle::platform::is_cpu_place(ctx.GetPlace()),
                   "It must use CPUPlace.");
    bool is_INT8 =
        std::is_same<T, int8_t>::value || std::is_same<T, uint8_t>::value;
    if (!is_INT8) {
      ComputeFP32(ctx);
    } else {
      std::string fuse_activation = ctx.Attr<std::string>("fuse_activation");
      bool fuse_residual_conn = ctx.Attr<bool>("fuse_residual_connection");
      bool force_fp32_output = ctx.Attr<bool>("force_fp32_output");
      auto residual_param = ctx.Input<Tensor>("ResidualData");
      auto dst_dt = GetDstType(true, force_fp32_output, fuse_activation,
                               fuse_residual_conn, residual_param);
      if (dst_dt == mkldnn::memory::data_type::f32) {
        ComputeINT8<float>(ctx);
      } else if (dst_dt == mkldnn::memory::data_type::u8) {
        ComputeINT8<uint8_t>(ctx);
      } else if (dst_dt == mkldnn::memory::data_type::s8) {
        ComputeINT8<int8_t>(ctx);
      }
    }
  }

  void ComputeFP32(const paddle::framework::ExecutionContext& ctx) const {
    const bool is_test = ctx.Attr<bool>("is_test");

    auto& dev_ctx =
        ctx.template device_context<paddle::platform::MKLDNNDeviceContext>();
    const auto& mkldnn_engine = dev_ctx.GetEngine();

    auto* input = ctx.Input<Tensor>("Input");
    auto* filter = ctx.Input<Tensor>("Filter");
    auto* bias = ctx.HasInput("Bias") ? ctx.Input<Tensor>("Bias") : nullptr;
    auto* output = ctx.Output<Tensor>("Output");

    PADDLE_ENFORCE_EQ(input->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for Input tensor");
    PADDLE_ENFORCE_NE(input->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for Input tensor");

    PADDLE_ENFORCE_EQ(filter->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for Filter tensor");
    PADDLE_ENFORCE_NE(filter->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for Filter tensor");

    PADDLE_ENFORCE_GE(
        input->dims().size(), 4,
        "Input must be with 4 or 5 dimensions, i.e. NCHW or NCDHW");
    PADDLE_ENFORCE_LE(
        input->dims().size(), 5,
        "Input must be with 4 or 5 dimensions, i.e. NCHW or NCDHW");

    PADDLE_ENFORCE_GE(
        filter->dims().size(), 4,
        "Filter must be with 4 or 5 dimensions, i.e. OIHW or OIDHW");
    PADDLE_ENFORCE_LE(
        filter->dims().size(), 5,
        "Filter must be with 4 or 5 dimensions, i.e. OIHW or OIDHW");

    if (bias) {
      PADDLE_ENFORCE_EQ(bias->layout(), DataLayout::kMKLDNN,
                        "Wrong layout set for Bias tensor");
      PADDLE_ENFORCE_NE(bias->format(), MKLDNNMemoryFormat::format_undef,
                        "Wrong format set for Bias tensor");

      PADDLE_ENFORCE_EQ(bias->dims().size(), 1,
                        "Bias must only have 1 dimension, i.e. X");
    }

    std::vector<int> strides = ctx.Attr<std::vector<int>>("strides");
    std::vector<int> paddings = ctx.Attr<std::vector<int>>("paddings");
    std::vector<int> dilations = ctx.Attr<std::vector<int>>("dilations");
    std::string fuse_activation = ctx.Attr<std::string>("fuse_activation");
    float fuse_alpha = ctx.Attr<float>("fuse_alpha");
    float fuse_beta = ctx.Attr<float>("fuse_beta");
    bool fuse_residual_conn = ctx.Attr<bool>("fuse_residual_connection");
    int groups = ctx.Attr<int>("groups");
    bool is_conv3d = strides.size() == 3U;

    PADDLE_ENFORCE(
        is_conv3d
            ? dilations.size() == 3 && dilations[0] == 1 && dilations[1] == 1 &&
                  dilations[2] == 1
            : dilations.size() == 2 && dilations[0] == 1 && dilations[1] == 1,
        "dilation in convolution is not implemented yet");

    const T* input_data = input->data<T>();
    const T* filter_data = filter->data<T>();

    auto src_tz = paddle::framework::vectorize<int>(input->dims());
    auto weights_tz = paddle::framework::vectorize<int>(filter->dims());
    int g = std::max(groups, 1);
    GetWeightsTz(weights_tz, g, is_conv3d);
    auto dst_tz = paddle::framework::vectorize<int>(output->dims());

    // Get unique name for storing MKLDNN primitives
    const std::string key = platform::CreateKey(
        src_tz, weights_tz, fuse_activation, strides, paddings, dilations,
        groups, ctx.op().Input("Input") + ctx.op().Input("Filter"));

    std::vector<primitive> pipeline;

    auto src_format = input->format();
    MKLDNNMemoryFormat weights_format =
        GetWeightsFormat(filter->format(), g, is_conv3d);

    auto user_src_md = platform::MKLDNNMemDesc(
        {src_tz}, platform::MKLDNNGetDataType<T>(), src_format);
    auto user_weights_md = platform::MKLDNNMemDesc(
        {weights_tz}, platform::MKLDNNGetDataType<T>(), weights_format);

    /* create memory descriptor for convolution without specified format
     * ('any') which lets a primitive (convolution in this case) choose
     * the memory format preferred for best performance
     */
    std::string data_format = ctx.Attr<std::string>("data_format");
    auto chosen_memory_format =
        platform::data_format_to_memory_format(data_format);

    weights_format = MKLDNNMemoryFormat::any;
    // Check the format for user's special output
    if (chosen_memory_format != MKLDNNMemoryFormat::any) {
      if (is_conv3d) {
        chosen_memory_format =
            platform::MKLDNNFormatForSize(src_tz.size(), chosen_memory_format);
      }
    }

    auto src_md = platform::MKLDNNMemDesc(
        src_tz, platform::MKLDNNGetDataType<T>(), chosen_memory_format);
    auto weights_md = platform::MKLDNNMemDesc(
        weights_tz, platform::MKLDNNGetDataType<T>(), weights_format);
    std::vector<int> bias_tz;
    auto dst_md = platform::MKLDNNMemDesc(
        dst_tz, platform::MKLDNNGetDataType<T>(), chosen_memory_format);

    platform::ConvMKLDNNHandler handler(dev_ctx, mkldnn_engine, key);

    // create a conv primitive descriptor and save it for usage in backward
    std::shared_ptr<mkldnn::convolution_forward::primitive_desc> conv_pd;
    auto fwd_prop_kind = is_test ? mkldnn::prop_kind::forward_inference
                                 : mkldnn::prop_kind::forward_training;
    if (bias) {
      bias_tz = paddle::framework::vectorize<int>(bias->dims());
      auto bias_md = platform::MKLDNNMemDesc(
          bias_tz, platform::MKLDNNGetDataType<T>(), MKLDNNMemoryFormat::x);
      conv_pd = handler.AcquireConvolutionPrimitiveDescriptor(
          src_md, weights_md, bias_md, dst_md, strides, paddings, mkldnn_engine,
          fuse_activation, fuse_alpha, fuse_beta, fuse_residual_conn,
          fwd_prop_kind);
    } else {
      conv_pd = handler.AcquireConvolutionPrimitiveDescriptor(
          src_md, weights_md, boost::none, dst_md, strides, paddings,
          mkldnn_engine, fuse_activation, fuse_alpha, fuse_beta,
          fuse_residual_conn, fwd_prop_kind);
    }

    // create mkldnn memory from input tensors (data/weights)
    auto user_src_memory_p =
        handler.AcquireSrcMemory(user_src_md, to_void_cast<T>(input_data));
    auto user_weights_memory_p = handler.AcquireWeightsMemory(
        user_weights_md, to_void_cast<T>(filter_data));

    // create reorder primitive if the input format is not the preferred one
    auto src_memory_p =
        handler.AcquireSrcMemoryFromPrimitive(user_src_memory_p, pipeline);
    auto weights_memory_p = handler.AcquireWeightsMemoryFromPrimitive(
        user_weights_memory_p, pipeline, is_test);

    std::shared_ptr<mkldnn::memory> dst_memory_p, user_residual_memory_p;

    if (fuse_residual_conn) {
      auto residual_param = ctx.Input<Tensor>("ResidualData");
      auto residual_param_data = residual_param->data<T>();

      PADDLE_ENFORCE(
          residual_param_data != nullptr,
          "Provide data if you want MKLDNN conv+elementwise_add fusion");
      PADDLE_ENFORCE_EQ(output->dims(), residual_param->dims(),
                        "Output and elementwise parameter need to have the "
                        "same dimension sizes");

      if (residual_param->format() != handler.GetDstFormat()) {
        auto output_data =
            output->mutable_data<T>(ctx.GetPlace(), handler.GetDstMemorySize());
        auto residual_data_tz =
            paddle::framework::vectorize<int>(residual_param->dims());
        auto residual_data_type =
            paddle::framework::ToMKLDNNDataType(residual_param->type());

        auto user_residual_md = platform::MKLDNNMemDesc(
            residual_data_tz, residual_data_type, residual_param->format());
        user_residual_memory_p = handler.AcquireResidualDataMemory(
            user_residual_md, to_void_cast<T>(residual_param_data));

        dst_memory_p = handler.AcquireDstMemoryFromResidualDataMemory(
            user_residual_memory_p, to_void_cast<T>(output_data), pipeline);
      } else {
        output->ShareDataWith(*residual_param);
        auto output_data = output->mutable_data<T>(ctx.GetPlace());
        dst_memory_p =
            handler.AcquireDstMemoryFromPrimitive(to_void_cast<T>(output_data));
      }
    } else {
      auto output_data =
          output->mutable_data<T>(ctx.GetPlace(), handler.GetDstMemorySize());
      dst_memory_p =
          handler.AcquireDstMemoryFromPrimitive(to_void_cast<T>(output_data));
    }

    // create convolution op primitive
    std::shared_ptr<mkldnn::convolution_forward> conv_p;
    std::shared_ptr<mkldnn::memory> user_bias_memory_p, bias_memory_p;
    if (bias) {
      const T* bias_data = bias->data<T>();
      auto user_bias_md = platform::MKLDNNMemDesc(
          {bias_tz}, platform::MKLDNNGetDataType<T>(), MKLDNNMemoryFormat::x);
      user_bias_memory_p =
          handler.AcquireBiasMemory(user_bias_md, to_void_cast<T>(bias_data));

      bias_memory_p =
          handler.AcquireBiasMemoryFromPrimitive(user_bias_memory_p, pipeline);
      conv_p = handler.AcquireConvolution(src_memory_p, weights_memory_p,
                                          bias_memory_p, dst_memory_p);
    } else {
      conv_p = handler.AcquireConvolution(src_memory_p, weights_memory_p,
                                          dst_memory_p);
    }

    // push primitive to stream and wait until it's executed
    pipeline.push_back(*conv_p);
    stream(stream::kind::eager).submit(pipeline).wait();

    output->set_layout(DataLayout::kMKLDNN);
    output->set_format(GetMKLDNNFormat(*dst_memory_p));
  }
  template <typename T_out>
  void ComputeINT8(const paddle::framework::ExecutionContext& ctx) const {
    const bool is_test = ctx.Attr<bool>("is_test");

    auto& dev_ctx =
        ctx.template device_context<paddle::platform::MKLDNNDeviceContext>();
    const auto& mkldnn_engine = dev_ctx.GetEngine();

    auto* input = ctx.Input<Tensor>("Input");
    auto* filter = ctx.Input<Tensor>("Filter");
    auto* bias = ctx.HasInput("Bias") ? ctx.Input<Tensor>("Bias") : nullptr;
    auto* output = ctx.Output<Tensor>("Output");

    PADDLE_ENFORCE_EQ(input->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for Input tensor");
    PADDLE_ENFORCE_NE(input->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for Input tensor");

    PADDLE_ENFORCE_EQ(filter->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for Filter tensor");
    PADDLE_ENFORCE_NE(filter->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for Filter tensor");

    PADDLE_ENFORCE_GE(
        input->dims().size(), 4,
        "Input must be with 4 or 5 dimensions, i.e. NCHW or NCDHW");
    PADDLE_ENFORCE_LE(
        input->dims().size(), 5,
        "Input must be with 4 or 5 dimensions, i.e. NCHW or NCDHW");

    PADDLE_ENFORCE_GE(
        filter->dims().size(), 4,
        "Filter must be with 4 or 5 dimensions, i.e. OIHW or OIDHW");
    PADDLE_ENFORCE_LE(
        filter->dims().size(), 5,
        "Filter must be with 4 or 5 dimensions, i.e. OIHW or OIDHW");

    if (bias) {
      PADDLE_ENFORCE_EQ(bias->layout(), DataLayout::kMKLDNN,
                        "Wrong layout set for Bias tensor");
      PADDLE_ENFORCE_NE(bias->format(), MKLDNNMemoryFormat::format_undef,
                        "Wrong format set for Bias tensor");

      PADDLE_ENFORCE_EQ(bias->dims().size(), 1,
                        "Bias must only have 1 dimension, i.e. X");
    }

    std::vector<int> strides = ctx.Attr<std::vector<int>>("strides");
    std::vector<int> paddings = ctx.Attr<std::vector<int>>("paddings");
    std::vector<int> dilations = ctx.Attr<std::vector<int>>("dilations");
    int groups = ctx.Attr<int>("groups");
    std::string fuse_activation = ctx.Attr<std::string>("fuse_activation");
    float fuse_alpha = ctx.Attr<float>("fuse_alpha");
    float fuse_beta = ctx.Attr<float>("fuse_beta");
    bool fuse_residual_conn = ctx.Attr<bool>("fuse_residual_connection");
    bool force_fp32_output = ctx.Attr<bool>("force_fp32_output");
    bool unsigned_output =
        (fuse_activation == "relu" || fuse_activation == "relu6");

    PADDLE_ENFORCE(!fuse_residual_conn || !force_fp32_output,
                   "residual fusion does not support force output with fp32");

    bool is_conv3d = strides.size() == 3U;
    PADDLE_ENFORCE(
        is_conv3d
            ? dilations.size() == 3 && dilations[0] == 1 && dilations[1] == 1 &&
                  dilations[2] == 1
            : dilations.size() == 2 && dilations[0] == 1 && dilations[1] == 1,
        "dilation in convolution is not implemented yet");

    PADDLE_ENFORCE(is_conv3d != true, "int8 does not support conv3d currently");

    const T* input_data = input->data<T>();

    auto src_tz = paddle::framework::vectorize<int>(input->dims());
    auto weights_tz = paddle::framework::vectorize<int>(filter->dims());
    int g = std::max(groups, 1);

    GetWeightsTz(weights_tz, g, is_conv3d);
    auto dst_tz = paddle::framework::vectorize<int>(output->dims());

    mkldnn::memory::data_type src_dt =
        paddle::framework::ToMKLDNNDataType(input->type());

    // Get unique name for storing MKLDNN primitives
    const std::string key = platform::CreateKey(
        src_tz, weights_tz, strides, paddings, dilations, groups, src_dt,
        input->format(), fuse_activation, fuse_residual_conn,
        ctx.op().Input("Input") + ctx.op().Input("Filter"));

    const std::string key_conv_pd = key + "@conv_pd";

    bool need_s8_to_u8 = false;
    std::shared_ptr<mkldnn::convolution_forward> conv_p;
    std::shared_ptr<mkldnn::memory> src_memory_p;
    std::shared_ptr<mkldnn::memory> user_src_memory_p;
    std::shared_ptr<mkldnn::memory> dst_memory_p;
    std::vector<primitive> pipeline;
    std::shared_ptr<mkldnn::convolution_forward::primitive_desc> conv_pd;
    std::shared_ptr<platform::ConvMKLDNNHandler> handler;

    // This is workaround for hacky implementation
    // of conv int8 mkl-dnn. Once conv fp32 and conv int8
    // are merged/unified, this will disappear
    std::string key_tid = "";
    if (platform::get_cur_mkldnn_session_id() ==
        platform::kMKLDNNSessionID_Default) {
      key_tid = "-t:" + platform::ThreadIDasStr();
    }

    auto prim_key = key + key_tid + "@conv_p";
    auto dst_key = key + key_tid + "@dst_mem_p";
    auto src_key = key + key_tid + "@src_mem_p";
    auto user_src_key = key + key_tid + "@user_src_mem_p";
    auto src_reorder_key = key + key_tid + "@src_mem_preorder_p";
    auto residual_reorder_key = key + key_tid + "@residual_data_mem_preorder_p";

    conv_p = std::static_pointer_cast<mkldnn::convolution_forward>(
        dev_ctx.GetBlob(prim_key));

    if (conv_p == nullptr || !is_test) {
      const K* filter_data = filter->data<K>();
      auto scale_in_data = ctx.Attr<float>("Scale_in");
      auto scale_in_eltwise_data = ctx.Attr<float>("Scale_in_eltwise");
      auto scale_weights_data = ctx.Attr<std::vector<float>>("Scale_weights");
      auto scale_out_data =
          force_fp32_output ? 1.0f : ctx.Attr<float>("Scale_out");
      float sum_scale =
          fuse_residual_conn ? scale_out_data / scale_in_eltwise_data : 1.0f;

      bool is_multi_channel = scale_weights_data.size() > 1;

      int count = is_multi_channel ? (g > 1 ? (weights_tz)[1] * (weights_tz)[0]
                                            : (weights_tz)[0])
                                   : 1;
      std::vector<float> output_shift_scale(count);
#pragma omp parallel for if (count > 1)
      for (int i = 0; i < count; i++) {
        if (scale_weights_data[i] == 0.0)
          output_shift_scale[i] =
              scale_out_data;  // weights data will contain 0
                               // in some models, then weights
                               // scale couldn't be calculated
        else
          output_shift_scale[i] =
              static_cast<float>(static_cast<double>(scale_out_data) /
                                 (static_cast<double>(scale_in_data) *
                                  static_cast<double>(scale_weights_data[i])));
      }

      auto user_src_md =
          platform::MKLDNNMemDesc({src_tz}, src_dt, input->format());
      auto user_weights_md = platform::MKLDNNMemDesc(
          {weights_tz}, platform::MKLDNNGetDataType<K>(),
          ((g) == 1) ? MKLDNNMemoryFormat::oihw : MKLDNNMemoryFormat::goihw);

      /* create memory descriptor for convolution without specified format
      * ('any') which lets a primitive (convolution in this case) choose
      * the memory format preferred for best performance
      */
      std::string data_format = ctx.Attr<std::string>("data_format");
      auto chosen_memory_format =
          platform::data_format_to_memory_format(data_format);

      std::vector<int> bias_tz;

      auto src_md =
          platform::MKLDNNMemDesc(src_tz, src_dt, chosen_memory_format);
      auto weights_md = platform::MKLDNNMemDesc(
          weights_tz, memory::data_type::s8, chosen_memory_format);
      auto dst_md = platform::MKLDNNMemDesc(
          dst_tz, platform::MKLDNNGetDataType<T_out>(), chosen_memory_format);

      handler.reset(
          new platform::ConvMKLDNNHandler(dev_ctx, mkldnn_engine, key));
      // create a conv primitive descriptor and save it for usage in backward
      auto propagation = is_test ? mkldnn::prop_kind::forward_scoring
                                 : mkldnn::prop_kind::forward_training;

      if (bias) {
        bias_tz = paddle::framework::vectorize<int>(bias->dims());
        auto bias_md = platform::MKLDNNMemDesc(bias_tz, memory::data_type::s32,
                                               MKLDNNMemoryFormat::x);
        conv_pd = handler->AcquireConvolutionPrimitiveDescriptor(
            src_md, weights_md, bias_md, dst_md, strides, paddings,
            mkldnn_engine, fuse_activation, fuse_alpha, fuse_beta,
            fuse_residual_conn, propagation, output_shift_scale, sum_scale);
      } else {
        conv_pd = handler->AcquireConvolutionPrimitiveDescriptor(
            src_md, weights_md, boost::none, dst_md, strides, paddings,
            mkldnn_engine, fuse_activation, fuse_alpha, fuse_beta,
            fuse_residual_conn, propagation, output_shift_scale, sum_scale);
      }

      // create mkldnn memory from input tensors (data/weights)
      user_src_memory_p =
          handler->AcquireSrcMemory(user_src_md, to_void_cast<T>(input_data));
      auto user_weights_memory_p = handler->AcquireWeightsMemory(
          user_weights_md, to_void_cast<K>(filter_data));

      // create reorder primitive if the input format is not the preferred one
      src_memory_p =
          handler->AcquireSrcMemoryFromPrimitive(user_src_memory_p, pipeline);

      std::shared_ptr<mkldnn::memory> weights_memory_p;
      int mask_reorder =
          is_multi_channel ? ((g != 1) ? (1 << 1) + (1 << 0) : 1 << 0) : 0;
      weights_memory_p = handler->AcquireWeightsMemoryFromPrimitive(
          user_weights_memory_p, pipeline, is_test, true, scale_weights_data,
          mask_reorder);

      if (fuse_residual_conn) {
        auto residual_param = ctx.Input<Tensor>("ResidualData");
        PADDLE_ENFORCE_EQ(output->dims(), residual_param->dims(),
                          "Output and elementwise parameter need to have the "
                          "same dimension sizes");
        auto residual_dt =
            paddle::framework::ToMKLDNNDataType(residual_param->type());
        if (residual_param->format() != handler->GetDstFormat()) {
          auto residual_data_tz =
              paddle::framework::vectorize<int>(residual_param->dims());
          auto user_residual_md = platform::MKLDNNMemDesc(
              residual_data_tz, residual_dt, residual_param->format());
          dst_memory_p = platform::SetDstMemory<T_out>(
              ctx, output, residual_param, user_residual_md, handler,
              &pipeline);
        } else {
          output->ShareDataWith(*residual_param);
          dst_memory_p = platform::SetDstMemory<T_out>(ctx, output, handler);
        }
        need_s8_to_u8 =
            (platform::MKLDNNGetDataType<T_out>() == memory::data_type::s8) &&
            unsigned_output;
      } else {
        dst_memory_p = platform::SetDstMemory<T_out>(ctx, output, handler);
      }

      // create convolution op primitive
      auto scale_bias_key = key + "@scale_bias";
      if (bias) {
        const K* bias_data = bias->data<K>();
        auto user_bias_md = platform::MKLDNNMemDesc(
            {bias_tz}, platform::MKLDNNGetDataType<K>(), MKLDNNMemoryFormat::x);
        auto user_bias_memory_p = handler->AcquireBiasMemory(
            user_bias_md, to_void_cast<K>(bias_data));
        std::shared_ptr<mkldnn::memory> bias_memory_p;
        int mask_reorder = is_multi_channel ? 1 << 0 : 1;
        int count =
            is_multi_channel
                ? (g > 1 ? (weights_tz)[1] * (weights_tz)[0] : (weights_tz)[0])
                : 1;
        std::vector<float> scale_bias_data(count);
#pragma omp parallel for if (count > 1)
        for (int i = 0; i < count; i++) {
          scale_bias_data[i] = scale_in_data * scale_weights_data[i];
        }
        bias_memory_p = handler->AcquireBiasMemoryFromPrimitive(
            user_bias_memory_p, pipeline, is_test, true, scale_bias_data,
            mask_reorder);
        conv_p = handler->AcquireConvolution(src_memory_p, weights_memory_p,
                                             bias_memory_p, dst_memory_p);
      } else {
        conv_p = handler->AcquireConvolution(src_memory_p, weights_memory_p,
                                             dst_memory_p);
      }
      // push primitive to stream and wait until it's executed
      pipeline.push_back(*conv_p);
    } else {
      auto src_memory_reorder_p = std::static_pointer_cast<mkldnn::memory>(
          dev_ctx.GetBlob(src_reorder_key));
      src_memory_p =
          std::static_pointer_cast<mkldnn::memory>(dev_ctx.GetBlob(src_key));
      if (src_memory_reorder_p) {
        user_src_memory_p = std::static_pointer_cast<mkldnn::memory>(
            dev_ctx.GetBlob(user_src_key));
        user_src_memory_p->set_data_handle(to_void_cast<T>(input_data));
      } else if (src_memory_p) {
        src_memory_p->set_data_handle(to_void_cast<T>(input_data));
      }

      dst_memory_p =
          std::static_pointer_cast<mkldnn::memory>(dev_ctx.GetBlob(dst_key));
      conv_pd =
          std::static_pointer_cast<mkldnn::convolution_forward::primitive_desc>(
              dev_ctx.GetBlob(key_conv_pd));
      if (conv_pd) {
        handler.reset(new platform::ConvMKLDNNHandler(conv_pd, dev_ctx,
                                                      mkldnn_engine, key));
      }

      if (fuse_residual_conn) {
        auto residual_param = ctx.Input<Tensor>("ResidualData");
        output->ShareDataWith(*residual_param);
        need_s8_to_u8 =
            (platform::MKLDNNGetDataType<T_out>() == memory::data_type::s8) &&
            unsigned_output;
      }
      platform::SetDstMemoryHandler<T_out>(ctx, output, handler, dst_memory_p);

      if (src_memory_reorder_p) {
        pipeline.push_back(*src_memory_reorder_p);
      }

      auto residual_reorder_p = std::static_pointer_cast<mkldnn::memory>(
          dev_ctx.GetBlob(residual_reorder_key));
      if (residual_reorder_p) {
        pipeline.push_back(*residual_reorder_p);
      }
      pipeline.push_back(*conv_p);
    }
    // push primitive to stream and wait until it's executed
    stream(stream::kind::eager).submit(pipeline).wait();
    if (need_s8_to_u8) {
      output->mutable_data<uint8_t>(ctx.GetPlace());
    }
    output->set_layout(DataLayout::kMKLDNN);
    output->set_format(GetMKLDNNFormat(*dst_memory_p));
  }
};

template <typename T>
class ConvMKLDNNGradOpKernel : public paddle::framework::OpKernel<T> {
 public:
  void Compute(const paddle::framework::ExecutionContext& ctx) const override {
    PADDLE_ENFORCE(paddle::platform::is_cpu_place(ctx.GetPlace()),
                   "It must use CPUPlace.");

    auto& dev_ctx =
        ctx.template device_context<platform::MKLDNNDeviceContext>();
    const auto& mkldnn_engine = dev_ctx.GetEngine();

    const Tensor* input = ctx.Input<Tensor>("Input");
    const Tensor* filter = ctx.Input<Tensor>("Filter");
    const Tensor* output_grad =
        ctx.Input<Tensor>(framework::GradVarName("Output"));
    Tensor* input_grad = ctx.Output<Tensor>(framework::GradVarName("Input"));
    Tensor* filter_grad = ctx.Output<Tensor>(framework::GradVarName("Filter"));

    PADDLE_ENFORCE_EQ(input->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for Input tensor");
    PADDLE_ENFORCE_NE(input->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for Input tensor");

    PADDLE_ENFORCE_EQ(filter->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for Filter tensor");
    PADDLE_ENFORCE_NE(filter->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for Filter tensor");

    PADDLE_ENFORCE_EQ(output_grad->layout(), DataLayout::kMKLDNN,
                      "Wrong layout set for output_grad tensor");
    PADDLE_ENFORCE_NE(output_grad->format(), MKLDNNMemoryFormat::format_undef,
                      "Wrong format set for output_grad tensor");

    PADDLE_ENFORCE_EQ(
        ctx.Attr<bool>("is_test"), false,
        "is_test attribute should be set to False in training phase.");

    if (!input_grad && !filter_grad) return;

    std::vector<int> strides = ctx.Attr<std::vector<int>>("strides");
    std::vector<int> paddings = ctx.Attr<std::vector<int>>("paddings");
    std::vector<int> dilations = ctx.Attr<std::vector<int>>("dilations");
    int groups = ctx.Attr<int>("groups");

    bool is_conv3d = strides.size() == 3U;
    const T* input_data = input->data<T>();
    const T* filter_data = filter->data<T>();
    const T* output_grad_data = output_grad->data<T>();
    T* input_grad_data = nullptr;
    T* filter_grad_data = nullptr;

    auto src_tz = paddle::framework::vectorize<int>(input->dims());
    auto weights_tz = paddle::framework::vectorize<int>(filter->dims());
    int g = std::max(groups, 1);
    GetWeightsTz(weights_tz, g, is_conv3d);
    auto dst_tz = paddle::framework::vectorize<int>(output_grad->dims());
    auto src_format = input->format();
    MKLDNNMemoryFormat weights_format =
        GetWeightsFormat(filter->format(), g, is_conv3d);

    // Get an unique name from "argument" name of "input" and "Filter" variable
    // as well as attributes of primitive to be created
    // This name will be used as key when saving info into device context
    const std::string key = platform::CreateKey(
        src_tz, weights_tz, "", strides, paddings, dilations, groups,
        ctx.op().Input("Input") + ctx.op().Input("Filter"));

    const std::string key_conv_pd = key + "@conv_pd";
    std::vector<primitive> pipeline;

    // Create user memory descriptors
    auto user_src_md = platform::MKLDNNMemDesc(
        {src_tz}, platform::MKLDNNGetDataType<T>(), src_format);
    auto user_weights_md = platform::MKLDNNMemDesc(
        {weights_tz}, platform::MKLDNNGetDataType<T>(), weights_format);
    auto user_diff_dst_md = platform::MKLDNNMemDesc(
        {dst_tz}, platform::MKLDNNGetDataType<T>(), output_grad->format());

    /* create memory descriptor for conv backward without specified format
     * ('any') which lets a primitive (conv backward in this case) choose
     * the memory format preferred for best performance
     */
    std::string data_format = ctx.Attr<std::string>("data_format");
    auto chosen_memory_format =
        platform::data_format_to_memory_format(data_format);

    weights_format = MKLDNNMemoryFormat::any;
    // Check the format for user's special output
    if (chosen_memory_format != MKLDNNMemoryFormat::any) {
      if (is_conv3d) {
        chosen_memory_format =
            platform::MKLDNNFormatForSize(src_tz.size(), chosen_memory_format);
      }
    }

    auto src_md = platform::MKLDNNMemDesc(
        src_tz, platform::MKLDNNGetDataType<T>(), chosen_memory_format);
    auto diff_src_md = platform::MKLDNNMemDesc(
        src_tz, platform::MKLDNNGetDataType<T>(), chosen_memory_format);
    auto weights_md = platform::MKLDNNMemDesc(
        weights_tz, platform::MKLDNNGetDataType<T>(), weights_format);
    auto diff_weights_md = platform::MKLDNNMemDesc(
        weights_tz, platform::MKLDNNGetDataType<T>(), weights_format);
    auto diff_dst_md = platform::MKLDNNMemDesc(
        dst_tz, platform::MKLDNNGetDataType<T>(), chosen_memory_format);

    // Retrieve conv_pd from device context
    auto conv_pd =
        std::static_pointer_cast<mkldnn::convolution_forward::primitive_desc>(
            dev_ctx.GetBlob(key_conv_pd));
    PADDLE_ENFORCE(conv_pd != nullptr,
                   "Fail to find conv_pd in device context");

    // create backward convolution weights primitive descriptor
    auto conv_bwd_weights_desc = mkldnn::convolution_backward_weights::desc(
        mkldnn::convolution_direct, src_md, diff_weights_md, diff_dst_md,
        strides, paddings, paddings, mkldnn::padding_kind::zero);
    auto conv_bwd_weights_pd =
        std::make_shared<mkldnn::convolution_backward_weights::primitive_desc>(
            conv_bwd_weights_desc, mkldnn_engine, *conv_pd);

    // create backward convolution data primitive descriptor
    auto conv_bwd_data_desc = mkldnn::convolution_backward_data::desc(
        mkldnn::convolution_direct, diff_src_md, weights_md, diff_dst_md,
        strides, paddings, paddings, mkldnn::padding_kind::zero);
    auto conv_bwd_data_pd =
        std::make_shared<mkldnn::convolution_backward_data::primitive_desc>(
            conv_bwd_data_desc, mkldnn_engine, *conv_pd);

    platform::ConvMKLDNNHandler handler(conv_pd, conv_bwd_data_pd,
                                        conv_bwd_weights_pd, dev_ctx,
                                        mkldnn_engine, key);

    // create mkldnn memory from input tensors (data/weights)
    auto user_src_memory_p =
        handler.AcquireSrcMemory(user_src_md, to_void_cast<T>(input_data));
    auto user_weights_memory_p = handler.AcquireWeightsMemory(
        user_weights_md, to_void_cast<T>(filter_data));
    auto user_diff_dst_memory_p = handler.AcquireDiffDstMemory(
        user_diff_dst_md, to_void_cast<T>(output_grad_data));

    // create backward conv primitive for weights
    if (filter_grad) {
      auto src_memory_p = handler.AcquireSrcMemoryFromWeightsPrimitive(
          user_src_memory_p, pipeline);

      auto diff_dst_memory_4filter_p =
          handler.AcquireDiffDstMemoryFromWeightsPrimitive(
              user_diff_dst_memory_p, pipeline);

      const size_t size = handler.GetDiffWeightsMemorySize();
      filter_grad_data = filter_grad->mutable_data<T>(ctx.GetPlace(), size);

      auto diff_weights_memory_p =
          handler.AcquireDiffWeightsMemoryFromWeightsPrimitive(
              reinterpret_cast<void*>(filter_grad_data));

      auto conv_bwd_weights_p = handler.AcquireConvolutionBackwardWeights(
          src_memory_p, diff_dst_memory_4filter_p, diff_weights_memory_p);

      // push primitive to stream and wait until it's executed
      pipeline.push_back(*conv_bwd_weights_p);

      filter_grad->set_layout(DataLayout::kMKLDNN);
      filter_grad->set_format(GetMKLDNNFormat(*diff_weights_memory_p));
    }

    if (input_grad) {
      auto weights_memory_p = handler.AcquireWeightsMemoryFromDataPrimitive(
          user_weights_memory_p, pipeline);

      auto diff_dst_memory_4data_p =
          handler.AcquireDiffDstMemoryFromDataPrimitive(user_diff_dst_memory_p,
                                                        pipeline);

      const size_t size = handler.GetDiffSourceMemorySize();
      input_grad_data = input_grad->mutable_data<T>(ctx.GetPlace(), size);

      auto diff_src_memory_p = handler.AcquireDiffSrcMemoryFromDataPrimitive(
          reinterpret_cast<void*>(input_grad_data));

      auto conv_bwd_data_p = handler.AcquireConvolutionBackwardData(
          diff_dst_memory_4data_p, weights_memory_p, diff_src_memory_p);

      pipeline.push_back(*conv_bwd_data_p);

      input_grad->set_layout(DataLayout::kMKLDNN);
      input_grad->set_format(GetMKLDNNFormat(*diff_src_memory_p));
    }
    stream(stream::kind::eager).submit(pipeline).wait();
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;

REGISTER_OP_KERNEL_WITH_CUSTOM_TYPE(conv2d, MKLDNN,
                                    ::paddle::platform::CPUPlace, FP32,
                                    ops::kConvMKLDNNFP32,
                                    ops::ConvMKLDNNOpKernel<float, float>);

REGISTER_OP_KERNEL_WITH_CUSTOM_TYPE(conv2d, MKLDNN,
                                    ::paddle::platform::CPUPlace, U8,
                                    ops::kConvMKLDNNINT8,
                                    ops::ConvMKLDNNOpKernel<uint8_t, float>);

REGISTER_OP_KERNEL_WITH_CUSTOM_TYPE(conv2d, MKLDNN,
                                    ::paddle::platform::CPUPlace, S8,
                                    ops::kConvMKLDNNINT8,
                                    ops::ConvMKLDNNOpKernel<int8_t, float>);

REGISTER_OP_KERNEL_WITH_CUSTOM_TYPE(conv2d_grad, MKLDNN,
                                    ::paddle::platform::CPUPlace, FP32,
                                    ops::kConvMKLDNNFP32,
                                    ops::ConvMKLDNNGradOpKernel<float>);

REGISTER_OP_KERNEL_WITH_CUSTOM_TYPE(conv3d, MKLDNN,
                                    ::paddle::platform::CPUPlace, FP32,
                                    ops::kConvMKLDNNFP32,
                                    ops::ConvMKLDNNOpKernel<float, float>);

REGISTER_OP_KERNEL_WITH_CUSTOM_TYPE(conv3d_grad, MKLDNN,
                                    ::paddle::platform::CPUPlace, FP32,
                                    ops::kConvMKLDNNFP32,
                                    ops::ConvMKLDNNGradOpKernel<float>);
