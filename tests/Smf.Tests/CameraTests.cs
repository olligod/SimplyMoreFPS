using System;
using SimplyMoreFPS;

internal static class CameraTests
{
    public static int Run()
    {
        VanillaKeySpeed();
        ZoomMatchesVanilla60Hz();
        ZoomIndependentOfRefreshRate();
        InertiaIndependentOfRefreshRate();
        DragTracksPointer();
        ZoomKeepsPointUnderPointer();
        MotionGateAndBounds();
        EdgeScrollGates();
        ProjectionAndCrop();
        InvalidInputCannotPoisonCamera();
        InvalidConfigurationIsRejected();
        ProjectionRejectsInvalidNumbers();
        ZeroFrictionStopsReleasedMotion();
        ScriptedPanMatchesVanillaCurve();
        ScriptedPanIsIndependentOfRefreshRate();
        ManualInputCancelsPanWithoutJumping();
        PanCommandsCannotReplayAfterCompletion();
        ConfigurationControlsCameraMotion();

        return 18;
    }

    private static CameraInput Input() => new CameraInput
    {
        Width = 1920,
        Height = 1080,
        PointerX = 960,
        PointerY = 540,
        UiScale = 1
    };

    private static CameraMotion Motion() => new CameraMotion(CameraSettings.Vanilla, new CameraPose(125, 125, 24));

    private static void VanillaKeySpeed()
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.PanX = 1;

        camera.Update(input, 1.0 / 60, 250, 250);
        double speed = 50 * ((24.0 - 11) / (60 - 11) * 0.7 + 0.3);
        Near(125 + speed * 2 / 60, camera.Pose.X, "vanilla keyboard displacement");
        Near(speed * 0.85, camera.VelocityX, "vanilla end-of-frame velocity");

