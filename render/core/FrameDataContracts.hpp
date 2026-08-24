#pragma once

#include "render/core/FrameIdentity.hpp"
#include "render/core/RenderSettingsStore.hpp"
#include "render/resources/FrameGraph.hpp"
#include "render/world/RenderWorld.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

struct ImDrawData;

namespace lumin::render::core {

    /// 描述一个物理纹理及其在当前 FrameGraph 中唯一的逻辑身份。
    struct TextureFrameData {
        /// Feature 拥有或由交换链提供的物理 NvRHI 纹理。
        nvrhi::TextureHandle texture;
        /// 由本帧资源导入器返回的唯一 FrameGraph handle。
        FrameGraphResourceHandle graphResource;
        /// 最后一次写入该数据的 pass；没有生产 pass 时可无效。
        FrameGraphPassHandle readyPass;
        /// 纹理格式；必须与物理资源和消费者契约一致。
        nvrhi::Format format = nvrhi::Format::UNKNOWN;
        /// 当前纹理覆盖的有效像素范围。
        RenderExtent extent;

        /// 返回物理纹理和 FrameGraph 身份是否都有效。
        [[nodiscard]] bool isValid() const noexcept {
            return texture && graphResource.isValid();
        }
    };

    /// 描述一个物理 buffer 及其在当前 FrameGraph 中唯一的逻辑身份。
    struct BufferFrameData {
        /// Feature 拥有的物理 NvRHI buffer。
        nvrhi::BufferHandle buffer;
        /// 由本帧资源导入器返回的唯一 FrameGraph handle。
        FrameGraphResourceHandle graphResource;
        /// 最后一次写入该数据的 pass；没有生产 pass 时可无效。
        FrameGraphPassHandle readyPass;

        /// 返回物理 buffer 和 FrameGraph 身份是否都有效。
        [[nodiscard]] bool isValid() const noexcept {
            return buffer && graphResource.isValid();
        }
    };

    /// 描述一个物理加速结构及其在当前 FrameGraph 中唯一的逻辑身份。
    struct AccelerationStructureFrameData {
        /// GPU Scene Feature 拥有的物理 NvRHI 加速结构。
        nvrhi::rt::AccelStructHandle accelerationStructure;
        /// 由本帧资源导入器返回的唯一 FrameGraph handle。
        FrameGraphResourceHandle graphResource;
        /// 最后一次构建或更新该加速结构的 pass。
        FrameGraphPassHandle readyPass;

        /// 返回物理加速结构和 FrameGraph 身份是否都有效。
        [[nodiscard]] bool isValid() const noexcept {
            return accelerationStructure && graphResource.isValid();
        }
    };

    /// 保存主线程提交的一帧相机矩阵和时序基线，不引用活动 `Camera`。
    struct CameraFrameData {
        /// 当前帧世界到视图矩阵。
        glm::mat4 view{1.0f};
        /// 当前帧带抖动投影矩阵。
        glm::mat4 projection{1.0f};
        /// 当前帧带抖动视图投影矩阵。
        glm::mat4 viewProjection{1.0f};
        /// 最近一次成功提交帧的视图投影矩阵。
        glm::mat4 previousViewProjection{1.0f};
        /// 世界空间相机位置，`w` 固定为 1。
        glm::vec4 position{0.0f, 0.0f, 0.0f, 1.0f};
        /// 世界空间前向向量，`w` 固定为 0。
        glm::vec4 forward{0.0f, 0.0f, -1.0f, 0.0f};
        /// 世界空间相机右方向。
        glm::vec3 right{1.0f, 0.0f, 0.0f};
        /// 世界空间相机上方向。
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        /// 垂直视场角，单位为度。
        float fieldOfViewDegrees = 60.0f;
        /// 近裁剪面距离。
        float nearPlane = 0.05f;
        /// 远裁剪面距离。
        float farPlane = 200.0f;
        /// 活动相机渲染属性的修订号。
        std::uint64_t revision = 0;
        /// 当前帧 TAA 像素抖动。
        glm::vec2 jitter{0.0f};
        /// 显式切镜代数；连续相机移动不得修改。
        std::uint64_t cutEpoch = 0;
    };

    /** 由逻辑线程从活动场景提取，渲染主线程只读取该不可变快照。 */
    struct FrameSceneData {
        /// 完全拥有且不可变的渲染世界快照。
        world::RenderWorldSnapshotPtr world;
        /// 与该世界快照同一次主线程提交生成的相机数据。
        CameraFrameData camera;
        /// 全部 Feature 设置的不可变快照。
        RenderSettingsSnapshot settings;
        /// 相对最近一次成功提交世界快照的场景变化。
        world::SceneChangeMask changes = world::SceneChangeMask::None;
    };

