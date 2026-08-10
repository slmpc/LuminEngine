#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "assets/ObjLoader.hpp"
#include "scene/Environment.hpp"
#include "scene/Material.hpp"

namespace lumin::scene {

    class Level;

    struct MeshHandle {
        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;

        [[nodiscard]] bool isValid() const noexcept;
        friend bool operator==(MeshHandle, MeshHandle) = default;
    };

    struct ModelHandle {
        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;

        [[nodiscard]] bool isValid() const noexcept {
            return index != std::numeric_limits<std::uint32_t>::max() && generation != 0;
        }

        explicit operator bool() const noexcept {
            return isValid();
        }

        friend bool operator==(ModelHandle, ModelHandle) = default;
    };

    inline constexpr ModelHandle InvalidModelHandle{};

    struct ActorHandle {
        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;

        [[nodiscard]] bool isValid() const noexcept {
            return index != std::numeric_limits<std::uint32_t>::max() && generation != 0;
        }

        explicit operator bool() const noexcept {
            return isValid();
        }

        friend bool operator==(ActorHandle, ActorHandle) = default;
    };

    using ActorId = ActorHandle;

    struct Transform {
        glm::vec3 position{0.0f};
        glm::vec3 rotationDegrees{0.0f};
        glm::vec3 scale{1.0f};

        [[nodiscard]] glm::mat4 matrix() const;
    };

    struct ModelInstance {
        MeshHandle mesh;
        Transform transform;
        Material material;
    };

    /**
     * Base class for objects owned by a Level.
     *
     * Actor callbacks are invoked by Level. Spawns and destroys requested from
     * inside any callback are deferred until the active callback has returned.
     */
    class Actor {
    public:
        virtual ~Actor();

        virtual void onSpawn(Level& level);
        virtual void onDestroy(Level& level);
        virtual void tick(Level& level, float deltaSeconds);
        // The overloads make small actors convenient to write while retaining
        // access to their owning Level through level().
        virtual void tick(float deltaSeconds);
        virtual void onTick(Level& level, float deltaSeconds);
        virtual void onTick(float deltaSeconds);

        [[nodiscard]] Level* level() noexcept;
        [[nodiscard]] const Level* level() const noexcept;
        [[nodiscard]] ActorHandle handle() const noexcept;
        [[nodiscard]] bool isSpawned() const noexcept;
        [[nodiscard]] bool isPendingDestroy() const noexcept;

        [[nodiscard]] const Transform& transform() const noexcept;
        void setTransform(Transform transform);
        void translate(const glm::vec3& offset);

        [[nodiscard]] const Material& material() const noexcept;
        void setMaterial(Material material);

        [[nodiscard]] ModelHandle modelHandle() const noexcept;
        ModelHandle attachModel(MeshHandle mesh, Material material = {});
        void detachModel();
        void destroy();

    protected:
        Actor() = default;
        Actor(const Actor&) = delete;
        Actor& operator=(const Actor&) = delete;
        Actor(Actor&&) = delete;
        Actor& operator=(Actor&&) = delete;

    private:
        friend class Level;

        Level* owner_ = nullptr;
        ActorHandle handle_{};
        Transform transform_{};
        Material material_{};
        ModelHandle modelHandle_ = InvalidModelHandle;
        std::optional<MeshHandle> requestedMesh_;
    };

    class Level {
    public:
        Level() = default;
        ~Level();

        Level(const Level&) = delete;
        Level& operator=(const Level&) = delete;
        Level(Level&&) = delete;
        Level& operator=(Level&&) = delete;

        [[nodiscard]] MeshHandle addMesh(assets::Mesh mesh);
        bool removeMesh(MeshHandle mesh) noexcept;
        [[nodiscard]] bool isMeshAlive(MeshHandle mesh) const noexcept;
        ModelHandle addModel(MeshHandle mesh, Transform transform = {}, Material material = {});
        bool removeModel(ModelHandle model) noexcept;
        bool setModelMesh(ModelHandle model, MeshHandle mesh);
        bool setModelTransform(ModelHandle model, Transform transform);
        bool setModelMaterial(ModelHandle model, Material material);
        bool replaceMesh(MeshHandle mesh, assets::Mesh replacement);

        [[nodiscard]] const ModelInstance& model(ModelHandle handle) const;

        [[nodiscard]] const assets::Mesh& mesh(MeshHandle handle) const;
        [[nodiscard]] const std::vector<assets::Mesh>& meshes() const noexcept;
        [[nodiscard]] const std::vector<ModelInstance>& models() const noexcept;
        [[nodiscard]] std::vector<ModelHandle> modelHandles() const;

