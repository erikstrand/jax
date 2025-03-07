#include "jaxlib/gpu/triton.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "pybind11/pybind11.h"
#include "pybind11/pytypes.h"
#include "pybind11/stl.h"
#include "absl/base/call_once.h"
#include "absl/base/optimization.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "jaxlib/gpu/gpu_kernel_helpers.h"
#include "jaxlib/gpu/triton.pb.h"
#include "jaxlib/gpu/vendor.h"
#include "jaxlib/kernel_pybind11_helpers.h"
#include "pybind11_abseil/status_casters.h"  // IWYU pragma: keep
#include "xla/service/custom_call_status.h"
#include "xla/stream_executor/gpu/asm_compiler.h"

#define CUDA_RETURN_IF_ERROR(expr) JAX_RETURN_IF_ERROR(JAX_AS_STATUS(expr))

#define JAX_ASSIGN_OR_RETURN(lhs, rexpr)    \
  auto statusor = (rexpr);                  \
  if (ABSL_PREDICT_FALSE(!statusor.ok())) { \
    return statusor.status();               \
  }                                         \
  lhs = (*std::move(statusor))

namespace py = pybind11;

namespace jax::JAX_GPU_NAMESPACE {

// TODO(cjfj): Move this to `gpu_kernel_helpers`?
// Used via JAX_AS_STATUS(expr) macro.
absl::Status AsStatus(CUresult error, const char* file, std::int64_t line,
                      const char* expr) {
  if (ABSL_PREDICT_TRUE(error == CUDA_SUCCESS)) {
    return absl::OkStatus();
  }

  const char* str;
  CHECK_EQ(cuGetErrorName(error, &str), CUDA_SUCCESS);
  return absl::InternalError(
      absl::StrFormat("%s:%d: operation %s failed: %s", file, line, expr, str));
}

}  // namespace jax::JAX_GPU_NAMESPACE

namespace jax_triton {
namespace {

constexpr uint32_t kNumThreadsPerWarp = 32;

struct CuModuleDeleter {
  void operator()(CUmodule module) { cuModuleUnload(module); }
};

using OwnedCUmodule =
    std::unique_ptr<std::remove_pointer_t<CUmodule>, CuModuleDeleter>;

class ModuleImage {
 public:
  ModuleImage(std::string_view kernel_name, std::vector<uint8_t> module_image,
              uint32_t shared_mem_bytes)
      : kernel_name_(kernel_name),
        module_image_(std::move(module_image)),
        shared_mem_bytes_(shared_mem_bytes) {}

  absl::StatusOr<CUfunction> GetFunctionForContext(CUcontext context) {
    absl::MutexLock lock(&mutex_);
    auto it = functions_.find(context);
    if (ABSL_PREDICT_TRUE(it != functions_.end())) {
      return it->second;
    }

    CUDA_RETURN_IF_ERROR(cuCtxPushCurrent(context));
    absl::Cleanup ctx_restorer = [] { cuCtxPopCurrent(nullptr); };

    CUmodule module;
    CUDA_RETURN_IF_ERROR(cuModuleLoadData(&module, module_image_.data()));
    modules_.push_back(OwnedCUmodule(module, CuModuleDeleter()));

    CUfunction function;
    CUDA_RETURN_IF_ERROR(
        cuModuleGetFunction(&function, module, kernel_name_.c_str()));
    auto [_, success] = functions_.insert({context, function});
    CHECK(success);

    // The maximum permitted static shared memory allocation in CUDA is 48kB,
    // but we can expose more to the kernel using dynamic shared memory.
    constexpr int kMaxStaticSharedMemBytes = 49152;
    if (shared_mem_bytes_ <= kMaxStaticSharedMemBytes) {
      return function;
    }

    // Set up dynamic shared memory.
    CUdevice device;
    CUDA_RETURN_IF_ERROR(cuCtxGetDevice(&device));

    int shared_optin;
    CUDA_RETURN_IF_ERROR(cuDeviceGetAttribute(
        &shared_optin, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN,
        device));

    if (shared_mem_bytes_ > shared_optin) {
      return absl::InvalidArgumentError(
          "Shared memory requested exceeds device resources.");
    }

    if (shared_optin > kMaxStaticSharedMemBytes) {
      CUDA_RETURN_IF_ERROR(
          cuFuncSetCacheConfig(function, CU_FUNC_CACHE_PREFER_SHARED));
      int shared_total;
      CUDA_RETURN_IF_ERROR(cuDeviceGetAttribute(
          &shared_total,
          CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR, device));
      int shared_static;
      CUDA_RETURN_IF_ERROR(cuFuncGetAttribute(
          &shared_static, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, function));
      CUDA_RETURN_IF_ERROR(cuFuncSetAttribute(
          function, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
          shared_optin - shared_static));
    }
    return function;
  }

