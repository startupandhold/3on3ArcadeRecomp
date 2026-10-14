// Vulkan presentation for the launcher, after Dear ImGui's
// example_sdl3_vulkan. Built with IMGUI_IMPL_VULKAN_NO_PROTOTYPES: every
// Vulkan entry point is looked up through SDL's loader, so neither building
// nor running needs the Vulkan development files.

#include "nhl3on3_launcher_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdint>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

namespace launcher_window {
namespace {

#define LAUNCHER_VK_FUNCTIONS(X)                                                              \
  X(vkCreateDevice) X(vkDestroyDevice) X(vkDestroyInstance) X(vkGetDeviceQueue)               \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
  X(vkAcquireNextImageKHR) X(vkWaitForFences) X(vkResetFences) X(vkResetCommandPool)           \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)  \
  X(vkQueueSubmit) X(vkQueuePresentKHR) X(vkDeviceWaitIdle)

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
#define LAUNCHER_VK_DECLARE(name) PFN_##name name = nullptr;
LAUNCHER_VK_FUNCTIONS(LAUNCHER_VK_DECLARE)
#undef LAUNCHER_VK_DECLARE

constexpr uint32_t kMinImageCount = 2;

SDL_Window* g_window = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
uint32_t g_queue_family = UINT32_MAX;
VkQueue g_queue = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
ImGui_ImplVulkanH_Window g_frames;
bool g_rebuild = false;
bool g_imgui_vulkan = false;

PFN_vkVoidFunction LoadFunction(const char* name, void*) {
  return vkGetInstanceProcAddr(g_instance, name);
}

bool Fail(std::string& error, const char* what, VkResult result = VK_SUCCESS) {
  error = what;
  if (result != VK_SUCCESS) error += " (VkResult " + std::to_string(int(result)) + ")";
  return false;
}

bool SetupVulkan(std::string& error) {
  vkGetInstanceProcAddr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!vkGetInstanceProcAddr) return Fail(error, "Vulkan is not available");
  vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
      vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!vkCreateInstance) return Fail(error, "Vulkan is not available");

  uint32_t count = 0;
  const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.enabledExtensionCount = count;
  instance_info.ppEnabledExtensionNames = extensions;
  if (VkResult r = vkCreateInstance(&instance_info, nullptr, &g_instance); r != VK_SUCCESS) {
    return Fail(error, "Couldn't create a Vulkan instance", r);
  }
#define LAUNCHER_VK_LOAD(name)                                                              \
  name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(g_instance, #name));            \
  if (!name) return Fail(error, "Vulkan function " #name " is missing");
  LAUNCHER_VK_FUNCTIONS(LAUNCHER_VK_LOAD)
#undef LAUNCHER_VK_LOAD
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, LoadFunction)) {
    return Fail(error, "Couldn't load Vulkan functions for ImGui");
  }

  g_physical_device = ImGui_ImplVulkanH_SelectPhysicalDevice(g_instance);
  if (g_physical_device == VK_NULL_HANDLE) return Fail(error, "No Vulkan GPU found");
  g_queue_family = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_physical_device);
  if (g_queue_family == UINT32_MAX) return Fail(error, "No Vulkan graphics queue found");

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = g_queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = 1;
  device_info.ppEnabledExtensionNames = device_extensions;
  if (VkResult r = vkCreateDevice(g_physical_device, &device_info, nullptr, &g_device);
      r != VK_SUCCESS) {
    return Fail(error, "Couldn't create a Vulkan device", r);
  }
  vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);

  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = pool_size.descriptorCount;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (VkResult r = vkCreateDescriptorPool(g_device, &pool_info, nullptr, &g_descriptor_pool);
      r != VK_SUCCESS) {
    return Fail(error, "Couldn't create a Vulkan descriptor pool", r);
  }
  return true;
}

bool SetupSwapchain(std::string& error) {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (!SDL_Vulkan_CreateSurface(g_window, g_instance, nullptr, &surface)) {
    return Fail(error, "Couldn't create a Vulkan surface");
  }
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(g_physical_device, g_queue_family, surface, &supported);
  if (!supported) return Fail(error, "The Vulkan GPU can't present to this window");
  g_frames.Surface = surface;
  const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
  g_frames.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
      g_physical_device, surface, formats, IM_ARRAYSIZE(formats), VK_COLORSPACE_SRGB_NONLINEAR_KHR);
  const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR};
  g_frames.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_physical_device, surface, modes, 1);
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g_window, &w, &h);
  ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_physical_device, g_device, &g_frames,
                                         g_queue_family, nullptr, w, h, kMinImageCount, 0);
  return true;
}

