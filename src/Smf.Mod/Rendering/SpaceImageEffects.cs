using System;
using System.Collections.Generic;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

internal interface ISceneImageEffect
{
    void DescribeScene(ScenePacketBuffer packet, Camera source, Camera coverage, ulong cacheSerial);
}

internal sealed class SpaceImageEffects : IDisposable
{
    private readonly Camera source;
    private readonly MapCoverageCapture coverage;
    private readonly List<MonoBehaviour> components = new List<MonoBehaviour>();
    private readonly List<IMapImageEffect> owned = new List<IMapImageEffect>();
    private readonly List<ISceneImageEffect> current = new List<ISceneImageEffect>();

    internal SpaceImageEffects(Camera source, MapCoverageCapture coverage)
    {
        this.source = source;
        this.coverage = coverage;
    }

    internal bool Prepare()
    {
        current.Clear();
        // Effects can change before the host has retired this capture.
        if (!MapImageEffects.CanCapture(source, out _))
            return false;

        source.GetComponents(components);
        try
        {
            foreach (MonoBehaviour component in components)
            {
                if (component == null || !component.isActiveAndEnabled)
                    continue;
                MapImageEffects.Definition? definition = MapImageEffects.Identify(component.GetType());
                if (definition == null)
                    continue;
                if (definition.Create == null)
                    return false;

                IMapImageEffect? adapter = null;
                foreach (IMapImageEffect existing in owned)
                {
                    if (existing.Source == component)
                    {
                        adapter = existing;
                        break;
                    }
                }

                if (adapter == null)
                {
                    adapter = definition.Create(component);
                    owned.Add(adapter);
                }

                if (!adapter.Prepare())
                    return false;
                if (!(adapter is ISceneImageEffect scene))
                    throw new InvalidOperationException("No detached scene adapter for " + component.GetType().FullName);
                current.Add(scene);
            }

            return true;
        }
        finally
        {
            components.Clear();
        }
    }

    internal void Describe(ScenePacketBuffer packet)
    {
        foreach (ISceneImageEffect effect in current)
            effect.DescribeScene(packet, source, coverage.CoverageCamera, coverage.CacheSerial);
    }

    public void Dispose()
    {
        // Disabled effects can still be referenced by a retained native frame.
        foreach (IMapImageEffect effect in owned)
            effect.Dispose();
        owned.Clear();
        current.Clear();
        components.Clear();
    }
}
