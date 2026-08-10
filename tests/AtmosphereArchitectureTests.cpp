#include "render/atmosphere/AtmosphereGpuConstants.hpp"
#include "render/atmosphere/AtmosphereLutScheduler.hpp"
#include "render/atmosphere/AtmosphereTypes.hpp"
#include "scene/Camera.hpp"
#include "scene/Environment.hpp"

#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/geometric.hpp>

namespace {

    using lumin::render::atmosphere::AtmosphereLut;
    using lumin::render::atmosphere::AtmosphereLutFrameInput;
    using lumin::render::atmosphere::AtmosphereLutScheduler;
    using lumin::render::atmosphere::AtmosphereLutSignatures;
    using lumin::render::atmosphere::LightingSignature;
    using lumin::render::atmosphere::OpticalSignature;
    using lumin::render::atmosphere::SurfaceSignature;
    using lumin::render::atmosphere::ViewSignature;
    using lumin::render::core::RenderExtent;
    using lumin::render::core::RenderSequence;
    using lumin::scene::SceneEnvironment;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    void requireNear(float actual, float expected, float epsilon, const std::string& message) {
        if (std::abs(actual - expected) > epsilon) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function>
    void requireThrows(Function&& function, const std::string& message) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    AtmosphereLutSignatures signatures(std::uint64_t optical = 1, std::uint64_t surface = 2, std::uint64_t lighting = 3,
                                       std::uint64_t skyView = 4, std::uint64_t aerialPerspective = 5) {
        return AtmosphereLutSignatures{
            .optical = OpticalSignature{optical},
            .surface = SurfaceSignature{surface},
            .lighting = LightingSignature{lighting},
            .skyView = ViewSignature{skyView},
            .aerialPerspective = ViewSignature{aerialPerspective},
        };
    }

    void requireRebuildSet(const lumin::render::atmosphere::AtmosphereLutPlan& plan, bool transmittance,
                           bool multiScattering, bool skyView, bool aerialPerspective, const std::string& message) {
        require(plan.rebuilds(AtmosphereLut::Transmittance) == transmittance &&
                    plan.rebuilds(AtmosphereLut::MultiScattering) == multiScattering &&
                    plan.rebuilds(AtmosphereLut::SkyView) == skyView &&
                    plan.rebuilds(AtmosphereLut::AerialPerspective) == aerialPerspective,
                message);
    }

    void testEnvironmentValidation() {
        const SceneEnvironment defaults;
        require(lumin::scene::validateDirectionalLight(defaults.sun) &&
                    lumin::scene::validateAtmosphereTransform(defaults.atmosphereTransform) &&
                    lumin::scene::validateAtmosphereParameters(defaults.atmosphere) &&
                    lumin::scene::validateSceneEnvironment(defaults),
                "The default scene environment must be physically and numerically valid.");

        SceneEnvironment invalid = defaults;
        invalid.atmosphereTransform.kilometersPerWorldUnit = 0.0f;
        require(!lumin::scene::validateSceneEnvironment(invalid),
                "Atmosphere world scale must be finite and positive.");
        invalid = defaults;
        invalid.atmosphereTransform.seaLevelWorldY = std::numeric_limits<float>::infinity();
        require(!lumin::scene::validateSceneEnvironment(invalid), "Atmosphere sea level must be finite.");

        invalid = defaults;
        invalid.atmosphere.topRadiusKm = invalid.atmosphere.bottomRadiusKm;
        require(!lumin::scene::validateSceneEnvironment(invalid),
                "Atmosphere top radius must exceed the bottom radius.");
        invalid = defaults;
        invalid.atmosphere.rayleighScatteringPerKm.x = -0.001f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Scattering coefficients cannot be negative.");
        invalid = defaults;
        invalid.atmosphere.mieAbsorptionPerKm.y = std::numeric_limits<float>::quiet_NaN();
        require(!lumin::scene::validateSceneEnvironment(invalid), "Atmosphere coefficients must be finite.");
        invalid = defaults;
        invalid.atmosphere.rayleighDensityScaleKm = 0.0f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Density scale heights must be positive.");
        invalid = defaults;
        invalid.atmosphere.miePhaseG = 1.0f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Mie phase g must remain strictly below one.");
        invalid = defaults;
        invalid.atmosphere.ozoneLayerHalfWidthKm = 0.0f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Ozone layer half-width must be positive.");
        invalid = defaults;
        invalid.atmosphere.ozoneLayerCenterKm = 101.0f;
        require(!lumin::scene::validateSceneEnvironment(invalid),
                "Ozone layer center must lie within the atmosphere shell.");
        invalid = defaults;
        invalid.atmosphere.groundAlbedo.z = 1.01f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Ground albedo must remain in [0, 1].");

        invalid = defaults;
        invalid.sun.direction = glm::vec3{0.0f};
        require(!lumin::scene::validateSceneEnvironment(invalid), "The sun propagation direction cannot be zero.");
        invalid = defaults;
        invalid.sun.color.x = -0.1f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Sun color cannot be negative.");
        invalid = defaults;
        invalid.sun.illuminanceLux = -1.0f;
        require(!lumin::scene::validateSceneEnvironment(invalid), "Sun illuminance cannot be negative.");
    }

    void testGpuConstantsUseToSunDirectionAndKilometers() {
        SceneEnvironment environment;
        environment.sun.direction = glm::vec3{0.0f, -2.0f, 0.0f};
        environment.sun.color = glm::vec3{0.8f, 0.7f, 0.6f};
        environment.atmosphereTransform.kilometersPerWorldUnit = 0.01f;
        environment.atmosphereTransform.seaLevelWorldY = 100.0f;

        lumin::scene::Camera camera;
        camera.setPosition(glm::vec3{2.0f, 110.0f, -3.0f});
        const auto view = lumin::render::atmosphere::makeAtmosphereViewInput(camera, RenderExtent{800, 400});
        const auto constants = lumin::render::atmosphere::buildAtmosphereGpuConstants(environment, view);

        requireNear(constants.toSunWorld.x, 0.0f, 1.0e-6f, "The to-sun X component is incorrect.");
        requireNear(constants.toSunWorld.y, 1.0f, 1.0e-6f,
                    "DirectionalLight direction must be negated before atmosphere shading.");
        requireNear(constants.toSunWorld.z, 0.0f, 1.0e-6f, "The to-sun Z component is incorrect.");
        requireNear(glm::length(glm::vec3{constants.toSunWorld}), 1.0f, 1.0e-6f,
                    "The to-sun vector must be normalized.");

        requireNear(constants.cameraPlanetPositionKm.x, 0.02f, 1.0e-6f,
                    "World X must use the explicit kilometers-per-unit scale.");
        requireNear(constants.cameraPlanetPositionKm.y, environment.atmosphere.bottomRadiusKm + 0.1f, 1.0e-4f,
                    "Camera altitude must be measured from the configured sea level.");
        requireNear(constants.cameraPlanetPositionKm.z, -0.03f, 1.0e-6f,
                    "World Z must use the explicit kilometers-per-unit scale.");
        requireNear(constants.worldMappingAndClipKm.z, camera.nearPlane() * 0.01f, 1.0e-7f,
                    "Near plane must be converted to kilometers.");
        requireNear(constants.worldMappingAndClipKm.w, camera.farPlane() * 0.01f, 1.0e-6f,
                    "Far plane must be converted to kilometers.");
        requireNear(constants.rayleighScatteringAndInvScaleHeight.w,
                    1.0f / environment.atmosphere.rayleighDensityScaleKm, 1.0e-6f,
                    "Rayleigh inverse scale height is incorrect.");
    }

    void testSignatureDomainsRemainIndependent() {
        SceneEnvironment environment;
        lumin::scene::Camera camera;
        const RenderExtent extent{1280, 720};
        const auto baseView = lumin::render::atmosphere::makeAtmosphereViewInput(camera, extent);
        const AtmosphereLutSignatures base =
            lumin::render::atmosphere::makeAtmosphereLutSignatures(environment, baseView);
        require(base.isValid(), "Generated atmosphere signatures must be initialized.");

        SceneEnvironment changed = environment;
        changed.atmosphere.rayleighScatteringPerKm.x *= 1.1f;
        const auto optical = lumin::render::atmosphere::makeAtmosphereLutSignatures(changed, baseView);
        require(optical.optical != base.optical && optical.surface == base.surface &&
                    optical.lighting == base.lighting && optical.skyView == base.skyView &&
                    optical.aerialPerspective == base.aerialPerspective,
                "Optical medium changes must stay in the optical signature domain.");

        changed = environment;
        changed.atmosphere.enabled = false;
        const auto disabled = lumin::render::atmosphere::makeAtmosphereLutSignatures(changed, baseView);
        require(disabled.optical != base.optical && disabled.surface == base.surface &&
                    disabled.lighting == base.lighting && disabled.skyView == base.skyView &&
                    disabled.aerialPerspective == base.aerialPerspective,
                "Enabling or disabling atmosphere must invalidate the complete optical dependency chain.");

        changed = environment;
        changed.atmosphere.groundAlbedo.x = 0.4f;
        const auto surface = lumin::render::atmosphere::makeAtmosphereLutSignatures(changed, baseView);
        require(surface.optical == base.optical && surface.surface != base.surface && surface.lighting == base.lighting,
                "Ground albedo must only modify the surface signature domain.");

        changed = environment;
        changed.sun.direction *= 4.0f;
        const auto equivalentLighting = lumin::render::atmosphere::makeAtmosphereLutSignatures(changed, baseView);
        require(equivalentLighting.lighting == base.lighting,
                "Changing only the magnitude of a directional light must not invalidate atmosphere lighting.");
        changed.sun.direction = glm::vec3{-0.2f, -0.9f, 0.3f};
        const auto lighting = lumin::render::atmosphere::makeAtmosphereLutSignatures(changed, baseView);
        require(lighting.optical == base.optical && lighting.surface == base.surface &&
                    lighting.lighting != base.lighting,
                "Sun direction changes must stay in the lighting signature domain.");

        camera.setOrientation(-45.0f, 15.0f);
        const auto rotatedView = lumin::render::atmosphere::makeAtmosphereViewInput(camera, extent);
        const auto rotated = lumin::render::atmosphere::makeAtmosphereLutSignatures(environment, rotatedView);
        require(rotated.skyView == base.skyView && rotated.aerialPerspective != base.aerialPerspective,
                "Camera rotation must rebuild aerial perspective without rebuilding the altitude-only sky-view LUT.");

        camera.setPosition(camera.position() + glm::vec3{0.0f, 10.0f, 0.0f});
        const auto elevatedView = lumin::render::atmosphere::makeAtmosphereViewInput(camera, extent);
        const auto elevated = lumin::render::atmosphere::makeAtmosphereLutSignatures(environment, elevatedView);
        require(elevated.skyView != rotated.skyView && elevated.aerialPerspective != rotated.aerialPerspective,
                "Camera altitude must invalidate both view-dependent signatures.");
    }

    void testSchedulerBuildsMinimalDependencyClosure() {
        AtmosphereLutScheduler scheduler;
        const AtmosphereLutSignatures base = signatures();
        const auto first = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{0}, base});
        require(first.isValid(), "The first atmosphere LUT plan must be valid.");
        requireRebuildSet(first, true, true, true, true, "The first submitted frame must initialize every LUT.");
        scheduler.commitSubmittedFrame(RenderSequence{0});

