using System;
using HarmonyLib;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal sealed class ColorCorrectionEffect : IMapImageEffect, ISceneImageEffect
{
    private static Type? correctionType;
    private static Func<object, bool>? useDepthCorrection;
    private static Func<object, bool>? selectiveCorrection;

    private Material? material;
    private RenderTexture? sceneLut;
    private ulong sceneLutPointer;

    public MonoBehaviour Source { get; }

    private ColorCorrectionEffect(MonoBehaviour source)
    {
        Source = source;
    }

    internal static void Register()
    {
        MapImageEffects.Register("UnityStandardAssets.ImageEffects.ColorCorrectionCurves",
            source => new ColorCorrectionEffect(source), Restriction);
    }

    private static object ReadField(object value, string name) => AccessTools.Field(value.GetType(), name).GetValue(value);

    private static string? Restriction(MonoBehaviour source)
    {
        if (!source.isActiveAndEnabled)
        {
            return null;
        }

        Type type = source.GetType();
        if (type != correctionType)
        {
            useDepthCorrection = Compatibility.Compat.Getter<bool>(
                Compatibility.Compat.Field(type, "useDepthCorrection", typeof(bool)));
            selectiveCorrection = Compatibility.Compat.Getter<bool>(
                Compatibility.Compat.Field(type, "selectiveCc", typeof(bool)));
            correctionType = type;
        }

        return useDepthCorrection!(source) || selectiveCorrection!(source)
            ? "SMF_DepthColorCorrection".Translate().ToString() : null;
    }

    public bool Prepare()
    {
        if ((bool)ReadField(Source, "useDepthCorrection") || (bool)ReadField(Source, "selectiveCc"))
        {
            throw new InvalidOperationException("Map coverage requires the simple native color correction path.");
        }

        var original = ReadField(Source, "ccMaterial") as Material;
        var lut = ReadField(Source, "rgbChannelTex") as Texture;

        if ((bool)ReadField(Source, "updateTexturesOnStartup") || original == null || lut == null)
        {
            return false;
        }

        if (!original.shader.isSupported || original.passCount != 1)
        {
            throw new InvalidOperationException("Unsupported native color correction material.");
        }

        if (material == null)
        {
            material = new Material(original) { hideFlags = HideFlags.HideAndDontSave, name = "SMF map coverage color correction" };
        }
        else
        {
            if (material.shader != original.shader)
            {
                throw new InvalidOperationException("Native color correction shader changed.");
            }

            material.CopyPropertiesFromMaterial(original);
        }

        material.SetTexture("_RgbTex", lut);
        material.SetFloat("_Saturation", (float)ReadField(Source, "saturation"));
        return true;
    }

    public void Validate()
    {
    }

    public void Render(Camera projection, RenderTexture input, RenderTexture output)
    {
        Graphics.Blit(input, output, material);
    }

    public unsafe void DescribeScene(ScenePacketBuffer packet, Camera source, Camera coverage, ulong cacheSerial)
    {
        if (material == null)
        {
            throw new InvalidOperationException("Color correction is not prepared.");
        }

        Texture lut = material.GetTexture("_RgbTex");
        if (lut == null || lut.width != 256 || lut.height != 4)
            throw new InvalidOperationException("Color correction changed its lookup texture layout.");

        if (sceneLut == null)
        {
            sceneLut = new RenderTexture(256, 4, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear)
            {
                name = "SMF scene color lookup",
                hideFlags = HideFlags.HideAndDontSave,
                filterMode = FilterMode.Bilinear,
                wrapMode = TextureWrapMode.Clamp
            };
            if (!sceneLut.Create())
            {
                throw new InvalidOperationException("Color lookup texture creation failed.");
            }

            sceneLutPointer = unchecked((ulong)sceneLut.GetNativeTexturePtr().ToInt64());
        }

        RenderTexture previous = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        try
        {
            GL.sRGBWrite = false;
            Graphics.Blit(lut, sceneLut);
        }
        finally
        {
            RenderTexture.active = previous;
            GL.sRGBWrite = srgb;
        }

        ScenePackets.ImageFlags flags = ScenePackets.ImageFlags.LinearFilter;
        // Lookup rows use the source texture's UVs, not screen coordinates.
        if (!SystemInfo.graphicsUVStartsAtTop)
        {
            flags |= ScenePackets.ImageFlags.FlipY;
        }

        var effect = new ScenePackets.Effect
        {
            Kind = ScenePackets.EffectKind.ColorCorrection,
            FirstImage = packet.AddImage(sceneLutPointer, checked((ulong)Time.frameCount), 256, 4, flags),
            SecondImage = ScenePackets.NoImage
        };
        effect.Parameters[0] = material.GetFloat("_Saturation");
        packet.AddEffect(effect);
    }

    public void Dispose()
    {
        if (sceneLut != null)
        {
            sceneLut.Release();
            UnityEngine.Object.Destroy(sceneLut);
            sceneLut = null;
            sceneLutPointer = 0;
        }
        if (material != null)
        {
            UnityEngine.Object.Destroy(material);
            material = null;
        }
    }
}
