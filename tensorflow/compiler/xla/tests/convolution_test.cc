/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

// Tests of 2+D convolution with trivial kernels and no special variations (like
// strides and padding).

#include <memory>

#include "absl/strings/str_cat.h"
#include "tensorflow/compiler/xla/array2d.h"
#include "tensorflow/compiler/xla/array4d.h"
#include "tensorflow/compiler/xla/client/global_data.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/client/padding.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/reference_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/tests/client_library_test_base.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/test.h"

namespace xla {
namespace {

class ConvolutionTest : public ClientLibraryTestBase {
 protected:
#if XLA_TEST_BACKEND_GPU
  // XLA:GPU sometimes uses FFT convolution which isn't as precise as spatial
  // convolution. So relax the absolute error threshold.
  ErrorSpec error_spec_ = ErrorSpec(1e-2, 1e-3);
#else
  ErrorSpec error_spec_ = ErrorSpec(1e-4, 1e-3);
#endif
};

using TestTypes = ::testing::Types<
// TODO(b/183565702): Support integer convs on GPU.
#if !XLA_TEST_BACKEND_GPU
    int32_t,
#endif
#ifndef XLA_BACKEND_DOES_NOT_SUPPORT_FLOAT16
    Eigen::half,
#endif
    float>;

template <typename T>
class ForwardPassConvolution_3x3x256_256_OutputZ_Iota : public ConvolutionTest {
 public:
  void RunTest() {
    const int kInputActivationSizeY = 3;
    const int kInputActivationSizeX = 3;
    const int kInputActivationSizeZ = 256;
    const int kKernelSizeX = 2;
    const int kKernelSizeY = 2;
    const int kOutputActivationSizeZ = 256;
    const int kMiniBatchSize = 4;
    auto alhs = std::make_unique<Array4D<T>>(
        kMiniBatchSize, kInputActivationSizeZ, kInputActivationSizeY,
        kInputActivationSizeX);
    alhs->FillWithMultiples(static_cast<T>(static_cast<T>(1.0f)));
    ASSERT_EQ(3, alhs->width());
    ASSERT_EQ(3, alhs->height());

    auto arhs = std::make_unique<Array4D<T>>(kOutputActivationSizeZ,
                                             kInputActivationSizeZ,
                                             kKernelSizeY, kKernelSizeX);
    Array2D<T> rhs_raster({
        {static_cast<T>(1.0f), static_cast<T>(0.0f)},  // row 0
        {static_cast<T>(0.0f), static_cast<T>(0.0f)},  // row 1
    });
    arhs->FillWithYX(rhs_raster);
    ASSERT_EQ(2, arhs->width());
    ASSERT_EQ(2, arhs->height());

    XlaBuilder builder(TestName());
    auto lhs = ConstantR4FromArray4D<T>(&builder, *alhs);
    auto rhs = ConstantR4FromArray4D<T>(&builder, *arhs);
    PrecisionConfig precision;
    // The left hand side of the convolution is numbers between 0 and 2304 which
    // requires at least 11 mantissa bits and the DEFAULT precision config is
    // allowed to round to bfloat16 which only has 7 mantissa bits.
    precision.add_operand_precision(PrecisionConfig::HIGHEST);
    precision.add_operand_precision(PrecisionConfig::DEFAULT);
    Conv(lhs, rhs, {1, 1}, Padding::kValid, /*feature_group_count=*/1,
         /*batch_group_count=*/1, &precision);

    ComputeAndCompare(&builder, {}, error_spec_);
  }
};

TYPED_TEST_CASE(ForwardPassConvolution_3x3x256_256_OutputZ_Iota, TestTypes);
XLA_TYPED_TEST(ForwardPassConvolution_3x3x256_256_OutputZ_Iota, Types) {
  this->RunTest();
}

template <typename T>
class Convolve_1x1x1x2_1x1x1x2_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 1, 2});
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 1, 2});
    auto input = Parameter(&builder, 0, input_shape, "input");
    auto filter = Parameter(&builder, 1, filter_shape, "filter");
    Conv(input, filter, {1, 1}, Padding::kValid);

    Array4D<T> input_data(1, 1, 1, 2);
    input_data.FillWithYX(Array2D<T>({
        {static_cast<T>(1.0f), static_cast<T>(2.0f)},
    }));
    Array4D<T> filter_data(1, 1, 1, 2);
    filter_data.FillWithYX(Array2D<T>({
        {static_cast<T>(5.0f), static_cast<T>(6.0f)},
    }));

    ComputeAndCompare(&builder,
                      {LiteralUtil::CreateFromArray(input_data),
                       LiteralUtil::CreateFromArray(filter_data)},
                      error_spec_);
  }
};

TYPED_TEST_CASE(Convolve_1x1x1x2_1x1x1x2_Valid, TestTypes);
TYPED_TEST(Convolve_1x1x1x2_1x1x1x2_Valid, Types) { this->RunTest(); }

