using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace SimplyMoreFPS.Rendering;

internal sealed class SpaceDebrisCapture
{
    private readonly Camera source;
    private readonly MapCoverageCapture coverage;
    private readonly SpaceDebrisDraws commands;
    private readonly GameObject liveOwner;
    private readonly GameObject cacheOwner;
    private readonly Camera liveCamera;
    private readonly Camera cacheCamera;
    private readonly CommandBuffer buffer;
    private readonly Material add;
    private readonly Texture2D offset;
    private readonly RenderTexture liveDepthTarget;
    private readonly RenderTexture cacheDepthTarget;
    private readonly List<Group> groups = new List<Group>();
    private readonly List<OrderedDraw> ordered = new List<OrderedDraw>();
    private readonly List<float> factors = new List<float>();
    private readonly uint cacheWidth;
    private readonly uint cacheHeight;
    private int groupCount;
    private int frame = -1;
    private ulong cachedMapSerial;
    private ulong cacheSerial;
    private Vector3 livePosition;
    private Vector3 cachePosition;
    private bool rendering;
    private bool released;

    internal RenderTexture Reference
    {
        get;
    }
    internal RenderTexture Probe
    {
        get;
    }
    internal RenderTexture Depth
    {
        get;
    }
    internal ulong ReferencePointer
    {
        get;
    }
    internal ulong ProbePointer
    {
        get;
    }
    internal ulong DepthPointer
    {
        get;
    }
    internal bool TargetsRestored => !rendering;

    internal SpaceDebrisCapture(Camera source, MapCoverageCapture coverage, SpaceDebrisDraws commands, Material premultCopy)
    {
        this.source = source;
        this.coverage = coverage;
        this.commands = commands;
        cacheWidth = coverage.Width;
        cacheHeight = coverage.Height;
        try
        {
            Reference = NewTexture("debris reference", source.pixelWidth, source.pixelHeight, RenderTextureFormat.ARGBHalf);
            Probe = NewTexture("debris probe", source.pixelWidth, source.pixelHeight, RenderTextureFormat.ARGBHalf);
            Depth = NewTexture("debris live depth", source.pixelWidth, source.pixelHeight, RenderTextureFormat.RFloat);
            ReferencePointer = NativePointer(Reference);
            ProbePointer = NativePointer(Probe);
            DepthPointer = NativePointer(Depth);
            liveDepthTarget = NewTexture("debris live depth target", source.pixelWidth, source.pixelHeight, RenderTextureFormat.Depth);
            cacheDepthTarget = NewTexture("debris cache depth target", (int)cacheWidth, (int)cacheHeight, RenderTextureFormat.Depth);
            liveOwner = new GameObject("SMF live debris camera") { hideFlags = HideFlags.HideAndDontSave };
            cacheOwner = new GameObject("SMF cached debris camera") { hideFlags = HideFlags.HideAndDontSave };
            UnityEngine.Object.DontDestroyOnLoad(liveOwner);
            UnityEngine.Object.DontDestroyOnLoad(cacheOwner);
            liveCamera = liveOwner.AddComponent<Camera>();
            cacheCamera = cacheOwner.AddComponent<Camera>();
            liveCamera.enabled = false;
            cacheCamera.enabled = false;
            SpaceDebrisDraws.Exclude(liveCamera);
            SpaceDebrisDraws.Exclude(cacheCamera);
            buffer = new CommandBuffer { name = "SMF orbital debris" };
            add = new Material(premultCopy) { name = "SMF debris probe copy", hideFlags = HideFlags.HideAndDontSave };
            offset = new Texture2D(1, 1, TextureFormat.RGBAHalf, false, true)
            {
                name = "SMF debris probe offset",
                hideFlags = HideFlags.HideAndDontSave
            };
            offset.SetPixel(0, 0, new Color(1, 1, 1, 0));
            offset.Apply(false, true);
        }
        catch
        {
            Release();
            throw;
        }
    }

