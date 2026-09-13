using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal sealed class SpaceMapCapture
{
    private static readonly HashSet<MethodInfo> PatchedEffects = new HashSet<MethodInfo>();
    private static readonly Dictionary<Type, MethodInfo?> EffectMethods = new Dictionary<Type, MethodInfo?>();
    private static Harmony? harmony;
    private static SpaceMapCapture? active;

    private readonly Camera source;
    private readonly Camera backgroundCamera;
    private readonly Func<bool> mayCapture;
    private readonly Action<Exception> reportFailure;
    private readonly Material add;
    private readonly List<MonoBehaviour> components = new List<MonoBehaviour>();
    private readonly Dictionary<object, EffectInput> effectInputs = new Dictionary<object, EffectInput>();
    private readonly CommandBuffer backgroundCopy;
    private CommandBuffer? directCopy;
    private readonly GameObject probeOwner;
    private readonly Camera probeCamera;
    private readonly Texture2D offset;
    private MonoBehaviour? firstEffect;
    private RenderTexture? input;
    private RenderTexture? probe;
    private int backgroundFrame = -1;
    private int inputFrame = -1;
    private int capturedFrame = -1;
    private bool rendering;
    private bool released;

    internal RenderTexture Background
    {
        get;
    }

    internal RenderTexture? Input => input;

    internal RenderTexture? Probe => probe;

    internal ulong BackgroundPointer
    {
        get; private set;
    }

    internal ulong InputPointer
    {
        get; private set;
    }

    internal bool InputFlippedY
    {
        get; private set;
    }

    internal ulong ProbePointer
    {
        get; private set;
    }

    internal bool TargetsRestored => !rendering;

    internal SpaceMapCapture(Camera source, Camera backgroundCamera, Material premultCopy,
        Func<bool> mayCapture, Action<Exception> reportFailure)
    {
        this.source = source;
        this.backgroundCamera = backgroundCamera;
        this.mayCapture = mayCapture;
        this.reportFailure = reportFailure;
        try
        {
            add = new Material(premultCopy)
            {
                name = "SMF background probe copy",
                hideFlags = HideFlags.HideAndDontSave
            };

            // Zero alpha keeps the destination alpha while the premultiplied copy adds RGB.
            offset = new Texture2D(1, 1, TextureFormat.RGBAHalf, false, true)
            {
                name = "SMF background probe offset",
                hideFlags = HideFlags.HideAndDontSave
            };
            offset.SetPixel(0, 0, new Color(1, 1, 1, 0));
            offset.Apply(false, true);

            Background = NewTexture("space background", new RenderTextureDescriptor(
                source.pixelWidth, source.pixelHeight, RenderTextureFormat.ARGBHalf, 0));
            BackgroundPointer = NativePointer(Background);
            backgroundCopy = new CommandBuffer { name = "SMF space background" };
            backgroundCopy.Blit(BuiltinRenderTextureType.CurrentActive, Background);
            Camera.onPostRender += BackgroundRendered;

            probeOwner = new GameObject("SMF map probe camera") { hideFlags = HideFlags.HideAndDontSave };
            UnityEngine.Object.DontDestroyOnLoad(probeOwner);
            probeCamera = probeOwner.AddComponent<Camera>();
            probeCamera.enabled = false;
            SpaceDebrisDraws.Exclude(probeCamera);
            RefreshEffect();
            Camera.onPreCull += SourceBeforeCull;
        }
        catch
        {
            Release();
            throw;
        }
    }

    internal static void InstallHooks(string owner)
    {
        if (harmony != null)
            return;
        harmony = new Harmony(owner + ".space");
    }

    internal static void RemoveHooks()
    {
        Select(null);
        harmony?.UnpatchAll(harmony.Id);
        harmony = null;
        PatchedEffects.Clear();
        EffectMethods.Clear();
    }

    internal static void Select(SpaceMapCapture? capture)
    {
        if (ReferenceEquals(active, capture))
            return;
        if (active != null)
        {
            active.RemoveDirectCopy();
            if (active.backgroundCamera != null)
                active.backgroundCamera.RemoveCommandBuffer(CameraEvent.AfterEverything, active.backgroundCopy);
        }
        active = capture;
        if (active != null)
            active.backgroundCamera.AddCommandBuffer(CameraEvent.AfterEverything, active.backgroundCopy);
    }

    internal void RefreshEffect()
    {
        firstEffect = null;
        source.GetComponents(components);
        try
        {
            foreach (MonoBehaviour component in components)
            {
                if (component == null || !component.isActiveAndEnabled)
                    continue;

                Type type = component.GetType();
                if (!EffectMethods.TryGetValue(type, out MethodInfo? callback))
                {
                    callback = AccessTools.Method(type, "OnRenderImage", new[] { typeof(RenderTexture), typeof(RenderTexture) });
                    EffectMethods.Add(type, callback);
                }
                if (callback == null)
                    continue;

                if (firstEffect == null)
                    firstEffect = component;
                if (!effectInputs.ContainsKey(component))
                    effectInputs.Add(component, default);
                if (!PatchedEffects.Contains(callback))
                {
                    if (harmony == null)
                        throw new InvalidOperationException("Space capture hooks are unavailable.");
                    harmony.Patch(callback, prefix: new HarmonyMethod(typeof(SpaceMapCapture), nameof(BeforeImageEffect)));
                    PatchedEffects.Add(callback);
                }
            }
        }
        finally
        {
            components.Clear();
        }
    }

    private void BackgroundRendered(Camera camera)
    {
        if (!ReferenceEquals(active, this))
            return;
        if (camera == backgroundCamera)
            backgroundFrame = Time.frameCount;
        if (camera == source && directCopy != null && ReferenceEquals(active, this) && mayCapture())
        {
            // A backbuffer blit already has display-oriented rows.
            InputFlippedY = false;
            inputFrame = Time.frameCount;
        }
    }

    private void SourceBeforeCull(Camera camera)
    {
        if (camera != source || released || !ReferenceEquals(active, this) || !mayCapture())
            return;
        try
        {
            RefreshEffect();
            if (firstEffect == null && directCopy == null)
            {
                // Cameras without image effects still need their completed color image.
                EnsureInput(new RenderTextureDescriptor(source.pixelWidth, source.pixelHeight, RenderTextureFormat.ARGBHalf, 24));
                directCopy = new CommandBuffer { name = "SMF map image before GUI" };
                directCopy.Blit(BuiltinRenderTextureType.CurrentActive, input);
                source.AddCommandBuffer(CameraEvent.AfterEverything, directCopy);
            }
            else if (firstEffect != null)
            {
                RemoveDirectCopy();
            }
        }
        catch (Exception error)
        {
            reportFailure(error);
        }
    }

    private static void BeforeImageEffect(object __instance, RenderTexture __0)
    {
        SpaceMapCapture? capture = active;
        if (capture == null || capture.released || !capture.effectInputs.ContainsKey(__instance) || !capture.mayCapture())
            return;

        try
        {
            // Camera modes can change before the host retires this capture.
            if (!MapImageEffects.CanCapture(capture.source, out _))
                return;

            capture.effectInputs[__instance] = new EffectInput
            {
                Frame = Time.frameCount,
                Filter = __0.filterMode,
                Format = __0.format
            };
            if (ReferenceEquals(capture.firstEffect, __instance))
                capture.CopyInput(__0);
        }
        catch (Exception error)
        {
            capture.reportFailure(error);
        }
    }

    internal static bool ReadEffectInput(MonoBehaviour component, out FilterMode filter, out RenderTextureFormat format)
    {
        filter = FilterMode.Point;
        format = RenderTextureFormat.Default;
        if (active == null || !active.effectInputs.TryGetValue(component, out EffectInput input) || input.Frame != Time.frameCount)
            return false;
        filter = input.Filter;
        format = input.Format;
        return true;
    }

    private struct EffectInput
    {
        internal int Frame;
        internal FilterMode Filter;
        internal RenderTextureFormat Format;
    }

    private void CopyInput(RenderTexture original)
    {
        if (inputFrame == Time.frameCount)
            throw new InvalidOperationException("The original map image was captured twice.");

        if (original == null || original.width != Background.width || original.height != Background.height ||
            original.antiAliasing != 1 || original.dimension != TextureDimension.Tex2D)
            throw new InvalidOperationException("The original map image has an unsupported descriptor.");

        if (input != null && input.graphicsFormat != original.graphicsFormat)
        {
            throw new InvalidOperationException("The original map image format changed.");
        }
        EnsureInput(original.descriptor);
        InputFlippedY = SystemInfo.graphicsUVStartsAtTop;

        RenderTexture previous = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        try
        {
            Graphics.Blit(original, input);
            inputFrame = Time.frameCount;
        }
        finally
        {
            RenderTexture.active = previous;
            GL.sRGBWrite = srgb;
        }
    }

    private void EnsureInput(RenderTextureDescriptor descriptor)
    {
        if (input != null)
            return;
        probe = NewTexture("map probe", descriptor);
        descriptor.depthBufferBits = 0;
        input = NewTexture("map image before effects", descriptor);
        InputPointer = NativePointer(input);
        ProbePointer = NativePointer(probe);
    }

    private void RemoveDirectCopy()
    {
        if (directCopy == null)
            return;
        source.RemoveCommandBuffer(CameraEvent.AfterEverything, directCopy);
        directCopy.Release();
        directCopy = null;
    }

    internal bool Capture(RenderTexture? reference = null)
    {
        if (released || !mayCapture() || input == null || probe == null || inputFrame != Time.frameCount ||
            backgroundFrame != Time.frameCount)
            return false;
        if (capturedFrame == Time.frameCount)
            return true;
        if (rendering)
            throw new InvalidOperationException("Recursive map probe rendering.");

        RenderTexture previous = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        rendering = true;
        try
        {
            Graphics.Blit(reference != null ? reference : Background, probe);
            Graphics.Blit(offset, probe, add);

            probeCamera.CopyFrom(source);
            probeCamera.enabled = false;
            probeCamera.RemoveAllCommandBuffers();
            probeCamera.transform.SetPositionAndRotation(source.transform.position, source.transform.rotation);
            probeCamera.targetTexture = probe;
            probeCamera.clearFlags = CameraClearFlags.Depth;
            probeCamera.Render();
            capturedFrame = Time.frameCount;
            return true;
        }
        finally
        {
            probeCamera.enabled = false;
            RenderTexture.active = previous;
            GL.sRGBWrite = srgb;
            rendering = false;
        }
    }

    private static RenderTexture NewTexture(string name, RenderTextureDescriptor descriptor)
    {
        descriptor.msaaSamples = 1;
        descriptor.useMipMap = false;
        descriptor.autoGenerateMips = false;
        var texture = new RenderTexture(descriptor)
        {
            name = "SMF " + name,
            hideFlags = HideFlags.HideAndDontSave,
            filterMode = FilterMode.Point,
            wrapMode = TextureWrapMode.Clamp
        };

        if (texture.Create())
            return texture;
        UnityEngine.Object.Destroy(texture);
        throw new InvalidOperationException("Space capture texture creation failed: " + name);
    }

    private static ulong NativePointer(RenderTexture texture)
    {
        ulong pointer = unchecked((ulong)texture.GetNativeTexturePtr().ToInt64());
        if (pointer == 0)
            throw new InvalidOperationException("A space capture texture has no native resource.");
        return pointer;
    }

    internal void Release()
    {
        if (released)
            return;
        if (rendering)
            throw new InvalidOperationException("A map probe is still rendering.");
        if (ReferenceEquals(active, this))
            active = null;

        Camera.onPostRender -= BackgroundRendered;
        Camera.onPreCull -= SourceBeforeCull;
        RemoveDirectCopy();
        if (backgroundCopy != null)
        {
            if (backgroundCamera != null)
                backgroundCamera.RemoveCommandBuffer(CameraEvent.AfterEverything, backgroundCopy);
            backgroundCopy.Release();
        }

        if (probeCamera != null)
        {
            SpaceDebrisDraws.Forget(probeCamera);
            probeCamera.targetTexture = null;
        }
        UnityEngine.Object.Destroy(probeOwner);
        UnityEngine.Object.Destroy(add);
        UnityEngine.Object.Destroy(offset);
        ReleaseTexture(input);
        ReleaseTexture(probe);
        ReleaseTexture(Background);
        input = null;
        probe = null;
        InputPointer = ProbePointer = BackgroundPointer = 0;
        released = true;
    }

    private static void ReleaseTexture(RenderTexture? texture)
    {
        if (texture == null)
            return;
        texture.Release();
        UnityEngine.Object.Destroy(texture);
    }
}