// Tests valid padding for 2D convolution in raster space.
template <typename T>
class Convolve_1x1x4x4_1x1x2x2_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 4, 4});
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 2, 2});
    auto input = Parameter(&builder, 0, input_shape, "input");
    auto filter = Parameter(&builder, 1, filter_shape, "filter");
    Conv(input, filter, {1, 1}, Padding::kValid);

    Array4D<T> input_data(1, 1, 4, 4);
    input_data.FillWithYX(Array2D<T>({
        {static_cast<T>(1.0f), static_cast<T>(2.0f), static_cast<T>(3.0f),
         static_cast<T>(4.0f)},
        {static_cast<T>(5.0f), static_cast<T>(6.0f), static_cast<T>(7.0f),
         static_cast<T>(8.0f)},
        {static_cast<T>(9.0f), static_cast<T>(10.0f), static_cast<T>(11.0f),
         static_cast<T>(12.0f)},
        {static_cast<T>(13.0f), static_cast<T>(14.0f), static_cast<T>(15.0f),
         static_cast<T>(16.0f)},
    }));
    Array4D<T> filter_data(1, 1, 2, 2);
    filter_data.FillWithYX(Array2D<T>({
        {static_cast<T>(5.0f), static_cast<T>(6.0f)},
        {static_cast<T>(7.0f), static_cast<T>(8.0f)},
    }));
    ComputeAndCompare(&builder,
                      {LiteralUtil::CreateFromArray(input_data),
                       LiteralUtil::CreateFromArray(filter_data)},
                      error_spec_);
  }
};

TYPED_TEST_CASE(Convolve_1x1x4x4_1x1x2x2_Valid, TestTypes);
TYPED_TEST(Convolve_1x1x4x4_1x1x2x2_Valid, Types) { this->RunTest(); }

// Tests same padding for 2D convolution in raster space.
template <typename T>
class Convolve_1x1x4x4_1x1x2x2_Same : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 4, 4});
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 2, 2});
    auto input = Parameter(&builder, 0, input_shape, "input");
    auto filter = Parameter(&builder, 1, filter_shape, "filter");
    Conv(input, filter, {1, 1}, Padding::kSame);

    Array4D<T> input_data(1, 1, 4, 4);
    input_data.FillWithYX(Array2D<T>({
        {static_cast<T>(1.0f), static_cast<T>(2.0f), static_cast<T>(3.0f),
         static_cast<T>(4.0f)},
        {static_cast<T>(5.0f), static_cast<T>(6.0f), static_cast<T>(7.0f),
         static_cast<T>(8.0f)},
        {static_cast<T>(9.0f), static_cast<T>(10.0f), static_cast<T>(11.0f),
         static_cast<T>(12.0f)},
        {static_cast<T>(13.0f), static_cast<T>(14.0f), static_cast<T>(15.0f),
         static_cast<T>(16.0f)},
    }));
    Array4D<T> filter_data(1, 1, 2, 2);
    filter_data.FillWithYX(Array2D<T>({
        {static_cast<T>(5.0f), static_cast<T>(6.0f)},
        {static_cast<T>(7.0f), static_cast<T>(8.0f)},
    }));

    ComputeAndCompare(&builder,
                      {LiteralUtil::CreateFromArray(input_data),
                       LiteralUtil::CreateFromArray(filter_data)},
                      error_spec_);
  }
};

TYPED_TEST_CASE(Convolve_1x1x4x4_1x1x2x2_Same, TestTypes);
TYPED_TEST(Convolve_1x1x4x4_1x1x2x2_Same, Types) { this->RunTest(); }

// Tests same padding for 2D convolution in raster space with an odd sized
// kernel.
template <typename T>
class Convolve_1x1x4x4_1x1x3x3_Same : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 4, 4});
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>({1, 1, 3, 3});
    auto input = Parameter(&builder, 0, input_shape, "input");
    auto filter = Parameter(&builder, 1, filter_shape, "filter");
    Conv(input, filter, {1, 1}, Padding::kSame);

    Array4D<T> input_data(1, 1, 4, 4);
    input_data.FillWithYX(
        Array2D<T>({{static_cast<T>(1.0f), static_cast<T>(2.0f),
                     static_cast<T>(3.0f), static_cast<T>(4.0f)},
                    {static_cast<T>(5.0f), static_cast<T>(6.0f),
                     static_cast<T>(7.0f), static_cast<T>(8.0f)},
                    {static_cast<T>(9.0f), static_cast<T>(10.0f),
                     static_cast<T>(11.0f), static_cast<T>(12.0f)},
                    {static_cast<T>(13.0f), static_cast<T>(14.0f),
                     static_cast<T>(15.0f), static_cast<T>(16.0f)}}));
    Array4D<T> filter_data(1, 1, 3, 3);
    filter_data.FillWithYX(Array2D<T>(
        {{static_cast<T>(5.0f), static_cast<T>(6.0f), static_cast<T>(7.0f)},
         {static_cast<T>(8.0f), static_cast<T>(9.0f), static_cast<T>(10.0f)},
         {static_cast<T>(11.0f), static_cast<T>(12.0f),
          static_cast<T>(13.0f)}}));
    // clang-format on
    ComputeAndCompare(&builder,
                      {LiteralUtil::CreateFromArray(input_data),
                       LiteralUtil::CreateFromArray(filter_data)},
                      error_spec_);
  }
};

