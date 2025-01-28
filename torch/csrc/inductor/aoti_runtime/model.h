#pragma once

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <optional>
#include <regex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

// WARNING: Be careful when adding new includes here. This header will be used
// in model.so, and should not refer to any aten/c10 headers except the stable
// C ABI defined in torch/csrc/inductor/aoti_torch/c/shim.h. The same rule
// applies to other files under torch/csrc/inductor/aoti_runtime/.
#include <torch/csrc/inductor/aoti_runtime/device_utils.h>
#ifdef USE_XPU
#include <torch/csrc/inductor/aoti_runtime/utils_xpu.h>
#else
#include <torch/csrc/inductor/aoti_runtime/utils.h>
#endif

#define AOTI_RUNTIME_CHECK(EXPR, MSG) \
  do {                                \
    bool ok = EXPR;                   \
    if (!ok) {                        \
      throw std::runtime_error(MSG);  \
    }                                 \
  } while (0)

// At codegen time, we write out a binary file called constants.bin.
// We then turn the raw binary to an object file that exposes this
// symbol and link it into the final .so.
// For information on the binary format, see `man objcopy`, under
// the "binary-architecture" flag:
// https://man7.org/linux/man-pages/man1/objcopy.1.html
// todo: use #embed in C++ 23 once available
// The constants are NOT readonly because they may be mutated.
// NOLINTNEXTLINE(*array*)
extern uint8_t _binary_constants_bin_start[];
// NOLINTNEXTLINE(*array*)
extern uint8_t _binary_constants_bin_end[];

#define AOTI_CONST_GPU_ALIGNMENT 64

namespace {

using RAIIDataPtr = std::unique_ptr<void, std::function<void(void*)>>;

#ifdef USE_CUDA

RAIIDataPtr RAII_gpuMalloc(size_t num_bytes) {
  void* data_ptr;
  AOTI_RUNTIME_DEVICE_CHECK(cudaMalloc((void**)&data_ptr, num_bytes));
  auto deleter = [](void* ptr) { AOTI_RUNTIME_DEVICE_CHECK(cudaFree(ptr)); };
  return RAIIDataPtr(data_ptr, deleter);
}

#endif // USE_CUDA

#ifdef USE_XPU

RAIIDataPtr RAII_gpuMalloc(size_t num_bytes) {
  sycl::queue* queue_ptr = nullptr;
  aoti_torch_get_current_sycl_queue((void**)&queue_ptr);
  void* data_ptr = sycl::malloc_device(num_bytes, *queue_ptr);
  auto deleter = [queue_ptr](void* ptr) { sycl::free(ptr, *queue_ptr); };
  return RAIIDataPtr(data_ptr, deleter);
}

#endif // USE_XPU

} // anonymous namespace