    /// GPU Scene Feature 发布的逐帧几何、材质和加速结构资源。
    struct GpuSceneData {
        /// 当前帧可追踪实例的顶层加速结构。
        AccelerationStructureFrameData topLevelAccelerationStructure;
        /// 稳定实例索引对应的实例记录 buffer。
        BufferFrameData instances;
        /// 稳定材质索引对应的材质记录 buffer。
        BufferFrameData materials;
        /// 按 GPU mesh 索引排列的顶点 buffer。
        std::vector<BufferFrameData> vertices;
        /// 按 GPU mesh 索引排列的索引 buffer。
        std::vector<BufferFrameData> indices;
    };

    /// Raster Shadow Feature 发布的四级联阴影数据。
    struct ShadowData {
        /// 四张阴影深度纹理及其唯一 FrameGraph 身份。
        std::array<TextureFrameData, 4> cascades;
        /// 每一级联的世界到光源裁剪空间矩阵。
        std::array<glm::mat4, 4> viewProjections{};
        /// 视图空间级联分割距离。
        glm::vec4 splits{0.0f};
    };

    /// Raster Surface Feature 发布的标准 G-buffer 数据。
    struct RasterSurfaceData {
        /// 世界空间位置。
        TextureFrameData position;
        /// 世界空间法线与感知粗糙度。
        TextureFrameData normalRoughness;
        /// 线性基础颜色与金属度。
        TextureFrameData albedoMetallic;
        /// 当前 UV 到上一成功帧 UV 的运动矢量。
        TextureFrameData motion;
        /// 稳定 GPU 材质索引。
        TextureFrameData materialId;
        /// 场景深度。
        TextureFrameData depth;
        /// 与 `materialId` 同一索引空间的 GPU 材质表。
        BufferFrameData materials;
    };

    /// Ray Tracing Surface Feature 发布的 primary visibility 与表面信号。
    struct RtSurfaceData {
        /// 世界空间命中位置与主射线命中距离。
        TextureFrameData worldPositionHitDistance;
        /// 世界空间法线与感知粗糙度。
        TextureFrameData normalRoughness;
        /// 线性基础颜色与金属度。
        TextureFrameData albedoMetallic;
        /// 当前 UV 到上一成功帧 UV 的运动矢量。
        TextureFrameData motion;
        /// 稳定 GPU 材质索引。
        TextureFrameData materialId;
        /// 视图空间线性深度。
        TextureFrameData viewDepth;
        /// 光源或环境可见度。
        TextureFrameData visibility;
        /// primary surface 上计算的直接辐射亮度。
        TextureFrameData directRadiance;
        /// 未解调的直接光漫反射波瓣与 shadow hit distance。
        TextureFrameData directDiffuseRadianceHitDistance;
        /// 未解调的直接光镜面波瓣与 shadow hit distance。
        TextureFrameData directSpecularRadianceHitDistance;
    };

    /// Atmosphere Feature 发布的共享散射查找表。
    struct AtmosphereData {
        /// 大气透射率 LUT。
        TextureFrameData transmittance;
        /// 多次散射 LUT。
        TextureFrameData multiScattering;
        /// 当前视图天空 LUT。
        TextureFrameData skyView;
        /// 空中透视体积或二维投影结果。
        TextureFrameData aerialPerspective;
    };

    /// GI Feature 发布的原始间接光照信号。
    struct IndirectLightingData {
        /// 按 NRD REBLUR 契约编码的漫反射辐亮度与命中距离。
        TextureFrameData diffuse;
        /// 按 NRD REBLUR 契约编码的镜面辐亮度与命中距离。
        TextureFrameData specular;
        /// 可选的预合成间接光照；未生产时可无效。
        TextureFrameData combined;
    };

    /// Denoising Feature 发布的稳定直接光与间接光信号。
    struct DenoisedLightingData {
        /// 去噪并恢复主表面材质的直接光；NRD 关闭时直接引用 RTDI raw 输出。
        TextureFrameData direct;
        /// 去噪后的漫反射间接辐亮度与命中距离。
        TextureFrameData diffuse;
        /// 去噪后的镜面间接辐亮度与命中距离。
        TextureFrameData specular;
        /// 已恢复主表面材质的合并间接光照。
        TextureFrameData combined;
    };

    /// Lighting Composite Feature 发布的 HDR 场景及后处理辅助输入。
    struct SceneHdrData {
        /// 线性 HDR 场景颜色。
        TextureFrameData color;
        /// 与 HDR 颜色对应的世界空间位置；`w > 0` 表示几何像素。
        TextureFrameData position;
        /// 与 HDR 颜色对应的运动矢量。
        TextureFrameData motion;
        /// 与 HDR 颜色对应的场景深度。
        TextureFrameData depth;
    };