TYPED_TEST_CASE(Convolve_1x1x4x4_1x1x3x3_Same, TestTypes);
TYPED_TEST(Convolve_1x1x4x4_1x1x3x3_Same, Types) { this->RunTest(); }

XLA_TEST_F(ConvolutionTest, Convolve3D_1x4x2x3x3_2x2x2x3x3_Valid) {
  XlaBuilder builder(TestName());
  std::vector<int64_t> input_dims = {1, 4, 2, 3, 3};
  std::vector<int64_t> filter_dims = {2, 2, 2, 3, 3};
  Shape input_shape = ShapeUtil::MakeShape(F32, input_dims);
  Shape filter_shape = ShapeUtil::MakeShape(F32, filter_dims);
  {
    auto input = Parameter(&builder, 0, input_shape, "input");
    auto filter = Parameter(&builder, 1, filter_shape, "filter");

    // Tensorflow dimension numbers for 3D convolution.
    ConvolutionDimensionNumbers dnums;
    dnums.set_input_batch_dimension(0);
    dnums.set_output_batch_dimension(0);
    dnums.add_input_spatial_dimensions(1);
    dnums.add_output_spatial_dimensions(1);
    dnums.add_input_spatial_dimensions(2);
    dnums.add_output_spatial_dimensions(2);
    dnums.add_input_spatial_dimensions(3);
    dnums.add_output_spatial_dimensions(3);
    dnums.set_input_feature_dimension(4);
    dnums.set_output_feature_dimension(4);
    dnums.add_kernel_spatial_dimensions(0);
    dnums.add_kernel_spatial_dimensions(1);
    dnums.add_kernel_spatial_dimensions(2);
    dnums.set_kernel_input_feature_dimension(3);
    dnums.set_kernel_output_feature_dimension(4);

    ConvWithGeneralDimensions(input, filter, {1, 1, 1}, Padding::kValid, dnums);
  }

  std::vector<float> input_elems(ShapeUtil::ElementsIn(input_shape));
  iota(input_elems.begin(), input_elems.end(), 1.0f);
  auto input_r1 = LiteralUtil::CreateR1<float>(input_elems);
  auto input_r5 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

  std::vector<float> filter_elems(ShapeUtil::ElementsIn(filter_shape));
  iota(filter_elems.begin(), filter_elems.end(), 1.0f);
  auto filter_r1 = LiteralUtil::CreateR1<float>(filter_elems);
  auto filter_r5 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

  auto expected_r1 = LiteralUtil::CreateR1<float>(
      {19554, 19962, 20370, 22110, 22590, 23070, 34890, 35730, 36570, 37446,
       38358, 39270, 50226, 51498, 52770, 52782, 54126, 55470});
  auto expected_r5 = expected_r1.Reshape({1, 3, 1, 2, 3}).ConsumeValueOrDie();

  auto input_literal = client_->TransferToServer(input_r5).ConsumeValueOrDie();
  auto filter_literal =
      client_->TransferToServer(filter_r5).ConsumeValueOrDie();

  ComputeAndCompareLiteral(&builder, expected_r5,
                           {input_literal.get(), filter_literal.get()},
                           error_spec_);
}

// std::iota doesn't work when init_value has a type Eigen::half in some build
// servers. The error message is missing the operator ++.
template <typename T>
void iota_int_init_value(std::vector<T>& values, int init_value) {
  absl::c_for_each(values,
                   [&](T& value) { value = static_cast<T>(init_value++); });
}

template <typename T>
class Convolve2D_1x3x3x5_3x3x5x3_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 3, 3, 5};
    std::vector<int64_t> filter_dims = {3, 3, 5, 3};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 = LiteralUtil::CreateR1<T>(
        {static_cast<T>(92115), static_cast<T>(93150), static_cast<T>(94185)});
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 3}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x3x3x5_3x3x5x3_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x3x3x5_3x3x5x3_Valid, Types) { this->RunTest(); }

template <typename T>
class Convolve2D_1x3x3x5_3x3x1x15_Depthwise_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 3, 3, 5};
    std::vector<int64_t> filter_dims = {3, 3, 1, 15};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/5);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 = LiteralUtil::CreateR1<T>(
        {static_cast<T>(16029), static_cast<T>(16218), static_cast<T>(16407),
         static_cast<T>(17172), static_cast<T>(17370), static_cast<T>(17568),
         static_cast<T>(18369), static_cast<T>(18576), static_cast<T>(18783),
         static_cast<T>(19620), static_cast<T>(19836), static_cast<T>(20052),
         static_cast<T>(20925), static_cast<T>(21150), static_cast<T>(21375)});
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 15}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x3x3x5_3x3x1x15_Depthwise_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x3x3x5_3x3x1x15_Depthwise_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x5_3x3x1x5_Depthwise_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 5};
    std::vector<int64_t> filter_dims = {3, 3, 1, 5};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/5);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 = LiteralUtil::CreateR1<T>(
        {static_cast<T>(6864),  static_cast<T>(7296),  static_cast<T>(7746),
         static_cast<T>(8214),  static_cast<T>(8700),  static_cast<T>(7809),
         static_cast<T>(8286),  static_cast<T>(8781),  static_cast<T>(9294),
         static_cast<T>(9825),  static_cast<T>(10644), static_cast<T>(11256),
         static_cast<T>(11886), static_cast<T>(12534), static_cast<T>(13200),
         static_cast<T>(11589), static_cast<T>(12246), static_cast<T>(12921),
         static_cast<T>(13614), static_cast<T>(14325)});
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 5}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);

    auto filter_r = filter_r1.Reshape(filter_dims);
  }
};

