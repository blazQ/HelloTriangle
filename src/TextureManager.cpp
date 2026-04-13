#include "TextureManager.hpp"
#include "Device.hpp"

#include <cmath>
#include <stdexcept>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

void TextureManager::init(Device& device)
{
    device_ = &device;
}

uint32_t TextureManager::load(const std::filesystem::path& path, bool linearFormat)
{
    if (atLimit())
        throw std::runtime_error("texture limit reached");

    // Cache hit: same path already uploaded.
    auto it = cache_.find(path.string());
    if (it != cache_.end())
        return it->second;

    int w, h, channels;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels)
        throw std::runtime_error("failed to load texture: " + path.string());

    vk::Format fmt = linearFormat ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
    uint32_t index = uploadPixels(pixels, w, h, fmt);
    stbi_image_free(pixels);

    cache_[path.string()] = index;
    return index;
}

uint32_t TextureManager::loadFromMemory(const std::vector<uint8_t>& bytes, bool linearFormat)
{
    if (atLimit())
        throw std::runtime_error("texture limit reached");

    int w, h, channels;
    stbi_uc* pixels = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels)
        throw std::runtime_error("failed to decode texture from memory");

    vk::Format fmt = linearFormat ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
    uint32_t index = uploadPixels(pixels, w, h, fmt);
    stbi_image_free(pixels);
    return index;
}

uint32_t TextureManager::uploadPixels(const uint8_t* pixels, int w, int h, vk::Format fmt)
{
    uint32_t mipLevels = static_cast<uint32_t>(
        std::floor(std::log2(std::max(w, h)))) + 1;
    vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(w) * h * 4;

    // Stage on the CPU side.
    vk::raii::Buffer       staging({});
    vk::raii::DeviceMemory stagingMem({});
    device_->createBuffer(
        imageSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        staging, stagingMem);
    void* data = stagingMem.mapMemory(0, imageSize);
    memcpy(data, pixels, imageSize);
    stagingMem.unmapMemory();

    // Allocate device-local image.
    Texture tex;
    device_->createImage(
        static_cast<uint32_t>(w), static_cast<uint32_t>(h), mipLevels,
        vk::SampleCountFlagBits::e1, fmt, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eTransferSrc  |
        vk::ImageUsageFlagBits::eTransferDst  |
        vk::ImageUsageFlagBits::eSampled,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        tex.image, tex.memory);

    device_->transitionImageLayout(tex.image,
        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
    device_->copyBufferToImage(staging, tex.image,
        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    generateMipmaps(tex.image, fmt, w, h, mipLevels);

    tex.view = device_->createImageView(*tex.image, fmt,
        vk::ImageAspectFlagBits::eColor, mipLevels);

    vk::PhysicalDeviceProperties props =
        device_->getPhysicalDevice().getProperties();
    vk::SamplerCreateInfo samplerInfo{
        .magFilter               = vk::Filter::eLinear,
        .minFilter               = vk::Filter::eLinear,
        .mipmapMode              = vk::SamplerMipmapMode::eLinear,
        .addressModeU            = vk::SamplerAddressMode::eRepeat,
        .addressModeV            = vk::SamplerAddressMode::eRepeat,
        .addressModeW            = vk::SamplerAddressMode::eRepeat,
        .anisotropyEnable        = vk::True,
        .maxAnisotropy           = props.limits.maxSamplerAnisotropy,
        .compareOp               = vk::CompareOp::eAlways,
        .maxLod                  = vk::LodClampNone,
        .borderColor             = vk::BorderColor::eIntOpaqueBlack,
        .unnormalizedCoordinates = vk::False};
    tex.sampler = vk::raii::Sampler(device_->getLogicalDevice(), samplerInfo);

    uint32_t index = static_cast<uint32_t>(textures_.size());
    textures_.push_back(std::move(tex));
    return index;
}

// Generates mip levels 1..N from mip 0. For each level:
//   1. Barrier level i-1: TransferDst → TransferSrc
//   2. Blit level i-1 → level i (half resolution)
//   3. Barrier level i-1: TransferSrc → ShaderReadOnly
// After the loop the last level (never used as blit src) is transitioned
// to ShaderReadOnly separately.
void TextureManager::generateMipmaps(vk::raii::Image& image, vk::Format format,
                                      int32_t texWidth, int32_t texHeight,
                                      uint32_t mipLevels)
{
    vk::FormatProperties fmtProps =
        device_->getPhysicalDevice().getFormatProperties(format);
    if (!(fmtProps.optimalTilingFeatures &
          vk::FormatFeatureFlagBits::eSampledImageFilterLinear))
        throw std::runtime_error(
            "texture image format does not support linear blitting");

    auto cmd = device_->beginSingleTimeCommands();

    vk::ImageMemoryBarrier barrier{
        .srcAccessMask       = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask       = vk::AccessFlagBits::eTransferRead,
        .oldLayout           = vk::ImageLayout::eTransferDstOptimal,
        .newLayout           = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image               = *image,
        .subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};

    int32_t mipW = texWidth, mipH = texHeight;
    for (uint32_t i = 1; i < mipLevels; ++i)
    {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout     = vk::ImageLayout::eTransferSrcOptimal;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        cmd->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eTransfer,
                             {}, {}, {}, barrier);

        vk::ArrayWrapper1D<vk::Offset3D, 2> srcOff, dstOff;
        srcOff[0] = vk::Offset3D{0, 0, 0};
        srcOff[1] = vk::Offset3D{mipW, mipH, 1};
        dstOff[0] = vk::Offset3D{0, 0, 0};
        dstOff[1] = vk::Offset3D{mipW > 1 ? mipW / 2 : 1, mipH > 1 ? mipH / 2 : 1, 1};

        vk::ImageBlit blit{};
        blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, i - 1, 0, 1};
        blit.srcOffsets     = srcOff;
        blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, i,     0, 1};
        blit.dstOffsets     = dstOff;
        cmd->blitImage(image, vk::ImageLayout::eTransferSrcOptimal,
                       image, vk::ImageLayout::eTransferDstOptimal,
                       {blit}, vk::Filter::eLinear);

        barrier.oldLayout     = vk::ImageLayout::eTransferSrcOptimal;
        barrier.newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eFragmentShader,
                             {}, {}, {}, barrier);

        if (mipW > 1) mipW /= 2;
        if (mipH > 1) mipH /= 2;
    }

    // Transition the last mip level (was only ever a blit destination).
    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    cmd->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eFragmentShader,
                         {}, {}, {}, barrier);

    device_->endSingleTimeCommands(*cmd);
}

std::vector<vk::DescriptorImageInfo> TextureManager::descriptorImageInfos() const
{
    std::vector<vk::DescriptorImageInfo> infos;
    infos.reserve(textures_.size());
    for (const auto& tex : textures_)
        infos.push_back({*tex.sampler, *tex.view,
                         vk::ImageLayout::eShaderReadOnlyOptimal});
    return infos;
}