namespace torch::aot_inductor {
enum ConstantType : uint8_t {
  Unknown = 0,
  Parameter = 1,
  Buffer = 2,
  TensorConstant = 3,
  FoldedConstant = 4,
};

using ConstantMap = std::unordered_map<std::string, RAIIAtenTensorHandle>;

// valid device strs are: cpu, cuda, cuda:0, cuda:1, ...
// Update the list here if more devices are supported in the future
inline void parse_device_str(
    const std::string& device_str,
    int32_t& device_type,
    int32_t& device_idx) {
  std::regex re("(cpu|cuda|xpu)(:([0-9]+))?");
  std::smatch sm;
  bool matched = std::regex_match(device_str, sm, re);
  AOTI_RUNTIME_CHECK(matched, "Invalid device: " + device_str);

  if (sm[1].str() == "cpu") {
    device_type = aoti_torch_device_type_cpu();
  } else if (sm[1].str() == "cuda") {
    device_type = aoti_torch_device_type_cuda();
#ifdef USE_XPU
  } else if (sm[1].str() == "xpu") {
    device_type = aoti_torch_device_type_xpu();
#endif
  } else {
    AOTI_RUNTIME_CHECK(false, "Invalid device: " + device_str);
  }

  if (sm[3].matched) {
    device_idx = stoi(sm[3].str());
  } else {
    device_idx = -1;
  }
}

// Defines the base class for AOTInductorModel, which is generated by the
// AOTInductor cpp codegen. Since we do not need dynamic dispatch, we rely
// on curiously recurring template pattern (CRTP) to save some runtime
// v-table overhead. The generated AOTInductorModel is specialized with
// methods such as run_impl.
template <typename Model>
class AOTInductorModelBase {
 public:
  AOTInductorModelBase(
      size_t num_inputs,
      size_t num_outputs,
      size_t num_constants,
      const std::string& device_str,
      std::optional<std::string> cubin_dir,
      bool include_weights = true)
      : inputs_info_(num_inputs),
        outputs_info_(num_outputs),
        constants_info_(num_constants),
        cubin_dir_(std::move(cubin_dir)),
        include_weights(include_weights) {
    parse_device_str(device_str, device_type_, device_idx_);

#ifdef USE_CUDA
    if (device_idx_ == -1) {
      AOTI_RUNTIME_DEVICE_CHECK(cudaGetDevice(&device_idx_));
    } else {
      // If device_idx_ is passed in, we need to set the current device to it
      AOTI_RUNTIME_DEVICE_CHECK(cudaSetDevice(device_idx_));
    }
#endif // USE_CUDA
#ifdef USE_XPU
    if (device_idx_ == -1) {
      aoti_torch_get_current_xpu_device(&device_idx_);
    } else {
      aoti_torch_set_current_xpu_device(device_idx_);
    }
#endif // USE_XPU
  }

  // NOLINTNEXTLINE(modernize-use-equals-default)
  ~AOTInductorModelBase() {
#ifdef USE_CUDA
    if (run_finished_) {
      auto code = cudaEventDestroy(*run_finished_);
      if (code != cudaSuccess) {
        std::cerr << "Failed to destroy CUDA event in AOTInductor model: "
                  << cudaGetErrorString(code) << std::endl;
      }
    }
#endif // USE_CUDA
#ifdef USE_XPU
    if (run_finished_) {
      (*run_finished_)->wait_and_throw();
      delete *run_finished_;
    }
#endif // USE_XPU
  }

  AOTInductorModelBase(AOTInductorModelBase&&) = delete;
  AOTInductorModelBase& operator=(AOTInductorModelBase&&) = delete;
  AOTInductorModelBase(const AOTInductorModelBase&) = delete;
  AOTInductorModelBase& operator=(const AOTInductorModelBase&) = delete;

  void run(
      AtenTensorHandle*
          input_handles, // array of input AtenTensorHandle; handles
                         // are stolen; the array itself is borrowed
      AtenTensorHandle*
          output_handles, // array for writing output AtenTensorHandle; handles
                          // will be stolen by the caller; the array itself is
                          // borrowed
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor) {
#ifdef USE_CUDA
    if (!run_finished_) {
      cudaEvent_t run_finished;
      AOTI_RUNTIME_DEVICE_CHECK(cudaEventCreate(&run_finished));
      run_finished_.emplace(run_finished);
    }
#elif defined(USE_XPU)
    if (run_finished_) {
      (*run_finished_)->wait_and_throw();
      delete *run_finished_;
      run_finished_.reset();
    }
#else // !USE_CUDA && !USE_XPU
    run_finished_ = false;
#endif

    auto* model = static_cast<Model*>(this);
    model->run_impl(input_handles, output_handles, stream, proxy_executor);

#ifdef USE_CUDA
    AOTI_RUNTIME_DEVICE_CHECK(cudaEventRecord(*run_finished_, stream));
#elif defined(USE_XPU)
    run_finished_ = std::make_optional<sycl::event*>(new sycl::event(
        static_cast<sycl::queue*>(stream)->ext_oneapi_submit_barrier()));
#else // !USE_CUDA && !USE_XPU
    run_finished_ = true;
#endif // USE_CUDA
  }