 private:
  std::string kernel_name_;
  std::vector<uint8_t> module_image_;
  uint32_t shared_mem_bytes_;

  absl::Mutex mutex_;
  std::vector<OwnedCUmodule> modules_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<CUcontext, CUfunction> functions_ ABSL_GUARDED_BY(mutex_);
};

absl::StatusOr<ModuleImage*> GetModuleImage(std::string kernel_name,
                                            uint32_t shared_mem_bytes,
                                            std::string_view ptx,
                                            int compute_capability) {
  auto key =
      std::make_tuple(kernel_name, shared_mem_bytes, ptx, compute_capability);

  static absl::Mutex mutex;
  static auto& module_images =
      *new absl::flat_hash_map<decltype(key), std::unique_ptr<ModuleImage>>
          ABSL_GUARDED_BY(mutex);

  absl::MutexLock lock(&mutex);
  auto it = module_images.find(key);
  if (it != module_images.end()) return it->second.get();

  // TODO(cjfj): Support `TRITON_PTXAS_PATH` environment variable?
  int cc_major = compute_capability / 10;
  int cc_minor = compute_capability % 10;
  JAX_ASSIGN_OR_RETURN(
      std::vector<uint8_t> module_image,
      stream_executor::CompileGpuAsm(cc_major, cc_minor, ptx.data(),
                                     stream_executor::GpuAsmOpts{}));

  auto [it2, success] = module_images.insert(
      {std::move(key),
       std::make_unique<ModuleImage>(
           std::move(kernel_name), std::move(module_image), shared_mem_bytes)});
  CHECK(success);
  return it2->second.get();
}

class Kernel {
 public:
  Kernel(std::string kernel_name, uint32_t num_warps, uint32_t shared_mem_bytes,
         std::string ptx, std::string ttir, int compute_capability)
      : kernel_name_(std::move(kernel_name)),
        block_dim_x_(num_warps * kNumThreadsPerWarp),
        shared_mem_bytes_(shared_mem_bytes),
        ptx_(std::move(ptx)),
        ttir_(std::move(ttir)),
        compute_capability_(compute_capability) {}

  absl::Status Launch(CUstream stream, uint32_t grid[3], void** params) {
    if (ABSL_PREDICT_FALSE(module_image_ == nullptr)) {
      JAX_ASSIGN_OR_RETURN(module_image_,
                           GetModuleImage(kernel_name_, shared_mem_bytes_, ptx_,
                                          compute_capability_));
    }

    CUcontext context;
    CUDA_RETURN_IF_ERROR(cuStreamGetCtx(stream, &context));
    JAX_ASSIGN_OR_RETURN(CUfunction kernel,
                         module_image_->GetFunctionForContext(context));
    return JAX_AS_STATUS(cuLaunchKernel(
        kernel, grid[0], grid[1], grid[2], block_dim_x_,
        /*blockDimY=*/1, /*blockDimZ=*/1, shared_mem_bytes_, stream, params,
        /*extra=*/nullptr));
  }

  static Kernel FromProto(const TritonKernel& proto) {
    return Kernel(proto.kernel_name(), proto.num_warps(),
                  proto.shared_mem_bytes(), proto.ptx(), proto.ttir(),
                  proto.compute_capability());
  }

