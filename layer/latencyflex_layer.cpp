// Copyright 2021 Tatsuyuki Ishi
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "latencyflex_layer.h"
#include "version.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>

#include <dlfcn.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vk_layer_dispatch_table.h>
#include <vulkan/vulkan.h>

#include "latencyflex.h"

#define LAYER_NAME "VK_LAYER_LFX_LatencyFleX"

namespace {
std::atomic_uint64_t frame_counter = 0;
std::atomic_bool ticker_needs_reset = false;
std::atomic_uint64_t frame_counter_render = 0;

lfx::LatencyFleX manager;

class IdleTracker {
public:
  // Returns: true if the sleep was fully performed or false if it was determined unnecessary
  //          because there are no inflight frames.
  bool SleepAndBegin(uint64_t frame, const std::chrono::nanoseconds &dur) {
    std::unique_lock l(m);
    bool skipped = cv.wait_for(l, dur, [this] { return last_began_frame == last_finished_frame; });
    last_began_frame = frame;
    return !skipped;
  }

  void End(uint64_t frame) {
    std::unique_lock l(m);
    last_finished_frame = frame;
    if (last_began_frame == last_finished_frame)
      cv.notify_all();
  }

private:
  std::mutex m;
  std::condition_variable cv;
  std::uint64_t last_began_frame = UINT64_MAX;
  std::uint64_t last_finished_frame = UINT64_MAX;
};

IdleTracker idle_tracker;

// Placebo mode. This turns off all sleeping but still retains latency and frame time tracking.
// Useful for comparison benchmarks. Note that if the game does its own sleeping between the
// syncpoint and input sampling, latency values from placebo mode might not be accurate.
bool is_placebo_mode = false;

typedef void(VKAPI_PTR *PFN_overlay_SetMetrics)(const char **, const float *, size_t);
PFN_overlay_SetMetrics overlay_SetMetrics = nullptr;

const int kMaxFrameDrift = 16;
const std::chrono::milliseconds kRecalibrationSleepTime(200);

typedef std::lock_guard<std::mutex> scoped_lock;
// single global lock, for simplicity
std::mutex global_lock;

struct PresentInfo {
  VkDevice device;
  VkFence fence;
  uint64_t frame_id;
};

// use the loader's dispatch table pointer as a key for dispatch map lookups
template <typename DispatchableType> void *GetKey(DispatchableType inst) { return *(void **)inst; }

// layer book-keeping information, to store dispatch tables by key
std::map<void *, VkLayerInstanceDispatchTable> instance_dispatch;
std::map<void *, VkLayerDispatchTable> device_dispatch;
std::map<void *, VkDevice> device_map;

class FenceWaitThread {
public:
  FenceWaitThread();

  ~FenceWaitThread();

  void Push(PresentInfo &&info) {
    scoped_lock l(local_lock_);
    queue_.push_back(info);
    notify_.notify_all();
  }

private:
  void Worker();

  std::thread thread_;
  std::mutex local_lock_;
  std::condition_variable notify_;
  std::deque<PresentInfo> queue_;
  bool running_ = true;
};

FenceWaitThread::FenceWaitThread() : thread_(&FenceWaitThread::Worker, this) {}

FenceWaitThread::~FenceWaitThread() {
  running_ = false;
  notify_.notify_all();
  thread_.join();
}

void FenceWaitThread::Worker() {
  while (true) {
    PresentInfo info;
    {
      std::unique_lock<std::mutex> l(local_lock_);
      while (queue_.empty()) {
        if (!running_)
          return;
        notify_.wait(l);
      }
      info = queue_.front();
      queue_.pop_front();
    }
    VkDevice device = info.device;
    VkLayerDispatchTable &dispatch = device_dispatch[GetKey(info.device)];
    dispatch.WaitForFences(device, 1, &info.fence, VK_TRUE, -1);
    uint64_t complete = current_time_ns();
    dispatch.DestroyFence(device, info.fence, nullptr);

    uint64_t latency;
    {
      scoped_lock l(global_lock);
      manager.EndFrame(info.frame_id, complete, &latency, nullptr);
    }
    idle_tracker.End(info.frame_id);
    float latency_f = latency / 1000000.;
    const char *name = "Latency";
    if (overlay_SetMetrics && latency != UINT64_MAX) {
      overlay_SetMetrics(&name, &latency_f, 1);
    }
  }
}

std::map<void *, std::unique_ptr<FenceWaitThread>> wait_threads;
} // namespace

