#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include <concepts>
#include <type_traits>

namespace {

    static_assert(std::is_enum_v<nvrhi::ResourceStates>);
    static_assert(std::is_default_constructible_v<nvrhi::vulkan::DeviceDesc>);

    static_assert(requires(nvrhi::ICommandList* commandList) {
        commandList->setEnableAutomaticBarriers(true);
    });

    static_assert(requires(nvrhi::IDevice* device, nvrhi::ObjectType objectType, nvrhi::Object nativeTexture,
                           const nvrhi::TextureDesc& textureDesc) {
        { device->createHandleForNativeTexture(objectType, nativeTexture, textureDesc) }
            -> std::same_as<nvrhi::TextureHandle>;
    });

}

int main() {
    return 0;
}