  TritonKernel ToProto() const {
    TritonKernel proto;
    proto.set_kernel_name(kernel_name_);
    proto.set_num_warps(block_dim_x_ / kNumThreadsPerWarp);
    proto.set_shared_mem_bytes(shared_mem_bytes_);
    proto.set_ptx(ptx_);
    proto.set_ttir(ttir_);
    proto.set_compute_capability(compute_capability_);
    return proto;
  }

 private:
  std::string kernel_name_;
  uint32_t block_dim_x_;
  uint32_t shared_mem_bytes_;
  std::string ptx_;
  std::string ttir_;
  int compute_capability_;

  ModuleImage* module_image_ = nullptr;
};

struct KernelCallBase {
  virtual ~KernelCallBase() = default;
  virtual absl::Status Launch(CUstream stream, void** buffers) = 0;
};

class KernelCall : public KernelCallBase {
 public:
  struct Parameter {
    struct Array {
      size_t bytes_to_zero;
      size_t ptr_divisibility;
    };

    static absl::StatusOr<Parameter> FromProto(
        const TritonKernelCall_Parameter& proto) {
      Parameter param;
      switch (proto.value_case()) {
        case TritonKernelCall_Parameter::kArray:
          param.value = Array{proto.array().bytes_to_zero(),
                              proto.array().ptr_divisibility()};
          break;
        case TritonKernelCall_Parameter::kBool:
          param.value = proto.bool_();
          break;
        case TritonKernelCall_Parameter::kI32:
          param.value = proto.i32();
          break;
        case TritonKernelCall_Parameter::kU32:
          param.value = proto.u32();
          break;
        case TritonKernelCall_Parameter::kI64:
          param.value = proto.i64();
          break;
        case TritonKernelCall_Parameter::kU64:
          param.value = proto.u64();
          break;
        default:
          return absl::InvalidArgumentError("Unknown scalar parameter type.");
      }
      return param;
    }

    TritonKernelCall_Parameter ToProto() const {
      TritonKernelCall_Parameter proto;
      if (std::holds_alternative<Array>(value)) {
        proto.mutable_array()->set_bytes_to_zero(
            std::get<Array>(value).bytes_to_zero);
        proto.mutable_array()->set_ptr_divisibility(
            std::get<Array>(value).ptr_divisibility);
      } else if (std::holds_alternative<bool>(value)) {
        proto.set_bool_(std::get<bool>(value));
      } else if (std::holds_alternative<int32_t>(value)) {
        proto.set_i32(std::get<int32_t>(value));
      } else if (std::holds_alternative<uint32_t>(value)) {
        proto.set_u32(std::get<uint32_t>(value));
      } else if (std::holds_alternative<int64_t>(value)) {
        proto.set_i64(std::get<int64_t>(value));
      } else {
        CHECK(std::holds_alternative<uint64_t>(value));
        proto.set_u64(std::get<uint64_t>(value));
      }
      return proto;
    }

    std::variant<Array, bool, int32_t, uint32_t, int64_t, uint64_t> value;
  };

  KernelCall(Kernel kernel, uint32_t grid_0, uint32_t grid_1, uint32_t grid_2,
             std::vector<Parameter> parameters)
      : kernel_(std::move(kernel)),
        grid_{grid_0, grid_1, grid_2},
        parameters_(std::move(parameters)) {}

  absl::Status Launch(CUstream stream, void** buffers) override final {
    std::vector<void*> params;
    params.reserve(parameters_.size());
    for (size_t i = 0; i < parameters_.size(); ++i) {
      const Parameter& param = parameters_[i];
      if (std::holds_alternative<Parameter::Array>(param.value)) {
        const auto& array = std::get<Parameter::Array>(param.value);
        void*& ptr = *(buffers++);
        auto cu_ptr = reinterpret_cast<CUdeviceptr>(ptr);

        if (ABSL_PREDICT_FALSE((array.ptr_divisibility != 0) &&
                               (cu_ptr % array.ptr_divisibility != 0))) {
          return absl::InvalidArgumentError(
              absl::StrFormat("Parameter %zu (%p) is not divisible by %d.", i,
                              ptr, array.ptr_divisibility));
        }

        if (array.bytes_to_zero > 0) {
          CUDA_RETURN_IF_ERROR(
              cuMemsetD8Async(cu_ptr, 0, array.bytes_to_zero, stream));
        }
        params.push_back(&ptr);
      } else {
        params.push_back(const_cast<void*>(std::visit(
            [](auto&& arg) { return reinterpret_cast<const void*>(&arg); },
            param.value)));
      }
    }

    return kernel_.Launch(stream, grid_, params.data());
  }

