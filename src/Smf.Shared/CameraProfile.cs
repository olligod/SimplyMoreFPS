using System;
using System.Collections.Generic;

namespace SimplyMoreFPS.API
{
    public enum CameraCurveMode
    {
        Constant,
        PowerRange,
        Polynomial2,
        Step,
        PiecewiseLinear
    }

    public enum CameraCurveDomain
    {
        CurrentLogicalZoom,
        DesiredLogicalZoom,
        ProjectionHalfHeight
    }

    public readonly struct CameraCurvePoint
    {
        public double X { get; }
        public double Y { get; }

        public CameraCurvePoint(double x, double y)
        {
            X = x;
            Y = y;
        }
    }

    /// <summary>Immutable nonnegative curve. Tables clamp outside their endpoints; analytic ranges may extrapolate.</summary>
    public sealed class CameraCurve
    {
        public const int MaximumPoints = 16;

        public CameraCurveMode Mode { get; }
        public CameraCurveDomain Domain { get; }
        public double InputMin { get; }
        public double InputMax { get; }
        public double A { get; }
        public double B { get; }
        public double C { get; }
        public double Exponent { get; }
        public bool ClampInput { get; }
        public IReadOnlyList<CameraCurvePoint> Points { get; }

        private CameraCurve(CameraCurveMode mode, CameraCurveDomain domain, double min, double max,
            double a, double b, double c, double exponent, CameraCurvePoint[]? points = null, bool clampInput = false)
        {
            if (!Enum.IsDefined(typeof(CameraCurveDomain), domain)) throw new ArgumentOutOfRangeException(nameof(domain));
            RequireFinite(min);
            RequireFinite(max);
            RequireFinite(a);
            RequireFinite(b);
            RequireFinite(c);
            RequireFinite(exponent);

            Mode = mode;
            Domain = domain;
            InputMin = min;
            InputMax = max;
            A = a;
            B = b;
            C = c;
            Exponent = exponent;
            ClampInput = clampInput;
            Points = Array.AsReadOnly(points == null ? Array.Empty<CameraCurvePoint>() : (CameraCurvePoint[])points.Clone());

            if (mode == CameraCurveMode.Constant)
            {
                RequireValue(a);
            }
            else if (mode == CameraCurveMode.PowerRange || mode == CameraCurveMode.Polynomial2)
            {
                if (max <= min || !CameraMotion.Finite(max - min)) throw new ArgumentOutOfRangeException(nameof(max));

                if (mode == CameraCurveMode.PowerRange)
                {
                    RequireValue(a);
                    RequireValue(b);
                    if (exponent <= 0) throw new ArgumentOutOfRangeException(nameof(exponent));
                }
                else
                {
                    // Check both ends and the vertex, which is where a quadratic can dip negative.
                    RequireValue(a);
                    RequireValue(a + b + c);

                    if (c != 0)
                    {
                        double extremum = -(b / c) * 0.5;
                        if (extremum > 0 && extremum < 1) RequireValue(a + b * extremum + c * extremum * extremum);
                    }
                }
            }
            else
            {
                if (Points.Count < 1 || Points.Count > MaximumPoints) throw new ArgumentOutOfRangeException(nameof(points));

                for (int i = 0; i < Points.Count; i++)
                {
                    RequireFinite(Points[i].X);
                    RequireValue(Points[i].Y);
                    if (i > 0 && (Points[i].X <= Points[i - 1].X || !CameraMotion.Finite(Points[i].X - Points[i - 1].X)))
                        throw new ArgumentException("Curve points must have increasing finite input intervals.", nameof(points));
                }
            }
        }

        public static CameraCurve Constant(double value, CameraCurveDomain domain = CameraCurveDomain.CurrentLogicalZoom)
            => new CameraCurve(CameraCurveMode.Constant, domain, 0, 1, value, 0, 0, 1);

        public static CameraCurve PowerRange(double inputMin, double inputMax, double outputMin, double outputMax,
            double exponent = 1, CameraCurveDomain domain = CameraCurveDomain.CurrentLogicalZoom, bool clampInput = false)
            => new CameraCurve(CameraCurveMode.PowerRange, domain, inputMin, inputMax, outputMin, outputMax, 0, exponent, clampInput: clampInput);

        /// <summary>A + B*t + C*t*t, where t is the normalized input, optionally clamped to [0, 1].</summary>
        public static CameraCurve Polynomial2(double inputMin, double inputMax, double a, double b, double c,
            CameraCurveDomain domain = CameraCurveDomain.CurrentLogicalZoom, bool clampInput = false)
            => new CameraCurve(CameraCurveMode.Polynomial2, domain, inputMin, inputMax, a, b, c, 1, clampInput: clampInput);

