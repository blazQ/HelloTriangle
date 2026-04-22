#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#if defined(__INTELLISENSE__) || !defined(USE_CPP20_MODULES)
#include <vulkan/vulkan_raii.hpp>
#else
import vulkan_hpp;
#endif

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Camera.hpp"
#include "Device.hpp"
#include "ImGuiLayer.hpp"
#include "Renderable.hpp"
#include "RenderSettings.hpp"
#include "Scene.hpp"
#include "SceneEditor.hpp"
#include "Swapchain.hpp"
#include "TextureManager.hpp"

class VulkanRenderer
{
public:
    void run();

private:
    // -------------------------------------------------------------------------
    // Constants
    // -------------------------------------------------------------------------

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    // -------------------------------------------------------------------------
    // Nested types
    // -------------------------------------------------------------------------

    // Push constants sent per draw call: model matrix + texture slot indices.
    struct PushConstants
    {
        glm::mat4 model;
        uint32_t  textureIndex;
        uint32_t  metallicRoughnessIndex; // 0xFFFF = use default roughness/metallic from UBO
        uint32_t  normalMapIndex;         // 0xFFFF = use geometric normal
        uint32_t  heightMapIndex;         // 0xFFFF = no POM
    };

    // Data layout of the uniform buffer as the shader sees it.
    // All fields are vec4-aligned — do not insert scalars between them.
    // Must stay in sync with SkyUBO in sky.slang.
    struct UniformBufferObject
    {
        glm::mat4 view;
        glm::mat4 proj;
        glm::mat4 lightSpaceMatrix;
        glm::vec4 lightDir;
        glm::vec4 cameraPos;
        glm::vec4 materialParams;        // x=ambient, y=defaultRoughness, z=defaultMetallic, w=exposure
        glm::vec4 pointLightPos[4];      // xyz=world position, w=intensity
        glm::vec4 pointLightColor[4];    // xyz=color, w=radius (falloff distance)
        glm::vec4 lightCounts;           // x=number of active point lights
        glm::vec4 shadowParams;          // x=biasMin, y=biasMax
        glm::vec4 pomParams;             // x=depthScale, y=minSteps, z=maxSteps
        glm::vec4 fogParams;             // x=density, y=heightFalloff, z=maxOpacity
        glm::vec4 fogColor;
        glm::mat4 invProj;               // inverse of proj (sky ray reconstruction)
        glm::mat4 invViewRot;            // inverse of view rotation (sky ray reconstruction)
    };

    // -------------------------------------------------------------------------
    // Member variables
    // -------------------------------------------------------------------------

    // Base path: directory containing the executable. Used to resolve all
    // asset paths so the binary can be run from any working directory.
    std::filesystem::path basePath_;

    // Window
    GLFWwindow* window           = nullptr;
    bool        framebufferResized = false;

    // Core Vulkan objects
    std::unique_ptr<Device>    vulkanDevice;
    std::unique_ptr<Swapchain> swapchain;

    // Scene pipeline
    vk::raii::DescriptorSetLayout descriptorSetLayout = nullptr;
    vk::raii::PipelineLayout      pipelineLayout      = nullptr;
    vk::raii::Pipeline            graphicsPipeline    = nullptr;
    vk::raii::Pipeline            shadowPipeline      = nullptr;

    // Sky pipeline
    vk::raii::DescriptorSetLayout        skyDescriptorSetLayout = nullptr;
    vk::raii::PipelineLayout             skyPipelineLayout      = nullptr;
    vk::raii::Pipeline                   skyPipeline            = nullptr;
    vk::raii::DescriptorPool             skyDescriptorPool      = nullptr;
    std::vector<vk::raii::DescriptorSet> skyDescriptorSets;

    // Command recording
    vk::raii::CommandPool                 commandPool = nullptr;
    std::vector<vk::raii::CommandBuffer>  commandBuffers;
    uint32_t frameIndex = 0;

