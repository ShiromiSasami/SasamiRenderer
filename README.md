# Sasami Renderer

Sasami Renderer は C++20 のレンダラー実験プロジェクトです。DirectX 12 の feature render path を主軸に、RenderGraph、PBR、GBuffer、shadow、Screen Space Reflection、Software Ray Tracing、DXR、GI、複数 RHI バックエンドの検証を同じコードベースで進めています。

この README は **2026-08-24 時点のソースコード**を基準にしています。実装済みでも実機確認が終わっていない機能、DX12 専用機能、部分実装は各節で明示します。

## Project Layout

| Path | Role |
| --- | --- |
| `Source/` | C++ の engine、renderer、platform 実装 |
| `Shaders/` | ファーストパーティ HLSL/HLSLI の正式な配置場所 |
| `Assets/` | サンプルモデル、テクスチャなど |
| `Libraries/` | 外部依存。`Libraries/NRD/Shaders` はサードパーティ側の構成なので移動対象外 |
| `Tools/` | DXC などの開発補助ツール |
| `Samples/` | `PBRApp`、`RayMarchApp`、`FluidApp` などのサンプルアプリ |
| `Build/bin/` | 現行プロジェクトの実行ファイル・ライブラリ出力。コミット対象外 |
| `Build/obj/` | 現行プロジェクトの中間生成物。コミット対象外 |
| `x64/` | 旧 Visual Studio 出力が残る場合のディレクトリ。コミット対象外 |

## Architecture

主要な依存方向とフレームデータの流れは次のとおりです。

```text
Sample App (IApplication)
  -> ApplicationCore / SObject / ECS
  -> RenderProxy / SkinnedRenderProxy
  -> SceneSynchronizer -> SceneSubmitter
  -> RendererFrameCoordinator / RenderFrameOrchestrator
  -> RenderPassRegistry -> RenderGraph
  -> IRHIDevice / IRhiCommandEncoder
  -> DX12 | Vulkan | DX11 | OpenGL
```

- `ApplicationCore` はウィンドウ、入力、アプリライフサイクル、オブジェクト/ECS、起動処理を管理します。`IApplication::OnInit/OnUpdate/OnRender/OnShutdown` がサンプル側の拡張点です。
- `SceneSynchronizer` と `SceneSubmitter` はアプリ層のカメラ・静的/スキンドモデルをレンダラーのフレームデータへ変換し、GPU リソースの生成・更新を担当します。
- `RenderPassRegistry` は組み込みパスと追加パスを保持し、`RenderGraph` が read/write 宣言から依存関係、実行順、外部リソース状態遷移、graphics/compute 実行を組み立てます。
- `IRhiDevice` は中立な下位 RHI、`IRHIDevice` は DX12 feature path との互換面を残す移行中の上位インターフェースです。完全な feature path は DX12、他バックエンドは native fallback path です。

## Implemented Feature Summary

| Area | Current implementation | Backend scope |
| --- | --- | --- |
| Raster | Deferred opaque PBR (5 GBuffer + depth) + forward transparent PBR、weighted-blended OIT | DX12 feature path |
| Geometry | Static/skinned mesh、tessellation、mesh shader/meshlet debug | DX12。static/skinned の簡易描画は Vulkan/DX11/OpenGL にも対応 |
| Shadows | Directional CSM/VSM、spot 最大8灯、point 最大4灯×6面、screen-space contact shadow、SWRT directional shadow | DX12 feature path |
| AO | Material AO、SSAO + blur、SWRT AO、Hybrid、direct-light micro-shadowing | DX12 feature path |
| Reflections | SSR、SWRT reflection、temporal/A-Trous denoise、IBL fallback | DX12 feature path |
| Ray tracing | CPU SAH BVH + GPU SWRT、legacy/ReSTIR 経路、DXR BLAS/TLAS/pipeline/SBT/dispatch | DX12。Vulkan は RHI RT primitives + opt-in smoke test まで |
| GI | SWRT で更新する L2 SH irradiance probe grid、有限 bake、可視性近似、sidecar cache、debug probe spheres | DX12 feature path |
| Environment | HDR/LDR skybox、IBL、procedural sky、volumetric cloud | DX12 feature path（native fallback は簡易機能） |
| Simulation | GPU heightfield fluid surface、GPU billboard particle system | DX12 feature path |
| Runtime | Progressive Boot、Job System、非同期アセットロード、shader/mesh/GI cache、profiling、debug remote control | 共通基盤。GPU 機能は backend caps でゲート |

## Build

Visual Studio 2022 で `SasamiRenderer.sln` を開き、`x64` + `Debug` または `Release` を選んでビルドします。

Developer Prompt からの例:

```bat
msbuild SasamiRenderer.vcxproj /p:Configuration=Debug /p:Platform=x64
msbuild SasamiRenderer.vcxproj /p:Configuration=Release /p:Platform=x64
```

`SasamiRenderer.vcxproj` はレンダラーの static library です。実行確認にはソリューションをビルドし、`PBRApp` などのサンプルをスタートアッププロジェクトに指定してください。バックエンド別構成は `Debug_Vulkan` / `Release_Vulkan`、`Debug_DX11` / `Release_DX11`、`Debug_OpenGL` / `Release_OpenGL` です。Vulkan 構成には `VULKAN_SDK` が必要です。出力は `Build/bin/<Platform>/<Debug|Release>/`（非 DX12 はその下の backend サブディレクトリ）に生成されます。

必要環境は Windows 10/11 SDK、C++20 対応 MSVC、DXC です。D3D12 Debug Layer / GPU-based validation を使う場合は Windows の「Graphics Tools」もインストールしてください。

Debug では D3D12 Debug Layer が既定で有効です。GPU-based validation（GBV）は初回フレームでの全シェーダ計装により数十秒の一回性ストールを生むため既定でオフになり、環境変数 `SASAMI_GPU_VALIDATION=1` を設定した場合のみ有効化されます。

## Progressive Boot（起動の非同期・段階的初期化）

起動時のフリーズ（Windows「応答なし」）を解消するため、初期化はメッセージポンプを止めずに段階的に実行されます（2026-08-20 導入）。

- **即時ウィンドウ表示**: `ApplicationCore::Run()` はウィンドウ表示後すぐメッセージループに入り、`StartupCoordinator`（`Source/AppFramework/Boot/`）がブートフェーズ（`CreatingRendererCore → LoadingWithBootScreen → SceneReady → Running`）を駆動します。
- **初期化タスクの並列/時分割実行**: `InitTaskScheduler` + `MainThreadTaskPump`（`Source/Foundation/Boot/`）が初期化タスクを依存関係つきで管理し、バックエンドの `supportsThreadedResourceCreation`/`supportsThreadedPipelineCreation` capsに応じて JobSystem ワーカーまたはメインスレッド時分割（ブート中100ms/フレーム、シーン表示後4ms）で実行します。SWRT/GI/流体/パーティクル/DXR は `RendererReadyState` のreadyフラグ公開後にフレームグラフへ順次参加します。
- **ローディングUI**: ブート中は `Renderer::RenderBootFrame` + `BootProgressWindow`（ImGui）が項目別進捗を表示し、シーン表示後は未完了タスクのみ右上に小型表示します（ImGuiオーバーレイ非対応バックエンドはタイトルバーにテキスト進捗）。
- **アセットの非同期ロード**: `AsyncAssetLoadService`（`Source/AppFramework/Loader/`）が glTF/OBJ パース+テクスチャデコード、HDRスカイボックスデコード+IBL事前生成をワーカーで実行します。`StaticModel::LoadModelAsync` / `ApplicationCore::LoadSkyboxAsync` で利用し、完了したアセットはフレーム側で自動採用されます（`MeshComponent::MeshLoadState` でポーリング可能）。
- **SWRT BVH の非同期ビルド**: `GpuSoftwareRayTracer::UpdateScene` はシーンスナップショットを取ってワーカーでSAHビルド（メッシュ単位 `ParallelFor` 並列）し、完了後の呼び出しでGPU公開します。ビルド中は旧BVHで描画継続（初回はSWRT系パスがフォールバックにスキップ）。
- **DXR AS の遅延構築**: DXRのBLAS/TLAS再構築はシーン提出時ではなく、ハードウェアRTを実際にディスパッチする直前に遅延実行されます（未使用時はコストゼロ）。
- **シェーダキャッシュの全面適用**: 従来 `RenderPipelineStateCache` のみだった `.cso` ディスクキャッシュを、SWRT/GI/流体/パーティクル/DXRライブラリのアドホックDXCコンパイル約22本にも適用（`ShaderCompilationService::GetOrCompileShaderBytecodeDxc`）。ウォーム起動ではランタイムコンパイル0本になります。
- **スカイボックス cubemap の GPU 生成**: `SkyCubemapGenerator`（`Source/Renderer/Scene/`）が equirect HDR → cubemap 変換とミップ連鎖構築を2本のコンピュートシェーダ（`Shaders/Compute/Sky/SkyCubemapFromEquirect_CS.hlsl` / `SkyCubemapDownsample_CS.hlsl`）で行います。face 2048・6面・RGBA16F・1x1までのミップ連鎖で `Skybox::UploadHdrSkyboxTexture` が **342.8ms**（CPU 版 `RendererMathUtility::GenerateSkyCubemapFromEquirect` は 9925ms）。失敗時は CPU パスへフォールバックし、失敗はラッチして毎フレーム再試行しません（成功時はラッチしないので環境マップの再読み込みでも GPU パスを使います）。**必要な SRV ディスクリプタは24個で、枯渇するとフォールバックして無言で約14.6秒かかります**（下の Descriptor Heap Budget 参照）。
- **テクスチャの重複排除と一括アップロード**: `CpuTexturePathCache` が同一パスのCPUデコードを1回に統合（Sponzaで103回→47ユニーク）し、`SceneSubmitter` はテクスチャを1コマンドリストへ一括記録して単一submit+フェンス遅延解放で転送します（従来はテクスチャ1枚ごとに `WaitForGPU`）。

実測（Debug/DX12・PBRApp・`IsHungAppWindow` による60秒サンプリング）: ウィンドウ表示0.5秒、「応答なし」サンプル 0/118（導入前は初期化完了まで約15秒無応答+初回フレーム約30秒ストール）。