///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown

VkResult VKAPI_CALL lfx_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                       const VkAllocationCallbacks *pAllocator,
                                       VkInstance *pInstance) {
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while (layerCreateInfo &&
         (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
          layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }

  if (layerCreateInfo == nullptr) {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

  VkResult ret = createFunc(pCreateInfo, pAllocator, pInstance);
  if (ret != VK_SUCCESS)
    return ret;

  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerInstanceDispatchTable dispatchTable;
  dispatchTable.GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)gpa(*pInstance, "vkGetInstanceProcAddr");
  dispatchTable.DestroyInstance = (PFN_vkDestroyInstance)gpa(*pInstance, "vkDestroyInstance");
  dispatchTable.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gpa(
      *pInstance, "vkEnumerateDeviceExtensionProperties");

  // store the table by key
  {
    scoped_lock l(global_lock);
    instance_dispatch[GetKey(*pInstance)] = dispatchTable;

    if (void *mod = dlopen("libMangoHud.so", RTLD_NOW | RTLD_NOLOAD)) {
      overlay_SetMetrics = (PFN_overlay_SetMetrics)dlsym(mod, "overlay_SetMetrics");
    }
  }

  return VK_SUCCESS;
}

void VKAPI_CALL lfx_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
  scoped_lock l(global_lock);
  instance_dispatch[GetKey(instance)].DestroyInstance(instance, pAllocator);
  instance_dispatch.erase(GetKey(instance));
}

VkResult VKAPI_CALL lfx_CreateDevice(VkPhysicalDevice physicalDevice,
                                     const VkDeviceCreateInfo *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while (layerCreateInfo &&
         (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
          layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if (layerCreateInfo == nullptr) {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
  if (ret != VK_SUCCESS)
    return ret;

#define ASSIGN_FUNCTION(name) dispatchTable.name = (PFN_vk##name)gdpa(*pDevice, "vk" #name);
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerDispatchTable dispatchTable;
  ASSIGN_FUNCTION(GetDeviceProcAddr);
  ASSIGN_FUNCTION(DestroyDevice);
  ASSIGN_FUNCTION(QueuePresentKHR);
  ASSIGN_FUNCTION(AcquireNextImageKHR);
  ASSIGN_FUNCTION(AcquireNextImage2KHR);
  ASSIGN_FUNCTION(CreateFence);
  ASSIGN_FUNCTION(DestroyFence);
  ASSIGN_FUNCTION(QueueSubmit);
  ASSIGN_FUNCTION(WaitForFences);
#undef ASSIGN_FUNCTION

  // store the table by key
  {
    scoped_lock l(global_lock);
    device_dispatch[GetKey(*pDevice)] = dispatchTable;
    device_map[GetKey(*pDevice)] = *pDevice;
    wait_threads[GetKey(*pDevice)] = std::make_unique<FenceWaitThread>();
  }

  return VK_SUCCESS;
}

void VKAPI_CALL lfx_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
  scoped_lock l(global_lock);
  wait_threads.erase(GetKey(device));
  device_dispatch[GetKey(device)].DestroyDevice(device, pAllocator);
  device_dispatch.erase(GetKey(device));
  device_map.erase(GetKey(device));
}

///////////////////////////////////////////////////////////////////////////////////////////
// Enumeration function

VkResult VKAPI_CALL lfx_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                         VkLayerProperties *pProperties) {
  if (pPropertyCount)
    *pPropertyCount = 1;

  if (pProperties) {
    strcpy(pProperties->layerName, LAYER_NAME);
    strcpy(pProperties->description, "LatencyFleX (TM) latency reduction middleware");
    pProperties->implementationVersion = 1;
    pProperties->specVersion = VK_MAKE_VERSION(1, 2, 136);
  }

  return VK_SUCCESS;
}

VkResult VKAPI_CALL lfx_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                       uint32_t *pPropertyCount,
                                                       VkLayerProperties *pProperties) {
  return lfx_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VkResult VKAPI_CALL lfx_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                                             uint32_t *pPropertyCount,
                                                             VkExtensionProperties *pProperties) {
  if (pLayerName == nullptr || strcmp(pLayerName, LAYER_NAME))
    return VK_ERROR_LAYER_NOT_PRESENT;

  // don't expose any extensions
  if (pPropertyCount)
    *pPropertyCount = 0;
  return VK_SUCCESS;
}

