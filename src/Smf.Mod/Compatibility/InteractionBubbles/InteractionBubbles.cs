using System;
using System.Reflection;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// Interaction Bubbles draws speech bubbles in world space from its own static Draw().
internal static class InteractionBubbles
{
    internal const string PackageId = "jaxe.bubbles";

    internal static void Register()
    {
        if (!ModsConfig.IsActive(PackageId))
            return;

        try
        {
            Type type = Compat.RequireType("Bubbles.Core.Bubbler");
            MethodInfo draw = type.GetMethod("Draw", BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly, null, Type.EmptyTypes, null)
                ?? throw new MissingMethodException(type.FullName, "Draw()");
            if (draw.ReturnType != typeof(void) || draw.ContainsGenericParameters)
                throw new InvalidOperationException("Expected public static void Bubbles.Core.Bubbler.Draw().");

            WorldOverlayApi.Register(PackageId, draw);
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("World overlay '" + PackageId + "' failed during binding.", error);
        }
    }
}