## Render Path

不透明ジオメトリは**デファード**、透明は**フォワード**で描画する単一パイプラインです（2026-08-23 に整理）。描画方式を切り替えるモードはありません。

```
Shadow → OpaqueGBuffer → RuntimeAO → RuntimeAOBlur → Lighting   ← 不透明: デファード
       → ScreenSpaceReflection → SoftwareReflection → Composite → Skybox
       → TransparentBackfaceDistance → TransparentSceneColorCopy
       → TransparentLighting → TransparentComposite              ← 透明: フォワード(OIT)
       → PostProcess
```

- **不透明（デファード）**: `OpaqueGBufferRenderPass` が MRT で Albedo / Normal / Material / Emissive / SpecularWorkflow の5枚 + SceneDepth を1パスで書き、`LightingRenderPass` がフルスクリーンでライティングして SceneColor を生成します。
- **透明（フォワード）**: `TransparentLightingRenderPass` が実ジオメトリをラスタライズしてピクセル単位で PBR シェーディングし（`TransparentOIT_PS.hlsl` が `CookTorranceGGX_PS.hlsl` を include）、weighted-blended OIT の accum/revealage に出力。`TransparentCompositeRenderPass` が SceneColor へ合成します。深度は GBuffer パスが書いた SceneDepth を深度書き込み無しでテストするため、不透明による遮蔽が正しく効きます。

以前は `rasterShaderMode` による「PBRデファード / アンリット」の排他2択があり、透明描画の実装までそのトグルで入れ替わっていました。アンリット経路（`OpaqueRenderPass` / `Opaque_*.hlsl` / 関連PSO）は削除済みです。

**注意**: `RenderPassRegistry` の組み込みパス配列は `RenderPassType` の値をそのままインデックスとして使う位置依存配列です。列挙子を退役させる場合は要素を削除せず `nullptr` でスロットを維持してください（詰めると以降のパスが全てズレます）。

## Asset Cache

モデルのパース・変換結果は `<exe>/AssetCache/*.smesh` にバイナリキャッシュされ、2回目以降の起動では読み込むだけになります（2026-08-23 導入）。実測で **ufbxパース1.8秒 + メッシュ変換7.3秒（計約9.2秒）が消滅**します。

- キャッシュは `LoadStaticModel`（`ModelLoader.cpp`）の層にあり、全フォーマットが正規化された `std::vector<LoadedStaticMesh>` を保存するため **FBX/glTF/OBJ すべてに効きます**
- 失効判定は元ファイルのサイズ・更新時刻・`uniformScale`・フォーマット版。`Vertex`/`SurfaceMaterial` の `sizeof` をフォーマット版に畳み込んであるため、構造体を変更すると古いキャッシュは自動的に無効になります
- 書き込みは一時ファイル + rename、読み込みは全カウントを残バイト数と照合してから確保するため、中断や破損は「再パースにフォールバック」で吸収されます
- `AssetCache/` は `.gitignore` 済み。強制的に作り直したい場合はディレクトリごと削除してください

プロジェクト内の永続キャッシュはすべて同じ「一時ファイルへ書いてから rename」で書き込みます（2026-08-23 統一）。中断・クラッシュ時に既存の正常なキャッシュが壊れたり、半端なファイルが残ったりしないためです。

| キャッシュ | 実装 |
| --- | --- |
| メッシュ (`AssetCache/*.smesh`) | `StaticMeshCache::Save` |
| GIプローブ (`<app>.gi_probe_cache.bin`) | `Renderer::SaveGIProbeCache` |
| コンパイル済みシェーダ (`*.cso`) | `GetOrCompileShaderBytecodeDxc` |

`.cso` はワーカースレッドから同一シェーダが並行コンパイルされうるため、一時ファイル名にスレッドIDを含めます。失効判定がタイムスタンプ比較のみで内容検証をしないので、裂けた blob が次回起動で正常なバイトコードとして読まれるのを防ぐためです。

## Asset Formats

| 形式 | 対応 | 実装 |
| --- | --- | --- |
| glTF / GLB | 静的・スキンドメッシュ。外部 `.bin` と GLB 埋め込み BIN chunk に対応 | `ModelLoader.cpp` / `ModelLoaderSkinned.cpp` / `ModelLoaderGlb.cpp`（rapidjson） |
| OBJ | 静的メッシュ | `ModelLoaderOBJ.cpp` |
| FBX | 静的メッシュ（2026-08-23 追加） | `ModelLoaderFbx.cpp` + `Libraries/ufbx`（MIT） |
| PNG / JPG 等 | WIC 経由で RGBA8 | `AssetLoader::LoadRgba8ViaWIC` |
| DDS（BC1/2/3/4/5/7、DX10拡張、非圧縮32bpp） | RGBA8 へデコード（2026-08-23 追加） | `DdsTextureLoader.cpp` + `Libraries/bcdec`（MIT） |
| HDR (equirect) | スカイボックス・IBL | `LoadSkyboxAsync` |

FBX/DDS 対応は Amazon Lumberyard Bistro（`Assets/Models/Bistro/`）を読み込むために追加しました。`ModelLoaderFbx` は ufbx のシーンを既存の `LoadedStaticMesh` へ正規化するため（メッシュ×マテリアルパート×ノードインスタンス単位に分割、右手 Y-up・メートル単位へ変換、頂点値ベースの重複排除）、下流のレンダラ・非同期ロード基盤は形式非依存のまま動きます。テクスチャは拡張子で `CpuTexturePathCache` が WIC と DDS を自動的に振り分けます。

既知の制限:

- **FBX のみ同期ロード**です。ワーカースレッドで ufbx を実行するとプロセスが数秒後に終了する未解明の問題があり（例外は捕捉されず、メインスレッドでは安定）、原因究明までは PBRApp で `LoadModel` を使っています。Bistro のパース中（約8.5秒）はウィンドウが応答しません。
- DDS は最大 1024px のミップにデコードします（`kMaxDecodedTextureDimension`）。Bistro の 2048px テクスチャ337枚を原寸展開すると CPU/GPU 合わせて 10GB を超えるためで、本来は BC 圧縮のまま GPU へ上げるべき箇所です。
- 法線マップは `LoadedStaticMesh` に受け皿が無いため読み飛ばします。

## Sample Apps

`Samples/` 配下、各アプリは `.vcxproj` を repo root に持ち、`SasamiRenderer.sln` からスタートアッププロジェクトとして選択できます。

| App | 内容 |
| --- | --- |
| `PBRApp` | メインの検証用サンプル。PBR shading、Particle System、GI、SSR/SWRT reflection、スキンメッシュアニメーションなど、実装済み機能の大半をここでトグルできます（Fluid Surface は `FluidApp` に分離）。 |
| `RayMarchApp` | `CameraMode::RayMarch` を使った専用レイマーチパスのサンプル。 |
| `FluidApp` | Fluid Surface (V1) 専用の最小サンプル。フロアに合わせて `FitFluidToSceneBounds` でグリッドを配置し、既定で表示を有効化。ImGui パネルから Wave Speed / Damping の調整と Splash 注入ができます。 |
| `NeuralShaderApp` | 将来の Neural Shader 機能向けの最小スキャフォールドアプリ。既定の PBR パイプライン + カメラ + 単一プリミティブのみで、機能トグルはまだありません。 |
| `PhotorealAnimeApp` | 将来のフォトリアル・アニメ調環境向けの最小スキャフォールドアプリ。構成は `NeuralShaderApp` と同じで、機能トグルはまだありません。 |

`FluidApp`/`NeuralShaderApp`/`PhotorealAnimeApp` はいずれも `UseDefaultRenderNodePreset()` を使う標準 PBR パイプラインのサンプルで、`RayMarchApp` のようなカスタム `RenderPass`/`CameraMode` は持ちません。

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
| DirectX 12 | 主実装。feature render path、RenderGraph、GBuffer、PBR、SSR、SWRT、DXR、GPU heightfield 流体サーフェス (V1) 周辺を検証対象にしています。RHI 抽象化層 (BLAS/TLAS ビルド、RT パイプライン、SBT) の DX12 実装完了。 |
| Vulkan | native fallback path。clear/present、static mesh (Y-flip viewport、albedo texture、DX-compatible projection)、skinned mesh (`NativeMeshSkinned.hlsl` 4ボーン線形ブレンドスキニング、bone matrices は `register(b0, space1)` dynamic UBO、最大128本×同時16描画スロット)、D32 depth test、swapchain resize、RGBA8 texture upload、ray march (`RayMarchApp`)、compute dispatch を実装済み。拡張機能はランタイムで検出 (VK_KHR_dynamic_rendering / descriptor_indexing / timeline_semaphore / ray_query / ray_tracing_pipeline / acceleration_structure)。BLAS/TLAS ビルド (BuildRhiBlases / BuildRhiTlas)、RT パイプライン (CreateRhiRayTracingPipeline)、SBT (CreateRhiShaderBindingTable)、trace dispatch (command encoder の DispatchRays / vkCmdTraceRaysKHR) を VK_KHR_ray_tracing_pipeline で実装済み。RT パイプラインは `RhiRayTracingPipelineDesc::spirvBytecode` に SPIR-V を渡す (DX12 は並列の DXIL フィールドを使用)。ただし DxrRayTracer と render graph は DX12 専用のため、これらの RHI プリミティブはまだ Vulkan の描画パスには結線されていません。DX12 feature path と同等ではありません。 |
| DirectX 11 | native fallback path。clear/present、static mesh、skinned mesh（HLSL row_major スキニング頂点シェーダー、`JOINTS_0`/`WEIGHTS_0` 頂点属性、ボーンパレット定数バッファ 最大128本）、depth test、swapchain resize、RGBA8 texture upload、ray march (`RayMarchApp`)、compute dispatch を実装済み。DX12 feature path と同等ではありません。 |
| OpenGL | native fallback path。clear/present、static mesh (行列の行列の列優先変換修正済み)、skinned mesh (GPU スキニング頂点シェーダー、ボーンパレット uniform 配列 最大128本)、depth test、window resize、albedo texture sampling、ray march (`RayMarchApp`)、compute dispatch を実装済み。DX12 feature path と同等ではありません。 |

