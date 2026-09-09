#nullable disable
using System;
using Verse;

namespace SimplyMoreFPS.Rendering.CameraControl;

public static partial class CameraOwnershipAdapter
{
    // A game-started pan that the native worker is currently driving.
    private sealed class CapturedPan
    {
        internal CameraDriver Driver;
        internal CameraPanner Original;
        internal NativeCameraBridge.KernelTrajectory Packet;
    }

    private static CapturedPan capturedPan;
    private static ulong panCounter;

    internal static NativeCameraBridge.KernelTrajectory Trajectory => capturedPan == null ? default : capturedPan.Packet;

    private static bool MatchesScriptedPan(DesiredPose pose)
    {
        if (capturedPan == null) return true;
        if (pose.ActivePanId == capturedPan.Packet.Id || pose.FinishedPanId == capturedPan.Packet.Id) return true;
        // A fresh worker epoch must get its empty seed accepted before it can
        // pick up the pending pan.
        return !owned && acceptedSequence == 0 && pose.ActivePanId == 0 && pose.FinishedPanId == 0 && pose.PanFlags == 0;
    }

    private static void CaptureScriptedPan()
    {
        // The game (and any hooks) already chose source, target and duration.
        CameraPanner pan = Panner(driver);
        var source = pan.Source;
        var target = pan.Destination;
        double now = NativeCameraBridge.NowSeconds();

        if (float.IsNaN(pan.Duration) || float.IsInfinity(pan.Duration) || pan.Duration < 0
            || float.IsNaN(pan.TimeSinceStart) || float.IsInfinity(pan.TimeSinceStart) || pan.TimeSinceStart < 0)
            throw new InvalidOperationException("Scripted camera pan has an invalid duration or elapsed time.");

        var packet = new NativeCameraBridge.KernelTrajectory
        {
            Id = checked(++panCounter),
            Kind = 1,
            StartSeconds = now - Math.Min(now, pan.TimeSinceStart),
            DurationSeconds = pan.Duration,
            SourceX = source.Position.x,
            SourceZ = source.Position.z,
            SourceRootSize = source.Size,
            TargetX = target.Position.x,
            TargetZ = target.Position.z,
            TargetRootSize = target.Size
        };

        if (packet.Id > long.MaxValue) throw new InvalidOperationException("Scripted camera pan ID space exhausted.");
        // Completion stays in Mono. Replacing a pan drops the old callback,
        // exactly as CameraPanner.PanTo does.
        capturedPan = new CapturedPan { Driver = driver, Original = pan, Packet = packet };

        pan.Moving = false;
        pan.Completion = null;
        Panner(driver) = pan;
    }

    private static void CompleteScriptedPan(DesiredPose pose)
    {
        CapturedPan captured = capturedPan;
        if (captured == null || pose.FinishedPanId != captured.Packet.Id) return;

        capturedPan = null;
        if ((pose.PanFlags & 1) == 0) return; // Cancelled, not completed.
        if (captured.Driver != driver || Find.CameraDriver != driver || Find.CurrentMap != map) return;

        // The completion callback may replace the pan, map or renderer, so the
        // capture is already cleared and context is refreshed afterwards.
        try
        {
            captured.Original.Completion?.Invoke();
        }
        finally
        {
            if (Installed)
            {
                RefreshContext();
                RefreshPolicy();
                ObservePose();
            }
        }
    }

    private static void ReleaseScriptedPan(bool restore)
    {
        CapturedPan captured = capturedPan;
        if (captured == null) return;
        capturedPan = null;
        if (!restore || captured.Driver == null || captured.Driver != driver) return;

        CameraPanner original = captured.Original;
        double elapsed = Math.Max(0, NativeCameraBridge.NowSeconds() - captured.Packet.StartSeconds);
        original.TimeSinceStart = (float)Math.Min(elapsed, original.Duration);
        original.Moving = true;

        // Hand the pan back with its original source so the easing curve
        // continues instead of restarting.
        Panner(captured.Driver) = original;
    }
}
