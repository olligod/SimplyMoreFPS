#nullable disable
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using RimWorld;
using RimWorld.Planet;
using SimplyMoreFPS.API;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

// Draws the whole map into an offscreen texture with clones of the game's cameras, riding on
// the game's own draw pass. The original cameras and the simulation's view queries are untouched.
internal sealed class MapCoverageCapture
{
    private static MapCoverageCapture active;
    private static Harmony harmony;

    // CameraDriver keeps one static view cache for every driver; Select ends the old borrow
    // before a new owner writes it.
    private static readonly AccessTools.FieldRef<CellRect> LastViewRect = AccessTools.StaticFieldRefAccess<CellRect>(AccessTools.Field(typeof(CameraDriver), "lastViewRect"));
    private static readonly AccessTools.FieldRef<int> LastViewRectFrame = AccessTools.StaticFieldRefAccess<int>(AccessTools.Field(typeof(CameraDriver), "lastViewRectGetFrame"));
    private static readonly MethodInfo DrawStart = AccessTools.Method(typeof(GlobalRendererUtility), "UpdateGlobalShadersParams");
    private static readonly MethodInfo DrawEnd = AccessTools.Method(typeof(FleckManager), "FleckManagerDraw");

    private readonly Map map;
    private readonly Camera source;
    private readonly CameraDriver driver;
    private readonly BorrowedViewCache<ViewCacheValue> viewCache;
    private readonly BorrowedViewCache<ViewCacheValue>.Lease[] drawScopes = new BorrowedViewCache<ViewCacheValue>.Lease[64];
    private int drawScopeCount;
    private readonly Func<bool> mayCapture;
    private readonly Action<Exception> reportFailure;
    private readonly List<Auxiliary> cameras = new List<Auxiliary>();
    private readonly int nativeWidth;
    private readonly int nativeHeight;
    private Auxiliary color;
    private CoverageColorEffect effect;
    private CellRect fullMapView;
    private Matrix4x4 view;
    private Matrix4x4 projection;
    private float minX;
    private float maxX;
    private float minZ;
    private float maxZ;
    private Affine pendingAffine;
    private Affine cachedAffine;
    private ulong cacheSerial;
    private ulong texturePointer;
    private int refreshTick;
    private int cachedTick;
    private long nextRefresh;
    private Globals savedGlobals;
    private int frame = -1;
    private int finishedFrame = -1;
    private bool rendering;
    private bool released;
    private bool cancelled;
    private bool boundsDirty;

    internal uint Width { get; private set; }

    internal uint Height { get; private set; }

    internal bool TargetsRestored => savedGlobals == null && !rendering && viewCache.Count == 0;

    private bool Capturing => !released && !cancelled && frame == Time.frameCount && finishedFrame != frame;

    internal MapCoverageCapture(SceneContext context, Func<bool> eligible, Action<Exception> onFailure)
    {
        map = Find.CurrentMap;
        source = Find.Camera;
        driver = Find.CameraDriver;

        viewCache = new BorrowedViewCache<ViewCacheValue>(
            () => new ViewCacheValue { Rect = LastViewRect(), Frame = LastViewRectFrame() },
            value =>
            {
                LastViewRect() = value.Rect;
                LastViewRectFrame() = value.Frame;
            });

        nativeWidth = checked((int)context.Width);
        nativeHeight = checked((int)context.Height);
        mayCapture = eligible;
        reportFailure = onFailure;
    }

    internal void Create()
    {
        CheckSource();
        UpdateBounds();
        PrepareProjection(true);

        if (Width > SystemInfo.maxTextureSize || Height > SystemInfo.maxTextureSize ||
            Width > 16384 || Height > 16384 ||
            (ulong)Width * Height * 4 > 64ul * 1024 * 1024)
        {
            throw new InvalidOperationException("Map coverage exceeds the device texture limit.");
        }

        float depth = Camera.allCameras.Max(c => c.depth) + 1;

        foreach (SubcameraDef def in DefDatabase<SubcameraDef>.AllDefsListForReading.OrderBy(d => d.depth))
        {
            Camera original = Current.SubcameraDriver.GetSubcamera(def);
            if (original == null || !original.enabled) continue;

            if (def.doNotUpdate || original.targetTexture == null || original.commandBufferCount != 0 ||
                original.GetComponents<MonoBehaviour>().Length != 0)
            {
                throw new InvalidOperationException("Unsupported map coverage dependent camera: " + def.defName);
            }

            RenderTextureDescriptor descriptor = original.targetTexture.descriptor;
            descriptor.width = (int)Width;
            descriptor.height = (int)Height;
            AddCamera(original, new RenderTexture(descriptor), depth++, def == SubcameraDefOf.WaterDepth);
        }

        color = AddCamera(source, new RenderTexture((int)Width, (int)Height, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default), depth, false);

        // An offscreen target starts empty, so paint the border outside the map with the source
        // camera's background before the normal color effect runs over it.
        color.Camera.clearFlags = CameraClearFlags.SolidColor;
        color.Camera.backgroundColor = source.backgroundColor;

        effect = color.Owner.AddComponent<CoverageColorEffect>();
        effect.Bind(source);

        texturePointer = unchecked((ulong)color.Target.GetNativeTexturePtr().ToInt64());
        if (texturePointer == 0) throw new InvalidOperationException("The map cache has no native texture.");
    }

