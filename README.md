# Sasami Renderer

Sasami Renderer は C++23 のレンダラー実験プロジェクトです。DirectX 12 の feature render path を主軸に、RenderGraph、PBR、GBuffer、shadow、Screen Space Reflection、Software Ray Tracing、DXR、GI、複数 RHI バックエンドの検証を同じコードベースで進めています。

この README は **2026-09-23 時点のソースコード調査**を基準にしています。最後に 4 バックエンドを同じ観点で通したビルド・実行監査は 2026-09-20 です。実装済みでも実機確認が終わっていない機能、DX12 専用機能、部分実装は各節で明示します。

## バックエンドごとの機能ゲート

**最終クロスバックエンド監査（2026-09-20、PBRApp、ヘッドレス）**

| 構成 | ビルド | 実行・キャプチャ | 備考 |
|---|---|---|---|
| Debug（DX12） | ソリューション全体の Rebuild 成功 | CLI smoke とバックバッファ PNG の目視確認に成功 | GPU 合計 12.32ms、Shadow 10.92ms。D3D12 Debug Layer の error/warning は 0 件。GBV は未実施 |
| Release（DX12） | 成功 | CLI smoke とバックバッファ PNG の目視確認に成功 | GPU 合計 14.37ms、Shadow 12.50ms |
| Release_Vulkan | 成功 | 起動、状態取得、アプリ内 PNG キャプチャと目視確認に成功 | native fallback path。GPU timestamp は未対応 |
| Release_DX11 | 成功 | 起動、状態取得、アプリ内 PNG キャプチャと目視確認に成功 | native fallback path。GPU timestamp は未対応 |
| Release_OpenGL | 成功 | 起動、状態取得、アプリ内 PNG キャプチャと目視確認に成功 | native fallback path。GPU timestamp は未対応 |

非 DX12 の 3 構成はいずれもネイティブフレーム経路（静的・スキンメッシュ描画とレイマーチ）で動く。
出力先は `Build/bin/x64/Release/<Vulkan|DirectX11|OpenGL>/` で、DX12 の `Release/` とは別ディレクトリ。

## 実装到達点と優先 To Do

DX12 は RenderGraph、deferred PBR、各種 shadow、SSR/SWRT/DXR、GI、Virtual Geometry、流体・煙、particle、post process をつないだ feature path です。Vulkan / DX11 / OpenGL は UI、静的・スキンメッシュ、depth、texture、sky、native shadow などを持つ実動する fallback path ですが、DX12 の feature pass 群を RenderGraph ごと移植した状態ではありません。したがって「4 バックエンドで起動すること」と「4 バックエンドで feature parity があること」は分けて扱います。