void Render(ImDrawData* draw_data) {
  ImGui_ImplVulkanH_Window* wd = &g_frames;
  VkSemaphore acquired = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore complete = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkResult r = vkAcquireNextImageKHR(g_device, wd->Swapchain, UINT64_MAX, acquired,
                                     VK_NULL_HANDLE, &wd->FrameIndex);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_rebuild = true;
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;

  ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
  vkWaitForFences(g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
  vkResetFences(g_device, 1, &fd->Fence);
  vkResetCommandPool(g_device, fd->CommandPool, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(fd->CommandBuffer, &begin);
  VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = wd->RenderPass;
  pass.framebuffer = fd->Framebuffer;
  pass.renderArea.extent = {uint32_t(wd->Width), uint32_t(wd->Height)};
  pass.clearValueCount = 1;
  pass.pClearValues = &wd->ClearValue;
  vkCmdBeginRenderPass(fd->CommandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
  vkCmdEndRenderPass(fd->CommandBuffer);
  vkEndCommandBuffer(fd->CommandBuffer);
  const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &acquired;
  submit.pWaitDstStageMask = &stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &fd->CommandBuffer;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &complete;
  vkQueueSubmit(g_queue, 1, &submit, fd->Fence);

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &complete;
  present.swapchainCount = 1;
  present.pSwapchains = &wd->Swapchain;
  present.pImageIndices = &wd->FrameIndex;
  r = vkQueuePresentKHR(g_queue, &present);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_rebuild = true;
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

}  // namespace

bool Open(const char* title, int width, int height, std::string& error) {
  g_window = SDL_CreateWindow(title, width, height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!g_window) {
    error = std::string("Couldn't create the window: ") + SDL_GetError();
    return false;
  }
  if (!SetupVulkan(error) || !SetupSwapchain(error)) return false;
  g_frames.ClearValue.color = {{0.12f, 0.12f, 0.13f, 1.0f}};
  SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(g_window);

  ImGui_ImplSDL3_InitForVulkan(g_window);
  ImGui_ImplVulkan_InitInfo init{};
  init.ApiVersion = VK_API_VERSION_1_0;
  init.Instance = g_instance;
  init.PhysicalDevice = g_physical_device;
  init.Device = g_device;
  init.QueueFamily = g_queue_family;
  init.Queue = g_queue;
  init.DescriptorPool = g_descriptor_pool;
  init.MinImageCount = kMinImageCount;
  init.ImageCount = g_frames.ImageCount;
  init.PipelineInfoMain.RenderPass = g_frames.RenderPass;
  init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&init)) return Fail(error, "Couldn't initialize ImGui for Vulkan");
  g_imgui_vulkan = true;
  return true;
}

SDL_Window* Get() { return g_window; }

void BeginFrame() {
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g_window, &w, &h);
  if (w > 0 && h > 0 && (g_rebuild || g_frames.Width != w || g_frames.Height != h)) {
    ImGui_ImplVulkan_SetMinImageCount(kMinImageCount);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_physical_device, g_device, &g_frames,
                                           g_queue_family, nullptr, w, h, kMinImageCount, 0);
    g_frames.FrameIndex = 0;
    g_rebuild = false;
  }
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

void EndFrame() {
  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();
  if (draw_data->DisplaySize.x > 0 && draw_data->DisplaySize.y > 0) Render(draw_data);
}

void Close() {
  if (g_device) vkDeviceWaitIdle(g_device);
  if (g_imgui_vulkan) ImGui_ImplVulkan_Shutdown();
  if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData) {
    ImGui_ImplSDL3_Shutdown();
  }
  if (g_device) {
    if (g_frames.Swapchain || g_frames.Surface) {
      ImGui_ImplVulkanH_DestroyWindow(g_instance, g_device, &g_frames, nullptr);
    }
    if (g_descriptor_pool) vkDestroyDescriptorPool(g_device, g_descriptor_pool, nullptr);
    vkDestroyDevice(g_device, nullptr);
  }
  if (g_instance) vkDestroyInstance(g_instance, nullptr);
  if (g_window) SDL_DestroyWindow(g_window);
  g_window = nullptr;
}

}  // namespace launcher_window