Vulkan / DirectX 11 / OpenGL では、メインループが `OnUpdate` / `OnRender` を経由して `SyncModelsToRenderer` と `UpdateCameraCB` を正しく呼び出すため、DX12 と同一のゲームループパスを使用します。これらのバックエンドは DX12 feature pass ベースの RenderGraph には未対応です。

RayMarch バックエンド対応について: Vulkan は DXC で HLSL を SPIR-V にコンパイルして `RayMarch_VS.hlsl` / `RayMarch_PS.hlsl` を再利用します。OpenGL は GLSL 330 core で同等のシェーダーをインライン実装しています。いずれも `RhiBackendRayMarchFrameDesc` 経由で `ExecuteBackendFrame` から呼び出されます。

Vulkan RT インフラについて: GPU が RT 拡張機能をサポートしている場合、`GetCapabilities().supportsHardwareRayTracing`/`supportsRayQuery` が true になります。BLAS/TLAS 構築に加え、RT パイプライン (`CreateRhiRayTracingPipeline`)、SBT (`CreateRhiShaderBindingTable`)、trace dispatch (`VulkanRhiCommandEncoder::DispatchRays` → `vkCmdTraceRaysKHR`) を実装済みです。SPIR-V は `RhiRayTracingPipelineDesc::spirvBytecode` で供給し (DX12 の DXIL フィールドと並列、非破壊)、パイプラインは内部固定の descriptor set layout (binding 0 = acceleration structure、binding 1 = storage image) を構築します。SPIR-V の各 export は OpEntryPoint の execution model からステージを解決するため、追加のステージメタデータは不要です。

このパスは `RunRayTracingSmokeTest` で検証できます。環境変数 `SASAMI_VK_RT_SMOKETEST=1` を設定して Vulkan 構成の `PBRApp` を起動すると、初期化時に三角形 1 枚の BLAS/TLAS を構築し、`VulkanRtSmokeTest.hlsl` を SPIR-V にコンパイルして RT パイプライン + SBT を作成し、64×64 の storage image に 1 ピクセル 1 レイでトレースして、中心ピクセル = 赤 (hit) / 端 = 青 (miss) を読み戻し検証します。結果は `PBRApp.exe.log` に `Vulkan RT smoke test: PASS` として記録されます。

制限事項: `DxrRayTracer` と render graph の RT パス、および DXR シェーダの `ResourceDescriptorHeap` bindless モデルは DX12 専用です。これらの RHI プリミティブを Vulkan の描画パスに結線し、実画面へ HW RT を出力するには、Vulkan 互換の binding モデルを持つシェーダ改修を含む追加作業が必要です。

## Debug Remote Control

実行中のアプリを外部プロセス（AI含む）から操作するためのデバッグ用チャンネルです。**既定では無効**で、環境変数 `SASAMI_DEBUG_REMOTE=1` を設定した起動時のみ有効になります。

- **通信方式**: Windows名前付きパイプ `\\.\pipe\SasamiRenderer.Debug`、同時接続1クライアント、overlapped byte-stream I/O。TCP port は開きません。現実装は `PIPE_REJECT_REMOTE_CLIENTS` や明示的な security descriptor を指定していないため、OS/共有設定をまたいだ厳密な「ローカル限定」は保証しません。信頼できない環境では有効化しないでください
- **プロトコル**: 改行区切りのテキスト。1行のリクエストに1行のレスポンスを返し、改行を含む handler 結果は空白へ sanitize します。失敗は `ERR <理由>`、通常の変更成功は `OK`、`ping` は `pong`、`help` はコマンド一覧そのものを返します
- **スレッドモデル**: 受信スレッドが行を受け取り `DebugCommandQueue` 経由でメインスレッドへ渡し、`ApplicationCore::OnUpdate()` が実行して結果を返します。**シーンやカメラを受信スレッドから直接触らない**ための構造です

| ファイル | 責務 |
| --- | --- |
| `IDebugTransport.h` | 1行受け取り1行返す抽象。**通信方式を知るのはこの実装のみ** |
| `DebugNamedPipeTransport` | 名前付きパイプ実装（オーバーラップドI/O + 停止イベント） |
| `DebugCommandQueue` | 受信スレッド→メインスレッドの同期受け渡し |
| `DebugCommandRegistry` | コマンド名→ハンドラの対応表とパース・振り分け |
| `DebugRemoteControlServer` | 上記を束ねる窓口（`Start`/`Stop`/`DrainPendingCommands`） |
| `DebugSceneCommands` | 実際のコマンド定義（`ApplicationCore` に対して登録） |
| `DebugCaptureCommands` | スクリーンショット系のコマンド定義。シーン操作とは責務が別なので分離 |

現在のコマンド:

| Command | Purpose |
| --- | --- |
| `ping` | 疎通確認。`pong` を返す |
| `help` | 登録済みコマンドを名前順で列挙 |
| `camera.get` | カメラ target / yaw / pitch を取得 |
| `camera.setTarget <x> <y> <z>` | カメラ target を変更 |
| `camera.setYawPitch <yaw> <pitch>` | カメラ姿勢を変更。**単位はラジアン**（度ではない） |
| `camera.setDistance <d>` | orbit distance を変更 |
| `light.spot.list` | 全 spot light の位置、姿勢、range、cone、intensity を取得 |
| `scene.save <path>` | シーンを保存。空白を含むパスも残りの引数を連結して扱う |
| `scene.load <path>` | シーンを破壊的にロード。空白を含むパスに対応 |
| `debug.screenshot <path>` | バックバッファのキャプチャを予約。`OK queued <絶対パス>` を返す |
| `debug.screenshot.status` | 予約したキャプチャの完了確認。完了まで `PENDING`、完了後に `OK <パス>` |
| `render.gputime` | パス単位の GPU 実行時間（ミリ秒）と合計を取得。結果は `kFrameLatency`(=3) フレーム遅れる |

TCP実装は `IDebugTransport` を実装したクラスを1つ足すだけで追加できます（キュー・レジストリ・コマンド層の変更は不要）。

検証状況: 実機で名前付きパイプ越しにコマンドを送り、応答とスクリーンショットの出力まで確認済みです。

### トーンマップの定数バッファ

`Renderer::ToneMapSceneColor` は `ToneMapCB`(b0 / ルートパラメータ2) を**必ずバインドする**こと。
`ToneMap_PS.hlsl` は `u_toneMapParams` をオフセット128から読むため、バインドを省くと直前の描画が残した
128バイトのカメラCBの**範囲外**を読み、露出がゴミ値になる。これは「画面が真っ黒になる」「露出スライダーが効かない」
という形で現れる（2026-08-30 に修正済み）。カメラCBレイアウトの `extra0` がちょうどオフセット128に当たるので、
`PushCameraCB(frame, mvp, world, extra0, ...)` の `extra0` に露出を積めばよい。

### GPU パス別プロファイリング

`render.gputime` は D3D12 タイムスタンプクエリでレンダーグラフのパスごとの GPU 時間を返します。

```
> render.gputime
< OK Shadow_0=27.55 OpaqueGBuffer_1=7.07 SSAO_2=0.16 ... | total=35.01
```

実装は `Source/Renderer/Profiling/GpuTimestampProfiler.{h,cpp}`（クエリヒープ、`kFrameLatency=3` のリング
バッファ、リードバックの常時 Map）。レンダーグラフ側は `RenderGraphExecuteContext::gpuTimestampProfiler` に
プロファイラを受け取り、`RenderGraph::Execute` が各パスを RAII で挟みます。フレーム境界（`BeginFrame` /
`EndFrame` / `UpdateResults`）はグラフからは見えないため `Renderer` が管理します。

GPU はコマンドを非同期に実行するので、結果は**記録中のフレームより 3 フレーム遅れます**。したがって起動直後の
数回の問い合わせは `PENDING` を返します。この `PENDING` 応答には診断値（`scopesThisFrame` / `frameCounter` /
`askedFrame` / `slotHeldFrame` / `slotValid`）が含まれており、「初期化に失敗して永久に出ない」のか
「レイテンシ窓が埋まっていないだけ」なのかを推測せずに切り分けられます。初期化自体に失敗した場合は
`PENDING` ではなく `ERR` を返します（プロファイラが無効でも描画には影響しません）。

## Screenshot / Headless（描画結果の外部確認）

ウィンドウの配置・重なり・他アプリの状態に影響されずに、**実際に描画された絵**を PNG で取り出す仕組みです。`Tools/Debug/ss.ps1` の `PrintWindow` 方式と違い、ウィンドウ枠もタイトルバーも入らず、DWM 合成やウィンドウの重なりの影響も受けません。

```powershell
# 起動（デバッグパイプを有効化）
Build\run-debug.bat
# ヘッドレス（ウィンドウを一切表示しない）
Build\run-debug.bat 1

# 撮影（予約→ポーリング→パス出力までを1コマンドで隠蔽）
powershell -ExecutionPolicy Bypass -File Build\screenshot.ps1 -Path Build\screenshots\shot.png
```

| 要素 | 責務 |
| --- | --- |
| `Renderer/Capture/BackBufferCapture` | リードバックへの `CopyTextureRegion` と WIC による PNG エンコード |
| `Renderer/Runtime/RendererScreenshot.cpp` | 予約状態の管理と、フレーム送信の前後へのフック |
| `AppFramework/.../DebugCaptureCommands` | `debug.screenshot` / `debug.screenshot.status` の定義 |
| `Tools/Debug/screenshot.ps1` | 予約とポーリングを隠す CLI ラッパ |
| `Tools/Debug/run-debug.bat` | デバッグパイプ有効（＋任意でヘッドレス）での起動 |

**WSL から起動する場合の注意**: WSL の interop 経由で起動したプロセスは、**起動元のコマンドが終了した時点でツリーごと終了させられます**。`cmd.exe` の `start` や `Start-Process` では切り離せません。WSL 側から常駐させたい場合は、interop のツリーに属さない形で作る必要があります:

```bash
powershell.exe -NoProfile -Command "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = 'cmd.exe /c D:\Git\SasamiRenderer\Build\run-debug.bat'; CurrentDirectory = 'D:\Git\SasamiRenderer' }"
```

Windows 側から直接 `run-debug.bat` を叩く場合はこの問題は起きません。

実装上の要点:

- **行ピッチ**: `GetCopyableFootprints` が返す `RowPitch` は `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`(256) に切り上げられており、`width * 4` とは一致しません。詰め直さずに書くと絵が斜めにずれます
- **色順**: `R8G8B8A8` / `B8G8R8A8`（各 `_SRGB` 含む）のみ受理し、内部で WIC が要求する BGRA に正規化します。想定外のフォーマットは**黙って変な色で保存せず失敗**させます
- **GPU 同期**: コピーは `Renderer::SubmitAndPresent` の**送信前**に記録し、PNG は**送信後に `WaitForGPU()` してから**書き出します。待たずに Map すると壊れた絵やティアリングした絵になります
- **合流点がひとつ**: メインのフレームグラフ、グラフ失敗時の2つのフォールバック、`RenderBootFrame` のいずれの経路も `SubmitAndPresent` を通るため、どの経路で組まれたフレームでも撮れます
- **2コマンド構成の理由**: ハンドラはメインスレッドで実行され、キャプチャの完了も同じスレッドの後続フレームでしか起きません。ハンドラ内で完了を待つとレンダーループごとデッドロックするため、予約とポーリングを分けています
- **ヘッドレス**: `SASAMI_HEADLESS=1` で `SW_HIDE` 起動します。最小化(`SW_MINIMIZE`)は Present が抑制されうるため使いません。非表示でもフレームは実行されバックバッファは埋まるので、キャプチャは成立します

## Shadow System

DX12 feature path は directional / spot / point のラスタシャドウと、任意の SWRT directional shadow を持ちます。

- **Directional**: 1 または4 cascade の CSM/VSM。CSM はテクセル単位で回転を固定した16-tap Poisson PCF、cascade 境界を blend します。VSM は depth moments と任意の separable Gaussian blur を使います。VSM の skinned moment 出力は未実装です
- **Spot**: 先頭最大8灯を `Texture2DArray` の個別 slice に描画し、比較サンプラによる3x3 PCFで参照します
- **Point**: 先頭最大4灯を各6面、合計最大24 slice の `Texture2DArray` に描画し、比較サンプラによる3x3 PCFで参照します。面規約は `0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z` です
- static mesh と skinned mesh は directional CSM、spot、point の深度描画に参加します。VSM の色出力は static mesh のみです
- 静的サンプラ `s1` = `MakeShadowComparisonSampler()`（`LESS_EQUAL` / `CLAMP` / `COMPARISON_MIN_MAG_LINEAR_MIP_POINT`）は通常・deferred・skinned の3ルートシグネチャに登録されています
- シェーダ側は `SampleCmpLevelZero()` を使い、**深度を比較してから平均**します。深度マップを線形サンプラで読んで後から閾値処理すると、遮蔽ではなく深度値を補間することになり PCF になりません
- `CLAMP` アドレッシングにより、PCFカーネルの端のタップがシャドウマップ（ポイントライトはキューブ面）の反対側へ回り込みません
- スポット/ポイントのシャドウマップは `R16_TYPELESS`/`D16_UNORM` で作り、SRV は `R16_UNORM`。プレースホルダSRVも同じフォーマットに揃えてあります
- 実装は `Shaders/Raster/Lighting/PBR/PBR_Shadow.hlsli` に集約し、forward と deferred で共有します

検証状況: 全プロジェクトのビルドと単一シーンの連続描画は確認済みです。複数の spot/point light を同時配置した最終的な影の目視比較は未実施です。

## Render Pipeline

既定パス順:

```text
Shadow -> OpaqueGBuffer -> RuntimeAO -> RuntimeAOBlur -> Lighting -> ScreenSpaceReflection -> SoftwareReflection -> SoftwareReflectionComposite -> Skybox -> TransparentBackfaceDistance -> TransparentSceneColorCopy -> TransparentLighting -> TransparentComposite -> PostProcess
```

主なパス:

- `Shadow`: directional CSM/VSM、spot 最大8灯、point 最大4灯×6面を描画します。spot/point は各ライトに `shadowIndex` を割り当て、上限を超えたライトは影なしで照明計算を継続します。forward (`CookTorranceGGX_PS.hlsl`) と deferred (`DeferredLighting_PS.hlsl`) は同じ shadow helper を使います
- `RuntimeAO` / `RuntimeAOBlur`: SSAO または runtime AO。間接光(IBL/GI)への適用に加え、`AO Direct Lighting`(`aoDirectLightingStrength`, 0〜1, 既定 0.5)で直接光にもライトごとの遮蔽を適用する。適用はマイクロシャドウイング(Chan 2018 "Material Advances in Call of Duty: WWII"、Filament 採用式。AO を可視コーンの開口とみなし NdotL に応じて減衰)で、シャドウマップの visibility に追加乗算される。さらに point/spot 光には光源方向へのスクリーンスペースコンタクトシャドウ(UE Contact Shadows / Filament punctual 相当。レイ長は光源距離でクランプ、コンタクトシャドウ設定でゲート)を適用。PBRApp の AO 設定 UI と settings ini に永続化される。同じ `aoDirectLightingStrength` はレイトレース DI にも適用される: DXR パス（RayTracing.hlsl）は directional/point/spot の直接光にマテリアル AO 由来のマイクロシャドウイングを乗算し、SWRT ReSTIR DI パス（SWRT_ReSTIR_Shade_CS）は CPU で焼き込んだマテリアル実効 AO（occlusion テクスチャ R 平均を strength で lerp、hitPosition.w に metallic と 5bit+5bit パック）を反射ヒット面の直接光マイクロシャドウイングとアンビエント減衰に使う（マイクロシャドウ式は `Shaders/Shared/Common/MicroShadowing.hlsli` に共通化）。EmissiveColor もこの `ao` を乗算してから最終合成する(`CookTorranceGGX_PS.hlsl`/`DeferredLighting_PS.hlsl` の shading 出力側のみ。GBuffer emissive チャンネルは生値のまま書き、ディファードのフルスクリーン合成側で一度だけ `ao` を掛けるため二重適用にはならない)
- `OpaqueGBuffer`: opaque mesh から GBuffer と depth を出力
- `Lighting`: GBuffer/depth/light/shadow/IBL/AO を読む fullscreen deferred lighting combine。`SceneColor` を生成
- `ScreenSpaceReflection`: Lighting 後の `SceneColor` をコピーし、compute shader で SSR radiance/confidence を生成
- `SoftwareReflection` / `SoftwareReflectionComposite`: SWRT reflection。SWRT が有効な場合は SSR より優先
- `Skybox`: sky / procedural sky
- `Transparent*`: transparent backface distance、scene color copy、weighted blended OIT、transparent lighting/composite
- `PostProcess`: tone mapping など

### スケルタルメッシュ / アニメーション再生

- text glTF + 外部 `.bin` と GLB + 埋め込み BIN chunk のスキン・アニメーションを読み込みます。フォーマット判定は拡張子ではなく GLB magic を確認します
- `ApplicationCore::CreateSkinnedModel()` → `SkinnedModel::LoadModel()` で生成し、`PlayAnimation(index)` でクリップを再生します。ロード時は clip 0 を自動再生します
- DX12 は `SkinnedMesh_VS.hlsl` とボーンパレット CB `b3`（最大128本）で GPU skinning し、CSM、spot、point shadow を描画します。VSM moment 出力だけは未対応です
- Vulkan / DX11 / OpenGL native fallback も最大128本・4 bone linear blend の簡易 skinned mesh 描画に対応しますが、shadow / IBL / feature render path と同等ではありません
- VB/IB は `meshId` 差分時のみ再アップロードし、置換時は GPU 完了後に旧バッファを解放します。NORMAL がない glTF は面積重み付き面法線から頂点法線を生成します
- `Skeleton` は bind pose の local TRS を保持します。animation track がない成分は identity ではなく bind pose を維持します
- PBRApp は `Assets/Models/Fox` の Survey / Walk / Run 3 clip をサンプルとして使用します。現在の主な制限は `skin[0]` のみ、skinned model の非同期ロード未対応です

## Screen Space Reflection

SSR は `CookTorranceGGX_PS.hlsl` 内の inline ray march ではなく、専用 render pass と compute shader に分離しています。

実装ファイル:

- `Source/Renderer/Passes/Reflections/ScreenSpaceReflectionRenderPass.*`
- `Shaders/Effects/Reflections/SSR/ScreenSpaceReflection_CS.hlsl`
- `Source/Renderer/Resources/RenderPipelineStateCacheSsr.cpp`

現在の挙動:

- Lighting 完了後の `SceneColor` を `SSRSceneColorCopy` にコピーします。
- `SceneDepth`、`GBufferNormal`、`GBufferMaterial`、`SSRSceneColorCopy` を compute shader で参照します。
- world-space position を `cameraInvPV` から復元し、反射方向を screen-space に投影して depth hit を探索します。
- 出力 `SSRReflection` は RGB に reflected radiance、A に confidence を格納します。
- 既存の reflection composite shader で `SSRReflection` を `SceneColor` に加算合成します。
- SWRT reflection が有効な場合、SSR は実行しません。
- レイが画面外に出た、または深度バッファとの hit が見つからずステップ予算を使い切った場合、IBL が有効なら prefiltered cubemap (`IblSystem::PublishIblSrvs` の prefilter SRV) をフォールバックとしてサンプルします。mip は `saturate(roughness) * iblPrefilterMaxMip`（SWRT reflection の `SampleReflectionEnvironment` と同じロジック）、強度は `iblIntensity`。IBL が無効な場合は従来どおり RGBA 0 を出力します。
- `ReflectionRadiance` debug view は SSR raw radiance、`ReflectionAlpha` debug view は SSR confidence alpha を表示します。
- `PhaseTag()` は `"Scene"` を返します。他の scene pass と同フェーズに属さないと render graph の位相ソートが循環グラフと誤判定します。

既知の制限:

- hierarchical Z、temporal accumulation、stochastic sampling、roughness mip blur は未実装です。
- 視線とほぼ平行な法線（back-facing）、または反射方向が視線側を向く縮退ケースはレイマーチ自体を行わず、IBL フォールバックの対象外として RGBA 0 を返します。
- 現状は DX12 feature render path 用です。Vulkan / DX11 / OpenGL native fallback では SSR は動作しません。
- 実機での目視検証は未実施です（ビルド検証のみ）。

