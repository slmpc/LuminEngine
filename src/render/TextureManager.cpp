#include "lumin/render/TextureManager.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace lumin::render {
    namespace {

        constexpr std::uint32_t sampledImageCount = 8 + shadowCascadeCount;
        constexpr std::uint32_t samplerBinding = sampledImageCount;
        constexpr std::uint32_t uniformBinding = sampledImageCount + 1;

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

    } // namespace

    TextureManager::TextureManager(VulkanContext& context) : context_(context), resources_(context) {
    }

    TextureManager::~TextureManager() {
        destroy();
    }

    void TextureManager::create(VkExtent2D extent) {
        destroy();
        if (extent.width == 0 || extent.height == 0) {
            throw std::invalid_argument("TextureManager requires a non-zero render extent.");
        }
        try {
            createImages(extent);
            createSamplerAndDescriptors();
        } catch (...) {
            destroy();
            throw;
        }
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
            resources_.destroyBuffer(frame.postProcessUniform);
            for (VulkanImage& shadow : frame.shadowCascades) {
                resources_.destroyImage(shadow);
            }
            resources_.destroyImage(frame.history);
            resources_.destroyImage(frame.taaResolved);
            resources_.destroyImage(frame.lighting);
            resources_.destroyImage(frame.globalIllumination);
            resources_.destroyImage(frame.depth);
            resources_.destroyImage(frame.motion);
            resources_.destroyImage(frame.albedo);
            resources_.destroyImage(frame.normalRoughness);
            resources_.destroyImage(frame.position);
        }
        descriptorSets_.fill(VK_NULL_HANDLE);
        historyValid_.fill(false);
        historyInitialized_.fill(false);
    }

    void TextureManager::updatePostProcessUniforms(std::uint32_t frameIndex, const PostProcessUniforms& uniforms) {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager uniform frame index is out of range.");
        }
        resources_.writeBuffer(frames_[frameIndex].postProcessUniform, &uniforms, sizeof(uniforms));
    }

    void TextureManager::invalidateHistory() noexcept {
        historyValid_.fill(false);
    }

    void TextureManager::markHistoryValid(std::uint32_t frameIndex) {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager history frame index is out of range.");
        }
        historyValid_[frameIndex] = true;
        historyInitialized_[frameIndex] = true;
    }

    const TextureFrameResources& TextureManager::frame(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager frame index is out of range.");
        }
        return frames_[frameIndex];
    }

    bool TextureManager::historyValid(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager history frame index is out of range.");
        }
        return historyValid_[frameIndex];
    }

    bool TextureManager::historyInitialized(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager history frame index is out of range.");
        }
        return historyInitialized_[frameIndex];
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

    VkFormat TextureManager::motionFormat() const noexcept {
        return motionFormat_;
    }

    VkFormat TextureManager::depthFormat() const noexcept {
        return depthFormat_;
    }

    VkFormat TextureManager::globalIlluminationFormat() const noexcept {
        return globalIlluminationFormat_;
    }

    VkFormat TextureManager::lightingFormat() const noexcept {
        return lightingFormat_;
    }

    VkFormat TextureManager::shadowDepthFormat() const noexcept {
        return shadowDepthFormat_;
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
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    }

    void TextureManager::createImages(VkExtent2D extent) {
        positionFormat_ = chooseFormat({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT});
        normalFormat_ = positionFormat_;
        albedoFormat_ = chooseFormat({VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM});
        motionFormat_ = chooseFormat({VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT});
        globalIlluminationFormat_ = resources_.findSupportedFormat(
            {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}, VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        lightingFormat_ = chooseFormat({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT});
        depthFormat_ = resources_.findDepthFormat();
        shadowDepthFormat_ = resources_.findSupportedFormat(
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}, VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);

        const VkImageUsageFlags colorSampled = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const VkMemoryPropertyFlags hostMemory =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (TextureFrameResources& frame : frames_) {
            frame.position = resources_.createImage(extent.width, extent.height, positionFormat_, colorSampled,
                                                    VK_IMAGE_ASPECT_COLOR_BIT);
            frame.normalRoughness = resources_.createImage(extent.width, extent.height, normalFormat_, colorSampled,
                                                           VK_IMAGE_ASPECT_COLOR_BIT);
            frame.albedo = resources_.createImage(extent.width, extent.height, albedoFormat_, colorSampled,
                                                  VK_IMAGE_ASPECT_COLOR_BIT);
            frame.motion = resources_.createImage(extent.width, extent.height, motionFormat_, colorSampled,
                                                  VK_IMAGE_ASPECT_COLOR_BIT);
            frame.depth =
                resources_.createImage(extent.width, extent.height, depthFormat_,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            frame.globalIllumination =
                resources_.createImage(extent.width, extent.height, globalIlluminationFormat_,
                                       colorSampled | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            frame.lighting = resources_.createImage(extent.width, extent.height, lightingFormat_, colorSampled,
                                                    VK_IMAGE_ASPECT_COLOR_BIT);
            frame.taaResolved =
                resources_.createImage(extent.width, extent.height, lightingFormat_,
                                       colorSampled | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            frame.history = resources_.createImage(extent.width, extent.height, lightingFormat_,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                   VK_IMAGE_ASPECT_COLOR_BIT);
            for (VulkanImage& shadow : frame.shadowCascades) {
                shadow =
                    resources_.createImage(shadowMapResolution, shadowMapResolution, shadowDepthFormat_,
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           VK_IMAGE_ASPECT_DEPTH_BIT);
            }
            frame.postProcessUniform =
                resources_.createBuffer(sizeof(PostProcessUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostMemory);
        }
        historyValid_.fill(false);
        historyInitialized_.fill(false);
    }

    void TextureManager::createSamplerAndDescriptors() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        checkVk(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_),
                "Failed to create render texture sampler.");

        std::array<VkDescriptorSetLayoutBinding, sampledImageCount + 2> bindings{};
        for (std::uint32_t binding = 0; binding < sampledImageCount; ++binding) {
            bindings[binding] = VkDescriptorSetLayoutBinding{binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        bindings[samplerBinding] = VkDescriptorSetLayoutBinding{samplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[uniformBinding] = VkDescriptorSetLayoutBinding{uniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(context_.device(), &layoutInfo, nullptr, &descriptorSetLayout_),
                "Failed to create render texture descriptor set layout.");

        const std::array<VkDescriptorPoolSize, 3> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxFramesInFlight * sampledImageCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, maxFramesInFlight},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxFramesInFlight},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = maxFramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        checkVk(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &descriptorPool_),
                "Failed to create render texture descriptor pool.");

        std::array<VkDescriptorSetLayout, maxFramesInFlight> layouts{};
        layouts.fill(descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = descriptorPool_;
        allocateInfo.descriptorSetCount = maxFramesInFlight;
        allocateInfo.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(context_.device(), &allocateInfo, descriptorSets_.data()),
                "Failed to allocate render texture descriptor sets.");

        for (std::uint32_t frameIndex = 0; frameIndex < maxFramesInFlight; ++frameIndex) {
            const TextureFrameResources& frame = frames_[frameIndex];
            const TextureFrameResources& previousFrame =
                frames_[(frameIndex + maxFramesInFlight - 1) % maxFramesInFlight];
            std::array<VkDescriptorImageInfo, sampledImageCount> images{};
            const std::array<const VulkanImage*, 8> frameImages = {
                &frame.position,           &frame.normalRoughness, &frame.albedo,          &frame.motion,
                &frame.globalIllumination, &frame.lighting,        &previousFrame.history, &frame.taaResolved,
            };
            for (std::uint32_t index = 0; index < frameImages.size(); ++index) {
                images[index] = VkDescriptorImageInfo{VK_NULL_HANDLE, frameImages[index]->view,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            }
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                images[8 + cascade] = VkDescriptorImageInfo{VK_NULL_HANDLE, frame.shadowCascades[cascade].view,
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            }
            const VkDescriptorImageInfo samplerDescriptor{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDescriptorBufferInfo uniformDescriptor{frame.postProcessUniform.buffer, 0,
                                                           sizeof(PostProcessUniforms)};

            std::array<VkWriteDescriptorSet, sampledImageCount + 2> writes{};
            for (std::uint32_t binding = 0; binding < sampledImageCount; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = descriptorSets_[frameIndex];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[binding].pImageInfo = &images[binding];
            }
            writes[samplerBinding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[samplerBinding].dstSet = descriptorSets_[frameIndex];
            writes[samplerBinding].dstBinding = samplerBinding;
            writes[samplerBinding].descriptorCount = 1;
            writes[samplerBinding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[samplerBinding].pImageInfo = &samplerDescriptor;

            writes[uniformBinding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[uniformBinding].dstSet = descriptorSets_[frameIndex];
            writes[uniformBinding].dstBinding = uniformBinding;
            writes[uniformBinding].descriptorCount = 1;
            writes[uniformBinding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[uniformBinding].pBufferInfo = &uniformDescriptor;

            vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                                   nullptr);
        }
    }

} // namespace lumin::render