        ActorHandle spawnActor(std::unique_ptr<Actor> actor);

        template <typename T, typename... Args>
            requires std::is_base_of_v<Actor, T>
        ActorHandle spawnActor(Args&&... args) {
            return spawnActor(std::make_unique<T>(std::forward<Args>(args)...));
        }

        template <typename T, typename... Args>
            requires std::is_base_of_v<Actor, T>
        ActorHandle spawn(Args&&... args) {
            return spawnActor<T>(std::forward<Args>(args)...);
        }

        bool destroyActor(ActorHandle handle);
        bool destroyActor(Actor& actor);
        bool destroy(ActorHandle handle);
        bool removeActor(ActorHandle handle);
        bool isActorAlive(ActorHandle handle) const noexcept;
        [[nodiscard]] std::vector<ActorHandle> actorHandles() const;
        [[nodiscard]] Actor* actor(ActorHandle handle) noexcept;
        [[nodiscard]] const Actor* actor(ActorHandle handle) const noexcept;
        [[nodiscard]] Actor* getActor(ActorHandle handle) noexcept;
        [[nodiscard]] const Actor* getActor(ActorHandle handle) const noexcept;
        [[nodiscard]] std::size_t actorCount() const noexcept;
        [[nodiscard]] std::size_t pendingActorCount() const noexcept;
        void tick(float deltaSeconds);

        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::uint64_t dataRevision() const noexcept;
        [[nodiscard]] std::uint64_t topologyRevision() const noexcept;
        [[nodiscard]] std::uint64_t modelRevision() const noexcept;
        [[nodiscard]] std::uint64_t modelsRevision() const noexcept;
        [[nodiscard]] std::uint64_t meshRevision() const noexcept;

        /** 返回只读场景环境。修改必须通过 setter，以便推进准确的 revision。 */
        [[nodiscard]] const SceneEnvironment& environment() const noexcept;
        void setEnvironment(SceneEnvironment environment) noexcept;
        void setSun(DirectionalLight sun) noexcept;
        void setAtmosphere(AtmosphereParameters atmosphere) noexcept;
        [[nodiscard]] std::uint64_t lightingRevision() const noexcept;
        [[nodiscard]] std::uint64_t atmosphereRevision() const noexcept;

    private:
        enum class ActorSlotState : std::uint8_t {
            Empty,
            Active,
            PendingSpawn,
            PendingDestroy,
            PendingSpawnDestroy,
        };

        struct ActorSlot {
            std::unique_ptr<Actor> actor;
            std::uint32_t generation = 0;
            ActorSlotState state = ActorSlotState::Empty;
        };

        struct ModelSlot {
            std::uint32_t generation = 0;
            std::uint32_t denseIndex = std::numeric_limits<std::uint32_t>::max();
            bool active = false;
        };

        [[nodiscard]] ActorSlot* findActorSlot(ActorHandle handle) noexcept;
        [[nodiscard]] const ActorSlot* findActorSlot(ActorHandle handle) const noexcept;
        [[nodiscard]] ModelSlot* findModelSlot(ModelHandle handle) noexcept;
        [[nodiscard]] const ModelSlot* findModelSlot(ModelHandle handle) const noexcept;
        ActorHandle allocateActorSlot(std::unique_ptr<Actor> actor);
        void activateActor(ActorHandle handle);
        void flushActorChanges();
        void destroyActorSlot(ActorHandle handle);
        void touchRevision(bool topology, bool model);
        void touchActorRevision() noexcept;

        friend class Actor;

        std::vector<assets::Mesh> meshes_;
        std::vector<std::uint32_t> meshGenerations_;
        std::vector<bool> meshAlive_;
        std::vector<ModelInstance> models_;
        std::vector<ModelSlot> modelSlots_;
        std::vector<std::uint32_t> denseModelSlots_;
        std::vector<ActorSlot> actors_;
        bool ticking_ = false;
        bool flushingActorChanges_ = false;
        bool destroying_ = false;
        std::size_t callbackDepth_ = 0;
        std::uint64_t revision_ = 0;
        std::uint64_t topologyRevision_ = 0;
        std::uint64_t modelRevision_ = 0;
        SceneEnvironment environment_{};
        std::uint64_t lightingRevision_ = 0;
        std::uint64_t atmosphereRevision_ = 0;
    };

} // namespace lumin::scene
