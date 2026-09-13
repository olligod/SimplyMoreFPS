using System;
using System.Reflection;
using HarmonyLib;
using SimplyMoreFPS.Rendering;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// Reuse the live effect: adding another controller replaces AntiAliasing's singleton.
internal sealed partial class AntiAliasing : IMapImageEffect, ISceneImageEffect
{
    internal const string ControllerName = "AntiAliasing.Rendering.AntiAliasingController";

    private readonly MonoBehaviour controller;
    private readonly Action<RenderTexture, RenderTexture> render;

    public MonoBehaviour Source => controller;

    // Bind settings only when this mod's controller is encountered.
    private static class Settings
    {
        internal static readonly Func<object> Get;
        internal static readonly Func<object, int> ReadMethod;
        internal static readonly int Ssaa;

        static Settings()
        {
            Type mod = Compat.RequireType("AntiAliasing.AntiAliasingMod");
            Type settingsType = Compat.RequireType("AntiAliasing.AntiAliasingSettings");
            Type methodType = Compat.RequireType("AntiAliasing.AntiAliasingType");
            if (!methodType.IsEnum || Enum.GetUnderlyingType(methodType) != typeof(int))
            {
                throw new InvalidOperationException("Unsupported AntiAliasing method enum.");
            }

            PropertyInfo property = mod.GetProperty("Settings", Compat.AnyStatic)
                ?? throw new InvalidOperationException("AntiAliasing settings property is missing.");
            MethodInfo getter = property.GetGetMethod(true)
                ?? throw new InvalidOperationException("AntiAliasing settings getter is missing.");
            if (property.PropertyType != settingsType)
            {
                throw new InvalidOperationException("Unsupported AntiAliasing settings property.");
            }

            Get = (Func<object>)Delegate.CreateDelegate(typeof(Func<object>), getter);
            ReadMethod = Compat.Getter<int>(Compat.Field(settingsType, "antiAliasingType", methodType));
            Ssaa = Convert.ToInt32(Enum.Parse(methodType, "SSAA"));
        }
    }

    internal static void Register()
    {
        MapImageEffects.Register(ControllerName, source => new AntiAliasing(source), Restriction);
    }

    private static string? Restriction(MonoBehaviour source)
    {
        // SSAA disables OnRenderImage before replacing the camera target.
        return CanCapture ? null : "SMF_AntiAliasingCameraMode".Translate().ToString();
    }

    internal static bool CanCapture
    {
        get
        {
            object settings = Settings.Get();
            return settings != null && Settings.ReadMethod(settings) != Settings.Ssaa;
        }
    }

    internal AntiAliasing(MonoBehaviour controller)
    {
        if (!ModsConfig.IsActive("remi.antialiasing") || controller.GetType() != Compat.RequireType(ControllerName))
        {
            throw new InvalidOperationException("Unsupported image effect: " + controller.GetType().FullName);
        }

        this.controller = controller;

        MethodInfo callback = AccessTools.Method(controller.GetType(), "OnRenderImage",
            new[] { typeof(RenderTexture), typeof(RenderTexture) });
        if (callback == null || callback.IsStatic || callback.ReturnType != typeof(void))
        {
            throw new InvalidOperationException("Unsupported AntiAliasing image effect callback.");
        }

        render = (Action<RenderTexture, RenderTexture>)Delegate.CreateDelegate(
            typeof(Action<RenderTexture, RenderTexture>), controller, callback);
    }

    public bool Prepare()
    {
        Validate();
        return true;
    }

    public void Validate()
    {
        object current = Settings.Get();
        if (current == null)
        {
            throw new InvalidOperationException("AntiAliasing settings are not initialized.");
        }

        if (Settings.ReadMethod(current) == Settings.Ssaa)
        {
            throw new InvalidOperationException("Map coverage does not support AntiAliasing SSAA camera target replacement.");
        }
    }

    public void Render(Camera projection, RenderTexture input, RenderTexture output)
    {
        if (!UnityData.IsInMainThread || controller == null || !controller.isActiveAndEnabled)
        {
            throw new InvalidOperationException("The AntiAliasing source effect changed before coverage rendering.");
        }

        Validate();
        render(input, output);
    }

    public void Dispose()
    {
        ReleaseSceneTextures();
    }
}
