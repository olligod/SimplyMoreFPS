using System;
using System.Collections.Generic;

namespace SimplyMoreFPS
{
    // Camera settings as exported by the running game.
    public sealed class CameraSettings
    {
        public double MinSize = 11;
        public double MaxSize = 60;
        public double DollyRateKeys = 50;
        public double DollyRateScreenEdge = 35;
        public double SpeedDecay = 0.85;
        public double MoveSpeed = 2;
        public double ZoomSpeed = 2.6;
        public double ScrollWheelRate = 0.35;
        public double ZoomPreserveFactor;
        public double DragSensitivity = 1.3;
        public bool SmoothZoom;
        public bool ZoomToMouse;
        public bool EdgeScroll = true;
        public API.CameraProfile? Profile;
        public API.CameraMotionBounds? Bounds;

        public static CameraSettings Vanilla => new CameraSettings();
    }

    public struct CameraPose
    {
        public double X;
        public double Z;
        public double Size;

        public CameraPose(double x, double z, double size)
        {
            X = x;
            Z = z;
            Size = size;
        }
    }

    // Pointer and drag coordinates are physical pixels with the origin at the top left.
    public struct CameraInput
    {
        public double PanX;
        public double PanZ;
        public double DragX;
        public double DragY;
        public double WheelDelta; // Unity GUI scroll delta.y, not SDL wheel notches
        public bool ZoomIn;
        public bool ZoomOut;
        public bool FastPan;
        public bool MotionBlocked;
        public bool MiddleReleased;
        public double PointerX;
        public double PointerY;
        public double Width;
        public double Height;
        public double UiScale;
        public bool AllowEdgeScroll;
        public bool Fullscreen;
        public bool PointerOverUi;
        public double InspectPaneHeight;
    }

    // Orthographic map movement with the constants of vanilla CameraDriver's 60 Hz update.
    // Decay is integrated over elapsed time so a faster presenter does not change the feel.
    public sealed class CameraMotion
    {
        private struct DragSample
        {
            public double Time;
            public double X;
            public double Z;
        }

        private readonly Queue<DragSample> dragSamples = new Queue<DragSample>(32);
        private CameraPose pose;
        private double velocityX;
        private double velocityZ;
        private double elapsed;
        private double bottomEdgeSince = -1;
        private long activePanId;
        private CameraPose panSource;
        private CameraPose panTarget;
        private double panDuration;
        private double panElapsed;

        public CameraSettings Settings { get; set; }
        public CameraPose Pose => pose;
        public double ProjectionHalfHeight => Project(pose.Size);
        public double DesiredSize { get; private set; }
        public double VelocityX => velocityX;
        public double VelocityZ => velocityZ;
        public long ActivePanId => activePanId;
        public long LastFinishedPanId { get; private set; }
        public bool LastPanCompleted { get; private set; }

        public CameraMotion(CameraSettings settings, CameraPose initialPose)
        {
            Settings = settings ?? throw new ArgumentNullException(nameof(settings));
            Reset(initialPose);
        }

        public void Reset(CameraPose value)
        {
            CameraProjection.ValidatePose(value);
            CancelPan();

            pose = value;
            DesiredSize = value.Size;
            velocityX = 0;
            velocityZ = 0;
            elapsed = 0;
            bottomEdgeSince = -1;
            dragSamples.Clear();
        }

        public void PanTo(long commandId, CameraPose destination, double durationSeconds)
        {
            CameraProjection.ValidatePose(destination);
            if (commandId <= 0 || !Finite(durationSeconds) || durationSeconds < 0)
                throw new ArgumentOutOfRangeException(nameof(commandId));
            if (commandId <= LastFinishedPanId || commandId <= activePanId) return;

            CancelPan();
            activePanId = commandId;
            panSource = pose;
            panTarget = destination;
            panDuration = durationSeconds;
            panElapsed = 0;
            velocityX = 0;
            velocityZ = 0;
            DesiredSize = pose.Size;
            dragSamples.Clear();

            if (durationSeconds == 0) AdvancePan(0);
        }

        // Finish at the target, still subject to bounds, without advancing the motion clock.
        public void CompletePan()
        {
            if (activePanId == 0) return;
            panElapsed = panDuration;
            AdvancePan(0);
        }