  static absl::StatusOr<KernelCall> FromProto(const TritonKernelCall& proto) {
    std::vector<KernelCall::Parameter> parameters;
    for (const TritonKernelCall_Parameter& parameter : proto.parameters()) {
      JAX_ASSIGN_OR_RETURN(Parameter p, Parameter::FromProto(parameter));
      parameters.push_back(p);
    }

    return KernelCall(Kernel::FromProto(proto.kernel()), proto.grid_0(),
                      proto.grid_1(), proto.grid_2(), std::move(parameters));
  }

  TritonKernelCall ToProto() const {
    TritonKernelCall proto;
    *proto.mutable_kernel() = kernel_.ToProto();
    proto.set_grid_0(grid_[0]);
    proto.set_grid_1(grid_[1]);
    proto.set_grid_2(grid_[2]);
    for (const Parameter& param : parameters_) {
      *proto.add_parameters() = param.ToProto();
    }
    return proto;
  }

 private:
  Kernel kernel_;
  uint32_t grid_[3];
  std::vector<Parameter> parameters_;
};

class AutotunedKernelCall : public KernelCallBase {
 public:
  struct Config {
    KernelCall kernel_call;
    std::string description;
  };

  AutotunedKernelCall(
      std::string name, std::vector<Config> configs,
      std::vector<std::tuple<size_t, size_t, size_t>> input_output_aliases)
      : name_(std::move(name)),
        configs_(std::move(configs)),
        input_output_aliases_(std::move(input_output_aliases)) {}

  absl::Status Launch(CUstream stream, void** buffers) override {
    absl::call_once(autotune_once_, [=]() {
      if (configs_.size() > 1) {
        autotune_status_ = Autotune(stream, buffers);
      }
    });
    JAX_RETURN_IF_ERROR(autotune_status_);
    return configs_[0].kernel_call.Launch(stream, buffers);
  }

  static absl::StatusOr<std::unique_ptr<AutotunedKernelCall>> FromProto(
      const TritonAutotunedKernelCall& proto) {
    std::vector<Config> configs;
    for (const TritonAutotunedKernelCall_Config& config : proto.configs()) {
      JAX_ASSIGN_OR_RETURN(auto kernel_call,
                           KernelCall::FromProto(config.kernel_call()));
      configs.push_back(Config{std::move(kernel_call), config.description()});
    }

    std::vector<std::tuple<size_t, size_t, size_t>> input_output_aliases;
    for (const TritonAutotunedKernelCall_InputOutputAlias& a :
         proto.input_output_aliases()) {
      input_output_aliases.push_back(std::make_tuple(
          a.input_buffer_idx(), a.output_buffer_idx(), a.buffer_size_bytes()));
    }

    return std::make_unique<AutotunedKernelCall>(
        proto.name(), std::move(configs), std::move(input_output_aliases));
  }

  TritonAutotunedKernelCall ToProto() const {
    TritonAutotunedKernelCall proto;
    proto.set_name(name_);
    for (const Config& config : configs_) {
      TritonAutotunedKernelCall_Config* c = proto.add_configs();
      *c->mutable_kernel_call() = config.kernel_call.ToProto();
      c->set_description(config.description);
    }
    for (const auto& [input_idx, output_idx, size] : input_output_aliases_) {
      TritonAutotunedKernelCall_InputOutputAlias* a =
          proto.add_input_output_aliases();
      a->set_input_buffer_idx(input_idx);
      a->set_output_buffer_idx(output_idx);
      a->set_buffer_size_bytes(size);
    }
    return proto;
  }

