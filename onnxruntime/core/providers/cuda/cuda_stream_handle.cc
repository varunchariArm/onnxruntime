#include "core/providers/cuda/cuda_stream_handle.h"
#include "core/providers/cuda/cuda_common.h"
#include "core/common/spin_pause.h"

namespace onnxruntime {

struct CudaNotification {
  void notify() { 
    // record event with cudaEventBlockingSync so we can support sync on host with out busy wait.
    CUDA_CALL_THROW(cudaEventRecordWithFlags(event_, stream_, cudaEventBlockingSync));
    //activate the notification.
    ready_.store(true); 
  };

  void wait_on_device() {
    // wait for the notification to be activated
    while (!ready_.load()) {
      onnxruntime::concurrency::SpinPause();
    }
    // launch a wait command to the cuda stream
    CUDA_CALL_THROW(cudaStreamWaitEvent(stream_, event_));
  };

  void wait_on_host() {
    // wait for the notification to be activated
    while (!ready_.load()) {
      onnxruntime::concurrency::SpinPause();
    }
    // launch a wait command to the cuda stream
    CUDA_CALL_THROW(cudaEventSynchronize(event_));
  }

  CudaNotification::CudaNotification(cudaStream_t stream) : stream_(stream) {
    CUDA_CALL_THROW(cudaEventCreate(&event_));
  }

  ~CudaNotification() {
    CUDA_CALL_THROW(cudaEventDestroy(event_)); 
  }

  std::atomic_bool ready_{};
  cudaEvent_t event_;
  cudaStream_t stream_;
};

// CPU Stream command handles
void WaitCudaNotificationOnDevice(NotificationHandle& notification) {
  static_cast<CudaNotification*>(notification)->wait_on_device();
}

void WaitCudaNotificationOnHost(NotificationHandle& notification) {
  static_cast<CudaNotification*>(notification)->wait_on_host();
}

void NotifyCudaNotification(NotificationHandle& notification) {
  static_cast<CudaNotification*>(notification)->notify();
}

void* CreateCudaNotification(const StreamHandle& stream) {
  return new CudaNotification(static_cast<cudaStream_t>(stream));
}

void ReleaseCUdaNotification(void* handle) {
  delete static_cast<CudaNotification*>(handle);
}

StreamHandle CreateCudaStream() {
  cudaStream_t stream = nullptr;
  CUDA_CALL_THROW(cudaStreamCreate(&stream));
  return stream;
}

void ReleaseCPUStram(StreamHandle handle) {
  CUDA_CALL(cudaStreamDestroy(static_cast<cudaStream_t>(handle)));
}

void RegisterCudaStreamHandles(IStreamCommandHandleRegistry& stream_handle_registry) {
  // wait cuda notification on cuda ep
  stream_handle_registry.RegisterWaitFn(kCudaExecutionProvider, kCudaExecutionProvider, WaitCudaNotificationOnDevice);
  // wait cuda notification on cpu ep
  stream_handle_registry.RegisterWaitFn(kCudaExecutionProvider, kCudaExecutionProvider, WaitCudaNotificationOnHost);

  stream_handle_registry.RegisterNotifyFn(kCudaExecutionProvider, NotifyCudaNotification);
  stream_handle_registry.RegisterCreateNotificationFn(kCudaExecutionProvider, CreateCudaNotification);
  stream_handle_registry.RegisterReleaseNotificationFn(kCudaExecutionProvider, ReleaseCUdaNotification);
  stream_handle_registry.RegisterCreateStreamFn(kCudaExecutionProvider, CreateCudaStream);
  stream_handle_registry.RegisterReleaseStreamFn(kCudaExecutionProvider, ReleaseCPUStram);
}

}