    /// Temporal Feature 发布的解析结果和本帧历史读写资源。
    struct TemporalOutputData {
        /// 时序解析后的 HDR 颜色。
        TextureFrameData color;
        /// 本帧读取的最近成功历史。
        TextureFrameData historyRead;
        /// 仅在提交成功后成为新历史的候选写入资源。
        TextureFrameData historyWrite;
    };

    /// Bloom Feature 发布的 HDR 后处理结果。
    struct BloomOutputData {
        /// 已合成多级泛光的线性 HDR 颜色；Bloom 关闭时为 Temporal 输出的逐像素副本。
        TextureFrameData color;
    };

    /** Runtime 在获取交换链图像后发布给 Presentation Feature 的输入。 */
    struct PresentationInputData {
        /// 仅在同步 `drawFrame()` 调用期间有效的 Dear ImGui 绘制数据。
        const ImDrawData* ui = nullptr;
        /// 当前交换链图像及其唯一 FrameGraph 身份。
        TextureFrameData swapchain;
        /// Presentation 字体图集及其唯一 FrameGraph 身份。
        TextureFrameData fontAtlas;
        /// 当前交换链图像索引。
        std::uint32_t imageIndex = 0;
        /// 当前帧槽索引。
        std::uint32_t frameSlot = 0;
    };

    /// Tone Mapping Feature 发布、Presentation Feature 消费的 Editor Viewport 输出。
    struct ViewportOutputData {
        /// 已完成 tone mapping 的 Viewport 纹理。
        TextureFrameData color;
    };

    /// Presentation Feature 发布的最终 Viewport 与交换链输出。
    struct PresentData {
        /// Editor 以稳定逻辑纹理 ID 引用的 Viewport 输出。
        TextureFrameData viewport;
        /// 本帧获取并转换到可 present 状态的交换链图像。
        TextureFrameData swapchain;
    };

    /// 提供内置帧数据类型的稳定契约对象。
    namespace frame_data {

        /// 返回类型 `T` 对应的进程内稳定契约；首次调用时拥有诊断名。
        template <typename T> [[nodiscard]] const FrameDataContract& contract(std::string_view name) {
            static const FrameDataContract value = FrameDataContract::of<T>(name);
            return value;
        }

        /// 返回 `FrameSceneData` 契约。
        [[nodiscard]] inline const FrameDataContract& scene() {
            return contract<FrameSceneData>("frame-scene");
        }
        /// 返回 `GpuSceneData` 契约。
        [[nodiscard]] inline const FrameDataContract& gpuScene() {
            return contract<GpuSceneData>("gpu-scene");
        }
        /// 返回 `ShadowData` 契约。
        [[nodiscard]] inline const FrameDataContract& shadows() {
            return contract<ShadowData>("shadows");
        }
        /// 返回 `RasterSurfaceData` 契约。
        [[nodiscard]] inline const FrameDataContract& rasterSurface() {
            return contract<RasterSurfaceData>("raster-surface");
        }
        /// 返回 `RtSurfaceData` 契约。
        [[nodiscard]] inline const FrameDataContract& rtSurface() {
            return contract<RtSurfaceData>("rt-surface");
        }
        /// 返回 `AtmosphereData` 契约。
        [[nodiscard]] inline const FrameDataContract& atmosphere() {
            return contract<AtmosphereData>("atmosphere");
        }
        /// 返回 `IndirectLightingData` 契约。
        [[nodiscard]] inline const FrameDataContract& indirectLighting() {
            return contract<IndirectLightingData>("indirect-lighting");
        }
        /// 返回 `DenoisedLightingData` 契约。
        [[nodiscard]] inline const FrameDataContract& denoisedLighting() {
            return contract<DenoisedLightingData>("denoised-lighting");
        }
        /// 返回 `SceneHdrData` 契约。
        [[nodiscard]] inline const FrameDataContract& sceneHdr() {
            return contract<SceneHdrData>("scene-hdr");
        }
        /// 返回 `TemporalOutputData` 契约。
        [[nodiscard]] inline const FrameDataContract& temporalOutput() {
            return contract<TemporalOutputData>("temporal-output");
        }
        /// 返回 `BloomOutputData` 契约。
        [[nodiscard]] inline const FrameDataContract& bloomOutput() {
            return contract<BloomOutputData>("bloom-output");
        }
        /// 返回 `PresentationInputData` 契约。
        [[nodiscard]] inline const FrameDataContract& presentationInput() {
            return contract<PresentationInputData>("presentation-input");
        }
        /// 返回 `ViewportOutputData` 契约。
        [[nodiscard]] inline const FrameDataContract& viewportOutput() {
            return contract<ViewportOutputData>("viewport-output");
        }
        /// 返回 `PresentData` 契约。
        [[nodiscard]] inline const FrameDataContract& present() {
            return contract<PresentData>("present");
        }

    } // namespace frame_data

} // namespace lumin::render::core
