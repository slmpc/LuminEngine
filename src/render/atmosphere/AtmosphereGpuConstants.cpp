#include "render/atmosphere/AtmosphereGpuConstants.hpp"

#include <cmath>
#include <stdexcept>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace lumin::render::atmosphere {
    namespace {

        [[nodiscard]] bool finiteMat4(const glm::mat4& value) noexcept {
            for (glm::length_t column = 0; column < 4; ++column) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    if (!std::isfinite(value[column][row])) {
                        return false;
                    }
                }
            }
            return true;
        }

    } // namespace

    AtmosphereGpuConstants buildAtmosphereGpuConstants(const scene::SceneEnvironment& environment,
                                                       const AtmosphereViewInput& view) {
        if (!scene::validateSceneEnvironment(environment) || !validateAtmosphereViewInput(view)) {
            throw std::invalid_argument("Atmosphere GPU constants require a valid environment and view.");
        }

        const glm::mat4 inverseView = glm::inverse(view.view);
        const glm::mat4 inverseProjection = glm::inverse(view.projection);
        const glm::mat4 inverseViewProjection = glm::inverse(view.projection * view.view);
        if (!finiteMat4(inverseView) || !finiteMat4(inverseProjection) || !finiteMat4(inverseViewProjection)) {
            throw std::invalid_argument("Atmosphere view matrices must have finite inverses.");
        }

        const scene::AtmosphereParameters& atmosphere = environment.atmosphere;
        const float worldToKm = environment.atmosphereTransform.kilometersPerWorldUnit;
        const float atmosphereThicknessKm = atmosphere.topRadiusKm - atmosphere.bottomRadiusKm;
        const glm::vec3 toSunWorld = glm::normalize(-environment.sun.direction);
        const glm::vec3 cameraPlanetPositionKm = worldPositionToPlanetKm(view.cameraPositionWorld, environment);

        return AtmosphereGpuConstants{
            .view = view.view,
            .projection = view.projection,
            .inverseView = inverseView,
            .inverseProjection = inverseProjection,
            .inverseViewProjection = inverseViewProjection,
            .cameraPositionWorld = glm::vec4{view.cameraPositionWorld, 1.0f},
            .cameraPlanetPositionKm = glm::vec4{cameraPlanetPositionKm, 1.0f},
            .toSunWorld = glm::vec4{toSunWorld, 0.0f},
            .sunColorIlluminanceLux = glm::vec4{environment.sun.color, environment.sun.illuminanceLux},
            .atmosphereRadiiKm = glm::vec4{atmosphere.bottomRadiusKm, atmosphere.topRadiusKm, atmosphereThicknessKm,
                                           1.0f / atmosphereThicknessKm},
            .worldMappingAndClipKm = glm::vec4{worldToKm, environment.atmosphereTransform.seaLevelWorldY,
                                               view.nearPlaneWorld * worldToKm, view.farPlaneWorld * worldToKm},
            .renderExtent =
                glm::vec4{static_cast<float>(view.renderExtent.width), static_cast<float>(view.renderExtent.height),
                          1.0f / static_cast<float>(view.renderExtent.width),
                          1.0f / static_cast<float>(view.renderExtent.height)},
            .rayleighScatteringAndInvScaleHeight =
                glm::vec4{atmosphere.rayleighScatteringPerKm, 1.0f / atmosphere.rayleighDensityScaleKm},
            .mieScatteringAndInvScaleHeight =
                glm::vec4{atmosphere.mieScatteringPerKm, 1.0f / atmosphere.mieDensityScaleKm},
            .mieAbsorptionAndPhaseG = glm::vec4{atmosphere.mieAbsorptionPerKm, atmosphere.miePhaseG},
            .ozoneAbsorptionPerKm = glm::vec4{atmosphere.ozoneAbsorptionPerKm, 0.0f},
            .ozoneDensityProfileKm = glm::vec4{atmosphere.ozoneLayerCenterKm, atmosphere.ozoneLayerHalfWidthKm,
                                               1.0f / atmosphere.ozoneLayerHalfWidthKm, 0.0f},
            .groundAlbedoAndEnabled = glm::vec4{atmosphere.groundAlbedo, atmosphere.enabled ? 1.0f : 0.0f},
        };
    }

} // namespace lumin::render::atmosphere
