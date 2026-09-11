using System;
using System.Collections.Generic;
using HarmonyLib;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal static class MapImageEffects
{
    internal sealed class Definition
    {
        internal readonly Func<MonoBehaviour, IMapImageEffect>? Create;
        internal readonly Func<MonoBehaviour, string?>? Restriction;

        internal Definition(Func<MonoBehaviour, IMapImageEffect>? create,
            Func<MonoBehaviour, string?>? restriction = null)
        {
            Create = create;
            Restriction = restriction;
        }
    }

    private static readonly Definition Unsupported = new Definition(null);
    private static readonly Dictionary<string, Definition> Registered = new Dictionary<string, Definition>();
    private static readonly Dictionary<Type, Definition?> Types = new Dictionary<Type, Definition?>();
    private static readonly List<MonoBehaviour> Components = new List<MonoBehaviour>();
    private static readonly HashSet<Definition> Seen = new HashSet<Definition>();

    static MapImageEffects()
    {
        ColorCorrectionEffect.Register();
        Compatibility.Compat.RegisterImageEffects();
    }

    internal static void Register(string typeName, Func<MonoBehaviour, IMapImageEffect> create,
        Func<MonoBehaviour, string?>? restriction = null)
    {
        Registered.Add(typeName, new Definition(create, restriction));
        Types.Clear();
    }

    internal static Definition? Identify(Type type)
    {
        if (Types.TryGetValue(type, out Definition? definition))
        {
            return definition;
        }

        if (!Registered.TryGetValue(type.FullName, out definition))
        {
            definition = AccessTools.Method(type, "OnRenderImage", new[] { typeof(RenderTexture), typeof(RenderTexture) }) == null
                ? null : Unsupported;
        }

        Types.Add(type, definition);
        return definition;
    }

    internal static bool CanCapture(Camera camera, out string? reason)
    {
        reason = null;
        if (camera == null)
        {
            return false;
        }

        if (camera.targetTexture != null)
        {
            reason = "SMF_CustomCameraTarget".Translate();
            return false;
        }

        camera.GetComponents(Components);
        try
        {
            foreach (MonoBehaviour component in Components)
            {
                if (component == null)
                {
                    continue;
                }

                Definition? definition = Identify(component.GetType());

                // An adapter may need normal rendering even while its image callback is disabled.
                reason = definition?.Restriction?.Invoke(component);
                if (reason != null)
                {
                    return false;
                }

                if (definition == null || !component.isActiveAndEnabled)
                {
                    continue;
                }

                if (definition.Create == null || !Seen.Add(definition))
                {
                    reason = "SMF_CustomImageEffect".Translate(component.GetType().FullName);
                    return false;
                }
            }

            return true;
        }
        finally
        {
            Components.Clear();
            Seen.Clear();
        }
    }
}
