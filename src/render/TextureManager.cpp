#include "lumin/render/TextureManager.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace lumin::render {
    namespace {

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

    }

    TextureManager::TextureManager(VulkanContext& context) : context_(context), resources_(context) {
    }

    TextureManager::~TextureManager() {
        destroy();
    }

    void TextureManager::create(VkExtent2D extent) {
        destroy();
        createImages(extent);
        createSamplerAndDescriptors();
    }

    void TextureManager::destroy() noexcept {
        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_.device(), descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }
        if (descriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_.device(), descriptorSetLayout_, nullptr);
            descriptorSetLayout_ = VK_NULL_HANDLE;
        }
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(context_.device(), sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        for (TextureFrameResources& frame : frames_) {
            resources_.destroyImage(frame.position);
            resources_.destroyImage(frame.normalRoughness);
            resources_.destroyImage(frame.albedo);
            resources_.destroyImage(frame.depth);
        }
    }

    const TextureFrameResources& TextureManager::frame(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager frame index is out of range.");
        }
        return frames_[frameIndex];
    }

    VkFormat TextureManager::positionFormat() const noexcept {
        return positionFormat_;
    }

    VkFormat TextureManager::normalFormat() const noexcept {
        return normalFormat_;
    }

    VkFormat TextureManager::albedoFormat() const noexcept {
        return albedoFormat_;
    }

    VkFormat TextureManager::depthFormat() const noexcept {
        return depthFormat_;
    }

    VkSampler TextureManager::sampler() const noexcept {
        return sampler_;
    }

    VkDescriptorSetLayout TextureManager::descriptorSetLayout() const noexcept {
        return descriptorSetLayout_;
    }

    VkDescriptorSet TextureManager::descriptorSet(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager descriptor frame index is out of range.");
        }
        return descriptorSets_[frameIndex];
    }

    VkFormat TextureManager::chooseFormat(const std::vector<VkFormat>& candidates) const {
        return resources_.findSupportedFormat(candidates, VK_IMAGE_TILING_OPTIMAL,
                                              VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    }

    void TextureManager::createImages(VkExtent2D extent) {
        positionFormat_ = chooseFormat({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT});
        normalFormat_ = positionFormat_;
        albedoFormat_ = chooseFormat({VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM});
        depthFormat_ = resources_.findDepthFormat();
        for (TextureFrameResources& frame : frames_) {
            frame.position = resources_.createImage(extent.width, extent.height, positionFormat_,
                                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                     VK_IMAGE_ASPECT_COLOR_BIT);
            frame.normalRoughness = resources_.createImage(
                extent.width, extent.height, normalFormat_,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            frame.albedo = resources_.createImage(extent.width, extent.height, albedoFormat_,
                                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  VK_IMAGE_ASPECT_COLOR_BIT);
            frame.depth = resources_.createImage(extent.width, extent.height, depthFormat_,
                                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        }
    }

    void TextureManager::createSamplerAndDescriptors() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 1.0f;
        checkVk(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_),
                "Failed to create G-buffer sampler.");

        const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(context_.device(), &layoutInfo, nullptr, &descriptorSetLayout_),
                "Failed to create texture descriptor set layout.");

        const std::array<VkDescriptorPoolSize, 2> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxFramesInFlight * 3},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, maxFramesInFlight},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = maxFramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        checkVk(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &descriptorPool_),
                "Failed to create texture descriptor pool.");

        const std::array<VkDescriptorSetLayout, maxFramesInFlight> layouts = {descriptorSetLayout_,
                                                                                descriptorSetLayout_};
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = descriptorPool_;
        allocateInfo.descriptorSetCount = maxFramesInFlight;
        allocateInfo.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(context_.device(), &allocateInfo, descriptorSets_.data()),
                "Failed to allocate texture descriptor sets.");

        for (std::uint32_t index = 0; index < maxFramesInFlight; ++index) {
            const std::array<VkDescriptorImageInfo, 3> images = {
                VkDescriptorImageInfo{VK_NULL_HANDLE, frames_[index].position.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                VkDescriptorImageInfo{VK_NULL_HANDLE, frames_[index].normalRoughness.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                VkDescriptorImageInfo{VK_NULL_HANDLE, frames_[index].albedo.view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            };
            const VkDescriptorImageInfo samplerInfo{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (std::uint32_t binding = 0; binding < 3; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = descriptorSets_[index];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[binding].pImageInfo = &images[binding];
            }
            writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3].dstSet = descriptorSets_[index];
            writes[3].dstBinding = 3;
            writes[3].descriptorCount = 1;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[3].pImageInfo = &samplerInfo;
            vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                                   nullptr);
        }
    }

}
