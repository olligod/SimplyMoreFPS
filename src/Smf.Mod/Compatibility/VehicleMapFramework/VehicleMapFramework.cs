using System;
using System.Reflection;
using System.Reflection.Emit;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// Vehicle Map Framework draws vehicle maps on a fixed 200x200 planet stage instead of the map grid.
internal static class VehicleMapFramework
{
    internal const string PackageId = "oels.vehiclemapframework";

    private static bool bound;
    private static Func<Map, bool>? isVehicleMap;
    private static Func<object, object>? readSettings;
    private static Func<object, bool>? readDrawPlanet;
    private static readonly CameraGeometryPolicy PlanetPolicy = new CameraGeometryPolicy();

    internal static void RegisterGeometry()
    {
        CameraGeometryProviders.Register(PackageId, priority: 200,
            policy: context => HasVehiclePlanet(context.Map) ? PlanetPolicy : null,
            movementExtent: context => HasVehiclePlanet(context.Map)
                ? new CameraMovementExtent(200, 200) : (CameraMovementExtent?)null,
            coverageBounds: context => HasVehiclePlanet(context.Map)
                ? new CameraCoverageBounds(-4, 204, -4, 204) : (CameraCoverageBounds?)null);
    }

    internal static bool HasVehiclePlanet(Map map)
    {
        if (!UnityData.IsInMainThread)
            throw new InvalidOperationException("Camera geometry requires Unity main.");

        try
        {
            if (!bound)
                Bind();
            if (isVehicleMap == null)
                return false;

            object settings = readSettings!(null!) ?? throw new InvalidOperationException("VMF settings are not initialized.");
            return readDrawPlanet!(settings) && isVehicleMap(map);
        }
        catch (Exception error)
        {
            string stage = bound ? "evaluation" : "binding";
            throw new InvalidOperationException("Camera geometry '" + PackageId + "' failed during " + stage + ".", error);
        }
    }

    private static void Bind()
    {
        if (!ModsConfig.IsActive(PackageId))
        {
            bound = true;
            return;
        }

        Type utility = Compat.RequireType("VehicleMapFramework.VehicleMapUtility");
        Type main = Compat.RequireType("VehicleMapFramework.VehicleMapFramework");
        Type settingsType = Compat.RequireType("VehicleMapFramework.VehicleMapSettings");
        var settingsGetter = Compat.Getter<object>(Compat.Field(main, "settings", settingsType, true));
        var planetGetter = Compat.Getter<bool>(Compat.Field(settingsType, "drawPlanet", typeof(bool)));

        MethodInfo? direct = utility.GetMethod("get_IsVehicleMap", Compat.AnyStatic, null, new[] { typeof(Map) }, null);
        Func<Map, bool> predicate;

        if (direct != null)
        {
            if (direct.ReturnType != typeof(bool) || direct.ContainsGenericParameters)
                throw new InvalidOperationException("Expected static bool VehicleMapUtility.get_IsVehicleMap(Map).");
            predicate = (Func<Map, bool>)Delegate.CreateDelegate(typeof(Func<Map, bool>), direct);
        }
        else
        {
            // Older VMF builds have no IsVehicleMap getter; read the cached parent's vehicle field instead.
            Type cache = Compat.RequireType("VehicleMapFramework.VehicleMapParentsComponent");
            Type parent = Compat.RequireType("VehicleMapFramework.MapParent_Vehicle");
            Type vehicle = Compat.RequireType("VehicleMapFramework.VehiclePawnWithMap");

            MethodInfo method = cache.GetMethod("GetCachedVehicle", Compat.AnyStatic, null, new[] { typeof(Map) }, null)
                ?? throw new MissingMethodException(cache.FullName, "GetCachedVehicle(Map)");
            if (method.ReturnType != parent || method.ContainsGenericParameters)
                throw new InvalidOperationException("Unexpected GetCachedVehicle return type.");
            FieldInfo field = Compat.Field(parent, "vehicle", vehicle);

            var thunk = new DynamicMethod("smf_vmf_IsVehicleMap", typeof(bool), new[] { typeof(Map) }, typeof(VehicleMapFramework).Module, true);
            ILGenerator il = thunk.GetILGenerator();
            Label present = il.DefineLabel();

            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Call, method);
            il.Emit(OpCodes.Dup);
            il.Emit(OpCodes.Brtrue_S, present);
            il.Emit(OpCodes.Pop);
            il.Emit(OpCodes.Ldc_I4_0);
            il.Emit(OpCodes.Ret);
            il.MarkLabel(present);
            il.Emit(OpCodes.Ldfld, field);
            il.Emit(OpCodes.Ldnull);
            il.Emit(OpCodes.Cgt_Un);
            il.Emit(OpCodes.Ret);

            predicate = (Func<Map, bool>)thunk.CreateDelegate(typeof(Func<Map, bool>));
        }

        readSettings = settingsGetter;
        readDrawPlanet = planetGetter;
        isVehicleMap = predicate;
        bound = true;
    }
}