  std::unordered_map<std::string, AtenTensorHandle> run_const_fold(
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor,
      bool initialization = false) {
#ifdef USE_CUDA
    if (!run_finished_) {
      cudaEvent_t run_finished;
      AOTI_RUNTIME_DEVICE_CHECK(cudaEventCreate(&run_finished));
      run_finished_.emplace(run_finished);
    }
#elif defined(USE_XPU)
    if (run_finished_) {
      (*run_finished_)->wait_and_throw();
      delete *run_finished_;
      run_finished_.reset();
    }
#else // !USE_CUDA && !USE_XPU
    run_finished_ = false;
#endif

    auto* model = static_cast<Model*>(this);
    auto folded_constants =
        model->const_run_impl(stream, proxy_executor, initialization);

#ifdef USE_CUDA
    AOTI_RUNTIME_DEVICE_CHECK(cudaEventRecord(*run_finished_, stream));
#elif defined(USE_XPU)
    // sycl::queue* queue_ptr = nullptr;
    // aoti_torch_get_current_sycl_queue((void**)&queue_ptr);
    run_finished_ = std::make_optional<sycl::event*>(new sycl::event(
        static_cast<sycl::queue*>(stream)->ext_oneapi_submit_barrier()));

#else // !USE_CUDA && !USE_XPU
    run_finished_ = true;
#endif // USE_CUDA

    return folded_constants;
  }

  void load_constants() {
    size_t num_constants = this->num_constants();
    constants_map_->reserve(num_constants);

    std::vector<size_t> constants_internal_offset(num_constants);
    if (device_type_ != aoti_torch_device_type_cpu()) {
      size_t blob_size = 0;
      compute_gpu_constant_blob(blob_size, constants_internal_offset);
#if defined(USE_CUDA) || defined(USE_XPU)
      constant_blob_ = RAII_gpuMalloc(blob_size);
#endif
    }
    if (!include_weights) {
      return;
    }

    size_t bytes_read = 0;
    for (size_t i = 0; i < num_constants; i++) {
      bool from_folded = this->constant_from_folded(i);
#if not defined(USE_XPU) && not defined(USE_CUDA)
      if (from_folded) {
        // We do not reallocate and copy for CPU.
        continue;
      }
#endif // USE_CUDA
      std::string name = this->constant_name(i);
      size_t data_size = this->constant_data_size(i);
      uint8_t* internal_ptr = (data_size != 0)
          ? constant_ptr(
                constants_internal_offset[i],
                bytes_read,
                data_size,
                from_folded)
          : nullptr;
      bytes_read += data_size;

      // Create at::Tensor from copied memory.
      auto dtype = this->constant_dtype(i);
      auto ndim = this->constant_ndim(i);
      auto size = this->constant_shape(i);
      auto stride = this->constant_stride(i);
      auto offset = this->constant_offset(i);
      auto layout = this->constant_layout(i);
      auto opaque_metadata_ptr = this->opaque_metadata(i);
      auto opaque_metadata_size = this->opaque_metadata_size(i);

      AtenTensorHandle tensor_handle = nullptr;
#ifdef AOTI_USE_CREATE_TENSOR_FROM_BLOB_V1
      // When opaque_metadata_size is not 0, we need to have the
      // aoti_torch_create_tensor_from_blob_v2 available
      AOTI_RUNTIME_CHECK(
          opaque_metadata_size == 0,
          "Expect opaque_metadata_size to be 0 when AOTI_USE_CREATE_TENSOR_FROM_BLOB_V1 is defined");
      AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_create_tensor_from_blob(
          internal_ptr,
          ndim,
          size,
          stride,
          offset,
          dtype,
          device_type_,
          device_idx_,
          &tensor_handle));
#else
      AOTI_TORCH_ERROR_CODE_CHECK(aoti_torch_create_tensor_from_blob_v2(
          internal_ptr,
          ndim,
          size,
          stride,
          offset,
          dtype,
          device_type_,
          device_idx_,
          &tensor_handle,
          layout,
          opaque_metadata_ptr,
          opaque_metadata_size));
#endif // AOTI_USE_CREATE_TENSOR_FROM_BLOB_V1
      constants_map_->emplace(std::move(name), tensor_handle);
    }
    if (constants_map_) {
      this->update_constants_array_from_map();
    }
  }

