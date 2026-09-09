using System.Runtime.InteropServices;

namespace Smf.Camera.TestFixture;

// The two packets the mod exchanges with the camera worker. Reference.cs checks their
// layout and the float conversion rule the mod applies before handing a pose to Unity.

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct DesiredPose
{
    public const uint VersionValue = 2;
    public const uint ByteSize = 88;
    public uint Version;
    public uint Size;
    public ulong Epoch;
    public ulong Sequence;
    public int MapId;
    public uint Flags; // reserved, must be zero
    public double X;
    public double Z;
    public double RootSize;
    public double ProjectionHalfHeight;
    public ulong ActivePanId;
    public ulong FinishedPanId;
    public uint PanFlags;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct MainCameraState
{
    public const uint VersionValue = 2;
    public const uint ByteSize = 112;
    public uint Version;
    public uint Size;
    public ulong Epoch;
    public ulong AppliedSequence;
    public ulong SourceFrame;
    public int MapId;
    public uint Flags;
    public double X;
    public double Z;
    public double RootSize;
    public double MinSize;
    public double MaxSize;
    public double UiScale;
    public uint PixelWidth;
    public uint PixelHeight;
    public uint Reason;
    public uint Reserved;
    public double ProjectionHalfHeight;
}

public static class PoseValidation
{
    // Vanilla clamps translation before zoom-to-mouse compensation, so a valid pose can sit
    // outside the map. Only the float conversion is checked here, never the range.
    public static bool CanConvertToUnity(DesiredPose p)
    {
        return FitsFloat(p.X) && FitsFloat(p.Z) &&
            FitsFloat(p.RootSize) && p.RootSize > 0 && (float)p.RootSize > 0 &&
            FitsFloat(p.ProjectionHalfHeight) && p.ProjectionHalfHeight > 0 && (float)p.ProjectionHalfHeight > 0;
    }

    private static bool FitsFloat(double n) => Finite(n) && n >= -float.MaxValue && n <= float.MaxValue;

    private static bool Finite(double n) => !double.IsNaN(n) && !double.IsInfinity(n);
}
