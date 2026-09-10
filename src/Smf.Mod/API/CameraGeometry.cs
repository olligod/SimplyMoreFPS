using System;
using System.Runtime.CompilerServices;

namespace SimplyMoreFPS.API;

/// <summary>Size of the area the camera may move in; the fixed two-cell inset is applied on top.</summary>
public readonly struct CameraMovementExtent
{
    public double Width
    {
        get;
    }
    public double Height
    {
        get;
    }

    public CameraMovementExtent(double width, double height)
    {
        Width = width;
        Height = height;
        RequireValid();
    }

    internal void RequireValid()
    {
        bool valid = !double.IsNaN(Width) && !double.IsInfinity(Width) && Width >= 4
            && !double.IsNaN(Height) && !double.IsInfinity(Height) && Height >= 4;
        if (!valid)
            throw new ArgumentOutOfRangeException(nameof(CameraMovementExtent), "Camera movement extents must be finite and at least four cells.");
    }
}

/// <summary>World-space bounds of what the renderer covers, independent of the map grid.</summary>
public readonly struct CameraCoverageBounds
{
    public float MinX
    {
        get;
    }
    public float MaxX
    {
        get;
    }
    public float MinZ
    {
        get;
    }
    public float MaxZ
    {
        get;
    }

    public CameraCoverageBounds(float minX, float maxX, float minZ, float maxZ)
    {
        MinX = minX;
        MaxX = maxX;
        MinZ = minZ;
        MaxZ = maxZ;
        RequireValid();
    }

    internal void RequireValid()
    {
        bool valid = Finite(MinX) && Finite(MaxX) && Finite(MinZ) && Finite(MaxZ)
            && MaxX > MinX && MaxZ > MinZ
            && Finite(MaxX - MinX) && Finite(MaxZ - MinZ);
        if (!valid)
            throw new ArgumentOutOfRangeException(nameof(CameraCoverageBounds), "Camera coverage must have finite, positive axis spans.");
    }

    private static bool Finite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);
}

/// <summary>Main-thread patch points for camera movement and coverage geometry.</summary>
public static class CameraGeometry
{
    private static readonly CameraGeometryPolicy DefaultPolicy = new CameraGeometryPolicy();

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static CameraGeometryPolicy GetPolicy(CameraContext context)
    {
        RequireContext(context);
        return CameraGeometryProviders.ResolvePolicy(context) ?? DefaultPolicy;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static CameraMovementExtent GetMovementExtent(CameraContext context)
    {
        RequireContext(context);
        return CameraGeometryProviders.ResolveMovementExtent(context)
            ?? new CameraMovementExtent(context.Map.Size.x, context.Map.Size.z);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static CameraCoverageBounds GetCoverageBounds(CameraContext context)
    {
        RequireContext(context);

        CameraGeometryPolicy policy = GetPolicy(context) ?? throw new InvalidOperationException("Camera geometry policy cannot be null.");
        if (policy.CoverageBounds.HasValue)
            return policy.CoverageBounds.Value;
        return CameraGeometryProviders.ResolveCoverageBounds(context)
            ?? new CameraCoverageBounds(-4, (float)context.Map.Size.x + 4, -4, (float)context.Map.Size.z + 4);
    }

    private static void RequireContext(CameraContext context)
    {
        CameraProviders.RequireMainThread();
        if (context.Map == null || context.Driver == null)
            throw new ArgumentException("Camera geometry requires a live map camera context.", nameof(context));
    }
}
