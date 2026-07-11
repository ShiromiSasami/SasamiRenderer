# Sasami Renderer

Sasami Renderer は C++20 のレンダラー実験プロジェクトです。DirectX 12 の feature render path を主軸に、RenderGraph、PBR、GBuffer、shadow、Screen Space Reflection、Software Ray Tracing、DXR、GI、複数 RHI バックエンドの検証を同じコードベースで進めています。

この README は 2026-07-04 時点の実装状態を基準にしています。未検証または部分実装の項目は明示します。

## Project Layout

| Path | Role |
| --- | --- |
| `Source/` | C++ の engine、renderer、platform 実装 |
| `Shaders/` | ファーストパーティ HLSL/HLSLI の正式な配置場所 |
| `Assets/` | サンプルモデル、テクスチャなど |
| `Libraries/` | 外部依存。`Libraries/NRD/Shaders` はサードパーティ側の構成なので移動対象外 |
| `Tools/` | DXC などの開発補助ツール |
| `x64/` | Visual Studio のビルド出力。コミット対象外 |

## Build

Visual Studio 2022 で `SasamiRenderer.sln` を開き、`x64` + `Debug` または `Release` を選んでビルドします。

Developer Prompt からの例:

```bat
msbuild SasamiRenderer.vcxproj /p:Configuration=Debug /p:Platform=x64
msbuild SasamiRenderer.vcxproj /p:Configuration=Release /p:Platform=x64
```

Debug では D3D12 Debug Layer と GPU-based validation を有効にして、Output window の D3D12 メッセージを確認してください。

## Shader Layout

ファーストパーティの HLSL/HLSLI は `Shaders/` 配下に集約し、責務別に階層化しています。

主な分類:

- `Shaders/Shared/`: 共通定義、共通 constant buffer、基礎 shader
- `Shaders/Raster/Geometry/`: opaque、skinned mesh、tessellation、mesh shader
- `Shaders/Raster/Lighting/`: PBR lighting、shadow
- `Shaders/Raster/Transparency/`: transparent / OIT
- `Shaders/Effects/`: SSAO、SSR、post process、sky、volumetric cloud、ray march
- `Shaders/RayTracing/`: DXR、SWRT、GI probe update
- `Shaders/Denoising/`: denoiser integration 用 shader include
- `Shaders/Backend/`: native backend fallback 用 shader
- `Shaders/Debug/`: debug visualization

代表例:

- `Shaders/Raster/Lighting/PBR/CookTorranceGGX_VS.hlsl`
- `Shaders/Raster/Lighting/PBR/CookTorranceGGX_PS.hlsl`
- `Shaders/Effects/Reflections/SSR/ScreenSpaceReflection_CS.hlsl`
- `Shaders/RayTracing/SWRT/*.hlsl`
- `Shaders/RayTracing/DXR/RayTracing.hlsl`
- `Shaders/Backend/Native/NativeMesh.hlsl`

ランタイムのシェーダー解決は `ShaderCompilationService::GetShaderSourceRoot()` を使います。現在は `Shaders/` を優先し、旧配置 `Source/Renderer/Shaders` はフォールバックとしてのみ扱います。

MSBuild 側の `ShaderSourceRoot` も `$(ProjectDir)Shaders` を参照します。新しいシェーダーを追加する場合は、責務カテゴリを先に選び、必要に応じて `SasamiRenderer.vcxproj` と `.filters` に追加してください。

## Backend Support

| Backend | Status |
| --- | --- |
| DirectX 12 | 主実装。feature render path、RenderGraph、GBuffer、PBR、SSR、SWRT、DXR 周辺を検証対象にしています。RHI 抽象化層 (BLAS/TLAS ビルド、RT パイプライン、SBT) の DX12 実装完了。 |
| Vulkan | native fallback path。clear/present、static mesh (Y-flip viewport、albedo texture、DX-compatible projection)、D32 depth test、swapchain resize、RGBA8 texture upload、ray march (`RayMarchApp`)、compute dispatch を実装済み。拡張機能はランタイムで検出 (VK_KHR_dynamic_rendering / descriptor_indexing / timeline_semaphore / ray_query / ray_tracing_pipeline / acceleration_structure)。BLAS/TLAS ビルド (BuildRhiBlases / BuildRhiTlas)、RT パイプライン (CreateRhiRayTracingPipeline)、SBT (CreateRhiShaderBindingTable)、trace dispatch (command encoder の DispatchRays / vkCmdTraceRaysKHR) を VK_KHR_ray_tracing_pipeline で実装済み。RT パイプラインは `RhiRayTracingPipelineDesc::spirvBytecode` に SPIR-V を渡す (DX12 は並列の DXIL フィールドを使用)。ただし DxrRayTracer と render graph は DX12 専用のため、これらの RHI プリミティブはまだ Vulkan の描画パスには結線されていません。DX12 feature path と同等ではありません。 |
| DirectX 11 | native fallback path。clear/present、static mesh、depth test、swapchain resize、RGBA8 texture upload、ray march (`RayMarchApp`)、compute dispatch を実装済み。DX12 feature path と同等ではありません。 |
| OpenGL | native fallback path。clear/present、static mesh (行列の行列の列優先変換修正済み)、depth test、window resize、albedo texture sampling、ray march (`RayMarchApp`)、compute dispatch を実装済み。DX12 feature path と同等ではありません。 |