参考にした実装方針:

- Morgan McGuire and Michael Mara, "Efficient GPU Screen-Space Ray Tracing", JCGT 2014: https://jcgt.org/published/0003/04/04/
- AMD FidelityFX Stochastic Screen Space Reflections: https://gpuopen.com/fidelityfx-sssr/
- Unreal Engine Screen Space Reflections documentation: https://dev.epicgames.com/documentation/unreal-engine/screen-space-reflections-in-unreal-engine

## Fluid Surface (V1)

GPU heightfield ベースの浅水波シミュレーション（Müller-Fischer, GDC 2008 の離散波動方程式手法）を実装しています。既定では無効です。

実装ファイル:

- `Source/Renderer/Fluid/FluidHeightfieldSim.*`: ping-pong デュアルバッファで高さ場を保持し、`Shaders/Compute/Fluid/FluidHeightfield_CS.hlsl` で波動方程式を更新。スプラッシュ注入 (`InjectSplash`)、グリッド原点/セルサイズ/波速/減衰の実行時設定に対応。
- `Source/Renderer/Passes/Fluid/FluidSurfaceRenderPass.*`: `FluidHeightfieldSim` の高さバッファを `Shaders/Fluid/FluidSurface_VS.hlsl` / `FluidSurface_PS.hlsl` で描画。

現在の挙動:

- `Renderer::EnsureFluidSurfacePassInserted()` により `Skybox` パスの直後（フォールバックで `Lighting` の直後）に挿入されます。`UseDefaultRenderNodePreset()` / `SetRenderPassSequence()` / 初期化時に自動で再挿入されるため、プリセットリセット後も残ります。
- シミュレーションは `Renderer::Render()` 内で毎フレーム無条件に更新されます（GI probe bake のような on-demand 処理ではなく常時進行）。更新後のバッファは次フレームから可視になります（1フレーム遅延）。
- `FluidApp` サンプルでは「Fluid」の ImGui パネルから表示トグル・Wave Speed・Damping・Splash ボタンを操作できます。
- `ApplicationCore` は `SetFluidSurfaceEnabled` / `FitFluidToSceneBounds` / `InjectFluidSplash` などのメソッドで `Renderer` の機能をそのまま転送します。

既知の制限（V1 スコープ）:

- 平面サーフェスの heightfield 波動のみ。volumetric / particle ベースの流体力学は未実装です。
- 現状は DX12 feature render path 専用です。
- ピクセルシェーダーにカメラ位置 CB がまだ渡っていないため、true view-dependent specular は未実装です。
- 複数インスタンス・SObject 統合は未対応です（V2 で対応予定、`TODO.md` 参照）。

## Particle System (V1)

GPU 駆動の固定容量パーティクルシミュレーション（単一ファウンテン式エミッタ、重力+線形ドラッグ積分）を実装しています。既定では無効です。

実装ファイル:

- `Source/Renderer/Particles/ParticleSystem.*`: 単一の `RWStructuredBuffer<Particle>`（ping-pong ではない）を `Shaders/Particles/ParticleUpdate_CS.hlsl` で emit + integrate。ホスト側のリングカーソル（`m_emitCursor`）でエミッション予算を管理し、シェーダー側は決定論的なリングウィンドウでスロットを再スポーンします。
- `Source/Renderer/Passes/Particles/ParticleRenderPass.*`: `ParticleSystem` のバッファをカメラ正対ビルボードとして `Shaders/Particles/ParticleBillboard_VS.hlsl` / `ParticleBillboard_PS.hlsl` で描画（頂点/インデックスバッファなし、`SV_VertexID`/`SV_InstanceID` 駆動）。

現在の挙動:

- `Renderer::EnsureParticlePassInserted()` により `FluidSurface` パスの直後（フォールバックで `Skybox` → `Lighting`）に挿入されます。Fluid と同じ自動再挿入の仕組みに従います。
- シミュレーションは `Renderer::Render()` 内で毎フレーム無条件に更新されます（1フレーム遅延で可視化）。
- PBRApp サンプルでは「Particles (V1)」の ImGui パネルから表示トグル・Emission Rate・Gravity・Drag を操作できます。設定は `PBRApp.settings.ini` に永続化されます。
- `ApplicationCore` は `SetParticlesEnabled` / `SetParticleEmissionRate` / `SetParticleGravity` / `SetParticleDrag` / `SetParticleEmitOrigin` などのメソッドで `Renderer` の機能をそのまま転送します。

既知の制限（V1 スコープ）:

- 単一エミッタのみ。複数インスタンス・SObject 統合は未対応です。
- 現状は DX12 feature render path 専用です。
- グリッド原点/セルサイズのような空間分割の概念はありません（Fluid と異なり全パーティクルが単一エミッタから発生）。

参考にした実装方針:

- Matthias Müller, GDC 2008, "Fast Water Simulation for Games Using Height Fields"（離散 2D 波動方程式による heightfield 水面シミュレーション）

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

## Job System

`Source/Foundation/Jobs/` にスレッドベースのワークスティーリング型ジョブシステムを実装しています。ファイバーは使わず、Chase-Lev 循環デック + カウンタベースの fork-join 同期という構成です。現在は Progressive Boot の worker task、`AsyncAssetLoadService` のモデル/skybox decode、SWRT BVH の非同期構築、メッシュ単位の SAH `ParallelFor` で実運用されています。

実装ファイル:

- `Source/Foundation/Jobs/JobTypes.h`: POD な `Job`(関数ポインタ + userdata + `JobCounter*`、ヒープ確保なし)。
- `Source/Foundation/Jobs/JobCounter.h`: C++20 `std::atomic::wait`/`notify_all` による fork-join カウンタ。
- `Source/Foundation/Jobs/WorkStealingDeque.h`: Chase-Lev ロックフリー循環ワークスティーリングデック。`Steal()` は所有ワーカー以外のスレッドからロックフリーに呼べますが、`PushBottom()`/`PopBottom()` はジョブを投入するスレッド(kick 元。ワーカー自身とは限らない)とキュー所有ワーカーのポップ側が異なるスレッドから同時に呼ばれうるため、両者を同一 `std::mutex` で直列化しています(教科書通りの Chase-Lev は Push/Pop を単一の所有スレッドに限定しますが、このジョブシステムでは Kick を任意スレッドから呼べる設計上、素の非アトミック load+store での `m_bottom` 競合がジョブロスト → `JobCounter::Wait()` の永久ハングを引き起こすため)。
- `Source/Foundation/Jobs/JobWorker.h`/`.cpp`: 1 ワーカースレッドのラッパー(`std::jthread`/`std::stop_token`)。自キュー pop → 他ワーカーからの steal → 共有 `std::counting_semaphore` でアイドル待機。
- `Source/Foundation/Jobs/JobSystem.h`/`.cpp`: 公開 API(`namespace SasamiRenderer::JobSystem` のフリー関数)。`Initialize`/`Shutdown`/`Kick`/`KickN`/`Wait`/`KickAndWait`/`GetWorkerCount`。`ApplicationCore` のコンストラクタ/デストラクタで `Profiler` と同様にライフサイクル管理されます。
- `Source/Foundation/Jobs/ParallelFor.h`: `KickAndWait` の上に構築する範囲分割ヘルパー。

検証: `_DEBUG` ビルドでは `ApplicationCore` 初期化直後に `JobSystem::RunSelfTest()` が自動実行され、約1万ジョブの fan-out/fan-in カウンタ検証と `ParallelFor` の書き込み正当性検証を行い、結果を `PBRApp.exe.log` に `[JobSystemSelfTest] ... PASS/FAIL` として記録します。恒久的なリグレッション検知用で、使い捨てのテストコードではありません。

参考にした実装方針:

- Chase & Lev, "Dynamic Circular Work-Stealing Deque" (SPAA 2005)、およびアトミック順序付けの修正版として Lê, Pop, Cohen, Nardelli, "Correct and Efficient Work-Stealing for Weak Memory Models" (PPoPP 2013)
- Christian Gyrling, GDC 2015 "Parallelizing the Naughty Dog Engine Using Fibers"(カウンタベース fork-join 同期パターン)
- Stefan Reinalter (Molecular Matters), "Job System 2.0: Lock-Free Work Stealing" ブログシリーズ
- turanszkij/JobSystem (Wicked Engine)(固定サイズプリアロケート済みジョブプール)

## Descriptor Heap Budget / Shader Signature Notes

`SrvDescriptorAllocator`（`Source/Renderer/Resources/`）は free list を持たない bump allocator で、`Renderer::Initialize` が渡す容量がレンダラー全体の SRV/UAV 上限になります。現在 **4096**（シェーダ可視 CBV/SRV/UAV は 32B/個なので 128KB）。

512 では Bistro で不足しました。シーンのマテリアルテクスチャだけで約 490 を消費し、`SkyCubemapGenerator` が要求する 24 個（equirect SRV 1 + mip UAV 12 + mip SRV 11、face 2048 の場合）が入りきらずに失敗します。この失敗はユーザーからは見えず、`Skybox` が CPU cubemap 生成へ黙ってフォールバックして起動が約 14.6 秒延びるだけなので、枯渇ログには capacity / used / requested を出しています。新しい常設 SRV 消費者を足すときはこの予算を意識してください。

**シェーダモデルの解決**: `ShaderCompilationService::ResolveEffectiveShaderModel()` が `D3D12_FEATURE_SHADER_MODEL` を降順に問い合わせてデバイス上限を決めます。候補表 `kKnownShaderModels` は **ABI 値（0x60〜0x69）を直書きしてキャストします**。`D3D_SHADER_MODEL_6_x` は d3d12.h の **enum メンバーであってマクロではない**ので、`#ifdef D3D_SHADER_MODEL_6_9` のようなガードを付けると常に false になり、表が 6_0 の1要素に潰れて**どんなハードウェアでも 6_0 に落ちます**。これを検出するための `static_assert` を2本置いてあります。ランタイムが知らないモデルを候補に入れるのは安全です（`CheckFeatureSupport` が E_INVALIDARG を返し、降順ループが次を試します）。