        const auto stable = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{1}, base});
        requireRebuildSet(stable, false, false, false, false, "Stable inputs must preserve all atmosphere LUTs.");
        scheduler.commitSubmittedFrame(RenderSequence{1});

        AtmosphereLutSignatures changed = base;
        changed.lighting = LightingSignature{30};
        const auto lighting = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{2}, changed});
        requireRebuildSet(lighting, false, false, true, true,
                          "Lighting changes must rebuild only sky-view and aerial perspective.");
        scheduler.commitSubmittedFrame(RenderSequence{2});

        changed.aerialPerspective = ViewSignature{50};
        const auto aerialView = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{3}, changed});
        requireRebuildSet(aerialView, false, false, false, true,
                          "A full-view-only change must rebuild only aerial perspective.");
        scheduler.commitSubmittedFrame(RenderSequence{3});

        changed.skyView = ViewSignature{40};
        const auto skyView = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{4}, changed});
        requireRebuildSet(skyView, false, false, true, false,
                          "An altitude-only signature change must rebuild only sky-view.");
        scheduler.commitSubmittedFrame(RenderSequence{4});

        changed.surface = SurfaceSignature{20};
        const auto surface = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{5}, changed});
        requireRebuildSet(surface, false, true, true, true,
                          "Surface changes must propagate from multi-scattering to both downstream LUTs.");
        scheduler.commitSubmittedFrame(RenderSequence{5});

        changed.optical = OpticalSignature{10};
        const auto optical = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{6}, changed});
        requireRebuildSet(optical, true, true, true, true,
                          "Optical changes must propagate through the complete LUT dependency chain.");
        scheduler.commitSubmittedFrame(RenderSequence{6});

        require(scheduler.state(AtmosphereLut::Transmittance).generation == 2 &&
                    scheduler.state(AtmosphereLut::MultiScattering).generation == 3 &&
                    scheduler.state(AtmosphereLut::SkyView).generation == 5 &&
                    scheduler.state(AtmosphereLut::AerialPerspective).generation == 5,
                "Only rebuilt LUTs may advance their committed generation.");
    }

    void testSchedulerAdvancesOnlyAfterSuccessfulSubmit() {
        AtmosphereLutScheduler scheduler;
        const AtmosphereLutSignatures base = signatures();
        const auto first = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{10}, base});
        require(first.generationAfterSubmit(AtmosphereLut::Transmittance) == 1,
                "The first successful transmittance build must target generation one.");

        requireThrows<std::logic_error>(
            [&] {
                (void)scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{11}, base});
            },
            "A nested atmosphere LUT frame must be rejected.");
        requireThrows<std::logic_error>(
            [&] {
                scheduler.commitSubmittedFrame(RenderSequence{11});
            },
            "A mismatched sequence must not commit the active atmosphere transaction.");
        require(scheduler.hasActiveFrame(), "A mismatched completion must leave the correct transaction active.");
        scheduler.abandonFrame(RenderSequence{10});

        for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(AtmosphereLut::Count); ++index) {
            const auto& state = scheduler.state(static_cast<AtmosphereLut>(index));
            require(!state.valid && state.generation == 0 && !state.lastRebuildSequence.isValid(),
                    "Abandoning a frame must not advance any atmosphere LUT state.");
        }
        require(!scheduler.lastSuccessfulSequence().isValid() && scheduler.committedSignatures() == nullptr,
                "Abandoning the first frame must not publish signatures or a successful sequence.");

        const auto retry = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{10}, base});
        requireRebuildSet(retry, true, true, true, true,
                          "Retrying an abandoned first frame must rebuild every atmosphere LUT.");
        scheduler.commitSubmittedFrame(RenderSequence{10});
        require(scheduler.committedSignatures() != nullptr && *scheduler.committedSignatures() == base,
                "A successful submission must atomically publish its input signatures.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{10}, base});
            },
            "A successfully submitted render sequence must not be reused.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{11}, AtmosphereLutSignatures{}});
            },
            "The scheduler must reject uninitialized signatures.");
    }

    void testSchedulerForceRebuildRestoresReenabledAtmosphere() {
        AtmosphereLutScheduler scheduler;
        const AtmosphereLutSignatures base = signatures();
        scheduler.commitSubmittedFrame(
            scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{20}, base}).sequence());

        const auto reenabled = scheduler.beginFrame(AtmosphereLutFrameInput{
            .sequence = RenderSequence{22},
            .signatures = base,
            .forceRebuild = true,
        });
        requireRebuildSet(reenabled, true, true, true, true,
                          "Re-enabling atmosphere must rebuild every LUT even when signatures are unchanged.");
        for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(AtmosphereLut::Count); ++index) {
            const AtmosphereLut lut = static_cast<AtmosphereLut>(index);
            require(reenabled.generationAfterSubmit(lut) == 2,
                    "A forced rebuild must target the next generation for every LUT.");
        }

        scheduler.abandonFrame(RenderSequence{22});
        for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(AtmosphereLut::Count); ++index) {
            const auto& state = scheduler.state(static_cast<AtmosphereLut>(index));
            require(state.valid && state.generation == 1 && state.lastRebuildSequence == RenderSequence{20},
                    "Abandoning a forced rebuild must preserve the last submitted LUT generation.");
        }

        const auto retry = scheduler.beginFrame(AtmosphereLutFrameInput{
            .sequence = RenderSequence{22},
            .signatures = base,
            .forceRebuild = true,
        });
        requireRebuildSet(retry, true, true, true, true,
                          "A failed re-enable attempt must be retried as a full rebuild.");
        scheduler.commitSubmittedFrame(RenderSequence{22});

        const auto stable = scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{23}, base});
        requireRebuildSet(stable, false, false, false, false,
                          "A successful forced rebuild must restore normal signature-based reuse.");
        scheduler.abandonFrame(RenderSequence{23});
    }

} // namespace

int main() {
    try {
        testEnvironmentValidation();
        testGpuConstantsUseToSunDirectionAndKilometers();
        testSignatureDomainsRemainIndependent();
        testSchedulerBuildsMinimalDependencyClosure();
        testSchedulerAdvancesOnlyAfterSuccessfulSubmit();
        testSchedulerForceRebuildRestoresReenabledAtmosphere();
        std::cout << "Atmosphere architecture tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Atmosphere architecture test failed: " << exception.what() << '\n';
        return 1;
    }
}
