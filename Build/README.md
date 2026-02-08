Build output root.

- `bin/`: final binaries and libraries
- `obj/`: intermediate files

Platform build symbols:

- `PLATFORM_WINDOWS`
- `PLATFORM_LINUX`
- `PLATFORM_MACOS`
- `PLATFORM_ANDROID`

RHI backend build symbols:

- `RHI_DIRECTX12`
- `RHI_DIRECTX11`
- `RHI_VULKAN`
- `RHI_OPENGL`

Current project setting is `PLATFORM_WINDOWS=1` and `RHI_DIRECTX12=1` (others `0`).
`Renderer` picks the first enabled backend in this order:
`DirectX12 -> Vulkan -> DirectX11 -> OpenGL`.

Core RHI entry points:
- `CreateRHIDevice(GraphicsRuntime)`
- `GetBuildDefaultGraphicsRuntime()`
- `IsGraphicsRuntimeEnabled(GraphicsRuntime)`

Transitional compatibility:
- Legacy graphics macros (`PLATFORM_DX12` etc.) are still accepted and mapped to `RHI_*`.
- Legacy API names (`RHIBackend`, `GraphicsDevice` / `GraphicsBackend`) remain as aliases.
