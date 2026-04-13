#pragma once

#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <vulkan/vulkan.hpp>

// Sky appearance pushed per-frame. Must match SkyPush in sky.slang.
struct SkyPushConstants
{
    glm::vec4 horizonColor = {0.60f, 0.75f, 0.95f, 1.0f};
    glm::vec4 zenithColor  = {0.10f, 0.30f, 0.75f, 1.0f};
    glm::vec4 groundColor  = {0.20f, 0.15f, 0.10f, 1.0f};
    glm::vec4 sunParams    = {1.5f,  1.3f,  1.0f,  0.9998f};
};

// CPU-side point light, mirrored into the UBO each frame.
struct PointLightData
{
    glm::vec3 position  = {0.0f, 0.0f, 3.0f};
    float     intensity = 3.0f;
    glm::vec3 color     = {1.0f, 1.0f, 1.0f};
    float     radius    = 8.0f;
    bool      enabled   = false;
};

// All user-tweakable render parameters in one place.
// Written by SceneEditor (ImGui), read by VulkanRenderer each frame.
struct RenderSettings
{
    static constexpr int MAX_POINT_LIGHTS = 4;

    // Device / swapchain
    vk::PresentModeKHR      pendingPresentMode = vk::PresentModeKHR::eMailbox;
    vk::SampleCountFlagBits pendingMsaaSamples = vk::SampleCountFlagBits::e1;

    // Lighting
    bool  lightOrbit    = true;
    float lightAngle    = 0.0f;
    float shadowBiasMin = 0.0005f;
    float shadowBiasMax = 0.003f;
    float shadowFar     = 60.0f;

    // Parallax Occlusion Mapping
    float pomDepthScale = 0.05f;
    float pomMinSteps   = 8.0f;
    float pomMaxSteps   = 32.0f;

    // Material / tone
    float ambient          = 0.2f;
    float defaultRoughness = 0.5f;
    float defaultMetallic  = 0.0f;
    float exposure         = 1.0f;
    bool  tonemapping      = true;

    // Sky
    bool             skyEnabled = true;
    SkyPushConstants skyPush;

    // Fog
    bool      fogEnabled       = true;
    float     fogDensity       = 0.02f;
    float     fogHeightFalloff = 0.3f;
    float     fogMaxOpacity    = 1.0f;
    bool      fogSyncSky       = true;
    glm::vec3 fogColor         = {0.60f, 0.75f, 0.95f};

    // Point lights
    std::vector<PointLightData> pointLights = {
        PointLightData{{-4.0f,  3.0f, 4.0f}, 4.0f, {1.0f, 0.85f, 0.5f},  8.0f, true},
        PointLightData{{ 4.0f, -3.0f, 3.0f}, 3.0f, {0.4f, 0.6f,  1.0f}, 10.0f, true},
    };
};