 private:
  static constexpr float kBenchmarkTimeMillis = 10.;

  absl::Status Autotune(CUstream stream, void** buffers) {
    // Ensure a valid context for driver calls that don't take the stream.
    CUcontext context;
    CUDA_RETURN_IF_ERROR(cuStreamGetCtx(stream, &context));
    CUDA_RETURN_IF_ERROR(cuCtxPushCurrent(context));
    absl::Cleanup ctx_restorer = [] { cuCtxPopCurrent(nullptr); };

    // If an input aliases with an output, it will get overwritten during the
    // kernel execution. If the kernel is called repeatedly, as we do during
    // auto-tuning, the final result will be junk, so we take a copy of the
    // input to restore after auto-tuning.
    std::unordered_map<size_t, std::vector<uint8_t>> input_copies;
    for (auto [input_idx, output_idx, size] : input_output_aliases_) {
      if (buffers[input_idx] == buffers[output_idx]) {
        std::vector<uint8_t> input_copy(size);
        CUDA_RETURN_IF_ERROR(cuMemcpyDtoHAsync(
            input_copy.data(),
            reinterpret_cast<CUdeviceptr>(buffers[input_idx]), size, stream));
        input_copies[input_idx] = std::move(input_copy);
      }
    }

    LOG(INFO) << "Autotuning function: " << name_;
    // First run a single iteration of each to config to determine how many
    // iterations to run for benchmarking.
    float best = std::numeric_limits<float>::infinity();
    for (Config& config : configs_) {
      JAX_ASSIGN_OR_RETURN(float t,
                           Benchmark(stream, config.kernel_call, buffers, 1));
      LOG(INFO) << config.description << ", ran 1 iter in " << t << " ms";
      best = std::min(best, t);
    }

    int timed_iters =
        std::max(static_cast<int>(kBenchmarkTimeMillis / best), 1);
    if (timed_iters > 100) {
      timed_iters = 100;
      LOG(INFO) << "Benchmarking with 100 iters (capped at 100)";
    } else {
      timed_iters = std::min(timed_iters, 100);
      LOG(INFO) << "Benchmarking with " << timed_iters
                << " iters (target time: " << kBenchmarkTimeMillis << " ms)";
    }

    best = std::numeric_limits<float>::infinity();
    for (Config& config : configs_) {
      JAX_ASSIGN_OR_RETURN(
          float t, Benchmark(stream, config.kernel_call, buffers, timed_iters));
      LOG(INFO) << config.description << ", ran " << timed_iters << " iters in "
                << t << " ms";

      if (t < best) {
        LOG(INFO) << config.description << " is the new best config";
        best = t;
        std::swap(config, configs_[0]);
      }
    }

    // Discard all but the best config.
    configs_.erase(configs_.begin() + 1, configs_.end());

    LOG(INFO) << "Finished autotuning function: " << name_ << " best config "
              << configs_[0].description;

    // Restore aliased inputs to their original values.
    for (auto [input_idx, _, size] : input_output_aliases_) {
      CUDA_RETURN_IF_ERROR(
          cuMemcpyHtoDAsync(reinterpret_cast<CUdeviceptr>(buffers[input_idx]),
                            input_copies[input_idx].data(), size, stream));
    }
    // Synchronize stream to ensure copies are complete before the host copy
    // is deleted.
    return JAX_AS_STATUS(cuStreamSynchronize(stream));
  }

  absl::StatusOr<float> Benchmark(CUstream stream, KernelCall& kernel_call,
                                  void** buffers, int num_iterations) {
    CUevent start, stop;
    CUDA_RETURN_IF_ERROR(cuEventCreate(&start, /*Flags=*/CU_EVENT_DEFAULT));
    CUDA_RETURN_IF_ERROR(cuEventCreate(&stop, /*Flags=*/CU_EVENT_DEFAULT));
    JAX_RETURN_IF_ERROR(kernel_call.Launch(stream, buffers));  // Warm-up.
    CUDA_RETURN_IF_ERROR(cuEventRecord(start, stream));
    for (int i = 0; i < num_iterations; ++i) {
      JAX_RETURN_IF_ERROR(kernel_call.Launch(stream, buffers));
    }
    CUDA_RETURN_IF_ERROR(cuEventRecord(stop, stream));
    CUDA_RETURN_IF_ERROR(cuEventSynchronize(stop));
    float elapsed_ms;
    CUDA_RETURN_IF_ERROR(cuEventElapsedTime(&elapsed_ms, start, stop));
    CUDA_RETURN_IF_ERROR(cuEventDestroy(start));
    CUDA_RETURN_IF_ERROR(cuEventDestroy(stop));
    return elapsed_ms;
  }

