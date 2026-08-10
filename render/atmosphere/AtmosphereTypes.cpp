#include "render/atmosphere/AtmosphereTypes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace lumin::render::atmosphere {
    namespace {

        constexpr float minimumCameraAltitudeKm = 0.001f;

        class SignatureBuilder final {
        public:
            void add(bool value) noexcept {
                addByte(value ? 1U : 0U);
            }

            void add(std::uint32_t value) noexcept {
                for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
                    addByte(static_cast<std::uint8_t>(value >> shift));
                }
            }

            void add(std::uint64_t value) noexcept {
                for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
                    addByte(static_cast<std::uint8_t>(value >> shift));
                }
            }

            void add(float value) noexcept {
                // 浮点比较把正负零视为相同，签名也保持同一语义。
                const float canonicalValue = value == 0.0f ? 0.0f : value;
                add(std::bit_cast<std::uint32_t>(canonicalValue));
            }

            void add(const glm::vec3& value) noexcept {
                add(value.x);
                add(value.y);
                add(value.z);
            }

            void add(const glm::mat4& value) noexcept {
                for (glm::length_t column = 0; column < 4; ++column) {
                    for (glm::length_t row = 0; row < 4; ++row) {
                        add(value[column][row]);
                    }
                }
            }

            [[nodiscard]] std::uint64_t finish() const noexcept {
                return state_ == 0 ? 1 : state_;
            }

        private:
            void addByte(std::uint8_t value) noexcept {
                state_ ^= value;
                state_ *= 1099511628211ULL;
            }

            std::uint64_t state_ = 14695981039346656037ULL;
        };

        [[nodiscard]] bool finiteVec3(const glm::vec3& value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

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

        [[nodiscard]] bool invertibleMat4(const glm::mat4& value) noexcept {
            const float determinant = glm::determinant(value);
            return std::isfinite(determinant) && std::abs(determinant) > 1.0e-12f;
        }

    } // namespace

    AtmosphereViewInput makeAtmosphereViewInput(const scene::Camera& camera, core::RenderExtent renderExtent) {
        if (renderExtent.isEmpty()) {
            throw std::invalid_argument("Atmosphere view render extent must not be empty.");
        }

        const float aspectRatio = static_cast<float>(renderExtent.width) / static_cast<float>(renderExtent.height);
        AtmosphereViewInput result{
            .cameraPositionWorld = camera.position(),
            .view = camera.viewMatrix(),
            .projection = camera.projectionMatrix(aspectRatio),
            .nearPlaneWorld = camera.nearPlane(),
            .farPlaneWorld = camera.farPlane(),
            .renderExtent = renderExtent,
        };
        if (!validateAtmosphereViewInput(result)) {
            throw std::invalid_argument("Camera produced an invalid atmosphere view.");
        }
        return result;
    }

    bool validateAtmosphereViewInput(const AtmosphereViewInput& view) noexcept {
        return finiteVec3(view.cameraPositionWorld) && finiteMat4(view.view) && invertibleMat4(view.view) &&
               finiteMat4(view.projection) && invertibleMat4(view.projection) && std::isfinite(view.nearPlaneWorld) &&
               view.nearPlaneWorld > 0.0f && std::isfinite(view.farPlaneWorld) &&
               view.farPlaneWorld > view.nearPlaneWorld && !view.renderExtent.isEmpty();
    }

    glm::vec3 worldPositionToPlanetKm(const glm::vec3& worldPosition, const scene::SceneEnvironment& environment) {
        if (!finiteVec3(worldPosition) || !scene::validateAtmosphereParameters(environment.atmosphere) ||
            !scene::validateAtmosphereTransform(environment.atmosphereTransform)) {
            throw std::invalid_argument("Atmosphere world-to-planet mapping requires valid finite inputs.");
        }

        const float scale = environment.atmosphereTransform.kilometersPerWorldUnit;
        const float altitudeKm = std::max((worldPosition.y - environment.atmosphereTransform.seaLevelWorldY) * scale,
                                          minimumCameraAltitudeKm);
        const glm::vec3 result{worldPosition.x * scale, environment.atmosphere.bottomRadiusKm + altitudeKm,
                               worldPosition.z * scale};
        if (!finiteVec3(result)) {
            throw std::invalid_argument("Atmosphere world-to-planet mapping overflowed.");
        }
        return result;
    }

    AtmosphereLutSignatures makeAtmosphereLutSignatures(const scene::SceneEnvironment& environment,
                                                        const AtmosphereViewInput& view) {
        if (!scene::validateSceneEnvironment(environment) || !validateAtmosphereViewInput(view)) {
            throw std::invalid_argument("Atmosphere LUT signatures require a valid environment and view.");
        }

        const scene::AtmosphereParameters& atmosphere = environment.atmosphere;
        SignatureBuilder optical;
        optical.add(0x4f50544943414cULL);
        optical.add(atmosphere.enabled);
        optical.add(atmosphere.bottomRadiusKm);
        optical.add(atmosphere.topRadiusKm);
        optical.add(atmosphere.rayleighScatteringPerKm);
        optical.add(atmosphere.rayleighDensityScaleKm);
        optical.add(atmosphere.mieScatteringPerKm);
        optical.add(atmosphere.mieAbsorptionPerKm);
        optical.add(atmosphere.mieDensityScaleKm);
        optical.add(atmosphere.miePhaseG);
        optical.add(atmosphere.ozoneAbsorptionPerKm);
        optical.add(atmosphere.ozoneLayerCenterKm);
        optical.add(atmosphere.ozoneLayerHalfWidthKm);

        SignatureBuilder surface;
        surface.add(0x53555246414345ULL);
        surface.add(atmosphere.groundAlbedo);

        const glm::vec3 toSunWorld = glm::normalize(-environment.sun.direction);
        SignatureBuilder lighting;
        lighting.add(0x4c49474854494e47ULL);
        lighting.add(toSunWorld);
        lighting.add(environment.sun.color);
        lighting.add(environment.sun.illuminanceLux);

        const glm::vec3 cameraPlanetPositionKm = worldPositionToPlanetKm(view.cameraPositionWorld, environment);
        SignatureBuilder skyView;
        skyView.add(0x534b5956494557ULL);
        skyView.add(cameraPlanetPositionKm.y);

        SignatureBuilder aerialPerspective;
        aerialPerspective.add(0x41455249414cULL);
        aerialPerspective.add(cameraPlanetPositionKm);
        aerialPerspective.add(view.view);
        aerialPerspective.add(view.projection);
        aerialPerspective.add(view.nearPlaneWorld * environment.atmosphereTransform.kilometersPerWorldUnit);
        aerialPerspective.add(view.farPlaneWorld * environment.atmosphereTransform.kilometersPerWorldUnit);
        aerialPerspective.add(view.renderExtent.width);
        aerialPerspective.add(view.renderExtent.height);

        return AtmosphereLutSignatures{
            .optical = OpticalSignature{optical.finish()},
            .surface = SurfaceSignature{surface.finish()},
            .lighting = LightingSignature{lighting.finish()},
            .skyView = ViewSignature{skyView.finish()},
            .aerialPerspective = ViewSignature{aerialPerspective.finish()},
        };
    }

} // namespace lumin::render::atmosphere
