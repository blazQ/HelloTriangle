#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "Camera.hpp"
#include "Renderable.hpp"
#include "RenderSettings.hpp"

// Owns the ImGui widget tree for the scene editor panel.
// Receives a RenderSettings reference each frame and modifies it directly —
// VulkanRenderer reads settings back after the call.
//
// Keeping widget definitions here (rather than in VulkanRenderer) means the
// renderer has no knowledge of ImGui, and the editor has no knowledge of
// Vulkan pipelines or GPU resources.
class SceneEditor
{
public:
    // Read-only information gathered by VulkanRenderer before calling draw().
    struct DisplayInfo
    {
        vk::Extent2D         resolution;
        const char*          deviceName;   // lifetime: valid for duration of the Device
        uint32_t             textureCount;
        vk::SampleCountFlags supportedMsaa;
    };

    // Build the full ImGui window for one frame.
    // Must be called between ImGuiLayer::beginFrame() and ImGuiLayer::endFrame().
    void draw(RenderSettings&          settings,
              std::vector<Renderable>& renderables,
              Camera&                  camera,
              const DisplayInfo&       info);
};
