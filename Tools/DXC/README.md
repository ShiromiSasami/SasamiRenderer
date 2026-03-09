Bundled DXC package for Shader Model 6.9 work.

- Source: https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1
- Asset: `dxc_2025_07_14.zip`
- Package version: `v1.8.2505.1`
- Binary version noted by release: `1.8.2505.32`

Expected runtime files:

- `bin/x64/dxc.exe`
- `bin/x64/dxcompiler.dll`
- `bin/x64/dxil.dll`

Project defaults:

- Build-time shader compile prefers `Tools/DXC/bin/x64/dxc.exe`
- Runtime fallback compile prefers `Tools/DXC/bin/x64/dxcompiler.dll`

Overrides:

- `DXC_EXECUTABLE`
- `DXC_DLL`
- `RENDERER_SHADER_MODEL`
