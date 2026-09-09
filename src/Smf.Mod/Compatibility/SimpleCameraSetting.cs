using System;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

internal static class SimpleCameraSetting
{
    internal const string PackageId = "ray1203.simplecamerasetting";

    internal static void Register()
    {
        if (!ModsConfig.IsActive(PackageId)) return;
        try

        {
            CameraProviders.Register(new Provider(Compat.RequireType("SimpleCameraSetting.SimpleCameraModSetting")));
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("Camera provider '" + PackageId + "' failed during binding.", error);
        }
    }

    // SCS keeps one move and one zoom speed per zoom bracket; the brackets start at these zoom levels.
    private sealed class Provider : ICameraProvider
    {
        private static readonly string[] Suffixes = { "1", "3", "5", "10", "20", "40", "60", "100", "200" };
        private static readonly double[] Starts = { 0, 1, 3, 5, 10, 20, 40, 60, 100 };

        private readonly Func<object, object> settings;
        private readonly Func<object, float>[] fields = new Func<object, float>[18];
        private readonly double[] values = new double[18];
        private readonly double[] previous = new double[18];
        private CameraPolicy? policy;

        public string Id => PackageId;
        public int Priority => 100;

        internal Provider(Type main)
        {
            Type type = Compat.RequireType("SimpleCameraSetting.ModSetting");
            settings = Compat.Getter<object>(Compat.Field(main, "modSetting", type, true));

            for (int i = 0; i < 9; i++)
            {
                fields[i] = Compat.Getter<float>(Compat.Field(type, "moveSpeedScale_" + Suffixes[i], typeof(float)));
                fields[i + 9] = Compat.Getter<float>(Compat.Field(type, "zoomSpeedScale_" + Suffixes[i], typeof(float)));
            }
        }

        public CameraPolicy? Resolve(CameraContext context)
        {
            if (context.Driver.config.GetType() != typeof(CameraMapConfig_Normal)) return null;
            object instance = settings(null!) ?? throw new InvalidOperationException("SimpleCameraSetting settings are not initialized.");

            bool changed = policy == null;
            for (int i = 0; i < fields.Length; i++)
            {
                values[i] = fields[i](instance);
                changed |= values[i] != previous[i];
            }

            if (!changed) return policy;

            var move = new CameraCurvePoint[9];
            var zoom = new CameraCurvePoint[9];
            for (int i = 0; i < 9; i++)
            {
                move[i] = new CameraCurvePoint(Starts[i], values[i]);
                zoom[i] = new CameraCurvePoint(Starts[i], values[i + 9]);
            }

            // SCS already writes its zoom range, smooth zoom and zoom-to-mouse into the vanilla config and Prefs.
            var profile = new CameraProfile(
                moveSpeed: CameraCurve.Step(move, CameraCurveDomain.DesiredLogicalZoom),
                zoomSpeed: CameraCurve.Step(zoom, CameraCurveDomain.DesiredLogicalZoom));
            var next = new CameraPolicy(profile);
            Array.Copy(values, previous, values.Length);
            policy = next;
            return next;
        }
    }
}