TYPED_TEST_CASE(Convolve2D_1x4x4x5_3x3x1x5_Depthwise_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x4x4x5_3x3x1x5_Depthwise_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x512_3x3x1x512_Depthwise_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 512};
    std::vector<int64_t> filter_dims = {3, 3, 1, 512};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/512);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(2048, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 512}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x4x4x512_3x3x1x512_Depthwise_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x4x4x512_3x3x1x512_Depthwise_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x512_3x3x1x512_Depthwise_Valid_Output_Batch_In_Lanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 512};
    std::vector<int64_t> filter_dims = {3, 3, 1, 512};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/512);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(2048, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 512}).ConsumeValueOrDie();
    auto expected_r4_relaid =
        expected_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4_relaid,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_, &expected_r4_relaid.shape());
  }
};

TYPED_TEST_CASE(
    Convolve2D_1x4x4x512_3x3x1x512_Depthwise_Valid_Output_Batch_In_Lanes,
    TestTypes);
TYPED_TEST(Convolve2D_1x4x4x512_3x3x1x512_Depthwise_Valid_Output_Batch_In_Lanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_256x4x4x512_3x3x1x512_Depthwise_Input_Batch_in_Lanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {256, 4, 4, 512};
    std::vector<int64_t> filter_dims = {3, 3, 1, 512};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/512);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();
    auto input_r4_relaid =
        input_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(2048 * 256, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 =
        expected_r1.Reshape({256, 2, 2, 512}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4_relaid).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_256x4x4x512_3x3x1x512_Depthwise_Input_Batch_in_Lanes,
                TestTypes);
TYPED_TEST(Convolve2D_256x4x4x512_3x3x1x512_Depthwise_Input_Batch_in_Lanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_256x4x4x512_3x3x1x512_Depthwise_Both_Batch_in_Lanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {256, 4, 4, 512};
    std::vector<int64_t> filter_dims = {3, 3, 1, 512};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/512);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();
    auto input_r4_relaid =
        input_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(2048 * 256, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 =
        expected_r1.Reshape({256, 2, 2, 512}).ConsumeValueOrDie();
    auto expected_r4_relaid =
        expected_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    auto input_literal =
        client_->TransferToServer(input_r4_relaid).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4_relaid,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_, &expected_r4_relaid.shape());
  }
};

TYPED_TEST_CASE(Convolve2D_256x4x4x512_3x3x1x512_Depthwise_Both_Batch_in_Lanes,
                TestTypes);
TYPED_TEST(Convolve2D_256x4x4x512_3x3x1x512_Depthwise_Both_Batch_in_Lanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x5_3x3x1x5_Depthwise_Valid_Output_Batch_In_Lanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 5};
    std::vector<int64_t> filter_dims = {3, 3, 1, 5};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/5);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();
    auto input_r4_relaid =
        input_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 = LiteralUtil::CreateR1<T>(
        {static_cast<T>(6864),  static_cast<T>(7296),  static_cast<T>(7746),
         static_cast<T>(8214),  static_cast<T>(8700),  static_cast<T>(7809),
         static_cast<T>(8286),  static_cast<T>(8781),  static_cast<T>(9294),
         static_cast<T>(9825),  static_cast<T>(10644), static_cast<T>(11256),
         static_cast<T>(11886), static_cast<T>(12534), static_cast<T>(13200),
         static_cast<T>(11589), static_cast<T>(12246), static_cast<T>(12921),
         static_cast<T>(13614), static_cast<T>(14325)});
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 5}).ConsumeValueOrDie();
    auto expected_r4_relaid =
        expected_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    auto input_literal =
        client_->TransferToServer(input_r4_relaid).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4_relaid,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_, &expected_r4_relaid.shape());
  }
};

TYPED_TEST_CASE(
    Convolve2D_1x4x4x5_3x3x1x5_Depthwise_Valid_Output_Batch_In_Lanes,
    TestTypes);
TYPED_TEST(Convolve2D_1x4x4x5_3x3x1x5_Depthwise_Valid_Output_Batch_In_Lanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 160};
    std::vector<int64_t> filter_dims = {3, 3, 1, 160};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/160);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(640, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 160}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Input_Batch_In_Lanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 160};
    std::vector<int64_t> filter_dims = {3, 3, 1, 160};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/160);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();
    auto input_r4_relaid =
        input_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(640, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 160}).ConsumeValueOrDie();
    auto expected_r4_relaid =
        expected_r4.Relayout(LayoutUtil::MakeLayout({3, 0, 2, 1}));

    auto input_literal =
        client_->TransferToServer(input_r4_relaid).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4_relaid,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_, &expected_r4_relaid.shape());
  }
};

