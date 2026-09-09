using System;
using System.Reflection;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

internal static class CameraPlus
{
    internal const string PackageId = "brrainz.cameraplus";

    internal static void Register()
    {
        if (!ModsConfig.IsActive(PackageId)) return;

        try
        {
            CameraProviders.Register(new Provider(Compat.RequireType("CameraPlus.CameraPlusMain")));
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("Camera provider '" + PackageId + "' failed during binding.", error);
        }
    }

    // Rebuilds the profile from Camera+ settings whenever one of them changes.
    private sealed class Provider : ICameraProvider
    {
        private readonly Func<object, object> settings;
        private readonly Func<object, float>[] fields;
        private readonly Func<object, bool> zoomToMouse;
        private readonly double[] values = new double[8];
        private readonly double[] previous = new double[8];
        private CameraPolicy? policy;

        public string Id => PackageId;
        public int Priority => 200;

        internal Provider(Type main)
        {
            Type type = Compat.RequireType("CameraPlus.CameraPlusSettings");
            settings = Compat.Getter<object>(Compat.Field(main, "Settings", type, true));

            FieldInfo[] settingFields =
            {
                Compat.Field(type, "minRootResult", typeof(float), true),
                Compat.Field(type, "maxRootResult", typeof(float), true),
                Compat.Field(type, "exponentiality", typeof(float)),
                Compat.Field(type, "zoomedInDollyPercent", typeof(float)),
                Compat.Field(type, "zoomedOutDollyPercent", typeof(float)),
                Compat.Field(type, "zoomedInScreenEdgeDollyFactor", typeof(float)),
                Compat.Field(type, "zoomedOutScreenEdgeDollyFactor", typeof(float)),
            };

            fields = new Func<object, float>[settingFields.Length];
            for (int i = 0; i < settingFields.Length; i++)
            {
                fields[i] = Compat.Getter<float>(settingFields[i]);
            }

            zoomToMouse = Compat.Getter<bool>(Compat.Field(type, "zoomToMouse", typeof(bool)));
        }

        public CameraPolicy? Resolve(CameraContext context)
        {
            if (context.Driver.config.GetType() != typeof(CameraMapConfig_Normal)) return null;
            object instance = settings(null!) ?? throw new InvalidOperationException("Camera+ settings are not initialized.");

            bool changed = policy == null;
            for (int i = 0; i < fields.Length; i++)
            {
                values[i] = fields[i](instance);
                changed |= values[i] != previous[i];
            }

            values[7] = zoomToMouse(instance) ? 1 : 0;
            changed |= values[7] != previous[7];
            if (!changed) return policy;

            double minRoot = values[0];
            double maxRoot = values[1];
            double exponent = values[2];
            if (exponent < 0 || double.IsNaN(exponent) || double.IsInfinity(exponent))
                throw new ArgumentOutOfRangeException("exponentiality");

            CameraCurve projection = minRoot == maxRoot
                ? CameraCurve.Constant(minRoot)
                : CameraCurve.PowerRange(11, 60, minRoot, maxRoot, exponent == 0 ? 1 : 2 * exponent);

            double keyIn = 25 * values[3];
            double keyOut = 247.5 * values[4];
            double moveIn = values[3];
            double moveOut = 10 * values[4];
            double edgeIn = 30 * values[5];
            double edgeOut = 30 * values[6];
            CameraCurve keys;
            CameraCurve edge;

            if (minRoot == maxRoot)
            {
                // Camera+ LerpDoubleSafe picks the midpoint when its input range is zero.
                keys = CameraCurve.Constant((keyIn + keyOut) / 2);
                edge = CameraCurve.Constant((moveIn + moveOut) / 2 * ((edgeIn + edgeOut) / 2));
            }
            else
            {
                keys = CameraCurve.PowerRange(minRoot, maxRoot, keyIn, keyOut, domain: CameraCurveDomain.ProjectionHalfHeight);
                double moveSpan = moveOut - moveIn;
                double edgeSpan = edgeOut - edgeIn;
                edge = CameraCurve.Polynomial2(minRoot, maxRoot, moveIn * edgeIn, moveIn * edgeSpan + edgeIn * moveSpan, moveSpan * edgeSpan,
                    CameraCurveDomain.ProjectionHalfHeight);
            }

            var profile = new CameraProfile(projection, keys, edge, zoomToMouse: values[7] != 0, disableZoomToMouseWhileShiftHeld: true);
            var next = new CameraPolicy(profile, scalars: new CameraScalarOverrides(speedDecay: 0.85));
            Array.Copy(values, previous, values.Length);
            policy = next;
            return next;
        }
    }
}
