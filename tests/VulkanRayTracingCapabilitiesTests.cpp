#include "render/RayTracingBuildConfiguration.hpp"
#include "render/platform/vulkan/VulkanRayTracingCapabilities.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using lumin::render::configuredRayTracingBuildMode;
    using lumin::render::enabledVulkanRayTracingDeviceExtensions;
    using lumin::render::evaluateVulkanRayTracingSupport;
    using lumin::render::formatRayTracingDecision;
    using lumin::render::inspectVulkanRayTracingExtensions;
    using lumin::render::RayTracingDecisionStatus;
    using lumin::render::RayTracingDiagnosticCode;
    using lumin::render::rayTracingImplementationAvailable;
    using lumin::render::RayTracingMode;
    using lumin::render::RayTracingPolicy;
    using lumin::render::resolveRayTracingPolicy;
    using lumin::render::VulkanRayTracingDeviceProbe;
    using lumin::render::VulkanRayTracingRequirement;
    using lumin::render::VulkanRayTracingTier;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function>
    void requireThrows(Function&& function, const std::string& message) {
        bool rejected = false;
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            rejected = true;
        }
        require(rejected, message);
    }

    [[nodiscard]] VulkanRayTracingDeviceProbe completeProbe(bool rayQuery = true) {
        return VulkanRayTracingDeviceProbe{
            .extensions =
                {
                    .accelerationStructure = true,
                    .rayTracingPipeline = true,
                    .deferredHostOperations = true,
                    .rayQuery = rayQuery,
                    .computeShaderDerivatives = true,
                },
            .features =
                {
                    .shaderInt64 = true,
                    .shaderFloat16 = true,
                    .uniformAndStorageBuffer16BitAccess = true,
                    .storageBuffer16BitAccess = true,
                    .bufferDeviceAddress = true,
                    .shaderStorageBufferArrayNonUniformIndexing = true,
                    .descriptorBindingPartiallyBound = true,
                    .accelerationStructure = true,
                    .rayTracingPipeline = true,
                    .rayQuery = rayQuery,
                    .computeDerivativeGroupQuads = true,
                },
            .maxRayRecursionDepth = 2,
        };
    }

    void testExtensionInspectionIsExactAndComplete() {
        const std::array extensions{
            std::string_view{"VK_KHR_acceleration_structure"},     std::string_view{"VK_KHR_ray_tracing_pipeline"},
            std::string_view{"VK_KHR_deferred_host_operations"},   std::string_view{"VK_KHR_ray_query"},
            std::string_view{"VK_KHR_compute_shader_derivatives"}, std::string_view{"VK_KHR_swapchain"},
        };
        const auto support = inspectVulkanRayTracingExtensions(extensions);
        require(support.accelerationStructure && support.rayTracingPipeline && support.deferredHostOperations &&
                    support.rayQuery && support.computeShaderDerivatives,
                "All exact RT extension names must be detected.");

        const std::array wrongCase{std::string_view{"vk_khr_acceleration_structure"}};
        require(!inspectVulkanRayTracingExtensions(wrongCase).accelerationStructure,
                "Vulkan extension matching must remain case-sensitive.");
    }

    void testCapabilityTierRequiresTheWholePipelineBundle() {
        auto complete = evaluateVulkanRayTracingSupport(completeProbe());
        require(complete.tier == VulkanRayTracingTier::RayTracingPipelineWithRayQuery &&
                    complete.supportsRayTracingPipeline() && complete.supportsRayQuery(),
                "A complete device must expose pipeline plus optional ray-query tier.");
        require(complete.missingPipelineRequirements.empty(),
                "A complete pipeline bundle must not report missing requirements.");
        require(complete.supportsSharcShaderStorage(),
                "A complete probe must expose the shader storage capabilities required by SHARC.");

        VulkanRayTracingDeviceProbe noSharcStorage = completeProbe();
        noSharcStorage.features.shaderFloat16 = false;
        const auto rawRtOnly = evaluateVulkanRayTracingSupport(noSharcStorage);
        require(rawRtOnly.supportsRayTracingPipeline() && !rawRtOnly.supportsSharcShaderStorage(),
                "Missing SHARC storage features must not disable the otherwise valid raw RT path.");

        VulkanRayTracingDeviceProbe missingDeferred = completeProbe();
        missingDeferred.extensions.deferredHostOperations = false;
        const auto incomplete = evaluateVulkanRayTracingSupport(missingDeferred);
        require(!incomplete.supportsRayTracingPipeline() && !incomplete.supportsRayQuery(),
                "Acceleration structures must require deferred host operations for both RT paths.");
        require(std::find(incomplete.missingPipelineRequirements.begin(), incomplete.missingPipelineRequirements.end(),
                          VulkanRayTracingRequirement::DeferredHostOperationsExtension) !=
                    incomplete.missingPipelineRequirements.end(),
                "The missing deferred-host-operations extension must be reported explicitly.");

        VulkanRayTracingDeviceProbe queryOnly = completeProbe();
        queryOnly.extensions.rayTracingPipeline = false;
        queryOnly.features.rayTracingPipeline = false;
        queryOnly.maxRayRecursionDepth = 0;
        const auto queryOnlySupport = evaluateVulkanRayTracingSupport(queryOnly);
        require(queryOnlySupport.tier == VulkanRayTracingTier::RayQuery && queryOnlySupport.supportsRayQuery() &&
                    !queryOnlySupport.supportsRayTracingPipeline(),
                "Ray Query support must remain independent from Ray Tracing Pipeline support.");
    }

    void testPipelineDescriptorFeaturesAndRecursionDepthAreRequired() {
        VulkanRayTracingDeviceProbe missingNonUniformStorage = completeProbe();
        missingNonUniformStorage.features.shaderStorageBufferArrayNonUniformIndexing = false;
        const auto missingNonUniformSupport = evaluateVulkanRayTracingSupport(missingNonUniformStorage);
        require(!missingNonUniformSupport.supportsRayTracingPipeline(),
                "Storage-buffer descriptor arrays must support non-uniform indexing for the RT pipeline.");
        require(std::find(missingNonUniformSupport.missingPipelineRequirements.begin(),
                          missingNonUniformSupport.missingPipelineRequirements.end(),
                          VulkanRayTracingRequirement::ShaderStorageBufferArrayNonUniformIndexingFeature) !=
                    missingNonUniformSupport.missingPipelineRequirements.end(),
                "Missing shaderStorageBufferArrayNonUniformIndexing must be reported explicitly.");

        VulkanRayTracingDeviceProbe missingPartiallyBound = completeProbe();
        missingPartiallyBound.features.descriptorBindingPartiallyBound = false;
        const auto missingPartiallyBoundSupport = evaluateVulkanRayTracingSupport(missingPartiallyBound);
        require(!missingPartiallyBoundSupport.supportsRayTracingPipeline(),
                "Partially-bound descriptor arrays must be supported for the RT pipeline.");
        require(std::find(missingPartiallyBoundSupport.missingPipelineRequirements.begin(),
                          missingPartiallyBoundSupport.missingPipelineRequirements.end(),
                          VulkanRayTracingRequirement::DescriptorBindingPartiallyBoundFeature) !=
                    missingPartiallyBoundSupport.missingPipelineRequirements.end(),
                "Missing descriptorBindingPartiallyBound must be reported explicitly.");

        VulkanRayTracingDeviceProbe shallowPipeline = completeProbe();
        shallowPipeline.maxRayRecursionDepth = 1;
        const auto shallowSupport = evaluateVulkanRayTracingSupport(shallowPipeline);
        require(!shallowSupport.supportsRayTracingPipeline(),
                "A recursion depth of one must not satisfy the two-level RT pipeline contract.");
        require(std::find(shallowSupport.missingPipelineRequirements.begin(),
                          shallowSupport.missingPipelineRequirements.end(),
                          VulkanRayTracingRequirement::RayRecursionDepth) !=
                    shallowSupport.missingPipelineRequirements.end(),
                "Insufficient maxRayRecursionDepth must be reported explicitly.");

        const auto depthTwoSupport = evaluateVulkanRayTracingSupport(completeProbe());
        require(depthTwoSupport.supportsRayTracingPipeline(),
                "A recursion depth of two must satisfy the RT pipeline contract.");

        VulkanRayTracingDeviceProbe missingBothDescriptorFeatures = completeProbe();
        missingBothDescriptorFeatures.features.shaderStorageBufferArrayNonUniformIndexing = false;
        missingBothDescriptorFeatures.features.descriptorBindingPartiallyBound = false;
        const auto decision = resolveRayTracingPolicy(RayTracingPolicy{}, true,
                                                      evaluateVulkanRayTracingSupport(missingBothDescriptorFeatures));
        require(decision.status == RayTracingDecisionStatus::Fallback && decision.diagnostics.size() == 2,
                "AUTO must diagnose both missing descriptor indexing features before falling back.");
        require(std::all_of(decision.diagnostics.begin(), decision.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == RayTracingDiagnosticCode::MissingDeviceFeature;
                            }),
                "Descriptor indexing requirements must be classified as missing Vulkan features.");
        require(decision.diagnostics[0].subject == "shaderStorageBufferArrayNonUniformIndexing" &&
                    decision.diagnostics[1].subject == "descriptorBindingPartiallyBound",
                "Descriptor feature diagnostics must retain their stable requirement order and Vulkan names.");
    }

    void testAutoEnablesACompleteDeviceAndPropagatesExtensions() {
        const auto support = evaluateVulkanRayTracingSupport(completeProbe());
        const auto decision = resolveRayTracingPolicy(RayTracingPolicy{}, true, support);
        require(decision.status == RayTracingDecisionStatus::Enabled && decision.enabled() && !decision.rejected(),
                "AUTO/AUTO must enable a complete built implementation.");
        require(decision.tier == VulkanRayTracingTier::RayTracingPipelineWithRayQuery && decision.enablesRayQuery(),
                "The enabled decision must retain optional Ray Query support.");

        const std::vector<std::string_view> extensions = enabledVulkanRayTracingDeviceExtensions(decision);
        require(extensions == std::vector<std::string_view>{"VK_KHR_acceleration_structure",
                                                            "VK_KHR_ray_tracing_pipeline",
                                                            "VK_KHR_deferred_host_operations",
                                                            "VK_KHR_compute_shader_derivatives", "VK_KHR_ray_query"},
                "The exact enabled RT extension bundle must be reusable by Vulkan and NvRHI device creation.");
        require(formatRayTracingDecision(decision).find("RayTracingPipelineWithRayQuery") != std::string::npos,
                "Enabled diagnostics must expose the selected tier.");
    }

    void testAutoFallbackIsNonFatalAndDiagnostic() {
        VulkanRayTracingDeviceProbe probe = completeProbe(false);
        probe.extensions.rayTracingPipeline = false;
        probe.features.bufferDeviceAddress = false;
        const auto support = evaluateVulkanRayTracingSupport(probe);
        const auto decision = resolveRayTracingPolicy(RayTracingPolicy{}, true, support);

        require(decision.status == RayTracingDecisionStatus::Fallback && !decision.enabled() && !decision.rejected(),
                "AUTO must fall back without rejecting an incomplete device.");
        require(enabledVulkanRayTracingDeviceExtensions(decision).empty(),
                "Fallback must not enable any RT extension or permit RT resource creation.");
        require(std::any_of(decision.diagnostics.begin(), decision.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == RayTracingDiagnosticCode::MissingDeviceExtension &&
                                       diagnostic.subject == "VK_KHR_ray_tracing_pipeline";
                            }),
                "Fallback diagnostics must identify a missing device extension.");
        require(std::any_of(decision.diagnostics.begin(), decision.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == RayTracingDiagnosticCode::MissingDeviceFeature &&
                                       diagnostic.subject == "bufferDeviceAddress";
                            }),
                "Fallback diagnostics must identify a missing device feature.");
        const std::string message = formatRayTracingDecision(decision);
        require(message.find("AUTO fallback") != std::string::npos &&
                    message.find("VK_KHR_ray_tracing_pipeline") != std::string::npos &&
                    message.find("bufferDeviceAddress") != std::string::npos,
                "Formatted fallback diagnostics must be actionable without inspecting raw flags.");

        const auto inconsistent = resolveRayTracingPolicy(RayTracingPolicy{}, true, {});
        require(inconsistent.status == RayTracingDecisionStatus::Fallback && !inconsistent.diagnostics.empty(),
                "Even a hand-built inconsistent capability snapshot must produce a fallback reason.");
    }

    void testOnRejectsAndOffSuppressesDeviceRequirements() {
        VulkanRayTracingDeviceProbe probe = completeProbe();
        probe.features.rayTracingPipeline = false;
        const auto support = evaluateVulkanRayTracingSupport(probe);

        const auto required = resolveRayTracingPolicy(
            RayTracingPolicy{.build = RayTracingMode::On, .runtime = RayTracingMode::On}, true, support);
        require(required.status == RayTracingDecisionStatus::Rejected && required.rejected(),
                "Runtime ON must reject a device that cannot create a ray-tracing pipeline.");

        const auto disabled =
            resolveRayTracingPolicy(RayTracingPolicy{.build = RayTracingMode::Auto, .runtime = RayTracingMode::Off},
                                    true, evaluateVulkanRayTracingSupport(completeProbe()));
        require(disabled.status == RayTracingDecisionStatus::Disabled && !disabled.enabled() &&
                    enabledVulkanRayTracingDeviceExtensions(disabled).empty(),
                "Runtime OFF must suppress RT even on a complete device.");

        const auto conflict =
            resolveRayTracingPolicy(RayTracingPolicy{.build = RayTracingMode::Off, .runtime = RayTracingMode::On}, true,
                                    evaluateVulkanRayTracingSupport(completeProbe()));
        require(conflict.rejected() && conflict.diagnostics.size() == 1 &&
                    conflict.diagnostics.front().code == RayTracingDiagnosticCode::ConflictingPolicies,
                "Build OFF plus runtime ON must be rejected as a policy conflict.");
    }

    void testBuildAvailabilityParticipatesInPolicyResolution() {
        const auto support = evaluateVulkanRayTracingSupport(completeProbe());
        const auto automatic = resolveRayTracingPolicy(RayTracingPolicy{}, false, support);
        require(automatic.status == RayTracingDecisionStatus::Fallback && !automatic.enabled(),
                "AUTO must fall back when the current build omits the RT implementation.");

        const auto required = resolveRayTracingPolicy(
            RayTracingPolicy{.build = RayTracingMode::On, .runtime = RayTracingMode::Auto}, false, support);
        require(required.rejected(), "Build ON must reject a product that omitted the RT implementation.");

        requireThrows<std::invalid_argument>(
            [&] {
                (void)resolveRayTracingPolicy(
                    RayTracingPolicy{.build = static_cast<RayTracingMode>(255), .runtime = RayTracingMode::Auto}, true,
                    support);
            },
            "Invalid policy values must be rejected before resolving device support.");
    }

    void testGeneratedBuildConfigurationMatchesCMakeCache() {
        const std::string_view expectedMode = LUMIN_EXPECTED_RAY_TRACING_MODE;
        RayTracingMode expected = RayTracingMode::Off;
        if (expectedMode == "AUTO") {
            expected = RayTracingMode::Auto;
        } else if (expectedMode == "ON") {
            expected = RayTracingMode::On;
        }
        require(configuredRayTracingBuildMode == expected,
                "The generated build mode must match the canonical CMake cache value.");
        require(rayTracingImplementationAvailable == static_cast<bool>(LUMIN_EXPECTED_RAY_TRACING_AVAILABLE),
                "The generated implementation availability must match the selected build mode.");
        require(rayTracingImplementationAvailable == (configuredRayTracingBuildMode != RayTracingMode::Off),
                "Only an OFF build may omit the ray tracing implementation.");
    }

} // namespace

int main() {
    try {
        testExtensionInspectionIsExactAndComplete();
        testCapabilityTierRequiresTheWholePipelineBundle();
        testPipelineDescriptorFeaturesAndRecursionDepthAreRequired();
        testAutoEnablesACompleteDeviceAndPropagatesExtensions();
        testAutoFallbackIsNonFatalAndDiagnostic();
        testOnRejectsAndOffSuppressesDeviceRequirements();
        testBuildAvailabilityParticipatesInPolicyResolution();
        testGeneratedBuildConfigurationMatchesCMakeCache();
        std::cout << "Vulkan ray tracing capability tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Vulkan ray tracing capability test failed: " << exception.what() << '\n';
        return 1;
    }
}