        public void CancelPan()
        {
            if (activePanId == 0) return;

            LastFinishedPanId = activePanId;
            LastPanCompleted = false;
            activePanId = 0;
            DesiredSize = pose.Size;
            velocityX = 0;
            velocityZ = 0;
        }

        public CameraPose Update(CameraInput input, double deltaSeconds, double mapWidth, double mapHeight)
        {
            CameraProjection.ValidateDimensions(input.Width, input.Height);
            if (!Finite(deltaSeconds) || deltaSeconds < 0)
                throw new ArgumentOutOfRangeException(nameof(deltaSeconds));
            if (!Finite(mapWidth) || !Finite(mapHeight) || mapWidth < 4 || mapHeight < 4)
                throw new ArgumentOutOfRangeException(nameof(mapWidth));
            ValidateSettings();
            ValidateInput(input);

            // Rates sample the state before this input is applied, as the game's OnGUI does.
            API.CameraProfile? profile = Settings.Profile;
            double projectedSize = ProjectionHalfHeight;
            double zoomSpeed = Rate(profile?.ZoomSpeed, Settings.ZoomSpeed, projectedSize);
            double keyboardRate = Rate(profile?.KeyboardRate, Settings.DollyRateKeys, projectedSize);
            double edgeRate = Rate(profile?.EdgeRate, Settings.DollyRateScreenEdge, projectedSize);
            double moveSpeed = Rate(profile?.MoveSpeed, Settings.MoveSpeed, projectedSize);

            if (!Finite(elapsed + deltaSeconds))
                throw new ArgumentOutOfRangeException(nameof(deltaSeconds));

            bool manualInput = input.PanX != 0 || input.PanZ != 0 || input.DragX != 0 || input.DragY != 0 ||
                input.WheelDelta != 0 || input.ZoomIn || input.ZoomOut;
            if (activePanId != 0 && !input.MotionBlocked && manualInput) CancelPan();
            elapsed += deltaSeconds;

            while (dragSamples.Count > 0 && dragSamples.Peek().Time < elapsed - 0.05)
            {
                dragSamples.Dequeue();
            }

            double panX = 0;
            double panZ = 0;

            if (!input.MotionBlocked)
            {
                double zoom = -input.WheelDelta * Settings.ScrollWheelRate;
                if (input.ZoomIn) zoom += 4;
                if (input.ZoomOut) zoom -= 4;
                DesiredSize -= zoom * zoomSpeed * pose.Size / 35;

                panX = Clamp(input.PanX, -1, 1) * keyboardRate;
                panZ = Clamp(input.PanZ, -1, 1) * keyboardRate;
                AddEdgePan(input, edgeRate, ref panX, ref panZ);
                if (input.FastPan)
                {
                    panX *= 2.4;
                    panZ *= 2.4;
                }
            }
            else
            {
                bottomEdgeSince = -1;
            }

            if (activePanId != 0)
            {
                if (panX != 0 || panZ != 0)
                    CancelPan();
                else
                    return AdvancePan(deltaSeconds);
            }

            DesiredSize = ConstrainSize(Clamp(DesiredSize, Settings.MinSize, Settings.MaxSize));

            bool activePan = panX != 0 || panZ != 0;
            if (activePan)
            {
                double speed = (pose.Size - Settings.MinSize) / (Settings.MaxSize - Settings.MinSize) * 0.7 + 0.3;
                velocityX = panX * speed;
                velocityZ = panZ * speed;
            }

            if (input.MiddleReleased && dragSamples.Count > 0)
            {
                double duration = Math.Min(elapsed, 0.05);
                if (duration > 0)
                {
                    foreach (DragSample sample in dragSamples)
                    {
                        velocityX += sample.X / duration * 0.75;
                        velocityZ += sample.Z / duration * 0.75;
                    }
                }

                dragSamples.Clear();
            }

            // Vanilla caps translation at 100 ms across a hitch but lets zoom and friction
            // catch up over at most one second.
            double movementDt = Math.Min(deltaSeconds, 0.1);
            double decaySteps = Math.Min(deltaSeconds, 1) * 60;
            double decay = Math.Pow(Settings.SpeedDecay, decaySteps);

            if (!input.MotionBlocked)
            {
                double movement = activePan ? movementDt : InertiaMovement(movementDt);
                pose.X += velocityX * movement * moveSpeed;
                pose.Z += velocityZ * movement * moveSpeed;

                double cellsPerPixel = 2 * projectedSize / input.Height;
                double dragX = -input.DragX * cellsPerPixel * Settings.DragSensitivity;
                double dragZ = input.DragY * cellsPerPixel * Settings.DragSensitivity;
                pose.X += dragX;
                pose.Z += dragZ;
                if (dragX != 0 || dragZ != 0)
                    dragSamples.Enqueue(new DragSample { Time = elapsed, X = dragX, Z = dragZ });

                pose.X = Clamp(pose.X, 2, mapWidth - 2);
                pose.Z = Clamp(pose.Z, 2, mapHeight - 2);
            }

            // Leave the same released-key velocity as one vanilla 60 Hz held-key step would,
            // whatever the presenter's refresh rate.
            double velocityDecay = activePan ? Settings.SpeedDecay : decay;
            velocityX *= velocityDecay;
            velocityZ *= velocityDecay;

            if (velocityX * velocityX + velocityZ * velocityZ < 0.01)
            {
                velocityX = 0;
                velocityZ = 0;
            }

            double oldSize = pose.Size;
            double zoomStep = Settings.SmoothZoom ? 0.05 : 0.4;
            double preserve = Settings.ZoomPreserveFactor;
            double difference = DesiredSize - oldSize;
            double amount = preserve == 1
                ? difference * zoomStep * decaySteps
                : difference * (1 - Math.Pow(1 - zoomStep * (1 - preserve), decaySteps)) / (1 - preserve);

            double nextSize = ConstrainSize(pose.Size + amount);
            double nextProjectedSize = Project(nextSize);
            DesiredSize = ConstrainSize(DesiredSize + preserve * amount);
            pose.Size = nextSize;

            bool zoomToMouse = profile?.ZoomToMouse ?? Settings.ZoomToMouse;
            if (profile?.DisableZoomToMouseWhileShiftHeld == true && input.FastPan) zoomToMouse = false;

            if (zoomToMouse && oldSize != pose.Size)
            {
                double sizeDelta = projectedSize - nextProjectedSize;
                pose.X += (input.PointerX / input.Width * 2 - 1) * sizeDelta * input.Width / input.Height;
                pose.Z += (1 - input.PointerY / input.Height * 2) * sizeDelta;
            }

            return pose = ConstrainPose(pose);
        }

