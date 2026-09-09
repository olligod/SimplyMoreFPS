using System;

namespace SimplyMoreFPS.API;

/// <summary>Optional replacements for individual camera scalars; ApplyTo validates the combined result before writing.</summary>
public sealed class CameraScalarOverrides
{
    public double? MinSize { get; }
    public double? MaxSize { get; }
    public double? DollyRateKeys { get; }
    public double? DollyRateScreenEdge { get; }
    public double? SpeedDecay { get; }
    public double? MoveSpeed { get; }
    public double? ZoomSpeed { get; }
    public double? ScrollWheelRate { get; }
    public double? ZoomPreserveFactor { get; }
    public double? DragSensitivity { get; }
    public bool? SmoothZoom { get; }
    public bool? ZoomToMouse { get; }
    public bool? EdgeScroll { get; }

    public CameraScalarOverrides(double? minSize = null, double? maxSize = null, double? dollyRateKeys = null,
        double? dollyRateScreenEdge = null, double? speedDecay = null, double? moveSpeed = null, double? zoomSpeed = null,
        double? scrollWheelRate = null, double? zoomPreserveFactor = null, double? dragSensitivity = null,
        bool? smoothZoom = null, bool? zoomToMouse = null, bool? edgeScroll = null)
    {
        Check(minSize);
        Check(maxSize);
        Check(dollyRateKeys);
        Check(dollyRateScreenEdge);
        Check(speedDecay);
        Check(moveSpeed);
        Check(zoomSpeed);
        Check(scrollWheelRate);
        Check(zoomPreserveFactor);
        Check(dragSensitivity);
        if (minSize <= 0 || maxSize <= 0 || maxSize <= minSize || speedDecay > 1 || zoomPreserveFactor > 1)
            throw new ArgumentOutOfRangeException(nameof(minSize), "Invalid camera scalar override bounds.");

        MinSize = minSize;
        MaxSize = maxSize;
        DollyRateKeys = dollyRateKeys;
        DollyRateScreenEdge = dollyRateScreenEdge;
        SpeedDecay = speedDecay;
        MoveSpeed = moveSpeed;
        ZoomSpeed = zoomSpeed;
        ScrollWheelRate = scrollWheelRate;
        ZoomPreserveFactor = zoomPreserveFactor;
        DragSensitivity = dragSensitivity;
        SmoothZoom = smoothZoom;
        ZoomToMouse = zoomToMouse;
        EdgeScroll = edgeScroll;
    }

    public void ApplyTo(CameraSettings settings)
    {
        if (settings == null) throw new ArgumentNullException(nameof(settings));

        double minSize = MinSize ?? settings.MinSize;
        double maxSize = MaxSize ?? settings.MaxSize;
        double dollyRateKeys = DollyRateKeys ?? settings.DollyRateKeys;
        double dollyRateScreenEdge = DollyRateScreenEdge ?? settings.DollyRateScreenEdge;
        double speedDecay = SpeedDecay ?? settings.SpeedDecay;
        double moveSpeed = MoveSpeed ?? settings.MoveSpeed;
        double zoomSpeed = ZoomSpeed ?? settings.ZoomSpeed;
        double scrollWheelRate = ScrollWheelRate ?? settings.ScrollWheelRate;
        double zoomPreserveFactor = ZoomPreserveFactor ?? settings.ZoomPreserveFactor;
        double dragSensitivity = DragSensitivity ?? settings.DragSensitivity;

        Check(minSize);
        Check(maxSize);
        Check(dollyRateKeys);
        Check(dollyRateScreenEdge);
        Check(speedDecay);
        Check(moveSpeed);
        Check(zoomSpeed);
        Check(scrollWheelRate);
        Check(zoomPreserveFactor);
        Check(dragSensitivity);
        if (minSize <= 0 || maxSize <= minSize || speedDecay > 1 || zoomPreserveFactor > 1)
            throw new ArgumentOutOfRangeException(nameof(settings), "Camera scalar overrides produce invalid bounds.");

        settings.MinSize = minSize;
        settings.MaxSize = maxSize;
        settings.DollyRateKeys = dollyRateKeys;
        settings.DollyRateScreenEdge = dollyRateScreenEdge;
        settings.SpeedDecay = speedDecay;
        settings.MoveSpeed = moveSpeed;
        settings.ZoomSpeed = zoomSpeed;
        settings.ScrollWheelRate = scrollWheelRate;
        settings.ZoomPreserveFactor = zoomPreserveFactor;
        settings.DragSensitivity = dragSensitivity;
        settings.SmoothZoom = SmoothZoom ?? settings.SmoothZoom;
        settings.ZoomToMouse = ZoomToMouse ?? settings.ZoomToMouse;
        settings.EdgeScroll = EdgeScroll ?? settings.EdgeScroll;
    }

    private static void Check(double? value)
    {
        if (value.HasValue && (double.IsNaN(value.Value) || double.IsInfinity(value.Value) || value < 0))
            throw new ArgumentOutOfRangeException(nameof(value), "Camera scalar values must be finite and nonnegative.");
    }
}