TYPED_TEST_CASE(Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Input_Batch_In_Lanes,
                TestTypes);
TYPED_TEST(Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Input_Batch_In_Lanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Both_Batch_In_Lanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 160};
    std::vector<int64_t> filter_dims = {3, 3, 1, 160};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/160);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();
    auto input_r4_relaid =
        input_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(640, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 160}).ConsumeValueOrDie();
    auto expected_r4_relaid =
        expected_r4.Relayout(LayoutUtil::MakeLayout({0, 3, 2, 1}));

    auto input_literal =
        client_->TransferToServer(input_r4_relaid).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4_relaid,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_, &expected_r4_relaid.shape());
  }
};

TYPED_TEST_CASE(Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Both_Batch_In_Lanes,
                TestTypes);
TYPED_TEST(Convolve2D_1x4x4x160_3x3x1x160_Depthwise_Both_Batch_In_Lanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x4x4x1024_3x3x1x1024_Depthwise_Valid
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 4, 4, 1024};
    std::vector<int64_t> filter_dims = {3, 3, 1, 1024};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/1024);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(4096, static_cast<T>(18));

    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 2, 2, 1024}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x4x4x1024_3x3x1x1024_Depthwise_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x4x4x1024_3x3x1x1024_Depthwise_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x2x2x6_2x2x2x12_Grouped_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 2, 2, 6};
    std::vector<int64_t> filter_dims = {2, 2, 2, 12};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/3);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 = LiteralUtil::CreateR1<T>(
        {static_cast<T>(5076), static_cast<T>(5160), static_cast<T>(5244),
         static_cast<T>(5328), static_cast<T>(6164), static_cast<T>(6264),
         static_cast<T>(6364), static_cast<T>(6464), static_cast<T>(7380),
         static_cast<T>(7496), static_cast<T>(7612), static_cast<T>(7728)});
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 12}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x2x2x6_2x2x2x12_Grouped_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x2x2x6_2x2x2x12_Grouped_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x2x2x1024_2x2x128x512_Grouped_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 2, 2, 1024};
    std::vector<int64_t> filter_dims = {2, 2, 128, 512};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/8);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));

    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));

    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(512, static_cast<T>(1024));
    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 512}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x2x2x1024_2x2x128x512_Grouped_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x2x2x1024_2x2x128x512_Grouped_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x2x2x1024_2x2x128x8_Grouped_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 2, 2, 1024};
    std::vector<int64_t> filter_dims = {2, 2, 128, 8};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/8);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape),
                               static_cast<T>(1));

    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape),
                                static_cast<T>(2));

    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    std::vector<T> output_elems(8, static_cast<T>(1024));
    auto expected_r1 = LiteralUtil::CreateR1<T>(output_elems);
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 8}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x2x2x1024_2x2x128x8_Grouped_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x2x2x1024_2x2x128x8_Grouped_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x2x2x12_2x2x3x4_Grouped_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 2, 2, 12};
    std::vector<int64_t> filter_dims = {2, 2, 3, 4};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/4);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 =
        LiteralUtil::CreateR1<T>({static_cast<T>(7712), static_cast<T>(8816),
                                  static_cast<T>(9992), static_cast<T>(11240)});
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 4}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x2x2x12_2x2x3x4_Grouped_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x2x2x12_2x2x3x4_Grouped_Valid, Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x2x2x12_2x2x3x4_Grouped_Valid_Filter_OF_In_Sublanes
    : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 2, 2, 12};
    std::vector<int64_t> filter_dims = {2, 2, 4, 3};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(3);
      dnums.set_kernel_output_feature_dimension(2);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/4);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();
    auto filter_r4_relaid =
        filter_r4.Relayout(LayoutUtil::MakeLayout({3, 2, 1, 0}));
    auto expected_r1 = LiteralUtil::CreateR1<T>(
        {static_cast<T>(6968), static_cast<T>(8516), static_cast<T>(10280),
         static_cast<T>(12260)});
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 4}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4_relaid).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x2x2x12_2x2x3x4_Grouped_Valid_Filter_OF_In_Sublanes,
                TestTypes);
TYPED_TEST(Convolve2D_1x2x2x12_2x2x3x4_Grouped_Valid_Filter_OF_In_Sublanes,
           Types) {
  this->RunTest();
}

