#pragma once

#include <cstdint>
#include <string>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vulkan/vulkan_raii.hpp>

// GPU-side mesh instance: owns Vulkan buffers, a model matrix, and
// texture indices into the bindless array from TextureManager.
// The decomposed transform is kept alongside the baked matrix so the
// ImGui sliders can edit it at runtime.
struct Renderable
{
    vk::raii::Buffer       vertexBuffer       = nullptr;
    vk::raii::DeviceMemory vertexBufferMemory = nullptr;
    vk::raii::Buffer       indexBuffer        = nullptr;
    vk::raii::DeviceMemory indexBufferMemory  = nullptr;
    uint32_t indexCount = 0;

    glm::mat4 modelMatrix            = glm::mat4(1.0f);
    uint32_t  textureIndex           = 0xFFFFu;
    uint32_t  metallicRoughnessIndex = 0xFFFFu;
    uint32_t  normalMapIndex         = 0xFFFFu;
    uint32_t  heightMapIndex         = 0xFFFFu;

    std::string label;
    glm::vec3   position    = {0.0f, 0.0f, 0.0f};
    glm::vec3   rotationDeg = {0.0f, 0.0f, 0.0f}; // XYZ Euler, degrees
    float       scale       = 1.0f;
};

// Bakes position/rotation/scale into a model matrix (XYZ Euler order).
inline glm::mat4 buildModelMatrix(glm::vec3 pos, glm::vec3 rotDeg, float scale)
{
    glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
    m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
    return glm::scale(m, glm::vec3(scale));
}