    internal void Capture(RenderTexture background)
    {
        if (released)
            throw new ObjectDisposedException(nameof(SpaceDebrisCapture));
        if (rendering)
            throw new InvalidOperationException("Recursive orbital debris capture.");
        if (frame == Time.frameCount)
            return;
        if (background.width != Reference.width || background.height != Reference.height)
            throw new InvalidOperationException("The orbital debris background size changed.");

        ReadDraws();
        RenderTexture previous = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        rendering = true;
        try
        {
            GL.sRGBWrite = false;
            Prepare(liveCamera, source);
            FillCommands(null);
            Graphics.Blit(background, Reference);
            Render(liveCamera, Reference, liveDepthTarget, null);
            Graphics.Blit(liveDepthTarget, Depth);

            Graphics.Blit(background, Probe);
            Graphics.Blit(offset, Probe, add);
            Render(liveCamera, Probe, liveDepthTarget, null);

            bool changed = factors.Count != groupCount;
            for (int i = 0; i < factors.Count && !changed; ++i)
                changed |= groups[i].Parallax != factors[i];
            if (changed || cachedMapSerial != coverage.CacheSerial)
                CaptureCache();
            frame = Time.frameCount;
        }
        finally
        {
            liveCamera.RemoveAllCommandBuffers();
            cacheCamera.RemoveAllCommandBuffers();
            liveCamera.enabled = false;
            cacheCamera.enabled = false;
            RenderTexture.active = previous;
            GL.sRGBWrite = srgb;
            rendering = false;
        }
    }

    private void ReadDraws()
    {
        ordered.Clear();
        factors.Clear();
        IReadOnlyList<SpaceDebrisDraws.Draw> input = commands.ReadFrame();
        livePosition = input.Count == 0 ? source.transform.position : input[0].CameraPosition;
        for (int i = 0; i < input.Count; ++i)
        {
            SpaceDebrisDraws.Draw draw = input[i];
            if ((source.cullingMask & (1 << draw.Layer)) == 0)
                continue;
            if (draw.Material == null || draw.Mesh == null || draw.Material.shader == null ||
                draw.Material.shader.name != "Custom/OrbitalDebris" || !draw.Material.shader.isSupported ||
                draw.Material.passCount != 1 || draw.Material.renderQueue != 1999 || draw.CameraPosition != livePosition)
            {
                throw new InvalidOperationException("Orbital debris changed its native material or camera contract.");
            }

            float distance = -source.worldToCameraMatrix.MultiplyPoint3x4(draw.Matrix.MultiplyPoint3x4(draw.Mesh.bounds.center)).z;
            if (float.IsNaN(distance) || float.IsInfinity(distance))
                throw new InvalidOperationException("Orbital debris has a nonfinite draw distance.");
            ordered.Add(new OrderedDraw { Draw = draw, Distance = distance, Order = i });
            if (!factors.Contains(draw.Parallax))
                factors.Add(draw.Parallax);
        }

        ordered.Sort(CompareDraws);
    }

    private static int CompareDraws(OrderedDraw a, OrderedDraw b)
    {
        int order = a.Distance.CompareTo(b.Distance);
        return order != 0 ? order : a.Order.CompareTo(b.Order);
    }

    private void FillCommands(float? factor)
    {
        buffer.Clear();
        foreach (OrderedDraw item in ordered)
        {
            if (factor.HasValue && item.Draw.Parallax != factor.Value)
                continue;
            buffer.DrawMesh(item.Draw.Mesh, item.Draw.Matrix, item.Draw.Material, 0, 0);
        }
    }

    private static void Prepare(Camera camera, Camera original)
    {
        camera.CopyFrom(original);
        camera.enabled = false;
        camera.RemoveAllCommandBuffers();
        camera.transform.SetPositionAndRotation(original.transform.position, original.transform.rotation);
        camera.worldToCameraMatrix = original.worldToCameraMatrix;
        camera.projectionMatrix = original.projectionMatrix;
        camera.cullingMatrix = camera.projectionMatrix * camera.worldToCameraMatrix;
        camera.rect = new Rect(0, 0, 1, 1);
        camera.cullingMask = 0;
    }

    private void Render(Camera camera, RenderTexture color, RenderTexture depth, Color? clear)
    {
        camera.SetTargetBuffers(color.colorBuffer, depth.depthBuffer);
        camera.clearFlags = clear.HasValue ? CameraClearFlags.SolidColor : CameraClearFlags.Depth;
        if (clear.HasValue)
            camera.backgroundColor = clear.Value;
        camera.AddCommandBuffer(CameraEvent.AfterEverything, buffer);
        try
        {
            camera.Render();
        }
        finally
        {
            camera.RemoveCommandBuffer(CameraEvent.AfterEverything, buffer);
        }
    }