    private Auxiliary AddCamera(Camera original, RenderTexture target, float depth, bool water)
    {
        var item = new Auxiliary { Target = target, Water = water };

        // Track it before anything can throw so Release still cleans up a half-built camera.
        cameras.Add(item);

        target.name = "SMF map coverage " + original.name;
        target.hideFlags = HideFlags.HideAndDontSave;
        target.filterMode = FilterMode.Point;
        target.wrapMode = TextureWrapMode.Clamp;
        target.useMipMap = false;
        target.autoGenerateMips = false;

        if (!target.Create()) throw new InvalidOperationException("Map coverage texture creation failed.");

        item.Owner = new GameObject("SMF map coverage camera") { hideFlags = HideFlags.HideAndDontSave };
        UnityEngine.Object.DontDestroyOnLoad(item.Owner);
        item.Camera = item.Owner.AddComponent<Camera>();
        item.Camera.enabled = false;

        // CopyFrom copies the enabled flag too.
        item.Camera.CopyFrom(original);
        item.Camera.enabled = false;
        item.Camera.RemoveAllCommandBuffers();
        item.Camera.targetTexture = target;
        item.Camera.depth = depth;
        item.Camera.rect = new Rect(0, 0, 1, 1);
        item.Camera.allowDynamicResolution = false;

        return item;
    }

    private void CheckSource()
    {
        if (source != null && source.targetTexture != null)
        {
            throw new InvalidOperationException(
                "Map coverage does not support camera target replacement, including AntiAliasing SSAA: " + source.targetTexture.name);
        }

        if (!MapSceneReadiness.Ready || Find.CurrentMap != map || Find.Camera != source || Find.CameraDriver != driver ||
            source == null || !source.enabled || !source.orthographic || source.targetTexture != null ||
            !WorldRendererUtility.DrawingMap ||
            QualitySettings.activeColorSpace != ColorSpace.Gamma || QualitySettings.antiAliasing > 1 ||
            source.allowHDR || source.allowDynamicResolution || source.stereoEnabled ||
            source.rect != new Rect(0, 0, 1, 1) ||
            source.pixelWidth != nativeWidth || source.pixelHeight != nativeHeight)
        {
            throw new InvalidOperationException("Map coverage requires the full-client Gamma orthographic camera.");
        }
    }

    internal static void InstallHooks(string owner)
    {
        if (harmony != null) return;

        harmony = new Harmony(owner + ".coverage");

        harmony.Patch(AccessTools.Method(typeof(Root_Play), "Update"), prefix: Hook(nameof(BeforeRoot)));
        harmony.Patch(
            AccessTools.Method(typeof(Map), "MapUpdate"),
            prefix: Hook(nameof(BeforeMapUpdate)),
            transpiler: Hook(nameof(ScopeDraw)),
            finalizer: Hook(nameof(AfterMapUpdate)));
        harmony.Patch(
            AccessTools.Method(typeof(MapComponentUtility), "MapComponentOnDraw"),
            prefix: Hook(nameof(BeforeComponents)),
            finalizer: Hook(nameof(AfterComponents)));

        Camera.onPreCull += BeforeCull;
        Camera.onPreRender += BeforeRender;
        Camera.onPostRender += AfterRender;
    }

    internal static void RemoveHooks()
    {
        Select(null);
        if (harmony != null)
        {
            harmony.UnpatchAll(harmony.Id);
            harmony = null;
        }

        Camera.onPreCull -= BeforeCull;
        Camera.onPreRender -= BeforeRender;
        Camera.onPostRender -= AfterRender;
    }

    internal static void Select(MapCoverageCapture next)
    {
        if (ReferenceEquals(active, next)) return;

        if (active != null)
        {
            active.StopFrame();

            // The cameras may have been disabled mid-frame; drop that frame rather than publish it.
            active.cancelled = true;
        }

        active = next;
    }

    private static HarmonyMethod Hook(string name) => new HarmonyMethod(typeof(MapCoverageCapture), name);

