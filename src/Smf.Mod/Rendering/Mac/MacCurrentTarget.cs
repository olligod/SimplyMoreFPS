#nullable disable
using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

public sealed partial class MacRendererApi
{
    private readonly SourceTargetEnableFn enableSourceTarget;
    private readonly SourceTargetFn stageSourceTarget;
    private ulong sourceTargetSession;

    private int StageCurrentSourceTarget(ref FrameBundle value)
    {
        RequireMainThread(); // Before any Unity getter, including the checks below.
        if (value.Key.Session == 0
            || value.Key.Session != startedSession
            || value.Key.Session != sourceTargetSession
            || value.Key.Content == 0
            || value.Key.Generation == 0
            || value.Key.SourceFrame == 0
            || value.Width == 0
            || value.Height == 0
            || value.Width > 16384
            || value.Height > 16384
            || (ulong)value.Width * value.Height * 4 > 64UL * 1024 * 1024)
            throw new ArgumentException("The current target requires a matching live frame identity and extent.");
        if (RenderTexture.active != null) return 1;

        IntPtr nativeBuffer = Graphics.activeColorBuffer.GetNativeRenderBufferPtr();
        if (nativeBuffer == IntPtr.Zero) return 1;

        var target = new MacSourceTarget
        {
            Size = 64,
            Version = 1,
            Session = value.Key.Session,
            ContentRevision = value.Key.Content,
            Generation = value.Key.Generation,
            SourceFrame = value.Key.SourceFrame,
            NativeRenderBuffer = unchecked((ulong)nativeBuffer.ToInt64()),
            Width = value.Width,
            Height = value.Height,
            Flags = 0,
            Reserved = 0
        };

        // The existing pre-GUI render event resolves this; no extra event or wait.
        return stageSourceTarget(ref target, 64);
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct MacSourceTarget
    {
        public uint Size, Version;
        public ulong Session, ContentRevision, Generation, SourceFrame, NativeRenderBuffer;
        public uint Width, Height, Flags, Reserved;
    }

    private static void AssertSourceTargetAbi()
    {
        AssertSize<MacSourceTarget>(64);
        if ((int)Marshal.OffsetOf(typeof(MacSourceTarget), nameof(MacSourceTarget.Session)) != 8
            || (int)Marshal.OffsetOf(typeof(MacSourceTarget), nameof(MacSourceTarget.NativeRenderBuffer)) != 40
            || (int)Marshal.OffsetOf(typeof(MacSourceTarget), nameof(MacSourceTarget.Width)) != 48
            || (int)Marshal.OffsetOf(typeof(MacSourceTarget), nameof(MacSourceTarget.Reserved)) != 60)
            throw new InvalidOperationException("Mac source target ABI field offsets differ from the native header.");
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SourceTargetEnableFn(ulong session);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SourceTargetFn(ref MacSourceTarget target, uint bytes);
}
