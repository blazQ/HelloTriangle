#include "ImGuiLayer.hpp"
#include "Device.hpp"
#include "Swapchain.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

void ImGuiLayer::init(Device& device, Swapchain& swapchain, GLFWwindow* window)
{
    // imgui 1.92+ requires at least IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE
    // descriptors for the font atlas; allocating fewer causes silent failures.
    vk::DescriptorPoolSize poolSize(
        vk::DescriptorType::eCombinedImageSampler,
        IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE);
    vk::DescriptorPoolCreateInfo poolInfo{
        .flags        = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets      = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE,
        .poolSizeCount= 1,
        .pPoolSizes   = &poolSize};
    pool_ = vk::raii::DescriptorPool(device.getLogicalDevice(), poolInfo);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Scale UI for high-DPI displays.
    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    ImGui::GetIO().Fonts->AddFontDefault();
    ImGui::GetStyle().ScaleAllSizes(xscale);

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance       = *device.getInstance();
    initInfo.PhysicalDevice = *device.getPhysicalDevice();
    initInfo.Device         = *device.getLogicalDevice();
    initInfo.Queue          = *device.getGraphicsQueue();
    initInfo.DescriptorPool = *pool_;
    initInfo.MinImageCount  = 2;
    initInfo.ImageCount     = static_cast<uint32_t>(swapchain.getImages().size());

    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = reinterpret_cast<const VkFormat*>(
            &swapchain.getSurfaceFormat().format)};
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);
}

void ImGuiLayer::shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    pool_ = nullptr;
}

void ImGuiLayer::beginFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame()
{
    ImGui::Render();
}

void ImGuiLayer::renderDrawData(vk::raii::CommandBuffer& cmd,
                                 vk::ImageView            swapchainImageView,
                                 vk::Extent2D             extent)
{
    vk::RenderingAttachmentInfo colorAttachment{
        .imageView  = swapchainImageView,
        .imageLayout= vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp     = vk::AttachmentLoadOp::eLoad,   // preserve the scene underneath
        .storeOp    = vk::AttachmentStoreOp::eStore};

    vk::RenderingInfo renderingInfo{
        .renderArea           = {vk::Offset2D{0, 0}, extent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorAttachment};

    cmd.beginRendering(renderingInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), *cmd);
    cmd.endRendering();
}
