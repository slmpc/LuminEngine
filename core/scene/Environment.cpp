#include "scene/Environment.hpp"

#include <cmath>

#include <glm/geometric.hpp>

namespace lumin::scene {
    namespace {

        [[nodiscard]] bool finiteVec3(const glm::vec3& value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        [[nodiscard]] bool nonNegativeVec3(const glm::vec3& value) noexcept {
            return finiteVec3(value) && value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f;
        }

    } // namespace

    bool validateDirectionalLight(const DirectionalLight& light) noexcept {
        if (!finiteVec3(light.direction) || !nonNegativeVec3(light.color) || !std::isfinite(light.illuminanceLux) ||
            light.illuminanceLux < 0.0f) {
            return false;
        }
        const float directionLengthSquared = glm::dot(light.direction, light.direction);
        return std::isfinite(directionLengthSquared) && directionLengthSquared > 1.0e-12f;
    }

    bool validateAtmosphereTransform(const AtmosphereTransform& transform) noexcept {
        return std::isfinite(transform.kilometersPerWorldUnit) && transform.kilometersPerWorldUnit > 0.0f &&
               std::isfinite(transform.seaLevelWorldY);
    }

    bool validateAtmosphereParameters(const AtmosphereParameters& parameters) noexcept {
        if (!std::isfinite(parameters.bottomRadiusKm) || parameters.bottomRadiusKm <= 0.0f ||
            !std::isfinite(parameters.topRadiusKm) || parameters.topRadiusKm <= parameters.bottomRadiusKm ||
            !nonNegativeVec3(parameters.rayleighScatteringPerKm) || !std::isfinite(parameters.rayleighDensityScaleKm) ||
            parameters.rayleighDensityScaleKm <= 0.0f || !nonNegativeVec3(parameters.mieScatteringPerKm) ||
            !nonNegativeVec3(parameters.mieAbsorptionPerKm) || !std::isfinite(parameters.mieDensityScaleKm) ||
            parameters.mieDensityScaleKm <= 0.0f || !std::isfinite(parameters.miePhaseG) ||
            parameters.miePhaseG <= -1.0f || parameters.miePhaseG >= 1.0f ||
            !nonNegativeVec3(parameters.ozoneAbsorptionPerKm) || !std::isfinite(parameters.ozoneLayerCenterKm) ||
            parameters.ozoneLayerCenterKm < 0.0f || !std::isfinite(parameters.ozoneLayerHalfWidthKm) ||
            parameters.ozoneLayerHalfWidthKm <= 0.0f || !nonNegativeVec3(parameters.groundAlbedo)) {
            return false;
        }

        const float atmosphereThicknessKm = parameters.topRadiusKm - parameters.bottomRadiusKm;
        return parameters.ozoneLayerCenterKm <= atmosphereThicknessKm && parameters.groundAlbedo.x <= 1.0f &&
               parameters.groundAlbedo.y <= 1.0f && parameters.groundAlbedo.z <= 1.0f;
    }

    bool validateSceneEnvironment(const SceneEnvironment& environment) noexcept {
        return validateDirectionalLight(environment.sun) && validateAtmosphereParameters(environment.atmosphere) &&
               validateAtmosphereTransform(environment.atmosphereTransform);
    }

} // namespace lumin::scene
