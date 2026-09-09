using System;
using System.IO;
using System.Runtime.InteropServices;
using SimplyMoreFPS;
using SimplyMoreFPS.API;
using Smf.Camera;
using Smf.Camera.TestFixture;

// Writes reference.bin: the poses the kernel must produce, computed by driving CameraMotion
// directly. native_driver.cpp replays the file against the built NativeAOT kernel.
internal static class Reference
{
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Record
    {
        internal uint Operation;
        internal uint Scenario;
        internal KernelInit Init;
        internal KernelSettings Settings;
        internal KernelInput Input;
        internal KernelPose Expected;
    }

    private const uint OpCreate = 1;
    private const uint OpStep = 2;
    private const uint OpConfigure = 3;
    private const uint OpAdopt = 4;
    private const uint OpRelease = 5;

    private static FileStream file = null!;
    private static uint records;
    private static uint scenario;
    private static CameraMotion motion = null!;
    private static KernelSettings settings;
    private static KernelInit init;
    private static KernelInput input;
    private static ulong poseSequence;
    private static ulong inputSequence;
    private static double clock;
    private static int adapterChecks;
    private static int outsideMapPoses;

    public static int Main(string[] args)
    {
        ClockOnlyContract();

        if (args.Length != 1 || !BitConverter.IsLittleEndian)
            throw new ArgumentException("Pass one new little-endian fixture output path.");
        if (Marshal.SizeOf<DesiredPose>() != DesiredPose.ByteSize || Marshal.SizeOf<MainCameraState>() != MainCameraState.ByteSize)
            throw new InvalidOperationException("Adapter fixture layout mismatch.");
        if (Marshal.SizeOf<Record>() != 2312) throw new InvalidOperationException("Fixture record layout mismatch.");

        using (file = new FileStream(args[0], FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            file.Write("SMFCAM02"u8);
            Write(2u);
            Write(0u);

            foreach (int hz in new[] { 30, 60, 144, 360, 500 })
            {
                RunRate(hz);
            }

            Profiles();
            Bounds();

            // Patch the record count into the header now that it is known.
            file.Position = 12;
            Write(records);
        }

        CheckAdapter((double)float.MaxValue, -(double)float.MaxValue, .5, true);
        CheckAdapter(1, 2, 70, true);
        CheckAdapter(double.MaxValue, 2, 24, false);
        CheckAdapter(1, double.NegativeInfinity, 24, false);
        CheckAdapter(double.NaN, 2, 24, false);
        CheckAdapter(1, 2, double.MaxValue, false);
        CheckAdapter(1, 2, double.Epsilon, false);
        CheckAdapter(1, 2, 0, false);
        CheckAdapter(1, 2, -1, false);
        CheckAdapter(1, 2, 24, true, 600);
        CheckAdapter(1, 2, 24, false, 0);
        CheckAdapter(1, 2, 24, false, double.Epsilon);
        CheckAdapter(1, 2, 24, false, double.MaxValue);
        CheckAdapter(1, 2, 24, false, double.NaN);

        using (StreamWriter receipt = new StreamWriter(new FileStream(args[0] + ".json", FileMode.CreateNew)))
        {
            receipt.WriteLine("{\"referenceRecords\":" + records + ",\"adapterChecks\":" + adapterChecks +
                ",\"actualOutsideMapPoses\":" + outsideMapPoses + ",\"passed\":true,\"nativeExecuted\":false}");
        }

        Console.WriteLine("Generated " + records + " reference records; adapter checks " + adapterChecks +
            "; outside-map poses " + outsideMapPoses + ". No native kernel or graphics called.");
        return 0;
    }

    private static void CheckAdapter(double x, double z, double size, bool expected, double? projection = null)
    {
        DesiredPose pose = new DesiredPose { X = x, Z = z, RootSize = size, ProjectionHalfHeight = projection ?? size };
        if (PoseValidation.CanConvertToUnity(pose) != expected)
            throw new InvalidOperationException("Adapter float-conversion predicate mismatch.");
        adapterChecks++;
    }

    private static void Write<T>(T item) where T : unmanaged
    {
        ReadOnlySpan<T> one = MemoryMarshal.CreateReadOnlySpan(ref item, 1);
        file.Write(MemoryMarshal.AsBytes(one));
    }

    private static KernelPose Snapshot(int result = 0) => new KernelPose
    {
        Version = 2,
        Size = KernelPose.Bytes,
        Epoch = init.Epoch,
        MapId = init.MapId,
        PoseSequence = poseSequence,
        InputSequence = inputSequence,
        SettingsRevision = settings.Revision,
        Result = result,
        X = motion.Pose.X,
        Z = motion.Pose.Z,
        RootSize = motion.Pose.Size,
        DesiredSize = motion.DesiredSize,
        VelocityX = motion.VelocityX,
        VelocityZ = motion.VelocityZ,
        ProjectionHalfHeight = motion.ProjectionHalfHeight,
        ActivePanId = (ulong)motion.ActivePanId,
        FinishedPanId = (ulong)motion.LastFinishedPanId,
        PanFlags = motion.LastPanCompleted ? 1u : 0u
    };

    private static KernelPose ReleasedPose() => new KernelPose
    {
        Version = 2,
        Size = KernelPose.Bytes,
        MapId = -1
    };

    private static void Emit(uint operation, KernelPose expected)
    {
        Write(new Record
        {
            Operation = operation,
            Scenario = scenario,
            Init = init,
            Settings = settings,
            Input = input,
            Expected = expected
        });

        records++;
    }

    // Create or adopt: a fresh CameraMotion at the init pose.
    private static void Reset(bool create)
    {
        motion = new CameraMotion(Contract.ToSettings(settings), new CameraPose(init.X, init.Z, init.RootSize));
        clock = init.MonotonicSeconds;
        inputSequence = 0;
        poseSequence = create ? 1 : checked(poseSequence + 1);

        Emit(create ? OpCreate : OpAdopt, Snapshot());
    }

    private static void Step(bool duplicate = false)
    {
        if (duplicate)
        {
            Emit(OpStep, Snapshot(1));
            return;
        }

        CameraPose before = motion.Pose;
        motion.Update(Contract.ToInput(input, settings), input.MonotonicSeconds - clock, settings.MapWidth, settings.MapHeight);
        if (!Contract.FiniteState(motion)) throw new InvalidOperationException("Reference produced invalid motion.");
        CheckAdapter(motion.Pose.X, motion.Pose.Z, motion.Pose.Size, true, motion.ProjectionHalfHeight);
        if (motion.Pose.X < 2 || motion.Pose.Z < 2 || motion.Pose.X > settings.MapWidth - 2 || motion.Pose.Z > settings.MapHeight - 2)
            outsideMapPoses++;

        if (motion.Pose.X != before.X || motion.Pose.Z != before.Z || motion.Pose.Size != before.Size) poseSequence++;
        clock = input.MonotonicSeconds;
        inputSequence = input.Sequence;

        Emit(OpStep, Snapshot());
    }

    private static void Configure()
    {
        motion.Settings = Contract.ToSettings(settings);
        Emit(OpConfigure, Snapshot());
    }

    private static KernelSettings DefaultSettings() => new KernelSettings
    {
        Version = 2,
        Size = KernelSettings.Bytes,
        Revision = 1,
        Flags = 7,
        MapWidth = 250,
        MapHeight = 225,
        PixelWidth = 1920,
        PixelHeight = 1080,
        UiScale = 1.25,
        MinSize = 11,
        MaxSize = 60,
        DollyRateKeys = 45,
        DollyRateScreenEdge = 36,
        SpeedDecay = .85,
        MoveSpeed = 2,
        ZoomSpeed = 2.6,
        ScrollWheelRate = .35,
        ZoomPreserveFactor = 0,
        DragSensitivity = 1.3
    };

    private static KernelInput InputAt(double time) => new KernelInput
    {
        Version = 2,
        Size = KernelInput.Bytes,
        Epoch = init.Epoch,
        Sequence = inputSequence + 1,
        SettingsRevision = settings.Revision,
        MonotonicSeconds = time,
        PointerX = settings.PixelWidth * .7,
        PointerY = settings.PixelHeight * .25,
        InspectPaneHeight = 290
    };

    // Four seconds of mixed input at one step rate, with a settings change halfway through.
    private static void RunRate(int hz)
    {
        scenario = (uint)hz;
        settings = DefaultSettings();

        init = new KernelInit
        {
            Version = 2,
            Size = KernelInit.Bytes,
            Epoch = (ulong)hz + 1,
            MapId = hz,
            X = 125,
            Z = 112.5,
            RootSize = 24,
            MonotonicSeconds = 1000 + hz * 10
        };

        input = default;
        Reset(true);

        double origin = clock;
        for (int n = 1; n <= hz * 4; n++)
        {
            double t = (double)n / hz;
            if (n == hz * 2)
            {
                settings.Revision++;
                settings.PixelWidth = 1280;
                settings.PixelHeight = 720;
                settings.UiScale = 1.5;
                settings.DollyRateKeys = 51;
                settings.DragSensitivity = 1.1;
                Configure();
            }

            input = InputAt(origin + t);
            if (t < .7)
            {
                input.PanX = 1;
                input.PanZ = .5;
            }

            if (t > .4 && t < .6) input.Flags |= (uint)InputFlags.FastPan;
            if (n == hz) input.WheelDelta = -3;
            if (n == hz + hz / 2) input.Flags |= (uint)InputFlags.ZoomInPulse;
            if (n == hz + hz * 3 / 4) input.Flags |= (uint)InputFlags.ZoomOutPulse;

            if (n > hz * 2 && n <= hz * 5 / 2)
            {
                input.DragX = 200.0 / hz;
                input.DragY = 90.0 / hz;
            }

            if (n == hz * 5 / 2 + 1) input.Flags |= (uint)InputFlags.MiddleReleasedPulse;

            if (t > 3 && t < 3.5)
            {
                input.Flags |= (uint)(InputFlags.AllowEdgeScroll | InputFlags.Fullscreen);
                input.PointerX = 1;
                input.PointerY = settings.PixelHeight * .5;
            }

            if (t > 3.5)
            {
                input.Flags |= (uint)InputFlags.MotionBlocked;
                input.PanZ = -1;
                input.DragX = 10;
            }

            Step();
            if (n % Math.Max(1, hz / 3) == 0) Step(true);
        }

        // Adopt a pose on the map edge and zoom with the pointer outside the window.
        init.Epoch++;
        init.MapId++;
        init.X = 2.01;
        init.Z = 2.02;
        init.RootSize = 24;
        init.MonotonicSeconds = clock + 1;
        settings.Revision = 1;
        settings.Flags = (uint)SettingsFlags.ZoomToMouse;
        Reset(false);

        input = InputAt(clock); // a new sequence at an equal clock is valid
        int outsideBefore = outsideMapPoses;
        input.PointerX = settings.PixelWidth + 800;
        input.PointerY = -500;
        input.WheelDelta = 15;
        Step();

        input = InputAt(clock + 1.0 / hz);
        input.PointerX = settings.PixelWidth + 800;
        input.PointerY = -500;
        Step();
        if (outsideMapPoses == outsideBefore)
            throw new InvalidOperationException("Edge-zoom fixture did not produce an outside-map pose.");

        input = InputAt(clock + 1.0 / hz);
        input.Flags = (uint)InputFlags.ZoomInPulse;
        Step();

        Emit(OpRelease, ReleasedPose());
    }

    private static unsafe KernelCurve Pack(CameraCurve curve)
    {
        KernelCurve p = new KernelCurve
        {
            Mode = (uint)curve.Mode,
            Domain = (uint)curve.Domain,
            PointCount = (uint)curve.Points.Count,
            Flags = curve.ClampInput ? 1u : 0u,
            InputMin = curve.InputMin,
            InputMax = curve.InputMax,
            A = curve.A,
            B = curve.B,
            C = curve.C,
            Exponent = curve.Exponent
        };

        for (int i = 0; i < curve.Points.Count; i++)
        {
            p.X[i] = curve.Points[i].X;
            p.Y[i] = curve.Points[i].Y;
        }

        return p;
    }

    // Every curve kind in one profile, zooming across the step and rate table boundaries.
    private static void Profiles()
    {
        scenario = 2000;
        settings = DefaultSettings();
        settings.Flags = (uint)SettingsFlags.ZoomToMouse;
        settings.MinSize = 5;
        settings.MaxSize = 100;

        settings.Profile = new KernelProfilePack
        {
            PresentMask = 31,
            Projection = Pack(CameraCurve.PowerRange(5, 100, 5, 600, 1.4)),
            KeyboardRate = Pack(CameraCurve.Polynomial2(5, 100, 35, 15, 180, CameraCurveDomain.DesiredLogicalZoom)),
            EdgeRate = Pack(CameraCurve.PiecewiseLinear(
                new[] { new CameraCurvePoint(5, 20), new CameraCurvePoint(100, 120), new CameraCurvePoint(600, 200) },
                CameraCurveDomain.ProjectionHalfHeight)),
            MoveSpeed = Pack(CameraCurve.Constant(1.7)),
            ZoomSpeed = Pack(CameraCurve.Step(
                new[] { new CameraCurvePoint(0, 1.5), new CameraCurvePoint(20, 2.6), new CameraCurvePoint(40, 4.5) }))
        };

        init = new KernelInit
        {
            Version = 2,
            Size = KernelInit.Bytes,
            Epoch = 3000,
            MapId = 20,
            X = 125,
            Z = 112.5,
            RootSize = 19.9,
            MonotonicSeconds = 100
        };

        input = default;
        Reset(true);

        bool crossed20 = false;
        bool crossed40 = false;
        for (int n = 1; n <= 240; n++)
        {
            input = InputAt(clock + 1.0 / 120);
            input.WheelDelta = n <= 200 ? 1 : -1;
            input.PanX = n > 200 ? 1 : 0;
            Step();
            crossed20 |= motion.Pose.Size > 20;
            crossed40 |= motion.Pose.Size > 40;
        }

        if (!crossed20 || !crossed40) throw new InvalidOperationException("Profile fixture did not cross rate boundaries.");

        settings.Revision++;
        settings.Flags |= (uint)SettingsFlags.DisableZoomToMouseWhileShiftHeld;
        Configure();

        input = InputAt(clock + .02);
        input.Flags = (uint)InputFlags.FastPan;
        input.WheelDelta = -4;
        Step();

        Emit(OpRelease, ReleasedPose());
    }

    private static void Bounds()
    {
        scenario = 900;
        settings = DefaultSettings();

        settings.Bounds.X = new KernelAxisBounds
        {
            Enabled = 1,
            MinimumB = -20,
            MinimumLimitB = -20,
            MaximumB = 30,
            MaximumLimitB = 30,
            CollapsePosition = 5
        };

        // The Z bound depends on logical zoom, and the projection deliberately differs from the root size.
        settings.Bounds.Z = new KernelAxisBounds
        {
            Enabled = 1,
            MinimumA = 1,
            MinimumLimitB = 2,
            MaximumA = -1,
            MaximumB = 100,
            MaximumLimitB = 98,
            CollapsePosition = 50
        };

        settings.Profile.PresentMask = 1;
        settings.Profile.Projection = new KernelCurve { A = 100, InputMax = 1, Exponent = 1 };

        init = new KernelInit
        {
            Version = 2,
            Size = KernelInit.Bytes,
            Epoch = 901,
            MapId = 900,
            X = 125,
            Z = 112,
            RootSize = 24,
            MonotonicSeconds = 10000
        };

        input = default;
        Reset(true);
        input = InputAt(clock + .01);
        Step();
        if (motion.Pose.X != 30 || motion.Pose.Z != 76) throw new InvalidOperationException("Bounds ABI logical-size decode failed.");

        settings.Revision++;
        settings.Bounds.X = default;
        Configure();

        input = InputAt(clock + .02);
        input.PanX = 1;
        Step();
        if (motion.Pose.X <= 30) throw new InvalidOperationException("Disabled bounds axis remained active.");

        Emit(OpRelease, ReleasedPose());

        void Reject(KernelSettings candidate)
        {
            if (Contract.Valid(candidate) && Contract.TrySettings(candidate, new CameraPose(30, 50, 24), 24, out _))
                throw new InvalidOperationException("Malformed bounds packet was accepted.");
        }

        KernelSettings invalid = settings;
        invalid.Size = 1752;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.X.Reserved = 1;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.X.MinimumB = 1;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.X.Enabled = 2;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.Z.MinimumA = double.NaN;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.Z.MinimumA = double.MaxValue;
        Reject(invalid);
        invalid = settings;
        invalid.Size = 1912;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.MaximumSize = -1;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.MaximumSize = double.NaN;
        Reject(invalid);
        invalid = settings;
        invalid.Bounds.MaximumSize = double.PositiveInfinity;
        Reject(invalid);

        KernelSettings capped = DefaultSettings();
        capped.Bounds.MaximumSize = 20;
        CameraMotion capMotion = new CameraMotion(Contract.ToSettings(capped), new CameraPose(40, 40, 15));
        capMotion.PanTo(1, new CameraPose(100, 100, 80), 2);
        capMotion.Update(new CameraInput { Width = 1280, Height = 720, UiScale = 1 }, 1, 250, 225);
        if (capMotion.Pose.Size != 20) throw new InvalidOperationException("Cap-only bounds did not constrain pan midpoint.");
        capMotion.CompletePan();
        if (capMotion.Pose.Size != 20) throw new InvalidOperationException("Cap-only bounds did not constrain pan completion.");
    }

    private static void ClockOnlyContract()
    {
        KernelInput idle = new KernelInput
        {
            Version = 2,
            Size = KernelInput.Bytes,
            Epoch = 1,
            Sequence = 1,
            SettingsRevision = 1,
            MonotonicSeconds = 100,
            Flags = (uint)InputFlags.ClockOnly
        };

        void Require(KernelInput candidate, bool expected)
        {
            if (Contract.Valid(candidate) != expected) throw new InvalidOperationException("Clock-only input contract mismatch.");
        }

        Require(idle, true);

        KernelInput candidate = idle;
        candidate.Flags |= (uint)InputFlags.MotionBlocked;
        Require(candidate, true);

        foreach (uint flag in new uint[] { 1, 2, 4, 16, 32, 64, 128, 512 })
        {
            candidate = idle;
            candidate.Flags |= flag;
            Require(candidate, false);
        }

        candidate = idle;
        candidate.PanX = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.PanZ = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.DragX = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.DragY = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.WheelDelta = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.PointerX = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.PointerY = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.InspectPaneHeight = 1;
        Require(candidate, false);
        candidate = idle;
        candidate.MonotonicSeconds = double.NaN;
        Require(candidate, false);
        candidate = idle;
        candidate.Trajectory = new KernelTrajectory
        {
            Id = 1,
            Kind = 1,
            StartSeconds = 100,
            DurationSeconds = 1,
            SourceRootSize = 60,
            TargetRootSize = 40
        };
        Require(candidate, false);
        candidate = idle;
        candidate.Trajectory.TargetX = 1;
        Require(candidate, false);

        // MotionBlocked on its own is an ordinary motion packet and keeps its controls.
        candidate = idle;
        candidate.Flags = (uint)InputFlags.MotionBlocked;
        candidate.PanX = 1;
        Require(candidate, true);
    }
}