  RAIIDataPtr&& release_constant_blob() {
    return std::move(constant_blob_);
  }

  std::shared_ptr<std::vector<ConstantHandle>> get_constants_array() {
    return constants_;
  }

  int32_t get_device_type() const {
    return device_type_;
  }

  int32_t get_device_idx() const {
    return device_idx_;
  }

  uint8_t* constant_ptr(
      size_t constant_offset,
      size_t bytes_read,
      size_t data_size,
      bool skip_copy) {
#if defined(USE_CUDA) || defined(USE_XPU)
    auto* constants_ptr = static_cast<uint8_t*>(constant_blob_.get());
    uint8_t* internal_ptr = constants_ptr + constant_offset;
    // Copy data to GPU memory
    // TODO: Handle shared storage case.
    if (!skip_copy) {
#ifdef USE_XPU
      sycl::queue* queue_ptr = nullptr;
      aoti_torch_get_current_sycl_queue((void**)&queue_ptr);
      queue_ptr
          ->memcpy(internal_ptr, _get_constants_start() + bytes_read, data_size)
          .wait();

#else
      AOTI_RUNTIME_DEVICE_CHECK(cudaMemcpy(
          internal_ptr,
          _get_constants_start() + bytes_read,
          data_size,
          cudaMemcpyHostToDevice));
#endif
    }
    return internal_ptr;

#else
    // get pointer to constant which is packed in model during compile time.
    AOTI_RUNTIME_CHECK(!skip_copy, "pure cpu mode doesn't support skip copy");
    return _get_constants_start() + bytes_read;
#endif // USE_CUDA
  }

  void compute_gpu_constant_blob(
      size_t& blob_size,
      std::vector<size_t>& constants_internal_offset) {
#if defined(USE_CUDA) || defined(USE_XPU)
    size_t num_constants = this->num_constants();
    // Compute required blob size with 64-alignment if on GPU.
    blob_size = 0;
    for (size_t i = 0; i < num_constants; i++) {
      size_t data_size = this->constant_data_size(i);
      if (data_size % AOTI_CONST_GPU_ALIGNMENT) {
        data_size = AOTI_CONST_GPU_ALIGNMENT +
            (data_size / AOTI_CONST_GPU_ALIGNMENT) * AOTI_CONST_GPU_ALIGNMENT;
      }
      constants_internal_offset[i] = blob_size;
      blob_size += data_size;
    }
#endif // USE_CUDA
  }

  size_t num_inputs() const {
    return inputs_info_.size();
  }

  size_t num_outputs() const {
    return outputs_info_.size();
  }

  size_t num_constants() const {
    return constants_info_.size();
  }

  const char* input_name(int64_t idx) const {
    return inputs_info_.at(idx).name;
  }

  const char* output_name(int64_t idx) const {
    return outputs_info_.at(idx).name;
  }

  const char* constant_name(int64_t idx) const {
    return constants_info_.at(idx).name;
  }

  size_t constant_ndim(int64_t idx) {
    return constants_info_.at(idx).shape.size();
  }

  const int64_t* constant_shape(int64_t idx) const {
    return constants_info_.at(idx).shape.data();
  }

  const int64_t* constant_stride(int64_t idx) const {
    return constants_info_.at(idx).stride.data();
  }

  int32_t constant_dtype(int64_t idx) const {
    return constants_info_.at(idx).dtype;
  }

  int32_t constant_layout(int64_t idx) const {
    return constants_info_.at(idx).layout;
  }

  size_t constant_offset(int64_t idx) const {
    return constants_info_.at(idx).offset;
  }

  size_t constant_data_size(int64_t idx) const {
    return constants_info_.at(idx).data_size;
  }

  const char* constant_original_fqn(int64_t idx) const {
    return constants_info_.at(idx).original_fqn;
  }

  const uint8_t* opaque_metadata(int64_t idx) const {
    return constants_info_.at(idx).opaque_metadata.data();
  }

  size_t opaque_metadata_size(int64_t idx) {
    return constants_info_.at(idx).opaque_metadata.size();
  }