        /// <summary>Each point starts a bin, including at equality. Inputs below the first point use its value.</summary>
        public static CameraCurve Step(CameraCurvePoint[] points, CameraCurveDomain domain = CameraCurveDomain.CurrentLogicalZoom)
            => Table(CameraCurveMode.Step, points, domain);

        public static CameraCurve PiecewiseLinear(CameraCurvePoint[] points, CameraCurveDomain domain = CameraCurveDomain.CurrentLogicalZoom)
            => Table(CameraCurveMode.PiecewiseLinear, points, domain);

        private static CameraCurve Table(CameraCurveMode mode, CameraCurvePoint[] points, CameraCurveDomain domain)
        {
            if (points == null) throw new ArgumentNullException(nameof(points));
            return new CameraCurve(mode, domain, 0, 1, 0, 0, 0, 1, points);
        }

        public double Evaluate(double input)
        {
            RequireFinite(input);
            if (Mode == CameraCurveMode.Constant) return A;

            double result;
            if (Mode == CameraCurveMode.PowerRange || Mode == CameraCurveMode.Polynomial2)
            {
                double t = (input - InputMin) / (InputMax - InputMin);
                if (ClampInput) t = CameraMotion.Clamp(t, 0, 1);
                result = Mode == CameraCurveMode.PowerRange ? A + (B - A) * Math.Pow(t, Exponent) : A + B * t + C * t * t;
            }
            else
            {
                int i = 0;
                while (i + 1 < Points.Count && input >= Points[i + 1].X)
                {
                    i++;
                }

                result = Points[i].Y;
                if (Mode == CameraCurveMode.PiecewiseLinear && input > Points[i].X && i + 1 < Points.Count)
                {
                    double t = (input - Points[i].X) / (Points[i + 1].X - Points[i].X);
                    result += (Points[i + 1].Y - result) * t;
                }
            }

            RequireValue(result);
            return result;
        }

        internal double Evaluate(double current, double desired, double projected)
        {
            switch (Domain)
            {
                case CameraCurveDomain.CurrentLogicalZoom:
                    return Evaluate(current);
                case CameraCurveDomain.DesiredLogicalZoom:
                    return Evaluate(desired);
                default:
                    return Evaluate(projected);
            }
        }

        private static void RequireFinite(double value)
        {
            if (!CameraMotion.Finite(value)) throw new ArgumentOutOfRangeException(nameof(value), "Curve values must be finite.");
        }

        private static void RequireValue(double value)
        {
            RequireFinite(value);
            if (value < 0) throw new ArgumentOutOfRangeException(nameof(value), "Curve outputs must be nonnegative.");
        }
    }

    /// <summary>Optional curve overrides evaluated by the camera worker without calling back into the game.</summary>
    public sealed class CameraProfile
    {
        public CameraCurve? Projection { get; }
        public CameraCurve? KeyboardRate { get; }
        public CameraCurve? EdgeRate { get; }
        public CameraCurve? MoveSpeed { get; }
        public CameraCurve? ZoomSpeed { get; }
        public bool? ZoomToMouse { get; }
        public bool DisableZoomToMouseWhileShiftHeld { get; }

        public CameraProfile(CameraCurve? projection = null, CameraCurve? keyboardRate = null, CameraCurve? edgeRate = null,
            CameraCurve? moveSpeed = null, CameraCurve? zoomSpeed = null, bool? zoomToMouse = null, bool disableZoomToMouseWhileShiftHeld = false)
        {
            if (projection != null) RequirePositiveProjection(projection);
            Projection = projection;
            KeyboardRate = keyboardRate;
            EdgeRate = edgeRate;
            MoveSpeed = moveSpeed;
            ZoomSpeed = zoomSpeed;
            ZoomToMouse = zoomToMouse;
            DisableZoomToMouseWhileShiftHeld = disableZoomToMouseWhileShiftHeld;
        }

        private static void RequirePositiveProjection(CameraCurve projection)
        {
            if (projection.Domain != CameraCurveDomain.CurrentLogicalZoom)
                throw new ArgumentException("Projection requires current logical zoom.", nameof(projection));
            if (projection.Evaluate(projection.InputMin) <= 0 || projection.Evaluate(projection.InputMax) <= 0)
                throw new ArgumentException("Projection must remain positive.", nameof(projection));

            foreach (CameraCurvePoint point in projection.Points)
            {
                if (point.Y <= 0) throw new ArgumentException("Projection must remain positive.", nameof(projection));
            }

            if (projection.Mode == CameraCurveMode.Polynomial2 && projection.C != 0)
            {
                double t = -(projection.B / projection.C) * 0.5;
                if (t > 0 && t < 1 && projection.Evaluate(projection.InputMin + t * (projection.InputMax - projection.InputMin)) <= 0)
                    throw new ArgumentException("Projection must remain positive.", nameof(projection));
            }
        }
    }
}