    private static void BeforeRoot()
    {
        var capture = active;
        if (capture == null || !capture.mayCapture()) return;

        try
        {
            capture.BeginFrame();
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
    }

    private void BeginFrame()
    {
        if (frame >= 0 && finishedFrame != frame && !cancelled)
        {
            throw new InvalidOperationException("Coverage frame was not finished by the end of the previous frame.");
        }

        // Root_Play.Update can run once more after the game is cleared, before the host retires this capture.
        var tickManager = Current.Game?.tickManager;
        if (tickManager == null) return;

        int ticks = tickManager.TicksGame;

        // The host handles map and viewport changes on its next Update; do not arm a stale one.
        if (!MapSceneReadiness.Ready || Find.CurrentMap != map || Find.Camera != source || Find.CameraDriver != driver ||
            source == null || !WorldRendererUtility.DrawingMap ||
            source.pixelWidth != nativeWidth || source.pixelHeight != nativeHeight)
        {
            return;
        }

        // A render-target mode can change even while the simulation and map cache are paused.
        effect.Validate();
        UpdateBounds();
        if (!boundsDirty && cacheSerial != 0 && (ticks == cachedTick || Stopwatch.GetTimestamp() < nextRefresh)) return;

        CheckSource();

        // The color effect may still be building its textures.
        if (!effect.Refresh()) return;

        frame = Time.frameCount;
        finishedFrame = -1;
        cancelled = false;
        refreshTick = ticks;

        PrepareProjection(false);
        fullMapView = new CellRect(0, 0, map.Size.x, map.Size.z);
        effect.BeginFrame(frame);

        foreach (Auxiliary item in cameras)
        {
            item.PreRenders = 0;
            item.PostRenders = 0;
            item.Camera.transform.SetPositionAndRotation(source.transform.position, source.transform.rotation);
            item.Camera.orthographicSize = source.orthographicSize / Math.Abs(projection.m11 / source.projectionMatrix.m11);
            item.Camera.aspect = (float)Width / Height;
            item.Camera.worldToCameraMatrix = view;
            item.Camera.projectionMatrix = projection;
            item.Camera.cullingMatrix = projection * view;
            item.Camera.enabled = true;
        }

        color.Camera.backgroundColor = source.backgroundColor;
    }

    private void UpdateBounds()
    {
        // The texture covers only the map; native paints the fixed background underneath it and
        // takes that color from this texture's corner.
        CameraCoverageBounds bounds = CameraGeometry.GetCoverageBounds(new CameraContext(driver, map));
        bounds.RequireValid();

        bool changed = minX != bounds.MinX || maxX != bounds.MaxX || minZ != bounds.MinZ || maxZ != bounds.MaxZ;
        minX = bounds.MinX;
        maxX = bounds.MaxX;
        minZ = bounds.MinZ;
        maxZ = bounds.MaxZ;
        boundsDirty |= changed;
    }

    private void PrepareProjection(bool chooseSize)
    {
        view = source.worldToCameraMatrix;
        Matrix4x4 original = source.projectionMatrix;
        Matrix4x4 combined = original * view;

        // Same planar check the native side applies.
        ReadAffine(combined, (uint)nativeWidth, (uint)nativeHeight);

        float x0 = float.PositiveInfinity;
        float x1 = float.NegativeInfinity;
        float y0 = float.PositiveInfinity;
        float y1 = float.NegativeInfinity;

        Vector3[] corners =
        {
            new Vector3(minX, 0, minZ),
            new Vector3(maxX, 0, minZ),
            new Vector3(minX, 0, maxZ),
            new Vector3(maxX, 0, maxZ),
        };

        foreach (Vector3 corner in corners)
        {
            Vector3 clip = combined.MultiplyPoint3x4(corner);
            x0 = Math.Min(x0, clip.x);
            x1 = Math.Max(x1, clip.x);
            y0 = Math.Min(y0, clip.y);
            y1 = Math.Max(y1, clip.y);
        }

        if (!Finite(x0) || !Finite(x1) || !Finite(y0) || !Finite(y1) || x1 <= x0 || y1 <= y0)
        {
            throw new InvalidOperationException("Invalid full-map projected bounds.");
        }

        if (chooseSize)
        {
            double aspect = (double)(x1 - x0) * nativeWidth / ((y1 - y0) * nativeHeight);
            if (!Finite(aspect) || aspect <= 0) throw new InvalidOperationException("Invalid map cache aspect ratio.");

            Width = aspect >= 1 ? 2048u : (uint)Math.Max(1, Math.Round(2048 * aspect));
            Height = aspect >= 1 ? (uint)Math.Max(1, Math.Round(2048 / aspect)) : 2048u;
        }

        Matrix4x4 remap = Matrix4x4.identity;
        remap.m00 = 2 / (x1 - x0);
        remap.m03 = -(x1 + x0) / (x1 - x0);
        remap.m11 = 2 / (y1 - y0);
        remap.m13 = -(y1 + y0) / (y1 - y0);
        projection = remap * original;
    }

    private static bool Finite(double value) => !double.IsNaN(value) && !double.IsInfinity(value);

    private static Affine ReadAffine(Matrix4x4 matrix, uint width, uint height)
    {
        for (int i = 0; i < 16; ++i)
        {
            if (!Finite(matrix[i])) throw new InvalidOperationException("Nonfinite coverage projection.");
        }

        double horizontalBasis = Math.Sqrt((double)matrix.m00 * matrix.m00 + (double)matrix.m02 * matrix.m02);
        double verticalBasis = Math.Sqrt((double)matrix.m10 * matrix.m10 + (double)matrix.m12 * matrix.m12);

        // Four float ulps of rounding, so the check does not depend on zoom.
        const double angularTolerance = 4 * 1.1920928955078125e-7;
        if (Math.Abs(matrix.m30) > 1e-7 || Math.Abs(matrix.m32) > 1e-7 || Math.Abs(matrix.m33 - 1) > 1e-7 ||
            Math.Abs(matrix.m01) > angularTolerance * horizontalBasis ||
            Math.Abs(matrix.m11) > angularTolerance * verticalBasis)
        {
            throw new InvalidOperationException("Coverage requires the native vertical planar camera.");
        }

        var affine = new Affine
        {
            A = matrix.m00 * width * .5,
            B = matrix.m02 * width * .5,
            C = (matrix.m03 + 1) * width * .5,
            D = -matrix.m10 * height * .5,
            E = -matrix.m12 * height * .5,
            F = (1 - matrix.m13) * height * .5,
        };

        if (!Finite(affine.A) || !Finite(affine.B) || !Finite(affine.C) ||
            !Finite(affine.D) || !Finite(affine.E) || !Finite(affine.F) ||
            Math.Abs(affine.A * affine.E - affine.B * affine.D) < 1e-10)
        {
            throw new InvalidOperationException("Singular coverage projection.");
        }

        return affine;
    }

    private static void BeforeCull(Camera camera)
    {
        var capture = active;
        if (capture == null || !capture.Capturing) return;

        try
        {
            if (camera == capture.source && (capture.savedGlobals != null || capture.viewCache.Count != 0))
            {
                throw new InvalidOperationException("Coverage draw state survived into the native camera.");
            }

            if (!capture.cameras.Any(c => c.Camera == camera)) return;

            capture.rendering = true;
            if (capture.savedGlobals == null) capture.savedGlobals = new Globals();
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
    }

    private static void BeforeRender(Camera camera)
    {
        var capture = active;
        if (capture == null || !capture.Capturing) return;

        try
        {
            Auxiliary item = capture.cameras.FirstOrDefault(c => c.Camera == camera);
            if (item == null) return;

            ++item.PreRenders;
            if (item.PreRenders != 1) throw new InvalidOperationException("Coverage camera rendered twice in one native frame.");
            if (item != capture.color) return;

            capture.pendingAffine = ReadAffine(camera.projectionMatrix * camera.worldToCameraMatrix, capture.Width, capture.Height);
            capture.ValidateBackgroundCorner(capture.pendingAffine);
            if (capture.cameras.Any(c => c != item && (c.PreRenders != 1 || c.PostRenders != 1)))
            {
                throw new InvalidOperationException("Coverage dependency cameras did not finish before color.");
            }

            Auxiliary water = capture.cameras.FirstOrDefault(c => c.Water);
            if (water != null) Shader.SetGlobalTexture("_WaterOutputTex", water.Target);
            Shader.SetGlobalVector("_MainCameraScreenParams", new Vector4(capture.Width, capture.Height, 1f / capture.Width, 1f / capture.Height));
            Shader.SetGlobalMatrix("_MainCameraVP", GL.GetGPUProjectionMatrix(capture.projection, false) * capture.view);
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
    }

    private static void AfterRender(Camera camera)
    {
        var capture = active;
        if (capture == null || !capture.Capturing) return;

        try
        {
            Auxiliary item = capture.cameras.FirstOrDefault(c => c.Camera == camera);
            if (item == null) return;

            ++item.PostRenders;

            // The camera stays enabled here because OnRenderImage still has to run after this hook.
            if (item == capture.color) capture.RestoreGlobals();
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
        finally
        {
            if (capture.cameras.Any(c => c.Camera == camera)) capture.rendering = false;
        }
    }

    internal bool Attach(ref FrameBundle bundle)
    {
        if (released || cancelled || savedGlobals != null || rendering || viewCache.Count != 0) return false;

        if (Capturing)
        {
            if (!effect.Completed(frame) || cameras.Any(c => c.PreRenders != 1 || c.PostRenders != 1)) return false;
            PublishCache();
        }

        if (cacheSerial == 0 || cacheSerial > bundle.Key.SourceFrame || bundle.Pose.MapId != map.uniqueID ||
            bundle.Pose.CameraId != unchecked((ulong)source.GetInstanceID()))
        {
            return false;
        }

        bundle.CoverageTexture = texturePointer;
        bundle.CoverageSerial = cacheSerial;
        bundle.CoverageWidth = Width;
        bundle.CoverageHeight = Height;
        bundle.CoverageFlags = 1u | (SystemInfo.graphicsUVStartsAtTop ? 2u : 0u);
        bundle.CoverageA = cachedAffine.A;
        bundle.CoverageB = cachedAffine.B;
        bundle.CoverageC = cachedAffine.C;
        bundle.CoverageD = cachedAffine.D;
        bundle.CoverageE = cachedAffine.E;
        bundle.CoverageF = cachedAffine.F;

        return bundle.CoverageTexture != 0;
    }

    private void ValidateBackgroundCorner(Affine affine)
    {
        Material edge = map.MapEdgeMaterial;
        if (!map.DrawMapClippers ||
            (edge != MapEdgeClipDrawer.ClipMat && edge != MapEdgeClipDrawer.ClipMatMetalhell) ||
            edge == null || edge.shader != ShaderDatabase.MetaOverlay || !edge.shader.isSupported ||
            (edge.HasProperty("_MainTex") && edge.mainTexture != null && (edge.mainTexture.width != 1 || edge.mainTexture.height != 1)))
        {
            throw new InvalidOperationException("A uniform map background requires the native solid map edge material.");
        }

        // Native samples the top-left pixel (0,0) for its background after the map draw and color
        // correction, so that pixel has to land inside one of the game's edge clipper quads.
        double determinant = affine.A * affine.E - affine.B * affine.D;
        double u = .5 - affine.C;
        double v = .5 - affine.F;
        double x = (affine.E * u - affine.B * v) / determinant;
        double z = (affine.A * v - affine.D * u) / determinant;
        bool exterior = x < -3 || x > map.Size.x + 3 || z < -3 || z > map.Size.z + 3;

        // These are the four quads MapEdgeClipDrawer.DrawClippers draws.
        bool horizontal = z > 0 && z < map.Size.z &&
            ((x > -500 && x < 0) || (x > map.Size.x && x < map.Size.x + 500));
        bool vertical = x > map.Size.x * .5 - 500 && x < map.Size.x * .5 + 500 &&
            ((z > -500 && z < 0) || (z > map.Size.z && z < map.Size.z + 500));
        if (!Finite(x) || !Finite(z) || !exterior || !(horizontal || vertical))
        {
            throw new InvalidOperationException("Map cache background sample is not inside a native exterior clipper.");
        }
    }

    private void PublishCache()
    {
        if (cacheSerial == unchecked((ulong)frame)) return;

        cachedAffine = pendingAffine;
        cacheSerial = checked((ulong)frame);
        cachedTick = refreshTick;
        boundsDirty = false;
        nextRefresh = Stopwatch.GetTimestamp() + 2 * Stopwatch.Frequency;
    }

    internal void FinishFrame()
    {
        if (released || cancelled || frame != Time.frameCount || finishedFrame == frame) return;

        StopFrame();
        finishedFrame = frame;
        if (!effect.Completed(frame) || cameras.Any(c => c.PreRenders != 1 || c.PostRenders != 1))
        {
            throw new InvalidOperationException("Coverage did not complete its native color effect exactly once.");
        }

        PublishCache();
    }

    internal void StopFrame()
    {
        var errors = new List<Exception>();
        try
        {
            if (!viewCache.Cancel()) errors.Add(new InvalidOperationException("Another owner changed the borrowed map view cache."));
            Array.Clear(drawScopes, 0, drawScopeCount);
            drawScopeCount = 0;
        }
        catch (Exception error)
        {
            errors.Add(error);
        }

        foreach (Auxiliary item in cameras)
        {
            try
            {
                if (item.Camera != null) item.Camera.enabled = false;
            }
            catch (Exception error)
            {
                errors.Add(error);
            }
        }

        try
        {
            RestoreGlobals();
        }
        catch (Exception error)
        {
            errors.Add(error);
        }

        rendering = false;
        if (errors.Count != 0) throw new AggregateException(errors);
    }

    private void RestoreGlobals()
    {
        if (savedGlobals == null) return;

        savedGlobals.Restore();
        savedGlobals = null;
    }

    private void Fault(Exception error)
    {
        finishedFrame = frame;
        try
        {
            StopFrame();
        }
        catch (Exception cleanup)
        {
            error = new AggregateException(error, cleanup);
        }
        finally
        {
            reportFailure(error);
        }
    }

    // Only the host's render-owner retirement path calls this.
    internal void Release()
    {
        if (released) return;
        if (ReferenceEquals(active, this)) Select(null);

        StopFrame();
        if (!TargetsRestored || cameras.Any(c => c.Camera != null && c.Camera.enabled))
        {
            throw new InvalidOperationException("Coverage cameras must be quiescent before texture release.");
        }

        foreach (Auxiliary item in cameras)
        {
            if (item.Camera != null) item.Camera.targetTexture = null;

            if (item.Target != null)
            {
                if (RenderTexture.active == item.Target) throw new InvalidOperationException("Coverage texture is still bound.");
                item.Target.Release();
                UnityEngine.Object.Destroy(item.Target);
                item.Target = null;
            }

            if (item.Owner != null)
            {
                UnityEngine.Object.Destroy(item.Owner);
                item.Owner = null;
            }
        }

        released = true;
        texturePointer = 0;
    }

    private struct Affine
    {
        internal double A;
        internal double B;
        internal double C;
        internal double D;
        internal double E;
        internal double F;
    }

    private sealed class Auxiliary
    {
        internal Camera Camera;
        internal GameObject Owner;
        internal RenderTexture Target;
        internal bool Water;
        internal int PreRenders;
        internal int PostRenders;
    }

    private sealed class Globals
    {
        private readonly Texture water = Shader.GetGlobalTexture("_WaterOutputTex");
        private readonly Vector4 screen = Shader.GetGlobalVector("_MainCameraScreenParams");
        private readonly Matrix4x4 viewProjection = Shader.GetGlobalMatrix("_MainCameraVP");

        internal void Restore()
        {
            var errors = new List<Exception>();

            try
            {
                Shader.SetGlobalTexture("_WaterOutputTex", water);
            }
            catch (Exception error)
            {
                errors.Add(error);
            }

            try
            {
                Shader.SetGlobalVector("_MainCameraScreenParams", screen);
            }
            catch (Exception error)
            {
                errors.Add(error);
            }

            try
            {
                Shader.SetGlobalMatrix("_MainCameraVP", viewProjection);
            }
            catch (Exception error)
            {
                errors.Add(error);
            }

            if (errors.Count != 0) throw new AggregateException(errors);

            if (Shader.GetGlobalTexture("_WaterOutputTex") != water ||
                !Shader.GetGlobalVector("_MainCameraScreenParams").Equals(screen) ||
                !Shader.GetGlobalMatrix("_MainCameraVP").Equals(viewProjection))
            {
                throw new InvalidOperationException("Coverage shader globals did not restore.");
            }
        }
    }

    private struct ViewCacheValue : IEquatable<ViewCacheValue>
    {
        internal CellRect Rect;
        internal int Frame;

        public bool Equals(ViewCacheValue other) =>
            Frame == other.Frame &&
            Rect.minX == other.Rect.minX && Rect.maxX == other.Rect.maxX &&
            Rect.minZ == other.Rect.minZ && Rect.maxZ == other.Rect.maxZ;
    }

    private struct Scope
    {
        internal MapCoverageCapture Capture;
        internal BorrowedViewCache<ViewCacheValue>.Checkpoint Checkpoint;
        internal int DrawScopeCount;
    }

    private static void BeforeMapUpdate(Map __instance, out Scope __state)
    {
        var capture = active;
        if (capture == null || __instance != capture.map)
        {
            __state = default;
            return;
        }

        __state = new Scope
        {
            Capture = capture,
            Checkpoint = capture.viewCache.Mark(),
            DrawScopeCount = capture.drawScopeCount,
        };
    }

    private static Exception AfterMapUpdate(Exception __exception, Scope __state)
    {
        var capture = __state.Capture;
        if (capture == null) return __exception;

        Exception fault = null;

        try
        {
            bool current = capture.viewCache.IsCurrent(__state.Checkpoint);
            if (!capture.viewCache.RestoreAfter(__state.Checkpoint))
            {
                fault = new InvalidOperationException("Another owner changed the borrowed map view cache.");
            }

            if (current)
            {
                if (capture.drawScopeCount > __state.DrawScopeCount)
                {
                    Array.Clear(capture.drawScopes, __state.DrawScopeCount, capture.drawScopeCount - __state.DrawScopeCount);
                }

                capture.drawScopeCount = Math.Min(capture.drawScopeCount, __state.DrawScopeCount);
            }
        }
        catch (Exception error)
        {
            fault = error;
        }

        if (fault != null || (__exception != null && capture.Capturing))
        {
            Exception combined = fault;
            if (__exception != null) combined = fault == null ? __exception : new AggregateException(__exception, fault);
            capture.Fault(combined);
        }

        return __exception;
    }

    private static void BeforeComponents(Map map, out Scope __state)
    {
        __state = default;
        var capture = active;
        if (capture == null || !capture.Capturing || map != capture.map) return;

        __state = new Scope
        {
            Capture = capture,
            Checkpoint = capture.viewCache.Mark(),
            DrawScopeCount = capture.drawScopeCount,
        };

        try
        {
            capture.BorrowView();
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
    }

    private static Exception AfterComponents(Exception __exception, Scope __state) => AfterMapUpdate(__exception, __state);

    private BorrowedViewCache<ViewCacheValue>.Lease BorrowView()
    {
        if (Find.CameraDriver != driver || Find.CurrentMap != map)
        {
            throw new InvalidOperationException("Map view cache owner changed during draw.");
        }

        return viewCache.Enter(new ViewCacheValue { Rect = fullMapView, Frame = Time.frameCount });
    }

    private static void EnterDraw(Map map)
    {
        var capture = active;
        if (capture == null || !capture.Capturing || map != capture.map) return;

        try
        {
            if (capture.drawScopeCount == capture.drawScopes.Length)
            {
                throw new InvalidOperationException("Map draw scope depth exceeded.");
            }

            capture.drawScopes[capture.drawScopeCount++] = capture.BorrowView();
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
    }

    private static void LeaveDraw(Map map)
    {
        var capture = active;
        if (capture == null || map != capture.map || capture.drawScopeCount == 0) return;

        try
        {
            int index = capture.drawScopeCount - 1;
            bool clean = capture.viewCache.Exit(capture.drawScopes[index]);
            capture.drawScopes[index] = default;
            capture.drawScopeCount = index;
            if (!clean) throw new InvalidOperationException("Another owner changed the borrowed map view cache.");
        }
        catch (Exception error)
        {
            capture.Fault(error);
        }
    }

    private static IEnumerable<CodeInstruction> ScopeDraw(IEnumerable<CodeInstruction> instructions)
    {
        var code = instructions.ToList();
        int[] starts = code.Select((item, index) => new { item, index }).Where(x => x.item.Calls(DrawStart)).Select(x => x.index).ToArray();
        int[] ends = code.Select((item, index) => new { item, index }).Where(x => x.item.Calls(DrawEnd)).Select(x => x.index).ToArray();

        if (starts.Length != 1 || ends.Length != 1 || starts[0] >= ends[0])
        {
            throw new InvalidOperationException("The normal map draw boundaries changed.");
        }

        for (int i = 0; i < code.Count; ++i)
        {
            if (i == starts[0])
            {
                if (code[i].blocks.Count != 0)
                {
                    throw new InvalidOperationException("Unexpected exception boundary in the normal map draw.");
                }

                // Branch targets move onto the injected load so a jump here still runs EnterDraw.
                var load = new CodeInstruction(OpCodes.Ldarg_0);
                load.labels.AddRange(code[i].labels);
                code[i].labels.Clear();
                yield return load;
                yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(MapCoverageCapture), nameof(EnterDraw)));
            }

            yield return code[i];

            if (i == ends[0])
            {
                yield return new CodeInstruction(OpCodes.Ldarg_0);
                yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(MapCoverageCapture), nameof(LeaveDraw)));
            }
        }
    }
}

public sealed class CoverageColorEffect : MonoBehaviour
{
    private Camera sourceCamera;
    private MonoBehaviour source;
    private readonly List<MonoBehaviour> components = new List<MonoBehaviour>();
    private readonly Dictionary<Type, bool> imageEffectTypes = new Dictionary<Type, bool>();
    private Compatibility.AntiAliasing antiAliasing;
    private bool antiAliasingActive;
    private bool antiAliasingFirst;
    private Material material;
    private int frame = -1;
    private int calls;
    private int completed = -1;

    private static object ReadField(object value, string name) => AccessTools.Field(value.GetType(), name).GetValue(value);

    internal void Bind(Camera camera)
    {
        sourceCamera = camera;
        RefreshEffects();
    }

    private bool IsImageEffect(Type type)
    {
        if (!imageEffectTypes.TryGetValue(type, out bool result))
        {
            result = AccessTools.Method(type, "OnRenderImage", new[] { typeof(RenderTexture), typeof(RenderTexture) }) != null;
            imageEffectTypes.Add(type, result);
        }

        return result;
    }

    private void RefreshEffects()
    {
        sourceCamera.GetComponents(components);
        int colorIndex = -1;
        int aaIndex = -1;
        bool unsupported = false;

        for (int i = 0; i < components.Count; i++)
        {
            MonoBehaviour component = components[i];
            if (component == null)
            {
                continue;
            }

            Type type = component.GetType();
            if (type.FullName == Compatibility.AntiAliasing.ControllerName)
            {
                if (aaIndex >= 0)
                {
                    unsupported = true;
                }

                aaIndex = i;

                if (antiAliasing == null || !antiAliasing.Matches(component))
                {
                    antiAliasing = new Compatibility.AntiAliasing(component);
                }

                // SSAA disables its image effect but still replaces the original camera target.
                antiAliasing.Validate();
            }
            else if (component.isActiveAndEnabled && IsImageEffect(type))
            {
                if (type.FullName == "UnityStandardAssets.ImageEffects.ColorCorrectionCurves" && colorIndex < 0)
                {
                    colorIndex = i;
                }
                else
                {
                    unsupported = true;
                }
            }
        }

        if (unsupported || colorIndex < 0)
        {
            string names = string.Join(", ", components.Where(x => x != null && x.isActiveAndEnabled && IsImageEffect(x.GetType()))
                .Select(x => x.GetType().FullName));
            throw new InvalidOperationException("Unsupported map coverage image effects: " + names +
                ". Expected ColorCorrectionCurves with optional remi.antialiasing post-processing.");
        }

        source = components[colorIndex];
        antiAliasingActive = aaIndex >= 0 && components[aaIndex].isActiveAndEnabled;
        antiAliasingFirst = aaIndex < colorIndex;
        if (aaIndex < 0)
        {
            antiAliasing = null;
        }
    }

    internal bool Refresh()
    {
        RefreshEffects();
        if (source == null || !source.isActiveAndEnabled || (bool)ReadField(source, "useDepthCorrection") || (bool)ReadField(source, "selectiveCc"))
        {
            throw new InvalidOperationException("Map coverage requires the simple native color correction path.");
        }

        var original = ReadField(source, "ccMaterial") as Material;
        var lut = ReadField(source, "rgbChannelTex") as Texture;

        if ((bool)ReadField(source, "updateTexturesOnStartup") || original == null || lut == null) return false;
        if (!original.shader.isSupported || original.passCount != 1)
        {
            throw new InvalidOperationException("Unsupported native color correction material.");
        }

        if (material == null)
        {
            material = new Material(original) { hideFlags = HideFlags.HideAndDontSave, name = "SMF map coverage color correction" };
        }
        else
        {
            if (material.shader != original.shader) throw new InvalidOperationException("Native color correction shader changed.");
            material.CopyPropertiesFromMaterial(original);
        }

        material.SetTexture("_RgbTex", lut);
        material.SetFloat("_Saturation", (float)ReadField(source, "saturation"));

        return true;
    }

    internal void Validate() => antiAliasing?.Validate();

    internal void BeginFrame(int currentFrame)
    {
        frame = currentFrame;
        calls = 0;
        completed = -1;
    }

    internal bool Completed(int expected) => calls == 1 && completed == expected;

    private void OnRenderImage(RenderTexture input, RenderTexture output)
    {
        if (material == null || frame != Time.frameCount || calls != 0)
        {
            throw new InvalidOperationException("Unexpected map coverage image effect dispatch.");
        }

        ++calls;
        RenderTexture previous = RenderTexture.active;
        RenderTexture intermediate = null;
        try
        {
            if (!antiAliasingActive)
            {
                Graphics.Blit(input, output, material);
            }
            else
            {
                RenderTextureDescriptor descriptor = input.descriptor;
                descriptor.depthBufferBits = 0;
                descriptor.msaaSamples = 1;
                intermediate = RenderTexture.GetTemporary(descriptor);

                // Match the original component order, without creating another mod controller.
                if (antiAliasingFirst)
                {
                    antiAliasing.Render(input, intermediate);
                    Graphics.Blit(intermediate, output, material);
                }
                else
                {
                    Graphics.Blit(input, intermediate, material);
                    antiAliasing.Render(intermediate, output);
                }
            }

            completed = Time.frameCount;
        }
        finally
        {
            RenderTexture.active = previous;
            if (intermediate != null)
            {
                RenderTexture.ReleaseTemporary(intermediate);
            }
        }
    }

    private void OnDestroy()
    {
        if (material != null) Destroy(material);
    }
}