    private void CaptureCache()
    {
        if (factors.Count > (ScenePackets.MaximumImages - 17) / 3)
            throw new InvalidOperationException("Too many orbital debris parallax groups.");

        while (groups.Count < factors.Count)
        {
            var group = new Group();
            groups.Add(group);
            group.Black = NewTexture("debris cache black", (int)cacheWidth, (int)cacheHeight, RenderTextureFormat.ARGBHalf);
            group.White = NewTexture("debris cache white", (int)cacheWidth, (int)cacheHeight, RenderTextureFormat.ARGBHalf);
            group.Depth = NewTexture("debris cache depth", (int)cacheWidth, (int)cacheHeight, RenderTextureFormat.RFloat);
            group.BlackPointer = NativePointer(group.Black);
            group.WhitePointer = NativePointer(group.White);
            group.DepthPointer = NativePointer(group.Depth);
        }

        Prepare(cacheCamera, coverage.CoverageCamera);
        FitCache();
        for (int i = 0; i < factors.Count; ++i)
        {
            Group group = groups[i];
            group.Parallax = factors[i];
            FillCommands(group.Parallax);
            Render(cacheCamera, group.Black!, cacheDepthTarget, Color.clear);
            Graphics.Blit(cacheDepthTarget, group.Depth);
            Render(cacheCamera, group.White!, cacheDepthTarget, Color.white);
        }

        groupCount = factors.Count;
        cachePosition = livePosition;
        cachedMapSerial = coverage.CacheSerial;
        cacheSerial = checked((ulong)Time.frameCount);
    }

    private void FitCache()
    {
        if (ordered.Count == 0)
            return;
        Matrix4x4 matrix = cacheCamera.projectionMatrix * cacheCamera.worldToCameraMatrix;
        float minX = float.PositiveInfinity, minY = float.PositiveInfinity;
        float maxX = float.NegativeInfinity, maxY = float.NegativeInfinity;
        foreach (OrderedDraw item in ordered)
        {
            Bounds bounds = item.Draw.Mesh.bounds;
            for (int i = 0; i < 8; ++i)
            {
                Vector3 corner = bounds.center + Vector3.Scale(bounds.extents,
                    new Vector3((i & 1) == 0 ? -1 : 1, (i & 2) == 0 ? -1 : 1, (i & 4) == 0 ? -1 : 1));
                Vector3 projected = matrix.MultiplyPoint3x4(item.Draw.Matrix.MultiplyPoint3x4(corner));
                minX = Math.Min(minX, projected.x);
                maxX = Math.Max(maxX, projected.x);
                minY = Math.Min(minY, projected.y);
                maxY = Math.Max(maxY, projected.y);
            }
        }

        float spanX = maxX - minX, spanY = maxY - minY;
        if (!(spanX > 0 && spanY > 0) || float.IsInfinity(spanX) || float.IsInfinity(spanY))
            throw new InvalidOperationException("Invalid orbital debris cache bounds.");
        float padX = spanX * 2 / cacheWidth, padY = spanY * 2 / cacheHeight;
        minX -= padX;
        maxX += padX;
        minY -= padY;
        maxY += padY;
        Matrix4x4 remap = Matrix4x4.identity;
        remap.m00 = 2 / (maxX - minX);
        remap.m03 = -(maxX + minX) / (maxX - minX);
        remap.m11 = 2 / (maxY - minY);
        remap.m13 = -(maxY + minY) / (maxY - minY);
        cacheCamera.projectionMatrix = remap * cacheCamera.projectionMatrix;
        cacheCamera.cullingMatrix = cacheCamera.projectionMatrix * cacheCamera.worldToCameraMatrix;
    }

