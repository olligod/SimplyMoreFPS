using System;
using System.Collections.Generic;
using System.IO;
using HarmonyLib;
using SimplyMoreFPS.Rendering;
using SimplyMoreFPS.Rendering.Shaders;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Compatibility;

internal sealed partial class AntiAliasing
{
    private readonly Dictionary<string, CompiledShaderPrograms> scenePrograms = new Dictionary<string, CompiledShaderPrograms>();
    private readonly Dictionary<string, ulong> sceneHandles = new Dictionary<string, ulong>();
    private string? sceneBundlePath;
    private RenderTexture? sceneArea;
    private RenderTexture? sceneSearch;
    private Texture? originalArea;
    private Texture? originalSearch;
    private ulong sceneAreaPointer;
    private ulong sceneSearchPointer;
    private ulong sceneLookupSerial;

    private static object ReadSceneField(object owner, string name) => AccessTools.Field(owner.GetType(), name).GetValue(owner);

    public unsafe void DescribeScene(ScenePacketBuffer packet, Camera source, Camera coverage, ulong cacheSerial)
    {
        object settings = Settings.Get();
        int method = Settings.ReadMethod(settings);
        int cas = Convert.ToInt32(ReadSceneField(settings, "casMode"));
        float sharpness = (float)ReadSceneField(settings, "casSharpness");
        bool aaActive = (method == 1 && (bool)ReadSceneField(controller, "fxaaAvailable")) ||
            (method == 2 && (bool)ReadSceneField(controller, "smaaAvailable"));
        bool casActive = cas != 0 && sharpness > .0001f && (bool)ReadSceneField(controller, "casAvailable");
        if (!aaActive && !casActive)
            return;

        if (!SpaceMapCapture.ReadEffectInput(controller, out FilterMode filter, out RenderTextureFormat format))
            throw new InvalidOperationException("AntiAliasing has no source image for this frame.");
        if (format != RenderTextureFormat.ARGBHalf)
            throw new InvalidOperationException("AntiAliasing changed its scene image precision.");

        if (aaActive)
        {
            var effect = new ScenePackets.Effect
            {
                Kind = ScenePackets.EffectKind.ImageFilter,
                FirstImage = ScenePackets.NoImage,
                SecondImage = ScenePackets.NoImage
            };
            if (method == 1)
            {
                effect.Program = SceneProgram("FXAA", "", 0);
                effect.Parameters[0] = (float)ReadSceneField(settings, "fxaaSubpixelBlending");
                effect.Parameters[1] = (float)ReadSceneField(settings, "fxaaEdgeThreshold");
                effect.Parameters[2] = .0833f;
            }
            else
            {
                int quality = Convert.ToInt32(ReadSceneField(settings, "smaaQuality"));
                string keyword = quality == 0 ? "SMAA_Q_LOW" : quality == 1 ? "SMAA_Q_MEDIUM" : "SMAA_Q_HIGH";
                effect.Program = SceneProgram("SMAA", keyword, 0);
                PrepareSceneLookups();
                ScenePackets.ImageFlags flags = !SystemInfo.graphicsUVStartsAtTop
                    ? ScenePackets.ImageFlags.FlipY : ScenePackets.ImageFlags.None;
                effect.FirstImage = packet.AddImage(sceneAreaPointer, sceneLookupSerial, 160, 560, flags | ScenePackets.ImageFlags.LinearFilter);
                effect.SecondImage = packet.AddImage(sceneSearchPointer, sceneLookupSerial, 64, 16, flags);
                effect.Parameters[3] = (float)ReadSceneField(settings, "smaaEdgeThreshold");
            }
            effect.Parameters[15] = filter == FilterMode.Point ? 0 : 1;
            packet.AddEffect(effect);
        }

        if (casActive)
        {
            var effect = new ScenePackets.Effect
            {
                Kind = ScenePackets.EffectKind.ImageFilter,
                Program = SceneProgram("CAS", "", cas == 2 ? 1 : 0),
                FirstImage = ScenePackets.NoImage,
                SecondImage = ScenePackets.NoImage
            };
            effect.Parameters[4] = sharpness;
            // The AA-to-CAS temporary uses Unity's default bilinear filtering.
            effect.Parameters[15] = aaActive || filter != FilterMode.Point ? 1 : 0;
            packet.AddEffect(effect);
        }
    }

