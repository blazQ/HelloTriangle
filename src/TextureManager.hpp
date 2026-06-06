#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <filesystem>
#include <unordered_map>
#include <vector>

class Device;

// Owns every GPU texture used by the renderer.
// Textures are loaded on demand and cached by path; the same file is never
// uploaded twice. All loaded textures remain alive until the TextureManager
// is destroyed (or reset()), which makes the bindless index stable.
class TextureManager
{
  public:
    static constexpr uint32_t MAX_TEXTURES = 2048;

    TextureManager() = default;
    void init(Device& device);

    // Loads from disk. Returns the cached index immediately if already loaded.
    // linearFormat = true for data textures (normal maps, metallic-roughness).
    uint32_t load(const std::filesystem::path& path, bool linearFormat = false);

    // Loads from an in-memory byte buffer (used for GLB-embedded images).
    uint32_t loadFromMemory(const std::vector<uint8_t>& bytes, bool linearFormat = false);

    size_t count() const { return textures_.size(); }
    bool atLimit() const { return textures_.size() >= MAX_TEXTURES; }

    // Returns the DescriptorImageInfo array needed when writing the bindless
    // texture binding (binding 1) of the scene descriptor set.
    std::vector<vk::DescriptorImageInfo> descriptorImageInfos() const;

  private:
    struct Texture
    {
        vk::raii::Image image =
            nullptr; // Raw pixel data in GPU memory. Multi-dimensional image and its mipmap levels.
        vk::raii::DeviceMemory memory = nullptr; // Memory that holds the image.
        vk::raii::ImageView view = nullptr; // Specifies which part of an image to use, which format
                                            // and how to interpret data.
        vk::raii::Sampler sampler =
            nullptr; // How to sample the texture: nearest or linear, edge wrapping, anisotropy.
        // TODO: The sampler could actually be just one global object.
    };

    Device* device_ = nullptr;
    std::vector<Texture> textures_;
    std::unordered_map<std::string, uint32_t> cache_;

    // Uploads raw RGBA pixels to a device-local image with full mip chain.
    // Appends to textures_ and returns the new index.
    uint32_t uploadPixels(const uint8_t* pixels, int w, int h, vk::Format fmt);

    void generateMipmaps(
        vk::raii::Image& image, vk::Format format, int32_t w, int32_t h, uint32_t mipLevels);
};