    internal void Describe(ScenePacketBuffer packet, uint backgroundImage, uint referenceImage)
    {
        if (released || frame != Time.frameCount)
            throw new InvalidOperationException("Orbital debris did not complete this frame.");
        ScenePackets.ImageFlags flags = SystemInfo.graphicsUVStartsAtTop ? ScenePackets.ImageFlags.FlipY : ScenePackets.ImageFlags.None;
        ScenePackets.ImageFlags depthFlags = flags | ScenePackets.ImageFlags.Depth;
        if (SystemInfo.usesReversedZBuffer)
            depthFlags |= ScenePackets.ImageFlags.ReversedDepth;
        for (int i = 0; i < groupCount; ++i)
        {
            Group group = groups[i];
            var layer = new ScenePackets.Layer
            {
                Kind = ScenePackets.LayerKind.ParallaxMap,
                Color = packet.AddImage(group.BlackPointer, cacheSerial, cacheWidth, cacheHeight, flags),
                Probe = packet.AddImage(group.WhitePointer, cacheSerial, cacheWidth, cacheHeight, flags),
                Reference = ScenePackets.NoImage,
                Depth = packet.AddImage(group.DepthPointer, cacheSerial, cacheWidth, cacheHeight, depthFlags),
                Flags = ScenePackets.LayerFlags.DepthOcclusion,
                SourceX = cachePosition.x,
                SourceZ = cachePosition.z,
                ParallaxX = group.Parallax,
                ParallaxZ = group.Parallax
            };
            SceneCaptureGeometry.SetProjection(cacheCamera, cacheWidth, cacheHeight, ref layer);
            SceneCaptureGeometry.SetDepth(cacheCamera, ref layer);
            packet.AddLayer(layer);
        }

        if (groupCount == 0)
            return;
        uint width = (uint)Reference.width, height = (uint)Reference.height;
        var live = new ScenePackets.Layer
        {
            Kind = ScenePackets.LayerKind.LiveParallaxMap,
            Color = referenceImage,
            Probe = packet.AddImage(ProbePointer, checked((ulong)frame), width, height, flags),
            Reference = backgroundImage,
            Depth = packet.AddImage(DepthPointer, checked((ulong)frame), width, height, depthFlags),
            Flags = ScenePackets.LayerFlags.DepthOcclusion,
            SourceX = livePosition.x,
            SourceZ = livePosition.z
        };
        SceneCaptureGeometry.SetProjection(liveCamera, width, height, ref live);
        SceneCaptureGeometry.SetDepth(liveCamera, ref live);
        packet.AddLayer(live);
    }

    private static RenderTexture NewTexture(string name, int width, int height, RenderTextureFormat format)
    {
        var texture = new RenderTexture(width, height, format == RenderTextureFormat.Depth ? 32 : 0, format, RenderTextureReadWrite.Linear)
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
        throw new InvalidOperationException("Orbital debris texture creation failed: " + name);
    }

    private static ulong NativePointer(RenderTexture texture)
    {
        ulong pointer = unchecked((ulong)texture.GetNativeTexturePtr().ToInt64());
        if (pointer == 0)
            throw new InvalidOperationException("An orbital debris texture has no native resource.");
        return pointer;
    }

    internal void Release()
    {
        if (released)
            return;
        if (rendering)
            throw new InvalidOperationException("Orbital debris capture is still rendering.");
        if (liveCamera != null)
        {
            SpaceDebrisDraws.Forget(liveCamera);
            liveCamera.targetTexture = null;
        }
        if (cacheCamera != null)
        {
            SpaceDebrisDraws.Forget(cacheCamera);
            cacheCamera.targetTexture = null;
        }
        UnityEngine.Object.Destroy(liveOwner);
        UnityEngine.Object.Destroy(cacheOwner);
        buffer?.Release();
        UnityEngine.Object.Destroy(add);
        UnityEngine.Object.Destroy(offset);
        ReleaseTexture(Reference);
        ReleaseTexture(Probe);
        ReleaseTexture(Depth);
        ReleaseTexture(liveDepthTarget);
        ReleaseTexture(cacheDepthTarget);
        foreach (Group group in groups)
        {
            ReleaseTexture(group.Black);
            ReleaseTexture(group.White);
            ReleaseTexture(group.Depth);
            group.Black = group.White = group.Depth = null;
        }
        groups.Clear();
        released = true;
    }

    private static void ReleaseTexture(RenderTexture? texture)
    {
        if (texture == null)
            return;
        texture.Release();
        UnityEngine.Object.Destroy(texture);
    }

    private struct OrderedDraw
    {
        internal SpaceDebrisDraws.Draw Draw;
        internal float Distance;
        internal int Order;
    }

    private sealed class Group
    {
        internal RenderTexture? Black, White, Depth;
        internal ulong BlackPointer, WhitePointer, DepthPointer;
        internal float Parallax;
    }
}