**シェーダモデルはビルド時と実行時で対にすること。** MSBuild の `RendererShaderModel`（`SasamiRenderer.vcxproj`）と `GetConfiguredShaderModel()` の既定は**どちらも `6_7`** です。ビルドは `<name>.<entry>.<profile>_<model>.cso` という名前でプリコンパイルし、実行時はその名前で引くので、片方だけ変えると `.cso` が一切ヒットせず全シェーダがランタイムコンパイルに落ちます。また `ResolveEffectiveShaderModel` は要求をデバイス上限まで**引き下げる**ので、対象ハードより高いモデルを要求すると解決後のプロファイル名がビルド生成物と食い違います（`6_9` 要求／デバイス `6_8` で実際に起きていました）。`RENDERER_SHADER_MODEL` 環境変数で両方まとめて上書きできます。

**生の `CommandList` で描くパスは自分で `DebugIncrementDrawCount()` を呼ぶこと。** `CommandList::DrawInstanced` / `DrawIndexedInstanced`（`Source/Renderer/RHI/GraphicsDevice.h`）は素通しの薄いラッパーでカウンタを触りません。カウンタは RenderGraph の `[DrawRange] <pass>: draws [a, b)` ログの根拠なので、呼び忘れると**そのパスが何も描いていないように見えます**（実際にはトーンマップがこれで「0 draw」に見えていました）。RHI エンコーダ（`enc->Draw()` 系）経由の描画は自動で計上されます。

**VS/PS のシグネチャ整合はビルドでは検出できません。** 検査されるのは PSO 作成時、つまり実行時だけです:

- `Shaders/Debug/Meshlet/MeshletDebug_PS.hlsl` は通常の頂点シェーダとペアの PSO（`RenderPipelineStateCache` の "MeshletDebug"）で使われます。メッシュシェーダ系 PSO の PS は `CookTorranceGGX_PS` です。**VS は per-primitive 属性を供給できない**ため、この PS に `MESHLET_INDEX` のような属性入力を足すと `CreateGraphicsPipelineState` が `E_INVALIDARG` を返し、`RenderPipelineStateCache::Initialize` の失敗＝起動不能になります。
- メッシュレット index は `SV_PrimitiveID / kMaxTrianglesPerMeshlet` で求めます。除数は `Shaders/Shared/Common/MeshletConstants.hlsli` にあり、CPU 側 `MeshletBuffer::kMaxTrianglesPerMeshlet` と対になる唯一の定義です。この導出が正しいのは、ビルダーが index buffer を連番のチャンクに切っている間だけです。
- 64 本のシェーダのうち 13 本は vcxproj に CustomBuild 登録が無く**実行時コンパイル**です。ビルドが緑でもシェーダが壊れている場合があります。加えて、仮に CustomBuild を足しても dxc の単体コンパイルではステージ間リンケージは検証されません。

## Implementation Policy

レンダラー、graphics API、shading、lighting、ray tracing、GI、material、render graph の変更は、実装前に短い計画を作り、論文、公式 API 仕様、確立したエンジン実装、ベンダーサンプル、保守されている OSS renderer を設計・挙動の根拠として使います。互換性のないライセンスのコードはコピーしません。

実装後は可能な範囲で shader compile（warning as error）、x64 Debug/Release build、D3D12 Debug Layer + 必要に応じて GBV、サンプルシーンの描画・リソース寿命・フレーム安定性を検証します。未検証、推測、部分実装、backend 固有の制限は README または TODO に残します。

## RHI Implementation Notes