VkResult VKAPI_CALL lfx_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                           const char *pLayerName,
                                                           uint32_t *pPropertyCount,
                                                           VkExtensionProperties *pProperties) {
  // pass through any queries that aren't to us
  if (pLayerName == nullptr || strcmp(pLayerName, LAYER_NAME)) {
    if (physicalDevice == VK_NULL_HANDLE)
      return VK_SUCCESS;

    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
        physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  // don't expose any extensions
  if (pPropertyCount)
    *pPropertyCount = 0;
  return VK_SUCCESS;
}

VkResult VKAPI_CALL lfx_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
  frame_counter_render++;
  uint64_t frame_counter_local = frame_counter.load();
  uint64_t frame_counter_render_local = frame_counter_render.load();
  if (frame_counter_local > frame_counter_render_local + kMaxFrameDrift) {
    ticker_needs_reset.store(true);
  }

  std::unique_lock<std::mutex> l(global_lock);
  VkDevice device = device_map[GetKey(queue)];
  VkLayerDispatchTable &dispatch = device_dispatch[GetKey(queue)];
  VkFence fence;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  dispatch.CreateFence(device, &fenceInfo, nullptr,
                       &fence); // TODO: error check
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkPipelineStageFlags stages_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
  submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
  submitInfo.pWaitDstStageMask = &stages_wait;
  submitInfo.signalSemaphoreCount = pPresentInfo->waitSemaphoreCount;
  submitInfo.pSignalSemaphores = pPresentInfo->pWaitSemaphores;
  dispatch.QueueSubmit(queue, 1, &submitInfo, fence);
  wait_threads[GetKey(device)]->Push({device, fence, frame_counter_render_local});
  l.unlock();
  return dispatch.QueuePresentKHR(queue, pPresentInfo);
}

VkResult VKAPI_CALL lfx_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                            uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                                            uint32_t *pImageIndex) {
  std::unique_lock<std::mutex> l(global_lock);
  VkLayerDispatchTable &dispatch = device_dispatch[GetKey(device)];
  l.unlock();
  VkResult res =
      dispatch.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
  if (res < 0) {
    // An error has occurred likely due to an Alt-Tab or resize.
    // The application will likely give up presenting this frame, which means that we won't get a
    // call to QueuePresentKHR! This can cause the frame counter to desync. Schedule a recalibration
    // immediately.
    ticker_needs_reset.store(true);
  }
  return res;
}

VkResult VKAPI_CALL lfx_AcquireNextImage2KHR(VkDevice device,
                                             const VkAcquireNextImageInfoKHR *pAcquireInfo,
                                             uint32_t *pImageIndex) {
  std::unique_lock<std::mutex> l(global_lock);
  VkLayerDispatchTable &dispatch = device_dispatch[GetKey(device)];
  l.unlock();
  VkResult res = dispatch.AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
  if (res < 0) {
    // An error has occurred likely due to an Alt-Tab or resize.
    // The application will likely give up presenting this frame, which means that we won't get a
    // call to QueuePresentKHR! This can cause the frame counter to desync. Schedule a recalibration
    // immediately.
    ticker_needs_reset.store(true);
  }
  return res;
}

///////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions, entry points of the layer