Virtual Geometry は CPU DAG、screen-error LOD、mesh shader raster、CSM の GPU 選択 + `ExecuteIndirect`、feedback/LRU/root pinning、RAM page store、96MB GPU page pool まで動作しています。残る中心課題は GBuffer の完全 GPU 駆動化、material/texture の bindless 化、Hi-Z occlusion、disk paging、indirect 実行の RHI 抽象化です。詳細と計測値は [Virtual Geometry](#virtual-geometry) を参照してください。

優先度の高い既知問題と実装境界:

| 優先度 | 項目 | 現在の根拠と次の作業 |
|---|---|---|
| P0 | 実行中の `scene.load` で PBRApp が access violation | 2026-09-20 に `render.meshshader 1` の Release で再現。`LoadScene()` は `ClearObjects()` で全 `SObject` を破棄しますが、PBRApp は `Camera*` / `StaticModel*` を保持したまま `OnUpdate()` から参照します。GPU idle 待ちだけではこの CPU 側 dangling pointer は解消しません。所有権を handle/再検索へ変えるか、破壊的ロード後にアプリへ再バインド通知する必要があります。修正までは `scene.load` を使わず、シーンファイル変更後に再起動してください |
| P1 | Virtual Geometry の main-view GPU driven 化 | CSM は GPU 選択済みですが、GBuffer は CPU per-item submit と従来の全常駐 DAG buffer を使います。material table と texture heap index を GPU draw data に移し、page pool から main view を描く必要があります |
| P1 | RenderGraph の cross-queue resource handoff | Signal/Wait と command list の枠組みはありますが、graphics/compute 間の resource-state transition は未実装です。現状 `PreferredQueue() == Compute` の実パスは 0 で、共有リソースを触る pass を opt-in すると unsafe です |
| P1 | 検証の空白 | DX12 の GBV は opt-in のまま今回未実施です。非 DX12 は native path の GPU timestamp が無いため、起動・状態・画像を別々の証拠として確認しています |
| P2 | importer / shadow の残件 | glTF skin node が明示的な `matrix` を使う場合の bind TRS 分解と、VSM の skinned moment 出力は未実装です |
| P2 | Debug build warning | `GpuSoftwarePipelineCache.cpp` の `std::wstring` → `std::string` 変換に C4244 が 1 件残っています。ASCII の shader target profile では動作しますが、警告ゼロの基準には未到達です |

`TODO.md` は未解決不具合、将来機能、計測メモ、判断待ちを同じチェックリストで管理しているため、チェック数をそのまま実装率として扱いません。この節では実行可能性と依存関係から優先度を付けています。

## UI パーツのコミット方針

`Source/AppFramework/UI/` の各パーツ（`Volume` / `Angle` / `Color3` / `Color4` / `IntVolume` /
`Vector3`）は、既定では**確定時コミット**で動く。ドラッグ中は `UiEditBuffer` に途中の値を退避して
`false` を返し、スライダーを離した瞬間にだけ `true` を返す。呼び出し側が毎フレーム getter から
現在値を読み直す作りなので、途中の値をどこかに保持しないとスライダーを掴めないためであり、
同時に、変更のたびに重い再構築が走るのも防いでいる。

**即時反映が要る場所は `UI::CommitScope` でプリセットを切り替える**（`UiCommitPolicy.h`）:

```cpp
if (UI::Tab lightingTab{"Lighting"}) {
    UI::CommitScope realtime{UI::CommitPolicy::Realtime};
    ...
}
```

スコープ内のパーツは値が変わったフレームごとに `true` を返す。`CommitScope` は RAII で
前の方針へ戻すので入れ子にできる。Realtime では `UiEditBuffer` を使わない ――
呼び出し側がその場で書き戻すため、次フレームの getter が新しい値を返し、巻き戻らないから。

現在 Realtime を使っているのは PBRApp の Lighting タブのみ。ドラッグしながらシーンが追従することに
意味がある操作だからで、他のタブは既定の確定時コミットのまま。

## UI 描画バックエンドの抽象化

**2026-09-08: DX12 / Vulkan / DX11 / OpenGL の 4 バックエンドすべてで UI が描かれる**（4 構成とも実機確認済み）。

UI 描画は `UI::IUiRenderBackend`（`Source/AppFramework/UI/Backend/`）に切り出してある。
`ImGuiCoordinator` は ImGui 文脈と Win32 プラットフォーム層（全バックエンド共通）とウィンドウ/オーバーレイ登録だけを持ち、
描画は `UiRenderBackendDx12` / `UiRenderBackendVulkan` / `UiRenderBackendDx11` / `UiRenderBackendOpenGL` のいずれかへ委譲する。
`RhiBuild.props` によりバックエンドはコンパイル時に排他なので、`CreateUiRenderBackend()` は
条件コンパイルで 1 実装だけを返す（実行時の切り替えは無い）。

描画のタイミングが 2 系統に分かれる点が設計の要:

- **D3D12**: 従来どおりアプリがフレームグラフからコマンドリストを受け取り、オーバーレイコールバックで描く。
- **非 D3D12**: `ExecuteBackendFrame` がシーン描画から Present までを 1 関数で完結させるため、
  外からコマンドリストを渡せない。`RhiBackendFrameDesc::overlay`（`IRhiFrameOverlay`、`Renderer/RHI/RhiUiOverlay.h`）を
  通してバックエンド側が自分のフレーム内部から UI 層を呼び戻す。DX11 は Present 直前に深度ビューを外して
  バックバッファを再バインドしてから、OpenGL は `glFlush` 直前に描く。

Vulkan だけは専用のレンダーパスが要る。`VulkanGraphicsDevice` が UI 用の `VkRenderPass`
（色のみ・loadOp = LOAD・initial = final = `COLOR_ATTACHMENT_OPTIMAL`）とスワップチェーンイメージごとの
`VkFramebuffer` を持つ。レンダーパスはリサイズをまたいで生かす（ImGui のバックエンドは初期化時のレンダーパスを
内部パイプラインに焼き込むため、作り直すとハンドルが古くなる）。フレームバッファはイメージビューを参照するので
`CreateSwapChain` の破棄群で必ず作り直す。シーンを1つも描かないクリアのみの経路はイメージが
`TRANSFER_DST_OPTIMAL` で戻ってくるため、UI レンダーパスの前に遷移を挟み、描画後は
`frameOutputLayout/Access/Stage` を color-attachment 値へ更新する（この3変数は present バリアと
`vkQueueSubmit` の `pWaitDstStageMask` の両方で使われる）。

`ImGui::Render()` の呼び出しは `ImGuiCoordinator::EndFrame()` 1 箇所に集約した。
`[camera]` 節の無いシーンを読むと、以前はメインカメラが null のまま描画が毎フレーム飛ばされ、`NewFrame` だけが繰り返されて ImGui のエラー回復が再帰しスタックオーバーフロー（c00000fd）で落ちていた（2026-10-20 修正）。
今は `LoadScene` がデフォルトカメラを作り、`ImGuiCoordinator::NewFrame` は前フレームが閉じられていなければ `ImGui::EndFrame()` で閉じる。
ImGui のエラー回復ツールチップは切り、エラーは `[ImGui] error in window ...` として先頭32件だけ DebugLog に出す。
ImGui は upstream の docking ブランチ（1.92.0 WIP, 19198）を使う（2026-10-23）。`ImGuiCoordinator` は docking を有効にし、毎フレーム中央が素通しの DockSpace をビューポート全体に張る（マルチビューポートは無効）。
エディタ UI（`Source/AppFramework/Editor/`、全アプリ共通）: Ctrl+E で UE 風のレイアウト（右上 WorldObjects（ライト／モデル／カメラの一覧）、右下 Details（モデルの位置・回転（度）・拡大、ライトの色・強さ・範囲・向き、カメラの値）、下段 Content Browser、中央がビューポート）を開く。各アプリのパネル（FluidApp の Fluid など）は Details の隣のタブに連結される。Window > Reset Layout（`editor layout reset`）で既定配置に戻せる。既定は非表示。表示中はビューポートの左クリックでモデル（包含箱で判定）や点・スポットライトを選択でき、選択物は橙の枠で示す。表示中は画面上端にメニューバーが出て、Window メニューから WorldObjects・Details と各アプリのパネル（FluidApp の Fluid など）を開閉でき、View メニューで Lit / Unlit（ベースカラーのみ）/ Wireframe（ライティング結果に静的メッシュのエッジを重ねる、DX12 のみ）を切り替えられる。Window > Content Browser は `Assets/` 以下のモデル（.obj/.gltf/.glb/.fbx）をフォルダ木で表示し、ダブルクリックでカメラ前方 3 m に静的モデルとして追加する（同期ロード）。ビューポート上端中央の Play で編集中のシーン（モデルの行列、ライト、カメラ、平行光源）を保存し、Stop で元に戻す。Play 中は編集パネルが無効になる（Play 中に追加したモデルや粒子などは戻らない）。アプリ側は `ImGuiCoordinator::RegisterTab(ウィンドウ名, カテゴリ, タブ名, 描画関数)` の1行でタブを追加できる。選択中のモデルとライトにはギズモ（ImGuizmo）が出て、ドラッグで移動・回転・拡大できる。上端ツールバーの T/R/S と Local/World（または Space キー）で切り替える。モデルの削除、シーン保存、Undo はまだ無い（TODO.md「ゲームエンジン風ウィンドウ」段4〜5）。
非 D3D12 ではバックエンドのフレーム内部から描画データを取りに来るため、
`Renderer::Render` を呼ぶ前に描画データが確定している必要がある。

ImGui の各描画バックエンド（`imgui_impl_vulkan.cpp` / `imgui_impl_dx11.cpp` / `imgui_impl_opengl3.cpp`）は
`AppFramework.vcxproj` で構成ごとに `ExcludedFromBuild` を切り替える。
`imgui_impl_opengl3.cpp` は GL バージョン文字列の解析に `sscanf`/`strcpy` を使っており `/sdl` が C4996 をエラーへ格上げするので、
vendoring したファイルを書き換えるかわりにファイル単位で `SDLCheck=false` を指定している。

**未対応**: 起動中のブート進捗 UI（`RenderBootFrame` が非 D3D12 では no-op のため、進捗はタイトルバー表示のまま）。

## スクリーンショット（非 DX12 対応）

`debug.screenshot <path>` は **4 バックエンドすべてで動く**（DX12 / DirectX 11 2026-09-09 / OpenGL 2026-09-12 / Vulkan 2026-09-15）。
ネイティブフレーム経路では `RhiBackendFrameDesc::capture`（`RhiBackendCaptureRequest`）でリクエストを渡し、
`ExecuteBackendFrame` が **Present の直前に**バックバッファをステージングテクスチャへコピーして PNG に書き出す。
Present 後では読めない（`DXGI_SWAP_EFFECT_DISCARD` はバックバッファ内容が未定義になる）ため、この順序は必須。

PNG のエンコードは `Source/Renderer/Capture/PngWriter.h` の `SavePngBgra8` に切り出してあり、D3D12 に依存しない。
`TextureCapture`（DX12 経路）も同じ関数を使う。バックバッファが `R8G8B8A8` の場合は WIC の 32bppBGRA に合わせて R/B を入れ替える。

Vulkan だけは実装が一段深い（`Source/Renderer/Backends/Vulkan/VulkanGraphicsDeviceCapture.cpp`）。スワップチェーン画像を `TRANSFER_SRC` へ遷移 → `vkCmdCopyImageToBuffer` でホスト可視バッファへ → 元のレイアウトへ戻す、をプレゼント遷移の直前に記録し、`vkQueueSubmit` 後にフレームフェンスを待ってから読む。R/B の入れ替えはスワップチェーン形式を見て判定する（決め打ちにしない。OpenGL 版が入れ替えを落として全スクリーンショットが赤青反転した前例がある）。

非 DX12 のウィンドウは GDI ベースのキャプチャ（PrintWindow / BitBlt）では内容が読めないことがあるため、
**検証にはこのアプリ内蔵のスクリーンショットを使うのが確実**。

## ネイティブ経路の不透明／透明パス

ネイティブフレーム経路（Vulkan / DX11 / OpenGL）のメッシュ描画は**不透明パス → 透明パス**の2段。
不透明は**ブレンド無効・深度書き込み有効**で提出順、透明は `RhiBackendMeshDrawDesc::transparent` で選び、
**アルファブレンド有効・深度書き込み無効**でカメラから遠い順にソートして描く。
ソートキーはワールド平行移動と `desc.cameraPos` の距離の2乗で、
このレンダラーは行ベクトル規約（`mul(float4(pos,1), u_model)`）のため平行移動は `model[12..14]`。

2026-09-09 まで全ドローが1パス（ブレンド有効・深度書き込み有効）で描かれており、
ガラスや葉のカードが先に描かれるとその背後のジオメトリが深度テストで消えていた。

**アルファテスト（discard）は 3 バックエンドすべてにある**（Vulkan は 2026-09-15 に追加）。
ただし 3 つとも `SurfaceMaterial` にアルファモード/カットオフの情報が無く、サンプルしたテクスチャの
アルファを固定しきい値 0.5 で切っているだけ。Sponza の `alphaMode: MASK` 素材はいずれも
`alphaCutoff` が 0.5 なのでこれで足りるが、薄い布のようにカットオフの異なる素材には足りない。
同じピクセルシェーダを不透明パスと透明パスで共有しているため、テクスチャアルファの低い半透明が
切られうる点も変わっていない。

RHI 経路（DX12 の GBuffer）は 2026-10-20 に glTF の `alphaMode` / `alphaCutoff`（glTF 2.0 仕様 3.9.6）を読むようになった。
`SurfaceMaterial::alphaMode` が `Mask` なら GBuffer PS がアルファを `alphaCutoff` で `clip` し、不透明として扱う。
`Blend` なら常に透明（OIT）パスへ回す。`Opaque` は従来どおり（baseColor のアルファが 1 未満なら透明扱い）。
カットオフは `transparencyParams.z` で渡し、0 のときは判定しない。VRoid アバターの目（虹彩・まつ毛・ハイライトが
BLEND/MASK の板）が白く塗りつぶされていた問題はこれで直った。
シャドウ深度パス（CSM / スポット / ポイント、静的・スキン）も MASK 素材を `ShadowAlphaMask_PS` で切り抜く
（2026-10-20 実装。Release のみビルド確認済み、残り構成のビルドと実機確認は未実施）。
未対応: VSM・テッセレーション・クラスタ（メッシュシェーダー）の影、上のネイティブ 3 経路への `alphaCutoff` の受け渡し。

## ネイティブ経路の空

**3バックエンドとも HDR 環境キューブを引く**（2026-09 以降）。`RhiBackendMeshFrameDesc::environmentCubeSrv`
が 0 でなく解決できたときだけキューブをサンプルし、無ければ従来の解析的な手続き生成に落ちる。HDR パノラマは
非同期に読み込まれるので、起動直後は必ずフォールバック側を通る。

**キューブ側だけトーンマップする**。キューブは線形 HDR 放射輝度なので Reinhard + ガンマ（`c/(c+1)` のあと
`pow(c, 1/2.2)`）を掛けるが、**手続き生成側は掛けない** — そちらは既に表示用の値を返すので、二重に通すと白飛びする。
この分岐は Vulkan `NativeSky.hlsl`、DX11 `NativeSky_PS.hlsl`、OpenGL の空シェーダで同一。

「キューブが有効か」のフラグは、定数バッファを増やさずに既存の空きスロットへ載せている
（Vulkan は専用フィールド、DX11 は `u_sunColor.w`、OpenGL は `u_hasEnvCube`）。


ネイティブ 3 バックエンドは 2026-09-15 まで平坦な灰色クリアしかしておらず、メッシュの上が一様な灰色だった。
現在は 3 つとも解析的な空を描く。**正典は HLSL の `Shaders/Effects/Sky/ProceduralSky/ProceduralSky.hlsli`**
で、DX11 と Vulkan はそれを `#include` する。OpenGL だけは GLSL なので include できず、
`OpenGLShaderSources.cpp` の `kOpenGLSkyFunctionGLSL` がその翻訳を持つ ―― **GLSL 側は 1 コピーだけ**で、
レイマーチの背景とネイティブ空の両方がそれを差し込む。式を変えるときは HLSL と GLSL の両方を直すこと。

データの渡し方はバックエンドごとに違う:

| | 渡し方 | 深度の無効化 |
|---|---|---|
| DirectX 11 | 動的定数バッファ | 深度ビューを束縛しない |
| Vulkan | **プッシュ定数**（112 バイト、保証される 128 以内） | パイプライン状態 |
| OpenGL | uniform | 描画前後で明示的に無効化・復帰（GL は状態機械のため） |

空はそのアタッチメントへの最初の描画なので、専用のレンダーパスは作らず、
Vulkan ではネイティブメッシュのレンダーパスをそのまま共有している。

フルスクリーン三角形は頂点 ID から作る。**OpenGL のメッシュ用シェーダは GLSL 1.20 だが、
空とレイマーチのシェーダは `#version 330 core`** なので `gl_VertexID` が使え、頂点バッファは要らない。

## ネイティブ経路の深度規約（reverse-Z）

カメラの射影行列は **reverse-Z**（`Camera.cpp` の `BuildPerspectiveProjection` が NDC z = 近 1.0 / 遠 0.0 を生成）で、
その view-projection がそのままネイティブ経路の定数バッファへ渡る。したがってネイティブ 3 バックエンドも
**深度クリア 0.0 + 比較 `GREATER_EQUAL`** で統一する（DX12 経路の `RendererGraphicsCommands.cpp` と
`RhiDepthStencilStateDesc::depthCompare` の既定に合わせる）。

2026-09-10 まで Vulkan / DX11 / OpenGL は標準 Z 前提の「クリア 1.0 + `LESS_EQUAL`」のままだった。
手前の面（z≈1.0）を書いた後に奥の面（z≈0.0）が比較を通って上書きするため常に最奥のフラグメントが勝ち、
ネイティブ経路は背面カリングも無効なので**メッシュの裏面が見える**状態になっていた。

OpenGL のみ、`ExecuteBackendFrame` で `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` を
`LoadGlProc` 経由で 1 回だけ解決して適用する。取得できなければ何もしない ――
clip control が無くても GL は clip z ∈ [-1,1] を depth [0,1] に写すだけで、reverse-Z の z ∈ [0,w] は
depth [0.5,1.0]（近 1.0 / 遠 0.5）へ単調に写るため描画は正しく、深度精度が半分になるだけ。

## ネイティブ経路のシェーディング

**ネイティブ経路はレンダーパスのノードグラフを通らない。** `RendererFrameGraph.cpp` の
`UsesNativeBackendFrame` 分岐がパスレジストリを読むのは `RayMarchRenderPass` を探す1箇所だけで、
Shadow / GBuffer / Lighting / SSR / PostProcess の有無も順序も無視される。
GUI の「Render Node Preset」が非 DX12 で丸ごとグレーアウトされるのはこの実態を反映したもので
（`BackendFeatureSupport` が DX12 以外で全機能を false にする）、**DX11 用のパスプリセットを
定義しても描画結果は変わらない**。絵を変えるにはネイティブ経路そのものに手を入れる。

DX11 のシェーダは `Shaders/Native/Dx11/` にある。2026-09-10 まで `Dx11GraphicsDevice.cpp` の
文字列リテラルで、IBL・SSAO・シャドウは `0.10` / `0.92` / `0.88` という定数の係数にすぎず、
鏡面反射項が無かった。現在はファイル化した上で Cook-Torrance の実 PBR になっている
（GGX ＋ 高さ相関 Smith ＋ Schlick。出典は `NativePbr.hlsli` 冒頭）。

2026-10-19 から**ライティング式は 3 バックエンドで共通**。`Shaders/Native/NativePbr.hlsli`・`NativeShadow.hlsli`・
`NativeLighting.hlsli`（AO 合成、IBL 無効時の半球アンビエント、`NativeShadeSurface`）を DX11（FXC）と Vulkan（DXC→SPIR-V）の
メッシュシェーダが include する。OpenGL の GLSL（`OpenGLShaderSources.cpp`）は同じ式の手書き移植で、影は無い。
それ以前の Vulkan / OpenGL は Lambert ＋ 定数アンビエントだったため、DX11 と色が違って見えていた。

**アンビエントは環境キューブマップから引く**（2026-09 以降、Vulkan / DX11 / OpenGL の3経路すべて）。
`RhiBackendMeshFrameDesc` に `ambientCubeSrv` / `environmentCubeSrv` を追加し、`Skybox::InitializeNativeOnly` が
HDR パノラマから 32² のアンビエントキューブと 1024² の環境キューブを生成する。キューブが無い場合のみ定数に
フォールバックするので、起動直後や HDR を持たないシーンは従来どおりの絵になる。

アンビエント用だけ 32² と小さいのは意図的で、**フル解像度のキューブを法線方向にサンプルすると拡散光ではなく
鏡像になる**ため。バックエンドごとの差は次のとおり:

| バックエンド | 実装上の注意 |
|---|---|
| Vulkan | ディスクリプタセットのキーが (albedo, ambientCube, occlusion) の3つ組になる |
| DirectX 11 | `t3` にバインド。`u_featureFlags2.y` がゲート。1×1×6 の白ダミーはクラス変数（静的破棄順の回避） |
| OpenGL | メッシュ用シェーダが `#version 120` なので **`texture()` ではなく `textureCube()`**。ユニット2にバインド |

OpenGL は前提として2つの修正が要った。`CreateRhiShaderResourceView` が `RhiTextureViewDesc` を捨てていて
ビューがテクスチャターゲットを持たず（GL はバインド時に `GL_TEXTURE_CUBE_MAP` か `GL_TEXTURE_2D` かが要る）、
`OpenGLRhiTextureView { texture, target }` を導入して `desc.dimension` から記録するようにした。あわせて
`ToGlType` が `R16G16B16A16Float` に `GL_FLOAT`（32bit）を返していたのを `GL_HALF_FLOAT` に修正した
（Skybox は半精度で面を渡すため、テクセルあたり8バイトのバッファから16バイト読む状態だった）。

**マテリアル AO（materialAo）は4バックエンドすべてで同じ式**になっている。
`ao = lerp(kNativeAoFloor, 1.0, saturate(materialAo))` をアンビエント項とエミッシブに掛ける形で、
DX12 の `rawAo = materialAo * runtimeAo` のうち materialAo 側に一致する。`runtimeAo` 側は
**DX11 / Vulkan / OpenGL の3経路すべてで実装済み**（下記）。適用するかどうかは `RhiBackendMeshDrawDesc::occlusionSlotHasAo`
が決める。**ラスタのマテリアルはテクスチャスロットを1つしか持たない**ため、glTF が occlusion と
metallic-roughness を別画像で持つ場合、`MeshComponent` はスロットに metallic-roughness を入れて
`occlusionStrength` を 0 に落とす。したがってこのフラグは `occlusionStrength > 0` から導く必要があり、
「metallic-roughness テクスチャの有無」から導いてはならない（両者は一致しない）。
FBX ローダは occlusion パスを一切設定しないので、FBX アセットは常にこの「別画像」側に落ちる。

`kNativeAoFloor` は 0.25 の固定値である。DX12 は `u_shadowParams.w` で実行時に調整できるが、
ネイティブ経路のプッシュ定数は Vulkan の保証最小サイズ 128 バイトに達していて追加の余地が無い。
同じ理由で `occlusionStrength` も連続値ではなくオン／オフの2値としてしか効かない。

**直接光への遮蔽（マイクロシャドウ）も3経路に揃えた**（2026-10-05）。DX12 は
`ComputeMicroShadowing(NdotL, ao, u_debugParams.y)`（Chan 2018, "Material Advances in Call of
Duty: WWII"。Filament の `computeMicroShadowing` と同じ式）をシャドウマップの可視性に乗算で重ねて
いるが、ネイティブ経路にはこれが無く、`ao` はアンビエントとエミッシブにしか効いていなかった。
`Shaders/Shared/Common/MicroShadowing.hlsli` はリソースを一切参照しない純粋なスカラ計算なので、
**Vulkan（dxc の `-I <shader root>`）と DX11（`Dx11ShaderInclude.h` のシェーダルート分岐）は DX12 と
同じファイルをそのままインクルードしている**（`NativeShadow.hlsli` のように `Texture2D` /
`SamplerComparisonState` を引数に取るヘッダは移植が必要だったが、こちらは不要）。OpenGL だけは
手書き GLSL なので `computeMicroShadowing()` として翻訳した（`rsqrt`/`saturate`/`lerp` →
`inversesqrt`/`clamp`/`mix`）。`ao == 1` では恒等的に 1.0 を返すため遮蔽の無いジオメトリの絵は変わらない。
強度は `kNativeAoDirectStrength = 0.5`（DX12 の `RenderSettings::aoDirectLightingStrength` の既定値）の
固定値で、`kNativeAoFloor` と同じ理由（`RhiBackendMeshFrameDesc` に口が無い）で実行時に変えられない。
**屋内の明るさには効かない**: 直接光の項にしか掛からないので、日が届かず `direct ≈ 0` の場所では
ほぼ無変化である（屋内は `runtimeAo` がアンビエントを落とすことで効く）。

**DX12 にあってネイティブ3経路に無い遮蔽項は、残り2つとも「適用先が無い」ために欠けている**。
`ComputeSpecularOcclusion`（`SpecularOcclusion` 曲線と `ao^2` を metallic で混ぜ、`HorizonOcclusion` を
畳んだもの）は間接スペキュラに掛かる項だが、ネイティブ経路はプリフィルタ環境キューブも BRDF LUT も
持たず間接スペキュラ自体が存在しない。点光源・スポットのマイクロシャドウ
（`AccumulatePointLights(surf, u_debugParams.y)`）も、ネイティブのプッシュ定数が単一の
ディレクショナルライトしか運んでいないため適用先が無い。どちらもライティングモデルを足す側の
作業で、遮蔽項の移植としては完了している。

**DX11 の実行時 AO（runtimeAo / SSAO）は DX12 と同じシェーダで動く**（2026-10-05）。
ネイティブ経路はフォワードなので DX12 の SSAO パスが読む GBuffer（`SceneDepth` / `GBufferNormal`）が
存在しない。そこで `Dx11DepthNormalPrepass` が**深度＋ワールド法線のプリパスを専用に描き**、
`Dx11SsaoGenerator` がそれを入力に
`Shaders/Effects/AmbientOcclusion/SSAO/{SSAO_VS, SSAO_PS, SSAO_Blur_PS}.hlsl` ——
**DX12 がそのまま使っているファイル** —— を `vs_5_0` / `ps_5_0` でコンパイルして走らせる。
サンプルカーネル・遮蔽の重み付け・バイラテラルブラーは両バックエンドで同一コードであり、
差分はリソースのバインドだけになる。

| 項目 | 内容 |
|---|---|
| 実行順 | 影 → 空 → **AO（プリパス＋SSAO＋ブラー）** → 不透明 → 透明。AO はライティングの入力なので shaded パスより前 |
| プリパス深度 | `R32_TYPELESS`（DSV は `D32_FLOAT` / SRV は `R32_FLOAT`）。リバースZ、クリア 0.0、`GREATER_EQUAL` |
| プリパス法線 | `R16G16B16A16_FLOAT`。DX12 の `GBufferNormal` と同じ形式・同じ符号化（`n * 0.5 + 0.5`） |
| AO ターゲット | `R8G8B8A8_UNORM` ×2（生＋ブラー）、フル解像度。DX12 の `RenderTargetPool` と同じ |
| メッシュ側の受け取り | `t4`（`RuntimeAOTex`）。ゲートは `u_featureFlags.z`、画面 UV 用の逆解像度は `u_featureFlags2.zw` |
| 失敗時 | 1×1 白ダミーを `t4` に束縛しフラグを 0 にするので `runtimeAo = 1.0` に退化する（materialAo のみ） |
| 透明描画 | `runtimeAo` を適用しない。プリパスが透明を除外するため、透明ピクセルの AO はその背後の不透明面の値になる（DX12 も runtimeAo は不透明のみを塗るデファードライティングパスにある） |
| 単体確認 | `SASAMI_DX11_AO_DEBUG=1` で AO をグレースケールで画面に直接描く（`NativeAoDebug_PS.hlsl`） |

**深度微分による法線復元は採らなかった**。TODO.md の Vulkan SSAO 項に「精度が出ない」と記録があり、
`derivative-based TBN` でも同種の問題を踏んでいるため、法線は実際に描いている。
代償として SSAO 有効時は不透明ジオメトリの頂点パスが1回増える（`enableSsao` は既定で真）。

**SSAO のチューニング値はフレーム記述子で渡る**（2026-10-05）。`RhiBackendMeshFrameDesc` に
`runtimeAoRadius` / `runtimeAoBias` / `runtimeAoIntensity` / `runtimeAoThickness` / `runtimeAoQuality`
を追加し、`RendererNativeFrame.cpp` が `RenderSettings` から埋める。DX11 側は
`Dx11SsaoGenerator::Tuning` で受け取るので、UI スライダーが DX11 の絵に効く。
それまでは `kDefault*` に既定値（0.5 / 0.025 / 1.0 / 0.15 / 1）を複製していて、UI は無効だった。

**`runtimeAoIntensity` の既定値を 1.0 → 2.5 に引き上げた**（2026-10-06）。1.0 では「AO は動いているのに
見えない」状態で、Bistro / DX12 ラスタ / SSAO / 露出 1.3（既定）で測ると、8/255 を超えて動くピクセルは
フレームの 14.6% にとどまっていた。同一ラン内の A/B はビット一致（ノイズ床 maxAbs=0）なので、以下は
すべて信号である:

| `runtimeAoIntensity` | 1.0 | 1.5 | 1.75 | 2.0 | 2.25 | 2.5 | 3.0 | 4.0 |
|---|---|---|---|---|---|---|---|---|
| AO オフとの `changed>8` | 14.6% | 19.8% | 23.5% | 25.8% | 27.0% | **27.7%** | 28.4% | 29.2% |
| 同 `maxAbs` | 91 | 94 | 97 | 110 | 119 | **120** | 138 | 164 |

遮蔽の広がりは 2.5 で約 94% 飽和し、そこから先に増えるのは「もともと暗いピクセルの深さ」だけになる。
最悪ピクセルの暗転は 2.0〜2.5 で横ばい（110→120）だが 3.0 以降で加速し（138 → 164）、葉の内部と
窓の見込みが目に見えて潰れる。2.5 はその直前の値。

**この値は RTAO と共有だが、意味が違う**。SSAO（`SSAO_PS.hlsl`）は `1 - occlusion * intensity` の線形倍率、
RTAO（`SWRT_AO_CS.hlsl`）は `pow(ao, intensity)` の指数。RTAO 側でも実測し、2.5 は遮蔽面積を広げる
（暗転ピクセル 34.8% → 43.0%）一方でテールを深くしない（99.9 パーセンタイルの輝度低下 56 → 57）ことを
確認している —— 指数形は ao が 0 付近と 1 付近をほぼ動かさないため。**変更するなら両方式を測り直すこと。**
`RhiTypes.h` の `RhiBackendMeshFrameDesc::runtimeAoIntensity` は `RenderSettings` の既定値のミラーなので
同時に 2.5 へ更新している（片方だけ変えると、記述子を埋め忘れたパスが静かに旧値で描く）。

**計測用に `render.runtimeao.intensity <f>` と `render.runtimeao.method <0|1>` を追加した**（2026-10-06）。
値を振るたびに再起動していては、再起動のたびに時間蓄積パスが引き直されて、解像したい数 LSB の差が
ラン間ノイズに埋もれる。`render.runtimeao` が A/B 用に足されたのと同じ理由である。

**`runtimeAoEnabled` を全バックエンドの実行時 AO のマスタスイッチに統一した**（2026-10-07）。
ネイティブ3バックエンドは `RendererNativeFrame.cpp` で `runtimeAoEnabled` と
`aoMode != MaterialOnly` を **OR** していたため、既定の `aoMode = Hybrid` では
`render.runtimeao 0` が何もせず、「AO off」と称したキャプチャが実は AO on だった
（別条件のフレームとビット一致したことで発覚）。DX12 側も `RayTracedAOOnly` の分岐だけが
生の `ambientOcclusionMode` を見ていて、マスタスイッチが切れていても SWRT AO テクスチャを束縛していた。
`Renderer::GetEffectiveAmbientOcclusionMode()`（`runtimeAoEnabled` が false なら `MaterialOnly` に畳む）
を単一の出所にして、ネイティブの `enableSsao`・DX12 の AO SRV 選択・マテリアル定数の
`effectiveAoMode` すべてがこれを経由する。**既定の挙動が変わる**: `render.runtimeao 0` は
`aoMode` によらず実行時 AO を切る。実機 A/B（1264×681、ノイズ床 0.000% / maxAbs=0〜1）:

| 構成 | aoMode | on↔off の差分 | 可視差分 (>8/255) | maxAbs |
|------|--------|---------------|-------------------|--------|
| DX11 | Hybrid(3) | 85.745% | 32.331% | 36 |
| DX12 | Hybrid(3) | 39.685% | 9.278% | 63 |
| DX12 | RayTracedAOOnly(2) | 39.685% | 9.278% | 63 |

どちらの構成でも `render.runtimeao 0`（aoMode=3）と `render.ao 0` はビット一致する —— AND 意味論が
実際に効いていることの確認である。

**Vulkan の実行時 AO も同じ共有シェーダで動く**（2026-10-05）。構成は DX11 と同型で、
`VulkanAmbientOcclusion`（`VulkanAmbientOcclusion.cpp` が資源と記録、`VulkanAmbientOcclusionPipelines.cpp`
がパイプライン）が **深度＋ワールド法線プリパス → SSAO → バイラテラルブラー** の3レンダーパスを
自前のターゲットに対して走らせる。SSAO とブラーは
`Shaders/Effects/AmbientOcclusion/SSAO/{SSAO_VS, SSAO_PS, SSAO_Blur_PS}.hlsl` ——
**DX12 / DX11 と同じファイル** —— を dxc で SPIR-V にコンパイルしているだけで、移植版は存在しない。
Vulkan 固有なのはプリパスのみ（`NativeAoPrepass.hlsl` / `NativeAoPrepassSkinned.hlsl`、静的・スキンド）。
クラスの形は `VulkanShadowMap` に揃えてあり（`Initialize` / `Destroy` / `IsValid` / `Render`、失敗しても
白ダミーのビューが必ず束縛可能、失敗は非致命）、カスター列も影パスと同一の
`m_nativeShadowCasters`（`BuildNativeCasterList` がフレーム1回だけ構築）を共有する。

| 項目 | 内容 |
|---|---|
| 実行順 | 影 → **AO（プリパス＋SSAO＋ブラー）** → メッシュパス（空＋不透明＋透明）。Vulkan はレンダーパスを入れ子にできないので AO は `RenderMeshFrame` のレンダーパス開始前に完結させる |
| ビューポート | 3パスすべて**負の height**（`SetFlippedViewportAndScissor`）。メッシュパスと同じ D3D 原点にしないとプリパス深度が上下反転し、`SSAO_VS.hlsl` の D3D 規約の UV↔NDC 対応も崩れる |
| プリパス深度 | `VK_FORMAT_D32_SFLOAT` ＋ `SAMPLED_BIT`。リバースZ、クリア 0.0、`GREATER_OR_EQUAL`（メッシュパイプラインと同一） |
| プリパス法線 | `VK_FORMAT_R8G8B8A8_UNORM`、符号化は `n * 0.5 + 0.5`（`GBufferEncode.hlsli` と同じ） |
| AO ターゲット | `R8G8B8A8_UNORM` ×2（生＋ブラー）、スワップチェーン解像度。DX12 の SSAO/SSAOBlur と同形式 |
| 同期 | 各レンダーパスに `dstSubpass = VK_SUBPASS_EXTERNAL` の依存を張る。最終レイアウト遷移だけでは後続のテクスチャ読みと順序付けされない |
| メッシュ側の受け取り | `t4`/`s4`（`g_runtimeAo`、set0 の既存テクスチャセットに追加）。ゲートは `g_frame.aoParams.x`、画面 UV 用の逆解像度は `aoParams.yz`。`VulkanNativeMeshFrameConstants` に `aoParams` を1つ足した（プッシュ定数は 128 バイト上限で余地が無い） |
| 失敗時 | `AoImageView()` が 1×1 白を返し `aoParams.x` が 0 になるので `runtimeAo = 1.0` に退化する（materialAo のみ） |
| 単体確認 | `SASAMI_VK_AO_DEBUG=1` で AO をそのままバックバッファへ blit する（オーバーレイは上に残る）。`debug.screenshot` でも取れる |

**チューニング値は DX11 と同じく `RhiBackendMeshFrameDesc` から受け取る**（2026-10-05、
`RenderNativeAmbientOcclusion` が `VulkanAmbientOcclusion::Params` へ転記）。`enableSsao` は既定で真なので、SSAO 有効時は DX11 同様
不透明ジオメトリの頂点パスが1回増える。スキンド遮蔽体の上限は影パスと同じ 16 体。

**OpenGL の実行時 AO は唯一「移植版」である**（2026-10-05）。構成（深度＋ワールド法線プリパス →
SSAO → バイラテラルブラー）と式は DX12 と同一だが、**シェーダだけは共有できない**。
DX11 は `d3dcompiler`、Vulkan は dxc で**同じ HLSL ファイルをそのままコンパイルしている**のに対し、
この OpenGL コンテキスト（GLSL 1.20 / 330 core）は HLSL も SPIR-V も受け付けないため、
`OpenGLShaderSources.cpp` に `kOpenGLSsaoFS` / `kOpenGLSsaoBlurFS` / `kOpenGLAoPrepass*` を
**GLSL へ手で翻訳したものを置いている**。したがって `SSAO_PS.hlsl` / `SSAO_Blur_PS.hlsl` を直したら
この2本も手で追従させる必要がある（他の3バックエンドは自動で追従する）。

**コンピュートシェーダが無いことは SSAO の障害にはならなかった**。TODO.md のこの項目は
「OpenGL は `#version 120`/`330 core` でコンピュートが使えない（4.3以上が必要）」ことを着手前の
判断材料として挙げていたが、**DX12 の SSAO 自体がコンピュートではなくフルスクリーンのラスタパス**
（`SSAO_VS.hlsl` が3頂点を描く）なので、フラグメントシェーダで等価に書ける。コンテキストの引き上げは
不要だった。

| 項目 | 内容 |
|---|---|
| 実行順 | **AO（プリパス＋SSAO＋ブラー）** → 空 → 不透明 → 透明 → (AO デバッグ) → オーバーレイ。AO は自前の FBO に描いてデフォルトフレームバッファに戻すので、`ExecuteBackendFrame` が先にクリアしたバックバッファには触らない |
| プリパス深度 | `GL_DEPTH_COMPONENT32F` テクスチャ。**デフォルトフレームバッファの深度バッファはテクスチャとしてサンプルできない**ため、このオフスクリーン対が存在する。リバースZ、クリア 0.0、`GL_GEQUAL` |
| プリパス法線 | `GL_RGBA16F`、符号化は `n * 0.5 + 0.5`（DX12 の `GBufferNormal` と同じ） |
| AO ターゲット | `GL_R8` ×2（生＋ブラー）、レンダー解像度 |
| UV 規約 | **GL のテクスチャ原点は左下**なので、`SSAO_VS.hlsl` / `WorldPosReconstruction.hlsli` の D3D 用 Y 反転（`1 - uv.y`、`float2(0.5, -0.5)`）を**翻訳版では外してある**。アルゴリズム上の差はこの1点だけ |
| 深度範囲の前提 | `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` が**必須**。無い場合クリップ z は深度 [0.5, 1.0] に乗り、描画自体は正しいが深度からワールド位置を復元できないので **SSAO をスキップする**（`m_depthZeroToOne`） |
| メッシュ側の受け取り | `u_runtimeAoTexture`（テクスチャユニット3。0 albedo / 1 occlusion / 2 ambient cube）。フォワードなので画面 UV は `gl_FragCoord.xy / u_renderSize` で作る |
| 失敗時 | `u_hasRuntimeAoTexture = 0` で `runtimeAo = 1.0` に退化する（materialAo のみ）。`m_ssaoValid` はフレーム単位なので、生成を飛ばしたフレームが**前のカメラ位置の AO を使い回すことはない** |
| カットアウト | プリパスもアルベドのアルファで `discard` する。これをしないと葉や柵の板が四角形まるごと遮蔽体になる |
| 透明描画 | プリパスは不透明のみ（DX12 の GBuffer と同じ）。透明ピクセルの `runtimeAo` はその背後の不透明面の値になる |
| 単体確認 | `SASAMI_GL_AO_DEBUG=1` で AO をグレースケールでバックバッファに描く（`OpenGLGraphicsDeviceAoDebug.cpp`、オーバーレイは上に残る） |

**チューニング値は DX11 / Vulkan と同じく `RhiBackendMeshFrameDesc` から受け取る**（2026-10-05、
`OpenGLGraphicsDeviceSsao.cpp` の `kSsao*` 定数を廃止して `desc.runtimeAo*` を読む）。

**GL プログラムのコンパイルは `BuildGlProgram` に1本化した**（`OpenGLProgramBuilder.h`）。
OpenGL のバックエンドは「compile → ログ → link → ログ」を各ファイルで複製していて、AO の3プログラムで
さらに増えるところだった。ソースと属性バインドしか変わらないので、その手順だけを切り出してある。

**ネイティブ経路のライティングはモデル空間の法線で陰影を付けている**（既存の既知の簡略化。
`kOpenGLNativeMeshVS` が逆転置ワールド行列の補正を省いており、`v_normal` はモデル空間のまま）。
AO プリパスは SSAO が成立しないため**ワールド法線を実際に計算している**ので、この1フレーム内に
2つの法線規約が同居している。AO 側が正しいが、ライティング側を直すと既存の絵が変わるため本項目では触っていない。

**DX11 のシェーダコンパイルにはインクルードハンドラが必要になった**（`Dx11ShaderInclude.h`）。
`D3D_COMPILE_STANDARD_FILE_INCLUDE` は引用形式のインクルードを**インクルード元ファイルのディレクトリ
だけ**で解決するため、`SSAO_Sampling.hlsli` の `#include "Shared/Common/WorldPosReconstruction.hlsli"`
のようなシェーダルート相対のインクルードが開けない（DX12 は dxc に `-I <shader root>` を渡して解決している）。
`CompileDx11Shader` は自前のハンドラを渡し、**インクルード元 → シェーダルート**の順で探すようにした。
これで DX11 が共有シェーダをフォークせずに使える。

**落ち影は単一カスケードで動作する**（`Dx11ShadowMap` ＋ `NativeShadow.hlsli` の3x3 PCF）。
`render.nativeshadow <0|1>` で実行時にトグルできる（既定オン）。

深度スラブは**ライト平面から始める**こと（`shadowNear = 0.001`）。`LightSystemShadow.cpp:252` が
DX12 の CSM について書いているとおり、ライトとシーンの間にある壁・屋根・建物上層も影を落とすので、
near をシーンに寄せると必要なキャスターを捨ててしまい、全面が影判定になって直接光が消える。

ortho ボックスは**カメラ中心**に置く。`Math::BuildDirectionalLightViewProjection` は原点中心の行列を
返すので、ネイティブ経路ではワールドを `-center` で事前平行移動して追従させている。これが無いと
`orthoHalf` の島の外に影が一切付かない（原点から31ユニットの街路で影の寄与が実測 0.000% だった）。
中心は1テクセル単位にスナップする — しないと影の縁が毎フレーム這う。

シェーダの探索パスは `Shaders/`（正典）→ `Source/Renderer/Shaders/`（レガシー）の順。
以前は後者しか見ておらず、ファイル版シェーダの読み込みは必ず失敗していた。

## 法線マップの接空間（タンジェント無し）

`Vertex`（`Source/Renderer/Structures/Vertex.h`）は `position` / `normal` / `color` / `uv` の4つだけで、
**タンジェントを持たない**。そのため接空間は `GBufferEncode.hlsli` の `PerturbNormal` が
**画面空間微分から再構築**している（Schuler, "Normal Mapping Without Precomputed Tangents"）。
`OpaqueGBuffer_PS` と `MeshShaderGBuffer_PS` の両方がこの関数を通る。

**退化判定は必ずスケール相対にすること。** `T ≈ ddx(worldPos) × ddx(uv)` なので `dot(T, T)` は
**ピクセルフットプリントの4乗**で変化する。ここを固定の絶対値と比較すると、UV が退化していないのに
**カメラが近づいただけで「退化」と誤判定**され、法線マップが無効化される。

実際 2026-09-23 まで `maxLen <= 1e-12` という絶対値判定が入っており、約5mで `dot(T,T)≈1.1e-10`、
約0.5mで `≈1.1e-14` となるため、**近景で法線マップが切れて面の途中にくっきりした継ぎ目が出ていた**
（しきい値をまたぐ線がそのまま画面に出る）。`rsqrt(maxLen)` で T・B は正規化されるので結果自体はスケール不変であり、
このガードはゼロ除算を防ぐためだけのもの。現在は無次元の判定になっている:

```hlsl
if (maxLen <= 1e-8 * max(dot(dp1,dp1), dot(dp2,dp2)) * max(dot(duv1,duv1), dot(duv2,duv2))) {
    return n;
}
```

同種の罠は `ddx`/`ddy` を使う箇所すべてにある（SSAO、SSR、視差マッピング等）。**画面空間微分から作った量の
しきい値は、同じオーダーの量との比で書くこと。**

なお法線マップは BC5（2チャンネル）なので、Z は青チャンネルを信用せず
`sqrt(1 - dot(xy, xy))` で再構成している。

## マテリアルテクスチャの色空間

基本色テクスチャは `IRHIDevice::CreateRhiTexture2DFromRgba8` に `RhiFormat::R8G8B8A8UNormSrgb` を渡して
**リソース自体を sRGB フォーマットで作る**（既定は `R8G8B8A8UNorm`）。
型付きリソースは別フォーマットのビューを作れないため、ビュー側だけ sRGB にしても decode されない。
2026-09-09 までこの経路は UNORM 固定で、非 DX12 の絵は基本色がリニア扱いされて白飛びしていた。

**このレンダラーの機能はほぼすべて DirectX 12 専用**。`GraphicsDevice.h` がレンダラー側の型語彙を `D3D12_*`/`ID3D12*` に
typedef しており、Vulkan / DX11 / OpenGL は D3D12 互換面の 15 メソッドを `E_NOTIMPL` で返す。
非 DX12 バックエンドではレンダーグラフ自体が動かず、`UsesNativeBackendFrame` が短絡して
クリア色・メッシュ描画 1 件・レイマーチ 1 件だけの経路になる。

`BackendFeatureSupport`（`Source/Renderer/Config/`）がこれを一箇所に集約する。
バックエンド単位の静的プリセットと、デバイスが報告する capability を突き合わせて確定し、
起動時に未対応の機能と理由を 1 行ログに出す。UI（`UI::Gate`）はこれを読んでコントロールをグレーアウトし、
ホバー時に理由（"DirectX 12 only" / "requires DirectX Raytracing Tier 1.0" / "requires Mesh Shader Tier 1"）を出す。
非表示にはしない。何が存在するかは見えたままにして、操作だけを止める。

対象は機能トグル、パスビルダーの 15 項目、Render Type コンボ。
あわせて DX12 のメッシュシェーダティア判定をデバイス初期化へ移し、`RhiBackendCapabilities::supportsMeshShaders` を
正しく埋めるようにした（従来は PSO キャッシュだけが見ており、capability は false のままだった）。

**実測（2026-09-07）**: DX12 は `unsupported = none`（DXR・メッシュシェーダとも対応）。
Vulkan ビルドは 11 機能すべてが "DirectX 12 only"。
2026-09-08 に D3D12 専用の初期化 3 箇所（`InitializeFrameInfrastructure` / `FinalizeCoreInitialization` /
`RegisterDeferredInitTasks`）を `UsesNativeBackendFrame` でスキップするようにし、
**Vulkan ビルドは完全に起動して約 50fps で描画する**ようになった（起動時失敗 13 件 → 0 件）。
readyState のフラグは `MarkRasterSubsystemsReady()` に切り出してネイティブ経路でも発行する。
グレーアウトの見た目は 2026-09-08 の UI バックエンド抽象化により全バックエンドで確認できるようになった。

## デバッグレイヤ

D3D12 デバッグレイヤは `Dx12GraphicsDeviceInit.cpp` の `#ifdef _DEBUG` 内でのみ有効。
**Release ビルドのログにはデバッグレイヤのメッセージが 1 件も出ない**ので、検証は Debug ビルドで行うこと。
GPU ベースの検証はさらに `SASAMI_GPU_VALIDATION=1` でオプトイン（初回フレームが数十秒止まるため既定オフ）。

2026-09-16 時点で Debug 起動時に出るメッセージは **`sev=1` のエラーが 0 件**。
残るのは id=1328 / 820 / 821 / 745（sev=2、いずれも情報系）だけ。

id=527（バリアの Before state 不一致）は 55 件あったが解消済み。原因は一つで、RHI が
`RhiResourceState::ShaderResource` を **`NON_PIXEL | PIXEL`（0xC0）** として作るのに、宣言側が
`PIXEL` だけ（0x80）と言っていたこと。新しいバリアを書くときは、RHI 経由で作った
シェーダ読み取り可能なリソースは 0xC0 であると考えること。

## UI

**編集は確定時にコミットされる（2026-09-06）**。スライダー・カラー・位置ドラッグは、
ドラッグ中はレンダラーへ反映されず、離した瞬間（Ctrl+クリックでの直接入力なら確定時）に一度だけ反映される。
毎フレームのセッター呼び出しが無くなるので、プローブグリッドの再構築やレンダーターゲットの再確保、
シャドウキャッシュの無効化といった重い処理が編集中に連続で走らなくなる。

判定は UI ラッパ層（`Source/AppFramework/UI/`）だけに置いてある。呼び出し側の書き方は従来どおり
「true が返ったら setter を呼ぶ」で、true の意味が「値が動いた」から「編集が確定した」に変わっただけ。
`UiEditBuffer` が編集中の 1 ウィジェットの値を保持する（これが無いと、毎フレーム getter で読み直す
呼び出し側の作りではドラッグ中に値が巻き戻ってスライダーを操作できない）。

部品は 1 ファイル 1 種類: `UiVolume`（float）、`UiIntVolume`（int）、`UiColor`（RGB/RGBA）、
`UiVector3`（位置）、`UiAngle`（角度）。
チェックボックス・ラジオ・コンボは対象外。クリックした瞬間が確定であり編集途中が存在せず、
ImGui 側も `IsItemDeactivatedAfterEdit()` はこれらで誤検知しうると明記している。

## Project Layout

| Path | Role |
| --- | --- |
| `Source/` | C++ の engine、renderer、platform 実装 |
| `Shaders/` | ファーストパーティ HLSL/HLSLI の正式な配置場所 |
| `Assets/` | サンプルモデル、テクスチャなど |
| `Libraries/` | 外部依存。`Libraries/NRD/Shaders` はサードパーティ側の構成なので移動対象外 |
| `Tools/` | DXC などの開発補助ツール |
| `Samples/` | `PBRApp`、`RayMarchApp`、`FluidApp`、`WaveApp`、`PerfLabApp` などのサンプルアプリ |
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
- **状態遷移はグラフが単独で所有します。** パス内から `ResourceBarrier` を直接発行するとグラフのトラッカーが実態とズレ、次の遷移でデバッグレイヤが before-state 不一致を報告します。`CopyResource` を行うパスは `Read` / `Write` ではなく `RenderGraphBuilder::ReadAsCopySource` / `WriteAsCopyDest` を宣言してください。これらは `reads` / `writes` への登録を維持したまま遷移先の状態だけを `COPY_SOURCE` / `COPY_DEST` に変えるので、依存関係とハザード解析は通常の read/write と同じに保たれます。
- `IRhiDevice` は中立な下位 RHI、`IRHIDevice` は DX12 feature path との互換面を残す移行中の上位インターフェースです。完全な feature path は DX12、他バックエンドは native fallback path です。

### `Renderer/Runtime/` の責務分割

`Renderer::Render()` は元々1本の巨大な関数でしたが、**「フレームの順序を決める」責務だけを
`RendererFrameGraph.cpp` に残し、各段が「何をするか」は1ファイル1責務で切り出してあります。**
いずれも設定・シーン状態・自分自身の状態から判断する処理で、フレームの順序とは独立しているためです。

| ファイル | 責務 |
| --- | --- |
| `RendererFrameGraph.cpp` | `Render()` 本体。フレームの開始/終了とパス登録・グラフ実行の**順序**のみ |
| `RendererNativeFrame.cpp` | 非 DX12 バックエンドのフレーム全体（ノードグラフを通らない別経路） |
| `RendererCameraBudget.cpp` | シャドウ構成と描画アイテム数から、フレームのカメラ定数バッファ所要スロット数を算出して確保 |
| `RendererFrameTargets.cpp` | フレームのレンダーターゲット確保と、レンダーグラフへの外部リソース登録（インポート） |
| `RendererPostProcessPasses.cpp` | 画面空間ポストプロセス（light shaft / lens flare / FXAA）への設定反映と初回登録 |
| `RendererDrawCallbacks.cpp` | 段4: 静的メッシュの描画／シャドウ描画コールバック構築と SWRT 機能ゲート解決 |
| `RendererPassServices.cpp` | 段5: パス入力・サービス構築、スキンド描画コールバック、実行コンテキスト組み立て |
| `RendererComputeQueue.cpp` | クロスキュー compute フェンスの待機と signal（同じメンバを扱う一対の責務） |
| `RendererGIBakeStep.cpp` | 要求済み GI プローブベイクを1フレーム分進める状態機械（待機・BVH 更新・ディスパッチ・連続モード再起動） |
| `RendererGI.cpp` | GI ベイクの要求／状態・ログ／プローブキャッシュ I/O ／グリッド適合（フレーム中には走らない側） |
| `RendererGraphicsCommands.cpp` | 個々のコマンド記録シーケンス（遷移、クリア、トーンマップ、present など） |
| `RendererInitialization.cpp` | レンダラーの初期化 |
| `RendererPassSequence.cpp` | パスの挿入・置換・プリセット構成 |
| `RendererScreenshot.cpp` | スクリーンショット予約状態の管理とフレーム送信前後のフック |

段4・段5 が構築するクロージャはレンダーパスグラフ経由で後から呼ばれるため、フレームローカルな
状態は `FrameRenderContext`（`FrameRenderContext.h`）を通して受け渡します。詳細な理由は同ヘッダの
コメントを参照してください。

## Implemented Feature Summary

| Area | Current implementation | Backend scope |
| --- | --- | --- |
| Raster | Deferred opaque PBR / Toon (5 GBuffer + depth) + forward transparent PBR、weighted-blended OIT、リニア空間シェーディング（sRGB は HW デコード）、inverted-hull 輪郭線 | DX12 feature path |
| Geometry | Static/skinned mesh、tessellation、mesh shader/meshlet debug、クラスタ DAG LOD | DX12。static/skinned の簡易描画は Vulkan/DX11/OpenGL にも対応 |
| Virtual geometry | CPU DAG 構築・キャッシュ、GBuffer/CSM クラスタ描画、CSM の GPU 選択 + ExecuteIndirect、feedback/LRU 常駐管理、RAM page store + 96MB GPU page pool | DX12 mesh shader path。GBuffer の GPU 駆動化と disk paging は未実装 |
| Shadows | Directional CSM/VSM、spot 最大8灯、point 最大4灯×6面、screen-space contact shadow、SWRT directional shadow | DX12 feature path |
| AO | Material AO、SSAO + blur、SWRT AO、Hybrid、direct-light micro-shadowing、UE 風 MinOcclusion フロア(直接光・間接光で共通)、Frostbite horizon occlusion | DX12 feature path |
| Reflections | SSR、SWRT reflection、temporal/A-Trous denoise、IBL fallback | DX12 feature path |
| Ray tracing | CPU SAH BVH + GPU SWRT、legacy/ReSTIR 経路、DXR BLAS/TLAS/pipeline/SBT/dispatch | DX12。Vulkan は RHI RT primitives + opt-in smoke test まで |
| GI | SWRT で更新する L2 SH irradiance probe grid、有限 bake、可視性近似、sidecar cache、debug probe spheres | DX12 feature path |
| Environment | HDR/LDR skybox、IBL、procedural sky、volumetric cloud | DX12 feature path（native fallback は簡易機能） |
| Simulation | GPU heightfield fluid surface、GPU billboard particle system | DX12 feature path |
| Voxel occupancy | シーン不透明形状の 1bit 表面ボクセル化（CPU 参照 + GPU CS、ビット一致を self-test）、スライス/投影ビューア | GPU 版は DX12 のみ（CPU 参照は全環境） |
| Runtime | Progressive Boot、Job System、非同期アセットロード、shader/mesh/GI cache、profiling、debug remote control | 共通基盤。GPU 機能は backend caps でゲート |

## Build

Visual Studio 2022 で `SasamiRenderer.sln` を開き、`x64` + `Debug` または `Release` を選んでビルドします。

Developer Prompt からの例:

```bat
msbuild SasamiRenderer.vcxproj /p:Configuration=Debug /p:Platform=x64
msbuild SasamiRenderer.vcxproj /p:Configuration=Release /p:Platform=x64
```

`SasamiRenderer.vcxproj` はレンダラーの static library です。実行確認にはソリューションをビルドし、`PBRApp` などのサンプルをスタートアッププロジェクトに指定してください。バックエンド別構成は `Debug_Vulkan` / `Release_Vulkan`、`Debug_DX11` / `Release_DX11`、`Debug_OpenGL` / `Release_OpenGL` です。Vulkan 構成には `VULKAN_SDK` が必要です。出力は `Build/bin/<Platform>/<Debug|Release>/`（非 DX12 はその下の backend サブディレクトリ）に生成されます。

必要環境は Windows 10/11 SDK、C++23 対応 MSVC、DXC です。D3D12 Debug Layer / GPU-based validation を使う場合は Windows の「Graphics Tools」もインストールしてください。

Debug では D3D12 Debug Layer が既定で有効です。GPU-based validation（GBV）は初回フレームでの全シェーダ計装により数十秒の一回性ストールを生むため既定でオフになり、環境変数 `SASAMI_GPU_VALIDATION=1` を設定した場合のみ有効化されます。

2026-09-20 の監査では、増分 Debug build 直後の一度だけ `RenderPassBuilderCatalog::Find` で起動時 crash が発生し、ソリューション全体の Rebuild 後は同じ CLI smoke を完走しました。再現しないソース不具合とは断定せず、構成やヘッダをまたいだ変更後に同種の起動 crash が出た場合は、まず対象構成を Clean/Rebuild して mixed/stale object を除外してください。

## Progressive Boot（起動の非同期・段階的初期化）

起動時のフリーズ（Windows「応答なし」）を解消するため、初期化はメッセージポンプを止めずに段階的に実行されます（2026-08-20 導入）。

- **即時ウィンドウ表示**: `ApplicationCore::Run()` はウィンドウ表示後すぐメッセージループに入り、`StartupCoordinator`（`Source/AppFramework/Boot/`）がブートフェーズ（`CreatingRendererCore → LoadingWithBootScreen → SceneReady → Running`）を駆動します。
- **初期化タスクの並列/時分割実行**: `InitTaskScheduler` + `MainThreadTaskPump`（`Source/Foundation/Boot/`）が初期化タスクを依存関係つきで管理し、バックエンドの `supportsThreadedResourceCreation`/`supportsThreadedPipelineCreation` capsに応じて JobSystem ワーカーまたはメインスレッド時分割（ブート中100ms/フレーム、シーン表示後4ms）で実行します。SWRT/GI/流体/パーティクル/DXR は `RendererReadyState` のreadyフラグ公開後にフレームグラフへ順次参加します。
- **ローディングUI**: ブート中は `Renderer::RenderBootFrame` + `BootProgressWindow`（ImGui）が項目別進捗を表示し、シーン表示後は未完了タスクのみ右上に小型表示します（ImGuiオーバーレイ非対応バックエンドはタイトルバーにテキスト進捗）。
- **アセットの非同期ロード**: `AsyncAssetLoadService`（`Source/AppFramework/Loader/`）が glTF/OBJ パース+テクスチャデコード、HDRスカイボックスデコード+IBL事前生成をワーカーで実行します。`StaticModel::LoadModelAsync` / `ApplicationCore::LoadSkyboxAsync` で利用し、完了したアセットはフレーム側で自動採用されます（`MeshComponent::MeshLoadState` でポーリング可能）。
- **SWRT BVH の非同期ビルド**: `GpuSoftwareRayTracer::UpdateScene` はシーンスナップショットを取ってワーカーでSAHビルド（メッシュ単位 `ParallelFor` 並列）し、完了後の呼び出しでGPU公開します。ビルド中は旧BVHで描画継続（初回はSWRT系パスがフォールバックにスキップ）。
- **DXR AS の遅延構築**: DXRのBLAS/TLAS再構築はシーン提出時ではなく、ハードウェアRTを実際にディスパッチする直前に遅延実行されます（未使用時はコストゼロ）。
- **シェーダキャッシュの全面適用**: 従来 `RenderPipelineStateCache` のみだった `.cso` ディスクキャッシュを、SWRT/GI/流体/パーティクル/DXRライブラリのアドホックDXCコンパイル約22本にも適用（`ShaderCompilationService::GetOrCompileShaderBytecodeDxc`）。ウォーム起動ではランタイムコンパイル0本になります。
- **スカイボックス cubemap の GPU 生成**: `SkyCubemapGenerator`（`Source/Renderer/Scene/`）が equirect HDR → cubemap 変換とミップ連鎖構築を2本のコンピュートシェーダ（`Shaders/Compute/Sky/SkyCubemapFromEquirect_CS.hlsl` / `SkyCubemapDownsample_CS.hlsl`）で行います。face 2048・6面・RGBA16F・1x1までのミップ連鎖で、`Skybox::UploadHdrSkyboxTexture` は **80.429〜82.466ms**（2026-08-31 Release/DX12実測。過去のDebug実測342.8ms、CPU版 `RendererMathUtility::GenerateSkyCubemapFromEquirect` 9925ms）。失敗時は CPU パスへフォールバックし、失敗はラッチして毎フレーム再試行しません（成功時はラッチしないので環境マップの再読み込みでも GPU パスを使います）。**必要な SRV ディスクリプタは24個で、枯渇するとフォールバックして無言で約14.6秒かかります**（下の Descriptor Heap Budget 参照）。2026-08-31に既定のBistro HDRを水平・面境界・天頂方向からCLIキャプチャし、明瞭な継ぎ目や極の破綻がないことを目視確認済みです（画素単位の定量検査は未実施）。
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

## Virtual Geometry

**デバッグ可視化（2026-09-06）**: `render.clusterdebug <0|1|2>` で 0=off、1=クラスタ単位、
2=グループ単位（DAG の各レベルでまとめて簡略化される集合）の色分けができる。
GBuffer の albedo だけを整数ハッシュ色で置き換えるので、法線・深度・マテリアルは有効なままライティングも効く。
メッシュシェーダが `PSInput` に `nointerpolation uint2 clusterInfo` を載せるだけで、ルートシグネチャは変更していない。

**中間ターゲットのダンプ（2026-09-06）**: `debug.dumptarget <name> <path>` で
`backbuffer` / `shadow0`〜`shadow3` / `gbuffer.albedo|normal|material|emissive|specular` を PNG に保存できる。
完了待ちは `debug.screenshot.status` を共用。比較は `Tools/Debug/imgdiff.ps1`（B/G/R の 3 チャンネルすべてを見る。1 チャンネルだけの差を「一致」と誤判定しないため）。
深度は reverse-Z のまま固定マッピングでグレースケール化する（自動正規化すると A/B 比較が成立しないため）。

これで最終合成画像には出ない差を評価できる。影の LOD しきい値 1 と 4 の比較では、
カスケード0 は完全一致、カスケード2 で変化画素 0.177%（最終画像では 0.00%）だった。

**ゴールデン画像の回帰ゲート（2026-10-19）**: `Tools/Debug/golden.ps1 -Mode capture|check`。PBRApp をヘッドレス・固定 dt（1/60 秒）で起動し、`app.ready` を待ってから `app.ui hide`。`Tools/Debug/golden-views.json` の各視点でキャプチャして、`Tools/Debug/Golden/<Config>/` のベースラインと `imgdiff.ps1 -MaxChangedOver8Pct` で比較する。終了コードは 0=合格、1=不合格かベースラインなし、3=PBRApp が既に起動中、4=起動またはパイプの失敗。ベースラインは GPU・ドライバ・構成に依存するので gitignore（各マシンで capture し直す）。実測（Debug、Bistro）: 2 視点（IBL 0/1）とも、実行内・実行間のどちらも `changed=0.000% / maxAbs=0`。露出 1.4 を混ぜると `changed>8=70.1%` で不合格になる。

**影専用の LOD しきい値（2026-09-06）**: クラスタ LOD のしきい値をメインビューと影カスケードで分けた。
`clusterLodThresholdPx`（メイン、単位は画面ピクセル、既定 1）と `clusterShadowLodThresholdPx`
（影、単位は影マップのテクセル、既定 4）。CLI は `render.lodthreshold` と `render.shadowlodthreshold`。
両方とも ini に永続化される。しきい値の配線は元から3つの入力構造体でビュー単位に分かれており、
`RendererFrameGraph` の代入2行が同じ値に縛っていただけなので、HLSL とクラスタ側の実装は変更していない。

Bistro / Release の実測: 影 1 テクセルで合計 9.82-10.47ms（95-102fps、Shadow 6.42-6.46）、
影 4 テクセルで 6.11-7.82ms（128-164fps、Shadow 2.88-4.20）。
最終合成画像は影 1 と 4 で完全一致（差分 0.00%）、メインビューも従来経路とピクセル一致を維持している。

**シャドウパスの内訳＝選択 CS が支配的だが、その中身は走査ではない（2026-09-17）**:
`render.clusterselectonly 1` はカスケードごとの選択 CS だけを走らせて間接描画をスキップする
診断トグルで、`render.gputime` の Shadow がそのまま「選択だけの時間」になる（シャドウマップは
空になるので画は壊れる。確認用）。Bistro / DX12 Release / 同一カメラ・同一実行内での A/B
（`shadowdistance=48`、`shadowlodthreshold=4`、可視クラスタ 6914）は、通常 5.35ms に対し
選択のみ 4.25ms で、**描画の実費は 1.10ms（21%）しかない**。

残る 4.25ms が何なのかを `render.clusterselectstats 1`（選択 CS に BVH ノード数・走査階層数・
葉で読んだクラスタ desc 数を数えさせる診断カウンタ。既定オフ）で切り分けた結果、
**この 4.25ms は選択 CS が実際に行う仕事とは無関係の固定費**だと分かった。LOD しきい値を
1 → 100000 まで振ると:

| `shadowlodthreshold` | 可視クラスタ | `examined` | `nodes` | 選択のみの Shadow |
|---|---|---|---|---|
| 1 | 8997 | 30613 | 24772 | 4.17 / 4.26ms |
| 4 | 6914 | 22942 | 24430 | 4.35 / 4.24ms |
| 16 | 4605 | 12988 | 23801 | 4.22 / 4.16ms |
| 64 | 2406 | 5417 | 23249 | 4.21 / 4.29ms |
| 100000 | 644 | 644 | 22856 | 4.06 / 4.25ms |

葉で読むクラスタ desc は **47 倍（30613 → 644）** 変わり、可視クラスタは 14 倍変わるのに、
時間は 4.2ms から動かない。走査自体も `levels=3` / `nodes` 約 2.3 万（4 ビュー合計、
`instances=6788`）と小さく、しきい値を変えてもノード数は 8% しか減らない。
**つまり床は「BVH 走査」でも「葉のクラスタ処理」でもない**。`render.shadowdistance` を
400 → 25 に振っても選択のみは 4.28 / 4.71 / 4.38ms で動かない。

残る候補は Shadow パスのスコープに入っている**ビューあたりの固定費**で、
4 枚ぶんの 4096² デプスクリア、ビューあたり 128KB の要求ビットセットのリセットと
リードバックコピー、ディスパッチ／バリア／ステート遷移のオーバーヘッドのいずれか（または複合）。
**どれなのかはまだ切り分けていない**。2026-09-11 にシャドウマップを 4096→2048 にすると
7.87→5.32ms（-2.55ms）だったことは面積に比例するクリア／フィル成分の存在を示唆するが、
それだけでは断定できない。次の一手はシャドウマップ解像度を実行時ノブにするか、
クリア単体にタイマーを入れて分離すること。**カスケード間での走査共有は、
測定上コストがほぼゼロの処理を減らす最適化になるので着手しない。**

**ストリーミング遅延の計測（2026-09-06）**: `render.streamstats` の `wait=last/max/avg` が、
グループが初めて要求されてから常駐になるまでの待ちフレーム数を示す。常駐キュー側の待ちのみで、
要求集合の readback 遅延（フレームスロット数 = 2）は別途加算が必要。
Bistro / 予算 2500 / 保持 60 では、カメラを振った直後のロードが平均 306 フレーム（最大 500）待っており、
タイトな予算では新規要求が飢餓状態になることが分かる。予算無制限なら待ちは 0。

**撤去済み（2026-09-06）**: 旧 `MeshletBuffer` と「Meshlet Debug View」表示を削除した。
デバッグ表示は GBuffer パスでフラグが捨てられており操作しても無効、デバッグ用 PSO も呼び出し元ゼロ、
`EnsureMeshletsBuilt` も外部呼び出し元ゼロの死にコードだった。クラスタ単位の可視化は別途新規に作る（TODO 参照）。

**既定で有効（2026-09-06）**。Release / Bistro で計測したところ、従来の頂点シェーダ経路は 76.6ms（13.1fps）、
クラスタ経路は 15.4ms（64.7fps）で、しきい値 1px では画像がピクセル一致する。画質を落とさず 5 倍速くなるため既定を ON にした。
Mesh Shader 非対応の GPU では `Renderer::BuildRenderPassExecutionPolicy` が PSO の生成失敗を見て自動的に頂点シェーダ経路へ落ちる。
設定キーは `clusterMeshShaderPath`（旧 `useClusterMeshShader` から改名。既存 ini が opt-in 時代の 0 を保持しており、
そのままでは既定変更が既存環境に届かないため）。

**仮想ジオメトリとテッセレーションは独立した2軸**。仮想ジオメトリは静的不透明メッシュの描画を CPU の per-item 発行から
GPU のクラスタ選択に置き換えるもので、テッセレーションは残った頂点シェーダ描画（スキンメッシュ、および仮想ジオメトリ OFF 時の静的不透明）が
どのパイプラインを通るかの選択。GUI では前者がチェックボックス、後者が Standard / Tessellation のラジオになっている。
CLI は `render.meshshader 0|1` と `render.tessellation 0|1` で個別に切り替えられる。

**初回コスト**: クラスタ経路を最初に使うフレームで `SceneSubmitter::EnsureClusterDagGpuBuilt()` が走る（Release・DAG キャッシュ有りで約 250ms）。
起動処理へ移す試みは 2 か所とも失敗した（TODO 参照）。シーン投入直後（`BootPhase::SceneReady` の直前）で呼ぶと起動がハングする。
`ClusterDagStore::Build` は `ParallelFor` で並列化されており、`JobCounter::Wait` は呼び出しスレッドでジョブを実行せずに待つため、
アセット読み込みがワーカーを占有している間にメインスレッドを止めると、Win32 メッセージポンプも残りのメインスレッド初期化タスクも進まなくなる。
全初期化完了（`BootPhase::Running` の遷移）で呼ぶ案は逆に遅すぎて、その前に最初の実フレームが遅延構築を走らせてしまう（実測ログの順序で確認）。
現状は最初にクラスタで描くフレームで一度だけ発生する。

Nanite 型の仮想ジオメトリを段階的に導入しています。2026-09-23 時点では、CPU 側のクラスタ LOD 階層（DAG）構築とキャッシュに加え、GBuffer/CSM のクラスタ描画、CSM の GPU 駆動選択、要求 feedback、グループ単位の常駐管理、RAM page store から GPU page pool への転送まで実装済みです。GBuffer の GPU 駆動化、Hi-Z occlusion、normal-cone culling、disk paging は未実装です。normal cone は double-sided material の扱いが未確定なため保留しています。
設計根拠は Karis, "Nanite: A Deep Dive"（SIGGRAPH 2021）と meshoptimizer 1.2 の `demo/clusterlod.h`（NVIDIA `vk_lod_clusters` が採用している公式の DAG ビルダー）。

- **2026-09-20 起動時実測（PBRApp / DX12 Release）**: 65,312 clusters / 8,023 groups / 54,706,096 bytes。`ClusterDagStore::Build` は 259.22ms、`EnsureClusterDagGpuBuilt` は 512.33ms。初回 page upload は 16MB/frame の upload ring に収まらず 5,465 loads が deferred になりました。これは欠落ではなく後続フレームへの繰越ですが、初回クラスタ描画の安定まで複数フレーム必要です。

- `Source/Renderer/VirtualGeometry/`
  - `ClusterDag.h`: データモデル。クラスタ（≤128 三角形、元メッシュの頂点配列を指すインデックス）・グループ（同時に簡略化されたクラスタ集合と、その粗い結果の誤差球 `simplified`）・階層ごとの BVH 森（`nodes[0..levelCount)` がルート）。
  - `ClusterDagBuilder`: `clodBuild` を呼び、クラスタ化 → `meshopt_partitionClusters` でグループ化 → 境界頂点ロック付き簡略化 → 再クラスタ化、を 1 クラスタになるまで繰り返す。属性は法線+UV（UV シームは permissive 簡略化で保護）。`CLUSTERLOD_IMPLEMENTATION` はこの TU だけで定義する。
  - `ClusterDagSerializer` / `ClusterDagCache`: `SDAG` フォーマットのバイト列化と、頂点+インデックス+設定の FNV-1a 64 ハッシュをキーにした `<exe>/AssetCache/dag/<key>.sdag` キャッシュ（同一形状のメッシュはキーが一致して共有される）。
  - `ClusterDagStore`: `SceneSubmitter` が保持するメッシュ配列と同じ添字で DAG を所有し、`ParallelFor` でメッシュ並列にビルド/キャッシュ読込する。`SceneSubmitter::EnsureClusterDagsBuilt()` から遅延構築（メッシュレットと同じライフサイクル）。
- 実行時の LOD 判定式（AS と GPU 選択 CS に実装済み）: `error / max(distance(center, cameraPos) - radius, znear) * (proj[1][1] * 0.5)` を画面誤差とし、「自グループの `simplified.error` が閾値超 かつ（`refinedGroup` が無効 または そのグループの `simplified.error` が閾値以下）」のクラスタだけ描く。グループ誤差は DAG を上るほど単調増加するよう構築されるので隣接グループ間に隙間が出ない。
- 実測（Bistro, Debug）: 1699 メッシュ / 310 万三角形 → 12 階層、各階層で三角形がほぼ半減（3.10M → 1.53M → 747k → … → 54）、終端グループはメッシュごとの最終ルート 1 個のみ（簡略化に詰まったグループ 0）。初回ビルド 4.1 秒、キャッシュヒット時 0.19 秒、キャッシュ 74MB。
- **Phase 2（2026-09-05）: GBuffer 不透明描画のクラスタ経路**。`render.meshshader 1`（設定 `clusterMeshShaderPath`、既定 ON）で静的不透明メッシュを VS 経路の代わりに
  `MeshShaderRenderPass`（OpaqueGBuffer の直後に挿入、同じ GBuffer 5 枚＋SceneDepth に追記）で描く。
  - `ClusterDagGpuBuffer`: DAG を GPU 形式にパック（クラスタ 48B / グループ 32B、クラスタごとの一意頂点 id と 8:8:8 局所三角形）。`SceneSubmitter::EnsureClusterDagGpuBuilt()` で遅延構築（Bistro 65k クラスタ・54MB、Debug で約 1.1 秒）。
  - `MeshShader_AS.hlsl`: 64 クラスタ／グループで、上記 LOD 判定式＋球フラスタムカリングを行い、可視クラスタを groupshared で圧縮して `DispatchMesh`。`MeshShader_MS.hlsl`: 128 スレッドで頂点・三角形を 1 対 1 に出力。共通宣言は `MeshShader_Common.hlsli`。
  - `ClusterDrawSubmitter`: CPU 側の per-draw ループ（AABB カリング、`MaterialConstants` で VS 経路と同じ材質定数、テクスチャテーブル、36 dword ルート定数、`DispatchMesh`）。ルートシグネチャは 11 パラメータ（`RenderPipelineStateCacheMeshShader.cpp` 参照）。
  - 実測（Bistro, Debug, 1264×681）: VS OpaqueGBuffer 17.8 ms → クラスタ経路 LOD0（全クラスタ）7.8 ms / 1px 4.4 ms / 4px 2.2 ms。画像は LOD0 で VS 経路とピクセル一致（差分 0.00%）、1px で 0.01%、4px で 0.05% の画素が変化。デバッグレイヤの新規メッセージなし。
  - 制限: GBuffer は静的不透明メッシュのみ。スキン・半透明は VS 経路のままです。ディレクショナル CSM は Phase 2b 以降でクラスタ経路に対応しましたが、VSM・spot/point shadow は VS 経路のままです。誤差のワールド換算はモデル行列の最大スケールを使います（非一様スケールでは保守的）。
- **Phase 2b（2026-09-05）: ディレクショナル CSM のクラスタ経路**。`render.meshshader 1` のとき、カスケード深度パスの静的メッシュも AS/MS で描く（VSM・スポット/ポイント・スキンは VS 経路のまま）。
  - `MeshShaderShadow_MS.hlsl`（SV_POSITION のみ出力、PS なし）＋ `RenderPipelineStateCache::GetMeshShaderShadowPipelineState()`（D32 DSV、GREATER、`ShadowRasterizerState.h` の `MakeDirectionalShadowRasterizerDesc()` を VS 版 PSO と共有）。
  - LOD 判定は正射影モード（`CameraCB.g_orthographic`）: 距離項を持たず、メッシュ空間誤差 × 「シャドウマップのテクセル/ワールド単位」（ライト VP の第 1 列長 × 0.5 × 実マップ解像度）で texel 誤差にする。閾値は main view の `render.lodthreshold` から分離した `render.shadowlodthreshold`（既定 4 texels）を使う。
  - `LightSystem::ExecuteShadowPass` 末尾の `drawClusterCallback` をカスケードループで `drawCallback` の代わりに呼び、直後に VS 用のルートシグネチャ／PSO／テーブルを復元（`restoreDirectionalShadowState`）。サービスは `RenderPassExecutionServices::drawClusterShadowItems`、`ClusterDrawSubmitter::DrawShadow`。
  - 実測（Bistro, Debug, 4096² × 2 カスケード）: Shadow パス VS 24.9〜27.5 ms → クラスタ経路 LOD0 13.7 ms / 1px 7.4〜8.4 ms / 4px 3.9 ms。画像は 1px で VS 経路と一致（差分 0.00%）、4px で 0.23% の画素が変化。デバッグレイヤの新規メッセージなし。
- **Phase 3a（2026-09-05）: GPU 駆動のクラスタ選択＋ExecuteIndirect（CSM）**。`render.clustergpucull 1`（既定 ON、`render.meshshader 1` のとき有効）で、カスケードごとに CPU の per-item ループを廃し、
  (1) `ClusterSelect_CS.hlsl`（cs_6_6、インスタンス 1 個＝1 スレッドグループ）が DAG の BVH 森を groupshared のレベル同期リストで走査し、ノード誤差が閾値以下の部分木と視錐台外を枝刈りしつつ、
  葉（グループ）のクラスタに同じ LOD 判定を適用して可視クラスタ `(instance, cluster)` を UAV リストへ InterlockedAdd で追記、(2) 1 スレッドの `CS_ClusterSelectArgs` が DispatchMesh 引数（x=min(count,1024), y=ceil(count/1024)）を書き、
  (3) `MeshShaderShadowIndirect_MS.hlsl`（ms_6_6、AS なし）を `ExecuteIndirect` 1 回で起動する。頂点バッファは `ResourceDescriptorHeap[inst.vertexSrvIndex]`（バインドレス、ルートシグネチャに `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`）で引くため、
  `ClusterInstanceBuffer` がメッシュごとの `StructuredBuffer<Vertex>` SRV を共有 SRV ヒープに作る（ヒープ容量を 4096→16384 に拡張）。
  - 新規: `ClusterGpuCulling`（コンピュート RS/PSO、コマンドシグネチャ、フレーム×ビュー スロットごとの可視リスト 2MB〈初期容量。下記「可視クラスタリストの容量を適応化」で各スロットが実測需要に応じて伸びるようになった〉/ カウンタ / 引数バッファ、ゼロバッファからのコピーでリセット）、`ClusterInstanceBuffer`（フレームごとの 96B インスタンス配列＋頂点 SRV キャッシュ）、`ClusterDagGpuBuffer` に BVH ノード（32B）とグループのクラスタ範囲を追加、`ClusterDrawSubmitter::DrawShadowIndirect`。
  - 実測（Bistro, Debug, 4 カスケード, 1px）: Shadow パス GPU 駆動 8.2 ms / CPU 駆動 8.3 ms / VS 26.9 ms（4px: 4.3 ms）。GPU 時間は同等で、狙いは CPU 側の per-item 送出（1 カスケードあたり約 1700 回の DispatchMesh＋ルート定数）の撤去と、Phase 4 のストリーミングで必要になる GPU 側の可視クラスタ集合。画像は CPU 駆動と一致（差分は連続キャプチャ間のノイズ 0.5% 以内、64 超の画素差なし）。
  - 制限: GBuffer 側はまだ CPU 駆動（材質テクスチャのバインドレス化が要る）。可視クラスタ数は GPU 側のみ（`render.clusterstats` の shadowClustersDispatched は GPU 駆動時 0）。ExecuteIndirect/コマンドシグネチャは D3D12 直叩きで RHI 未抽象化。
- **Phase 4a（2026-09-05）: ストリーミングのフィードバック基盤**。選択 CS が選んだクラスタの DAG グループを `u3` のビットセット（絶対グループ index、Karis 2021 の「選択されたクラスタのページを要求する」の単位をグループにしたもの）に `InterlockedOr` し、
  `ClusterGpuCulling` がビュー（カスケード）ごとにカウンタ＋ビットセットを readback バッファへコピーする。CPU 側は同じフレームスロットを次に使うとき（＝バックバッファ数フレーム前の結果）に `ReadFeedback` で読み、
  `ClusterStreamingFeedback` が全ビューを OR してフレームの「要求グループ集合」と統計（可視クラスタ数、要求グループ数/率、前フレームからの増減、走査リストのオーバーフロー）を作る。`render.streamstats` で確認できる。
  - 実測（Bistro, 4 カスケード, 1px）: 選択クラスタ 30.9k（ビュー最大 15.0k、オーバーフロー 0）、要求グループ 3096/8023（38.6%）。4px では 16.7k / 33.7%。GPU 時間は CPU 駆動と同等（ビットセットのリセット/コピーは実グループ数分の 1KB に切り詰め）。
  - この集合が Phase 4b（ページ常駐管理: 要求されたのに非常駐のグループをロード、常駐だが不要なものを追い出し、非常駐は親の粗いクラスタで代替）の入力になる。
- **Phase 4b-1（2026-09-05）: 常駐ポリシーと GPU 側フォールバック**（データ移動はまだ: 全グループを GPU に置いたまま「常駐/非常駐」を模擬）。
  - `ClusterResidencyManager`: 要求集合（Phase 4a）からグループ予算内で常駐集合を決める。各メッシュの終端（ルート）グループは常に固定、非固定グループは最終要求フレームが最も古いものから追い出し（LRU）、1 フレームのロード数上限 256。`render.streambudget <groups>`（0 = 無制限）。
  - 選択 CS に常駐ビットセット `t4` を追加。非常駐グループのクラスタは描かず、**より細かい階層が非常駐なら「十分細かい」とみなして読み込み済みの粗いクラスタを描く**（vk_lod_clusters / Nanite のフォールバック規則）。要求集合は常駐と無関係に記録するので、非常駐でも必要なグループはロード要求される。
  - 実測（Bistro, 4 カスケード, 1px, 総グループ 8023・固定 1699）: 予算 4000 で要求集合 ≈ 常駐（無制限時と画像一致 0.00%）。予算 2500 では可視クラスタ 30.9k → 12.9k に落ち、粗い LOD で穴なく描画（missing 2515）。予算 < 固定数（1699）では固定分しか常駐できないので粗いまま。
  - 注意: 細かい階層が欠けると粗い側も要求集合に入るため、要求集合は無制限時（38.7%）より膨らむ（予算 4000 時 50.3%）。予算はその分の余裕を見る。
- **Phase 4b-2a（2026-09-05）: グループ単位ページ形式（`ClusterPageStore`）**。DAG グループ 1 個 = 1 ページ: 16B ヘッダ（clusterCount / vertexIdCount / triangleCount / group）＋ `GpuClusterDesc[]`（vertexOffset/triangleOffset はページ内の要素 index に書き換え）＋頂点 id 配列（メッシュ VB への global id）＋ 8:8:8 局所三角形。各セクション 16B 整列で、オフセットはヘッダから導出できる。
  `SceneSubmitter::EnsureClusterDagGpuBuilt()` が GPU バッファと同じ CPU 配列からページを生成し、`render.pagestats` で確認できる。頂点データ自体はメッシュ VB（バインドレス）に残す設計で、ディスクページングは DAG キャッシュが 0.2 秒で載る現状では見送り（RAM → GPU プールの常駐管理を先に行う）。
  - 実測（Bistro）: 8023 ページ / 51.9MB、最小 96B・最大 22.1KB（21 クラスタ）・平均 6.8KB。内訳: クラスタ記述子 3.1MB、頂点 id 26.8MB、三角形 24.2MB。
- **Phase 4b-2b（2026-09-06）: GPU ページプール**（`ClusterPagePool`）。可変長ページ用の first-fit フリーリスト（16B 粒度、隣接ブロック結合）を持つ 96MB の ByteAddressBuffer プールに、常駐集合（`ClusterResidencyManager`）との差分でページを載せ替える: 不要になったページは解放（GPU が使い終わるまで `frameCount` フレーム再利用を遅延）、必要なページはフレームごとの 16MB アップロードリングに memcpy → `CopyBufferRegion`。リングかプールが尽きた分は翌フレームに繰り越し（deferred）。
  - グループ→ページオフセット表（uint/グループ、非常駐は 0xFFFFFFFF）をフレームスロットごとにアップロードし、これが GPU 側の常駐判定になる（常駐ビットセットは廃止）。選択 CS はページからクラスタ記述子を読み、可視リストの要素は `(instance, pageOffset>>4 | clusterInPage<<27)`。間接 MS はページヘッダからセクション位置を求めて頂点 id と三角形を読む（頂点座標はバインドレスのメッシュ VB のまま）。ルートシグネチャは 5 パラメータに整理。
  - 実測（Bistro, 4 カスケード, 1px）: 無制限 = 8023 ページ / 51.9MB 常駐（初回は 16MB/フレームで約 4 フレームかけて載る）、可視クラスタは従来どおり 30.9k。予算 2500 = 2500 ページ / 5.9MB、可視 11.6k、粗い LOD で穴なし、deferred 0。Shadow パス定常: プール経由（無制限）9〜11 ms / CPU 駆動 15 ms / VS 62〜67 ms（同一実行内の相対値。予算 2500 では 2.5〜5 ms）。
  - 制限: GBuffer 側の CPU 駆動経路は従来の全常駐バッファを使うため、プール分 52MB は追加メモリ（3b で GBuffer もプール経由にすれば従来バッファを外せる）。ディスクページングは未実装（RAM のページストア → GPU プール）。ロードは同期 memcpy（JobSystem 化は未）。
- **可視リスト要素の詰め直しと計測（2026-09-06）**。要素は `x = instance | clusterInPage << 16`、`y = ページのバイトオフセット`。
  以前は `y` を 27bit オフセット＋5bit クラスタ index に分けていたため 1 ページ 31 クラスタ・プール 2GB が上限で、`ClusterDagBuildSettings::partitionSize` を上げると
  クラスタ index がオフセット側へ溢れて無警告で壊れた（Bistro の実測は 21 クラスタ／ページで余裕 10 しかなかった）。現在は各 16bit＋オフセット 32bit で、
  `ClusterPageStore::kMaxClustersPerPage` を超えるページは `render.pagestats` の `oversizedPages` に出る。
  あわせて `ClusterPagePool::ApplyResidency` の CPU 時間を `render.poolstats` の `cpu=` に追加した。実測（Bistro, Debug）: 定常 0.34〜0.56ms（ロード 0 件、内訳はオフセット表の転送と常駐差分の走査）、
  予算 2500 でのページ入替時 1.27〜1.75ms（130〜220 ページ/フレーム）。
- **可視リスト要素のインスタンス番号を 24bit へ（2026-10-09）**。上記 16/16 分割はインスタンス番号が 16bit だったため、65,536 体目以降は上位ビットが
  `clusterInPage` 側へ食い込み、別インスタンスの行列でクラスタが描かれていた（警告もアサートもなし）。プロシージャル配置 10 万体では確実に踏む。
  現在は `x = instance(24bit) | clusterInPage(8bit) << 24`、`y = ページのバイトオフセット(32bit)`。配分根拠は実測で、`render.pagestats` の
  Bistro 実測値が `pages=8023 maxClusters=21 oversizedPages=0`（= 5bit 必要）であり、`ClusterDagBuildSettings::partitionSize` の目標が 16 クラスタ/グループなので
  8bit（255）は実測最大の約 12 倍。インスタンス側は 16,777,216 体まで。`ClusterPageStore::kMaxClustersPerPage` は 8bit 側の写しなので `0xFF` に追従させた
  （どちらか片方だけ広げると `oversizedPages` が検出できなくなる）。
  超過検出も追加: `ClusterSelect_CS` が可視カウンタのバイト 4 にフラグを立て（`kOverflow*`: BVH リスト溢れ / インスタンス番号溢れ / ページのクラスタ数溢れ /
  可視リスト溢れ）、`render.streamstats` の `overflowFlags=0x..` に出る。特に可視リスト上限（262,144）超過はこれまで `if (slot < g_maxVisible)` で無言に捨てていた。
  ビット値の定義は `MeshShader_Common.hlsli` だけに置き、C++ 側は生のビット列をそのまま表示して写しを作らない。
  検証（Release, Bistro 1699 インスタンス, 同一視点）: `imgdiff` で変更前後ともノイズ床と同じ `maxAbs=0 / changed=0.000%`（ノイズ床は実行内・実行間ともに 0）。
  計測が詰め方に反応することの確認として、意図的に 1 ずらした詰め方では `changed=51.5% / maxAbs=136` が出る。
- **クラスタ選択 CS のディスパッチを 2 次元化（2026-10-09）**。`CS_ClusterSelect` は 1 インスタンス = 1 スレッドグループなので、
  `Dispatch(instanceCount, 1, 1)` のままでは D3D12 の「1 次元あたりスレッドグループ 65,535」を超えた時点で無効ディスパッチになる
  （プロシージャル配置 10 万体で確実に踏む。65,536 体目以降は選択が走らず、絵から消える）。
  詰め方は既存の `CS_ClusterSelectArgs`（間接 `DispatchMesh` を 2 次元に詰めている）と同一にした:
  `x = min(count, width)` / `y = ceil(count / width)` で発行し、シェーダ側は `groupId.y * width + groupId.x` でフラット番号に復元、
  切り上げで出る余剰グループは既存の `instanceIndex >= g_instanceCount` で早期 return する。幅は `kSelectGroupsX = 1024`
  （上限 1024 x 65,535 = 約 6,710 万体）。`kIndirectGroupsX` は生産側・消費側ともに HLSL なので 1 箇所で済むが、こちらは**生産側が C++**
  なので写しが 1 つ要る: `MeshShader_Common.hlsli` の `kSelectGroupsX` と `ClusterGpuCulling.cpp` の `kSelectGroupsX` が本体と写しで、
  ズレるとエラーも警告もなく別インスタンスが選択される旨を双方にコメントで明示した。
  検証（Release, Bistro 1699 インスタンス, 同一視点）: `imgdiff` で変更前後が `maxAbs=0 / changed=0.000%`（ノイズ床は実行内・実行間ともに 0 で同値）、
  `render.streamstats` は `instances=6788 visibleTotal=15183 overflowFlags=0x0` が変更前後で一致。
  計測がディスパッチ次元に反応することの確認として、復元式の x/y を意図的に入れ替えると `changed=20.198% / maxAbs=118`、
  `instances` も 6788 → 16 に落ちる。なお本シェーダは実行時 DXC コンパイルなので、ビルド成功ではなく
  `Build/bin/x64/Release/Shaders/.../ClusterSelect_CS.CS_ClusterSelect.cs_6_6.cso` の更新（12324 → 12344 バイト）で新コードの反映を確認した。
- **クラスタインスタンスバッファの容量を動的化（2026-10-09）**。`ClusterInstanceBuffer` の容量はフレームスロットあたり 8192 固定で、
  超過分は `Update` が `break` して打ち切り、ログ 1 行だけで黙って絵から消えていた（プロシージャル配置 10 万体では 12 分の 1 しか描けない）。
  現在は `Update` が先に対象インスタンス数を数え、書き込む直前のフレームスロットだけを倍々で再確保する。
  8192 は上限ではなく**初期容量**になった（Bistro 1697 インスタンスでは再確保が 1 度も起きず、768KB × 2 スロットのまま変わらない）。
  フレーム・イン・フライト対策は 2 段構え: (1) 書き込むスロットは `FrameCoordinator::BeginFrame` がフェンス待ち済みのものだけ、
  (2) それでも押し出した旧バッファは即解放せず `frameCount` 回の `Update` ぶん保持してから解放する（＝全スロットが一巡するまで GPU が読み得るバッファを壊さない）。
  上限は `kMaxInstances = 1 << 24` で、上の 24bit 詰めの写し（これを超えると `kOverflowInstanceIndex` が立ち別インスタンスの行列で描かれるため、
  破綻へ向かって伸ばすのではなく溢れとして報告する）。再確保が失敗した場合だけ切り捨てが残り、そのときは
  `ClusterStreamingFeedback::kOverflowInstanceCapacity`（= 16、シェーダ側 `kOverflow*` の採番の続き）を
  `render.streamstats` の `overflowFlags` に立てる。新しい通知経路は作らず、GPU 側の溢れと同じ 1 箇所に出る。
  判定条件（透明 / クラスタなし / 頂点 SRV 未作成）は数える側と書く側で必ず一致する必要があるため、1 つの `ResolveItem` に括り出した。
  検証（Release, Bistro 1697 インスタンス, 同一視点）: `imgdiff` で変更前後が `maxAbs=0 / changed=0.000%`（ノイズ床は実行内・実行間ともに 0 で同値、
  PNG の MD5 まで一致）。`render.streamstats` も `instances=6788 visibleTotal=21457 overflowFlags=0x0` が変更前後で一致。
  再確保経路の確認として初期容量を一時的に 16 にすると `GrowSlot: 16 -> 2048` が 2 スロットぶん出たうえで絵は `changed=0.000%`、
  さらに再確保を無効化して切り捨てを強制すると `changed=19.854% / maxAbs=136`、`instances` 6788 → 64、`overflowFlags=0x10` になる（確認後いずれも復帰）。
- **可視クラスタリストの容量を適応化（2026-10-09）**。`kMaxVisibleClusters = 1 << 18`（262,144 エントリ = 2MB）がビュー共通の固定上限で、
  超過分は `ClusterSelect_CS` の `if (slot < g_maxVisible)` で捨てていた（段階 1 で `overflowFlags=0x8` が立つようにはなったが、容量自体は固定のまま）。
  プロシージャル配置 10 万体では Bistro と同じ密度でも単一ビュー 60 万〜180 万エントリに達し、数倍〜7 倍の溢れになる。
  現在は固定上限ではなく**ビュースロットごとの容量**で、そのスロット自身の実測需要に合わせて倍々で伸びる（`ClusterInstanceBuffer` と同じ成長規則で、2 つ目の方式は作っていない）。
  需要の測り方: `CS_ClusterSelectArgs` がクランプ前の `InterlockedAdd` 総数（＝「詰めたかった数」）をカウンタのバイト 24 に保存し、
  CPU はそのスロットの readback からそれを読んで次の `RecordSelect` で容量を決める。従来はバイト 0 をその場でクランプしていたため
  「満杯だった」ことしか分からず「いくら足りないか」が失われていた。1 回の成長で需要全体を満たすので、学習の遅れは readback 1 往復（`frameCount` フレーム）分だけ。
  **ビュー一律に最大値を確保しない**根拠は実測で、Bistro 1697 インスタンス・4 カスケードの需要は `7507 / 3887 / 3641 / 148` エントリ（**50 倍の開き**。
  最も重いカスケード 1 枚で合計 15183 の半分を占める）。一律なら 4 × 7507 = 30028 エントリを確保して 15183 を賄うことになる。
  上限は `kVisibleClusterCeiling = 1 << 22`（4,194,304 エントリ = 32MB。実際に要求されない限り確保はされない）で、
  最も重いカスケードの 4.4 クラスタ/インスタンスなら 10 万体で約 44 万、悲観的な 18 クラスタ/インスタンス換算でも約 180 万なので余裕を取った値。
  上限に達しても壊れず、従来どおり `kOverflowVisibleList`（0x8）が `render.streamstats` の `overflowFlags` に出る（新しい通知経路は作っていない）。
  初期容量は旧上限と同じ 262,144 のままなので、**Bistro 規模では成長が 1 度も起きずメモリは従来とバイト一致**（`visibleCapTotal=1048576` エントリ = 4 ビュー × 2MB）。
  フレーム・イン・フライト対策は段階 3 の仕組みをそのまま流用: 押し出した旧バッファは即解放せず `RetiredVisibleBuffer{ handle, updatesRemaining = frameCount }` で保持する。
  カウントダウンはスロット単位で回す（1 つの `(frameIndex, viewSlot)` は 1 フレームに 1 回だけ記録されるので、これでフレーム単位の減算になる。オブジェクト単位にするとビュー数ぶん速く失効する）。
  観測用に `render.streamstats` へ `visibleReqMax`（クランプ前需要の最大）/ `visibleCapMax` / `visibleCapTotal` を追加した。
  検証（Release, Bistro 1697 インスタンス, 同一視点）: `imgdiff` で変更前後が `maxAbs=0 / changed=0.000%`（ノイズ床は実行内・実行間ともに `maxAbs=0 / changed=0.000%` で同値）。
  `render.streamstats` も `visibleTotal=15183 visibleMax=7507 visibleCapTotal=1048576 overflowFlags=0x0` が変更前後で一致し、成長ログは 0 件。
  成長経路の確認として初期容量を一時的に 1024 にすると、4 カスケードが自分の需要ぶんだけ **8192 / 4096 / 4096 / 1024**（計 17408 エントリ）へ伸び、
  絵は固定 262,144 時と `changed=0.000% / maxAbs=0` で一致した（一律に最大値なら 4 × 8192 = 32768 エントリ＝1.88 倍）。
  計測が容量に反応することの確認として上限も 1024 に固定して成長を封じると `changed=1.375% / maxAbs=100`、
  `visibleTotal` 15183 → 3178、`visibleMax=1024` に対し `visibleReqMax=7507`、`overflowViews=3 overflowFlags=0x8` になる（確認後いずれも復帰）。
- **GPU 駆動の前提処理をフレーム 1 回の独立ステップへ抽出（2026-10-10）**。インスタンスリストの更新（`ClusterInstanceBuffer::EnsureVertexSrvs` / `Update`）、
  DAG 再構築時の常駐初期化（`ClusterResidencyManager::Reset` / `SetBudget`、`ClusterPagePool::Reset`）、常駐集合のページプールへの適用（`ApplyResidency`）と
  オフセット表 VA の解決、ストリーミングフィードバックのフレーム境界（`BeginFrame` / `EndFrame` と `ClusterResidencyManager::Update`）は、
  いずれも「ビューごと」ではなく「フレームごと」の処理だが、**ディレクショナルシャドウのクラスタ描画サービスの内側**、しかも
  `render.clustergpucull` が有効かつ `activeCascadeIndex == 0` という条件下に置かれていた。パス順が Shadow → OpaqueGBuffer → MeshShader なので
  唯一の GPU 駆動ビューに対しては偶然成立していただけで、CPU 駆動経路やシャドウ無効時には前提が崩れる（メインビューを GPU 駆動化する前に外す必要がある）。
  現在は単一責務のクラス `ClusterFrameUpdate`（`Source/Renderer/VirtualGeometry/ClusterFrameUpdate.{h,cpp}`）に切り出し、
  `Renderer::BeginClusterFrameUpdate` / `EndClusterFrameUpdate`（`Source/Renderer/Runtime/RendererClusterFrame.cpp`）経由で
  `Renderer::Render` がフレームごとに 1 回だけ呼ぶ。`Begin` はパス登録の直前（ページプールへのコピーを記録するため、プールを読む最初のパスより前）、
  `End` は `RenderGraph::Execute` の直後（フィードバックを報告したビューは実行済みなので、後段のパスが 1 つ失敗した frame でも常駐判断を止めない）。
  残った条件は「クラスタ経路自体が有効か（`RenderPassExecutionPolicy::useMeshShader`）」と「バッファが初期化済みか（`IsValid`）」だけで、
  カリング方式やカスケード番号には依存しない。オフセット表 VA は `ClusterFrameUpdate::GetPageOffsetTableVA()` が公開し、`Renderer` から
  `m_clusterPageOffsetTableVA` / `m_clusterResidencyGroupCount` / `m_clusterInstanceFrame` の 3 メンバを削除した。
  あわせて終端グループ（`error == FLT_MAX` の番兵）の判定を `ClusterDagGpuBuffer::BuildTerminalGroupBits()` に移し、番兵の意味を DAG 側に閉じた。
  検証（Release, Bistro 1697 インスタンス, 同一視点, headless）: 変更前後の PNG が **MD5 まで一致**（`imgdiff` で `changed=0.000% / maxAbs=0`。
  ノイズ床は実行内・実行間ともに `changed=0.000% / maxAbs=0`）。`render.streamstats` / `render.poolstats` / `render.pagestats` も
  `views=4 visibleTotal=14660 wantedGroups=2689/8023 resident=8023/8023 pinned=1699 pages=8023 loads=0(+8023) used=51.9MB` が完全一致（差は計測の ms 値のみ）。
  抽出したステップを空にすると `changed=3.176% / maxAbs=111`、`views=0 resident=0/0 pages=0 used=0.0MB`、`shadowItemsDrawn` 6788 → 0 になる（確認後復帰）。
  **条件から外れたことの確認**: `render.clustergpucull 0`（CPU 駆動経路）にしたうえで `render.streambudget 3000` を与えると、
  変更前は `resident=3000/8023` に対しプールが `pages=8023 / used=51.9MB / evictions=0` のまま取り残される（`ApplyResidency` が呼ばれない）が、
  変更後は `pages=3000 / used=12.1MB / evictions=+5023` とプールが常駐集合に追従する。`render.streambudget 0` に戻すと `pages=8023 / loads=+13046` まで復帰する。
- **常駐更新の定常コストを除去（2026-09-06）**。上の計測でロード 0 件でも 0.55〜0.70ms 掛かっていた。
  内訳はページのコピーではなく、毎フレームの全グループ走査（8023 件）とオフセット表の転送だった。実際、ページコピーを JobSystem で並列化しても
  入替時のコストは 0.86〜2.14ms とばらつくだけで再現性のある改善が無かったため、並列化は戻した。
  代わりに「要求集合が前回と同一かつ当該フレームスロットのオフセット表が最新」なら丸ごと省く経路を入れた（オフセット表は世代番号で追跡）。
  実測: 定常 0.55〜0.70ms → **0.004〜0.015ms**。入替時は 0.86〜1.94ms で変わらず、常駐の挙動と描画結果も不変。
- **予算超過時のスラッシングを解消（2026-09-06）**。予算を絞ると、カメラ静止でも要求集合が毎フレーム 4956〜5036 と揺れ、
  常駐グループが毎フレーム 256 件（1 フレーム上限いっぱい）入れ替わり続けていた。原因はストリーミング自身のフィードバックループで、
  非常駐グループは粗い親クラスタで代替されるため親も要求される → 細かい方を読み込むと親が要求から外れる → 親が追い出されるとまた要求される、を繰り返す。
  対処として追い出し条件に保持期間を入れ、「直近 N フレーム以内に要求されたグループは追い出さない」ようにした（`render.streamretention <frames>`、既定 60）。
  実測（Bistro, 予算 2500, カメラ静止）: 保持 0（従来）は要求集合が揺れ続け毎フレーム 256 件入替。保持 60 では要求集合が一定になり、
  入替 0 件・ページコピー 0 件で静止する。画質は変わらず（従来経路との差はどちらも 0.06%）、予算内に収まらない分は粗い LOD のまま安定する。
- 今後: Phase 4b-3 = ロードの非同期化（JobSystem でリングへ書き込み。上記計測どおり効くのは入替時のみ）、ディスク版ページストア（DAG キャッシュを開かずにページだけ読む）、GBuffer 経路のプール化（3b と統合）。Phase 3b = GBuffer 側の GPU 駆動化（材質バインドレス）と RHI 抽象化。

  **フリーリストの断片化（2026-09-13 計測）**: ページは可変長なので、プールは first-fit のフリーリストで管理している。断片化が実害になっているかを判断できるよう、`render.poolstats` に空きブロック数だけでなく**最大空きブロック**と**解放待ちバイト数**を、また保留の原因を 4 つ（`ring` = 当フレームのアップロードリング枯渇 / `full` = プールの空きバイト不足 / `pending` = 空きはあるが実行中フレームのために保持中 / `frag` = 空きバイトは足りるが連続領域が無い）に分けて出すようにした。最初の失敗でそのフレームのロードを打ち切る設計なので、原因カウンタは 1 フレームに高々 1 つしか立たず、`deferred=` の総数とは一致しない（残りは同じ理由で飛ばされ、総数にのみ数える）。プールサイズは既定 96MB だが、既定値では枯渇も断片化も起こらず分類が一度も発火しないため、環境変数 `SASAMI_PAGE_POOL_MB` で縮められるようにした（`SASAMI_GPU_VALIDATION` と同じ opt-in 方式）。`SASAMI_PAGE_POOL_MB=8` の実測で `deferred=6570(ring=0 full=1 pending=0 frag=0)` を確認している。`ring` / `pending` / `frag` の 3 経路はこのシーンでは発火しない。ページプールを駆動するのは CSM のライト視点で、カスケード箱はカメラ位置中心なので `camera.setTarget` で要求集合は動くが（2800→2809 ページ）、約 2809 グループで飽和し、流入するのは既存の穴に収まる小ページだけで `frag` の条件（要求サイズ > 最大空きブロック かつ 空き総量は足りる）に届かない。予算 6000↔400 を 3 サイクル繰り返しても断片化は 249 ブロック・最大空きブロック 652.6KB で頭打ちになる（`InsertFreeBlock` の前後結合が効き、最大ページ 22KB の約 30 倍を保つ）。first-fit はフリーリスト全体を見るため、空きバイトが足りて確保に失敗するのは連続領域が無いときだけであり、この 4 分類は一意に決まる。実測（Bistro / DX12 Release）は、予算無制限で空き 1 ブロック（44.1MB）、予算 2500（5523 ページ追い出し後）で空き 87.4MB が 1299 ブロックに割れるものの、最大ブロックは未使用の末尾 44.1MB のままで `frag=0`。ページ最大 22KB に対し連続領域が桁違いに余るため、現時点で圧縮やサイズクラスは不要と判断した。

## Voxel Occupancy

環境を細かいボクセルに区切り、1 ボクセル＝1bit（0=空、1=固体）で占有を記録する。将来、流体の固体境界として使う前提の土台（流体側の速度・密度は float のまま）。

- 実装: `Source/Renderer/Voxel/`。三角形-箱の分離軸判定（Akenine-Möller 2001）で表面を塗る。`CpuSurfaceVoxelizer` が参照実装で、`GpuSurfaceVoxelizer`（1スレッド＝1三角形、`InterlockedOr`）はこれとビット完全一致させる。出典: Schwarz & Seidel 2010。
- 使い方: PBRApp の「Voxel」タブで「Voxel Occupancy」ウィンドウを開き、Max dim（最長辺のボクセル数）を決めて「Voxelize scene」。X/Y/Z 軸ごとに投影（列ごとの占有数）かスライスを表示する。デバッグコマンドは `voxel.scene [maxDim] [cpu]`、`voxel.view on|off [x|y|z] [proj|slice] [index]`、`voxel.selftest`。動的オブジェクト向けに `voxel.update <full|delta> [check]`（動いたアイテムの AABB 領域だけ再ボクセル化。`check` で全体との bit 差を報告）、`smoke.solid <0|1> [keep]`（`keep` で煙の場を保持してマスク差し替え）、`scene.models` / `scene.move <index> <dx> <dy> <dz>` を追加。4bit セル属性は `voxel.cells <coverage|material|selftest>`（coverage = 2³ サブボクセル被覆数 0..8、material = 材質クラス 1 汎用 / 2 金属 / 3 発光。直前の `voxel.scene` のグリッドに対して構築）。符号付き距離場は `voxel.sdf [selftest|upload|check]`（直前の `voxel.scene` の占有グリッドから CPU の厳密 EDT［Felzenszwalb & Huttenlocher 2012］で作る。単位はボクセルで、境界の面でゼロになる。`upload` で RHI の 3D テクスチャ〔R32F〕に載せる〔DX12/DX11/OpenGL/Vulkan〕が、まだどこからも使っていない。`check` は upload 後に compute で 3D テクスチャを読み戻し、CPU 値とのビット不一致数を報告する〔DX12 のみ〕）。`voxel.sdf.trace <path.png> [width height]` はメインカメラから CPU の sphere tracing（Hart 1996）でこの SDF を描き、PNG に書き出す（GPU 版は未実装）。`voxel.wind selftest | run <path.png> [count=N] [steps=N] [wind=x,y,z] [turb=T] [vps=V] [mode=curl|project]` はこの SDF を障害物にした風の粒子を CPU で流し、メインカメラから流線を PNG に描く。`mode=curl`（既定）は curl-noise（Bridson ほか 2007）。壁付近はゲージ点を最近接表面に取るため厳密な発散ゼロではなく、Bistro での貫通は約1.4%。`mode=project` は一様風を MAC グリッド上で圧力投影し（MIC(0)-PCG、Bridson & Müller-Fischer 2007）、建物を回り込む非圧縮の定常流を作る。このモードでは `turb` の既定が 0 で、`turb>0` を与えると投影した風に curl noise の乱れ（壁で接線方向になる）を上乗せする（`GustedWindField`）。Bistro での貫通は乱れ無しで 0.037%、`turb=0.3` で 0.29%（`vps=0.5` で 0.034%）。解くのに約2.2秒かかる。カメラ追従窓と GPU 版は未実装。
- 実測（Bistro・DX12 Debug）: 128 → 128x36x127・占有 8.38%・73KB・GPU 約40ms、512 → 512x141x506・2.69%・4.57MB・約115ms。三角形収集は CPU で約1.1s。
- 制限: 表面のみ（内部の充填は未実装）、スキン/半透明メッシュは対象外、GPU 版は DX12 のみ、巨大三角形は TDR の危険あり、実行は同期（毎フレームではない）。

## Crowd VAT (Vertex Animation Texture)

10万体規模の群衆描画（TODO 段8）の土台。スキンドメッシュのアニメーションを、フレームごとの表へ事前に焼き込む（SideFX Houdini VAT / Unreal AnimToTexture の Vertex・Bone 2モード、GPU Gems 3 ch.2 "Animated Crowd Rendering" に倣う）。

- 実装: `Source/Renderer/Crowd/`（`VatAnimation` = ベイクと再生参照、`VatSkinning` = CPU スキニング、`VatSelfTest`）。
  - 2つのモードがある。`Vertex` は (フレーム, 頂点) ごとに float4 の位置と法線を持ち、`Bone` は (フレーム, ボーン) ごとに 3x4 行列を持つ。
  - 1クリップのフレーム数は ceil(長さ×fps) で、t_f = f·長さ/フレーム数 に置くのでループの継ぎ目が出ない。クリップは連続配置。
  - クリップごとに `AnimationController` を新しく作る。使い回すと、そのクリップが駆動しないチャネルに前のクリップの姿勢が残るため。
- 使い方: デバッグコマンド `crowd.bake <vertex|bone|selftest> [fps]`。Fox.gltf を焼き、直接スキニングとの誤差を報告する。
- 実測（Fox・3クリップ・30fps・DX12 Debug）:
  - 160 フレームで、Vertex は 8.4MB（約49ms）、Bone は 180KB（約43ms）。
  - フレーム時刻での最大位置誤差は 1.9e-5。
  - フレーム間を線形補間した時の誤差は 1.83（60fps では 1.05）。モデル単位で、配置時はさらに 0.02 倍される。
- GPU へのアップロードとインスタンス描画は 8c（下記）。

### 群衆シミュレーション（8b）

- 実装: `CrowdStore`（SoA: 位置・速度・アニメ時刻・再生速度・クリップ）、`CrowdSimulation`（`CrowdUpdateBackend` = CpuSerial / CpuParallel / Gpu）、`GpuCrowdUpdater` + `Shaders/Compute/Crowd/CrowdUpdate_CS.hlsl`。
  - 更新は「移動 → 境界で反射 → アニメ時刻を進めてループ」。CPU 並列はチャンク 4096 の ParallelFor。
  - GPU 側は `precise` と1文1演算で mad 融合を防いでいるので、3経路の結果はビット一致する。
- 使い方: `crowd.selftest`、`crowd.sim <count> <steps> [cpu|gpu|both]`（CPU 直列/並列と GPU を比べ、時間と不一致数を報告する）。
- 実測（10万体・300ステップ・DX12 Debug）: 直列 1457ms、並列 248ms、GPU 5.6ms。不一致はいずれも 0。
- 制限: GPU 更新は DX12 のみ（DXIL）。描画との接続は 8c。

### 群衆インスタンス描画（8c・2026-10-20 ビルド・実機目視確認済み）

- 実装: `CrowdVatGpuAsset`（ベイク表・クリップ表・インデックスを GPU バッファへ）、`CrowdSystem`（更新モード Off/Cpu/Gpu・スポーン・モデル縮尺）、`CrowdRenderPass` + `Shaders/Raster/Geometry/Crowd/CrowdVat_VS/PS.hlsl`。
  - エージェントバッファを VS の t2 で直接読み、`DrawIndexed` 1回（instanceCount = 体数）で GBuffer へ描く。VS は2フレーム間を補間し、Bone モードでは 3x4 行列でスキニングする。向きは速度から求める（+Z 前方）。
  - パスは MeshShader の後（無ければ OpaqueGBuffer の後）に常に挿入され、無効時は Execute で即 return する。
- 使い方: `crowd.asset [vertex|bone] [rate]` → `crowd.spawn N [halfExtent] [seed] [height]`、`crowd.update gpu|cpu|off`、`crowd.status`、`crowd.material r g b rough`。
- 影: `crowd.shadow 1` で CSM の深度パスにも描く（`CrowdRenderPass::DrawShadowDepth`）。
- 確認状況: Vertex / Bone モード、`crowd.update gpu|cpu` の描画を目視で確認した。インデックスの無い glTF プリミティブには 0..N-1 を補う。cpu モードの device removed は、旧エージェントバッファを1フレーム遅らせて破棄するよう直した。
- 制限: DX12 のみ（`GetRhiBufferGpuAddress` が他バックエンドでは 0）。モーションベクトルを出さないので TAA でにじむ。影パスが先に保留中の GPU ステップを記録する（`CrowdSystem::RecordPendingStep`）ので、影は描画と同じフレームの状態を使う（cpu モード・immediate では従来どおり Update 側で更新）。影 PSO にはネイティブ影と同じラスタライザ depth bias を `RhiRasterStateDesc` 経由で設定している（DX12/DX11/Vulkan。OpenGL の RHI パイプラインは raster 設定自体を無視する）。

### 群衆 GPU カリング＋LOD（8d・2026-10-20 ビルド・実行確認済み）

- GPU 更新はフレームのコマンドエンコーダへ記録される（`crowd.step recorded` が既定。`immediate` は従来の submit＋待ち）。
- `CrowdGpuCuller` + `Shaders/Compute/Crowd/CrowdCull_CS.hlsl`: 各エージェントの包囲球を視錐台 6 平面で判定し（Gribb & Hartmann 2001）、カメラ距離で LOD を選び、LOD ごとの可視リストと DrawIndexed 引数を書く（Wihlidal GDC 2016 の方式）。
- LOD は `CrowdVatGpuAsset` が meshoptimizer の `meshopt_simplify` で作る（LOD0 / 0.5 / 0.2、1本のインデックスバッファ）。
- `CrowdRenderPass` は LOD ごとに `DrawIndexedIndirect`（ExecuteIndirect）を1回ずつ発行する。
- 使い方: `crowd.cull on|off`、`crowd.cull frustum on|off`、`crowd.lod <d1> <d2> [force|auto]`、`crowd.step recorded|immediate`、`crowd.status`（visible0..2 / visibleTotal）。
- 制限: パラメータのリングは8スロットで、在りフレーム数が8未満と仮定（未検証）。Cpu 更新モードは毎フレーム同期待ちする。

## CPU 参照実装: PBF 流体 / 顔影 SDF マップ / HZB（2026-10-20 ビルド・selftest 確認済み、描画は未接続）

GPU 版を作る前の正解として、CPU 側で計算を確かめるための実装。結果はまだ描画に使っていない。

- PBF 流体（`Source/Renderer/Fluid/PbfSolver`、`PbfSelfTest`）: Position Based Fluids（Macklin & Müller 2013）。
  - 近傍探索は一様グリッドで行う。λ の緩和係数は ε=600（Stanford CS348C の値）。
  - 既定値: 粒子半径 0.025、h=4r、scorrK=0.001。
  - 論文の k=0.1 は、質量 m=ρ0(2r)³ の SI 単位系では s_corr が λ より約1000倍大きくなる。ダムブレイクで密度比が約160〜198 まで崩れたため 0.001 にした。
  - コマンド: `fluid.pbf selftest`、`fluid.pbf run <N> <steps>`。
  - 表面メッシュ化: `fluid.pbf mesh <N> <steps> [show [r g b] [at x,y,z]]` でダムブレイク後の粒子を等値面（Marching Tetrahedra）にし、`show` でシーンに不透明メッシュとして表示する。`fluid.pbf hide` で消し、`fluid.pbf meshtest` で CPU 検査を行う。
- 顔影 SDF しきい値マップ（`Source/Renderer/Toon/FaceShadowThresholdMap`、`FaceShadowSelfTest`）: 原神風の顔影。
  - 入れ子になった N 枚の光源角マスク（例: 22.5° 刻みで 8 枚）の境界の符号付き距離を、隣り合うマスク間で補間する。これで 1 枚のしきい値マップ（0..1）を作る。
  - 出典は二次情報（Face_SDF_Generator ほか）のみ。
  - コマンド: `toon.facemap selftest`、`toon.facemap synth <size> <masks>`。
  - 段2（2026-10-21、描画に接続）: `toon.face [on|off] model=<i> mesh=<i,..>|all map=<png>|synth forward=x,y,z right=x,y,z soft=<f>`。
    フラグを立てた顔マテリアルでは、GBuffer にマップから求めた lit を書き、ライティングで N·L の代わりに使う。
    光の左右でマップをミラーする。頂点シェーダー経路に加え、2026-10-21 からメッシュシェーダー経路（直描画・GPU 間接描画の両方）でも効く。ネイティブ DX11/Vulkan/GL 経路は未対応。
    顔の向き（forward/right）は固定値で、頭の骨には追従しない。
- 髪のハイライト「天使の輪」（2026-10-21）: `toon.hair [on|off] model=<i> mesh=<i,..>|all up=x,y,z shift=a,b width=a,b strength=a,b soft=<f>`。
  Kajiya-Kay の二重シフトハイライト（Scheuermann, GDC 2004）を smoothstep で二値化したもの。GBuffer にタンジェントが無いため、頭の up を shading 平面に射影した方向を毛の向きとみなす（独自の近似）。
  フラグを立てた髪マテリアルはシェーディングセレクタ 0.625 を持ち、ディファードライティングで通常のトゥーン specular を置き換える。頂点/メッシュシェーダー両経路で効く。ネイティブ DX11/Vulkan/GL 経路は未対応、up は固定値。
- 頭周りの補助（2026-10-21、どちらも既定オフ、ディファードライティング側なので頂点/メッシュシェーダー両経路で効く）:
  - 前髪影 `toon.hairshadow [on|off] length=<m> thickness=<m> strength=<0..1>`: 非髪のトゥーンピクセルから光方向へ 8 ステップのスクリーンスペースレイを進め、
    `toon.hair` を立てた髪ピクセルに当たったら直接光を落とす。length の目安は 0.04 m（0.1 m だと目の下まで伸びる）。画面外のサンプルは遮蔽なし扱い。
  - 頭の球面法線 `toon.headnormal [on|off] center=x,y,z radius=<m> blend=<0..1>`: 頭の球の中にある非髪トゥーンピクセルのライティング用法線を球面法線に寄せ、
    外側 20% でフェードする（GGXrd の proxy 形状による法線編集（Motomura, GDC 2015）を、ライティング時に近似したもの）。GBuffer の法線は変えない。
    既定値 center=0,1.45,0 radius=0.13 blend=0.8 は VRoid サンプルを原点に置いたときの実測値。
  - 頭の中心・up は固定値でボーンに追従しない。ネイティブ DX11/Vulkan/GL 経路は未対応。
- トゥーン系の確認は ToonApp（`Tools/Debug/run-toon-release.bat`）で行い、PBRApp は PBR の物理表現専用とする。トゥーン機能はマテリアル単位のオプトインなので PBRApp の既定表示は変わらない。
- HZB（`Source/Renderer/Culling/HzbPyramid`、`HzbSelfTest`）: reverse-Z の深度で、各ミップに min を格納する。
  - 奇数サイズのレベルは floor 規約で作り、端の texel が 3 つのソースを吸収する（RasterGrid 2010）。
  - 判定には、矩形が 2x2 texel に収まるレベルを選ぶ（Nießner & Loop 2012）。
  - 奇数サイズのレベルでは、矩形の下限角も `width-1` / `height-1` でクランプする必要がある。
  - コマンド: `hzb selftest`、`hzb bench <W> <H>`、`hzb gpu`。
  - GPU 版（`GpuHzbBuilder`、`Shaders/Compute/Culling/HzbDownsample_CS.hlsl`、2026-10-20）: 全レベルを1本の structured float バッファに詰め、レベルごとに1ディスパッチする。
    `hzb gpu` は 7 サイズ（1x1〜1920x1080）で CPU 参照と全 texel を bit 比較する。Release / Debug とも mismatch=0（33 レベル・3,071,035 texel）。
    DX12 のみ（root descriptor に `GetRhiBufferGpuAddress` が要る）。深度テクスチャからの入力と群衆カリングへの接続は未実装。
- 制限: デバッグコマンドは 5 秒でタイムアウトする。selftest はその範囲に収まる大きさにしてある（Debug の PBF で約 15ms/step）。
- 実測（Release）:

  | コマンド | 結果 |
  |---|---|
  | `hzb bench 1920 1080` | 3.95ms |
  | `toon.facemap synth 512 8` | 68ms |
  | `fluid.pbf run 4000 60` | 12.8ms/step、平均密度比 0.98 |

## Animation Runtime / Gameplay（2026-10-20 ビルド・selftest 確認済み、描画は未接続）

既存の `AnimationController`（クリップ再生のみ）に足りなかった「ゲームのキャラクターを動かす」ための部品群。描画（GPU スキニング）にはまだ接続していない。

- `Source/Renderer/Animation/`（namespace `SasamiEngine::Render::Anim`）
  - 基礎: `AnimMath`（Vec3/Quat/Transform/Mat4）、`ClipSampler`（`SkeletonAnimation` をローカル姿勢へ）、`PoseModelSpace`（ローカル→モデル→スキン行列。`GetBoneMatrices` と同じレイアウト）。
  - ブレンド: `PoseBlend`（線形・ボーンマスク付き・加算）、`BoneMask`（骨名の部分一致で上半身などを選ぶ）、`BlendSpace1D`（位相同期。UE の Sync Groups）、`BlendSpace2D`（勾配バンド補間＝Johansen 2009／Unity 2D Freeform。Cartesian と Directional を切替）。
  - クリップイベント: `AnimNotifyTrack`（点通知と区間通知 Begin/End。ループ跨ぎで一度だけ発火。2026-10-19 追加・未ビルド）。`AnimStateMachine` の状態に `blendSpace2D` と `notifies` を指定でき、発火したイベントは `ConsumeNotifyEvents()` で取得する。
  - 遷移: `SpringDamper`（Holden "Spring-It-On"）、`Inertialization`（Bollo GDC 2018）、`AnimStateMachine`（条件・トリガー・exitTime。遷移方式は Crossfade と Inertialization を切り替えられる）。
  - ルートモーション: `RootMotion`（ルート骨の XZ と yaw を抽出・除去する。ループ境界をまたげる）。
  - IK: `TwoBoneIk`（ozz-animation IKTwoBoneJob）、`AimIk`（ozz IKAimJob と関節チェーンの LookAt）、`FootIk`（地面を調べ、骨盤を下げ、脚を IK で合わせ、足首を法線に揃える）。
  - 統合: `CharacterAnimator`（状態機械 → ルートモーション → LookAt → FootIk → スキン行列）。`AutoConfigureHumanoid` が骨名から頭・首・脚を推定する。
- `Source/AppFramework/Gameplay/`（namespace `SasamiEngine::App::Gameplay`）
  - `CharacterMovement`（速度をばねで追従、ジャンプ、コヨーテタイム、先行入力）、`FollowCamera`（ばねアーム）、`CameraShake`（Eiserloh GDC 2016 の trauma 方式）。
- デバッグコマンド: `anim.selftest`、`gameplay.selftest`、`anim.fox`（Fox.glb で状態機械と IK を回す）。
- 未完: 描画への接続（`SkinnedMeshComponent`）、モーションマッチング、ラグドール、ナビメッシュ（TODO.md「ゲーム・アニメーション機能追加」）。

## Asset Formats

| 形式 | 対応 | 実装 |
| --- | --- | --- |
| glTF / GLB | 静的・スキンドメッシュ。外部 `.bin` と GLB 埋め込み BIN chunk に対応。bufferView 埋め込み画像（PNG/JPEG、VRM も同じ）は `<exe>/AssetCache/EmbeddedImages/` に一度書き出して読む（2026-10-20、`ModelLoaderEmbeddedImages.cpp`）。data: URI と KTX2/WebP は未対応 | `ModelLoader.cpp` / `ModelLoaderSkinned.cpp` / `ModelLoaderGlb.cpp`（rapidjson） |
| OBJ | 静的メッシュ | `ModelLoaderOBJ.cpp` |
| FBX | 静的メッシュ（2026-08-23 追加） | `ModelLoaderFbx.cpp` + `Libraries/ufbx`（MIT） |
| PNG / JPG 等 | WIC 経由で RGBA8 | `AssetLoader::LoadRgba8ViaWIC` |
| DDS（BC1/2/3/4/5/7、DX10拡張、非圧縮32bpp） | RGBA8 へデコード（2026-08-23 追加） | `DdsTextureLoader.cpp` + `Libraries/bcdec`（MIT） |
| HDR (equirect) | スカイボックス・IBL | `LoadSkyboxAsync` |

VRoid のサンプルアバター（`Assets/Models/VRoidAvatarSampleD/AvatarSample_D.glb`、CC0、出典は同フォルダの `LICENSE.txt`）を同梱しています。VRM 拡張（MToon、SpringBone、表情）は読まず、通常の glTF PBR マテリアルとして描画します。トゥーン表示は `toon preset anime` と `toon.apply all` で当てます。正面はカメラ既定の向きとは逆側にあり、`camera.setTarget 0 1.3 -1.6` と `camera.setYawPitch 0 0` で正面が見える（2026-10-20 確認）。

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
| `ToonApp` | トゥーン（セル）シェーディングのサンプル。全マテリアルが `ShadingModel::Toon` を選択し、量子化した N·L バンド・段階的スペキュラ・リムライトと inverted-hull 輪郭線で描画します。各パラメータは ImGui パネルから調整できます。 |
| `WaveApp` | 波の表現用のサンプル（段1）。海と岸のシーンに、4つの Gerstner 波を重ねた CPU 格子の水面を描きます。波頭が折り返す所（水平ヤコビアン）と岸に白い泡を頂点カラーで載せ、毎フレーム頂点バッファへ直接書き込みます（メッシュシェーダ経路はオフ）。`wave.status` / `wave.set key=value` / `wave.selftest`。 `wave.flip on` で FLIP の水槽（造波板つき、`tall 1` で Tall Cell 圧力、CPU のみ）に切り替えます。 風（`wave.wind`、Wave タブの Wind 欄）は1つの突風つき風場で、風速・風向・吹送距離から JONSWAP のうねりを作り、突風でさざ波の強さを変え、砂浜の草を同じ風で曲げます（CPU のみ）。 |
| `PerfLabApp` | 最適化の効果を測るサンプル（段2a まで）。各ケースを Baseline と Optimized の2通りで N 回走らせ（1フレーム1回）、cold（1回目）と warm の中央値・最小・最大、両者の出力一致を表と CSV（既定 `Build/PerfLab/<case>.csv`）に出します。ケースは memcpy（バイトループ対 `std::memcpy`）と meshcodec_bunny / meshcodec_sponza（meshoptimizer の頂点・インデックス圧縮。生バイトの memcpy 対 デコード、サイズは表の Disk 列）、vcache_bunny / vcache_sponza（頂点キャッシュ・オーバードロー・フェッチ順の並べ替え。ACMR などの指標は表の Extras 列と CSV の extras 列）です。`perf.case list` / `perf.case run <case> [repeat=N]` / `perf.case all` / `perf.case status` / `perf.case csv [dir]`。 |

`FluidApp` / `NeuralShaderApp` は `UseDefaultRenderNodePreset()` を使います。`ToonApp` は既定パスの `Lighting` 直後に `ToonOutline` を追加する `UseToonRenderNodePreset()` を使います。`RayMarchApp` のような専用 `CameraMode` は持ちません。

## Toon Shading

Toon shading は通常の material workflow とは独立した `SurfaceMaterial::shadingModel` で opt-in します。既定値は `ShadingModel::Standard` なので、既存マテリアルの見た目は変わりません。

- `EncodeShadingSelector()` は Standard/metallic-roughness、Toon、Standard/specular-glossiness をそれぞれ `0.0` / `0.5` / `1.0` に符号化し、`GBufferSpecularWorkflow.a` の 1 チャンネルで deferred lighting へ渡します。読み出しは UNORM 量子化を考慮して範囲判定します。
- `DeferredLighting_PS.hlsl` の Toon 分岐は directional light に banded Lambert、閾値付きの flat specular、Fresnel 型 rim light を適用します。間接 diffuse は `ambientScale` で抑え、environment specular は無効化します。point/spot light は現状 `PBR_DirectLighting.hlsli` の標準 PBR 評価です。
- `LightSystem::ToonSettings` は band 数/softness、specular threshold/softness/intensity、rim strength/power、shadow floor、ambient scale、outline width/color を保持します。現在は material ごとではなくシーン共通で、`ToonApp` の ImGui パネルから変更できます。
- `UseToonRenderNodePreset()` は `Lighting` 直後に `ToonOutlineRenderPass` を挿入します。outline は front-face culling した opaque mesh を world-space normal 方向へ押し出し、既存 depth を使って `SceneColor` に単色描画する inverted-hull 方式です。
- 現在の Toon shading/outline は DX12 feature path 専用です。Toon 分岐は deferred opaque のみで、transparent forward path は Standard PBR のままです。outline は static opaque の VS draw callback を再利用し、material の shading model を個別判定しないため、Toon preset 内の全 static opaque mesh が対象です。cluster mesh shader 有効時はこの callback が空になるため、outline は描画されません。skinned mesh outline も未実装です。
- PBRApp からも使えます: `toon preset default|anime|ggxrd`、`toon key=value ...`、`toon.apply [all|off]`（全 static model の material を Toon/Standard に切り替え、outline パスを挿入）。スクリーン空間深度リム（原神風、depthRimWidth/Threshold/Strength）も追加しました。2026-10-20 に Bistro で目視確認済みです。
- ブルーム（2026-10-20、DX12 feature path）: `BloomRenderer`（`Source/Renderer/Passes/PostProcess/`）が ToneMap の直前に、SceneColor から半解像度の R16G16B16A16F mip チェーン（最大 6 段）を作ります。下りは Jimenez 2014（CoD:AW）の 13-tap で初段だけ Karis 平均、上りは 3x3 tent の加算です。閾値は使いません。`ToneMap_PS` は露出の前に `lerp(scene, bloom / mipCount, intensity)` で混ぜるので、エネルギーは保存されます（learnopengl "Phys.-Based Bloom" と同じ方式）。`render.bloom <0|1> [intensity 0..1] [radius]` で操作でき、既定は off、intensity 0.04、radius 0.005 です。VRoid で確認したところ、off に戻すと on 前とノイズの水準で一致し、0.15 で肩や腕、床まわりに滲みが出ます。ネイティブ DX11/Vulkan/GL 経路には無く、キャラ用のマテリアル単位マスクも未実装です。
- 2026-09-06 に `ToonApp.vcxproj` の x64 Debug build は成功しています。`ToonOutline` shader の runtime compile/PSO 作成と実画面出力は、この README 更新では最終確認していません。x64 Release build も未確認です。

## Shader Layout

ファーストパーティの HLSL/HLSLI は `Shaders/` 配下に集約し、責務別に階層化しています。

主な分類:

- `Shaders/Shared/`: 共通定義、共通 constant buffer、基礎 shader
- `Shaders/Raster/Geometry/`: opaque、skinned mesh、tessellation、mesh shader
- `Shaders/Raster/Lighting/`: PBR lighting、shadow
- `Shaders/Raster/Toon/`: inverted-hull outline
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

### バックエンドのファイル構成

2026-09-16 に 3 バックエンドとも責務ごとに分割した（いずれも 1 ファイルに全部入りだった）。
`<Backend>GraphicsDevice.cpp` はフレームループと共通の問い合わせだけを持ち、以下が別ファイルにある:

| 後置き | 中身 |
| --- | --- |
| `Setup` | インスタンス/デバイス/スワップチェーン生成と破棄（Vulkan のみ。毎フレーム走らない部分） |
| `Resource` | RHI のテクスチャ・バッファ・ビュー |
| `Pipeline` | シェーダモジュール・パイプライン・ディスクリプタ（Vulkan） |
| `CommandEncoder` | `IRhiCommandEncoder` 実装 |
| `NativeMesh` / `SkinnedMesh` / `Mesh` | ネイティブメッシュ描画 |
| `RayMarch` / `Sky` / `Capture` | それぞれの経路 |
| `AccelerationStructure` | BLAS/TLAS（Vulkan） |

分割の前提として、各バックエンドの共有ヘッダに「その翻訳単位が無いと何も呼べない」ものを集めてある:
`VulkanGraphicsDeviceUtils.h`・`VulkanRayTracingUtils.h`、`Dx11ConversionUtils.h`、
**`OpenGLProcs.h`**（Windows は GL 1.1 のヘッダしか持たずローダも無いため、GL の enum と関数ポインタ
typedef 50 本と `LoadGlProc` がここにある。これを作るまで OpenGL のファイルは分割できなかった）。
GLSL のシェーダ文字列は `OpenGLShaderSources.{h,cpp}` にデータとして分けてある。

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
| `DebugRenderCommands` | レンダラーの機能設定（GBuffer デバッグ表示、露出、IBL、AO、レンダーパス、クリップ面）のコマンド定義 |
| `DebugLightCommands` | 平行光源そのものの状態（向き、強度、シャドウ投影範囲、バイアス、カスケード）のコマンド定義。`DebugRenderCommands` が「レンダラーの設定」を触るのに対し、こちらは「シーンのライト」を触るため分離 |
| `DebugAppCommands` | アプリ本体の実行制御（固定 dt、UI 表示、シーンフレーム数、起動完了）のコマンド定義。決定論キャプチャ（`Tools/Debug/golden.ps1`）が使う |

現在のコマンド:

| Command | Purpose |
| --- | --- |
| `ping` | 疎通確認。`pong` を返す |
| `help` | 登録済みコマンドを名前順で列挙 |
| `camera.get` | カメラ位置 / yaw / pitch / `panScale` を取得 |
| `camera.setTarget <x> <y> <z>` | 互換用の旧名。フライカメラの視点位置を設定 |
| `camera.setYawPitch <yaw> <pitch>` | カメラ姿勢を変更。**単位はラジアン**（度ではない） |
| `camera.setDistance <d>` | 互換用の旧名。Shift+ドラッグ時のパン速度スケールを変更（オービット距離ではない） |
| `light.spot.list` | 全 spot light の位置、姿勢、range、cone、intensity を取得 |
| `scene.save <path>` | シーンを保存。空白を含むパスも残りの引数を連結して扱う |
| `scene.load <path>` | シーンを破壊的にロード。空白を含むパスに対応。**PBRApp では 2026-09-20 時点で access violation を再現しているため使用禁止**。`LoadScene()` が object を破棄した後もサンプル側の raw pointer が残る。修正まではファイル変更後にアプリを再起動する |
| `debug.screenshot <path>` | バックバッファのキャプチャを予約。`OK queued <絶対パス>` を返す |
| `debug.screenshot.status` | 予約したキャプチャの完了確認。完了まで `PENDING`、完了後に `OK <パス>` |
| `app.fixeddt [seconds\|off]` | シミュレーション dt を固定値にする（`0`/`off` で実時間に戻す）。起動時は環境変数 `SASAMI_FIXED_DT` でも指定できる |
| `app.ui [show\|hide]` | ImGui の描画を止める／戻す（UI ロジックは動き続ける）。キャプチャから UI を除くため |
| `editor panels` / `editor panel <name> on\|off` | 登録済み UI パネル（カテゴリ・タブ数・開閉）の一覧と開閉 |
| `editor content list\|refresh\|open on\|off\|add <Assets相対パス>` | Content Browser: Assets/ のモデル一覧、ウィンドウ開閉、カメラ前方 3 m へ StaticModel として同期追加 |
| `editor play [check]\|stop\|playstate` | Play/Stop（シーンのスナップショットと復元）。`play check` は Play→全モデル・ライト・カメラ・太陽を動かす→Stop→ビット一致を確認（mismatch=N） |
| `editor gizmo <translate\|rotate\|scale\|local\|world\|status>` | エディタのギズモの操作種別と座標系を切り替える。status は種別・座標系・ドラッグ中か・編集回数に加え、入力が有効か（パネル上にカーソルがあると無効で灰色表示）と ImGui のマウス状態を返す |
| `editor layout reset` | エディタのドック配置を UE 風の既定（右上 WorldObjects、右下 Details、下段 Content Browser、アプリのパネルは Details の隣のタブ）に戻す |
| `editor view [lit\|unlit\|wireframe]` | ビューモードの取得・切替（Unlit は GBuffer Albedo ビュー、Wireframe は DX12 のみ） |
| `editor [on\|off\|status\|select <static\|skinned\|point\|spot\|camera\|directional> <index>\|pick <x> <y>\|selftest]` | エディタ UI（WorldObjects/Details/Content Browser）の表示切替・選択・画面座標でのピック・状態取得。selftest は TRS 合成／分解とピッキングの CPU チェック |
| `app.frame` | 起動後に描画したシーンフレーム数 `OK frame=<n>`。sleep ではなくフレーム数で待つため |
| `app.ready` | 起動フェーズが Running に達したか `OK ready=0\|1` |
| `render.get` | GBuffer デバッグ表示 / 露出 / IBL / AO / レンダーパス / 頂点キャッシュ最適化の現在値をまとめて取得。`vertexCacheOpt` は読み込み時にしか決まらないため設定はできず、A/B で環境変数が実際に届いたかを確認するために表示している |
| `render.gbuffer <index\|name\|list>` | GBuffer デバッグ表示を切り替え。`list` で一覧 |
| | 追加ビュー `16=ShadowCascadeIndex`: 各ピクセルが実際にサンプルしている CSM カスケードを色で表示（赤=0 / 緑=1 / 青=2 / 黄=3 / マゼンタ=範囲外）。カスケード選択の切り替わりが症状に関与しているかを計測するために追加 |
| `render.exposure <f>` | トーンマップ露出 |
| `render.nearclip <f>` / `render.farclip <f>` | カメラのクリップ面 |
| `render.shadowdistance <f>` | 平行光源のシャドウ距離 |
| `render.ibl <f>` / `render.ao <index>` / `render.path <0\|1>` | IBL 強度 / AO モード / レンダーパス |
| `render.gi <f>` | GI プローブ強度（`GI_Common.hlsli` の `g_giIntensity`）。`DeferredLighting_PS` は GI が有効な間 IBL 分岐を通らないため、`render.ibl` では間接拡散を切り分けられない。0 にすると GI 寄与だけが消えるので、明るさの出所を特定する用途にも使う |
| `render.swrtreflection <0\|1>` | SWRT レイトレース反射の有効/無効。既定は無効で他に実行時の切り替え手段が無く、間接鏡面の二重計上のような「反射が動いているときだけ出る」不具合を実機で A/B するために必要。有効化すると SSR は排他で無効になる |
| `render.trianglestats` | 読み込み済み静的ジオメトリの三角形面積分布（パーセンタイルと、シャドウマップ1テクセル未満の割合）。`SASAMI_TRIANGLE_STATS=1` を設定して起動した場合のみ収集する（全三角形を走査するため既定では起動コストを払わない） |
| `render.dagstats` | 送信済み静的メッシュの仮想ジオメトリ用クラスタ DAG を（未構築なら）構築して統計を返す。メッシュ数・ビルド/キャッシュヒット数・所要秒・階層数・階層ごとのクラスタ数/三角形数・終端グループ数。構築はメインスレッドで走るため、Bistro より重いシーンでは初回応答が CLI の 5 秒タイムアウトに掛かることがある（その場合はもう一度呼ぶと結果が返る） |
| `render.meshshader` | `<0|1>` 静的不透明メッシュを仮想ジオメトリのクラスタ経路（AS で LOD 選択＋カリング、MS でラスタ）で描く。引数なしで現在値を返す。初回はクラスタ GPU バッファ構築でメインスレッドが止まる |
| `render.tessellation` | `<0|1>` 頂点シェーダ描画にテッセレーションパイプラインを使う。`render.meshshader` とは独立 |
| `render.lodthreshold` | `<px>` クラスタ LOD 判定の目標画面誤差（ピクセル、既定 1.0）。0 で常に LOD0（VS 経路と同じ形状） |
| `render.clusterstats` | 直前フレームのクラスタ経路の統計（検討アイテム数・描画アイテム数・ディスパッチしたクラスタ数） |
| `render.clustergpucull` | `<0|1>` クラスタ経路のシャドウで GPU 駆動選択（CS の BVH 走査＋ExecuteIndirect）を使う。0 で CPU 駆動（per-item DispatchMesh）にフォールバック。引数なしで現在値 |
| `render.clustergpumain` | `<0|1>` メインビュー（GBuffer）のクラスタ描画で GPU 駆動選択（ビュースロット 4 の CS 選択＋ExecuteIndirect、テクスチャは SM6.6 bindless）を使う。既定 1。0 または PSO 未初期化時は CPU 駆動（per-item DispatchMesh）。引数なしで現在値 |
| `render.streamstats` | 直近の完了フレームの GPU クラスタ選択フィードバック: ビュー数、可視クラスタ合計/最大、走査オーバーフローのビュー数、要求グループ数/総数（率）、前フレーム比の増減。ストリーミング（Phase 4b）の要求集合 |
| `render.streambudget` | `<groups>` 仮想ジオメトリの常駐予算（DAG グループ数、0 = 無制限）。予算内に収まらない細かい階層は粗いクラスタで代替される。`render.streamstats` の末尾に常駐統計（resident/pinned/budget/loads/evictions/missing）が付く |
| `render.streamretention` | `<frames>` 直近この数フレーム以内に要求されたグループを追い出さない（既定 60、0 で従来どおり最終要求順のみ）。予算超過時のスラッシング抑制用 |
| `render.pagestats` | 仮想ジオメトリのグループページ（ストリーミング単位）の統計: ページ数、総バイト、最小/最大/平均サイズ、ページ内最大クラスタ数、セクション別バイト数 |
| `smoke` | ボリューメトリック煙（`SmokeVolumeSim` + `SmokeVolumeRenderPass`）の ON/OFF: `<0|1>` |
| `smoke.emitter` | 煙の発生源をワールド座標で移動: `<x> <y> <z> [radius]` |
| `smoke.wind` | 煙を押す環境風: `<x> <y> <z> <strength>`。strength 0 で無効 |
| `smoke.vorticity` | 渦の復元強度（Fedkiw et al. 2001 の vorticity confinement）: `<strength>`。0 で無効 |
| `smoke.buoyancymode` | 煙の浮力モデル: `<0|1>`。0=密度駆動（旧来）、1=温度場（Fedkiw et al. 2001 式8、Boussinesq） |
| `smoke.temperature` | 温度浮力のパラメータ: `<β> <α> <emitterT> <cooling>`（すべて ≥0）。引数なしで現在値を返す |
| `smoke.volume` | シミュレーション箱の位置とセルサイズ: `<originX> <originY> <originZ> <cellSize>` |
| `fluid.refraction` | 水面の画面空間屈折の強さ: `<strength>`。0 で歪みなし（A/B 用） |
| `fluid.splash` | 高さ場シムに水しぶきを注入: `<x> <z> [radius] [strength]`。サンプルの注入 UI は ImGui ボタンでデバッグパイプから押せないため、foam の検証用に追加 |
| `render.lensflare.mode` | レンズフレアの方式: `<0|1|2>`。0 = 手書きスプライト表、1 = シリコンスタジオ手続き（CEDEC 2019）、2 = ray-traced ghost 配置（Capcom RE:2023 方式、実装済み） |
| `render.lensflare` | レンズフレア（`LensFlareRenderPass`）の ON/OFF・強度・絞り羽根枚数: `<0|1> [intensity] [blades]`。引数なしで現在値を報告 |
| `render.lightshaft` | ゴッドレイ（`LightShaftRenderPass`）の ON/OFF と強度: `<0|1> [intensity]`。引数なしで現在値を報告 |
| `render.poolstats` | GPU ページプールの状態: 容量/使用量、常駐ページ数、フレーム内および累計のロード/追い出し、繰り越し（`deferred=N(ring=/full=/pending=/frag=)` と原因の内訳）、リング容量、空きリストの内訳（`free=..MB in N blocks (largest=..KB)`）、解放待ちバイト数（`pendingFree=`） 、入替フレームの CPU 時間と内訳（`cpu=A(copy=B)`）、最大ロードフレームのピーク記録（`peakLoad=N pages in A(copy=B)`）。`render.poolstats reset` でピーク記録を消せる。起動時フィルは run 中で最大かつ最もばらつく入替なので、安定後の入替を測るにはリセットしてから行う |
| `render.giprobes [spacing] [budget]` | GI プローブ格子の間隔（m）。引数なしで現在の間隔・格子数・総プローブ数・推定VRAM。設定すると格子を張り直して再ベイクする |
| `render.gibake [start]` | GI ベイクの進捗（完了フラグ・進捗率・完了/総プローブ数・残フレーム・停滞フレーム数・フェーズ）。`start` で再ベイク。**画の一致でベイク完了を判定してはいけない** — 進んでいない状態も同じ画になるため |
| `render.indirect [specular] [diffuse]` | 間接光のソースを選ぶ。鏡面 `none|ssr|swrt|hwrt` / 拡散 `none|gi|swrt|hwrt`。実装が無い値（hwrt 鏡面、swrt・hwrt 拡散）は黙って無視せず `ERR` と理由を返す |
| `render.gputime` | パス単位の GPU 実行時間（ミリ秒）と合計を取得。結果は `kFrameLatency`(=3) フレーム遅れる |
| `light.get` | 平行光源の全状態（向き・強度・投影範囲・バイアス・カスケード）を1行で取得 |
| `light.dir <yawDeg> <pitchDeg>` | 太陽の向き。**単位は度**（`camera.setYawPitch` のラジアンとは異なるため、応答は度とラジアンを併記する）。pitch は `[-90°, +90°]` に正規化されて返る（下記） |
| `light.intensity <f>` | 平行光源の強度 |
| `light.ortho <f>` | シャドウ正射影の half-size |
| `light.range <nearZ> <farZ>` | シャドウ投影の near/far |
| `light.bias <depthBias> [slopeScale] [normalBias]` | シャドウバイアス。第2・第3引数は省略時に現在値を維持 |
| `light.contactshadow <0\|1>` | スクリーンスペース・コンタクトシャドウの ON/OFF。方向光の可視性項に乗算されるため、シャドウマップ由来の破綻と切り分けるのに使う。シーンには `contact_shadow` として保存される。遮蔽判定は**リニア深度（ワールド メートル）**で行う（`ComputeContactShadowAlongRay`）: サンプル点とその画素の深度バッファ面はカメラから見て同一視線上に乗るため、両者のカメラ距離の差がそのままワールド空間の前後関係になる。厚みウィンドウは既定 5cm（`LightSystemShadow.cpp` の `contactShadowParams[2]`）で、レイ自身の視線方向スパン/ステップ数の 2 倍を下限に取る（Filament `screenSpaceContactShadow` の tolerance 相当。1 ステップでまたぐ深度より窓が狭くなって接触を取りこぼすのを防ぐ）。以前は同じ値をリバース Z の NDC 深度で比較していたため、NDC 深度 ≒ near/距離 の非線形により同じ設定が距離ごとに全く違うワールド厚みを意味し、近距離では影が出ず中距離で突然出ていた |
| `light.cascade <exponent> [blendFraction]` | カスケード分割指数とブレンド率 |
| `light.gizmo <0\|1>` / `light.gizmos <0\|1>` | 平行光源 / ポイント・スポットのギズモ表示。**PBRApp が登録**（フレームワークではなくサンプル固有の状態のため） |

#### 頂点キャッシュ最適化（既定OFF）

`SASAMI_VERTEX_CACHE_OPT=1` を設定して起動すると、静的メッシュの読み込み時に
`meshopt_optimizeVertexCache` と `meshopt_optimizeVertexFetch` を適用します
（`Source/AppFramework/Loader/MeshVertexCacheOptimizer.h`）。インデックスと頂点の並べ替えのみで、
描画される三角形も頂点データも変わりません。`StaticMeshCache` への保存**後**に適用するため、
キャッシュには最適化前のジオメトリが残り、環境変数だけを変えた A/B が成立します。

**既定で OFF なのは、実測で効果が無かったためです** — シャドウパスのスループット問題（241 Mtri/s）の
原因候補として試しましたが、外部GPU負荷を打ち消す Shadow/GBuffer 比で +0.1% と変化がありませんでした。
再検証できるよう残してあります。詳細は TODO.md。

#### yaw/pitch の有効範囲

`Math::DirectionFromYawPitch`（`x = sin(yaw)cos(pitch)` / `y = -sin(pitch)` / `z = cos(yaw)cos(pitch)`）が
単射なのは **pitch ∈ [-90°, +90°]** の範囲だけです。これを超えると `cos(pitch)` の符号が反転して x,z が反転するため、
太陽の高度は上がらず「方位が 180° 回った同じ高度」に折り返されます（pitch=180° は高度 0°、つまり地平線上の太陽）。

そのため `ApplicationCore::SetDirectionalLight` が `Math::NormalizeYawPitch` で pitch を有効範囲に畳み込みます。
**畳み込みは向きを保存する**ので（`sin(π-p)=sin(p)` で y が不変、`cos(π-p)=-cos(p)` と `yaw+π` の符号反転が打ち消し合う）、
見た目は変わらず表現だけが正準形になります。UI の Pitch スライダも `-89°〜+89°` に制限してあり、
折り返し領域には入れません。`light.dir` / シーン読み込み / UI はすべて `SetDirectionalLight` を通るので、
どの経路から設定しても範囲外の値は保持されません。

TCP実装は `IDebugTransport` を実装したクラスを1つ足すだけで追加できます（キュー・レジストリ・コマンド層の変更は不要）。

`light.*` 系のうち `light.gizmo` / `light.gizmos` だけはフレームワーク側ではなくサンプル（`Samples/PBRApp/RenderingAppUI.cpp`）から登録しています。`ApplicationCore::GetDebugCommandRegistry()` がレジストリを返すため（`SASAMI_DEBUG_REMOTE` 未設定時は `nullptr`）、アプリ固有の状態を `ApplicationCore` に押し込まずにスクリプトから操作できます。レジストリは同期機構を持たないので、登録は初期化時のメインスレッドからのみ行ってください（受信スレッドは行をキューに積むだけで、レジストリを読むのはメインスレッドの `DrainPendingCommands` です）。

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

**入力はフォーカス時のみカメラに届く（2026-09-06 修正）**: 生入力は `RIDEV_INPUTSINK` で登録しているため非フォーカス時にも届く。
以前はそれをそのままカメラに流していたので、他アプリ向けのキー入力やホイールでフライカメラが勝手に動き、ヘッドレスの画像 A/B が
成立しなかった（実測: 30 秒で前方ベクトルの 4 倍ぶん後退）。`InputSystem` が毎フレーム前景ウィンドウを見て、非フォーカス中の生入力は
ImGui にもカメラにも渡さない。フォーカスを失った瞬間には移動キーと右ボタンを解放する（キーアップが他ウィンドウへ行くため）。
なお同一状態の連続キャプチャにはまだ 0.5% 未満の差が残る。これは ImGui パネルの数値と時間方向のフィルタによるもので、比較時はパネル領域を除外する。

ウィンドウの配置・重なり・他アプリの状態に影響されずに、**実際に描画された絵**を PNG で取り出す仕組みです。`Tools/Debug/ss.ps1` の `PrintWindow` 方式と違い、ウィンドウ枠もタイトルバーも入らず、DWM 合成やウィンドウの重なりの影響も受けません。

```powershell
# 起動（デバッグパイプを有効化）
Tools\Debug\run-debug.bat
# ヘッドレス（ウィンドウを一切表示しない）
Tools\Debug\run-debug.bat 1

# 撮影（予約→ポーリング→パス出力までを1コマンドで隠蔽）
powershell -ExecutionPolicy Bypass -File Tools\Debug\screenshot.ps1 -Path Tools\Debug\Screenshots\shot.png

# CLI スモークテスト（必要ならヘッドレス PBRApp も自動起動）
powershell -ExecutionPolicy Bypass -File Tools\Debug\test-cli.ps1
```

Codex から実行時描画を検証する場合は、プロジェクト固有スキル
`.codex/skills/sasami-renderer-cli-test/SKILL.md` がこの CLI、PNG の目視、GPU 時間、今回起動分のログを
組み合わせた確認手順を定義します。

| 要素 | 責務 |
| --- | --- |
| `Renderer/Capture/BackBufferCapture` | リードバックへの `CopyTextureRegion` と WIC による PNG エンコード |
| `Renderer/Runtime/RendererScreenshot.cpp` | 予約状態の管理と、フレーム送信の前後へのフック |
| `AppFramework/.../DebugCaptureCommands` | `debug.screenshot` / `debug.screenshot.status` の定義 |
| `Tools/Debug/screenshot.ps1` | 予約とポーリングを隠す CLI ラッパ |
| `Tools/Debug/test-cli.ps1` | 疎通・状態読取・GPU 時間・PNG 出力のスモークテスト |
| `Tools/Debug/run-debug.bat` | デバッグパイプ有効（＋任意でヘッドレス）での起動 |
| `Tools/Debug/run-wsl.ps1` | WSL interop から Windows CIM 経由で切り離して起動 |

**WSL から起動する場合の注意**: WSL の interop 経由で起動したプロセスは、**起動元のコマンドが終了した時点でツリーごと終了させられます**。`cmd.exe` の `start` や `Start-Process` では切り離せません。WSL 側から常駐させたい場合は、interop のツリーに属さない形で作る必要があります:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w Tools/Debug/run-wsl.ps1)" -Headless
```

Windows 側から直接 `run-debug.bat` を叩く場合はこの問題は起きません。

実装上の要点:

- **行ピッチ**: `GetCopyableFootprints` が返す `RowPitch` は `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`(256) に切り上げられており、`width * 4` とは一致しません。詰め直さずに書くと絵が斜めにずれます
- **色順**: `R8G8B8A8` / `B8G8R8A8`（各 `_SRGB` 含む）のみ受理し、内部で WIC が要求する BGRA に正規化します。想定外のフォーマットは**黙って変な色で保存せず失敗**させます
- **GPU 同期**: コピーは `Renderer::SubmitAndPresent` の**送信前**に記録し、PNG は**送信後に `WaitForGPU()` してから**書き出します。待たずに Map すると壊れた絵やティアリングした絵になります
- **合流点がひとつ**: メインのフレームグラフ、グラフ失敗時の2つのフォールバック、`RenderBootFrame` のいずれの経路も `SubmitAndPresent` を通るため、どの経路で組まれたフレームでも撮れます
- **2コマンド構成の理由**: ハンドラはメインスレッドで実行され、キャプチャの完了も同じスレッドの後続フレームでしか起きません。ハンドラ内で完了を待つとレンダーループごとデッドロックするため、予約とポーリングを分けています
- **ヘッドレス**: `SASAMI_HEADLESS=1` で `SW_HIDE` 起動します。最小化(`SW_MINIMIZE`)は Present が抑制されうるため使いません。非表示でもフレームは実行されバックバッファは埋まるので、キャプチャは成立します

## Depth Convention（リバースZ）

**このレンダラーはリバースZです。NDC の深度は near = 1.0、far = 0.0** で、標準Zとは逆向きです。
深度に触るコードを足すときは必ずこの規約に合わせてください。

- **動機**: 深度値はほぼ 1/距離 で near 付近が細かく遠方が粗い。一方 float は 0 付近が細かく 1 付近が粗い。
  標準Zは遠方を 1 付近に置くため両方の粗い側が重なって二重に損をする。リバースZは遠方を 0 付近に置くので
  互いに打ち消し、距離全域でほぼ均一な精度になる。実行時コストはゼロ（演算数もパス数も変わらない）
- 実測: `near = 0.0005` / `far = 500`（比100万）でも Z-ファイティングが出ません。標準Zではこの設定で
  建物ファサードと植栽に密なスペックルノイズが出ていました
- **深度クリア値は `0.0f`**、**深度比較は `GREATER_EQUAL`**（シャドウ深度専用 PSO は `GREATER`）、
  比較サンプラ `MakeShadowComparisonSampler` も `GREATER_EQUAL`
- **深度バイアスは符号が反転します。** ラスタライザの `DepthBias` / `SlopeScaledDepthBias` は負値、
  シェーダ側は `sdepth - bias` ではなく `sdepth + bias`
- フルスクリーンで「背景だけに描く」パス（スカイボックス、ボリュメトリック雲）は NDC z を **0.0** に置きます
- 逆VP行列でカメラレイを作るときは **z=1 が near 点、z=0 が far 点**です
- 「空/背景」の判定は `depth >= 1.0` ではなく **`depth <= 0.0`**
- `ReconstructWorldPos`（`Shaders/Shared/Common/WorldPosReconstruction.hlsli`）は逆VP行列のみを使うため
  規約に依存しません。そのまま使えます
- **線形カメラ距離**（`GBufferNormal` の `.w`、`TransparentBackfaceDistanceTex`）はこの規約と無関係です。
  SWRT の多くのパスはこちらを使っているので、深度規約に合わせて触らないでください
- spot/point のシャドウマップは `D16_UNORM` で精度上の利得はありませんが、比較サンプラが
  directional/spot/point で共有されているため規約を混在させられず、統一してあります

## Color Space

シェーディングは一貫してリニア空間で行い、sRGB との往復はハードウェアに任せます。

- **baseColor テクスチャ**のみ `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` で作成し、サンプラにデコードさせます。
  normal / metallicRoughness / occlusion はリニアな測定値なので `R8G8B8A8_UNORM` のままです。
  用途は `SceneSubmitter` の `TextureColorSpace` で指定し、同じ画像が baseColor と occlusion に使い回された場合に
  取り違えないよう、テクスチャキャッシュのキーにも色空間を含めています
- **ミップ生成**は色空間に合わせます。sRGB テクスチャは `TextureMipChainBuilder::BuildRgba8(..., srgbColor = true)` で
  リニアに戻して平均し、sRGB に再エンコードします。格納空間のまま箱フィルタすると、デコードを入れた意味が失われます
- **GBufferAlbedo** は `R8G8B8A8_UNORM_SRGB` です。G-Buffer PS が書くのはリニア値なので、UNORM のまま格納すると
  8bit では暗部が破綻します。`_SRGB` ビューなら書き込み時にエンコード・読み出し時にデコードされます
- リソース・RTV・SRV のフォーマットは必ず一致させます。D3D12 では型付きリソースを別フォーマットのビューで
  参照できるのは TYPELESS の場合（およびスワップチェーンバッファ）に限られるためです。RHI 層にも
  `RhiFormat::R8G8B8A8UNormSrgb` があり、DX12 / DX11 / Vulkan / OpenGL の各バックエンドで対応済みです
- 最終出力の `1/2.2` エンコードは `Shaders/Effects/PostProcess/ToneMap_PS.hlsl` の 1 箇所だけです

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
- CSM の light-space 深度スラブはライト平面を起点に、`maxZ = lightOffset + frustumRadius + depthMargin` まで張ります。ライトとカスケード境界球の間にある caster（街路に影を落とす上階や庇）も必ずスラブに含める必要があるためです。両端が frustumRadius と depthMargin だけの関数、つまりカメラ姿勢に依存しない定数になるので、深度方向のスナップは不要です
- 法線オフセットバイアス `m_shadowNormalBias` はカスケード0のテクセルサイズを基準に、各カスケードのテクセルサイズ比でスケールします。世界座標の定数のままだと、テクセルが桁違いに粗い遠方カスケードでサンプルを表面から十分に浮かせられません

検証状況: 全プロジェクトのビルドと単一シーンの連続描画は確認済みです。directional CSM は同一カメラポーズでの変更前後 A/B 撮影（`Tools/Debug/verify-shadow-slab.ps1`、比較先は `Tools/Debug/Screenshots/`）で確認済みです。複数の spot/point light を同時配置した最終的な影の目視比較は未実施です。

## Render Pipeline

既定パス順:

```text
Shadow -> OpaqueGBuffer -> RuntimeAO -> RuntimeAOBlur -> Lighting -> ScreenSpaceReflection -> SoftwareReflection -> SoftwareReflectionComposite -> Skybox -> TransparentBackfaceDistance -> TransparentSceneColorCopy -> TransparentLighting -> TransparentComposite -> PostProcess
```

Toon preset はこの既定順の `Lighting` 直後に `ToonOutline` を挿入します。通常の preset には追加されません。

主なパス:

- `Shadow`: directional CSM/VSM、spot 最大8灯、point 最大4灯×6面を描画します。spot/point は各ライトに `shadowIndex` を割り当て、上限を超えたライトは影なしで照明計算を継続します。forward (`CookTorranceGGX_PS.hlsl`) と deferred (`DeferredLighting_PS.hlsl`) は同じ shadow helper を使います
- `RuntimeAO` / `RuntimeAOBlur`: SSAO または runtime AO。間接光(IBL/GI)への適用に加え、`AO Direct Lighting`(`aoDirectLightingStrength`, 0〜1, 既定 0.5)で直接光にもライトごとの遮蔽を適用する。適用はマイクロシャドウイング(Chan 2018 "Material Advances in Call of Duty: WWII"、Filament 採用式。AO を可視コーンの開口とみなし NdotL に応じて減衰)で、シャドウマップの visibility に追加乗算される。さらに point/spot 光には光源方向へのスクリーンスペースコンタクトシャドウ(UE Contact Shadows / Filament punctual 相当。レイ長は光源距離でクランプ、コンタクトシャドウ設定でゲート)を適用。PBRApp の AO 設定 UI と settings ini に永続化される。同じ `aoDirectLightingStrength` はレイトレース DI にも適用される: DXR パス（RayTracing.hlsl）は directional/point/spot の直接光にマテリアル AO 由来のマイクロシャドウイングを乗算し、SWRT ReSTIR DI パス（SWRT_ReSTIR_Shade_CS）は CPU で焼き込んだマテリアル実効 AO（occlusion テクスチャ R 平均を strength で lerp、hitPosition.w に metallic と 5bit+5bit パック）を反射ヒット面の直接光マイクロシャドウイングとアンビエント減衰に使う（マイクロシャドウ式は `Shaders/Shared/Common/MicroShadowing.hlsli` に共通化）。EmissiveColor もこの `ao` を乗算してから最終合成する(`CookTorranceGGX_PS.hlsl`/`DeferredLighting_PS.hlsl` の shading 出力側のみ。GBuffer emissive チャンネルは生値のまま書き、ディファードのフルスクリーン合成側で一度だけ `ao` を掛けるため二重適用にはならない)
- 点光源/スポットの距離減衰: 全経路（ラスタ Forward/Deferred・BasicShader・DXR・SWRT ReSTIR/反射・GI プローブ）で `Shaders/Shared/Common/PunctualLightFalloff.hlsli` を共有する。`RenderSettings::punctualLightFalloff` で `InverseSquare`（既定。Karis 2013 eq.9 / Filament の窓付き逆二乗）、`LinearSquared`（旧ラスタ曲線 `(1-d/r)^2`）、`SmoothWindow`（旧 SWRT/GI 曲線 `(1-(d/r)^2)^2`）を切り替えられる。デバッグコマンドは `render.lightfalloff linear|smooth|physical`、settings ini にも保存される。ライト強度は無単位で、lm→cd 換算は未実装。
- `OpaqueGBuffer`: opaque mesh から GBuffer と depth を出力
- `Lighting`: GBuffer/depth/light/shadow/IBL/AO を読む fullscreen deferred lighting combine。`SceneColor` を生成
- `ToonOutline`: Toon preset のみ。opaque mesh の inverted hull を既存 depth とともに `SceneColor` へ描画
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
- 風エフェクト（火・煙）は `voxel.wind.fx volume 1`（Wind タブの「Volume」）で、粒子をグリッドへスプラットしてレイマーチするボリューム表示になります（自己影・多重散乱近似・黒体色の炎。DX12 のみ）。濃さなどは `voxel.wind.fx volume style smoke= flame= emit= soot=` で調整します。
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

#### 層と名前空間

ルート名前空間は **`SasamiEngine`**、その直下に層を置きます。

| 層 | 名前空間 | 宣言数 |
|---|---|---|
| `Source/Foundation` | `SasamiEngine::Foundation` | 23 |
| `Source/Renderer` | `SasamiEngine::Render` | 231 |
| `Source/AppFramework` | `SasamiEngine::App`（`App::UI` 42 / `App::Debug` 13） | 62 |

目標は 層＝名前空間 の対応で、依存の向き（Foundation ← Render ← App ← Samples）を
コンパイラに検出させることです。**旧 `namespace SasamiRenderer` からの移行は 2026-09-13 に完了**し、
同日ルートを `Sasami` から `SasamiEngine` に改めました。改名は 391 ファイル・811 箇所に及ぶため、4 構成のビルドに加えて**DX12 / DirectX11 / Vulkan / OpenGL の 4 バックエンドすべてで実際に起動することを確認済み**です（ウィンドウクラス名 `"Sasami Engine App Window"` での `RegisterClass` と、Vulkan の `pApplicationName` / `pEngineName` は実行時にしか効かないため）。ルートを層の名前（`SasamiRenderer`）にしないのは、
Render が3つある層のうちの1つにすぎず、`SasamiRenderer::Foundation::JobSystem` が成り立たないためです。

各ヘッダ末尾の `namespace SasamiEngine { using namespace <層>; }` は、
上の層が `Foundation::` / `Render::` を明示修飾せずに済ませるための指示です（Render 119 / App 56 / Foundation 18 箇所）。
外すには約 76 ファイルの未修飾参照を修飾し直す必要があり、機械的な変換にはならないため据え置いています。

**意図的に旧名のまま残しているもの**（互換性の契約であり、名前空間とは無関係）:
`\\.\pipe\SasamiRenderer.Debug`（`Tools/Debug/*.ps1` が接続する）、
`# SasamiRenderer Scene v1`（`PBRApp.scene` のヘッダ）。
`PBRApp.settings.ini` のバージョン表記は 2026-10-07 にコメント行 `# SasamiRenderer PBRApp settings v1` から
読み取り可能なキー `[meta] version` へ移しました（コメントはパーサが捨てるため不活性だった）。

既知の逆流: `Source/Foundation/Profiling/Profiler.h` が Render 層の `CommandList` を参照しています。
分割によって初めてコンパイルエラーとして表面化したもので、現在は `SasamiEngine::Render::CommandList` と
明示修飾して可視化してあります。詳細と対応案は TODO.md。

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

- ディスクリプタヒープはすべて `Dx12GraphicsDevice::CreateDescriptorHeap` で `RhiHeap_%08llu` の名前が付き、
  生成時に `[RhiHeapTrace] created id=.. type=.. numDescriptors=.. flags=..` がログに出ます。
  デバッグレイヤのヒープ不一致メッセージが `'Unnamed ID3D12DescriptorHeap Object'` ではなくヒープ名を出すため、
  どのヒープ同士の衝突かがメッセージだけで分かります（`CreateCommittedResource` の `RhiRes_` と同じ方式）。
  なお `ImGuiCoordinator` は `ID3D12Device` を直接呼ぶためこの命名の対象外です。
- **パスは自前の shader-visible ヒープを作らず、レンダラの共有 SRV ヒープから確保してください。**
  自前ヒープを `SetDescriptorHeaps` してから描画すると、そのパスが使うスロットが正しくても、
  共有ルートシグネチャの他のテーブルスロットに前段のパスが残したメインヒープのハンドルが
  「現在のヒープに属さない」と判定され、デバッグレイヤがヒープ不一致を報告します。

`SrvDescriptorAllocator`（`Source/Renderer/Resources/`）は free list を持たない bump allocator で、`Renderer::Initialize` が渡す容量がレンダラー全体の SRV/UAV 上限になります。現在 **16384**（シェーダ可視 CBV/SRV/UAV は 32B/個なので 512KB）。クラスタ経路がメッシュごとの bindless vertex-buffer SRV を必要とするため、4096 から拡張しました。

512 では Bistro で不足しました。シーンのマテリアルテクスチャだけで約 490 を消費し、`SkyCubemapGenerator` が要求する 24 個（equirect SRV 1 + mip UAV 12 + mip SRV 11、face 2048 の場合）が入りきらずに失敗します。この失敗はユーザーからは見えず、`Skybox` が CPU cubemap 生成へ黙ってフォールバックして起動が約 14.6 秒延びるだけなので、枯渇ログには capacity / used / requested を出しています。新しい常設 SRV 消費者を足すときはこの予算を意識してください。

**シェーダモデルの解決**: `ShaderCompilationService::ResolveEffectiveShaderModel()` が `D3D12_FEATURE_SHADER_MODEL` を降順に問い合わせてデバイス上限を決めます。候補表 `kKnownShaderModels` は **ABI 値（0x60〜0x69）を直書きしてキャストします**。`D3D_SHADER_MODEL_6_x` は d3d12.h の **enum メンバーであってマクロではない**ので、`#ifdef D3D_SHADER_MODEL_6_9` のようなガードを付けると常に false になり、表が 6_0 の1要素に潰れて**どんなハードウェアでも 6_0 に落ちます**。これを検出するための `static_assert` を2本置いてあります。ランタイムが知らないモデルを候補に入れるのは安全です（`CheckFeatureSupport` が E_INVALIDARG を返し、降順ループが次を試します）。

**シェーダモデルはビルド時と実行時で対にすること。** MSBuild の `RendererShaderModel`（`SasamiRenderer.vcxproj`）と `GetConfiguredShaderModel()` の既定は**どちらも `6_7`** です。ビルドは `<name>.<entry>.<profile>_<model>.cso` という名前でプリコンパイルし、実行時はその名前で引くので、片方だけ変えると `.cso` が一切ヒットせず全シェーダがランタイムコンパイルに落ちます。また `ResolveEffectiveShaderModel` は要求をデバイス上限まで**引き下げる**ので、対象ハードより高いモデルを要求すると解決後のプロファイル名がビルド生成物と食い違います（`6_9` 要求／デバイス `6_8` で実際に起きていました）。`RENDERER_SHADER_MODEL` 環境変数で両方まとめて上書きできます。

**生の `CommandList` で描くパスは自分で `DebugIncrementDrawCount()` を呼ぶこと。** `CommandList::DrawInstanced` / `DrawIndexedInstanced`（`Source/Renderer/RHI/GraphicsDevice.h`）は素通しの薄いラッパーでカウンタを触りません。カウンタは RenderGraph の `[DrawRange] <pass>: draws [a, b)` ログの根拠なので、呼び忘れると**そのパスが何も描いていないように見えます**（実際にはトーンマップがこれで「0 draw」に見えていました）。RHI エンコーダ（`enc->Draw()` 系）経由の描画は自動で計上されます。

**VS/PS のシグネチャ整合はビルドでは検出できません。** 検査されるのは PSO 作成時、つまり実行時だけです:

- `Shaders/Debug/Meshlet/MeshletDebug_PS.hlsl` は通常の頂点シェーダとペアの PSO（`RenderPipelineStateCache` の "MeshletDebug"）で使われます。メッシュシェーダ系 PSO の PS は専用の `Shaders/Raster/Geometry/MeshShader/MeshShaderGBuffer_PS.hlsl`（GBuffer 5 MRT 出力）です。**VS は per-primitive 属性を供給できない**ため、この PS に `MESHLET_INDEX` のような属性入力を足すと `CreateGraphicsPipelineState` が `E_INVALIDARG` を返し、`RenderPipelineStateCache::Initialize` の失敗＝起動不能になります。
- メッシュレット index は `SV_PrimitiveID / kMaxTrianglesPerMeshlet` で求めます。除数は `Shaders/Shared/Common/MeshletConstants.hlsli` にあり、CPU 側 `MeshletBuffer::kMaxTrianglesPerMeshlet` と対になる唯一の定義です。この導出が正しいのは、ビルダーが index buffer を連番のチャンクに切っている間だけです。
- 64 本のシェーダのうち 13 本は vcxproj に CustomBuild 登録が無く**実行時コンパイル**です。ビルドが緑でもシェーダが壊れている場合があります。加えて、仮に CustomBuild を足しても dxc の単体コンパイルではステージ間リンケージは検証されません。

## Implementation Policy

レンダラー、graphics API、shading、lighting、ray tracing、GI、material、render graph の変更は、実装前に短い計画を作り、論文、公式 API 仕様、確立したエンジン実装、ベンダーサンプル、保守されている OSS renderer を設計・挙動の根拠として使います。互換性のないライセンスのコードはコピーしません。

実装後は可能な範囲で shader compile（warning as error）、x64 Debug/Release build、D3D12 Debug Layer + 必要に応じて GBV、サンプルシーンの描画・リソース寿命・フレーム安定性を検証します。未検証、推測、部分実装、backend 固有の制限は README または TODO に残します。

## RHI Implementation Notes

Status as of 2026-09-23:

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
- メッシュシェーダ PSO（AS/MS/PS）の初期作成失敗は 2026-09-04 に修正済みです。GBuffer encoding は `GBufferEncode.hlsli` を VS/MS 経路で共有し、MS 専用 `MeshShaderGBuffer_PS.hlsl`、material/texture binding、`MeshShaderRenderPass` の登録まで接続済みです。`render.meshshader 1` は現在 static opaque GBuffer と directional CSM のクラスタ経路を有効化します。GPU 選択・page pool を含む現在の構成と制限は Virtual Geometry 節を参照してください。
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

- 保存対象: メインカメラ (`position`/legacy `target`/`yaw`/`pitch`/`distance`/`clip`/`move_speed`)、directional light (shadow mode/cascade/bias 含む)、ユーザーが追加した point/spot light、`RenderSettings` の永続化対象フィールド、GI トグル (enabled/intensity/ema/baked)、probe grid preset/debug 表示/radius、ライトギズモ表示フラグ。
- **`PBRApp.settings.ini` は疎保存**（2026-10-07〜）: 既定値と一致する項目は書き出さず、**ファイルに無いキーは読み込み時に「現在のコード側の既定値」を採ります**。これによりコード側で既定値を変更すると、ユーザーが触っていない設定には自動で届きます。従来は全フィールドを無条件に書いていたため、既定値を変えても既存の ini が永久にピン留めし、既定値を変えるたびにキー名を変える運用（`useMeshShader` → `useClusterMeshShader` → `clusterMeshShaderPath`）が必要でした。実害例として `clusterShadowLodThresholdPx` を 2026-09-25 に 4→8、09-27 に差し戻した際、その2日間に保存した環境に差し戻しが届きませんでした。行を消せばその項目だけ既定値に戻り、ファイルごと消せば全リセットになります。
  - 疎保存で表現できないのは「既存キーの意味が変わる」変更（リネーム・単位変更・enum 番号の入れ替え）だけです。そのために `[meta] version` を読み取り可能なキーとして持ち、`RenderingAppSettingsFile.cpp` の `MigrateSettings()` が読み込み前に旧バージョンのドキュメントを書き換えます（現時点では移行ルール無し。キーの追加・削除は疎保存が吸収するのでバージョンを上げません）。`IniDocument` に `Has` / `SetValue` / `RenameKey` を追加して移行を書けるようにしてあります。
  - float は `IniWriter::FormatFloat()` が `std::to_chars` の最短往復表記で書き出し、既定値との比較も**整形後の文字列**で行います（従来のストリーム既定 6 桁は往復安全でなく、保存のたびに値がずれる可能性がありました）。
  - enum は読み込み時に範囲クランプします（`ApplicationCoreScene.cpp` の `shadow_mode` / `depth_range_mode` と同じ書き方）。uint32 フィールドは負値を 0 にクランプします。
- 保存先 (プロジェクトルート、per-user・git 管理外): `PBRApp.scene`（カメラ+ライト）、`PBRApp.settings.ini`（描画/アプリ設定）、`PBRApp.gi_probe_cache.bin`（一致時のみ復元する GI probe SH sidecar cache）。パスは `ApplicationResourcePaths::ResolveConfigPathString()` が解決。
- 復元は非破壊: `ApplicationCore::ApplyCameraAndLights()` が `ClearObjects()` を呼ばず、既存のアクティブカメラ・directional light へ in-place 適用し、保存済み point/spot light のみ再構築します（`[static_model]` と未知セクションは無視）。破壊的なフルロードが必要な用途向けに `LoadScene()` は従来どおり残しています。
- **既知の制限**: `LoadScene()` / `scene.load` は `ClearObjects()` で object を破棄しますが、PBRApp の `RenderingApp` が保持する camera/model raw pointer を再バインドしません。2026-09-20 に Release + cluster mesh shader path で access violation を再現しています。通常起動時の session 復元は非破壊な `ApplyCameraAndLights()` を使うため、この問題を踏みません。
- レンダラは `Renderer::GetRenderSettings()/SetRenderSettings()` で `RenderSettings` を一括 round-trip します。読み込みは `renderer.GetRenderSettings()` ではなく `RenderSettings{}`（pristine defaults）から始めるため、起動後の再読込でも「ファイルに無いキー＝既定値」が成立します。
- GI probe のベイク結果は `PBRApp.gi_probe_cache.bin` に保存します。cache version（現在 v3）、scene/settings hash、probe grid layout が一致した場合だけ復元し、不一致またはロード失敗時は GI が有効なら再ベイクを要求します。これは再利用可能な editor bake asset ではなく PBRApp の session sidecar cache です。
- 保存されないもの: skybox パス、ImGui ウィンドウレイアウト（`imgui.ini` が別管理）。

Implementation references for the portable buffer copy/readback contract:

- Microsoft `D3D12_HEAP_TYPE`: readback-heap resources must remain in `D3D12_RESOURCE_STATE_COPY_DEST`: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
- Microsoft D3D12 readback guidance: synchronize copy completion before mapping the readback buffer: https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
- Khronos `vkCmdPipelineBarrier` / `VkBufferMemoryBarrier`: buffer barriers define the resource-specific access dependency around buffer copies: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkCmdPipelineBarrier.html
- Khronos `VkDescriptorSetLayoutBinding`: immutable sampler handles are copied into the layout but sampler objects must outlive layout/set use: https://registry.khronos.org/vulkan/specs/latest/man/html/VkDescriptorSetLayoutBinding.html

レンダラー、graphics API、shading、lighting、ray tracing、GI、material、render graph を変更する場合は、実装前に短い実装方針を作り、可能な範囲で論文、公式 API 仕様、Unreal Engine などの既存実装、ベンダーサンプル、保守されているオープンソースレンダラーを参照してください。

外部コードは設計や挙動の参考に留め、互換性のないライセンスのコードをコピーしないでください。実装後は Debug/Release ビルド、シェーダーコンパイル、実行時描画確認で検証してください。ローカル環境で実行できなかった検証は、その理由を明記してください。
