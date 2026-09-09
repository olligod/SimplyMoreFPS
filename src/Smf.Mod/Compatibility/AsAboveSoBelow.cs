using System;
using System.Reflection;
using System.Reflection.Emit;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// As Above So Below 2 stacks levels in one map; the camera is clamped to the current band of rows.
internal static class AsAboveSoBelow
{
    internal const string PackageId = "astryl.AsAboveSoBelow2";

    private delegate bool BandBounds(Map map, out float min, out float max);
    private delegate void LevelLimits(int level, out float maximum, out float margin);

    private static bool bound;
    private static BandBounds? bandBounds;
    private static Func<Map, int>? currentLevel;
    private static LevelLimits? levelLimits;
    private static Func<bool>? cameraOn;
    private static Func<bool>? calibrationUnlocked;
    private static Func<bool>? clampZoom;

    private static CameraGeometryPolicy? cachedPolicy;
    private static int cachedWidth;
    private static float cachedMin;
    private static float cachedMax;
    private static float cachedMargin;
    private static double? cachedMaximum;
    private static bool cachedMovement;
    private static bool cachedInput;
    private static bool cachedCoverage;

    internal static CameraGeometryPolicy? Resolve(CameraContext context)
    {
        if (!UnityData.IsInMainThread) throw new InvalidOperationException("Band geometry requires Unity main.");

        try
        {
            if (!bound) Bind();
            if (bandBounds == null || context.Map == null || context.Map != Find.CurrentMap || context.Map.Disposed)
                return null;
            if (!bandBounds(context.Map, out float min, out float max))
                return null;

            // View clipping stays on even with the mod's camera switch off; calibration only releases the movement clamps.
            bool input = cameraOn!();
            bool movement = input && Current.ProgramState == ProgramState.Playing && Scribe.mode == LoadSaveMode.Inactive
                && !calibrationUnlocked!();
            bool coverage = !WorldComponent_GravshipController.GravshipRenderInProgess;
            float margin = 0;
            double? maximum = null;

            if (movement)
            {
                levelLimits!(currentLevel!(context.Map), out float limit, out float rawMargin);
                margin = Math.Max(0f, rawMargin);
                if (clampZoom!()) maximum = limit > 0f ? limit : (max - min) * 0.5;
                if (float.IsNaN(rawMargin) || float.IsInfinity(rawMargin) || float.IsNaN(limit) || float.IsInfinity(limit))
                    throw new InvalidOperationException("Non-finite band camera limits.");
            }

            if (!movement && !input && !coverage) return null;

            int width = context.Map.Size.x;
            bool unchanged = cachedPolicy != null && width == cachedWidth && min == cachedMin && max == cachedMax
                && margin == cachedMargin && maximum == cachedMaximum
                && movement == cachedMovement && input == cachedInput && coverage == cachedCoverage;
            if (unchanged) return cachedPolicy;

            // No gutter on Z: the mod's own CurrentViewRect patch clips to the exact band rows.
            var policy = new CameraGeometryPolicy(
                movementBounds: movement ? new CameraMotionBounds(z: CameraAxisBounds.Framed(min, max, margin, 0.5)) : null,
                maximumSize: maximum,
                reservedWheelModifiers: input ? CameraWheelModifiers.Control : CameraWheelModifiers.None,
                coverageBounds: coverage ? new CameraCoverageBounds(-4, (float)width + 4, min, max) : (CameraCoverageBounds?)null);

            cachedWidth = width;
            cachedMin = min;
            cachedMax = max;
            cachedMargin = margin;
            cachedMaximum = maximum;
            cachedMovement = movement;
            cachedInput = input;
            cachedCoverage = coverage;
            cachedPolicy = policy;

            return policy;
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

        Type view = RequireType("ABBandView");
        Type camera = RequireType("ABCameraBounds");
        Type guard = RequireType("ABGuard");
        Type guardSwitch = RequireType("ABGuardSwitch");
        Type main = RequireType("ABMod");
        Type settings = RequireType("ABSettings");
        Type limitType = camera.GetNestedType("Limits", BindingFlags.Public | BindingFlags.NonPublic)
            ?? throw new TypeLoadException("AsAboveSoBelow.ABCameraBounds.Limits");
        if (!limitType.IsValueType) throw new InvalidOperationException("Expected value-type camera Limits.");

        MethodInfo tryBandBounds = Compat.RequireMethod(view, "TryBandBounds", typeof(bool),
            typeof(Map), typeof(float).MakeByRefType(), typeof(float).MakeByRefType());
        var getBounds = (BandBounds)Delegate.CreateDelegate(typeof(BandBounds), tryBandBounds);
        var getLevel = (Func<Map, int>)Delegate.CreateDelegate(typeof(Func<Map, int>), Compat.RequireMethod(view, "CurrentLevel", typeof(int), typeof(Map)));

        // Copy the fields of the foreign Limits struct into out parameters without boxing it.
        var getLimits = new DynamicMethod("smf_band_limits", typeof(void),
            new[] { typeof(int), typeof(float).MakeByRefType(), typeof(float).MakeByRefType() }, typeof(AsAboveSoBelow).Module, true);
        ILGenerator il = getLimits.GetILGenerator();
        LocalBuilder limits = il.DeclareLocal(limitType);

        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, Compat.RequireMethod(camera, "For", limitType, typeof(int)));
        il.Emit(OpCodes.Stloc, limits);
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Ldloca, limits);
        il.Emit(OpCodes.Ldfld, Compat.Field(limitType, "maxZoom", typeof(float)));
        il.Emit(OpCodes.Stind_R4);
        il.Emit(OpCodes.Ldarg_2);
        il.Emit(OpCodes.Ldloca, limits);
        il.Emit(OpCodes.Ldfld, Compat.Field(limitType, "panMargin", typeof(float)));
        il.Emit(OpCodes.Stind_R4);
        il.Emit(OpCodes.Ret);