Vulkan / DirectX 11 / OpenGL では、メインループが `OnUpdate` / `OnRender` を経由して `SyncModelsToRenderer` と `UpdateCameraCB` を正しく呼び出すため、DX12 と同一のゲームループパスを使用します。これらのバックエンドは DX12 feature pass ベースの RenderGraph には未対応です。

RayMarch バックエンド対応について: Vulkan は DXC で HLSL を SPIR-V にコンパイルして `RayMarch_VS/PS.hlsl` を再利用します。OpenGL は GLSL 330 core で同等のシェーダーをインライン実装しています。いずれも `RhiBackendRayMarchFrameDesc` 経由で `ExecuteBackendFrame` から呼び出されます。

Vulkan RT インフラについて: GPU が RT 拡張機能をサポートしている場合、`GetCapabilities().supportsHardwareRayTracing`/`supportsRayQuery` が true になります。BLAS/TLAS 構築に加え、RT パイプライン (`CreateRhiRayTracingPipeline`)、SBT (`CreateRhiShaderBindingTable`)、trace dispatch (`VulkanRhiCommandEncoder::DispatchRays` → `vkCmdTraceRaysKHR`) を実装済みです。SPIR-V は `RhiRayTracingPipelineDesc::spirvBytecode` で供給し (DX12 の DXIL フィールドと並列、非破壊)、パイプラインは内部固定の descriptor set layout (binding 0 = acceleration structure、binding 1 = storage image) を構築します。SPIR-V の各 export は OpEntryPoint の execution model からステージを解決するため、追加のステージメタデータは不要です。

このパスは `RunRayTracingSmokeTest` で検証できます。環境変数 `SASAMI_VK_RT_SMOKETEST=1` を設定して Vulkan 構成の `PBRApp` を起動すると、初期化時に三角形 1 枚の BLAS/TLAS を構築し、`VulkanRtSmokeTest.hlsl` を SPIR-V にコンパイルして RT パイプライン + SBT を作成し、64×64 の storage image に 1 ピクセル 1 レイでトレースして、中心ピクセル = 赤 (hit) / 端 = 青 (miss) を読み戻し検証します。結果は `PBRApp.exe.log` に `Vulkan RT smoke test: PASS` として記録されます。

制限事項: `DxrRayTracer` と render graph の RT パス、および DXR シェーダの `ResourceDescriptorHeap` bindless モデルは DX12 専用です。これらの RHI プリミティブを Vulkan の描画パスに結線し、実画面へ HW RT を出力するには、Vulkan 互換の binding モデルを持つシェーダ改修を含む追加作業が必要です。

## Render Pipeline

既定パス順:

```text
Shadow -> Opaque -> OpaqueGBuffer -> RuntimeAO -> RuntimeAOBlur -> Lighting -> ScreenSpaceReflection -> SoftwareReflection -> SoftwareReflectionComposite -> Skybox -> TransparentBackfaceDistance -> TransparentSceneColorCopy -> Transparent -> TransparentLighting -> TransparentComposite -> PostProcess
```

主なパス:

- `Shadow`: directional / CSM / VSM shadow map
- `Opaque`: forward opaque fallback と GBuffer 生成の前段
- `RuntimeAO` / `RuntimeAOBlur`: SSAO または runtime AO
- `OpaqueGBuffer`: opaque mesh から GBuffer と depth を出力
- `Lighting`: GBuffer/depth/light/shadow/IBL/AO を読む fullscreen deferred lighting combine。`SceneColor` を生成
- `ScreenSpaceReflection`: Lighting 後の `SceneColor` をコピーし、compute shader で SSR radiance/confidence を生成
- `SoftwareReflection` / `SoftwareReflectionComposite`: SWRT reflection。SWRT が有効な場合は SSR より優先
- `Skybox`: sky / procedural sky
- `Transparent*`: transparent backface distance、scene color copy、weighted blended OIT、transparent lighting/composite
- `PostProcess`: tone mapping など

## Screen Space Reflection

SSR は `CookTorranceGGX_PS.hlsl` 内の inline ray march ではなく、専用 render pass と compute shader に分離しています。

実装ファイル:

- `Source/Renderer/Passes/Reflections/ScreenSpaceReflectionRenderPass.*`
- `Shaders/Effects/Reflections/SSR/ScreenSpaceReflection_CS.hlsl`
- `Source/Renderer/Resources/RenderPipelineStateCache_Ssr.cpp`

現在の挙動:

- Lighting 完了後の `SceneColor` を `SSRSceneColorCopy` にコピーします。
- `SceneDepth`、`GBufferNormal`、`GBufferMaterial`、`SSRSceneColorCopy` を compute shader で参照します。
- world-space position を `cameraInvPV` から復元し、反射方向を screen-space に投影して depth hit を探索します。
- 出力 `SSRReflection` は RGB に reflected radiance、A に confidence を格納します。
- 既存の reflection composite shader で `SSRReflection` を `SceneColor` に加算合成します。
- SWRT reflection が有効な場合、SSR は実行しません。
- `ReflectionRadiance` debug view は SSR raw radiance、`ReflectionAlpha` debug view は SSR confidence alpha を表示します。
- `PhaseTag()` は `"Scene"` を返します。他の scene pass と同フェーズに属さないと render graph の位相ソートが循環グラフと誤判定します。

既知の制限:

- hierarchical Z、temporal accumulation、stochastic sampling、roughness mip blur は未実装です。
- screen-space 外、遮蔽物の裏側、近すぎる geometry、極端な法線、深度不連続は miss します。
- 現状は DX12 feature render path 用です。Vulkan / DX11 / OpenGL native fallback では SSR は動作しません。

参考にした実装方針:

- Morgan McGuire and Michael Mara, "Efficient GPU Screen-Space Ray Tracing", JCGT 2014: https://jcgt.org/published/0003/04/04/
- AMD FidelityFX Stochastic Screen Space Reflections: https://gpuopen.com/fidelityfx-sssr/
- Unreal Engine Screen Space Reflections documentation: https://dev.epicgames.com/documentation/unreal-engine/screen-space-reflections-in-unreal-engine

## Profiling

Microsoft 系ツールは以下の役割で使います。

| Tool | Use |
| --- | --- |
| PIX on Windows | DX12 GPU capture、timing capture、shader/resource inspection |
| GPUView | Windows GPU scheduler、queue、present、CPU/GPU overlap の ETL 解析 |
| WPR / WPA | CPU thread、wait、file IO、memory、custom TraceLogging event の解析 |

実装済み instrumentation:

- `Source/Foundation/Profiling/Profiler.*`
- `ApplicationCore` で `SasamiRenderer` TraceLogging provider を register/unregister
- `Renderer::Render` を CPU event として記録
- `RenderGraph::Execute` と各 render pass / phase completion node を CPU event として記録
- `pix3.h` が include path に存在する構成では、同じ render pass 名で DX12 command list に PIX GPU event を記録
- `pix3.h` がない構成では GPU event は no-op。ETW/TraceLogging は Windows SDK のみで動作します。

## Implementation Policy

## RHI Migration Notes

Status as of 2026-06-28:

