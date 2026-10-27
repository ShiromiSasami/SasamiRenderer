# Debug Tools

実行中の SasamiRenderer を外部から操作・観測するための CLI 一式です。
アプリ側の受け口は `Source/AppFramework/Debug/RemoteControl/`、名前付きパイプ
`\\.\pipe\SasamiRenderer.Debug` です。**既定では無効**で、`SASAMI_DEBUG_REMOTE=1`
を設定した起動時のみ開きます。

| ファイル | 用途 |
| --- | --- |
| `run-debug.bat` | デバッグパイプを有効にして起動。第1引数に `1` を渡すとヘッドレス（`SASAMI_HEADLESS=1`） |
| `run-release.bat` | Release の PBRApp を同じ条件で起動。性能計測向け |
| `run-wsl.ps1` | WSL interop の終了に巻き込まれないよう Windows CIM 経由で Debug/Release を起動 |
| `pipe.ps1` | 任意のコマンドを送る汎用クライアント。`;;` 区切りで複数送れる |
| `screenshot.ps1` | バックバッファを PNG で取得。予約とポーリングを内部で隠蔽する |
| `test-cli.ps1` | 疎通・状態読取・GPU 時間・PNG 出力をまとめて検証する読み取り中心のスモークテスト |
| `sweep-clip-shadow.ps1` | ニアクリップとシャドウ距離を掃引し、固定カメラポーズで各設定を撮影する A/B 用スイープ |
| `verify-light-gizmo.ps1` | ディレクショナルライトの Gizmo が ImGui オーバーレイ移設後も描画されるかをカメラ yaw 掃引で確認 |
| `verify-shadow-slab.ps1` | CSM 深度スラブ修正の検証。`Screenshots/sweep/shadow_80_view*.png` と同じポーズを再撮影して直接比較できる |
| `ss.ps1` | **旧方式**。`PrintWindow` によるウィンドウキャプチャ。ウィンドウ枠が入り、重なりや DWM 合成の影響を受けるため `screenshot.ps1` を推奨 |

## 使い方

```powershell
Tools\Debug\run-debug.bat            # 通常起動
Tools\Debug\run-debug.bat 1          # ヘッドレス起動

powershell -ExecutionPolicy Bypass -File Tools\Debug\screenshot.ps1
powershell -ExecutionPolicy Bypass -File Tools\Debug\pipe.ps1 -Script "render.get;;camera.get"
powershell -ExecutionPolicy Bypass -File Tools\Debug\test-cli.ps1
```

`test-cli.ps1` は CLI が有効な PBRApp が既に動作中ならそれを変更せず再利用し、存在しなければ
`Build/bin/x64/Debug/PBRApp.exe` をヘッドレスで起動します。疎通、描画・カメラ・ライト状態、GBuffer
一覧、GPU タイムスタンプ、PNG 署名を検証します。自分で起動したプロセスだけを終了し、既存プロセスは
終了しません。Release を対象にする場合は `-Configuration Release`、起動済みインスタンスだけを対象に
する場合は `-RequireRunning`、PNG が不要な非描画確認では `-SkipScreenshot` を指定できます。

`screenshot.ps1` は `-Path` 省略時、`Build/screenshots/shot-<日時>.png` に保存し、
最終行に保存先の絶対パスだけを出力します（進捗は `Write-Host` に流すので、
呼び出し側が標準出力を受け取るとパスだけが得られます）。

## 主なコマンド

`help` で全一覧が出ます。よく使うもの:

| コマンド | 内容 |
| --- | --- |
| `render.get` | 現在の描画設定（GBufferビュー・露出・IBL・AOモード・実行時AO・レンダーパス） |
| `render.gbuffer <index\|name\|list>` | GBuffer デバッグビューの切り替え |
| `render.exposure` / `render.ibl` / `render.gi` / `render.ao` / `render.path` | 各設定の取得・変更。`render.gi 0` は GI 寄与だけを消せるので明るさの出所の切り分けに使える |
| `render.runtimeao <0\|1>` | 実行時 AO（スクリーンスペース AO）パスの有効・無効。`render.ao` は OR で合成されるため単独では AO を切れず、A/B にはこちらが必要 |
| `debug.screenshot <path>` / `debug.screenshot.status` | キャプチャの予約とポーリング（`screenshot.ps1` が使う） |
| `camera.get` / `camera.setTarget` / `camera.setYawPitch` | フライカメラ操作。`setTarget` は互換用の旧名で視点位置を設定。**`setYawPitch` の単位はラジアン** |
| `camera.setDistance <d>` | 互換用の旧名。オービット距離ではなく Shift+ドラッグ時のパン速度スケールを設定 |
| `scene.save <path>` / `scene.load <path>` | シーンの保存・読み込み |

