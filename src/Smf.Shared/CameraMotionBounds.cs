using System;

namespace SimplyMoreFPS.API
{
    /// <summary>A zoom-dependent limit on one signed world axis: min/max are affine in logical size.</summary>
    public sealed class CameraAxisBounds
    {
        public double MinimumA { get; }
        public double MinimumB { get; }
        public double MinimumLimitA { get; }
        public double MinimumLimitB { get; }
        public double MaximumA { get; }
        public double MaximumB { get; }
        public double MaximumLimitA { get; }
        public double MaximumLimitB { get; }
        public double CollapsePosition { get; }

        public CameraAxisBounds(double minimumA, double minimumB, double minimumLimitA, double minimumLimitB,
            double maximumA, double maximumB, double maximumLimitA, double maximumLimitB, double collapsePosition)
        {
            RequireFinite(minimumA);
            RequireFinite(minimumB);
            RequireFinite(minimumLimitA);
            RequireFinite(minimumLimitB);
            RequireFinite(maximumA);
            RequireFinite(maximumB);
            RequireFinite(maximumLimitA);
            RequireFinite(maximumLimitB);
            RequireFinite(collapsePosition);

            MinimumA = minimumA;
            MinimumB = minimumB;
            MinimumLimitA = minimumLimitA;
            MinimumLimitB = minimumLimitB;
            MaximumA = maximumA;
            MaximumB = maximumB;
            MaximumLimitA = maximumLimitA;
            MaximumLimitB = maximumLimitB;
            CollapsePosition = collapsePosition;
        }

        public static CameraAxisBounds Fixed(double min, double max)
        {
            RequireRange(min, max);
            return new CameraAxisBounds(0, min, 0, min, 0, max, 0, max, min * .5 + max * .5);
        }

        /// <summary>Keeps the logical zoom frame inside min/max, with a pan margin and limited overscroll.</summary>
        public static CameraAxisBounds Framed(double min, double max, double panMargin, double overscrollFraction)
        {
            RequireRange(min, max);
            RequireFinite(panMargin);
            RequireFinite(overscrollFraction);

            return new CameraAxisBounds(1, min - panMargin, -overscrollFraction, min,
                -1, max + panMargin, overscrollFraction, max, min * .5 + max * .5);
        }

        public double Clamp(double position, double logicalSize)
        {
            RequireFinite(position);
            RequireFinite(logicalSize);
            if (logicalSize <= 0) throw new ArgumentOutOfRangeException(nameof(logicalSize));

            double lower = Math.Max(Affine(MinimumA, MinimumB, logicalSize), Affine(MinimumLimitA, MinimumLimitB, logicalSize));
            double upper = Math.Min(Affine(MaximumA, MaximumB, logicalSize), Affine(MaximumLimitA, MaximumLimitB, logicalSize));
            return lower > upper ? CollapsePosition : Math.Max(lower, Math.Min(upper, position));
        }

        private static double Affine(double a, double b, double size)
        {
            double product = a * size;
            double result = product + b;
            if (!CameraMotion.Finite(product) || !CameraMotion.Finite(result))
                throw new ArithmeticException("Camera bounds exceed the finite numeric range at this logical zoom.");
            return result;
        }

        private static void RequireRange(double min, double max)
        {
            RequireFinite(min);
            RequireFinite(max);
            if (max < min) throw new ArgumentOutOfRangeException(nameof(max), "Bounds maximum must not precede minimum.");
        }

        private static void RequireFinite(double value)
        {
            if (!CameraMotion.Finite(value)) throw new ArgumentOutOfRangeException(nameof(value), "Camera bounds must be finite.");
        }
    }

    /// <summary>Extra movement limits. A null axis leaves vanilla movement on that axis unchanged.</summary>
    public sealed class CameraMotionBounds
    {
        public CameraAxisBounds? X { get; }
        public CameraAxisBounds? Z { get; }
        public double? MaximumSize { get; }

        public CameraMotionBounds(CameraAxisBounds? x = null, CameraAxisBounds? z = null, double? maximumSize = null)
        {
            if (maximumSize.HasValue && (!CameraMotion.Finite(maximumSize.Value) || maximumSize.Value <= 0))
                throw new ArgumentOutOfRangeException(nameof(maximumSize), "Camera bounds maximum zoom must be finite and positive.");

            X = x;
            Z = z;
            MaximumSize = maximumSize;
        }
    }
}