  std::string name_;
  // After auto-tuning, all configurations, except the best, will be discarded.
  std::vector<Config> configs_;
  // (input buffer idx, output buffer idx, size)
  std::vector<std::tuple<size_t, size_t, size_t>> input_output_aliases_;
  absl::once_flag autotune_once_;
  absl::Status autotune_status_;
};

absl::StatusOr<std::string> ZlibUncompress(absl::string_view compressed) {
  std::string data;
  uLongf dest_len = 5 * compressed.size();
  while (true) {
    data.resize(dest_len);
    int ret = uncompress(reinterpret_cast<Bytef*>(data.data()), &dest_len,
                         reinterpret_cast<const Bytef*>(compressed.data()),
                         compressed.size());
    if (ret == Z_OK) {
      // `uncompress` overwrites `dest_len` with the uncompressed size.
      data.resize(dest_len);
      break;
    } else if (ret == Z_BUF_ERROR) {
      dest_len *= 2;  // The string buffer wasn't large enough.
    } else {
      return absl::InvalidArgumentError("Failed to uncompress opaque data.");
    }
  }
  return data;
}

absl::StatusOr<KernelCallBase*> GetKernelCall(absl::string_view opaque) {
  static absl::Mutex mutex;
  static auto& kernel_calls =
      *new absl::flat_hash_map<std::string, std::unique_ptr<KernelCallBase>>
          ABSL_GUARDED_BY(mutex);

  absl::MutexLock lock(&mutex);
  auto it = kernel_calls.find(opaque);
  if (ABSL_PREDICT_TRUE(it != kernel_calls.end())) return it->second.get();

  // The opaque data is a zlib compressed protobuf.
  JAX_ASSIGN_OR_RETURN(std::string serialized, ZlibUncompress(opaque));

  TritonAnyKernelCall proto;
  if (!proto.ParseFromString(serialized)) {
    return absl::InvalidArgumentError("Failed to parse serialized data.");
  }

  std::unique_ptr<KernelCallBase> kernel_call;
  if (proto.has_kernel_call()) {
    JAX_ASSIGN_OR_RETURN(auto kernel_call_,
                         KernelCall::FromProto(proto.kernel_call()));
    kernel_call = std::make_unique<KernelCall>(std::move(kernel_call_));
  } else if (proto.has_autotuned_kernel_call()) {
    JAX_ASSIGN_OR_RETURN(kernel_call, AutotunedKernelCall::FromProto(
                                          proto.autotuned_kernel_call()));
  } else {
    return absl::InvalidArgumentError("Unknown kernel call type.");
  }

  auto [it2, success] =
      kernel_calls.insert({std::string(opaque), std::move(kernel_call)});
  CHECK(success);
  return it2->second.get();
}

}  // namespace

void LaunchTritonKernel(CUstream stream, void** buffers, const char* opaque,
                        size_t opaque_len, XlaCustomCallStatus* status) {
  absl::Status result = [=] {
    JAX_ASSIGN_OR_RETURN(KernelCallBase * kernel_call,
                         GetKernelCall(absl::string_view(opaque, opaque_len)));
    return kernel_call->Launch(stream, buffers);
  }();
  if (!result.ok()) {
    absl::string_view msg = result.message();
    XlaCustomCallStatusSetFailure(status, msg.data(), msg.length());
  }
}

PYBIND11_MODULE(_triton, m) {
  py::class_<Kernel>(m, "TritonKernel")
      .def(py::init<std::string, uint32_t, uint32_t, std::string, std::string,
                    int>());

  py::class_<KernelCall::Parameter>(m, "TritonParameter");

  m.def("create_array_parameter",
        [](size_t bytes_to_zero, size_t ptr_divisibility) {
          return KernelCall::Parameter{
              KernelCall::Parameter::Array{bytes_to_zero, ptr_divisibility}};
        });

  m.def("create_scalar_parameter",
        [](py::bool_ value,
           std::string_view dtype) -> absl::StatusOr<KernelCall::Parameter> {
          if ((dtype == "int1") || (dtype == "B")) {
            return KernelCall::Parameter{static_cast<bool>(value)};
          } else {
            return absl::InvalidArgumentError(std::string("unknown dtype: ") +
                                              dtype.data());
          }
        });

  m.def("create_scalar_parameter",
        [](py::int_ value,
           std::string_view dtype) -> absl::StatusOr<KernelCall::Parameter> {
          if (dtype == "i32") {
            return KernelCall::Parameter{static_cast<int32_t>(value)};
          } else if (dtype == "u32") {
            return KernelCall::Parameter{static_cast<uint32_t>(value)};
          } else if (dtype == "i64") {
            return KernelCall::Parameter{static_cast<int64_t>(value)};
          } else if (dtype == "u64") {
            return KernelCall::Parameter{static_cast<uint64_t>(value)};
          } else {
            return absl::InvalidArgumentError(std::string("unknown dtype: ") +
                                              dtype.data());
          }
        });

  py::class_<KernelCall>(m, "TritonKernelCall")
      .def(py::init<Kernel, uint32_t, uint32_t, uint32_t,
                    std::vector<KernelCall::Parameter>>())
      .def("to_proto", [](const KernelCall& kernel_call, std::string metadata) {
        TritonAnyKernelCall proto;
        *proto.mutable_kernel_call() = kernel_call.ToProto();
        proto.set_metadata(std::move(metadata));
        return py::bytes(proto.SerializeAsString());
      });

  py::class_<AutotunedKernelCall>(m, "TritonAutotunedKernelCall")
      .def(py::init<>([](std::string name,
                         std::vector<std::pair<KernelCall, std::string>>
                             calls_and_descriptions,
                         std::vector<std::tuple<size_t, size_t, size_t>>
                             input_output_aliases) {
        std::vector<AutotunedKernelCall::Config> configs;
        configs.reserve(calls_and_descriptions.size());
        for (auto& [kernel_call, desc] : calls_and_descriptions) {
          configs.push_back({std::move(kernel_call), std::move(desc)});
        }
        return std::make_unique<AutotunedKernelCall>(
            std::move(name), std::move(configs),
            std::move(input_output_aliases));
      }))
      .def("to_proto",
           [](const AutotunedKernelCall& kernel_call, std::string metadata) {
             TritonAnyKernelCall proto;
             *proto.mutable_autotuned_kernel_call() = kernel_call.ToProto();
             proto.set_metadata(std::move(metadata));
             return py::bytes(proto.SerializeAsString());
           });

  m.def("get_custom_call",
        [] { return jax::EncapsulateFunction(&LaunchTritonKernel); });

  m.def("get_compute_capability", [](int device) -> absl::StatusOr<int> {
    int major, minor;
    CUDA_RETURN_IF_ERROR(cuInit(device));
    CUDA_RETURN_IF_ERROR(cuDeviceGetAttribute(
        &major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device));
    CUDA_RETURN_IF_ERROR(cuDeviceGetAttribute(
        &minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device));
    return major * 10 + minor;
  });

  m.def(
      "get_serialized_metadata",
      [](absl::string_view opaque) -> absl::StatusOr<py::bytes> {
        JAX_ASSIGN_OR_RETURN(std::string serialized, ZlibUncompress(opaque));
        TritonAnyKernelCall proto;
        if (!proto.ParseFromString(serialized)) {
          return absl::InvalidArgumentError("Failed to parse serialized data.");
        }
        return py::bytes(proto.metadata());
      });
}

}  // namespace jax_triton
