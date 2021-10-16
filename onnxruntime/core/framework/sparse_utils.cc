// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#if !defined(DISABLE_SPARSE_TENSORS)

#include "core/framework/sparse_utils.h"
#include "core/common/status.h"
#include "core/framework/tensor.h"
#include "core/framework/data_types_internal.h"
#include "core/framework/data_transfer_manager.h"
#include "core/framework/sparse_tensor.h"

#include "core/util/math_cpuonly.h"
#include <map>

namespace onnxruntime {
namespace sparse_utils {

#if !defined(ORT_MINIMAL_BUILD)
template <typename T, typename ValueRecorder>
void ScanAndRecordCsr(gsl::span<const T> src_span, int64_t cols,
                      std::vector<int64_t>& inner, std::vector<int64_t>& outer,
                      ValueRecorder recorder) {
  int64_t row = 0;
  int64_t index = 0;
  outer.push_back(0);
  NotZero<T> not_zero;
  for (const auto& v : src_span) {
    auto cur_row = index / cols;
    if (cur_row != row) {
      outer.push_back(static_cast<int64_t>(inner.size()));
      row = cur_row;
    }
    if (not_zero(v)) {
      auto cur_col = index - cur_row * cols;
      inner.push_back(cur_col);
      recorder(v);
    }
    ++index;
  }
  outer.push_back(static_cast<int64_t>(inner.size()));
}

Status DenseTensorToSparseCsr(const DataTransferManager& data_manager, const Tensor& src,
                              const AllocatorPtr& cpu_allocator, const AllocatorPtr& dst_allocator,
                              SparseTensor& dst) {
  const auto& src_dims = src.Shape().GetDims();
  if (src_dims.size() != 2) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Currently do not support dims higher than 2 dimensions: ", src_dims.size());
  }

  const bool is_string = src.IsDataTypeString();

  if (is_string && dst_allocator->Info().device.Type() != OrtDevice::CPU) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Unable to convert strings tensor to a sparse tensor that not on CPU");
  }

  const IDataTransfer* data_transfer = data_manager.GetDataTransfer(cpu_allocator->Info().device,
                                                                    dst_allocator->Info().device);
  ORT_RETURN_IF_NOT(data_transfer != nullptr, "Unable to find a data transfer for copying from device type: ",
                    cpu_allocator->Info().device.Type(), " to device type: ", dst_allocator->Info().device.Type());

  const auto element_size = src.DataType()->Size();
  gsl::span<const uint8_t> src_span;
  Tensor src_cpu;
  if (src.Location().device.Type() != OrtDevice::CPU) {
    Tensor t(src.DataType(), src.Shape(), cpu_allocator);
    ORT_RETURN_IF_ERROR(data_manager.CopyTensor(src, t));
    src_cpu = std::move(t);
    src_span = gsl::make_span(reinterpret_cast<const uint8_t*>(src_cpu.DataRaw()), src_cpu.SizeInBytes());
  } else {
    src_span = gsl::make_span(reinterpret_cast<const uint8_t*>(src.DataRaw()), src.SizeInBytes());
  }

  const auto rows = src_dims[0];
  const auto cols = src_dims[1];

  std::vector<int64_t> inner_indices;
  inner_indices.reserve(static_cast<size_t>(src.Shape().Size() / 2));
  std::vector<int64_t> outer_indices;
  outer_indices.reserve(static_cast<size_t>(rows) + 1);

  std::vector<uint8_t> values_8;
  std::vector<uint16_t> values_16;
  std::vector<uint32_t> values_32;
  std::vector<uint64_t> values_64;
  std::vector<std::reference_wrapper<const std::string>> values_str;
  Tensor nnz_tensor;