  bool constant_from_folded(int64_t idx) const {
    return constants_info_.at(idx).from_folded;
  }

  int32_t constant_type(int64_t idx) const {
    return constants_info_.at(idx).type;
  }

  const char* get_in_spec() const {
    return in_spec_.c_str();
  }

  const char* get_out_spec() const {
    return out_spec_.c_str();
  }

  void update_constants_array_from_map() {
    if (!constants_map_) {
      throw std::runtime_error{
          "constants_map_ was not ready when constants_ is trying to be constructed from it!"};
    }
    if (!constants_) {
      constants_ =
          std::make_shared<std::vector<ConstantHandle>>(constants_info_.size());
    } else {
      constants_->resize(constants_info_.size());
    }
    int idx = 0;
    for (const auto& info : constants_info_) {
      const auto it = constants_map_->find(info.name);
      if (it != constants_map_->end()) {
        constants_->at(idx) = ConstantHandle(it->second);
      }
      idx++;
    }
  }

  void update_constants_map(
      std::shared_ptr<ConstantMap> constants_map,
      bool remap_constants_array = true) {
    constants_map_ = std::move(constants_map);
    if (remap_constants_array) {
      update_constants_array_from_map();
    }
  }

  // This function allows us to update the constants_ that is used to look up
  // the corresponding constant tensor during runtime.
  void update_constants_array(
      std::shared_ptr<std::vector<ConstantHandle>> constants_array) {
    constants_ = std::move(constants_array);
  }

  /// Returns true if the model is complete.
  bool is_finished() {
#ifdef USE_CUDA
    if (!run_finished_) {
      throw std::runtime_error{"Model CUDA event was not initialized"};
    }

    auto event_status = cudaEventQuery(*run_finished_);
    if (event_status == cudaSuccess) {
      return true;
    } else if (event_status == cudaErrorNotReady) {
      return false;
    }

    throw std::runtime_error(
        std::string("The model did not finish successfully. Error: ") +
        cudaGetErrorString(cudaGetLastError()));
#elif defined(USE_XPU)
    if (!run_finished_) {
      throw std::runtime_error{"Model XPU event was not initialized"};
    }
    using namespace sycl::info;
    return (*run_finished_)->get_info<event::command_execution_status>() ==
        event_command_status::complete;

#else // !USE_CUDA && !USE_XPU
    return run_finished_;
#endif // USE_CUDA
  }

  /// Synchronizes completion event.
  void wait_for_completion() {
#ifdef USE_CUDA
    if (!run_finished_) {
      throw std::runtime_error{"Model event was not initialized"};
    }

    AOTI_RUNTIME_DEVICE_CHECK(cudaEventSynchronize(*run_finished_));
#endif // USE_CUDA
#ifdef USE_XPU
    if (!run_finished_) {
      throw std::runtime_error{"Model event was not initialized"};
    }
    (*run_finished_)->wait_and_throw();
#endif
  }

 protected:
  uint8_t* _get_constants_start() {
#ifndef USE_MMAP_SELF
    // NOLINTNEXTLINE(*const-cast*)
    return const_cast<uint8_t*>(_binary_constants_bin_start);
#else
    if (self_mmap) {
      return self_mmap;
    }
    Dl_info dl_info;
    // get pointer to constant which are appended to the binary
    AOTI_RUNTIME_CHECK(
        dladdr(__func__, &dl_info), "Can't find shared library name");
    int fd = open(dl_info.dli_fname, O_RDONLY);
    AOTI_RUNTIME_CHECK(fd >= 0, "Shared library file cannot be opened");
    auto fsize = lseek(fd, 0, SEEK_END);
    auto weights_size =
        reinterpret_cast<const uint64_t*>(_binary_constants_bin_start)[0];
    auto magic_number =
        reinterpret_cast<const uint64_t*>(_binary_constants_bin_start)[1];
    auto weights_offset = fsize - weights_size;
    AOTI_RUNTIME_CHECK(
        (weights_offset & 0x3fff) == 0,
        "weights_offset must be aligned to 16K boundary");
    auto ptr = mmap(
        NULL,
        weights_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE,
        fd,
        weights_offset);
    close(fd);
    AOTI_RUNTIME_CHECK(ptr != MAP_FAILED, "mmap() failed");
    self_mmap = static_cast<uint8_t*>(ptr);
    AOTI_RUNTIME_CHECK(
        reinterpret_cast<uint64_t*>(
            self_mmap + weights_size - sizeof(uint64_t))[0] == magic_number,
        "Weigths data seems corrupt");
    return self_mmap;
#endif
  }
  struct ParamInfo {
    const char* name = nullptr;
  };