- `IRhiDevice` is the neutral lower RHI interface. `IRHIDevice` is the transitional renderer-facing device that still exposes the D3D12 compatibility surface used by the current feature render path.
- `DebugProbeGridRenderPass` now creates its pipeline layout, graphics pipeline, and vertex buffer through RHI descriptors/API instead of directly owning D3D12 root signature and PSO objects.
- `RhiFormat::R32G32B32Float` is available for float3 vertex attributes and is mapped in the DX12, DX11, Vulkan, and OpenGL backend vertex-format converters. `RhiFormat::R16Float` is also available for single-channel half-float render targets such as transparent OIT revealage.
- `IRhiCommandEncoder` now has buffer-handle-based `SetVertexBufferBindings()` and `SetIndexBufferBinding()` methods. `DebugProbeGridRenderPass`, `MeshBuffer`, `SkinnedMeshBuffer`, `Skybox`, and `ProceduralSkyRenderPass` use these for RHI-created vertex/index buffers instead of packing RHI handle IDs into GPU-address views.
- `MeshBuffer`, `SkinnedMeshBuffer`, and `Skybox` use RHI buffer creation whenever `supportsRhiResourceCreation` is available. Their old GPU virtual address vertex/index binding paths are now D3D12-compatibility fallbacks only.
- `SceneSubmitter` now creates submitted RGBA8 scene textures through `CreateRhiTexture2DFromRgba8()` when RHI resource and descriptor creation are available. On the DX12 feature path, it writes SRVs into the renderer's existing global SRV heap so descriptor-table binding stays compatible with current passes.
- `LightSystem` now prefers RHI buffer creation for per-frame lighting constant buffers and point/spot light StructuredBuffers. Point/spot StructuredBuffer SRVs are created through `CreateRhiBufferShaderResourceView()`. The current DX12 feature path still uses compatibility resources for persistent mapped writes and root CBV GPU virtual-address binding.
- `ShadowMapManager` now prefers RHI texture creation for directional CSM depth maps (`R32Typeless`), spot depth maps (`R16Typeless`), and VSM shadow textures (`R32G32Float` SRV/RTV/UAV resources), then uses DX12 compatibility resources for the current feature path's DSV/RTV/SRV/UAV descriptor creation and barriers. `RhiFormat` includes `R32Typeless`, `R16Typeless`, `R16UNorm`, and `D16UNorm` mappings so typeless depth storage plus DSV/SRV view formats can be represented on DX backends. Non-DX backends currently map typeless depth formats to nearest typed/depth formats; exact typeless reinterpretation parity is not claimed for the DX12 feature path yet.
- OpenGL native fallback now samples submitted albedo textures in its mesh shader path and handles padded RGBA8 upload rows by compacting to tightly packed rows before `glTexImage2D()`. DirectX 11 rejects invalid RGBA8 upload pitches smaller than `width * 4` before `CreateTexture2D()`.
- The Vulkan backend implements `CreateRhiTexture2DFromRgba8()` using a host-visible staging buffer, `vkCmdCopyBufferToImage()`, and layout transitions from undefined to transfer destination to shader-read-only.
- `OpaqueGBufferRenderPass` owns the opaque mesh GBuffer/depth draw that was previously submitted from `LightingRenderPass`. This pass is registered at index 16 in `RenderPassRegistry::m_builtinPasses` and is included in `kDefaultRenderPathSequence`. `LightingRenderPass` no longer calls `drawOpaqueItems`; it now runs a fullscreen deferred lighting combine shader and writes `SceneColor`.
- `IrradianceProbeGrid::FillProbeGridCB` now gates `giEnabled` on `IsBaked()` in addition to `m_enabled && m_pso`. Before this fix, a failed SWRT/probe PSO would set `giEnabled = 0.0f`, falling back to IBL and causing the scene to be over-brightened by the HDR environment map. Now: probe not baked → `giEnabled = 0.0f` (IBL fallback, controlled by `iblIntensity`); probe baked → `giEnabled = 1.0f` (GI probe irradiance). `RenderSettings::iblIntensity` defaults to `0.0f` so the fallback path contributes no indirect diffuse until IBL is explicitly enabled.
- BakeGI is currently a finite runtime probe bake, not a persistent offline asset bake. `IrradianceProbeGrid::UpdateProbes()` advances up to 32 probes per call until every probe has been dispatched once; SWRT reflection reuse no longer keeps writing baked GI probes every frame. A future dedicated bake tool should load the scene, build/update the SWRT or DXR acceleration data, dispatch the probe bake to completion, read back SH probe data, and serialize a cache asset keyed by scene/probe/light settings. Until that exists, runtime callers must call `RequestGIBake()` or `ResetAndRebakeGI()` to intentionally rebake changed scene or lighting data.
- GI bake progress is exposed through `Renderer::GetGIBakeStatus()` and `ApplicationCore::GetGIBakeStatus()`. The status reports state (`Baking`, `Completed`, `WaitingForProbeGrid`, `WaitingForBvh`, or `Failed`), completed/total probe counts, probes processed per step, estimated remaining render frames, stalled frame count, and a SWRT BVH missing-buffer mask when blocked by `WaitingForBvh`. A 0% bake with `WaitingForBvh` means the SWRT BVH was not available yet; it should be shown as a blocked/waiting state instead of an unknown infinite bake.
- The PBR sample's existing `GI` tab displays GI bake state, a progress bar, probe counts, per-frame probe step, estimated remaining frames, and blocked/failed reason. If the GUI shows `Waiting for software ray tracing BVH buffers`, the bake is not merely slow; it is waiting for valid SWRT BVH GPU buffers. The tab also lists the missing SWRT BVH inputs (`bvhNodes`, `triangles`, `meshInfo`, `instances`, `tlasNodes`, `materials`, or SWRT initialization) so the wait can be diagnosed from the UI.
- `GpuSoftwareRayTracer::GetBvhGpuAddresses()` only reports a valid SWRT BVH when all six GPU buffers are available: BVH nodes, triangles, mesh info, instances, TLAS nodes, and materials. GI probe update dispatch must not run with a partial BVH binding set, because the compute shader reads all six buffers.
- `ResetAndRebakeGI()` resets bake progress without reallocating the GI probe buffer. Reallocating the probe buffer during `Renderer::Render()` is unsafe because earlier lighting commands in the same command list may already reference the previous buffer. Probe buffer reallocation is reserved for probe-grid layout changes such as `FitProbeGridToScene()`.
- `IrradianceProbeGrid` now prefers RHI buffer creation for the probe SH buffer and mapped constant buffer. Its probe SH SRV is created through `CreateRhiBufferShaderResourceView()`, while the current DX12 feature path still uses the compatibility resource for root GPU virtual-address bindings, UAV creation, barriers, and compute dispatch.
- `MeshletBuffer` now prefers RHI buffer creation for meshlet descriptor and meshlet index buffers, then uses DX12 compatibility resources for the current mesh shader feature path's GPU virtual-address root bindings. It now releases RHI handles on re-upload, compatibility fallback failure, and destruction instead of dropping failed compatibility handles.
- The RHI resource contract now exposes `DestroyRhiResource()` for explicit texture/buffer lifetime management. DX12 and DX11 remove the registry entry, Vulkan also destroys related image views plus the native image/buffer and memory, and OpenGL deletes the native texture/buffer plus cached texture-view bindings. Callers must ensure the resource is no longer in flight before destruction.
- The RHI buffer contract exposes `UpdateRhiBuffer()`, `ReadRhiBuffer()`, `CopyBuffer()`, and `CreateRhiBufferShaderResourceView()`. The portable readback sequence is GPU resource transition to copy source, `CopyBuffer()` into a `GpuToCpu` buffer, queue submission/idle synchronization, then `ReadRhiBuffer()`. DX12 uses readback heaps in `COPY_DEST`, DX11 uses staging buffers with CPU read access, Vulkan uses host-visible coherent memory, and OpenGL uses `glGetBufferSubData()`. Vulkan command encoding emits buffer memory barriers in addition to image barriers.
- Vulkan buffer and image descriptor tables are backed by an owned descriptor pool and per-command-encoder descriptor sets. Structured buffer SRVs map to storage-buffer descriptors. This does not make the DX12 feature render path portable by itself: inline GPU-address bindings and feature-pass shader portability remain incomplete.
- The RHI descriptor contract exposes texture and buffer unordered-access views. DX12 creates native UAV descriptors, DX11 binds compute UAV slots, Vulkan uses storage-image/storage-buffer descriptors, and OpenGL binds 2D images or SSBOs for compute. Texture UAV support currently covers 2D/2D-array on DX11 and Vulkan, 2D on OpenGL, and 2D/2D-array/3D on DX12. This is descriptor plumbing only; no non-DX12 feature pass has been declared visually equivalent yet.
- `RhiStaticSamplerDesc` is implemented on all four backends. DX12 uses root-signature static samplers, Vulkan uses immutable samplers owned for the descriptor-set-layout lifetime, DX11 owns `ID3D11SamplerState` objects in the RHI pipeline layout, and OpenGL owns sampler objects bound to texture units when the layout is selected. Vulkan space0 bindings use fixed DXC-compatible register-class ranges (`b=0`, `t=100`, `s=200`, `u=300`, 100 slots per class); register spaces other than zero remain unsupported by the current single-set Vulkan layout.
- RHI-owned resources in `MeshBuffer`, `SkinnedMeshBuffer`, `Skybox`, `SceneSubmitter`, `IrradianceProbeGrid`, `LightSystem`, `ShadowMapManager`, `IblSystem`, `MeshletBuffer`, and `RenderTargetPool` now explicitly call `DestroyRhiResource()` on release, shutdown, resize, or fallback failure paths. This is still a caller-side lifetime contract; GPU idle/fence safety must be handled before destroying resources that may be in flight.
- `RenderTargetPool` now prefers RHI texture creation for the shared typeless depth target, HDR scene color, GBuffer targets, SSAO/blur, SWRT shadow/reflection/AO, SSR scene-color/reflection targets, transparent scene/transmission copies, transparent backface distance, weighted blended OIT, ReSTIR shadow, and HW ray tracing output. The DX12 feature path keeps using compatibility resources for its DSV/RTV/SRV/UAV descriptors and barriers. Resize and pool release explicitly destroy the associated RHI handles. Direct DX12 allocation remains as a fallback when RHI resource creation or DX12 compatibility resources are unavailable.
- Baked GI probe diffuse is evaluated independently from the IBL enable/intensity toggle in both forward PBR and deferred lighting. When `g_giEnabled > 0.5`, lighting uses `GI_SampleProbeGrid() * giIntensity` as the indirect diffuse irradiance source; IBL diffuse remains the fallback when GI is not baked/enabled.
- Deferred lighting currently covers the core raster opaque path: directional/point/spot lights, CSM/VSM/spot shadows, IBL diffuse, runtime AO, emissive, GBuffer debug views, GI probe irradiance, and specular-glossiness material workflow. Specular-glossiness uses `GBufferSpecularWorkflow` to carry per-pixel specular color and workflow flag into the fullscreen lighting combine. Runtime visual parity with the previous forward path still needs scene-level capture verification.
- `IblSystem` now owns all IBL GPU resources (irradiance cubemap, prefilter cubemap, BRDF LUT) and is aggregated by `Skybox`. On the DX12 feature path, `IblSystem::UploadGeneratedIblTextures()` prefers `CreateRhiTexture` + `GetD3D12CompatibilityResource` for the three HDR IBL textures, then falls back to `CreateCommittedResource`. Upload buffers (upload-heap `Resource` wrappers) and the fallback 1×1 placeholder textures remain DX12-direct. `Skybox` getter shims (`IsIblEnabled`, `GetIblPrefilterResource`, etc.) now forward to `m_iblSystem` with no behavior change for callers. `Skybox_IBL.cpp` now contains only the `EnsureIblTexturesUploaded` delegate; all generation and upload logic lives in `IblSystem.cpp`.
- Vulkan native mesh rendering now owns one D32 depth image/view per swapchain image and recreates native framebuffers/depth resources on resize. Vulkan capability flags report only features enabled on the logical device; ray query, ray tracing pipeline, mesh shader, descriptor indexing, timeline semaphore, and dynamic rendering remain disabled until `CreateDevice()` enables the corresponding extension and feature chains.
- OpenGL format conversion now covers `R32UInt` and `D24UNormS8UInt` in addition to the existing color, float, and depth formats.
- Baked GI probe diffuse is currently consumed only by the DX12 feature render path. Vulkan / DX11 / OpenGL native fallback shaders do not bind or sample the baked SH probe buffer, so cross-backend GI visual parity is not claimed. Completing it requires portable buffer descriptor binding, a non-DX12 probe bake or serialized probe cache upload path, and native/feature shaders that evaluate the same probe interpolation.
- `CreateRhiBuffer()` supports immediate `initialData` for `GpuOnly` buffers on all four backends: DX12 and Vulkan use staging copies, while DX11 `CreateBuffer()` and OpenGL `glBufferData()` consume initial data directly. `GpuToCpu + initialData` is rejected on DX12/DX11 because readback resources are copy destinations rather than upload resources.
- Non-DX12 configurations exclude the DX12 RHI ray-tracing implementation at translation-unit scope. This prevents Vulkan, DX11, and OpenGL builds from compiling definitions for the macro-hidden `Dx12GraphicsDevice` class.
- Remaining limitation: several DX12 feature-path systems still expose GPU virtual addresses for constant buffers, shader resource buffers, and ray tracing structures. SWRT/DXR BVH, acceleration structure, scratch/result buffers, and upload-helper staging resources remain direct DX12 allocations where the current RHI contract does not yet represent the required build/copy/state semantics. Future work should move these resource owners to RHI handles where the backend contract can represent the required binding type and synchronization requirements.
- Debug probe-grid visualization ("Show Probe Spheres") fix: `IDevice::CreateRhiPipelineLayout()` / `CreateRhiGraphicsPipeline()` return handles whose id is an internal map key, but command-encoder binding (`SetGraphicsPipelineLayout`/`SetGraphicsPipeline`) treats `handle.id` as the raw native `ID3D12RootSignature*`/`ID3D12PipelineState*` (matching `RenderPipelineStateCache::MakeLayoutHandle`/`MakePipelineHandle`). `DebugProbeGridRenderPass` was the only pass creating its pipeline via the device path, so binding its create-handles dereferenced a small integer as a pointer and crashed. Added `IRHIDevice::GetBindablePipelineLayoutHandle()` / `GetBindablePipelineHandle()` (DX12 resolves the map entry to the native pointer; default identity for other backends); the pass now caches and binds the bindable forms.
- Debug probe-grid pass insertion fix: `Renderer::UseDefaultRenderNodePreset()` ("Reset Passes") re-inserted the volumetric-cloud pass but not the probe-grid pass, and `SetDebugProbeGridEnabled(true)` only set a flag without ensuring the pass was in the graph. Consolidated insertion into `Renderer::EnsureDebugProbeGridPassInserted()` (idempotent), now called from `UseDefaultRenderNodePreset()`, `SetRenderPassSequence()`, `ReinsertDebugProbeGrid()`, init, and on enable — so probes survive a preset reset and appear as soon as the toggle is checked.