        DynamicMethod getGuard = BoolThunk("guard");
        il = getGuard.GetILGenerator();
        il.Emit(OpCodes.Ldsfld, Compat.Field(guard, "Camera", guardSwitch, true));
        il.Emit(OpCodes.Call, Compat.RequireMethod(guard, "On", typeof(bool), guardSwitch));
        il.Emit(OpCodes.Ret);

        DynamicMethod getCalibration = BoolThunk("calibration");
        il = getCalibration.GetILGenerator();
        il.Emit(OpCodes.Ldsfld, Compat.Field(camera, "CalibrationUnlocked", typeof(bool), true));
        il.Emit(OpCodes.Ret);

        // Settings may still be null early on; treat that as clamp enabled.
        DynamicMethod getClamp = BoolThunk("zoom");
        il = getClamp.GetILGenerator();
        Label present = il.DefineLabel();

        il.Emit(OpCodes.Call, Compat.RequireMethod(main, "get_Settings", settings));
        il.Emit(OpCodes.Dup);
        il.Emit(OpCodes.Brtrue_S, present);
        il.Emit(OpCodes.Pop);
        il.Emit(OpCodes.Ldc_I4_1);
        il.Emit(OpCodes.Ret);
        il.MarkLabel(present);
        il.Emit(OpCodes.Ldfld, Compat.Field(settings, "clampZoomToLevel", typeof(bool)));
        il.Emit(OpCodes.Ret);

        currentLevel = getLevel;
        levelLimits = (LevelLimits)getLimits.CreateDelegate(typeof(LevelLimits));
        cameraOn = (Func<bool>)getGuard.CreateDelegate(typeof(Func<bool>));
        calibrationUnlocked = (Func<bool>)getCalibration.CreateDelegate(typeof(Func<bool>));
        clampZoom = (Func<bool>)getClamp.CreateDelegate(typeof(Func<bool>));
        bandBounds = getBounds;
        bound = true;
    }

    private static DynamicMethod BoolThunk(string name)
    {
        return new DynamicMethod("smf_band_" + name, typeof(bool), Type.EmptyTypes, typeof(AsAboveSoBelow).Module, true);
    }

    private static Type RequireType(string name)
    {
        return Compat.RequireType("AsAboveSoBelow." + name);
    }
}
