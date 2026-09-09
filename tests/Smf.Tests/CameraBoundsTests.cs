using System;
using SimplyMoreFPS;
using SimplyMoreFPS.API;

internal static class CameraBoundsTests
{
    internal static int Run()
    {
        NumericBoundsMatchFramingRules();
        ConstraintsFollowZoomAndVanillaClamp();
        ScriptedPansAndCompletionShareConstraints();
        InvalidNumbersCannotEnterBounds();
        OptionalBoundsPreserveBehaviorAndAllocateNothing();
        MaximumSizeConstrainsPanWithoutEndingItsTimeline();

        return 6;
    }

    private static CameraInput Input() => new CameraInput
    {
        Width = 1920,
        Height = 1080,
        PointerX = 960,
        PointerY = 540,
        UiScale = 1
    };

    private static void NumericBoundsMatchFramingRules()
    {
        CameraAxisBounds fixedAxis = CameraAxisBounds.Fixed(-40, -10);
        Near(-40, fixedAxis.Clamp(-100, 100), "signed fixed minimum");
        Near(-10, fixedAxis.Clamp(20, 1), "signed fixed maximum");

        CameraAxisBounds frame = CameraAxisBounds.Framed(10, 110, 20, .5);
        Near(5, frame.Clamp(-100, 10), "overscroll limits close lower bound");
        Near(115, frame.Clamp(200, 10), "overscroll limits close upper bound");
        Near(30, frame.Clamp(-100, 40), "frame limits far lower bound");
        Near(90, frame.Clamp(200, 40), "frame limits far upper bound");
        Near(60, frame.Clamp(200, 80), "crossed interval collapses at midpoint");
        Near(60, frame.Clamp(-100, 70), "equal interval remains a point");
        Near(2, CameraAxisBounds.Fixed(2, 2).Clamp(9, 20), "fixed point");
    }

    private static void ConstraintsFollowZoomAndVanillaClamp()
    {
        CameraSettings settings = new CameraSettings
        {
            Bounds = new CameraMotionBounds(x: CameraAxisBounds.Framed(0, 100, 0, 0)),
            Profile = new CameraProfile(projection: CameraCurve.Constant(500))
        };

        CameraMotion camera = new CameraMotion(settings, new CameraPose(98, 80, 30));
        CameraInput input = Input();
        input.WheelDelta = 10;

        CameraPose after = camera.Update(input, 1.0 / 60, 200, 200);
        Near(100 - after.Size, after.X, "bound uses final logical zoom, not projected size");
        Near(80, after.Z, "null Z axis preserves vanilla translation");

        settings.Bounds = new CameraMotionBounds(x: CameraAxisBounds.Fixed(-100, -50));
        after = camera.Update(Input(), 0, 200, 200);
        Near(-50, after.X, "signed extra constraint executes after vanilla map clamp");
    }

    private static void ScriptedPansAndCompletionShareConstraints()
    {
        CameraSettings settings = new CameraSettings { Bounds = new CameraMotionBounds(x: CameraAxisBounds.Framed(0, 100, 0, 0)) };
        CameraMotion camera = new CameraMotion(settings, new CameraPose(50, 50, 30));

        camera.PanTo(1, new CameraPose(300, 90, 60), 1);
        CameraPose halfway = camera.Update(Input(), .5, 400, 400);
        Near(45, halfway.Size, "scripted logical zoom");
        Near(55, halfway.X, "scripted midpoint constrained");
        Near(70, halfway.Z, "unconstrained scripted axis");

        camera.CompletePan();
        Near(50, camera.Pose.X, "completion collapses impossible interval");
        Near(90, camera.Pose.Z, "completion retains target unconstrained axis");
        Near(60, camera.Pose.Size, "completion retains target logical size");
        if (!camera.LastPanCompleted || camera.LastFinishedPanId != 1 || camera.ActivePanId != 0)
            throw new Exception("Bounds changed successful pan completion bookkeeping.");

        camera.PanTo(2, new CameraPose(-200, 80, 20), 0);
        Near(20, camera.Pose.X, "zero duration pan constrained");
        if (!camera.LastPanCompleted || camera.LastFinishedPanId != 2) throw new Exception("Immediate pan not completed.");

        camera.PanTo(3, new CameraPose(50, 80, 20), 1);
        camera.CancelPan();
        if (camera.LastPanCompleted || camera.LastFinishedPanId != 3) throw new Exception("Canceled pan became successful.");
    }