  struct ConstInfo {
    const char* name = nullptr;
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
    int32_t dtype{};
    int64_t offset{};
    size_t data_size{};
    int32_t layout{};
    std::vector<uint8_t> opaque_metadata;
    int64_t opaque_metadata_size{};
    const char* original_fqn = nullptr;
    bool from_folded{};
    int32_t type{};
  };

  std::vector<ParamInfo> inputs_info_;
  std::vector<ParamInfo> outputs_info_;
  std::vector<ConstInfo> constants_info_;
  std::string in_spec_;
  std::string out_spec_;

  std::shared_ptr<ConstantMap> constants_map_;
  std::shared_ptr<std::vector<ConstantHandle>> constants_;

  // Holds the blob storage for constants' at::Tensor.
  RAIIDataPtr constant_blob_;

#ifdef USE_MMAP_SELF
  uint8_t* self_mmap = NULL;
#endif

  // A directory with CUDA binary files, e.g. compiled kernels, etc.
  const std::optional<std::string> cubin_dir_;

  // This is the flag that implies whether the weight is included in the model.
  // If True, we would prepare the weight when loading the model, otherwise the
  // model will be loaded without weights, and need to be provided by the user.
  bool include_weights;

  // Record if the model finishes an inference run so that its owning
  // AOTModelContainer can re-use this instance.
#ifdef USE_CUDA
  std::optional<cudaEvent_t> run_finished_;
#elif defined(USE_XPU)
  std::optional<sycl::event*> run_finished_;
#else // !USE_CUDA
  bool run_finished_{};
#endif

  // Generated model uses this device index to create CUDA guards.
  int32_t device_type_{};
  int32_t device_idx_{};
};

// Codegen-ed classes can derive from this to keep pointers to loaded kernels.
class AOTInductorModelKernelsBase {
 public:
  virtual ~AOTInductorModelKernelsBase() = default;
};

class AOTInductorModel : public AOTInductorModelBase<AOTInductorModel> {
 public:
  AOTInductorModel(
      std::shared_ptr<ConstantMap> constants_map,
      std::shared_ptr<std::vector<ConstantHandle>> constants_array,
      const std::string& device_str,
      std::optional<std::string> cubin_dir,
      bool include_weights = true);

  std::unordered_map<std::string, AtenTensorHandle> const_run_impl(
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor,
      bool initialization = false);

  void _const_run_impl(
      std::vector<AtenTensorHandle>& output_handles,
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  void run_impl(
      AtenTensorHandle*
          input_handles, // array of input AtenTensorHandle; handles
                         // are stolen; the array itself is borrowed
      AtenTensorHandle*
          output_handles, // array for writing output AtenTensorHandle; handles
                          // will be stolen by the caller; the array itself is
                          // borrowed
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  template <typename Inputs, typename Outputs>
  Outputs run_impl_minimal_arrayref_interface(
      const Inputs& inputs,
      DeviceStreamType stream,
      AOTIProxyExecutorHandle proxy_executor);

  static std::unique_ptr<AOTInductorModel> Create(
      std::shared_ptr<ConstantMap> constants_map,
      std::shared_ptr<std::vector<ConstantHandle>> constants_array,
      const std::string& device_str,
      std::optional<std::string> cubin_dir) {
    return std::make_unique<AOTInductorModel>(
        std::move(constants_map),
        std::move(constants_array),
        device_str,
        std::move(cubin_dir));
  }

 private:
  std::unique_ptr<AOTInductorModelKernelsBase> kernels_;
};

} // namespace torch::aot_inductor