        private CameraPose AdvancePan(double seconds)
        {
            panElapsed = Math.Min(panDuration, panElapsed + seconds);
            double t = panDuration > 0 ? panElapsed / panDuration : 1;
            double blend = t * t * t * (t * (t * 6 - 15) + 10);

            CameraPose next = new CameraPose(
                panSource.X + (panTarget.X - panSource.X) * blend,
                panSource.Z + (panTarget.Z - panSource.Z) * blend,
                panSource.Size + (panTarget.Size - panSource.Size) * blend);
            if (t >= 1) next = panTarget;
            pose = ConstrainPose(next);
            DesiredSize = pose.Size;

            if (t >= 1)
            {
                LastFinishedPanId = activePanId;
                LastPanCompleted = true;
                activePanId = 0;
            }

            return pose;
        }

        private CameraPose ConstrainPose(CameraPose value)
        {
            API.CameraMotionBounds? bounds = Settings.Bounds;
            if (bounds == null) return value;
            if (bounds.MaximumSize.HasValue) value.Size = Math.Min(value.Size, bounds.MaximumSize.Value);

            // Both axes are evaluated at the final size before either is committed.
            double x = bounds.X?.Clamp(value.X, value.Size) ?? value.X;
            double z = bounds.Z?.Clamp(value.Z, value.Size) ?? value.Z;
            return new CameraPose(x, z, value.Size);
        }

        private double ConstrainSize(double value)
        {
            double? maximum = Settings.Bounds?.MaximumSize;
            return maximum.HasValue ? Math.Min(value, maximum.Value) : value;
        }