        camera = Motion();
        input.FastPan = true;
        camera.Update(input, 1.0 / 60, 250, 250);
        Near(125 + speed * 2.4 * 2 / 60, camera.Pose.X, "shift camera speed");
    }

    private static void ZoomMatchesVanilla60Hz()
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.WheelDelta = -3;

        camera.Update(input, 1.0 / 60, 250, 250);
        double target = 24 - 3 * 0.35 * 2.6 * 24 / 35;
        Near(target, camera.DesiredSize, "wheel target");
        Near(24 + (target - 24) * 0.4, camera.Pose.Size, "vanilla zoom step");
    }

    private static void ZoomIndependentOfRefreshRate()
    {
        double reference = ZoomAtRate(60);
        Near(reference, ZoomAtRate(30), "30 Hz zoom");
        Near(reference, ZoomAtRate(144), "144 Hz zoom");
        Near(reference, ZoomAtRate(360), "360 Hz zoom");
    }

    private static double ZoomAtRate(int hz)
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.ZoomIn = true;

        for (int i = 0; i < hz; i++)
        {
            camera.Update(input, 1.0 / hz, 250, 250);
            input.ZoomIn = false;
        }

        return camera.Pose.Size;
    }

    private static void InertiaIndependentOfRefreshRate()
    {
        double reference = InertiaAtRate(60);
        Near(reference, InertiaAtRate(30), "30 Hz inertial displacement");
        Near(reference, InertiaAtRate(144), "144 Hz inertial displacement");
        Near(reference, InertiaAtRate(360), "360 Hz inertial displacement");
    }

    private static double InertiaAtRate(int hz)
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.PanX = 1;

        for (int i = 0; i < hz; i++)
        {
            camera.Update(input, 1.0 / hz, 250, 250);
        }

        input.PanX = 0;
        // Stop before the velocity threshold, which vanilla snaps to zero.
        for (int i = 0; i < hz / 2; i++)
        {
            camera.Update(input, 1.0 / hz, 250, 250);
        }

        return camera.Pose.X;
    }

    private static void DragTracksPointer()
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.DragX = 100;
        input.DragY = 45;

        camera.Update(input, 1.0 / 144, 250, 250);
        Near(125 - 100 * 48.0 / 1080 * 1.3, camera.Pose.X, "drag horizontal distance");
        Near(125 + 45 * 48.0 / 1080 * 1.3, camera.Pose.Z, "drag vertical direction");

        input.DragX = 0;
        input.DragY = 0;
        input.MiddleReleased = true;
        camera.Update(input, 1.0 / 144, 250, 250);
        Require(camera.VelocityX < 0 && camera.VelocityZ > 0, "drag release momentum");
    }

    private static void ZoomKeepsPointUnderPointer()
    {
        CameraMotion camera = Motion();
        camera.Settings.ZoomToMouse = true;
        CameraInput input = Input();
        input.PointerX = 1500;
        input.PointerY = 250;

        CameraPoint before = CameraProjection.ScreenToWorld(camera.Pose, 1920, 1080, input.PointerX, input.PointerY);
        input.ZoomIn = true;
        camera.Update(input, 1.0 / 144, 250, 250);

        CameraPoint after = CameraProjection.ScreenToWorld(camera.Pose, 1920, 1080, input.PointerX, input.PointerY);
        Near(before.X, after.X, "zoom pointer world x");
        Near(before.Y, after.Y, "zoom pointer world z");
    }

    private static void MotionGateAndBounds()
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.PanX = 1;
        input.ZoomIn = true;
        input.MotionBlocked = true;

        camera.Update(input, 1, 250, 250);
        Near(125, camera.Pose.X, "modal blocks camera");
        Near(24, camera.Pose.Size, "modal blocks new zoom");

        input.MotionBlocked = false;
        input.ZoomIn = false;
        input.DragX = -100000;
        input.DragY = -100000;

        camera.Update(input, 1.0 / 60, 250, 250);
        Near(248, camera.Pose.X, "right map boundary");
        Near(2, camera.Pose.Z, "bottom map boundary");
    }

    private static void EdgeScrollGates()
    {
        CameraMotion camera = Motion();
        CameraInput input = Input();
        input.AllowEdgeScroll = true;
        input.Fullscreen = true;
        input.PointerY = 1080;

        for (int i = 0; i < 16; i++)
        {
            camera.Update(input, 1.0 / 60, 250, 250);
        }
        Near(125, camera.Pose.Z, "bottom edge delay");

        for (int i = 0; i < 3; i++)
        {
            camera.Update(input, 1.0 / 60, 250, 250);
        }
        Require(camera.Pose.Z < 125, "bottom edge activates");

        camera.Reset(new CameraPose(125, 125, 24));
        input.PointerX = 0;
        input.PointerY = 0;
        camera.Update(input, 0.1, 250, 250);
        Near(125, camera.Pose.X, "corner excludes edge scrolling");
        Near(125, camera.Pose.Z, "corner excludes vertical scrolling");
    }

    private static void ProjectionAndCrop()
    {
        CameraPose pose = new CameraPose(125, 125, 24);
        CameraPoint world = CameraProjection.ScreenToWorld(pose, 1920, 1080, 1300, 350);
        CameraPoint screen = CameraProjection.WorldToScreen(pose, 1920, 1080, world.X, world.Y);
        Near(1300, screen.X, "screen-world-screen x");
        Near(350, screen.Y, "screen-world-screen y");

        CameraPose source = CameraProjection.Overscan(pose, 2);
        CameraCrop crop = CameraProjection.CropForView(source, 3840, 2160, pose, 1920, 1080);
        Near(0.25, crop.X, "overscan crop x");
        Near(0.25, crop.Y, "overscan crop y");
        Near(0.5, crop.Width, "overscan crop width");
        Near(0.5, crop.Height, "overscan crop height");
        Require(crop.FullyCovered, "overscan coverage");

        CameraPose panned = new CameraPose(145, 137, 24);
        CameraPoint click = CameraProjection.ReprojectPoint(panned, 1920, 1080, source, 3840, 2160, 1300, 350);
        CameraPoint clickWorld = CameraProjection.ScreenToWorld(source, 3840, 2160, click.X, click.Y);
        CameraPoint expected = CameraProjection.ScreenToWorld(panned, 1920, 1080, 1300, 350);
        Near(expected.X, clickWorld.X, "detached click map x");
        Near(expected.Y, clickWorld.Y, "detached click map z");

        crop = CameraProjection.CropForView(source, 3840, 2160, new CameraPose(225, 125, 24), 1920, 1080);
        Require(!crop.FullyCovered, "capture bounds do not invent pixels");
    }

    private static void InvalidInputCannotPoisonCamera()
    {
        CameraMotion camera = Motion();

        foreach (double invalid in new[] { double.NaN, double.PositiveInfinity, double.NegativeInfinity })
        {
            CameraInput input = Input();
            input.DragX = invalid;
            Throws<ArgumentOutOfRangeException>(() => camera.Update(input, 1.0 / 60, 250, 250), "nonfinite drag");
            input = Input();
            input.PanZ = invalid;
            Throws<ArgumentOutOfRangeException>(() => camera.Update(input, 1.0 / 60, 250, 250), "nonfinite key axis");
            input = Input();
            input.WheelDelta = invalid;
            Throws<ArgumentOutOfRangeException>(() => camera.Update(input, 1.0 / 60, 250, 250), "nonfinite wheel");
            input = Input();
            input.PointerY = invalid;
            Throws<ArgumentOutOfRangeException>(() => camera.Update(input, 1.0 / 60, 250, 250), "nonfinite pointer");
        }

        camera.Update(Input(), 1.0 / 60, 250, 250);
        Near(125, camera.Pose.X, "invalid input leaves camera x intact");
        Near(125, camera.Pose.Z, "invalid input leaves camera z intact");
        Near(24, camera.Pose.Size, "invalid input leaves camera zoom intact");
    }

    private static void InvalidConfigurationIsRejected()
    {
        Action<CameraSettings>[] changes =
        {
            s => s.MoveSpeed = double.NaN,
            s => s.ZoomSpeed = double.PositiveInfinity,
            s => s.DragSensitivity = -1,
            s => s.DollyRateKeys = double.NaN,
            s => s.SpeedDecay = -1,
            s => s.MaxSize = s.MinSize
        };

        foreach (Action<CameraSettings> change in changes)
        {
            CameraMotion camera = Motion();
            change(camera.Settings);
            Throws<InvalidOperationException>(() => camera.Update(Input(), 1.0 / 60, 250, 250), "invalid camera settings");
            Near(125, camera.Pose.X, "rejected settings preserve camera");
        }
    }

    private static void ProjectionRejectsInvalidNumbers()
    {
        CameraPose pose = new CameraPose(125, 125, 24);

        Throws<ArgumentOutOfRangeException>(() => CameraProjection.ScreenToWorld(pose, 1920, 1080, double.NaN, 0), "nonfinite projection input");
        Throws<ArgumentOutOfRangeException>(() => CameraProjection.WorldToScreen(pose, 1920, 0, 125, 125), "zero projection height");
        Throws<ArgumentOutOfRangeException>(() => CameraProjection.Overscan(pose, double.MaxValue), "overscan multiplication overflow");
        Throws<ArithmeticException>(() => CameraProjection.ScreenToWorld(new CameraPose(0, 0, double.MaxValue), 1920, 1080, 10, 10), "projection overflow");
    }

    private static void ZeroFrictionStopsReleasedMotion()
    {
        CameraMotion camera = Motion();
        camera.Settings.SpeedDecay = 0;
        CameraInput input = Input();
        input.PanX = 1;

        camera.Update(input, 1.0 / 144, 250, 250);
        double x = camera.Pose.X;

        input.PanX = 0;
        camera.Update(input, 1.0 / 144, 250, 250);
        Near(x, camera.Pose.X, "zero decay has no released motion");
    }

    private static void ScriptedPanMatchesVanillaCurve()
    {
        CameraMotion camera = Motion();
        camera.PanTo(17, new CameraPose(165, 85, 16), 1);

        camera.Update(Input(), 0.25, 250, 250);
        const double quarterBlend = 0.103515625;
        Near(125 + 40 * quarterBlend, camera.Pose.X, "native smootherstep quarter x");
        Near(125 - 40 * quarterBlend, camera.Pose.Z, "native smootherstep quarter z");
        Near(24 - 8 * quarterBlend, camera.Pose.Size, "native smootherstep quarter zoom");

        camera.Update(Input(), 0.25, 250, 250);
        Near(145, camera.Pose.X, "native smootherstep midpoint");

        camera.Update(Input(), 0.5, 250, 250);
        Near(165, camera.Pose.X, "scripted pan endpoint");
        Require(camera.ActivePanId == 0 && camera.LastFinishedPanId == 17 && camera.LastPanCompleted, "completed pan acknowledgment");
    }

    private static void ScriptedPanIsIndependentOfRefreshRate()
    {
        foreach (int hz in new[] { 30, 60, 144, 360 })
        {
            CameraMotion camera = Motion();
            camera.PanTo(18, new CameraPose(165, 85, 16), 0.5);

            for (int i = 0; i < hz / 2 + 1; i++)
            {
                camera.Update(Input(), 1.0 / hz, 250, 250);
            }

            Near(165, camera.Pose.X, "pan endpoint at " + hz + " Hz");
            Near(85, camera.Pose.Z, "pan z endpoint at " + hz + " Hz");
            Near(16, camera.Pose.Size, "pan zoom endpoint at " + hz + " Hz");
            Require(camera.LastPanCompleted, "pan completes at " + hz + " Hz");
        }
    }

    private static void ManualInputCancelsPanWithoutJumping()
    {
        CameraMotion camera = Motion();
        camera.PanTo(19, new CameraPose(225, 225, 24), 1);
        camera.Update(Input(), 0.5, 250, 250);
        Near(175, camera.Pose.X, "pan before interruption");

        CameraInput input = Input();
        input.PanX = -1;
        camera.Update(input, 1.0 / 60, 250, 250);
        Require(camera.Pose.X < 175 && camera.Pose.X > 174, "manual camera takes over from visible pose");
        Require(camera.ActivePanId == 0 && camera.LastFinishedPanId == 19 && !camera.LastPanCompleted, "cancel acknowledgment");

        camera.PanTo(20, new CameraPose(225, 225, 24), 1);
        camera.Reset(new CameraPose(50, 50, 20));
        Require(camera.LastFinishedPanId == 20 && !camera.LastPanCompleted, "epoch reset cancels prior pan");

        camera.Update(Input(), 1, 250, 250);
        Near(50, camera.Pose.X, "cancelled pan cannot resume after reset");
    }

    private static void PanCommandsCannotReplayAfterCompletion()
    {
        CameraMotion camera = Motion();
        camera.PanTo(21, new CameraPose(150, 150, 20), 0);
        Near(150, camera.Pose.X, "zero duration pan applies immediately");
        Require(camera.LastFinishedPanId == 21 && camera.LastPanCompleted, "zero duration pan acknowledges");

        camera.PanTo(21, new CameraPose(230, 230, 24), 1);
        camera.Update(Input(), 0.5, 250, 250);
        Near(150, camera.Pose.X, "old capture cannot restart acknowledged pan");

        camera.PanTo(23, new CameraPose(200, 200, 24), 1);
        camera.PanTo(22, new CameraPose(50, 50, 24), 1);
        Require(camera.ActivePanId == 23, "older pan cannot supersede a newer command");
    }

    private static void ConfigurationControlsCameraMotion()
    {
        CameraMotion standard = Motion();
        CameraMotion customized = Motion();
        customized.Settings.DollyRateKeys *= 2;
        CameraInput input = Input();
        input.PanX = 1;

        standard.Update(input, 1.0 / 60, 250, 250);
        customized.Update(input, 1.0 / 60, 250, 250);
        Near(2 * (standard.Pose.X - 125), customized.Pose.X - 125, "exported key rate governs movement");

        customized = Motion();
        customized.Settings.SmoothZoom = true;
        customized.Settings.ZoomPreserveFactor = 0.3;
        customized.Settings.ScrollWheelRate = 0.55;
        input = Input();
        input.WheelDelta = -3;

        customized.Update(input, 1.0 / 60, 250, 250);
        double difference = -3 * 0.55 * 2.6 * 24 / 35;
        Near(24 + difference * 0.05, customized.Pose.Size, "custom smooth zoom step");
        Near(24 + difference + difference * 0.05 * 0.3, customized.DesiredSize, "custom zoom preservation");
    }

    private static void Near(double expected, double actual, string name)
    {
        if (!double.IsFinite(actual) || Math.Abs(expected - actual) > 1e-8)
            throw new Exception(name + ": expected " + expected + ", got " + actual);
    }

    private static void Throws<T>(Action action, string name) where T : Exception
    {
        try
        {
            action();
        }
        catch (T)
        {
            return;
        }

        throw new Exception(name + ": expected " + typeof(T).Name);
    }

    private static void Require(bool value, string name)
    {
        if (!value) throw new Exception(name);
    }
}