template <typename T>
class Convolve2D_1x1x1x12_1x1x3x4_Grouped_Valid : public ConvolutionTest {
 public:
  void RunTest() {
    XlaBuilder builder(TestName());
    std::vector<int64_t> input_dims = {1, 1, 1, 12};
    std::vector<int64_t> filter_dims = {1, 1, 3, 4};
    Shape input_shape = ShapeUtil::MakeShapeWithType<T>(input_dims);
    Shape filter_shape = ShapeUtil::MakeShapeWithType<T>(filter_dims);
    {
      auto input = Parameter(&builder, 0, input_shape, "input");
      auto filter = Parameter(&builder, 1, filter_shape, "filter");

      // Tensorflow dimension numbers for 2D convolution.
      ConvolutionDimensionNumbers dnums;
      dnums.set_input_batch_dimension(0);
      dnums.set_output_batch_dimension(0);
      dnums.add_input_spatial_dimensions(1);
      dnums.add_output_spatial_dimensions(1);
      dnums.add_input_spatial_dimensions(2);
      dnums.add_output_spatial_dimensions(2);
      dnums.set_input_feature_dimension(3);
      dnums.set_output_feature_dimension(3);
      dnums.add_kernel_spatial_dimensions(0);
      dnums.add_kernel_spatial_dimensions(1);
      dnums.set_kernel_input_feature_dimension(2);
      dnums.set_kernel_output_feature_dimension(3);

      ConvWithGeneralDimensions(input, filter, {1, 1}, Padding::kValid, dnums,
                                /*feature_group_count=*/4);
    }

    std::vector<T> input_elems(ShapeUtil::ElementsIn(input_shape));
    iota_int_init_value(input_elems, 1);
    auto input_r1 = LiteralUtil::CreateR1<T>(input_elems);
    auto input_r4 = input_r1.Reshape(input_dims).ConsumeValueOrDie();

    std::vector<T> filter_elems(ShapeUtil::ElementsIn(filter_shape));
    iota_int_init_value(filter_elems, 1);
    auto filter_r1 = LiteralUtil::CreateR1<T>(filter_elems);
    auto filter_r4 = filter_r1.Reshape(filter_dims).ConsumeValueOrDie();

    auto expected_r1 =
        LiteralUtil::CreateR1<T>({static_cast<T>(38), static_cast<T>(98),
                                  static_cast<T>(176), static_cast<T>(272)});
    auto expected_r4 = expected_r1.Reshape({1, 1, 1, 4}).ConsumeValueOrDie();

    auto input_literal =
        client_->TransferToServer(input_r4).ConsumeValueOrDie();
    auto filter_literal =
        client_->TransferToServer(filter_r4).ConsumeValueOrDie();

    ComputeAndCompareLiteral(&builder, expected_r4,
                             {input_literal.get(), filter_literal.get()},
                             error_spec_);
  }
};

TYPED_TEST_CASE(Convolve2D_1x1x1x12_1x1x3x4_Grouped_Valid, TestTypes);
TYPED_TEST(Convolve2D_1x1x1x12_1x1x3x4_Grouped_Valid, Types) {
  this->RunTest();
}

// Test fixture to run convolution tests with and without convolution
// canonicalization enabled.
class ConvolveWithAndWithoutCanonicalization
    : public ConvolutionTest,
      public ::testing::WithParamInterface<bool> {};

XLA_TEST_P(ConvolveWithAndWithoutCanonicalization,
           DISABLED_ON_GPU(Convolve2D_NoSpatialDims)) {
  if (GetParam()) {
    execution_options_.mutable_debug_options()->add_xla_disable_hlo_passes(
        "convolution-canonicalization");
  }
  XlaBuilder builder(TestName());
  Shape input_shape = ShapeUtil::MakeShape(F32, {4, 29});
  Shape filter_shape = ShapeUtil::MakeShape(F32, {4, 10});

  auto input = Parameter(&builder, 0, input_shape, "input");
  auto filter = Parameter(&builder, 1, filter_shape, "filter");

  ConvolutionDimensionNumbers dnums;
  dnums.set_input_feature_dimension(0);
  dnums.set_input_batch_dimension(1);
  dnums.set_kernel_input_feature_dimension(0);
  dnums.set_kernel_output_feature_dimension(1);
  dnums.set_output_batch_dimension(0);
  dnums.set_output_feature_dimension(1);
  ConvWithGeneralDimensions(input, filter, {}, Padding::kValid, dnums);

  Array2D<float> param0(4, 29);
  param0.FillUnique();

  Array2D<float> param1(4, 10);
  param1.FillUnique();

  Array2D<float> expected_result(29, 10);
  expected_result.Fill(0);

  ComputeAndCompare(&builder,
                    {LiteralUtil::CreateFromArray(param0),
                     LiteralUtil::CreateFromArray(param1)},
                    error_spec_);
}

INSTANTIATE_TEST_CASE_P(ConvolveWithAndWithoutCanonicalization_Instantiation,
                        ConvolveWithAndWithoutCanonicalization,
                        ::testing::Values(true, false));

XLA_TEST_F(ConvolutionTest, Convolve_bf16_1x1x1x2_1x1x1x2_Valid) {
  XlaBuilder builder(TestName());
  Shape input_shape = ShapeUtil::MakeShape(BF16, {1, 1, 1, 2});
  Shape filter_shape = ShapeUtil::MakeShape(BF16, {1, 1, 1, 2});
  auto input = Parameter(&builder, 0, input_shape, "input");
  auto filter = Parameter(&builder, 1, filter_shape, "filter");
  Conv(input, filter, {1, 1}, Padding::kValid);

  Array4D<bfloat16> input_data(1, 1, 1, 2);
  input_data.FillWithYX(Array2D<bfloat16>({
      {bfloat16(1), bfloat16(2)},
  }));
  Array4D<bfloat16> filter_data(1, 1, 1, 2);
  filter_data.FillWithYX(Array2D<bfloat16>({
      {bfloat16(5), bfloat16(6)},
  }));

  ComputeAndCompare(&builder,
                    {LiteralUtil::CreateFromArray(input_data),
                     LiteralUtil::CreateFromArray(filter_data)},
                    error_spec_);
}

