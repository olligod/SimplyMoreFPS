using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// Reuse the live effect: adding another controller replaces AntiAliasing's singleton.
internal sealed class AntiAliasing
{
    internal const string ControllerName = "AntiAliasing.Rendering.AntiAliasingController";

    private readonly MonoBehaviour controller;
    private readonly Action<RenderTexture, RenderTexture> render;
    private readonly Func<object> getSettings;
    private readonly Func<object, int> readMethod;
    private readonly int ssaa;

    internal AntiAliasing(MonoBehaviour controller)
    {
        if (!ModsConfig.IsActive("remi.antialiasing") || controller.GetType() != Compat.RequireType(ControllerName))
        {
            throw new InvalidOperationException("Unsupported image effect: " + controller.GetType().FullName);
        }

        this.controller = controller;
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

        getSettings = (Func<object>)Delegate.CreateDelegate(typeof(Func<object>), getter);
        readMethod = Compat.Getter<int>(Compat.Field(settingsType, "antiAliasingType", methodType));
        ssaa = Convert.ToInt32(Enum.Parse(methodType, "SSAA"));

        MethodInfo callback = AccessTools.Method(controller.GetType(), "OnRenderImage",
            new[] { typeof(RenderTexture), typeof(RenderTexture) });
        if (callback == null || callback.IsStatic || callback.ReturnType != typeof(void))
        {
            throw new InvalidOperationException("Unsupported AntiAliasing image effect callback.");
        }

        render = (Action<RenderTexture, RenderTexture>)Delegate.CreateDelegate(
            typeof(Action<RenderTexture, RenderTexture>), controller, callback);
    }

    internal bool Matches(MonoBehaviour value) => controller == value;

    internal void Validate()
    {
        object current = getSettings();
        if (current == null)
        {
            throw new InvalidOperationException("AntiAliasing settings are not initialized.");
        }

        if (readMethod(current) == ssaa)
        {
            throw new InvalidOperationException("Map coverage does not support AntiAliasing SSAA camera target replacement.");
        }
    }

    internal void Render(RenderTexture input, RenderTexture output)
    {
        if (!UnityData.IsInMainThread || controller == null || !controller.isActiveAndEnabled)
        {
            throw new InvalidOperationException("The AntiAliasing source effect changed before coverage rendering.");
        }

        Validate();
        render(input, output);
    }
}