#define GETPROCADDR(func)                                                                          \
  if (!strcmp(pName, "vk" #func))                                                                  \
  return (PFN_vkVoidFunction)&lfx_##func

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL lfx_GetDeviceProcAddr(VkDevice device,
                                                                               const char *pName) {
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(AcquireNextImageKHR);
  GETPROCADDR(AcquireNextImage2KHR);

  {
    scoped_lock l(global_lock);
    return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
  }
}

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
lfx_GetInstanceProcAddr(VkInstance instance, const char *pName) {
  // instance chain functions we intercept
  GETPROCADDR(GetInstanceProcAddr);
  GETPROCADDR(EnumerateInstanceLayerProperties);
  GETPROCADDR(EnumerateInstanceExtensionProperties);
  GETPROCADDR(CreateInstance);
  GETPROCADDR(DestroyInstance);

  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(AcquireNextImageKHR);
  GETPROCADDR(AcquireNextImage2KHR);

  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}

extern "C" VK_LAYER_EXPORT void lfx_WaitAndBeginFrame() {
  frame_counter++;
  uint64_t frame_counter_local = frame_counter.load();
  uint64_t frame_counter_render_local = frame_counter_render.load();

  if (frame_counter_local <= frame_counter_render_local) {
    // Presentation has happened without going through the Tick() hook!
    // This typically happens during initialization (where graphics are redrawn
    // without ticking the platform loop).
    ticker_needs_reset.store(true);
  }

  if (ticker_needs_reset.load()) {
    std::cerr << "LatencyFleX: Performing recalibration!" << std::endl;
    // Try to reset (recalibrate) the state by sleeping for a slightly long
    // period and force any work in the rendering thread or the RHI thread to be
    // flushed. The frame counter is reset after the calibration.
    std::this_thread::sleep_for(kRecalibrationSleepTime);
    // The ticker thread has already incremented the frame counter above. Start
    // from 1, or otherwise it will result in frame ID mismatch.
    frame_counter.store(1);
    frame_counter_local = 1;
    frame_counter_render.store(0);
    frame_counter_render_local = 0;
    ticker_needs_reset.store(false);
    scoped_lock l(global_lock);
    manager.Reset();
  }
  uint64_t now = current_time_ns();
  uint64_t target;
  uint64_t wakeup;
  {
    scoped_lock l(global_lock);
    target = manager.GetWaitTarget(frame_counter_local);
  }
  if (!is_placebo_mode && target > now) {
    // failsafe: if something ever goes wrong, sustain an interactive framerate
    // so the user can at least quit the application
    static uint64_t failsafe_triggered = 0;
    uint64_t failsafe = now + UINT64_C(50000000);
    if (target > failsafe) {
      wakeup = failsafe;
      failsafe_triggered++;
      if (failsafe_triggered > 5) {
        // If failsafe is triggered multiple times in a row, initiate a recalibration.
        ticker_needs_reset.store(true);
      }
    } else {
      wakeup = target;
      failsafe_triggered = 0;
    }
    if (!idle_tracker.SleepAndBegin(frame_counter_local, std::chrono::nanoseconds(wakeup - now)))
      wakeup = current_time_ns();
  } else {
    idle_tracker.SleepAndBegin(frame_counter_local, std::chrono::nanoseconds::zero());
    wakeup = now;
  }
  {
    scoped_lock l(global_lock);
    // Use the sleep target as the frame begin time. See `BeginFrame` docs.
    manager.BeginFrame(frame_counter_local, target, wakeup);
  }
}

extern "C" VK_LAYER_EXPORT void lfx_SetTargetFrameTime(uint64_t target_frame_time) {
  scoped_lock l(global_lock);
  manager.target_frame_time = target_frame_time;
  std::cerr << "LatencyFleX: setting target frame time to " << manager.target_frame_time
            << std::endl;
}

namespace {
class OnLoad {
public:
  OnLoad() {
    std::cerr << "LatencyFleX: module loaded" << std::endl;
    std::cerr << "LatencyFleX: Version " LATENCYFLEX_VERSION << std::endl;
    if (getenv("LFX_MAX_FPS")) {
      // No lock needed because this is done inside static initialization.
      manager.target_frame_time = 1000000000 / std::stoul(getenv("LFX_MAX_FPS"));
      std::cerr << "LatencyFleX: setting target frame time to " << manager.target_frame_time
                << std::endl;
    }
    if (getenv("LFX_PLACEBO")) {
      is_placebo_mode = true;
      std::cerr << "LatencyFleX: Running in placebo mode" << std::endl;
    }
  }
};

[[maybe_unused]] OnLoad on_load;
} // namespace