// Check that GPU convs still work if the CudnnAlgorithmPicker pass is disabled.
// (We run this test on all platforms, because, what the heck.)
XLA_TEST_F(ConvolutionTest, DISABLED_ON_GPU_ROCM(NoCudnnAlgorithmPicker)) {
  execution_options_.mutable_debug_options()->add_xla_disable_hlo_passes(
      "gpu-conv-algorithm-picker");

  XlaBuilder builder(TestName());
  Shape input_shape = ShapeUtil::MakeShape(F32, {1, 1, 1, 2});
  Shape filter_shape = ShapeUtil::MakeShape(F32, {1, 1, 1, 2});
  auto input = Parameter(&builder, 0, input_shape, "input");
  auto filter = Parameter(&builder, 1, filter_shape, "filter");
  Conv(input, filter, {1, 1}, Padding::kValid);

  Array4D<float> input_data(1, 1, 1, 2);
  input_data.FillIota(0);
  Array4D<float> filter_data(1, 1, 1, 2);
  filter_data.FillIota(10);

  ComputeAndCompare(&builder, {LiteralUtil::CreateFromArray(input_data),
                               LiteralUtil::CreateFromArray(filter_data)});
}

XLA_TEST_F(ConvolutionTest, ConvolveF32BackwardInputGroupedConvolution) {
  XlaBuilder builder(TestName());
  Shape input_shape = ShapeUtil::MakeShape(F32, {1, 64, 100, 100});
  Array4D<float> input_data(1, 64, 100, 100);
  input_data.FillRandom(/*stddev=*/0.023, 0.001, /*seed=*/45321);
  Shape filter_shape = ShapeUtil::MakeShape(F32, {7, 7, 1, 64});
  Array4D<float> filter_data(7, 7, 1, 64);
  filter_data.FillRandom(/*stddev=*/0.023, 0.001, /*seed=*/45320);
  auto input = Parameter(&builder, 0, input_shape, "input");
  auto filter = ConstantR4FromArray4D(&builder, filter_data);

  // Specify bf01_01io->bf01 as dimension numbers.
  ConvolutionDimensionNumbers dnums;
  // Input
  dnums.set_input_feature_dimension(1);
  dnums.set_input_batch_dimension(0);
  dnums.add_input_spatial_dimensions(2);
  dnums.add_input_spatial_dimensions(3);
  // Kernel
  dnums.set_kernel_input_feature_dimension(2);
  dnums.set_kernel_output_feature_dimension(3);
  dnums.add_kernel_spatial_dimensions(0);
  dnums.add_kernel_spatial_dimensions(1);
  // Output
  dnums.set_output_batch_dimension(0);
  dnums.set_output_feature_dimension(1);
  dnums.add_output_spatial_dimensions(2);
  dnums.add_output_spatial_dimensions(3);
  ConvGeneral(input, filter, /*window_strides=*/{1, 1},
              /*padding=*/{{3, 3}, {3, 3}}, /*dimension_numbers=*/dnums,
              /*feature_group_count=*/64);

  ComputeAndCompare(&builder, {LiteralUtil::CreateFromArray(input_data)},
                    error_spec_);
}

class ConvolutionHloTest : public HloTestBase {};

// double datatype is not yet supported in ROCm
XLA_TEST_F(ConvolutionHloTest, DISABLED_ON_GPU_ROCM(ConvolveF64Forward)) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %arg0 = f64[3,56,56,16] parameter(0)
  %arg1 = f64[3,3,3,64] parameter(1)
  ROOT %conv = f64[54,54,16,64] convolution(%arg0, %arg1), window={size=3x3}, dim_labels=f01b_i01o->01bf
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.001}));
}

XLA_TEST_F(ConvolutionHloTest, DISABLED_ON_GPU(ConvolveC64Forward)) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %arg0 = c64[3,56,56,16] parameter(0)
  %arg1 = c64[3,3,3,64] parameter(1)
  ROOT %conv = c64[54,54,16,64] convolution(%arg0, %arg1), window={size=3x3}, dim_labels=f01b_i01o->01bf
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

XLA_TEST_F(ConvolutionHloTest,
           DISABLED_ON_GPU_ROCM(ConvolveF32ForwardReversed)) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %arg0 = f32[3,56,56,16] parameter(0)
  %arg1 = f32[3,3,3,32] parameter(1)
  ROOT %conv = f32[54,54,16,32] convolution(%arg0, %arg1), window={size=3x3 rhs_reversal=1x1}, dim_labels=f01b_i01o->01bf
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.001}));
}

// double datatype is not yet supported in ROCm
XLA_TEST_F(ConvolutionHloTest,
           DISABLED_ON_GPU_ROCM(ConvolveF64BackwardFilter)) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %arg0 = f64[2,5,8,1] parameter(0)
  %arg1 = f64[2,5,8,2] parameter(1)
  ROOT %conv = f64[4,4,1,2] convolution(%arg0, %arg1), window={size=5x8 pad=1_2x1_2}, dim_labels=f01b_i01o->01bf
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.001}));
}