    // Render attachments (MSAA + depth)
    vk::SampleCountFlagBits msaaSamples = vk::SampleCountFlagBits::e1; // currently applied
    vk::raii::Image        colorImage       = nullptr;
    vk::raii::DeviceMemory colorImageMemory = nullptr;
    vk::raii::ImageView    colorImageView   = nullptr;
    vk::raii::Image        depthImage       = nullptr;
    vk::raii::DeviceMemory depthImageMemory = nullptr;
    vk::raii::ImageView    depthImageView   = nullptr;
    vk::raii::Image        normalImage          = nullptr;
    vk::raii::DeviceMemory normalImageMemory    = nullptr;
    vk::raii::ImageView    normalImageView      = nullptr;
    vk::raii::Image        normalResolveImage       = nullptr;
    vk::raii::DeviceMemory normalResolveImageMemory = nullptr;
    vk::raii::ImageView    normalResolveImageView   = nullptr;

    // Shadow map
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;
    vk::raii::Image        shadowMapImage       = nullptr;
    vk::raii::DeviceMemory shadowMapImageMemory = nullptr;
    vk::raii::ImageView    shadowMapImageView   = nullptr;
    vk::raii::Sampler      shadowMapSampler     = nullptr;

    // All loaded textures — owned by TextureManager.
    TextureManager textureManager_;

    // Scene
    std::vector<Renderable> renderables;

    // Uniform buffers (one per frame in flight)
    std::vector<vk::raii::Buffer>        uniformBuffers;
    std::vector<vk::raii::DeviceMemory>  uniformBuffersMemory;
    std::vector<void*>                   uniformBuffersMapped;

    // Camera
    Camera    camera;
    bool      cameraMode   = false;
    glm::vec2 lastMousePos = {0.0f, 0.0f};

    // All user-tweakable render parameters
    RenderSettings settings_;

    // Light animation time (not a setting — purely runtime state)
    float prevTime = 0.0f;

    // Descriptors
    vk::raii::DescriptorPool             descriptorPool = nullptr;
    std::vector<vk::raii::DescriptorSet> descriptorSets;

    // Synchronization
    std::vector<vk::raii::Semaphore> presentCompleteSemaphores;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
    std::vector<vk::raii::Fence>     inFlightFences;

    // Dear ImGui — Vulkan integration and frame lifecycle
    ImGuiLayer  imguiLayer_;
    SceneEditor sceneEditor_;

    // -------------------------------------------------------------------------
    // Private methods
    // -------------------------------------------------------------------------

    // Vertex format helpers
    static vk::VertexInputBindingDescription getVertexBindingDescription();
    static std::array<vk::VertexInputAttributeDescription, 5> getVertexAttributeDescriptions();

    // Window
    void initWindow();
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    // Vulkan setup
    void initVulkan();
    void createDescriptorSetLayout();
    void createGraphicsPipeline();
    void createShadowPipeline();
    void createSkyDescriptorSetLayout();
    void createSkyPipeline();
    void createSkyDescriptorSets();
    std::vector<char> readFile(const std::filesystem::path& path);
    vk::raii::ShaderModule createShaderModule(const std::vector<char>& code);

    // Command infrastructure
    void createCommandPool();
    void createCommandBuffers();

    // Render attachments
    void createColorResources();
    void createDepthResources();
    void createNormalResources();
    void createShadowMapResources();
    void createShadowMapSampler();

    // Scene
    void uploadRenderable(Renderable& r, const std::vector<Vertex>& verts,
                          const std::vector<uint32_t>& idxs);
    void loadScene();
    void createUniformBuffers();

    // Descriptors
    void createDescriptorPool();
    void createDescriptorSets();

    // Synchronization
    void createSyncObjects();

    // Per-frame rendering
    void mainLoop();
    void drawFrame();
    void updateUniformBuffer(uint32_t currentImage);
    void recordCommandBuffer(uint32_t imageIndex);
    void recordImageBarrier(vk::Image image,
                            vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                            vk::AccessFlags2 srcAccessMask, vk::AccessFlags2 dstAccessMask,
                            vk::PipelineStageFlags2 srcStageMask,
                            vk::PipelineStageFlags2 dstStageMask,
                            vk::ImageAspectFlags imageAspectFlags);

    // Lifecycle
    void recreateSwapChain();
    void rebuildMsaa();
    void cleanupSwapChain();
    void cleanup();

    // ImGui — initialisation only (widget tree lives in SceneEditor)
    void imGuiInit();
};
