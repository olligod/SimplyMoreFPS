using System;
using HarmonyLib;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal sealed class ColorCorrectionEffect : IMapImageEffect
{
    private static Type? correctionType;
    private static Func<object, bool>? useDepthCorrection;
    private static Func<object, bool>? selectiveCorrection;

    private Material? material;

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

    public void Dispose()
    {
        if (material != null)
        {
            UnityEngine.Object.Destroy(material);
            material = null;
        }
    }
}