    private ulong SceneProgram(string name, string keyword, int pass)
    {
        string key = name + ":" + keyword + ":" + pass;
        if (sceneHandles.TryGetValue(key, out ulong handle))
            return handle;
        if (sceneBundlePath == null)
            sceneBundlePath = FindSceneBundle();
        if (!scenePrograms.TryGetValue(name, out CompiledShaderPrograms shader))
        {
            var bundle = ReadSceneField(controller, "loadedBundle") as AssetBundle;
            var material = ReadSceneField(controller, name.ToLowerInvariant() + "Material") as Material;
            if (bundle == null || material == null || material.shader != bundle.LoadAsset<Shader>("Assets/Shaders/" + name + ".shader"))
                throw new InvalidOperationException("AntiAliasing's active shader differs from its installed bundle.");
            shader = CompiledShaderPrograms.Load(sceneBundlePath, "Hidden/AntiAliasing/" + name);
            scenePrograms.Add(name, shader);
        }

        var selected = new List<CompiledShaderPrograms.Variant>();
        int count = name == "SMAA" ? 3 : 1;
        for (int i = 0; i < count; ++i)
        {
            int index = count == 1 ? pass : i;
            if (index >= shader.Passes.Length)
                throw new InvalidOperationException("AntiAliasing shader pass is missing.");
            CompiledShaderPrograms.Variant? found = null;
            foreach (CompiledShaderPrograms.Variant variant in shader.Passes[index].Variants)
            {
                if (variant.Keywords.Length != 0 && !(variant.Keywords.Length == 1 && variant.Keywords[0] == keyword))
                    continue;
                if (found != null)
                    throw new InvalidOperationException("AntiAliasing shader variant is ambiguous.");
                found = variant;
            }
            if (found == null)
                throw new InvalidOperationException("AntiAliasing shader variant is missing.");
            selected.Add(found);
        }
        handle = SceneShaderPrograms.Register(sceneBundlePath + ":" + key, selected);
        sceneHandles.Add(key, handle);
        return handle;
    }

    private static string FindSceneBundle()
    {
        string? found = null;
        foreach (ModContentPack mod in LoadedModManager.RunningMods)
        {
            if (mod.PackageIdPlayerFacing != "remi.antialiasing")
                continue;
            foreach (string folder in new[] { "", "Common", "1.4", "1.5", "1.6" })
            {
                string path = Path.Combine(mod.RootDir, folder, "antialiasing");
                if (!File.Exists(path))
                    continue;
                if (found != null)
                    throw new InvalidOperationException("AntiAliasing has multiple installed shader bundles.");
                found = path;
            }
        }
        return found ?? throw new InvalidOperationException("AntiAliasing's installed shader bundle is missing.");
    }

    private void PrepareSceneLookups()
    {
        var area = ReadSceneField(controller, "smaaAreaTex") as Texture;
        var search = ReadSceneField(controller, "smaaSearchTex") as Texture;
        if (area == null || search == null || area.width != 160 || area.height != 560 || search.width != 64 || search.height != 16)
            throw new InvalidOperationException("SMAA changed its lookup texture layout.");
        if (sceneLookupSerial != 0 && area == originalArea && search == originalSearch)
            return;
        if (sceneArea == null)
        {
            sceneArea = NewSceneLookup(160, 560);
            sceneSearch = NewSceneLookup(64, 16);
            sceneAreaPointer = unchecked((ulong)sceneArea.GetNativeTexturePtr().ToInt64());
            sceneSearchPointer = unchecked((ulong)sceneSearch.GetNativeTexturePtr().ToInt64());
        }
        RenderTexture previous = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        try
        {
            GL.sRGBWrite = false;
            Graphics.Blit(area, sceneArea);
            Graphics.Blit(search, sceneSearch);
            sceneLookupSerial = checked((ulong)Time.frameCount);
            originalArea = area;
            originalSearch = search;
        }
        finally
        {
            RenderTexture.active = previous;
            GL.sRGBWrite = srgb;
        }
    }

    private static RenderTexture NewSceneLookup(int width, int height)
    {
        var texture = new RenderTexture(width, height, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear)
        {
            name = "SMF scene antialiasing lookup",
            hideFlags = HideFlags.HideAndDontSave,
            filterMode = FilterMode.Point,
            wrapMode = TextureWrapMode.Clamp
        };
        if (texture.Create())
            return texture;
        UnityEngine.Object.Destroy(texture);
        throw new InvalidOperationException("Antialiasing lookup creation failed.");
    }

    private void ReleaseSceneTextures()
    {
        if (sceneArea != null)
        {
            sceneArea.Release();
            UnityEngine.Object.Destroy(sceneArea);
            sceneArea = null;
        }
        if (sceneSearch != null)
        {
            sceneSearch.Release();
            UnityEngine.Object.Destroy(sceneSearch);
            sceneSearch = null;
        }
        originalArea = originalSearch = null;
        sceneLookupSerial = sceneAreaPointer = sceneSearchPointer = 0;
    }
}
