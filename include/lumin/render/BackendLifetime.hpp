#pragma once

#include <utility>

namespace lumin::render::detail {

    struct BackendLifetimeAvailability {
        bool children = false;
        bool swapchainWrappers = false;
        bool rhiDevice = false;
        bool vulkanDevice = false;
    };

    template <typename DestroyChildren, typename DestroySwapchainWrappers, typename DestroyRhiDevice,
              typename DestroyVulkanDevice>
    void destroyBackendLifetime(bool& destroyed, BackendLifetimeAvailability availability,
                                DestroyChildren&& destroyChildren,
                                DestroySwapchainWrappers&& destroySwapchainWrappers,
                                DestroyRhiDevice&& destroyRhiDevice, DestroyVulkanDevice&& destroyVulkanDevice) noexcept {
        if (destroyed) {
            return;
        }
        destroyed = true;
        if (availability.children) {
            std::forward<DestroyChildren>(destroyChildren)();
        }
        if (availability.swapchainWrappers) {
            std::forward<DestroySwapchainWrappers>(destroySwapchainWrappers)();
        }
        if (availability.rhiDevice) {
            std::forward<DestroyRhiDevice>(destroyRhiDevice)();
        }
        if (availability.vulkanDevice) {
            std::forward<DestroyVulkanDevice>(destroyVulkanDevice)();
        }
    }

}
