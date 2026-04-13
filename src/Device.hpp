#pragma once

#include <memory>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

class Device
{
public:
    Device(GLFWwindow *window, std::vector<const char *> requiredDeviceExtensions, std::vector<const char *> validationLayers);

    vk::raii::Instance &getInstance();
    vk::raii::SurfaceKHR &getSurface();
    vk::raii::PhysicalDevice &getPhysicalDevice();
    vk::raii::Device &getLogicalDevice();
    vk::raii::Queue &getGraphicsQueue();
    uint32_t getGraphicsIndex() const;

    vk::SampleCountFlagBits getMsaaSamples() const;
    vk::SampleCountFlagBits getMaxMsaaSamples() const;
    vk::SampleCountFlags getSupportedMsaaSamples() const;
    uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties);
    vk::Format findSupportedFormat(const std::vector<vk::Format> &candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);
    vk::Format findDepthFormat();
    vk::raii::ImageView createImageView(vk::Image image, vk::Format format,
                                                   vk::ImageAspectFlags aspectFlags,
                                                   uint32_t mipLevels);

    // -----------------------------------------------------------------------
    // GPU upload utilities
    // These operations are synchronous: they allocate a one-shot command
    // buffer, submit it, and wait for the GPU to finish before returning.
    // -----------------------------------------------------------------------
    void createImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                     vk::SampleCountFlagBits numSamples, vk::Format format,
                     vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                     vk::MemoryPropertyFlags properties,
                     vk::raii::Image& image, vk::raii::DeviceMemory& imageMemory);

    void createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                      vk::MemoryPropertyFlags properties,
                      vk::raii::Buffer& buffer, vk::raii::DeviceMemory& bufferMemory);

    std::unique_ptr<vk::raii::CommandBuffer> beginSingleTimeCommands();
    void endSingleTimeCommands(vk::raii::CommandBuffer& cmd);

    void copyBuffer(vk::raii::Buffer& src, vk::raii::Buffer& dst, vk::DeviceSize size);
    void copyBufferToImage(const vk::raii::Buffer& buffer, vk::raii::Image& image,
                           uint32_t width, uint32_t height);
    void transitionImageLayout(const vk::raii::Image& image,
                               vk::ImageLayout oldLayout, vk::ImageLayout newLayout);

private:
    vk::raii::Context context;
    vk::raii::Instance instance = nullptr;
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
    vk::raii::SurfaceKHR surface = nullptr;
    vk::raii::PhysicalDevice physicalDevice = nullptr;
    vk::raii::Device device = nullptr;
    vk::raii::Queue queue = nullptr;
    vk::raii::CommandPool uploadCommandPool_ = nullptr; // must be after device
    uint32_t graphicsIndex;

    vk::SampleCountFlagBits msaaSamples;
    vk::SampleCountFlagBits maxMsaaSamples;
    vk::SampleCountFlags supportedMsaaSamples;
    std::vector<const char *> requiredDeviceExtensions;
    std::vector<const char *> validationLayers;

    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow *window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createUploadCommandPool();
    bool isDeviceSuitable(const vk::raii::PhysicalDevice &device);

    vk::SampleCountFlagBits getMaxUsableSampleCount();
    std::vector<const char *> getRequiredExtensions();
    static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(
        vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        vk::DebugUtilsMessageTypeFlagsEXT messageType,
        const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData,
        void *pUserData);
};