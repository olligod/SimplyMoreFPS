using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using HarmonyLib;
using RimWorld.Planet;
using UnityEngine;
using UnityEngine.Rendering;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal sealed class SpaceBackgroundCapture
{
    private const float ProjectionScale = 1.25f;
    private static readonly FieldInfo GlowMeshes = AccessTools.Field(typeof(WorldDrawLayerBase), "subMeshes");
    private readonly Map map;
    private readonly Camera source;
    private readonly Camera skySource;
    private readonly GameObject worldOwner;
    private readonly GameObject skyOwner;
    private readonly GameObject worldStateOwner;
    private readonly GameObject skyStateOwner;
    private readonly Camera worldCamera;
    private readonly Camera skyCamera;
    private readonly Camera worldState;
    private readonly Camera skyState;
    private readonly RenderTexture depthTarget;
    private readonly RenderTexture sky;
    private readonly RenderTexture liveSky;
    private readonly CommandBuffer skyCopy;
    private readonly View?[] views = new View?[4];
    private readonly ulong liveSkyPointer;
    private readonly List<GlowState> liveGlows = new List<GlowState>();
    private readonly List<GlowState> cachedGlows = new List<GlowState>();
    private Matrix4x4 sourceProjection;
    private Matrix4x4 skyProjection;
    private Quaternion sourceRotation;
    private Quaternion skyRotation;
    private PlanetTile tile;
    private Vector3 layerOrigin;
    private Vector3 renderedMapPosition;
    private int worldFrame = -1;
    private int skyFrame = -1;
    private int liveSkyFrame = -1;
    private Exception? captureError;
    private long nextRefresh;
    private bool rendering;
    private bool selected;
    private bool released;

    internal ulong Serial
    {
        get; private set;
    }

    internal bool TargetsRestored => !rendering;

    internal SpaceBackgroundCapture(Map map, Camera source, Camera skySource)
    {
        this.map = map;
        this.source = source;
        this.skySource = skySource;
        try
        {
            CheckSource();
            double scale = Math.Min(1, 2048.0 / Math.Max(source.pixelWidth, source.pixelHeight));
            int width = Math.Max(1, (int)Math.Round(source.pixelWidth * scale));
            int height = Math.Max(1, (int)Math.Round(source.pixelHeight * scale));
            sky = NewTexture("wide space sky", width, height, RenderTextureFormat.ARGBHalf, 0);
            liveSky = NewTexture("live space sky", source.pixelWidth, source.pixelHeight, RenderTextureFormat.ARGBHalf, 0);
            liveSkyPointer = NativePointer(liveSky);
            skyCopy = new CommandBuffer { name = "SMF live space sky" };
            skyCopy.Blit(BuiltinRenderTextureType.CurrentActive, liveSky);
            depthTarget = NewTexture("space depth target", width, height, RenderTextureFormat.Depth, 24);
            for (int i = 0; i < views.Length; ++i)
            {
                var view = new View();
                views[i] = view;
                view.Color = NewTexture("space background " + i, width, height, RenderTextureFormat.ARGBHalf, 0);
                view.Depth = NewTexture("space depth " + i, width, height, RenderTextureFormat.RFloat, 0);
                view.ColorPointer = NativePointer(view.Color);
                view.DepthPointer = NativePointer(view.Depth);
            }

            worldOwner = NewOwner("SMF space world camera");
            skyOwner = NewOwner("SMF space sky camera");
            worldCamera = worldOwner.AddComponent<Camera>();
            skyCamera = skyOwner.AddComponent<Camera>();
            worldCamera.enabled = false;
            skyCamera.enabled = false;
            worldStateOwner = NewOwner("SMF space world camera state");
            skyStateOwner = NewOwner("SMF space sky camera state");
            worldState = worldStateOwner.AddComponent<Camera>();
            skyState = skyStateOwner.AddComponent<Camera>();
            worldState.enabled = false;
            skyState.enabled = false;
            Camera.onPreRender += RecordCamera;
            Camera.onPostRender += SkyRendered;
        }
        catch
        {
            Release();
            throw;
        }
    }

    private void SkyRendered(Camera camera)
    {
        if (camera == skySource && selected && !released)
            liveSkyFrame = Time.frameCount;
    }

    internal void Select(bool value)
    {
        if (selected == value || released)
            return;
        if (value)
            skySource.AddCommandBuffer(CameraEvent.AfterEverything, skyCopy);
        else if (skySource != null)
            skySource.RemoveCommandBuffer(CameraEvent.AfterEverything, skyCopy);
        selected = value;
    }

    private void CheckSource()
    {
        if (released || map.Disposed || Find.CurrentMap != map || source == null || skySource == null ||
            source.orthographic || skySource.orthographic || !source.enabled || !skySource.enabled ||
            source.targetTexture != null || skySource.targetTexture != null ||
            source.rect != new Rect(0, 0, 1, 1) || skySource.rect != source.rect ||
            map.Size.x <= 4 || map.Size.z <= 4)
        {
            throw new InvalidOperationException("Space capture requires the original perspective background cameras.");
        }
    }

    private void RecordCamera(Camera camera)
    {
        if (!selected || released || map.Disposed || Find.CurrentMap != map || (camera != source && camera != skySource))
            return;

        try
        {
            // WorldCameraDriver deliberately restores a different pose during OnGUI.
            Camera state = camera == source ? worldState : skyState;
            state.CopyFrom(camera);
            state.enabled = false;
            state.RemoveAllCommandBuffers();
            state.transform.SetPositionAndRotation(camera.transform.position, camera.transform.rotation);
            state.worldToCameraMatrix = camera.worldToCameraMatrix;
            state.projectionMatrix = camera.projectionMatrix;
            if (camera == source)
            {
                ReadGlows();
                renderedMapPosition = Find.Camera.transform.position;
                worldFrame = Time.frameCount;
            }
            else
            {
                skyFrame = Time.frameCount;
            }
        }
        catch (Exception error)
        {
            captureError = error;
        }
    }

    internal bool Capture()
    {
        CheckSource();
        if (captureError != null)
            throw new InvalidOperationException("The rendered space camera state was not captured.", captureError);
        if (worldFrame != Time.frameCount || skyFrame != Time.frameCount || liveSkyFrame != Time.frameCount)
            return false;

        Matrix4x4 projection = worldState.projectionMatrix;
        Matrix4x4 nextSkyProjection = skyState.projectionMatrix;
        Quaternion rotation = worldState.transform.rotation;
        Quaternion nextSkyRotation = skyState.transform.rotation;
        if (Serial != 0 && Stopwatch.GetTimestamp() < nextRefresh && tile == map.Tile &&
            layerOrigin == map.Tile.Layer.Origin && sourceProjection == projection && sourceRotation == rotation &&
            skyProjection == nextSkyProjection && skyRotation == nextSkyRotation && SameGlowGeometry())
        {
            return true;
        }

        if (rendering)
            throw new InvalidOperationException("Recursive space background capture.");
        RenderTexture previous = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        rendering = true;
        try
        {
            ValidateGlowMeshes();
            Prepare(skyCamera, skyState, sky);
            skyCamera.Render();

            Vector3 position = renderedMapPosition;
            double sourceX = Math.Max(2, Math.Min(map.Size.x - 2, position.x));
            double sourceZ = Math.Max(2, Math.Min(map.Size.z - 2, position.z));
            ReadParallax(out double perX, out double perZ);
            for (int i = 0; i < views.Length; ++i)
            {
                View view = views[i]!;
                view.X = (i & 1) == 0 ? 2 : map.Size.x - 2;
                view.Z = (i & 2) == 0 ? 2 : map.Size.z - 2;
                Vector3 displacement = worldState.transform.right * (float)((view.X - sourceX) * perX) +
                    worldState.transform.up * (float)((view.Z - sourceZ) * perZ);

                Graphics.Blit(sky, view.Color);
                Prepare(worldCamera, worldState, view.Color!);
                worldCamera.transform.position = worldState.transform.position + displacement;
                worldCamera.worldToCameraMatrix = worldState.worldToCameraMatrix * Matrix4x4.Translate(-displacement);
                worldCamera.cullingMatrix = worldCamera.projectionMatrix * worldCamera.worldToCameraMatrix;
                worldCamera.Render();
                Graphics.Blit(depthTarget, view.Depth);
                view.Projection = worldCamera.projectionMatrix;
            }

            sourceProjection = projection;
            sourceRotation = rotation;
            skyProjection = nextSkyProjection;
            skyRotation = nextSkyRotation;
            tile = map.Tile;
            layerOrigin = map.Tile.Layer.Origin;
            cachedGlows.Clear();
            cachedGlows.AddRange(liveGlows);
            Serial = checked((ulong)Time.frameCount);
            nextRefresh = Stopwatch.GetTimestamp() + 2 * Stopwatch.Frequency;
        }
        finally
        {
            worldCamera.enabled = false;
            skyCamera.enabled = false;
            RenderTexture.active = previous;
            GL.sRGBWrite = srgb;
            rendering = false;
        }

        return true;
    }

    private void ReadGlows()
    {
        liveGlows.Clear();
        Vector3 sun = Shader.GetGlobalVector(ShaderPropertyIDs.PlanetSunLightDirection);
        foreach (WorldDrawLayerBase layer in Find.World.renderer.AllVisibleDrawLayers)
        {
            if (!(layer is WorldDrawLayer_Glow) || !layer.Visible)
                continue;
            if (!(GlowMeshes.GetValue(layer) is List<LayerSubMesh> meshes))
                throw new InvalidOperationException("The world glow meshes are unavailable.");

            foreach (LayerSubMesh item in meshes)
            {
                if (!item.finalized)
                    continue;
                Material material = item.material;
                if (liveGlows.Count == ScenePackets.MaximumBackgroundGlows || material == null ||
                    material.shader != ShaderDatabase.PlanetGlow || material.shader.name != "Custom/PlanetGlow" ||
                    material.passCount != 1 || material.renderQueue != 3001 || item.mesh == null || item.verts.Count == 0)
                {
                    throw new InvalidOperationException("The world glow does not use a supported spherical material.");
                }

                var glow = new GlowState
                {
                    Mesh = item.mesh,
                    Vertices = item.verts,
                    MeshCenter = layer.Position,
                    MeshRadius = item.verts[0].magnitude,
                    PlanetOrigin = material.GetVector("_PlanetOrigin"),
                    PlanetRadius = material.GetFloat("_PlanetRadius"),
                    GlowRadius = material.GetFloat("_GlowRadius"),
                    Intensity = material.GetFloat("_Intensity"),
                    Sun = sun
                };
                if (!Finite(glow.MeshCenter) || !Finite(glow.PlanetOrigin) || !Finite(glow.Sun) ||
                    !Finite(glow.MeshRadius) || glow.MeshRadius <= 0 ||
                    !Finite(glow.PlanetRadius) || glow.PlanetRadius <= 0 ||
                    !Finite(glow.GlowRadius) || glow.GlowRadius <= 0 || !Finite(glow.Intensity))
                {
                    throw new InvalidOperationException("The world glow contains invalid material values.");
                }
                liveGlows.Add(glow);
            }
        }
    }

    private bool SameGlowGeometry()
    {
        if (liveGlows.Count != cachedGlows.Count)
            return false;
        for (int i = 0; i < liveGlows.Count; ++i)
        {
            GlowState live = liveGlows[i];
            GlowState cached = cachedGlows[i];
            if (live.Mesh != cached.Mesh || live.MeshCenter != cached.MeshCenter || live.MeshRadius != cached.MeshRadius ||
                live.PlanetOrigin != cached.PlanetOrigin || live.PlanetRadius != cached.PlanetRadius ||
                live.GlowRadius != cached.GlowRadius)
                return false;
        }
        return true;
    }

    private void ValidateGlowMeshes()
    {
        foreach (GlowState glow in liveGlows)
        {
            if (glow.Mesh.vertexCount != glow.Vertices.Count)
                throw new InvalidOperationException("The world glow mesh changed outside its layer.");
            foreach (Vector3 vertex in glow.Vertices)
            {
                if (!Finite(vertex) || Math.Abs(vertex.magnitude - glow.MeshRadius) > glow.MeshRadius * .0001f)
                    throw new InvalidOperationException("The world glow requires a spherical mesh.");
            }
        }
    }

    private static bool Finite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);

    private static bool Finite(Vector3 value) => Finite(value.x) && Finite(value.y) && Finite(value.z);

    private void Prepare(Camera camera, Camera original, RenderTexture color)
    {
        camera.CopyFrom(original);
        camera.enabled = false;
        camera.RemoveAllCommandBuffers();
        camera.transform.SetPositionAndRotation(original.transform.position, original.transform.rotation);
        camera.rect = new Rect(0, 0, 1, 1);
        camera.aspect = (float)color.width / color.height;

        Matrix4x4 scale = Matrix4x4.identity;
        scale.m00 = scale.m11 = 1 / ProjectionScale;
        camera.worldToCameraMatrix = original.worldToCameraMatrix;
        camera.projectionMatrix = scale * original.projectionMatrix;
        camera.cullingMatrix = camera.projectionMatrix * camera.worldToCameraMatrix;
        camera.SetTargetBuffers(color.colorBuffer, depthTarget.depthBuffer);
    }

    private void ReadParallax(out double perX, out double perZ)
    {
        float distance = map.Tile.Layer.BackgroundWorldCameraParallaxDistancePer100Cells;
        float xScale = 1;
        float zScale = 1;
        if (map.Size.x > map.Size.z)
        {
            zScale = (float)map.Size.z / map.Size.x;
            distance *= map.Size.x / 100f;
        }
        else if (map.Size.z > map.Size.x)
        {
            xScale = (float)map.Size.x / map.Size.z;
            distance *= map.Size.z / 100f;
        }

        perX = distance * xScale / (map.Size.x - 4);
        perZ = distance * zScale / (map.Size.z - 4);
    }

    internal unsafe ScenePackets.Background Describe(ScenePacketBuffer packet, uint liveColor)
    {
        if (released || Serial == 0)
            throw new InvalidOperationException("The space background is not ready.");

        Vector3 position = renderedMapPosition;
        ReadParallax(out double perX, out double perZ);
        ScenePackets.ImageFlags flags = SystemInfo.graphicsUVStartsAtTop
            ? ScenePackets.ImageFlags.FlipY : ScenePackets.ImageFlags.None;
        var result = new ScenePackets.Background
        {
            LiveColor = liveColor,
            Sky = packet.AddImage(liveSkyPointer, checked((ulong)liveSkyFrame),
                (uint)liveSky.width, (uint)liveSky.height, flags),
            ViewCount = 4,
            Flags = SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.OpenGLCore ? 1u : 0u,
            SourceX = position.x,
            SourceZ = position.z,
            MinX = 2,
            MaxX = map.Size.x - 2,
            MinZ = 2,
            MaxZ = map.Size.z - 2,
            CameraXPerCell = perX,
            CameraYPerCell = perZ,
            SkyScale = 1,
            View0 = DescribeView(packet, views[0]!, flags),
            View1 = DescribeView(packet, views[1]!, flags),
            View2 = DescribeView(packet, views[2]!, flags),
            View3 = DescribeView(packet, views[3]!, flags)
        };

        CopyProjection(worldState.projectionMatrix, result.Projection);
        result.LiveGlowCount = checked((uint)liveGlows.Count);
        result.CachedGlowCount = checked((uint)cachedGlows.Count);
        ScenePackets.BackgroundGlow* live = &result.LiveGlow0;
        ScenePackets.BackgroundGlow* cached = &result.CachedGlow0;
        for (int i = 0; i < liveGlows.Count; ++i)
            live[i] = DescribeGlow(liveGlows[i]);
        for (int i = 0; i < cachedGlows.Count; ++i)
            cached[i] = DescribeGlow(cachedGlows[i]);
        return result;
    }

    private unsafe ScenePackets.BackgroundGlow DescribeGlow(GlowState value)
    {
        Matrix4x4 view = worldState.worldToCameraMatrix;
        var result = new ScenePackets.BackgroundGlow
        {
            PlanetRadius = value.PlanetRadius,
            GlowRadius = value.GlowRadius,
            Intensity = value.Intensity,
            MeshRadius = value.MeshRadius
        };
        CopyVector(view.MultiplyPoint3x4(Vector3.zero), result.WorldOrigin);
        CopyVector(view.MultiplyPoint3x4(value.PlanetOrigin), result.PlanetOrigin);
        CopyVector(view.MultiplyVector(value.Sun), result.Sun);
        CopyVector(view.MultiplyPoint3x4(value.MeshCenter), result.MeshCenter);
        return result;
    }

    private static unsafe void CopyVector(Vector3 value, float* output)
    {
        output[0] = value.x;
        output[1] = value.y;
        output[2] = value.z;
    }

    private unsafe ScenePackets.BackgroundView DescribeView(ScenePacketBuffer packet, View view, ScenePackets.ImageFlags flags)
    {
        var result = new ScenePackets.BackgroundView
        {
            Color = packet.AddImage(view.ColorPointer, Serial, (uint)sky.width, (uint)sky.height, flags),
            Depth = packet.AddImage(view.DepthPointer, Serial, (uint)sky.width, (uint)sky.height, flags | ScenePackets.ImageFlags.Depth),
            X = view.X,
            Z = view.Z
        };
        CopyProjection(view.Projection, result.Projection);
        return result;
    }

    private static unsafe void CopyProjection(Matrix4x4 projection, float* output)
    {
        // Texture row orientation is separate from the device's depth range.
        Matrix4x4 device = GL.GetGPUProjectionMatrix(projection, true);
        projection.SetRow(2, device.GetRow(2));
        for (int i = 0; i < 16; ++i)
            output[i] = projection[i];
    }

    private static GameObject NewOwner(string name)
    {
        var owner = new GameObject(name) { hideFlags = HideFlags.HideAndDontSave };
        UnityEngine.Object.DontDestroyOnLoad(owner);
        return owner;
    }

    private static RenderTexture NewTexture(string name, int width, int height, RenderTextureFormat format, int depth)
    {
        var texture = new RenderTexture(width, height, depth, format, RenderTextureReadWrite.Linear)
        {
            name = "SMF " + name,
            hideFlags = HideFlags.HideAndDontSave,
            filterMode = FilterMode.Point,
            wrapMode = TextureWrapMode.Clamp,
            useMipMap = false,
            autoGenerateMips = false
        };

        if (texture.Create())
            return texture;
        UnityEngine.Object.Destroy(texture);
        throw new InvalidOperationException("Space background texture creation failed: " + name);
    }

    private static ulong NativePointer(RenderTexture texture)
    {
        ulong pointer = unchecked((ulong)texture.GetNativeTexturePtr().ToInt64());
        if (pointer == 0)
            throw new InvalidOperationException("A space background texture has no native resource.");
        return pointer;
    }

    internal void Release()
    {
        if (released)
            return;
        if (rendering)
            throw new InvalidOperationException("A space background capture is still rendering.");
        Camera.onPreRender -= RecordCamera;
        Camera.onPostRender -= SkyRendered;
        if (skySource != null && skyCopy != null)
            skySource.RemoveCommandBuffer(CameraEvent.AfterEverything, skyCopy);
        skyCopy?.Release();
        if (worldCamera != null)
            worldCamera.targetTexture = null;
        if (skyCamera != null)
            skyCamera.targetTexture = null;
        UnityEngine.Object.Destroy(worldOwner);
        UnityEngine.Object.Destroy(skyOwner);
        UnityEngine.Object.Destroy(worldStateOwner);
        UnityEngine.Object.Destroy(skyStateOwner);
        foreach (View? view in views)
        {
            if (view == null)
                continue;
            ReleaseTexture(view.Color);
            ReleaseTexture(view.Depth);
            view.Color = view.Depth = null;
            view.ColorPointer = view.DepthPointer = 0;
        }
        ReleaseTexture(sky);
        ReleaseTexture(liveSky);
        ReleaseTexture(depthTarget);
        liveGlows.Clear();
        cachedGlows.Clear();
        released = true;
    }

    private struct GlowState
    {
        internal Mesh Mesh;
        internal List<Vector3> Vertices;
        internal Vector3 MeshCenter, PlanetOrigin, Sun;
        internal float MeshRadius, PlanetRadius, GlowRadius, Intensity;
    }

    private static void ReleaseTexture(RenderTexture? texture)
    {
        if (texture == null)
            return;
        texture.Release();
        UnityEngine.Object.Destroy(texture);
    }

    private sealed class View
    {
        internal RenderTexture? Color;
        internal RenderTexture? Depth;
        internal ulong ColorPointer, DepthPointer;
        internal Matrix4x4 Projection;
        internal double X, Z;
    }
}