設定変更系は**要求値ではなく読み戻した値**を返します。内部でクランプ・拒否された場合に
「適用された」と誤報しないためです。

## ゴールデン画像回帰ゲート (golden.ps1)

固定カメラ・固定タイムステップで PBRApp をヘッドレス起動し、`golden-views.json` の各ビューを撮影して
基準画像と比較します。先に基準を撮り（capture）、変更後に比較します（check）。

```powershell
powershell -ExecutionPolicy Bypass -File Tools\Debug\golden.ps1 -Mode capture   # 基準画像を Golden\<Configuration>\ に保存
powershell -ExecutionPolicy Bypass -File Tools\Debug\golden.ps1 -Mode check     # 基準と比較
```

WSL から呼ぶ場合:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w Tools/Debug/golden.ps1)" -Mode check
```

主なオプションは `-Configuration Debug|Release`、`-Views a,b`（名前で絞り込み）、`-BaselineDir`、`-OutDir`、
`-BootTimeoutSeconds`、`-KeepRunning` です。PBRApp が既に動作中だと誤ったアプリを操作するため実行を拒否します。

| 終了コード | 意味 |
| --- | --- |
| 0 | 全ビューが PASS（capture では撮影成功） |
| 1 | FAIL（閾値超過）または基準画像なし（MISSING） |
| 3 | PBRApp が既に起動している |
| 4 | 起動タイムアウトまたはパイプ障害 |

基準画像は GPU・ドライバ・構成（Debug/Release）に依存するため `Tools/Debug/Golden/` は gitignore 対象で、
マシンごと・構成ごとに capture し直します。各ビューは 10 フレーム離して 2 回撮影し（`<name>_repeat.png`）、
同一実行内のノイズも併記します。比較は `imgdiff.ps1` の `-MaxChangedOver8Pct` / `-MaxAbs` ゲートで行い、
閾値は `golden-views.json` の既定値かビューごとの上書き（`maxChangedOver8Pct` / `maxAbs`、負値でゲート無効）です。

同一実行内の再撮影はビット一致しますが、実行をまたぐと IBL 有効時にわずかにずれます。既定の閾値を
「8 を超えて変化したピクセルが 0.5%」としているのはこのためです。

ゴールデン用に追加したデバッグコマンドと環境変数:

| コマンド | 内容 |
| --- | --- |
| `app.fixeddt <seconds\|off>` | 1 フレームの時間を固定値にする。`off` で実時間に戻す |
| `app.ui show\|hide` | ImGui パネルの表示切替。撮影時は隠す |
| `app.frame` | 描画フレーム番号を `OK frame=<n>` で返す。フレーム数での待機に使う |
| `app.ready` | 起動フェーズが Running なら `OK ready=1` |

環境変数 `SASAMI_FIXED_DT=<seconds>` は起動時から固定タイムステップを有効にします（`app.fixeddt` の起動時版）。

## WSL から起動する場合の注意

WSL の interop 経由で起動したプロセスは、**起動元コマンドの終了時にツリーごと落とされます**。
`start` や `Start-Process` では切り離せません。常駐させるには interop のツリーに属さない
形で作ります:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w Tools/Debug/run-wsl.ps1)" -Headless
# PBRApp 以外 / Release:  ... run-wsl.ps1 -App FluidApp -Configuration Release
```

Windows 側から直接叩く場合はこの問題は起きません。

**WSL から起動するときは必ず `run-wsl.ps1` を使ってください（一時的な起動スクリプトを作らない）。**
`Win32_Process.Create` で `cmd /c start "" <exe>` を表示状態のまま起動すると、`cmd` とアプリのコンソールが
既定のターミナル (Windows Terminal) に 2 つ開き、アプリ終了後も空のウィンドウとして残り続けます。
`run-wsl.ps1` はコンソールを非表示で作り、アプリも `start /b` でそのコンソールを共有させるので窓が出ません。
