using System;
using System.Reflection;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

internal static class SimpleCameraSetting
{
    internal const string PackageId = "ray1203.simplecamerasetting";

    internal static void Register()
    {
        if (!ModsConfig.IsActive(PackageId))
            return;

        try
        {
            CameraProviders.Register(new Provider(Compat.RequireType("SimpleCameraSetting.SimpleCameraModSetting")));
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("Camera provider '" + PackageId + "' failed during binding.", error);
        }
    }

    private static readonly string[] Suffixes = { "1", "3", "5", "10", "20", "40", "60", "100", "200" };

    internal static Func<object, float>[] BindSpeedFields(Type type)
    {
        bool bracketZoom = false;
        foreach (FieldInfo field in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static))
        {
            bool zoom = field.Name.StartsWith("zoomSpeedScale_", StringComparison.Ordinal);
            bool move = field.Name.StartsWith("moveSpeedScale_", StringComparison.Ordinal);
            if (!zoom && !move)
            {
                continue;
            }

            string suffix = field.Name.Substring(15);
            if (Array.IndexOf(Suffixes, suffix) < 0)
            {
                throw new InvalidOperationException("Unsupported camera speed bracket: " + type.FullName + "." + field.Name);
            }

            bracketZoom |= zoom;
        }

        // Before May 2026 SCS wrote one zoomSpeed into the vanilla config. Keep that base value.
        if (!bracketZoom)
        {
            Compat.Field(type, "zoomSpeed", typeof(float));
        }

        var fields = new Func<object, float>[bracketZoom ? 18 : 9];
        for (int i = 0; i < Suffixes.Length; i++)
        {
            fields[i] = Compat.Getter<float>(Compat.Field(type, "moveSpeedScale_" + Suffixes[i], typeof(float)));

            if (bracketZoom)
            {
                fields[i + 9] = Compat.Getter<float>(Compat.Field(type, "zoomSpeedScale_" + Suffixes[i], typeof(float)));
            }
        }

        return fields;
    }

    // The brackets start at these desired zoom levels, including equality.
    private sealed class Provider : ICameraProvider
    {
        private static readonly double[] Starts = { 0, 1, 3, 5, 10, 20, 40, 60, 100 };

        private readonly Func<object, object> settings;
        private readonly Func<object, float>[] fields;
        private readonly double[] values;
        private readonly double[] previous;
        private CameraPolicy? policy;

        public string Id => PackageId;
        public int Priority => 100;

        internal Provider(Type main)
        {
            Type type = Compat.RequireType("SimpleCameraSetting.ModSetting");
            settings = Compat.Getter<object>(Compat.Field(main, "modSetting", type, true));

            fields = BindSpeedFields(type);
            values = new double[fields.Length];
            previous = new double[fields.Length];
        }

        public CameraPolicy? Resolve(CameraContext context)
        {
            if (context.Driver.config.GetType() != typeof(CameraMapConfig_Normal))
                return null;
            object instance = settings(null!) ?? throw new InvalidOperationException("SimpleCameraSetting settings are not initialized.");

            bool changed = policy == null;
            for (int i = 0; i < fields.Length; i++)
            {
                values[i] = fields[i](instance);
                changed |= values[i] != previous[i];
            }

            if (!changed)
                return policy;

            var move = new CameraCurvePoint[9];
            var zoom = fields.Length == 18 ? new CameraCurvePoint[9] : null;
            for (int i = 0; i < 9; i++)
            {
                move[i] = new CameraCurvePoint(Starts[i], values[i]);

                if (zoom != null)
                {
                    zoom[i] = new CameraCurvePoint(Starts[i], values[i + 9]);
                }
            }

            // SCS already writes its zoom range, smooth zoom and zoom-to-mouse into the vanilla config and Prefs.
            var profile = new CameraProfile(
                moveSpeed: CameraCurve.Step(move, CameraCurveDomain.DesiredLogicalZoom),
                zoomSpeed: zoom == null ? null : CameraCurve.Step(zoom, CameraCurveDomain.DesiredLogicalZoom));
            var next = new CameraPolicy(profile);
            Array.Copy(values, previous, values.Length);
            policy = next;
            return next;
        }
    }
}
