using System;
using SimplyMoreFPS;
using SimplyMoreFPS.API;

internal static class CameraProfileTests
{
    internal static int Run()
    {
        ProjectionMatchesDocumentedCameraPlusPoints();
        NonlinearZoomPreservesPointerAnchor();
        DesiredZoomCrossesSpeedBinsWithoutConfigurationRefresh();
        TablesCopyInputsAndKeepStrictBoundaries();
        InvalidProfilesAreRejected();
        ProjectionOverridesPhysicalDragAndShiftAnchor();

        return 6;
    }

    private static CameraInput Input() => new CameraInput
    {
        Width = 1920,
        Height = 1080,
        PointerX = 1350,
        PointerY = 450,
        UiScale = 1
    };

    private static void ProjectionMatchesDocumentedCameraPlusPoints()
    {
        // Camera+ defaults: logical 11..60 maps to projected 2..130; its n = .5 is exponent 1 here.
        CameraCurve linear = CameraCurve.PowerRange(11, 60, 2, 130);
        Near(2, linear.Evaluate(11), "Camera+ close");
        Near(66, linear.Evaluate(35.5), "Camera+ default middle");
        Near(130, linear.Evaluate(60), "Camera+ far");

        CameraCurve quadratic = CameraCurve.PowerRange(11, 60, 2, 130, exponent: 2);
        Near(34, quadratic.Evaluate(35.5), "Camera+ nonlinear middle");
        Near(514, quadratic.Evaluate(109), "unclamped bookmark extrapolation");
        Near(130, CameraCurve.PowerRange(11, 60, 2, 130, 2, clampInput: true).Evaluate(109), "explicit clamp");

        // Edge rate is the product of independently interpolated movement and factor endpoints.
        CameraCurve edge = CameraCurve.Polynomial2(2, 130, 30, 300, 270, CameraCurveDomain.ProjectionHalfHeight);
        Near(247.5, edge.Evaluate(66), "independent edge product midpoint");
    }

    private static void NonlinearZoomPreservesPointerAnchor()
    {
        CameraProfile profile = new CameraProfile(projection: CameraCurve.PowerRange(11, 60, 2, 130, 2), zoomToMouse: true);
        CameraMotion camera = new CameraMotion(new CameraSettings { Profile = profile }, new CameraPose(400, 400, 30));
        CameraInput input = Input();
        CameraPoint anchor = PointerWorld(camera, input);

        input.WheelDelta = -2;
        for (int i = 0; i < 120; i++)
        {
            camera.Update(input, 1.0 / 120, 800, 800);
            input.WheelDelta = 0;
            CameraPoint point = PointerWorld(camera, input);
            Near(anchor.X, point.X, "nonlinear pointer anchor X");
            Near(anchor.Y, point.Y, "nonlinear pointer anchor Z");
        }
    }

    private static CameraPoint PointerWorld(CameraMotion camera, CameraInput input)
    {
        CameraPose projected = new CameraPose(camera.Pose.X, camera.Pose.Z, camera.ProjectionHalfHeight);
        return CameraProjection.ScreenToWorld(projected, input.Width, input.Height, input.PointerX, input.PointerY);
    }

    private static void DesiredZoomCrossesSpeedBinsWithoutConfigurationRefresh()
    {
        CameraProfile profile = new CameraProfile(moveSpeed: CameraCurve.Step(
            new[] { new CameraCurvePoint(0, 1.5), new CameraCurvePoint(10, 2) },
            CameraCurveDomain.DesiredLogicalZoom));
        CameraMotion camera = new CameraMotion(new CameraSettings { MinSize = .1, MaxSize = 120, Profile = profile }, new CameraPose(400, 400, 9.9));
        CameraMotion control = new CameraMotion(new CameraSettings { MinSize = .1, MaxSize = 120, MoveSpeed = 1.5 }, new CameraPose(400, 400, 9.9));

        CameraInput input = Input();
        input.PanX = 1;
        input.WheelDelta = 10;

        camera.Update(input, 1.0 / 60, 800, 800);
        control.Update(input, 1.0 / 60, 800, 800);

        input.WheelDelta = 0;
        double previous = camera.Pose.X;
        double controlPrevious = control.Pose.X;
        double after = camera.Update(input, 1.0 / 60, 800, 800).X - previous;
        double controlAfter = control.Update(input, 1.0 / 60, 800, 800).X - controlPrevious;

        // Both cameras share the zoom and the vanilla size multiplier; only the desired-domain rate differs.
        Near(2.0 / 1.5, after / controlAfter, "desired-domain speed refresh");
    }

