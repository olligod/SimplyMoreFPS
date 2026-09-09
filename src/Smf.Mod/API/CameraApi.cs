using System;
using SimplyMoreFPS.Rendering.CameraControl;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.API;

/// <summary>Snapshot of the detached camera state.</summary>
public sealed class CameraStatus
{
    public bool DetachedMotionActive { get; }
    public bool Installed { get; }
    public string ProviderId { get; }
    public string LastError { get; }
    public int MapId { get; }

    public CameraStatus(bool detachedMotionActive, bool installed, string providerId, string lastError, int mapId)
    {
        DetachedMotionActive = detachedMotionActive;
        Installed = installed;
        ProviderId = providerId ?? "";
        LastError = lastError ?? "";
        MapId = mapId;
    }
}

/// <summary>Main-thread entry points for reading and moving the map camera.</summary>
public static class CameraApi
{
    public static int Version => 1;

    public static CameraStatus GetStatus()
    {
        CameraProviders.RequireMainThread();
        return CameraOwnershipAdapter.GetPublicStatus();
    }

    /// <summary>Jumps on the current map. Size is in the game's camera units.</summary>
    public static void Jump(Vector3 position, float? size = null)
    {
        CameraDriver driver = RequireCamera();
        float targetSize = size ?? driver.RootSize;
        Validate(position, targetSize, 0);

        driver.SetRootPosAndSize(position, targetSize);
    }

    /// <summary>Pans with vanilla duration and completion semantics.</summary>
    public static void Pan(Vector3 position, float size, float duration = 0.25f, Action? completion = null)
    {
        CameraDriver driver = RequireCamera();
        Validate(position, size, duration);

        PanCompletionCallback? callback = completion == null ? null : new PanCompletionCallback(completion);
        driver.PanToMapLocAndSize(position, size, duration, callback);
    }

    private static CameraDriver RequireCamera()
    {
        CameraProviders.RequireMainThread();
        if (Find.CurrentMap == null || Find.CameraDriver == null)
            throw new InvalidOperationException("No current map camera.");

        return Find.CameraDriver;
    }

    private static void Validate(Vector3 position, float size, float duration)
    {
        bool valid = Finite(position.x) && Finite(position.y) && Finite(position.z)
            && Finite(size) && size > 0
            && Finite(duration) && duration >= 0;
        if (!valid)
            throw new ArgumentOutOfRangeException(nameof(position), "Camera pose and duration must be finite, size positive and duration nonnegative.");
    }

    private static bool Finite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);
}
