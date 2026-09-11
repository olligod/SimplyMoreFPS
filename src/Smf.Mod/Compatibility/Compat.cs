using System;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// Each mod has its own folder and binds by reflection, without an assembly reference.
// Keep shared binding helpers here; external integrations use ../API.
// Each hook kind binds on its first use, so a broken mod only takes down the hook it feeds:
// camera providers on the first Resolve, world overlays when the raster hooks install,
// geometry (VehicleMapFramework, AsAboveSoBelow) on its first query.
internal static class Compat
{
    internal const BindingFlags AnyStatic = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;

    private static bool worldOverlaysRegistered;

    internal static void RegisterCameraProviders()
    {
        CameraPlus.Register();
        SimpleCameraSetting.Register();
        PerspectiveShift.Register();
        FollowMe.Register();
    }

    internal static void RegisterImageEffects()
    {
        AntiAliasing.Register();
        Nivarian.Register();
    }

    internal static bool InstallScreenMeshHooks(Harmony patches)
    {
        return CameraPlusEdges.Install(patches);
    }

    internal static void RegisterWorldOverlays()
    {
        if (!UnityData.IsInMainThread)
            throw new InvalidOperationException("World overlay binding requires Unity main.");
        if (worldOverlaysRegistered)
            return;

        InteractionBubbles.Register();
        worldOverlaysRegistered = true;
    }

    internal static void RegisterGeometryProviders()
    {
        VehicleMapFramework.RegisterGeometry();
        AsAboveSoBelow.RegisterGeometry();
    }

    internal static Type RequireType(string name)
    {
        return AccessTools.TypeByName(name) ?? throw new TypeLoadException(name);
    }

    internal static FieldInfo Field(Type type, string name, Type expected, bool isStatic = false)
    {
        BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | (isStatic ? BindingFlags.Static : BindingFlags.Instance);
        FieldInfo field = type.GetField(name, flags) ?? throw new MissingFieldException(type.FullName, name);
        if (field.FieldType != expected || field.IsStatic != isStatic)
            throw new InvalidOperationException("Unsupported field shape: " + type.FullName + "." + name);
        return field;
    }

    internal static MethodInfo RequireMethod(Type type, string name, Type result, params Type[] args)
    {
        MethodInfo method = type.GetMethod(name, AnyStatic, null, args, null) ?? throw new MissingMethodException(type.FullName, name);
        if (method.ReturnType != result || method.ContainsGenericParameters)
            throw new InvalidOperationException("Unsupported method shape: " + type.FullName + "." + name);
        return method;
    }

    // Typed getter so reading a setting every frame does not box.
    internal static Func<object, T> Getter<T>(FieldInfo field)
    {
        var method = new DynamicMethod("smf_camera_" + field.Name, typeof(T), new[] { typeof(object) }, typeof(Compat).Module, true);
        ILGenerator il = method.GetILGenerator();

        if (field.IsStatic)
        {
            il.Emit(OpCodes.Ldsfld, field);
        }
        else
        {
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Castclass, field.DeclaringType!);
            il.Emit(OpCodes.Ldfld, field);
        }

        il.Emit(OpCodes.Ret);
        return (Func<object, T>)method.CreateDelegate(typeof(Func<object, T>));
    }
}