  if (is_string) {
    auto str_span = src.DataAsSpan<std::string>();
    ScanAndRecordCsr(str_span, cols, inner_indices, outer_indices,
                     [&](const std::string& s) { values_str.push_back(std::cref(s)); });
  } else {
    switch (element_size) {
      case sizeof(uint8_t): {
        ScanAndRecordCsr(src_span, cols, inner_indices, outer_indices, [&](uint8_t v) { values_8.push_back(v); });
        Tensor t(src.DataType(), {static_cast<int64_t>(values_8.size())}, values_8.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      case sizeof(uint16_t): {
        // MFFloat16 and BFloat16 are handled fine
        auto span16 = src_span.as_span<const uint16_t>();
        ScanAndRecordCsr(span16, cols, inner_indices, outer_indices, [&](uint16_t v) { values_16.push_back(v); });
        Tensor t(src.DataType(), {static_cast<int64_t>(values_16.size())}, values_16.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      case sizeof(uint32_t): {
        auto span32 = src_span.as_span<const uint32_t>();
        ScanAndRecordCsr(span32, cols, inner_indices, outer_indices, [&](uint32_t v) { values_32.push_back(v); });
        Tensor t(src.DataType(), {static_cast<int64_t>(values_32.size())}, values_32.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      case sizeof(uint64_t): {
        auto span64 = src_span.as_span<const uint64_t>();
        ScanAndRecordCsr(span64, cols, inner_indices, outer_indices, [&](uint64_t v) { values_64.push_back(v); });
        Tensor t(src.DataType(), {static_cast<int64_t>(values_64.size())}, values_64.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      default:
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported element size: ", element_size);
    }
  }

  const auto nnz = inner_indices.size();
  const size_t outer_size = (nnz > 0) ? outer_indices.size() : 0U;

  SparseTensor dst_tensor(src.DataType(), src.Shape(), dst_allocator);
  auto mutator = dst_tensor.MakeCsrData(nnz, nnz, outer_size);
  if (nnz > 0) {
    if (is_string) {
      auto dst_span = mutator.Values().MutableDataAsSpan<std::string>();
      std::copy(values_str.cbegin(), values_str.cend(), dst_span.begin());
    } else {
      ORT_RETURN_IF_ERROR(data_transfer->CopyTensor(nnz_tensor, mutator.Values()));
    }
    auto index_type = DataTypeImpl::GetType<int64_t>();
    Tensor inner(index_type, {static_cast<int64_t>(nnz)}, inner_indices.data(), cpu_allocator->Info());
    ORT_RETURN_IF_ERROR(data_transfer->CopyTensor(inner, mutator.Inner()));
    Tensor outer(index_type, {static_cast<int64_t>(outer_size)},
                 outer_indices.data(), cpu_allocator->Info());
    ORT_RETURN_IF_ERROR(data_transfer->CopyTensor(outer, mutator.Outer()));
  }

  dst = std::move(dst_tensor);
  return Status::OK();
}

Status SparseCsrToDenseTensor(const DataTransferManager& data_manager, const SparseTensor& src,
                              const AllocatorPtr& cpu_allocator, const AllocatorPtr& dst_allocator,
                              Tensor& dst) {
  const auto& src_dims = src.DenseShape().GetDims();
  if (src_dims.size() != 2) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Support 2-D matrices only");
  }

  if (!(src.Format() == SparseFormat::kCsrc)) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input must be of CSR format");
  }

  const bool is_string = src.IsDataTypeString();

  if (is_string && dst_allocator->Info().device.Type() != OrtDevice::CPU) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Unable to convert strings tensor to a sparse tensor that is not on CPU");
  }

  const AllocatorPtr& conversion_allocator = (dst_allocator->Info().device.Type() == OrtDevice::CPU)
                                                 ? dst_allocator
                                                 : cpu_allocator;

  Tensor cpu_result(src.DataType(), src.DenseShape(), conversion_allocator);
  if (!is_string) {
    memset(cpu_result.MutableDataRaw(), 0, cpu_result.SizeInBytes());
  }

  if (src.NumValues() > 0) {
    const auto rows = src_dims[0];
    const auto cols = src_dims[1];

    {
      auto csr_view = src.AsCsr();
      const auto inner_num = csr_view.Inner().Shape().Size();
      const auto outer_num = csr_view.Outer().Shape().Size();
      ORT_ENFORCE(inner_num == src.Values().Shape().Size(), "Expecting inner indices to be same as nnz. Got: ", inner_num);
      ORT_ENFORCE(outer_num == (rows + 1), "Outer indices must be M + 1. Got: ", outer_num);
    }

    CopyElementFunc copy_func;
    if (is_string) {
      copy_func = CopyElementAligned<std::string>;
    } else {
      const auto element_size = src.DataType()->Size();
      switch (element_size) {
        case sizeof(uint8_t):
          copy_func = CopyElementAligned<uint8_t>;
          break;
        case sizeof(uint16_t):
          copy_func = CopyElementAligned<uint16_t>;
          break;
        case sizeof(uint32_t):
          copy_func = CopyElementAligned<uint32_t>;
          break;
        case sizeof(uint64_t):
          copy_func = CopyElementAligned<uint64_t>;
          break;
        default:
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported element size: ", element_size);
      }
    }

    SparseTensor cpu_src;
    const void* values = nullptr;
    gsl::span<const int64_t> inner_span;
    gsl::span<const int64_t> outer_span;
    if (src.Location().device.Type() != OrtDevice::CPU) {
      SparseTensor t(src.DataType(), src.DenseShape(), cpu_allocator);
      ORT_RETURN_IF_ERROR(data_manager.CopySparseTensor(src, t));
      cpu_src = std::move(t);
      values = cpu_src.Values().DataRaw();
      inner_span = cpu_src.AsCsr().Inner().DataAsSpan<int64_t>();
      outer_span = cpu_src.AsCsr().Outer().DataAsSpan<int64_t>();
    } else {
      values = src.Values().DataRaw();
      inner_span = src.AsCsr().Inner().DataAsSpan<int64_t>();
      outer_span = src.AsCsr().Outer().DataAsSpan<int64_t>();
    }

    void* output = cpu_result.MutableDataRaw();

    size_t src_idx = 0;
    size_t inner_idx = 0;
    for (size_t out_i = 1; out_i < outer_span.size(); ++out_i) {
      auto row_size = outer_span[out_i] - outer_span[out_i - 1];
      for (int64_t cnt = 0; cnt < row_size; ++cnt, ++inner_idx) {
        assert(inner_idx < inner_span.size());
        auto col = inner_span[inner_idx];
        auto dst_idx = (out_i - 1) * cols + col;
        copy_func(output, values, dst_idx, src_idx);
      }
    }
  }

  if (dst_allocator->Info().device.Type() != OrtDevice::CPU) {
    Tensor dest_tensor(src.DataType(), src.DenseShape(), dst_allocator);
    ORT_RETURN_IF_ERROR(data_manager.CopyTensor(cpu_result, dest_tensor));
    dst = std::move(dest_tensor);
  } else {
    dst = std::move(cpu_result);
  }

  return Status::OK();
}

Status SparseCooToDenseTensor(const DataTransferManager& data_manager, const SparseTensor& src,
                              const AllocatorPtr& cpu_allocator, const AllocatorPtr& dst_allocator, Tensor& dst) {
  const auto& src_dims = src.DenseShape().GetDims();
  ORT_RETURN_IF(src_dims.size() < 1 || src_dims.size() > 2,
                "Currently support 1-D and 2-D tensors: ", src_dims.size());
  ORT_RETURN_IF_NOT(src.Format() == SparseFormat::kCoo, "Input must be of COO format");

  const bool is_string = src.IsDataTypeString();
  if (is_string && dst_allocator->Info().device.Type() != OrtDevice::CPU) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Unable to convert strings tensor to a sparse tensor that is not on CPU");
  }

  const AllocatorPtr& conversion_allocator = (dst_allocator->Info().device.Type() == OrtDevice::CPU)
                                                 ? dst_allocator
                                                 : cpu_allocator;
  Tensor cpu_result(src.DataType(), src.DenseShape(), conversion_allocator);
  if (!is_string) {
    memset(cpu_result.MutableDataRaw(), 0, cpu_result.SizeInBytes());
  }

  if (src.NumValues() > 0) {
    const void* values = nullptr;
    const int64_t* indices = nullptr;
    const auto num_values = src.Values().Shape().Size();
    const auto num_indices = src.AsCoo().Indices().Shape().Size();
    ORT_RETURN_IF_NOT((num_values == num_indices || 2 * num_values == num_indices),
                      "Expecting indices to be equal the number of values or be twice as many");

    SparseTensor src_cpu;
    if (src.Location().device.Type() != OrtDevice::CPU) {
      SparseTensor t(src.DataType(), src.DenseShape(), cpu_allocator);
      ORT_RETURN_IF_ERROR(data_manager.CopySparseTensor(src, t));
      src_cpu = std::move(t);
      values = src_cpu.Values().DataRaw();
      indices = src_cpu.AsCoo().Indices().Data<int64_t>();
    } else {
      values = src.Values().DataRaw();
      indices = src.AsCoo().Indices().Data<int64_t>();
    }

    const auto element_size = src.DataType()->Size();
    CopyElementFunc copy_func = nullptr;
    if (src.IsDataTypeString()) {
      copy_func = CopyElementAligned<std::string>;
    } else {
      switch (element_size) {
        case sizeof(uint8_t):
          copy_func = CopyElementAligned<uint8_t>;
          break;
        case sizeof(uint16_t): {
          copy_func = CopyElementAligned<uint16_t>;
        } break;
        case sizeof(uint32_t): {
          copy_func = CopyElementAligned<uint32_t>;
        } break;
        case sizeof(uint64_t): {
          copy_func = CopyElementAligned<uint64_t>;
        } break;
        default:
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported element size: ", element_size);
      }
    }

    const auto dense_size = src.DenseShape().Size();
    void* output = cpu_result.MutableDataRaw();
    // Linear index
    if (num_indices == num_values) {
      for (int64_t src_idx = 0; src_idx < num_values; ++src_idx) {
        auto dst_idx = indices[src_idx];
        ORT_RETURN_IF_NOT(dst_idx < dense_size, "Invalid index: ", dst_idx, " > dense_size: ", dense_size);
        copy_func(output, values, dst_idx, src_idx);
      }
    } else {
      const auto cols = src_dims[1];
      for (int64_t src_idx = 0; src_idx < num_values; ++src_idx) {
        auto tuple_idx = src_idx * 2;
        auto dst_idx = indices[tuple_idx] * cols + indices[tuple_idx + 1];
        ORT_RETURN_IF_NOT(dst_idx < dense_size, "Invalid index: ", dst_idx, " > dense_size: ", dense_size);
        copy_func(output, values, dst_idx, src_idx);
      }
    }
  }

  if (dst_allocator->Info().device.Type() != OrtDevice::CPU) {
    Tensor t(src.DataType(), src.DenseShape(), dst_allocator);
    ORT_RETURN_IF_ERROR(data_manager.CopyTensor(cpu_result, t));
    dst = std::move(t);
  } else {
    dst = std::move(cpu_result);
  }

  return Status::OK();
}

#endif  //ORT_MINIMAL_BUILD

template <typename T, typename ValueRecorder>
void ScanAndRecordCoo(gsl::span<const T> src_span,
                      int64_t cols,
                      bool linear,
                      std::vector<int64_t>& indices,
                      ValueRecorder recorder) {
  int64_t index = 0;
  NotZero<T> not_zero;
  for (const auto& v : src_span) {
    if (not_zero(v)) {
      recorder(v);
      if (linear) {
        indices.push_back(index);
      } else {
        auto row = index / cols;
        auto col = index - row * cols;
        indices.push_back(row);
        indices.push_back(col);
      }
    }
    ++index;
  }
}

Status DenseTensorToSparseCoo(const DataTransferManager& data_manager, const Tensor& src,
                              const AllocatorPtr& cpu_allocator,
                              const AllocatorPtr& dst_allocator, bool linear_index, SparseTensor& dst) {
  const IDataTransfer* data_transfer = data_manager.GetDataTransfer(cpu_allocator->Info().device,
                                                                    dst_allocator->Info().device);
  ORT_RETURN_IF_NOT(data_transfer != nullptr, "Unable to find a data transfer for copying from device type: ",
                    cpu_allocator->Info().device.Type(), " to device type: ", dst_allocator->Info().device.Type());

  const auto& src_dims = src.Shape().GetDims();
  ORT_RETURN_IF(src_dims.size() < 1 || src_dims.size() > 2,
                "Currently support 1-D and 2-D tensors: ", src_dims.size());
  ORT_RETURN_IF(src_dims.size() == 1 && !linear_index, "1-D tensors may only have 1-D indices");

  const bool is_string = src.IsDataTypeString();
  if (is_string && dst_allocator->Info().device.Type() != OrtDevice::CPU) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Unable to convert strings tensor to a sparse tensor that is not on CPU");
  }

  gsl::span<const uint8_t> src_span;
  Tensor src_cpu;
  if (src.Location().device.Type() != OrtDevice::CPU) {
    Tensor t(src.DataType(), src.Shape(), cpu_allocator);
    ORT_RETURN_IF_ERROR(data_manager.CopyTensor(src, t));
    src_cpu = std::move(t);
    src_span = gsl::make_span(reinterpret_cast<const uint8_t*>(src_cpu.DataRaw()), src_cpu.SizeInBytes());
  } else {
    src_span = gsl::make_span(reinterpret_cast<const uint8_t*>(src.DataRaw()), src.SizeInBytes());
  }

  std::vector<int64_t> gathered_indices;
  gathered_indices.reserve(static_cast<size_t>(src.Shape().Size() / 2));
  const auto cols = (src_dims.size() == 2) ? src_dims[1] : src_dims[0];
  std::vector<uint8_t> values_8;
  std::vector<uint16_t> values_16;
  std::vector<uint32_t> values_32;
  std::vector<uint64_t> values_64;
  std::vector<std::reference_wrapper<const std::string>> values_str;
  Tensor nnz_tensor;

  if (is_string) {
    auto str_span = src.DataAsSpan<std::string>();
    ScanAndRecordCoo(str_span, cols, linear_index, gathered_indices,
                     [&](const std::string& s) { values_str.push_back(std::cref(s)); });
  } else {
    const auto element_size = src.DataType()->Size();
    switch (element_size) {
      case sizeof(uint8_t): {
        ScanAndRecordCoo(src_span, cols, linear_index, gathered_indices,
                         [&](int8_t v) { values_8.push_back(v); });
        Tensor t(src.DataType(), TensorShape{static_cast<int64_t>(values_8.size())},
                 values_8.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      case sizeof(uint16_t): {
        // MFFloat16 and BFloat16 are handled fine
        auto span16 = src_span.as_span<const uint16_t>();
        ScanAndRecordCoo(span16, cols, linear_index, gathered_indices, [&](int16_t v) { values_16.push_back(v); });
        Tensor t(src.DataType(), TensorShape{static_cast<int64_t>(values_16.size())},
                 values_16.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      case sizeof(uint32_t): {
        auto span32 = src_span.as_span<const uint32_t>();
        ScanAndRecordCoo(span32, cols, linear_index, gathered_indices, [&](int32_t v) { values_32.push_back(v); });
        Tensor t(src.DataType(), TensorShape{static_cast<int64_t>(values_32.size())},
                 values_32.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      case sizeof(uint64_t): {
        auto span64 = src_span.as_span<const uint64_t>();
        ScanAndRecordCoo(span64, cols, linear_index, gathered_indices, [&](int64_t v) { values_64.push_back(v); });
        Tensor t(src.DataType(), TensorShape{static_cast<int64_t>(values_64.size())},
                 values_64.data(), cpu_allocator->Info());
        nnz_tensor = std::move(t);
      } break;
      default:
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported element size: ", element_size);
    }
  }

  const auto nnz = (linear_index) ? gathered_indices.size() : gathered_indices.size() / 2;
  assert(gsl::narrow<int64_t>(nnz) == nnz_tensor.Shape().Size() || nnz == values_str.size());

  SparseTensor dst_result(src.DataType(), src.Shape(), dst_allocator);
  auto mutator = dst_result.MakeCooData(nnz, gathered_indices.size());
  if (nnz > 0) {
    if (is_string) {
      auto dst_iter = mutator.Values().MutableData<std::string>();
      std::copy(values_str.cbegin(), values_str.cend(), dst_iter);
    } else {
      ORT_RETURN_IF_ERROR(data_transfer->CopyTensor(nnz_tensor, mutator.Values()));
    }
    Tensor indices_tensor(DataTypeImpl::GetType<int64_t>(), mutator.Indices().Shape(), gathered_indices.data(), cpu_allocator->Info());
    ORT_RETURN_IF_ERROR(data_transfer->CopyTensor(indices_tensor, mutator.Indices()));
  }

  dst = std::move(dst_result);

  return Status::OK();
}

void CopyCpuTensor(const Tensor& src, Tensor& dst) {
  ORT_ENFORCE(src.Shape().Size() == dst.Shape().Size(), "Src and Dst tensors must be the same size");
  if (src.IsDataTypeString()) {
    auto str_span = src.DataAsSpan<std::string>();
    auto* dst_iter = dst.MutableData<std::string>();
    std::copy(str_span.cbegin(), str_span.cend(), dst_iter);
  } else {
    memcpy(dst.MutableDataRaw(), src.DataRaw(), src.SizeInBytes());
  }
}

void CopyCpuSparseCooTensor(const SparseTensor& src, SparseTensor& tgt) {
  if (&src != &tgt) {
    ORT_ENFORCE(src.DenseShape().Size() <= tgt.DenseShape().Size(), "Target shape Size() must be at least source size");
    ORT_ENFORCE(src.GetElementType() == tgt.GetElementType(), "Must the same element type");
    if (src.Format() == SparseFormat::kCoo) {
      auto coo_view = src.AsCoo();
      const auto indices_size = gsl::narrow<size_t>(coo_view.Indices().Shape().Size());
      auto coo_mutator = tgt.MakeCooData(src.NumValues(), indices_size);
      CopySparseCpuValues(src, coo_mutator.Values());
      memcpy(coo_mutator.Indices().MutableDataRaw(), coo_view.Indices().DataRaw(), coo_view.Indices().SizeInBytes());
    } else {
      ORT_THROW("Only COO format is supported. Consider using SparseTensor::Copy");
    }
  }
}

Status Convert2DCooIndicesTo1D(int64_t cols, const gsl::span<const int64_t>& input_span, gsl::span<int64_t> output_span) {
  ORT_RETURN_IF_NOT(input_span.size() % 2 == 0, "2-D indices size must be evenly divisible by 2");
  ORT_RETURN_IF_NOT(output_span.size() * 2 == input_span.size(), "Output span size must be twice less as input_span");
  for (size_t i = 0, dst_idx = 0, limit = input_span.size(); i < limit; i += 2, dst_idx++) {
    int64_t ind = input_span[i] * cols + input_span[i + 1];
    output_span[dst_idx] = ind;
  }
  return Status::OK();
}

Status GetCoo1DIndicesAndMaybeConvert(const SparseTensor& input, IndicesSpan& output) {
  if (input.Format() == SparseFormat::kCoo) {
    const auto& coo_indices = input.AsCoo().Indices();
    const auto num_dims = coo_indices.Shape().NumDimensions();
    if (num_dims == 2) {
      ORT_ENFORCE(input.DenseShape().NumDimensions() == 2, "Expecting dense shape to be 2-D");
      const auto cols = input.DenseShape().GetDims()[1];
      const auto ind_span = coo_indices.DataAsSpan<int64_t>();
      std::vector<int64_t> converted;
      converted.resize(ind_span.size() / 2);
      ORT_THROW_IF_ERROR(sparse_utils::Convert2DCooIndicesTo1D(cols, ind_span, gsl::make_span(converted)));
      output = IndicesSpan(std::move(converted));
    } else {
      ORT_RETURN_IF_NOT(num_dims == 1, "Expecting indices 1 or 2-D for COO");
      output = IndicesSpan(coo_indices.DataAsSpan<int64_t>());
    }
  } else {
    ORT_RETURN_IF_NOT(input.Format() == SparseFormat::kCsrc, "Only support COO and CSR formats");
    ORT_ENFORCE(input.DenseShape().NumDimensions() == 2, "Expecting dense shape to be 2-D");
    const auto rows = input.DenseShape().GetDims()[0];
    const auto cols = input.DenseShape().GetDims()[1];
    auto csr_view = input.AsCsr();
    // For vectors we simply point to the inner
    if (rows == 1 || cols == 1) {
      const auto inner_span = csr_view.Inner().DataAsSpan<int64_t>();
      assert(static_cast<size_t>(rows) == inner_span.size() || static_cast<size_t>(cols) == inner_span.size());
      output = IndicesSpan(inner_span);
    } else {
      const auto inner_span = csr_view.Inner().DataAsSpan<int64_t>();
      const auto outer_span = csr_view.Inner().DataAsSpan<int64_t>();
      std::vector<int64_t> converted;
      converted.resize(inner_span.size());
      ORT_RETURN_IF_ERROR(ConvertCsrIndicesToCooIndices(cols, inner_span, outer_span, gsl::make_span(converted)));
      output = IndicesSpan(std::move(converted));
    }
  }
  return Status::OK();
}

Status ConvertIndicesTo1DAndCopy(const SparseTensor& input_sparse, SparseTensor::CooMutator& coo_mutator) {
  const auto cols = input_sparse.DenseShape().GetDims()[1];
  const auto& indices = input_sparse.AsCoo().Indices();
  const auto ind_span = indices.DataAsSpan<int64_t>();

  auto& dest_indices = coo_mutator.Indices();
  ORT_RETURN_IF_ERROR(Convert2DCooIndicesTo1D(cols, ind_span, dest_indices.MutableDataAsSpan<int64_t>()));
  return Status::OK();
}

Status GetCsrIndicesAndMaybeConvert(const std::vector<int64_t>& computed_dims,
                                    const SparseTensor& input, CsrIndicesSpan& csr_span) {
  assert(computed_dims.size() == 2);

  if (input.Format() == SparseFormat::kCsrc) {
    auto csr_view = input.AsCsr();
    csr_span = CsrIndicesSpan(csr_view.Inner().DataAsSpan<int64_t>(),
                              csr_view.Outer().DataAsSpan<int64_t>());
  } else {
    ORT_RETURN_IF_NOT(input.Format() == SparseFormat::kCoo, "Supports COO and CSR formats only");
    const auto& coo_indices = input.AsCoo().Indices();
    const auto input_indices = coo_indices.DataAsSpan<int64_t>();
    // Fully sparse matrix
    if (input_indices.empty()) {
      csr_span = CsrIndicesSpan();
      return Status::OK();
    }

    const auto input_indices_ndims = coo_indices.Shape().NumDimensions();
    ORT_RETURN_IF_NOT(input_indices_ndims == 1 || input_indices_ndims == 2, "Expecting 1D or 2D COO indices");

    if (computed_dims[0] == 1 || computed_dims[1] == 1) {
      // For vectors we point to the original COO indices as if it is a row vector
      ORT_RETURN_IF_NOT(input_indices_ndims == 1, "COO indices must be 1-D for vectors");
      std::vector<int64_t> outer_indices{0, gsl::narrow<int64_t>(input_indices.size())};
      csr_span = CsrIndicesSpan(input_indices, std::move(outer_indices));
    } else {  // matrix
      const auto rows = computed_dims[0];
      const auto cols = computed_dims[1];
      std::vector<int64_t> inner_indices;
      std::vector<int64_t> outer_indices;
      outer_indices.reserve(static_cast<size_t>(rows + 1));
      outer_indices.push_back(0);
      int64_t row = 0;
      if (input_indices_ndims == 1) {
        inner_indices.reserve(input_indices.size());
        for (auto idx : input_indices) {
          const auto cur_row = idx / cols;
          const auto cur_col = idx - cur_row * cols;
          for (int64_t sz = static_cast<int64_t>(inner_indices.size()); row < cur_row; ++row) {
            outer_indices.push_back(sz);
          }
          inner_indices.push_back(cur_col);
        }
        assert(input_indices.size() == inner_indices.size());
      } else {
        inner_indices.reserve(input_indices.size() / 2);
        for (size_t i = 0, limit = input_indices.size(); i < limit; i += 2) {
          const auto cur_row = input_indices[i];
          const auto cur_col = input_indices[i + 1];
          for (int64_t sz = static_cast<int64_t>(inner_indices.size()); row < cur_row; ++row) {
            outer_indices.push_back(sz);
          }
          inner_indices.push_back(cur_col);
        }
        assert(input_indices.size() / 2 == inner_indices.size());
      }
      // Need to add entries for all the rows that are still missing
      for (int64_t sz = static_cast<int64_t>(inner_indices.size()); row < rows; ++row) {
        outer_indices.push_back(sz);
      }
      assert(outer_indices.size() == static_cast<size_t>(rows) + 1);
      csr_span = CsrIndicesSpan(std::move(inner_indices), std::move(outer_indices));
    }
  }

  return Status::OK();
}

Status GetCsrIndicesAndTranspose(const std::vector<int64_t>& computed_dims, const SparseTensor& input, CsrIndicesSpan& csr_span) {
  assert(computed_dims.size() == 2);
  const auto rows = computed_dims[0];
  const auto cols = computed_dims[1];

  // set is needed to sort by row
  using ConversionMap = std::map<int64_t, std::set<std::tuple<int64_t, size_t>>>;

  auto output_csr = [cols](const ConversionMap& col_to_row, size_t nnz) {

    std::vector<int64_t> inner_indices;
    std::vector<int64_t> outer_indices;
    std::vector<size_t> value_mapping;

    outer_indices.reserve(static_cast<size_t>(cols + 1));
    outer_indices.push_back(0);

    inner_indices.reserve(nnz);
    value_mapping.reserve(nnz);

    int64_t col = 0;
    for (const auto& p : col_to_row) {
      const auto cur_col = p.first;
      for (int64_t sz = static_cast<int64_t>(inner_indices.size()); col < cur_col; ++col) {
        outer_indices.push_back(sz);
      }
      for (const auto& t : p.second) {
        inner_indices.push_back(std::get<0>(t));
        value_mapping.push_back(std::get<1>(t));
      }
    }
    // Need to add entries for all the rows that are still missing
    for (int64_t sz = static_cast<int64_t>(inner_indices.size()); col < cols; ++col) {
      outer_indices.push_back(sz);
    }

    assert(outer_indices.size() == static_cast<size_t>(cols) + 1);
    assert(inner_indices.size() == nnz);
    assert(value_mapping.size() == nnz);
    return CsrIndicesSpan(std::move(inner_indices), std::move(outer_indices), std::move(value_mapping));
  };

  if (input.Format() == SparseFormat::kCsrc) {
    auto csr_view = input.AsCsr();
    const auto inner = csr_view.Inner().DataAsSpan<int64_t>();

    // Fully sparse
    if (inner.empty()) {
      csr_span = CsrIndicesSpan();
      return Status::OK();
    }

    const auto outer = csr_view.Outer().DataAsSpan<int64_t>();
    if (rows == 1 || cols == 1) {
      // We do not transpose a vector
      csr_span = CsrIndicesSpan(inner, outer);
    } else {
      ConversionMap col_to_row;
      size_t offset = 0;
      for (size_t i = 1, limit = outer.size(); i < limit; ++i) {
        const int64_t row = static_cast<int64_t>(i) - 1;
        for (int64_t c = 0, c_limit = outer[i] - outer[i - 1]; c < c_limit; ++c, ++offset) {
          const auto col = inner[c];
          ORT_RETURN_IF_NOT(col_to_row[col].insert(std::make_tuple(row, offset)).second, "Expecting no duplicates");
        }
      }
      csr_span = output_csr(col_to_row, inner.size());
    }
  } else {
    ORT_RETURN_IF_NOT(input.Format() == SparseFormat::kCoo, "Supports COO and CSR formats only");
    const auto& coo_indices = input.AsCoo().Indices();
    const auto input_indices = coo_indices.DataAsSpan<int64_t>();
    // Fully sparse matrix
    if (input_indices.empty()) {
      csr_span = CsrIndicesSpan();
      return Status::OK();
    }

    const auto input_indices_ndims = coo_indices.Shape().NumDimensions();
    ORT_RETURN_IF_NOT(input_indices_ndims == 1 || input_indices_ndims == 2, "Expecting 1D or 2D COO indices");

    if (rows == 1 || cols == 1) {
      // We do not transpose vectors, but the dims and transpose flag may need to be swapped by the caller
      // since this is returns as if it is always a row vector (and it may be a column)
      ORT_RETURN_IF_NOT(input_indices_ndims == 1, "COO indices must be 1-D for vectors");
      std::vector<int64_t> outer_indices{0, gsl::narrow<int64_t>(input_indices.size())};
      csr_span = CsrIndicesSpan(input_indices, std::move(outer_indices));
    } else {  // matrix
      ConversionMap col_to_row;
      // We swap rows and cols
      size_t offset = 0;
      size_t nnz;
      if (input_indices_ndims == 1) {
        nnz = input_indices.size();
        for (auto idx : input_indices) {
          const auto cur_row = idx / cols;
          const auto cur_col = idx - cur_row * cols;
          ORT_RETURN_IF_NOT(col_to_row[cur_col].insert(std::make_tuple(cur_row, offset++)).second, "Expecting no dups in the indices");
        }
      } else {
        nnz = input_indices.size() / 2;
        for (size_t i = 0, limit = input_indices.size(); i < limit; i += 2) {
          const auto cur_row = input_indices[i];
          const auto cur_col = input_indices[i + 1];
          ORT_RETURN_IF_NOT(col_to_row[cur_col].insert(std::make_tuple(cur_row, offset++)).second, "Expecting no dups in the indices");
        }
      }

      csr_span = output_csr(col_to_row, nnz);
    }
  }
  return Status::OK();
}

Status ConvertCsrIndicesToCooIndices(int64_t cols, gsl::span<const int64_t> input_inner,
                                     gsl::span<const int64_t> input_outer,
                                     gsl::span<int64_t> output_indices) {
  // Fully sparse
  if (input_inner.empty()) {
    return Status::OK();
  }

  ORT_RETURN_IF_NOT(input_inner.size() == output_indices.size(), "Expecting output size the same as inner indices");
  size_t inner_ind = 0;
  for (size_t i = 1, limit = input_outer.size(); i < limit; ++i) {
    const int64_t row_offset = static_cast<int64_t>((i - 1) * cols);
    for (int64_t c = 0, c_limit = input_outer[i] - input_outer[i - 1]; c < c_limit; ++c, ++inner_ind) {
      int64_t coo_ind = row_offset + input_inner[inner_ind];
      output_indices[inner_ind] = coo_ind;
    }
  }

  return Status::OK();
}

void ScanForSparseMatches(const gsl::span<const int64_t>& a_indices,
                          const gsl::span<const int64_t>& b_indices,
                          const std::function<void(size_t, size_t)>& match_cb) {
  size_t a_ind = 0, a_limit = a_indices.size();
  size_t b_ind = 0, b_limit = b_indices.size();
  while (a_ind < a_limit && b_ind < b_limit) {
    auto a_v = a_indices[a_ind];
    auto b_v = b_indices[b_ind];
    if (a_v == b_v) {
      match_cb(a_ind++, b_ind++);
    } else if (a_v < b_v) {
      ++a_ind;
    } else {
      ++b_ind;
    }
  }
}

}  // namespace sparse_utils
}  // namespace onnxruntime

#endif  // !defined(DISABLE_SPARSE_TENSORS)