        private double InertiaMovement(double dt)
        {
            if (Settings.SpeedDecay == 1) return dt;
            return (1 - Math.Pow(Settings.SpeedDecay, dt * 60)) / (1 - Settings.SpeedDecay) / 60;
        }

        private double Project(double logicalSize)
        {
            double result = Settings.Profile?.Projection?.Evaluate(logicalSize) ?? logicalSize;
            if (!Finite(result) || result <= 0)
                throw new InvalidOperationException("Camera projection must be finite and positive.");
            return result;
        }

        private double Rate(API.CameraCurve? curve, double fallback, double projected)
            => curve?.Evaluate(pose.Size, DesiredSize, projected) ?? fallback;

        private void AddEdgePan(CameraInput input, double edgeRate, ref double x, ref double z)
        {
            if (!Settings.EdgeScroll || !input.AllowEdgeScroll || input.PointerOverUi)
            {
                bottomEdgeSince = -1;
                return;
            }

            double uiScale = input.UiScale > 0 ? input.UiScale : 1;
            double width = input.Width / uiScale;
            double height = input.Height / uiScale;
            double px = input.PointerX / uiScale;
            double py = input.PointerY / uiScale;
            double inspectHeight = Math.Max(250, input.InspectPaneHeight);

            // The four corners hold vanilla UI and never edge scroll.
            if (Contains(px, py, 0, 0, 200, 200) ||
                Contains(px, py, width - 250, 0, 255, 255) ||
                Contains(px, py, 0, height - inspectHeight, 225, inspectHeight + 5) ||
                Contains(px, py, width - 250, height - 250, 255, 255))
            {
                bottomEdgeSince = -1;
                return;
            }

            if (px >= 0 && px < 20) x -= edgeRate;
            if (px <= width && px > width - 20) x += edgeRate;
            if (py >= 0 && py < 20) z += edgeRate;

            double bottomWidth = input.Fullscreen ? 6 : 20;
            if (py <= height && py > height - bottomWidth)
            {
                if (bottomEdgeSince < 0) bottomEdgeSince = elapsed;
                if (elapsed - bottomEdgeSince >= 0.28) z -= edgeRate;
            }
            else
            {
                bottomEdgeSince = -1;
            }
        }

        private void ValidateSettings()
        {
            if (Settings == null || !Finite(Settings.MinSize) || !Finite(Settings.MaxSize) ||
                Settings.MinSize <= 0 || Settings.MaxSize <= Settings.MinSize ||
                !Finite(Settings.SpeedDecay) || Settings.SpeedDecay < 0 || Settings.SpeedDecay > 1 ||
                !Finite(Settings.ZoomPreserveFactor) || Settings.ZoomPreserveFactor < 0 || Settings.ZoomPreserveFactor > 1 ||
                !Nonnegative(Settings.DollyRateKeys) || !Nonnegative(Settings.DollyRateScreenEdge) ||
                !Nonnegative(Settings.MoveSpeed) || !Nonnegative(Settings.ZoomSpeed) ||
                !Nonnegative(Settings.ScrollWheelRate) || !Nonnegative(Settings.DragSensitivity))
                throw new InvalidOperationException("The game supplied an invalid camera configuration.");
        }

        private static void ValidateInput(CameraInput input)
        {
            if (!Finite(input.PanX) || !Finite(input.PanZ) || !Finite(input.DragX) || !Finite(input.DragY) ||
                !Finite(input.WheelDelta) || !Finite(input.PointerX) || !Finite(input.PointerY) ||
                !Nonnegative(input.UiScale) || !Nonnegative(input.InspectPaneHeight))
                throw new ArgumentOutOfRangeException(nameof(input));
        }

        private static bool Contains(double x, double y, double left, double top, double width, double height)
            => x >= left && y >= top && x < left + width && y < top + height;

        internal static bool Finite(double value) => !double.IsNaN(value) && !double.IsInfinity(value);

        private static bool Nonnegative(double value) => Finite(value) && value >= 0;

        internal static double Clamp(double value, double min, double max) => Math.Max(min, Math.Min(max, value));
    }

    public struct CameraPoint
    {
        public double X;
        public double Y;

