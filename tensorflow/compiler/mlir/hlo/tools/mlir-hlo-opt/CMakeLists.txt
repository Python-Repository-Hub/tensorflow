#
# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
set(LIBS
        ${dialect_libs}
        ${conversion_libs}
        MLIROptLib

        GmlStPasses
        GmlStTestPasses
        GmlStDialect
        LmhloDialect
        LmhloGPUDialect
        MhloRegisterDialects
        LmhloPasses
        AllMhloPasses
        MLIRBufferTransforms
        MLIRHLOGPUTransforms
        MLIRHLOAnalysis
        MLIRHLOTestAnalysis
        )
add_llvm_executable(mlir-hlo-opt mlir-hlo-opt.cc
  DEPENDS
        MLIRLmhloPassIncGen
        MLIRMhloPassIncGen
        LMHLOTransformsPassIncGen
        LMHLOGPUTransformsPassIncGen
)
llvm_update_compile_flags(mlir-hlo-opt)
target_link_libraries(mlir-hlo-opt PRIVATE ${LIBS})

mlir_check_all_link_libraries(mlir-hlo-opt)
