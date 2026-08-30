# Debug Tools

実行中の SasamiRenderer を外部から操作・観測するための CLI 一式です。
アプリ側の受け口は `Source/AppFramework/Debug/RemoteControl/`、名前付きパイプ
`\\.\pipe\SasamiRenderer.Debug` です。**既定では無効**で、`SASAMI_DEBUG_REMOTE=1`
を設定した起動時のみ開きます。

| ファイル | 用途 |
| --- | --- |
| `run-debug.bat` | デバッグパイプを有効にして起動。第1引数に `1` を渡すとヘッドレス（`SASAMI_HEADLESS=1`） |
| `pipe.ps1` | 任意のコマンドを送る汎用クライアント。`;;` 区切りで複数送れる |
| `screenshot.ps1` | バックバッファを PNG で取得。予約とポーリングを内部で隠蔽する |
| `ss.ps1` | **旧方式**。`PrintWindow` によるウィンドウキャプチャ。ウィンドウ枠が入り、重なりや DWM 合成の影響を受けるため `screenshot.ps1` を推奨 |

## 使い方

```powershell
Tools\Debug\run-debug.bat            # 通常起動
Tools\Debug\run-debug.bat 1          # ヘッドレス起動

powershell -ExecutionPolicy Bypass -File Tools\Debug\screenshot.ps1
powershell -ExecutionPolicy Bypass -File Tools\Debug\pipe.ps1 -Script "render.get;;camera.get"
```

`screenshot.ps1` は `-Path` 省略時、`Build/screenshots/shot-<日時>.png` に保存し、
最終行に保存先の絶対パスだけを出力します（進捗は `Write-Host` に流すので、
呼び出し側が標準出力を受け取るとパスだけが得られます）。

## 主なコマンド

`help` で全一覧が出ます。よく使うもの:

| コマンド | 内容 |
| --- | --- |
| `render.get` | 現在の描画設定（GBufferビュー・露出・IBL・AOモード・レンダーパス） |
| `render.gbuffer <index\|name\|list>` | GBuffer デバッグビューの切り替え |
| `render.exposure` / `render.ibl` / `render.ao` / `render.path` | 各設定の取得・変更 |
| `debug.screenshot <path>` / `debug.screenshot.status` | キャプチャの予約とポーリング（`screenshot.ps1` が使う） |
| `camera.get` / `camera.setTarget` / `camera.setYawPitch` | カメラ操作。**`setYawPitch` の単位はラジアン** |
| `scene.save <path>` / `scene.load <path>` | シーンの保存・読み込み |

設定変更系は**要求値ではなく読み戻した値**を返します。内部でクランプ・拒否された場合に
「適用された」と誤報しないためです。

## WSL から起動する場合の注意

WSL の interop 経由で起動したプロセスは、**起動元コマンドの終了時にツリーごと落とされます**。
`start` や `Start-Process` では切り離せません。常駐させるには interop のツリーに属さない
形で作ります:

```bash
powershell.exe -NoProfile -Command "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = 'cmd.exe /c D:\Git\SasamiRenderer\Tools\Debug\run-debug.bat'; CurrentDirectory = 'D:\Git\SasamiRenderer' }"
```

Windows 側から直接叩く場合はこの問題は起きません。