        public CameraPoint(double x, double y)
        {
            X = x;
            Y = y;
        }
    }

    // Normalized top-down texture coordinates; values outside [0, 1] have no captured pixels.
    public struct CameraCrop
    {
        public double X;
        public double Y;
        public double Width;
        public double Height;

        public bool FullyCovered => X >= -1e-10 && Y >= -1e-10 && X + Width <= 1 + 1e-10 && Y + Height <= 1 + 1e-10;
    }

    public static class CameraProjection
    {
        public static CameraPoint ScreenToWorld(CameraPose view, double width, double height, double x, double y)
        {
            ValidatePose(view);
            ValidateDimensions(width, height);
            ValidateCoordinates(x, y);

            double units = 2 * view.Size / height;
            return CheckedPoint(view.X + (x - width * 0.5) * units, view.Z - (y - height * 0.5) * units);
        }

        public static CameraPoint WorldToScreen(CameraPose view, double width, double height, double x, double z)
        {
            ValidatePose(view);
            ValidateDimensions(width, height);
            ValidateCoordinates(x, z);

            double pixels = height / (2 * view.Size);
            return CheckedPoint(width * 0.5 + (x - view.X) * pixels, height * 0.5 - (z - view.Z) * pixels);
        }

        public static CameraPoint ReprojectPoint(CameraPose from, double fromWidth, double fromHeight,
            CameraPose to, double toWidth, double toHeight, double x, double y)
        {
            CameraPoint world = ScreenToWorld(from, fromWidth, fromHeight, x, y);
            return WorldToScreen(to, toWidth, toHeight, world.X, world.Y);
        }

        public static CameraCrop CropForView(CameraPose source, double sourceWidth, double sourceHeight,
            CameraPose view, double viewWidth, double viewHeight)
        {
            CameraPoint topLeft = ScreenToWorld(view, viewWidth, viewHeight, 0, 0);
            CameraPoint sourcePoint = WorldToScreen(source, sourceWidth, sourceHeight, topLeft.X, topLeft.Y);

            CameraCrop crop = new CameraCrop
            {
                X = sourcePoint.X / sourceWidth,
                Y = sourcePoint.Y / sourceHeight,
                Width = view.Size / source.Size * viewWidth / viewHeight * sourceHeight / sourceWidth,
                Height = view.Size / source.Size
            };

            if (!CameraMotion.Finite(crop.X) || !CameraMotion.Finite(crop.Y) ||
                !CameraMotion.Finite(crop.Width) || !CameraMotion.Finite(crop.Height) || crop.Width <= 0 || crop.Height <= 0)
                throw new ArithmeticException("The camera crop exceeds the supported numeric range.");
            return crop;
        }

        public static CameraPose Overscan(CameraPose view, double multiplier)
        {
            ValidatePose(view);
            if (!CameraMotion.Finite(multiplier) || multiplier < 1)
                throw new ArgumentOutOfRangeException(nameof(multiplier));

            CameraPose result = new CameraPose(view.X, view.Z, view.Size * multiplier);
            ValidatePose(result);
            return result;
        }

        internal static void ValidatePose(CameraPose pose)
        {
            if (!CameraMotion.Finite(pose.X) || !CameraMotion.Finite(pose.Z) ||
                !CameraMotion.Finite(pose.Size) || pose.Size <= 0)
                throw new ArgumentOutOfRangeException(nameof(pose));
        }

        internal static void ValidateDimensions(double width, double height)
        {
            if (!CameraMotion.Finite(width) || !CameraMotion.Finite(height) || width <= 0 || height <= 0)
                throw new ArgumentOutOfRangeException(nameof(width));
        }

        private static void ValidateCoordinates(double x, double y)
        {
            if (!CameraMotion.Finite(x) || !CameraMotion.Finite(y))
                throw new ArgumentOutOfRangeException(nameof(x));
        }

        private static CameraPoint CheckedPoint(double x, double y)
        {
            if (!CameraMotion.Finite(x) || !CameraMotion.Finite(y))
                throw new ArithmeticException("The projected coordinate exceeds the supported numeric range.");
            return new CameraPoint(x, y);
        }
    }
}
