using System;
using System.Runtime.InteropServices;

namespace SimplyMoreFPS.Rendering;

internal sealed unsafe class ScenePacketBuffer : IDisposable
{
    private IntPtr allocation;
    private ScenePackets.Description* description;
    private ScenePackets.Image* images;
    private ScenePackets.Layer* layers;
    private ScenePackets.Effect* effects;
    private bool begun;

    internal ulong Pointer
    {
        get
        {
            RequireFrame();
            return unchecked((ulong)allocation.ToInt64());
        }
    }

    internal ScenePacketBuffer()
    {
        ScenePackets.ValidateLayout();
        int size = sizeof(ScenePackets.Description) + ScenePackets.MaximumImages * sizeof(ScenePackets.Image) +
            ScenePackets.MaximumLayers * sizeof(ScenePackets.Layer) + ScenePackets.MaximumEffects * sizeof(ScenePackets.Effect);
        allocation = Marshal.AllocHGlobal(size);
        description = (ScenePackets.Description*)allocation;
        images = (ScenePackets.Image*)(description + 1);
        layers = (ScenePackets.Layer*)(images + ScenePackets.MaximumImages);
        effects = (ScenePackets.Effect*)(layers + ScenePackets.MaximumLayers);
        *description = default;
    }

    internal void Begin(ulong frame)
    {
        RequireLive();
        if (frame == 0)
            throw new ArgumentOutOfRangeException(nameof(frame));
        *description = new ScenePackets.Description
        {
            Size = (uint)sizeof(ScenePackets.Description),
            Version = ScenePackets.Version,
            SourceFrame = frame,
            Images = (ulong)images,
            Layers = (ulong)layers,
            Effects = (ulong)effects
        };
        begun = true;
    }

    internal uint AddImage(ulong texture, ulong serial, uint width, uint height, ScenePackets.ImageFlags flags)
    {
        RequireFrame();
        uint index = description->ImageCount;
        if (index >= ScenePackets.MaximumImages || texture == 0 || serial == 0 || serial > description->SourceFrame ||
            width == 0 || height == 0 || width > 16384 || height > 16384 || (ulong)width * height > 8 * 1024 * 1024)
        {
            throw new InvalidOperationException("Invalid scene image.");
        }

        images[index] = new ScenePackets.Image
        {
            Texture = texture,
            Serial = serial,
            Width = width,
            Height = height,
            Flags = flags
        };
        description->ImageCount++;
        return index;
    }

    internal void AddLayer(ScenePackets.Layer layer)
    {
        RequireFrame();
        if (description->LayerCount >= ScenePackets.MaximumLayers)
            throw new InvalidOperationException("Too many scene layers.");
        layers[description->LayerCount++] = layer;
    }

    internal void AddEffect(ScenePackets.Effect effect)
    {
        RequireFrame();
        if (description->EffectCount >= ScenePackets.MaximumEffects)
            throw new InvalidOperationException("Too many scene image effects.");
        effects[description->EffectCount++] = effect;
    }

    internal void SetBackground(ScenePackets.Background background)
    {
        RequireFrame();
        description->World = background;
    }

    private void RequireFrame()
    {
        RequireLive();
        if (!begun)
            throw new InvalidOperationException("No scene frame has begun.");
    }

    private void RequireLive()
    {
        if (allocation == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(ScenePacketBuffer));
    }

    public void Dispose()
    {
        if (allocation == IntPtr.Zero)
            return;
        Marshal.FreeHGlobal(allocation);
        allocation = IntPtr.Zero;
        description = null;
        images = null;
        layers = null;
        effects = null;
        begun = false;
    }
}