## セッション永続化 (PBRApp)

PBRApp は終了時にカメラ・ライト・描画設定を保存し、次回起動時に自動復元します。手続き生成される球/箱シーンは破壊されません。

- 保存対象: メインカメラ (`target`/`yaw`/`pitch`/`distance`/`clip`/`move_speed`)、directional light、ユーザーが追加した point/spot light、`RenderSettings` 構造体全フィールド、GI トグル (enabled/intensity/ema)、probe grid preset、ライトギズモ表示フラグ。
- 保存先 (プロジェクトルート、per-user・git 管理外): `PBRApp.scene`（カメラ+ライト）と `PBRApp.settings.ini`（描画/アプリ設定）。パスは `ApplicationResourcePaths::ResolveConfigPathString()` が解決。
- 復元は非破壊: `ApplicationCore::ApplyCameraAndLights()` が `ClearObjects()` を呼ばず、既存のアクティブカメラ・directional light へ in-place 適用し、保存済み point/spot light のみ再構築します（`[static_model]` と未知セクションは無視）。破壊的なフルロードが必要な用途向けに `LoadScene()` は従来どおり残しています。
- レンダラは `Renderer::GetRenderSettings()/SetRenderSettings()` で `RenderSettings` を一括 round-trip します。
- 保存されないもの: GI のベイク結果データ（トグルのみ保存し、起動後に再ベイク）、skybox パス、ImGui ウィンドウレイアウト（`imgui.ini` が別管理）。