    private static void InvalidNumbersCannotEnterBounds()
    {
        Throws<ArgumentException>(() => CameraAxisBounds.Fixed(double.NaN, 1));
        Throws<ArgumentException>(() => CameraAxisBounds.Fixed(2, 1));
        Throws<ArgumentException>(() => CameraAxisBounds.Framed(0, 1, double.PositiveInfinity, 0));
        Throws<ArgumentException>(() => CameraAxisBounds.Framed(-double.MaxValue, 1, double.MaxValue, 0));

        CameraAxisBounds overflow = new CameraAxisBounds(double.MaxValue, 0, 0, 0, 0, 100, 0, 100, 50);
        Throws<ArithmeticException>(() => overflow.Clamp(0, 2));

        Throws<ArgumentException>(() => CameraAxisBounds.Fixed(0, 1).Clamp(double.NaN, 1));
        Throws<ArgumentException>(() => CameraAxisBounds.Fixed(0, 1).Clamp(0, 0));
        Near(double.MaxValue, CameraAxisBounds.Fixed(double.MaxValue, double.MaxValue).Clamp(0, 1), "large finite midpoint does not overflow");
    }

    private static void OptionalBoundsPreserveBehaviorAndAllocateNothing()
    {
        CameraMotion unbounded = new CameraMotion(new CameraSettings(), new CameraPose(100, 100, 30));
        CameraMotion empty = new CameraMotion(new CameraSettings { Bounds = new CameraMotionBounds() }, new CameraPose(100, 100, 30));

        CameraInput input = Input();
        input.PanX = .4;
        input.WheelDelta = 1;
        for (int i = 0; i < 60; i++)
        {
            CameraPose a = unbounded.Update(input, 1.0 / 60, 400, 400);
            CameraPose b = empty.Update(input, 1.0 / 60, 400, 400);
            Near(a.X, b.X, "empty X compatibility");
            Near(a.Z, b.Z, "empty Z compatibility");
            Near(a.Size, b.Size, "empty zoom compatibility");
        }

        CameraAxisBounds bounds = CameraAxisBounds.Framed(-100, 100, 10, .2);
        for (int i = 0; i < 1000; i++)
        {
            bounds.Clamp(50, 30);
        }

        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int i = 0; i < 10000; i++)
        {
            bounds.Clamp(50, 30);
        }

        if (GC.GetAllocatedBytesForCurrentThread() != before) throw new Exception("Bound evaluation allocated per call.");
    }

    private static void MaximumSizeConstrainsPanWithoutEndingItsTimeline()
    {
        CameraMotionBounds bounds = new CameraMotionBounds(z: CameraAxisBounds.Framed(0, 200, 0, 0), maximumSize: 63);
        CameraMotion camera = new CameraMotion(new CameraSettings { MaxSize = 60, Bounds = bounds }, new CameraPose(50, 199, 24));
        camera.PanTo(1, new CameraPose(100, 199, 100), 4);

        bool cappedBeforeEnd = false;
        int completions = 0;
        long finished = camera.LastFinishedPanId;
        for (int i = 1; i <= 17; i++)
        {
            camera.Update(Input(), .25, 400, 400);

            if (i < 16 && camera.Pose.Size == 63)
            {
                cappedBeforeEnd = true;
                if (camera.ActivePanId != 1) throw new Exception("Zoom cap ended the trajectory early.");
            }
            if (cappedBeforeEnd && i <= 16) Near(63, camera.Pose.Size, "pan zoom remains capped");

            if (camera.LastFinishedPanId != finished)
            {
                completions++;
                finished = camera.LastFinishedPanId;
            }

            if (i <= 16) Near(camera.Pose.Size, camera.DesiredSize, "pan desired zoom follows bounded sample");
            if (i == 16) Near(137, camera.Pose.Z, "axis coefficients evaluate capped logical zoom");
        }

        if (!cappedBeforeEnd || completions != 1 || !camera.LastPanCompleted)
            throw new Exception("Bounded pan must complete once at its original duration.");

        // CompletePan and cap-only bounds keep the same cap.
        camera.Settings.Bounds = new CameraMotionBounds(maximumSize: 63);
        camera.PanTo(2, new CameraPose(150, 150, 100), 4);
        camera.CompletePan();
        Near(63, camera.Pose.Size, "CompletePan cap");
        Near(63, camera.DesiredSize, "CompletePan desired cap");

        camera.Settings.Bounds = null;
        camera.PanTo(3, new CameraPose(150, 150, 100), 0);
        Near(100, camera.Pose.Size, "vanilla Settings.MaxSize alone must not cap scripted pan");

        camera.Settings.Bounds = new CameraMotionBounds(maximumSize: 63);
        camera.Settings.MaxSize = 200;
        camera.Update(Input(), 0, 400, 400);
        Near(63, camera.Pose.Size, "ordinary update cap");
        Near(63, camera.DesiredSize, "ordinary desired cap");

        Throws<ArgumentException>(() => new CameraMotionBounds(maximumSize: 0));
        Throws<ArgumentException>(() => new CameraMotionBounds(maximumSize: double.NaN));
    }

    private static void Near(double expected, double actual, string label)
    {
        if (!double.IsFinite(actual) || Math.Abs(expected - actual) > 1e-10 * Math.Max(1, Math.Abs(expected)))
            throw new Exception(label + ": " + expected + " != " + actual);
    }

    private static void Throws<T>(Action action) where T : Exception
    {
        try
        {
            action();
        }
        catch (T)
        {
            return;
        }

        throw new Exception("Expected " + typeof(T).Name);
    }
}