    private static void TablesCopyInputsAndKeepStrictBoundaries()
    {
        CameraCurvePoint[] points = { new CameraCurvePoint(0, .2), new CameraCurvePoint(1, .4), new CameraCurvePoint(3, .8) };
        CameraCurve curve = CameraCurve.Step(points, CameraCurveDomain.DesiredLogicalZoom);
        points[1] = new CameraCurvePoint(1, 100);

        Near(.2, curve.Evaluate(.999999), "strict below first boundary");
        Near(.4, curve.Evaluate(1), "boundary equality selects new bin and input copy survives mutation");
        Near(.4, curve.Evaluate(2.999999), "strict below second boundary");
        Near(.8, curve.Evaluate(3), "second boundary equality");
        Near(.8, curve.Evaluate(100), "last bin extends");

        CameraCurve piecewise = CameraCurve.PiecewiseLinear(new[] { new CameraCurvePoint(1, 2), new CameraCurvePoint(3, 6) });
        Near(4, piecewise.Evaluate(2), "piecewise interpolation");
    }

    private static void InvalidProfilesAreRejected()
    {
        Reject(() => CameraCurve.Constant(double.NaN));
        Reject(() => CameraCurve.PowerRange(1, 1, 2, 3));
        Reject(() => CameraCurve.Step(new CameraCurvePoint[17]));
        Reject(() => CameraCurve.Constant(1, (CameraCurveDomain)99));
        Reject(() => CameraCurve.Polynomial2(0, 1, 1, -8, 8));
        Reject(() => new CameraProfile(projection: CameraCurve.Constant(0)));
        Reject(() => new CameraProfile(projection: CameraCurve.Constant(1, CameraCurveDomain.DesiredLogicalZoom)));
    }

    private static void ProjectionOverridesPhysicalDragAndShiftAnchor()
    {
        CameraProfile profile = new CameraProfile(projection: CameraCurve.Constant(50), zoomToMouse: true, disableZoomToMouseWhileShiftHeld: true);
        CameraMotion camera = new CameraMotion(new CameraSettings { Profile = profile }, new CameraPose(400, 400, 30));
        CameraInput input = Input();
        input.DragX = 10;

        Near(400 - 10 * 100.0 / 1080 * 1.3, camera.Update(input, 0, 800, 800).X, "drag uses physical projection");

        profile = new CameraProfile(projection: CameraCurve.PowerRange(11, 60, 2, 130, 2), zoomToMouse: true, disableZoomToMouseWhileShiftHeld: true);
        camera = new CameraMotion(new CameraSettings { Profile = profile }, new CameraPose(400, 400, 30));
        input = Input();
        input.FastPan = true;
        input.WheelDelta = -2;
        CameraPose pose = camera.Update(input, 1.0 / 60, 800, 800);

        Near(400, pose.X, "Shift disables pointer anchoring X");
        Near(400, pose.Z, "Shift disables pointer anchoring Z");
    }

    private static void Near(double expected, double actual, string label)
    {
        if (!double.IsFinite(actual) || Math.Abs(expected - actual) > 1e-10 * Math.Max(1, Math.Abs(expected)))
            throw new Exception(label + ": expected " + expected + ", got " + actual);
    }

    private static void Reject(Action action)
    {
        try
        {
            action();
        }
        catch (ArgumentException)
        {
            return;
        }

        throw new Exception("Invalid profile was accepted.");
    }
}