Status as of 2026-08-24:

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
- BakeGI is a finite runtime probe bake, not a full offline asset bake. `IrradianceProbeGrid::UpdateProbes()` advances up to 32 probes per call until every probe has been dispatched once; SWRT reflection reuse no longer keeps writing baked GI probes every frame. PBRApp now writes a session-local sidecar cache (`PBRApp.gi_probe_cache.bin`) for completed baked SH probe data and restores it only when the saved scene/settings hash and probe grid layout match. This cache is not a reusable editor bake asset; changed scene, material, light, or probe settings must still call `RequestGIBake()` or `ResetAndRebakeGI()` when no matching cache is available.
- GI bake progress is exposed through `Renderer::GetGIBakeStatus()` and `ApplicationCore::GetGIBakeStatus()`. The status reports state (`Baking`, `Completed`, `WaitingForProbeGrid`, `WaitingForBvh`, or `Failed`), completed/total probe counts, probes processed per step, estimated remaining render frames, stalled frame count, and a SWRT BVH missing-buffer mask when blocked by `WaitingForBvh`. A 0% bake with `WaitingForBvh` means the SWRT BVH was not available yet; it should be shown as a blocked/waiting state instead of an unknown infinite bake.
- The PBR sample's existing `GI` tab displays GI bake state, a progress bar, probe counts, per-frame probe step, estimated remaining frames, and blocked/failed reason. If the GUI shows `Waiting for software ray tracing BVH buffers`, the bake is not merely slow; it is waiting for valid SWRT BVH GPU buffers. The tab also lists the missing SWRT BVH inputs (`bvhNodes`, `triangles`, `meshInfo`, `instances`, `tlasNodes`, `materials`, or SWRT initialization) so the wait can be diagnosed from the UI.
- `GpuSoftwareRayTracer::GetBvhGpuAddresses()` only reports a valid SWRT BVH when all six GPU buffers are available: BVH nodes, triangles, mesh info, instances, TLAS nodes, and materials. GI probe update dispatch must not run with a partial BVH binding set, because the compute shader reads all six buffers.
- SWRT directional shadows no longer disable local-light shadows. `LightSystem::ExecuteShadowPass()` is split into `ExecuteDirectionalShadowPass()` (resource setup, cascade depth passes, VSM moments/blur) and `ExecuteLocalLightShadowPasses()` (spot maps, point cube maps); the SWRT path replaces only the former and now always runs the latter. Previously the SWRT branch returned before `ExecuteShadowPass()` entirely, so spot and point shadow maps were never rendered while `UpdateFrameLighting()` still set their enable flags -- `SampleSpotShadow()` and `SamplePointShadow()` then compared against never-rendered textures and returned 0, zeroing every punctual light.
- Tone mapping uses the Narkowicz 2015 ACES fit with an exposure scale instead of plain Reinhard. `x/(x+1)` mapped a radiance of 1.0 to sRGB 0.73, lifting every midtone so shadows never sat down; EV plumbing through a constant buffer is still pending.
- GI probe visibility was tightened to reduce light leaking through thin geometry: the soft-fade band is `0.25 * min(spacing)` rather than `0.5 * length(spacing)` (~0.87x spacing, wide enough to hide a whole floor slab), the 1% weight floor that let fully occluded probes always contribute is gone, and sample positions get a normal bias of `0.25 * min(spacing)`. A trilinear-only fallback keeps points outside the grid from going black.
- Punctual light accumulation lives in one place. `PBR_DirectLighting.hlsli` (`PbrSurface`, `AccumulatePointLights`, `AccumulateSpotLights`) is shared by the deferred and forward pixel shaders, which carried byte-identical copies of those loops -- the divergence that made the indirect-diffuse 1/PI fix a two-file edit and left normal mapping out of the forward path.
- Render-pass responsibilities were audited pass by pass and the clear deviations fixed. The G-Buffer fill pass no longer binds the shadow/light/IBL/AO/reflection/depth/spot-shadow/VSM/backface/point-shadow tables or the light CB: `OpaqueGBuffer_PS.hlsl` declares only b0/t0/t7/t17/s0, so those ten bindings were dead, and the AO one was worse than dead -- it targeted root parameter 6, which is the material occlusion slot (t7), not the screen-space AO slot the Lighting pass uses. `Setup()` declarations were realigned with what each pass actually touches (`SSAOBlur` reads GBufferNormal, `SSAO` declares its Write, SSR declares its color target, the SWRT reflection pass reads the three G-Buffer targets its compute shader samples instead of SceneColor/SceneDepth, and the deferred Lighting pass declares its AO and reflection inputs). The skinned draw path now binds runtime AO (t9) and the GI probe grid (b2/t10) like the static path does -- skinned transparent draws go through `CookTorranceGGX_PS`, which samples both, so they were reading undefined descriptors. Empty leftovers (`OpaqueRenderPass.*`, `TransparentRenderPass.*`, `Opaque_PS/VS.hlsl`, `RenderGraph_Registry.cpp`) were deleted.
- The raster GBuffer pass applies tangent-space normal maps. Material normal textures now travel glTF/FBX loader -> `LoadedStaticMesh::normalTexturePath` -> `MeshComponent` -> `RenderProxy::normalTexture` -> `SceneSubmitter::DrawItem` -> root parameter 16 (a 1x1 flat-normal texture is bound when a material has none), and `OpaqueGBuffer_PS.hlsl` perturbs the interpolated vertex normal with it. Vertices carry no tangents, so the TBN is rebuilt per pixel from screen-space derivatives (Schuler, "Normal Mapping Without Precomputed Tangents") and the tangent-space Z is reconstructed as `sqrt(1 - x^2 - y^2)` because BC5 normal maps store only two channels. Register note: t5/t6 are NOT free in the shared raster root signature -- the IBL range at t4 spans three descriptors (t4-t6) and adding a range at t6 fails `D3D12SerializeRootSignature` with E_INVALIDARG -- so the normal map lives at t17. `OpaqueGBuffer_PS` is also used by the skinned GBuffer PSO, so the skinned root signature carries the same t17 table (bone CB stays at index 16, normal table appended at 17) and binds the flat-normal fallback until skinned proxies carry their own normal texture. `StaticMeshCache` serializes the new path and its format version moved to 2, so existing mesh caches are rebuilt once.
- World-space normals are transformed with `mul(worldToObject, normal)` (matrix first). Positions use the row-vector product (`worldPos = objPos * u_world`), so the matching normal transform is `n * transpose(inverse(W))`; the previous `mul(normal, worldToObject)` applied `inverse(W)` directly, which for a rotation is the inverse rotation. Meshes whose node transform is identity (Sponza's glTF nodes) were unaffected, but every rotated node (the Bistro FBX blocks, which carry rotation in `geometry_to_world`) was lit from the wrong side -- undersides bright, sun-facing walls dark, and black speckle wherever N.L fell below zero. Fixed in `CookTorranceGGX_VS`, `SkinnedMesh_VS`, `Tessellation_DS`, `Tessellation_Debug_DS`, `BasicShader`, and `MeshShader_MS`. The DXR path is unchanged: `WorldToObject3x4()` is a column-vector matrix, so `mul(normal, worldToObject)` is already the inverse-transpose there.
- The GI probe grid can auto-fit the whole loaded scene. `Renderer::GetSceneWorldBounds()` reduces the ray-tracing instance AABBs, `IrradianceProbeGrid::FitToSceneBoundsWithBudget()` picks an isotropic spacing so the probe count stays inside a budget (coarsening further if rounding overshoots), and PBRApp exposes it as the `Scene Auto` grid preset (`probe_preset = 3`). The previous presets are all Sponza-sized boxes, so the Bistro block placed at x = 60 fell outside the grid entirely and every pixel there sampled the clamped boundary probes of the Sponza grid -- constant, unrelated irradiance rather than GI. With the auto-fit preset the scene bakes 13475 probes (budget 16384) and covers both models; the trade-off is a coarser spacing (~2m to ~2.8m) for Sponza's interior.
- Indirect diffuse now applies the Lambert 1/PI. `GI_EvaluateProbeSH()` and `EvaluateDiffuseIrradianceFromSh()` return irradiance E (their SH is convolved with the cosine lobe: A0=PI, A1=2PI/3, A2=PI/4), so the shading sites in `DeferredLighting_PS.hlsl` and `CookTorranceGGX_PS.hlsl` divide by PI exactly like the direct diffuse term does; before this they multiplied E by albedo directly and the indirect diffuse was PI times too bright. The prefiltered irradiance cubemap is NOT divided: `GenerateIrradianceCubemapFromEquirect()` stores a cosine-weighted average radiance (already E/PI). Measured in Sponza, the GI contribution over a GI-off baseline dropped from +6.1 to +2.2 (floor) and +9.4 to +3.6 (shadowed wall) in display luma.
- Material textures are uploaded with a full mip chain. `TextureMipChainBuilder::BuildRgba8()` (`Source/Renderer/Utilities/TextureMipChain.h`) box-filters an RGBA8 top level down to 1x1 on the CPU, `ResourceUploadUtility::CreateTexture2DFromRgba8WithMips()` uploads every level in one `UpdateSubresources` call, and `SceneSubmitter` sets the SRV `MipLevels` and `Texture::desc.mips` to the real level count. The shared linear/wrap static sampler now also sets `MinLOD = 0` / `MaxLOD = D3D12_FLOAT32_MAX`: a zero-initialised `D3D12_STATIC_SAMPLER_DESC` leaves `MaxLOD` at 0, which clamps every fetch to mip 0 and would make the uploaded chain inert. Measured on Bistro, this changes 3.6% of pixels; the residual shimmer under sub-pixel camera motion (15.7% of pixels change for a 0.17 degree rotation, down from 16.0%) is geometric edge aliasing, which needs anti-aliasing rather than texture filtering -- the renderer still has no MSAA/FXAA/TAA. The RHI texture creation path stays single-mip because `CreateRhiTexture2DFromRgba8()` cannot yet take a mip count; DX12 always uses the batched upload path.
- The probe bake writes traced irradiance at full weight on the first pass over the grid; `ema_alpha` only applies to later refresh passes (`IrradianceProbeGrid::FillUpdateCB` uses `IsBaked() ? m_emaAlpha : 1.0f`). The probe SH buffer is zero-initialised and a finite bake dispatches every probe exactly once, so blending the first pass with the EMA weight left the baked field at `ema_alpha * radiance` -- at the shipped `ema_alpha = 0.01` that is 1% of the correct irradiance, which looks like GI doing nothing. `AllocateProbeBuffer()` also clears bake progress whenever it creates a buffer without initial data, so a probe-grid resize cannot leave `IsBaked()` true over a freshly zeroed buffer. The GI probe cache version was bumped to 3 so sidecar caches baked with the old under-weighted path are rejected and re-baked.
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
- Deferred lighting currently covers the core raster opaque path: directional/point/spot lights, CSM/VSM/spot/point shadows, IBL diffuse, runtime AO, emissive, GI probe irradiance, and specular-glossiness material workflow. GBuffer debug views are isolated in `Shaders/Debug/GBuffer/GBufferDebug_PS.hlsl` and use a dedicated PSO selected by `LightingRenderPass`; the normal `DeferredLighting_PS.hlsl` contains only final-lighting output. Specular-glossiness uses `GBufferSpecularWorkflow` to carry per-pixel specular color and workflow flag into the fullscreen lighting combine. Runtime visual parity with the previous forward path still needs scene-level capture verification.
- `IblSystem` now owns all IBL GPU resources (irradiance cubemap, prefilter cubemap, BRDF LUT) and is aggregated by `Skybox`. On the DX12 feature path, `IblSystem::UploadGeneratedIblTextures()` prefers `CreateRhiTexture` + `GetD3D12CompatibilityResource` for the three HDR IBL textures, then falls back to `CreateCommittedResource`. Upload buffers (upload-heap `Resource` wrappers) and the fallback 1×1 placeholder textures remain DX12-direct. `Skybox` getter shims (`IsIblEnabled`, `GetIblPrefilterResource`, etc.) now forward to `m_iblSystem` with no behavior change for callers. `SkyboxIBL.cpp` contains only the `EnsureIblTexturesUploaded` delegate; all generation and upload logic lives in `IblSystem.cpp`.
- Vulkan native mesh rendering now owns one D32 depth image/view per swapchain image and recreates native framebuffers/depth resources on resize. Vulkan capability flags report only features enabled on the logical device; ray query, ray tracing pipeline, mesh shader, descriptor indexing, timeline semaphore, and dynamic rendering remain disabled until `CreateDevice()` enables the corresponding extension and feature chains.
- The Vulkan backend implements the hardware ray-tracing pipeline RHI primitives via `VK_KHR_ray_tracing_pipeline`: `CreateRhiRayTracingPipeline` builds a `VkPipeline` from `RhiRayTracingPipelineDesc::spirvBytecode` (a SPIR-V field parallel to the DX12 DXIL field, so the DXIL path is unchanged), `CreateRhiShaderBindingTable` packs raygen/miss/hit regions using the device's queried `shaderGroupHandleSize`/`shaderGroupBaseAlignment`/`shaderGroupHandleAlignment`, and `VulkanRhiCommandEncoder::DispatchRays` records `vkCmdTraceRaysKHR`. Shader stages are derived per export by scanning the SPIR-V `OpEntryPoint` execution models (so no extra stage metadata is needed), and the pipeline builds a fixed internal descriptor set layout (binding 0 = acceleration structure, binding 1 = storage image). `VulkanGraphicsDevice::RunRayTracingSmokeTest`, gated on `SASAMI_VK_RT_SMOKETEST=1` at init, validates the full path end to end: a one-triangle BLAS/TLAS, DXC HLSL→SPIR-V of `Shaders/RayTracing/DXR/VulkanRtSmokeTest.hlsl`, pipeline + SBT, `vkCmdTraceRaysKHR` into a 64×64 storage image, and readback verifying hit=red / miss=blue (`Vulkan RT smoke test: PASS` in `PBRApp.exe.log`). These primitives are not yet wired into a Vulkan render pass; `DxrRayTracer` and the render graph RT path, plus the DXR shader's `ResourceDescriptorHeap` bindless model, remain DX12-only.
- OpenGL format conversion now covers `R32UInt` and `D24UNormS8UInt` in addition to the existing color, float, and depth formats.
- Baked GI probe diffuse is currently consumed only by the DX12 feature render path. Vulkan / DX11 / OpenGL native fallback shaders do not bind or sample the baked SH probe buffer, so cross-backend GI visual parity is not claimed. Completing it requires portable buffer descriptor binding, a non-DX12 probe bake or serialized probe cache upload path, and native/feature shaders that evaluate the same probe interpolation.
- `CreateRhiBuffer()` supports immediate `initialData` for `GpuOnly` buffers on all four backends: DX12 and Vulkan use staging copies, while DX11 `CreateBuffer()` and OpenGL `glBufferData()` consume initial data directly. `GpuToCpu + initialData` is rejected on DX12/DX11 because readback resources are copy destinations rather than upload resources.
- Non-DX12 configurations exclude the DX12 RHI ray-tracing implementation at translation-unit scope. This prevents Vulkan, DX11, and OpenGL builds from compiling definitions for the macro-hidden `Dx12GraphicsDevice` class.
- Remaining limitation: several DX12 feature-path systems still expose GPU virtual addresses for constant buffers, shader resource buffers, and ray tracing structures. SWRT/DXR BVH, acceleration structure, scratch/result buffers, and upload-helper staging resources remain direct DX12 allocations where the current RHI contract does not yet represent the required build/copy/state semantics. Future work should move these resource owners to RHI handles where the backend contract can represent the required binding type and synchronization requirements.
- Debug probe-grid visualization ("Show Probe Spheres") fix: `IDevice::CreateRhiPipelineLayout()` / `CreateRhiGraphicsPipeline()` return handles whose id is an internal map key, but command-encoder binding (`SetGraphicsPipelineLayout`/`SetGraphicsPipeline`) treats `handle.id` as the raw native `ID3D12RootSignature*`/`ID3D12PipelineState*` (matching `RenderPipelineStateCache::MakeLayoutHandle`/`MakePipelineHandle`). `DebugProbeGridRenderPass` was the only pass creating its pipeline via the device path, so binding its create-handles dereferenced a small integer as a pointer and crashed. Added `IRHIDevice::GetBindablePipelineLayoutHandle()` / `GetBindablePipelineHandle()` (DX12 resolves the map entry to the native pointer; default identity for other backends); the pass now caches and binds the bindable forms.
- Debug probe-grid pass insertion fix: `Renderer::UseDefaultRenderNodePreset()` ("Reset Passes") re-inserted the volumetric-cloud pass but not the probe-grid pass, and `SetDebugProbeGridEnabled(true)` only set a flag without ensuring the pass was in the graph. Consolidated insertion into `Renderer::EnsureDebugProbeGridPassInserted()` (idempotent), now called from `UseDefaultRenderNodePreset()`, `SetRenderPassSequence()`, `ReinsertDebugProbeGrid()`, init, and on enable — so probes survive a preset reset and appear as soon as the toggle is checked.
- GI probe grid light-leak fix: `GI_SampleProbeGrid` (`Shaders/RayTracing/GI/GI_Common.hlsli`) previously blended the 8 surrounding probes with pure trilinear interpolation, so a probe's irradiance could bleed straight through floor/ceiling slabs and walls into adjacent rooms/floors with no occlusion awareness. Real-time ray-traced probe-to-point visibility was ruled out (the deferred/forward lighting PS root signatures have no spare BVH/TLAS slots, and a per-pixel SWRT walk in a full-screen pass is too costly), so occlusion is instead baked at probe-update time: each of the 64 rays traced per probe (`GI_ProbeUpdate_CS.hlsl`) now also projects its hit distance onto the same L2 SH basis (new scalar `ProjectOntoSH` overload in `GI_ProbeTracing.hlsli`), stored in the previously-unused `.w` component of the existing 9×float4 probe buffer — no new GPU storage, root-signature, or binding changes. At sample time, `GI_EvaluateProbeDistanceSH` reconstructs this directional "how far can I see" field with bare (non-cosine-convolved) SH basis constants — distinct from `GI_EvaluateProbeSH`'s convolved constants, which must not be reused for a non-radiance scalar — and `GI_ProbeVisibilityWeight` combines a smoothstep occlusion test against that field with a normal-based backface weight. `GI_SampleProbeGrid` now does an explicit 8-corner weighted sum (trilinear weight × visibility weight, renormalized) instead of a lerp chain. This is a mean-distance-only approximation (not full two-moment Chebyshev visibility), which is expected to still show some residual leak/over-darkening near small partial occluders like doorways. `kGIProbeCacheVersion` (`Source/Renderer/Runtime/Renderer.cpp`) was bumped from 1 to 2 so probe-cache files saved before this change are rejected and rebaked instead of silently importing with `.w = 0` (which would read as maximally occluded).
- Binary glTF (`.glb`) loading support: `LoadGLTFStatic` (`ModelLoader.cpp`) and `LoadGLTFSkinned` (`ModelLoaderSkinned.cpp`) share `ParseGlbContainer` in `ModelLoaderGlb.h`/`.cpp`, which reads the 12-byte GLB header, mandatory JSON chunk, and optional BIN chunk. Format detection sniffs the first 4 bytes for the `glTF` magic rather than trusting the extension, and a glTF `buffer` without `uri` resolves to the embedded BIN data. Skinned meshes loaded this way are submitted to DX12 and all three native fallback backends; their shading feature sets still differ as described above.
- OpenGL native fallback now renders skinned meshes. `RhiBackendMeshFrameDesc` gained a `skinnedDraws`/`skinnedDrawCount` array (`RhiBackendSkinnedMeshDrawDesc`, mirroring the existing static-mesh draw-desc convention) carrying a per-draw bone-matrix palette pointer. `SceneSubmitter::SubmitSkinnedRenderProxies` now branches on whether a `RendererFrameCoordinator`/`FrameContext` pair is supplied: the DX12 path still pushes bone matrices into a constant buffer (`EnsureBoneBuffers`/`PushBoneCB`), while native backends instead store the full `Skeleton::kMaxBones`-sized palette directly on the draw item (`SkinnedDrawItem::boneMatricesNative`). `OpenGLGraphicsDevice` gained a dedicated GLSL 120 skinning vertex shader (`EnsureSkinnedMeshFrameResources` / `RenderSkinnedMeshFrame`) that performs bone-palette linear-blend skinning of position and normal in the vertex stage and reuses the existing static-mesh fragment shader unchanged. This mirrors the static native path's existing simplification (no world-matrix normal transform, no shadows/IBL). `Dx11GraphicsDevice` gained the equivalent HLSL skinning vertex shader (`EnsureSkinnedMeshFrameResources` / `RenderSkinnedMeshFrame`), using `row_major` cbuffer matrices and true `uint4 JOINTS_0`/`float4 WEIGHTS_0` vertex attributes (`DXGI_FORMAT_R8G8B8A8_UINT`), and reuses the static path's pixel shader, sampler, and render states; `RenderSkinnedMeshFrame` only clears depth when the static mesh pass did not already run this frame, to avoid erasing its depth results.
- Vulkan native fallback now renders skinned meshes too, completing skinned-mesh support across all three non-DX12 backends. `VulkanGraphicsDevice` gained `EnsureNativeSkinnedMeshResources`/`DestroyNativeSkinnedMeshResources` and a dedicated `NativeMeshSkinned.hlsl` (4-bone linear-blend skinning in the vertex stage; the pixel shader is a verbatim duplicate of `NativeMesh.hlsl`'s, required because Vulkan pipelines bake VS+PS into one monolithic `VkPipeline` object). Bone matrices live in HLSL `register(b0, space1)`, which DXC's Vulkan register-shift table (only space0 is remapped) passes through unshifted to Vulkan `set=1, binding=0`; the set is a single `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` binding backed by one persistently-mapped host-coherent buffer sized for up to 16 simultaneous skinned draws per frame (`Skeleton::kMaxBones`-sized palette per slot, `minUniformBufferOffsetAlignment`-rounded stride), following the same single-buffer-no-per-frame-duplication pattern already used by the existing native RayMarch UBO. Skinned draws are recorded in the same render pass and framebuffer as static draws (`RenderMeshFrame`), immediately after the static-draw loop; texture descriptor-set lookup/creation/caching was factored out of the static loop into a shared `GetOrCreateNativeMeshTextureDescriptorSet` helper reused by both loops.
- Debug system decoupling: the previously tightly-coupled "Debug" functionality was split into two loosely-coupled, single-responsibility classes instead of one monolithic manager, to respect the Foundation→Renderer layering (Foundation must not depend on Renderer types). `DebugLogSystem` (new, `Source/Foundation/Tools/DebugLogSystem.h`/`.cpp`, Meyers singleton like `InputSystem`) owns a swappable sink list with per-sink `DebugLogLevel` (`Info`/`Warning`/`Error`) filtering; its constructor registers the original `OutputDebugString` + `<exe>.log` file sinks at `Info` level by default, so all ~466 existing `DebugLog(...)` call sites keep identical behavior unchanged. `DebugOutput.h`'s `DebugLog()` is now a thin wrapper delegating to `DebugLogSystem::Instance().Log(...)`. `DebugVisualizationSystem` (new, `Source/Renderer/Passes/Debug/DebugVisualizationSystem.h`/`.cpp`, value-member "System" pattern like `LightSystem`) consolidates the probe-grid debug visualization state/toggle/radius that used to be duplicated across `Renderer.h` and `ApplicationCoreProperties.cpp`; since only `Renderer` has render-graph pass-insertion authority, it holds an `EnsureInsertedCallback` bound by `Renderer` to `EnsureDebugProbeGridPassInserted()` rather than inserting passes itself. `Renderer::GetDebugVisualization()` replaces the old 4 duplicated accessors + private field. Public `ApplicationCore` API (`GetDebugProbeGridEnabled()` etc.) is unchanged; only its implementation now routes through `m_renderer->GetDebugVisualization()`.

## セッション永続化 (PBRApp)

PBRApp は終了時にカメラ・ライト・描画設定を保存し、次回起動時に自動復元します。手続き生成される球/箱シーンは破壊されません。

- 保存対象: メインカメラ (`position`/legacy `target`/`yaw`/`pitch`/`distance`/`clip`/`move_speed`)、directional light (shadow mode/cascade/bias 含む)、ユーザーが追加した point/spot light、`RenderSettings` 構造体全フィールド、GI トグル (enabled/intensity/ema/baked)、probe grid preset/debug 表示/radius、ライトギズモ表示フラグ。
- 保存先 (プロジェクトルート、per-user・git 管理外): `PBRApp.scene`（カメラ+ライト）、`PBRApp.settings.ini`（描画/アプリ設定）、`PBRApp.gi_probe_cache.bin`（一致時のみ復元する GI probe SH sidecar cache）。パスは `ApplicationResourcePaths::ResolveConfigPathString()` が解決。
- 復元は非破壊: `ApplicationCore::ApplyCameraAndLights()` が `ClearObjects()` を呼ばず、既存のアクティブカメラ・directional light へ in-place 適用し、保存済み point/spot light のみ再構築します（`[static_model]` と未知セクションは無視）。破壊的なフルロードが必要な用途向けに `LoadScene()` は従来どおり残しています。
- レンダラは `Renderer::GetRenderSettings()/SetRenderSettings()` で `RenderSettings` を一括 round-trip します。
- GI probe のベイク結果は `PBRApp.gi_probe_cache.bin` に保存します。cache version（現在 v2）、scene/settings hash、probe grid layout が一致した場合だけ復元し、不一致またはロード失敗時は GI が有効なら再ベイクを要求します。これは再利用可能な editor bake asset ではなく PBRApp の session sidecar cache です。
- 保存されないもの: skybox パス、ImGui ウィンドウレイアウト（`imgui.ini` が別管理）。

Implementation references for the portable buffer copy/readback contract:

- Microsoft `D3D12_HEAP_TYPE`: readback-heap resources must remain in `D3D12_RESOURCE_STATE_COPY_DEST`: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
- Microsoft D3D12 readback guidance: synchronize copy completion before mapping the readback buffer: https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
- Khronos `vkCmdPipelineBarrier` / `VkBufferMemoryBarrier`: buffer barriers define the resource-specific access dependency around buffer copies: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkCmdPipelineBarrier.html
- Khronos `VkDescriptorSetLayoutBinding`: immutable sampler handles are copied into the layout but sampler objects must outlive layout/set use: https://registry.khronos.org/vulkan/specs/latest/man/html/VkDescriptorSetLayoutBinding.html

レンダラー、graphics API、shading、lighting、ray tracing、GI、material、render graph を変更する場合は、実装前に短い実装方針を作り、可能な範囲で論文、公式 API 仕様、Unreal Engine などの既存実装、ベンダーサンプル、保守されているオープンソースレンダラーを参照してください。

外部コードは設計や挙動の参考に留め、互換性のないライセンスのコードをコピーしないでください。実装後は Debug/Release ビルド、シェーダーコンパイル、実行時描画確認で検証してください。ローカル環境で実行できなかった検証は、その理由を明記してください。
