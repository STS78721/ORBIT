# Changelog

All notable changes to **SteelTemple Orbitals** are documented here.

## [0.1.0] - 2026-11-23 (Early Release)

### Added
- **Initial Windows PC Static Recompilation (`Orbitals 1.0.5`):** Statically lifts and compiles `main`, `subsdk0`, and `sdk` into a native x86-64 `orbitals.exe` with Vulkan 1.3 rendering and Horizon OS runtime.
- **Native UE5 Resolution Scaling (720p–4K):** Full support for Original, 720p, 900p, 1080p, 1440p, and native 4K (`2160p`) internal rendering with per-monitor DPI scaling and AMD FSR 1.0 upscaling.
- **Clean Borderless Fullscreen (`F11` / `F1` Menu):** Fixed fullscreen swapchain sizing and black-screen transitions across 1080p, 1440p, and 4K displays.
- **2-Player Local Co-Op Multi-Controller Support:** Simultaneous P1 + P2 controller support for DualSense, Xbox, Switch / Switch 2 Pro, and SDL-compatible controllers.
- **Built-In Shader Pre-Warmer (`--warm-shaders`):** Pre-compiles recorded shader variants and Vulkan pipelines before launch to eliminate gameplay stutters.
- **High-Throughput Vulkan Renderer Optimisations:** Asynchronous Vulkan command replay, UBO and descriptor pointer caching, integer texture memoisation, incremental dynamic pipeline state diffing, and batched draw recording for significantly higher base FPS in 1080p, 1440p, and 4K.
- **Custom Orbitals Station `F1` Menu & Original Artwork:** Deep-space indigo, sky-cyan, and solar-coral `F1` menu motif, original twin-orbit starburst icon, `OrbitalsLauncher.exe`, and native SDL3/ImGui Setup Wizard + uninstaller.
- **Clean Zero-Config Defaults:** Fresh installs always launch at `1920x1080` windowed with safe hardware-agnostic settings; no copyrighted game files, saves, or developer `.ini` files are ever bundled.
