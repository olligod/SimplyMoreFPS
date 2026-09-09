# Unity native plugin header provenance

The original three native plugin API headers and project LICENSE were copied byte-for-byte from Unity-Technologies/NativeRenderingPlugin commit `522254181faf188efa8b50c3e3bf6fce720b26e4`, as recorded in the maintainer provenance receipt. The current IUnityGraphicsMetal.h has a local extension: the MTLCommandQueue forward declaration and IUnityGraphicsMetalV2 interface/GUID. It is no longer byte-identical to that upstream file; the original receipt covers the imported baseline only.

Local offline UnityPlayer inspection in `.runtime/mac-first-fault-r12-r2/decoder-findings.md` records the V2 CommitCurrentCommandBuffer and CommandQueue mappings and their ownership/ordering limits. This supports the inspected implementation behavior, not an upstream revision for the added declarations. No separate upstream revision for this local extension is recorded here. Preserve the extension explicitly when reviewing source provenance; do not represent the modified header as an unmodified upstream copy.

Upstream files:

- [IUnityInterface.h](https://raw.githubusercontent.com/Unity-Technologies/NativeRenderingPlugin/522254181faf188efa8b50c3e3bf6fce720b26e4/PluginSource/source/Unity/IUnityInterface.h)
- [IUnityGraphics.h](https://raw.githubusercontent.com/Unity-Technologies/NativeRenderingPlugin/522254181faf188efa8b50c3e3bf6fce720b26e4/PluginSource/source/Unity/IUnityGraphics.h)
- [IUnityGraphicsMetal.h](https://raw.githubusercontent.com/Unity-Technologies/NativeRenderingPlugin/522254181faf188efa8b50c3e3bf6fce720b26e4/PluginSource/source/Unity/IUnityGraphicsMetal.h)
- [Project LICENSE](https://raw.githubusercontent.com/Unity-Technologies/NativeRenderingPlugin/522254181faf188efa8b50c3e3bf6fce720b26e4/LICENSE)

Preserve the upstream distinction: the project LICENSE contains its MIT notice, while each header carries a Unity Companion License notice. Do not relabel the headers as MIT. UNITY-COMPANION-LICENSE.txt preserves the full extracted [official Unity Companion License](https://unity.com/legal/licenses/unity-companion-license), version 1.4 dated 2024-10-29, SHA-256 `f7308b122d6d47b172fdb45fb3017adad41cd4c59fa428ea54cdbca10e8e2ea7`.

The macOS builder preserves the original .NET notices and appends the upstream project LICENSE, header copyright/license comments, and full Companion notice in the release third-party notices file. This README records provenance; it does not replace those notices.
