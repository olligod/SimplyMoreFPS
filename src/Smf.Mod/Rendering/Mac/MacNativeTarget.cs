#nullable disable
using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

public sealed partial class MacRendererApi
{
    private readonly NativeTargetFn stageNativeTarget;

    private int StageCurrentNativeTarget(ref NativeFrameMarker value)
    {
        RequireMainThread(); // Before any Unity getter.
        if (value.BeginOnly) throw new ArgumentException("Only a complete native frame has an EOF target.");
        // During preparation or retirement a valid frame can be briefly unavailable.
        if (value.Session == 0 || value.Session != startedSession || value.Session != sourceTargetSession) return 1;
        if (value.Content == 0
            || value.SourceFrame == 0
            || value.Width == 0
            || value.Height == 0
            || value.Width > 16384
            || value.Height > 16384
            || (ulong)value.Width * value.Height * 4 > 64UL * 1024 * 1024)
            throw new ArgumentException("The native EOF target requires a matching frame identity and extent.");
        // Generation zero is valid here: native restoration frames carry
        // their identity in RestoreSerial instead.
        if (RenderTexture.active != null) return 1;

        IntPtr nativeBuffer = Graphics.activeColorBuffer.GetNativeRenderBufferPtr();
        if (nativeBuffer == IntPtr.Zero) return 1;

        var target = new MacSourceTarget
        {
            Size = 64,
            Version = 1,
            Session = value.Session,
            ContentRevision = value.Content,
            Generation = value.Generation,
            SourceFrame = value.SourceFrame,
            NativeRenderBuffer = unchecked((ulong)nativeBuffer.ToInt64()),
            Width = value.Width,
            Height = value.Height,
            Flags = 0,
            Reserved = 0
        };

        // Consumed by the existing complete-frame callback; no extra event or wait.
        return stageNativeTarget(ref target, 64);
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int NativeTargetFn(ref MacSourceTarget target, uint bytes);
}
