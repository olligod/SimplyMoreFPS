#nullable disable
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using HarmonyLib;
using SimplyMoreFPS.Rendering.Shaders;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

// Moves the built-in GUI materials onto derived copies of their own shaders that blend alpha separately.
// The compiled programs stay the game's own; only names and blend state differ.
internal sealed class GuiShaderResources
{
    private const string BundleCacheSlot = "SimplyMoreFPS.Rendering.DerivedGuiBundles.v1";
    private const string DerivationOptions = "v1;premult=true;gui=Hidden/SMFSessionAlpha/;copy=Hidden/SMFSessionAlpha/PremultCopy";
    private static readonly PropertyInfo RawRenderQueue = typeof(Material).GetProperty("rawRenderQueue", BindingFlags.Instance | BindingFlags.NonPublic);
    private static GuiShaderResources current;

    private readonly Dictionary<string, Shader> replacements = new Dictionary<string, Shader>(StringComparer.Ordinal);
    private readonly Dictionary<string, Shader> originals = new Dictionary<string, Shader>(StringComparer.Ordinal);
    private readonly Harmony patches;
    private readonly string owner;
    private bool editing;
    private bool restored;

    public Material PremultCopy { get; }

    public GuiShaderResources(string ownerId, string builtin)
    {
        if (current != null && !current.restored) throw new InvalidOperationException("A GUI shader transaction is already installed.");
        if (RawRenderQueue == null) throw new MissingMemberException("Material.rawRenderQueue");

        owner = ownerId;

        AssetBundle bundle = LoadDerivedBundle(builtin, ReadSchema("class-48.json"), ReadSchema("class-142.json"));
        Shader copy = null;

        foreach (Shader shader in bundle.LoadAllAssets<Shader>())
        {
            if (!shader.isSupported || shader.passCount != 1)
            {
                throw new InvalidOperationException("Derived GUI shader is unsupported: " + shader.name);
            }

            if (shader.name == "Hidden/SMFSessionAlpha/PremultCopy")
            {
                copy = shader;
                continue;
            }

            string kind = shader.name.Substring("Hidden/SMFSessionAlpha/".Length);
            if (kind != "GUITexture" && kind != "GUITextureClip" && kind != "GUITextureBlit")
            {
                throw new InvalidOperationException("Unexpected derived GUI shader.");
            }

            replacements.Add("Hidden/Internal-" + kind, shader);
        }

        if (copy == null || replacements.Count != 3) throw new InvalidOperationException("Derived shader set is incomplete.");

        PremultCopy = new Material(copy)
        {
            name = "SMF session premultiplied copy",
            hideFlags = HideFlags.HideAndDontSave
        };

        PremultCopy.SetColor("_Color", Color.white);
        PremultCopy.SetTextureScale("_MainTex", Vector2.one);
        PremultCopy.SetTextureOffset("_MainTex", Vector2.zero);

        patches = new Harmony(owner);
        current = this;

        try
        {
            // GUI creates its shared materials lazily, so touch the getters before scanning for them.
            foreach (string name in new[] { "blendMaterial", "blitMaterial", "roundedRectMaterial", "roundedRectWithColorPerBorderMaterial" })
            {
                PropertyInfo property = AccessTools.Property(typeof(GUI), name);
                if (property != null) property.GetValue(null, null);
            }

            foreach (Material material in Resources.FindObjectsOfTypeAll<Material>())
            {
                Adopt(material);
            }

            foreach (ConstructorInfo constructor in typeof(Material).GetConstructors(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                if (constructor.GetMethodBody() == null) continue;
                patches.Patch(constructor, postfix: new HarmonyMethod(typeof(GuiShaderResources), nameof(MaterialReady)));
            }

            Validate();
        }
        catch
        {
            Restore();
            throw;
        }
    }

    private static byte[] ReadSchema(string name)
    {
        string resource = "SimplyMoreFPS.Rendering.Shaders." + name;

        using (Stream input = typeof(GuiShaderResources).Assembly.GetManifestResourceStream(resource))
        {
            if (input == null) throw new InvalidOperationException("Missing embedded GUI shader schema: " + name);

            using (var output = new MemoryStream())
            {
                input.CopyTo(output);
                return output.ToArray();
            }
        }
    }

    private static AssetBundle LoadDerivedBundle(string builtin, byte[] shaderSchema, byte[] indexSchema)
    {
        byte[] source = File.ReadAllBytes(builtin);
        string key = Digest(Encoding.UTF8.GetBytes(
            DerivationOptions + ";" + Digest(source) + ";" + Digest(shaderSchema) + ";" + Digest(indexSchema)));

        // The cache sits in an AppDomain slot so it survives a reloaded mod assembly; only BCL and Unity types cross it.
        var cache = AppDomain.CurrentDomain.GetData(BundleCacheSlot) as Dictionary<string, AssetBundle>;
        if (cache == null)
        {
            cache = new Dictionary<string, AssetBundle>(StringComparer.Ordinal);
            AppDomain.CurrentDomain.SetData(BundleCacheSlot, cache);
        }

        if (cache.TryGetValue(key, out AssetBundle existing))
        {
            if (existing == null) throw new InvalidOperationException("A retained derived shader bundle was unexpectedly destroyed.");
            return existing;
        }

        // The key in the bundle name keeps different input sets, and any older build's bundle, from colliding.
        BundleResult result = GuiShaderBundleAssembler.Build(source, shaderSchema, indexSchema, new BundleOptions
        {
            IncludePremultipliedCopy = true,
            BundleName = "smf-persistent-session-alpha-" + key.ToLowerInvariant(),
            GuiNamePrefix = "Hidden/SMFSessionAlpha/",
            CopyShaderName = "Hidden/SMFSessionAlpha/PremultCopy"
        });

        AssetBundle bundle = AssetBundle.LoadFromMemory(result.Bytes);
        if (bundle == null) throw new InvalidOperationException("Derived GUI shader bundle failed to load.");

        // Kept even if installing the materials fails afterwards.
        cache.Add(key, bundle);
        return bundle;
    }

    private static string Digest(byte[] bytes)
    {
        using (SHA256 hash = SHA256.Create())
        {
            return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", "");
        }
    }

    private static void MaterialReady(Material __instance)
    {
        if (current != null && !current.restored && !current.editing) current.Adopt(__instance);
    }

    private void Adopt(Material material)
    {
        if (material == null || material.shader == null) return;
        if (!replacements.TryGetValue(material.shader.name, out Shader replacement)) return;

        originals[replacement.name] = material.shader;
        SetShader(material, replacement);
    }

    // Carries the queue, keywords and GI settings across the shader swap. rawRenderQueue keeps -1 for
    // "follow the shader", which the public renderQueue getter would resolve away.
    private void SetShader(Material material, Shader shader)
    {
        int queue = (int)RawRenderQueue.GetValue(material, null);
        string[] keywords = material.shaderKeywords;
        bool instancing = material.enableInstancing;
        bool doubleSided = material.doubleSidedGI;
        MaterialGlobalIlluminationFlags illumination = material.globalIlluminationFlags;

        // The constructor postfix must not adopt anything while a material is mid-edit.
        editing = true;
        try
        {
            material.shader = shader;
            material.renderQueue = queue;
            material.shaderKeywords = keywords;
            material.enableInstancing = instancing;
            material.doubleSidedGI = doubleSided;
            material.globalIlluminationFlags = illumination;
        }
        finally
        {
            editing = false;
        }
    }

    public void Validate()
    {
        if (restored || PremultCopy == null || PremultCopy.shader == null || !PremultCopy.shader.isSupported)
        {
            throw new InvalidOperationException("The owned GUI alpha/copy transaction is unavailable.");
        }
    }

    public void Restore()
    {
        if (restored) return;

        patches?.UnpatchAll(owner);

        // Scan everything: Unity native code copies materials without going through a managed constructor.
        foreach (Material material in Resources.FindObjectsOfTypeAll<Material>())
        {
            if (material != null && material.shader != null && originals.TryGetValue(material.shader.name, out Shader original))
            {
                SetShader(material, original);
            }
        }

        restored = true;

        // The bundle and PremultCopy stay alive until the process exits.
    }
}
