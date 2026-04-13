#pragma once

#include <vulkan/vulkan_raii.hpp>

// Forward-declare to avoid pulling in GLFW headers into every translation unit.
struct GLFWwindow;
class Device;
class Swapchain;

// Wraps the Dear ImGui Vulkan + GLFW backends.
// Owns the descriptor pool ImGui needs and manages the frame lifecycle.
//
// Typical usage per frame:
//   imguiLayer.beginFrame();
//   // ... ImGui::Begin / ImGui::SliderFloat / etc. ...
//   imguiLayer.endFrame();
//   // then in recordCommandBuffer():
//   imguiLayer.renderDrawData(cmd, swapchainImageView, extent);
class ImGuiLayer
{
public:
    // Initialises Dear ImGui with the Vulkan and GLFW backends.
    // Must be called after the device and swapchain are ready.
    void init(Device& device, Swapchain& swapchain, GLFWwindow* window);

    // Shuts down ImGui backends and releases GPU resources.
    // Must be called before the Vulkan device is destroyed.
    void shutdown();

    // Starts a new ImGui frame. Call once at the start of drawFrame(),
    // before building any ImGui widgets.
    void beginFrame();

    // Finalises the ImGui draw list (ImGui::Render).
    // Call after all ImGui::* widget calls, before recordCommandBuffer.
    void endFrame();

    // Records the ImGui draw data into cmd using dynamic rendering.
    // Handles beginRendering / endRendering internally.
    void renderDrawData(vk::raii::CommandBuffer& cmd,
                        vk::ImageView            swapchainImageView,
                        vk::Extent2D             extent);

private:
    vk::raii::DescriptorPool pool_ = nullptr;
};
