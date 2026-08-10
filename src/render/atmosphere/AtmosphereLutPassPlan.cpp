#include "render/atmosphere/AtmosphereLutPassPlan.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace lumin::render::atmosphere {
    namespace {

        [[nodiscard]] std::string_view passName(AtmosphereLut lut) {
            switch (lut) {
            case AtmosphereLut::Transmittance:
                return "Atmosphere.Transmittance";
            case AtmosphereLut::MultiScattering:
                return "Atmosphere.MultiScattering";
            case AtmosphereLut::SkyView:
                return "Atmosphere.SkyView";
            case AtmosphereLut::AerialPerspective:
                return "Atmosphere.AerialPerspective";
            case AtmosphereLut::Count:
                break;
            }
            return "Atmosphere.Invalid";
        }

        [[nodiscard]] std::vector<AtmosphereLutPassResourceUse> resourceUsesFor(AtmosphereLut target) {
            using Access = AtmosphereLutPassAccess;
            switch (target) {
            case AtmosphereLut::Transmittance:
                return {{AtmosphereLut::Transmittance, Access::StorageWrite}};
            case AtmosphereLut::MultiScattering:
                return {
                    {AtmosphereLut::Transmittance, Access::SampledRead},
                    {AtmosphereLut::MultiScattering, Access::StorageWrite},
                };
            case AtmosphereLut::SkyView:
                return {
                    {AtmosphereLut::Transmittance, Access::SampledRead},
                    {AtmosphereLut::MultiScattering, Access::SampledRead},
                    {AtmosphereLut::SkyView, Access::StorageWrite},
                };
            case AtmosphereLut::AerialPerspective:
                return {
                    {AtmosphereLut::Transmittance, Access::SampledRead},
                    {AtmosphereLut::MultiScattering, Access::SampledRead},
                    {AtmosphereLut::AerialPerspective, Access::StorageWrite},
                };
            case AtmosphereLut::Count:
                break;
            }
            throw std::invalid_argument("Atmosphere LUT pass target is invalid.");
        }

        [[nodiscard]] std::vector<AtmosphereLut> dependenciesFor(const AtmosphereLutPlan& plan, AtmosphereLut target) {
            switch (target) {
            case AtmosphereLut::Transmittance:
                return {};
            case AtmosphereLut::MultiScattering:
                return plan.rebuilds(AtmosphereLut::Transmittance)
                           ? std::vector<AtmosphereLut>{AtmosphereLut::Transmittance}
                           : std::vector<AtmosphereLut>{};
            case AtmosphereLut::SkyView:
            case AtmosphereLut::AerialPerspective:
                return plan.rebuilds(AtmosphereLut::MultiScattering)
                           ? std::vector<AtmosphereLut>{AtmosphereLut::MultiScattering}
                           : std::vector<AtmosphereLut>{};
            case AtmosphereLut::Count:
                break;
            }
            throw std::invalid_argument("Atmosphere LUT pass target is invalid.");
        }

    } // namespace

    AtmosphereLutComputePass::AtmosphereLutComputePass(AtmosphereLut target,
                                                       std::vector<AtmosphereLutPassResourceUse> resourceUses,
                                                       std::vector<AtmosphereLut> dependencies)
        : target_(target), resourceUses_(std::move(resourceUses)), dependencies_(std::move(dependencies)) {
    }

    AtmosphereLut AtmosphereLutComputePass::target() const noexcept {
        return target_;
    }

    std::string_view AtmosphereLutComputePass::name() const noexcept {
        return passName(target_);
    }

    std::span<const AtmosphereLutPassResourceUse> AtmosphereLutComputePass::resourceUses() const noexcept {
        return resourceUses_;
    }

    std::span<const AtmosphereLut> AtmosphereLutComputePass::dependencies() const noexcept {
        return dependencies_;
    }

    core::RenderSequence AtmosphereLutPassPlan::sequence() const noexcept {
        return sequence_;
    }

    std::span<const AtmosphereLutComputePass> AtmosphereLutPassPlan::passes() const noexcept {
        return passes_;
    }

    bool AtmosphereLutPassPlan::empty() const noexcept {
        return passes_.empty();
    }

    bool AtmosphereLutPassPlan::isValid() const noexcept {
        return sequence_.isValid();
    }

    AtmosphereLutPassPlan makeAtmosphereLutPassPlan(const AtmosphereLutPlan& lutPlan) {
        if (!lutPlan.isValid()) {
            throw std::invalid_argument("Atmosphere LUT scheduler plan is invalid.");
        }

        AtmosphereLutPassPlan result;
        result.sequence_ = lutPlan.sequence();
        result.passes_.reserve(atmosphereLutResourceCount);
        for (std::size_t index = 0; index < atmosphereLutResourceCount; ++index) {
            const AtmosphereLut target = static_cast<AtmosphereLut>(index);
            if (!lutPlan.rebuilds(target)) {
                continue;
            }
            result.passes_.emplace_back(target, resourceUsesFor(target), dependenciesFor(lutPlan, target));
        }
        return result;
    }

    FrameGraphResourceHandle AtmosphereLutFrameGraphResources::texture(AtmosphereLut lut) const {
        return textures[atmosphereLutResourceIndex(lut)];
    }

    bool AtmosphereLutFrameGraphResources::isValid() const noexcept {
        return constants.isValid() &&
               std::all_of(textures.begin(), textures.end(), [](FrameGraphResourceHandle handle) {
                   return handle.isValid();
               });
    }

    FrameGraphPassHandle AtmosphereLutFrameGraphPasses::pass(AtmosphereLut lut) const {
        return handles[atmosphereLutResourceIndex(lut)];
    }

    std::size_t AtmosphereLutFrameGraphPasses::size() const noexcept {
        return static_cast<std::size_t>(std::count_if(handles.begin(), handles.end(), [](FrameGraphPassHandle handle) {
            return handle.isValid();
        }));
    }

    AtmosphereLutFrameGraphPasses registerAtmosphereLutPasses(FrameGraph& frameGraph, const AtmosphereLutPassPlan& plan,
                                                              const AtmosphereLutFrameGraphResources& resources,
                                                              AtmosphereLutExecuteCallbacks callbacks) {
        if (!plan.isValid()) {
            throw std::invalid_argument("Atmosphere LUT pass plan is invalid.");
        }
        if (plan.empty()) {
            return {};
        }
        if (!resources.isValid()) {
            throw std::invalid_argument("Atmosphere LUT frame graph resources are invalid.");
        }
        for (const AtmosphereLutComputePass& pass : plan.passes()) {
            if (!callbacks.callbacks[atmosphereLutResourceIndex(pass.target())]) {
                throw std::invalid_argument("Atmosphere LUT compute pass execute callback is missing.");
            }
        }

        AtmosphereLutFrameGraphPasses registered;
        for (const AtmosphereLutComputePass& pass : plan.passes()) {
            std::vector<std::pair<FrameGraphResourceHandle, AtmosphereLutPassAccess>> resourceUses;
            resourceUses.reserve(pass.resourceUses().size());
            for (const AtmosphereLutPassResourceUse& use : pass.resourceUses()) {
                resourceUses.emplace_back(resources.texture(use.lut), use.access);
            }

            std::vector<FrameGraphPassHandle> dependencies;
            dependencies.reserve(pass.dependencies().size());
            for (const AtmosphereLut dependency : pass.dependencies()) {
                const FrameGraphPassHandle handle = registered.pass(dependency);
                if (!handle.isValid()) {
                    throw std::logic_error("Atmosphere LUT pass dependency was not registered earlier.");
                }
                dependencies.push_back(handle);
            }

            const AtmosphereLut target = pass.target();
            FrameGraph::ExecuteCallback execute = std::move(callbacks.callbacks[atmosphereLutResourceIndex(target)]);
            const FrameGraphPassHandle handle = frameGraph.addPass(
                std::string{pass.name()}, FrameGraphPassType::Compute,
                [resourceUses = std::move(resourceUses), dependencies = std::move(dependencies),
                 constants = resources.constants](FrameGraphBuilder& builder) {
                    builder.read(constants, nvrhi::ResourceStates::ConstantBuffer);
                    for (const auto& [resource, access] : resourceUses) {
                        if (access == AtmosphereLutPassAccess::SampledRead) {
                            builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                        } else {
                            builder.writeTexture(resource, nvrhi::ResourceStates::UnorderedAccess);
                        }
                    }
                    for (const FrameGraphPassHandle dependency : dependencies) {
                        builder.dependsOn(dependency);
                    }
                },
                std::move(execute));
            registered.handles[atmosphereLutResourceIndex(target)] = handle;
        }
        return registered;
    }

} // namespace lumin::render::atmosphere