// double datatype is not yet supported in ROCm
XLA_TEST_F(ConvolutionHloTest, DISABLED_ON_GPU_ROCM(ConvolveF64BackwardInput)) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %output = f64[4,5,16,16] parameter(0)
  %kernel = f64[5,3,7,7] parameter(1)
  %reverse = f64[5,3,7,7] reverse(f64[5,3,7,7] %kernel), dimensions={2,3}
  ROOT %convolution = f64[4,3,16,16] convolution(%output, %reverse), window={size=7x7 pad=3_3x3_3}, dim_labels=bf01_io01->bf01
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.001}));
}

XLA_TEST_F(ConvolutionHloTest, ConvolveBackwardInput) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %output = f32[3,3,64,64] parameter(0)
  %kernel = f32[672,7,7,64] parameter(1)
  %reverse = f32[672,7,7,64]{3,2,1,0} reverse(f32[672,7,7,64]{3,2,1,0} %kernel), dimensions={1,2}
  ROOT %convolution = f32[672,9,9,64]{3,2,1,0} convolution(f32[3,3,64,64]{3,2,1,0} %output, f32[672,7,7,64]{3,2,1,0} %reverse), window={size=7x7 pad=6_6x6_6}, dim_labels=01bf_o01i->f01b
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

XLA_TEST_F(ConvolutionHloTest, SwappedOperandConvolve) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %lhs = f32[3,3,7,7] parameter(0)
  %rhs = f32[5,11,11,7] parameter(1)
  ROOT %convolution = f32[5,21,2,7] convolution(lhs, rhs),
     window={size=11x11 pad=3_25x3_6},
     dim_labels=01bf_o01i->f01b
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

XLA_TEST_F(ConvolutionHloTest, SwappedOperandConvolveWithStride) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %lhs = f32[3,3,7,7] parameter(0)
  %rhs = f32[5,11,11,7] parameter(1)
  ROOT %convolution = f32[5,11,2,7] convolution(lhs, rhs),
     window={size=11x11 pad=3_26x3_6 stride=2x1},
     dim_labels=01bf_o01i->f01b
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}
XLA_TEST_F(ConvolutionHloTest, SwappedOperandConvolve2) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY Test {
  %lhs = f32[3,3,7,7] parameter(0)
  %rhs = f32[5,11,11,7] parameter(1)
  ROOT %convolution = f32[5,11,4,7] convolution(lhs, rhs),
     window={size=11x11 pad=3_25x3_6 lhs_dilate=1x2 rhs_dilate=2x1},
     dim_labels=01bf_o01i->f01b
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

XLA_TEST_F(ConvolutionHloTest, TestConv0D) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY TestComputation {
  %parameter.1 = f32[10,5]{1,0} parameter(0)
  %parameter.2 = f32[5,7]{1,0} parameter(1)
  ROOT %convolution.3 = f32[10,7]{1,0} convolution(f32[10,5]{1,0} %parameter.1, f32[5,7]{1,0} %parameter.2), dim_labels=bf_io->bf
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

XLA_TEST_F(ConvolutionHloTest, TestFusedConv2D) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY TestComputation {
  %p0 = f32[8,5,5,1] parameter(0)
  %p1 = f32[3,3,1,32] parameter(1)
  %conv = f32[8,5,5,32] convolution(p0, p1), window={size=3x3 pad=1_1x1_1}, dim_labels=b01f_01io->b01f
  %bias = f32[32] parameter(2)
  %broadcasted_bias = f32[8,5,5,32] broadcast(%bias), dimensions={3}
  %add = f32[8,5,5,32] add(%conv, %broadcasted_bias)
  %zero = f32[] constant(0)
  %zeros = f32[8,5,5,32] broadcast(%zero), dimensions={}
  ROOT relu = f32[8,5,5,32] maximum(%zeros, %add)
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

XLA_TEST_F(ConvolutionHloTest, TestFusedConv3D) {
  constexpr char kHlo[] = R"(
HloModule TestModule

ENTRY TestComputation {
  %p0 = f32[8,4,5,5,1] parameter(0)
  %p1 = f32[3,3,3,1,32] parameter(1)
  %conv = f32[8,4,5,5,32] convolution(p0, p1), window={size=3x3x3 pad=1_1x1_1x1_1}, dim_labels=b012f_012io->b012f
  %bias = f32[32] parameter(2)
  %broadcasted_bias = f32[8,4,5,5,32] broadcast(%bias), dimensions={4}
  %add = f32[8,4,5,5,32] add(%conv, %broadcasted_bias)
  %zero = f32[] constant(0)
  %zeros = f32[8,4,5,5,32] broadcast(%zero), dimensions={}
  ROOT relu = f32[8,4,5,5,32] maximum(%zeros, %add)
})";
  EXPECT_TRUE(RunAndCompare(kHlo, ErrorSpec{0.01, 0.01}));
}

}  // namespace
}  // namespace xla
