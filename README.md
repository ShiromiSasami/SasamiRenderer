# Sasami Renderer

Sasami Renderer は C++20 のレンダラー実験プロジェクトです。DirectX 12 の feature render path を主軸に、RenderGraph、PBR、GBuffer、shadow、Screen Space Reflection、Software Ray Tracing、DXR、GI、複数 RHI バックエンドの検証を同じコードベースで進めています。

この README は 2026-06-14 時点の実装状態を基準にしています。未検証または部分実装の項目は明示します。

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
| DirectX 12 | 主実装。feature render path、RenderGraph、GBuffer、PBR、SSR、SWRT、DXR 周辺を検証対象にしています。 |
| Vulkan | native fallback path。clear/present と簡易 static mesh fallback はありますが、DX12 feature path と同等ではありません。 |
| DirectX 11 | native fallback path。clear/present と簡易 static mesh fallback はありますが、DX12 feature path と同等ではありません。 |
| OpenGL | native fallback path。clear/present と簡易 static mesh fallback はありますが、DX12 feature path と同等ではありません。 |

Vulkan / DirectX 11 / OpenGL が灰色画面になる場合は、native fallback の mesh draw 入力、shader compile、vertex/index buffer binding、RHI 実装の順で確認してください。これらのバックエンドは DX12 feature pass ベースの RenderGraph には未対応です。

## Render Pipeline

既定パス順:

```text
Shadow -> Opaque -> RuntimeAO -> RuntimeAOBlur -> Lighting -> ScreenSpaceReflection -> SoftwareReflection -> SoftwareReflectionComposite -> Skybox -> TransparentBackfaceDistance -> TransparentSceneColorCopy -> Transparent -> TransparentLighting -> TransparentComposite -> PostProcess
```

主なパス:

- `Shadow`: directional / CSM / VSM shadow map
- `Opaque`: forward opaque fallback と GBuffer 生成の前段
- `RuntimeAO` / `RuntimeAOBlur`: SSAO または runtime AO
- `Lighting`: PBR lighting と GBuffer 出力
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

Status as of 2026-06-16:

- `IRhiDevice` is the neutral lower RHI interface. `IRHIDevice` is the transitional renderer-facing device that still exposes the D3D12 compatibility surface used by the current feature render path.
- `DebugProbeGridRenderPass` now creates its pipeline layout, graphics pipeline, and vertex buffer through RHI descriptors/API instead of directly owning D3D12 root signature and PSO objects.
- `RhiFormat::R32G32B32Float` is available for float3 vertex attributes and is mapped in the DX12, DX11, Vulkan, and OpenGL backend vertex-format converters.
- `IRhiCommandEncoder` now has buffer-handle-based `SetVertexBufferBindings()` and `SetIndexBufferBinding()` methods. `DebugProbeGridRenderPass`, `MeshBuffer`, `SkinnedMeshBuffer`, `Skybox`, and `ProceduralSkyRenderPass` use these for RHI-created vertex/index buffers instead of packing RHI handle IDs into GPU-address views.
- `MeshBuffer`, `SkinnedMeshBuffer`, and `Skybox` use RHI buffer creation whenever `supportsRhiResourceCreation` is available. Their old GPU virtual address vertex/index binding paths are now D3D12-compatibility fallbacks only.
- Remaining limitation: several DX12 feature-path systems still expose GPU virtual addresses for constant buffers, shader resource buffers, ray tracing structures, and compatibility resources. Future work should move these resource owners to RHI handles where the backend contract can represent the required binding type.

レンダラー、graphics API、shading、lighting、ray tracing、GI、material、render graph を変更する場合は、実装前に短い実装方針を作り、可能な範囲で論文、公式 API 仕様、Unreal Engine などの既存実装、ベンダーサンプル、保守されているオープンソースレンダラーを参照してください。

外部コードは設計や挙動の参考に留め、互換性のないライセンスのコードをコピーしないでください。実装後は Debug/Release ビルド、シェーダーコンパイル、実行時描画確認で検証してください。ローカル環境で実行できなかった検証は、その理由を明記してください。
