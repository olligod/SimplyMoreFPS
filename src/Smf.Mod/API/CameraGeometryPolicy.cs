using System;

namespace SimplyMoreFPS.API;

[Flags]
public enum CameraWheelModifiers
{
    None = 0,
    Control = 1,
    Alt = 2,
    Shift = 4,
}

/// <summary>Movement bounds, zoom limit and reserved wheel modifiers, composed separately from speed providers.</summary>
public sealed class CameraGeometryPolicy
{
    public CameraMotionBounds? MovementBounds { get; }
    public double? MaximumSize { get; }
    public CameraWheelModifiers ReservedWheelModifiers { get; }
    public CameraCoverageBounds? CoverageBounds { get; }

    public CameraGeometryPolicy(CameraMotionBounds? movementBounds = null, double? maximumSize = null,
        CameraWheelModifiers reservedWheelModifiers = CameraWheelModifiers.None, CameraCoverageBounds? coverageBounds = null)
    {
        if (maximumSize.HasValue && (double.IsNaN(maximumSize.Value) || double.IsInfinity(maximumSize.Value) || maximumSize.Value <= 0))
            throw new ArgumentOutOfRangeException(nameof(maximumSize));
        const CameraWheelModifiers known = CameraWheelModifiers.Control | CameraWheelModifiers.Alt | CameraWheelModifiers.Shift;
        if ((reservedWheelModifiers & ~known) != 0)
            throw new ArgumentOutOfRangeException(nameof(reservedWheelModifiers));
        coverageBounds?.RequireValid();

        // The smaller of the explicit limit and the one carried by the bounds wins.
        double? boundsMaximum = movementBounds?.MaximumSize;
        if (maximumSize.HasValue && boundsMaximum.HasValue)
            MaximumSize = Math.Min(maximumSize.Value, boundsMaximum.Value);
        else
            MaximumSize = maximumSize ?? boundsMaximum;

        if (MaximumSize == boundsMaximum)
            MovementBounds = movementBounds;
        else
            MovementBounds = new CameraMotionBounds(movementBounds?.X, movementBounds?.Z, MaximumSize);

        ReservedWheelModifiers = reservedWheelModifiers;
        CoverageBounds = coverageBounds;
    }

    internal void ApplyTo(CameraSettings settings)
    {
        double maximum = MaximumSize.HasValue ? Math.Min(settings.MaxSize, MaximumSize.Value) : settings.MaxSize;
        if (maximum <= settings.MinSize)
        {
            throw new InvalidOperationException("Camera geometry maximum zoom " + maximum
                + " must exceed the camera provider's minimum zoom " + settings.MinSize + ".");
        }

        settings.MaxSize = maximum;
        settings.Bounds = MovementBounds;
    }
}