Implementation references for the portable buffer copy/readback contract:

- Microsoft `D3D12_HEAP_TYPE`: readback-heap resources must remain in `D3D12_RESOURCE_STATE_COPY_DEST`: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
- Microsoft D3D12 readback guidance: synchronize copy completion before mapping the readback buffer: https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
- Khronos `vkCmdPipelineBarrier` / `VkBufferMemoryBarrier`: buffer barriers define the resource-specific access dependency around buffer copies: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkCmdPipelineBarrier.html
- Khronos `VkDescriptorSetLayoutBinding`: immutable sampler handles are copied into the layout but sampler objects must outlive layout/set use: https://registry.khronos.org/vulkan/specs/latest/man/html/VkDescriptorSetLayoutBinding.html

レンダラー、graphics API、shading、lighting、ray tracing、GI、material、render graph を変更する場合は、実装前に短い実装方針を作り、可能な範囲で論文、公式 API 仕様、Unreal Engine などの既存実装、ベンダーサンプル、保守されているオープンソースレンダラーを参照してください。

外部コードは設計や挙動の参考に留め、互換性のないライセンスのコードをコピーしないでください。実装後は Debug/Release ビルド、シェーダーコンパイル、実行時描画確認で検証してください。ローカル環境で実行できなかった検証は、その理由を明記してください。
