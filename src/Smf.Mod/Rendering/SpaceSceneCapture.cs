using System;
using RimWorld.Planet;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal sealed class SpaceSceneCapture
{
    private static SpaceSceneCapture? selected;
    private readonly Camera source;
    private readonly Map capturedMap;
    private readonly Func<bool> mayCapture;
    private readonly MapCoverageCapture coverage;
    private readonly SpaceDebrisDraws draws;
    private readonly SpaceMapCapture map;
    private readonly SpaceBackgroundCapture background;
    private readonly SpaceDebrisCapture debris;
    private readonly SpaceImageEffects effects;
    private readonly ScenePacketBuffer packet;
    private bool released;

    internal bool TargetsRestored => map.TargetsRestored && background.TargetsRestored && debris.TargetsRestored;

    internal SpaceSceneCapture(MapCoverageCapture coverage, Func<bool> mayCapture, Action<Exception> reportFailure,
        Material premultCopy)
    {
        this.coverage = coverage;
        this.mayCapture = mayCapture;
        capturedMap = Find.CurrentMap;
        source = Find.Camera;
        try
        {
            draws = new SpaceDebrisDraws(Find.CurrentMap, reportFailure);
            map = new SpaceMapCapture(source, WorldCameraManager.WorldCamera, premultCopy, mayCapture, reportFailure);
            background = new SpaceBackgroundCapture(Find.CurrentMap, WorldCameraManager.WorldCamera,
                WorldCameraManager.WorldSkyboxCamera);
            debris = new SpaceDebrisCapture(source, coverage, draws, premultCopy);
            effects = new SpaceImageEffects(source, coverage);
            packet = new ScenePacketBuffer();
        }
        catch
        {
            Release();
            throw;
        }
    }

    internal static void InstallHooks(string owner)
    {
        SpaceMapCapture.InstallHooks(owner);
        SpaceDebrisDraws.InstallHooks(owner);
    }

    internal static void RemoveHooks()
    {
        Select(null);
        SpaceMapCapture.RemoveHooks();
        SpaceDebrisDraws.RemoveHooks();
    }

    internal static void Select(SpaceSceneCapture? capture)
    {
        if (!ReferenceEquals(selected, capture))
        {
            // Retained generations keep their images, but stop copying new frames.
            selected?.background.Select(false);
            selected = capture;
            selected?.background.Select(true);
        }
        SpaceMapCapture.Select(capture?.map);
        SpaceDebrisDraws.Select(capture?.draws);
    }

    internal bool Attach(ref FrameBundle bundle)
    {
        if (released)
            throw new ObjectDisposedException(nameof(SpaceSceneCapture));
        if (!mayCapture() || !MapSceneReadiness.Ready || capturedMap.Disposed || Find.CurrentMap != capturedMap ||
            Find.Camera != source || !WorldRendererUtility.DrawingMap)
            return false;
        if (!background.Capture())
            return false;
        debris.Capture(map.Background);
        if (!map.Capture(debris.Reference) || !effects.Prepare())
            return false;

        packet.Begin(bundle.Key.SourceFrame);
        ScenePackets.ImageFlags flags = SystemInfo.graphicsUVStartsAtTop
            ? ScenePackets.ImageFlags.FlipY : ScenePackets.ImageFlags.None;
        uint planet = packet.AddImage(map.BackgroundPointer, bundle.Key.SourceFrame, bundle.Width, bundle.Height, flags);
        uint reference = packet.AddImage(debris.ReferencePointer, bundle.Key.SourceFrame, bundle.Width, bundle.Height, flags);
        ScenePackets.ImageFlags inputFlags = map.InputFlippedY
            ? ScenePackets.ImageFlags.FlipY : ScenePackets.ImageFlags.None;
        var live = new ScenePackets.Layer
        {
            Kind = ScenePackets.LayerKind.LiveMap,
            Color = packet.AddImage(map.InputPointer, bundle.Key.SourceFrame, bundle.Width, bundle.Height, inputFlags),
            Probe = packet.AddImage(map.ProbePointer, bundle.Key.SourceFrame, bundle.Width, bundle.Height, flags),
            Reference = reference,
            Depth = ScenePackets.NoImage,
            SourceX = bundle.Pose.X,
            SourceZ = bundle.Pose.Z
        };
        SceneCaptureGeometry.SetProjection(bundle.Pose, bundle.Width, bundle.Height, ref live);
        packet.AddLayer(live);
        coverage.DescribeRaw(packet);
        packet.SetBackground(background.Describe(packet, planet));
        debris.Describe(packet, planet, reference);
        effects.Describe(packet);
        bundle.SceneDescription = packet.Pointer;
        return true;
    }

    internal void Release()
    {
        if (released)
            return;
        if (ReferenceEquals(selected, this))
            Select(null);
        map?.Release();
        background?.Release();
        debris?.Release();
        effects?.Dispose();
        packet?.Dispose();
        released = true;
    }
}
