using System;
using SimplyMoreFPS.Compatibility;
using Verse;

namespace SimplyMoreFPS.API;

public interface ICameraProvider
{
    string Id
    {
        get;
    }
    int Priority
    {
        get;
    }
    CameraPolicy? Resolve(CameraContext context);
}

/// <summary>Valid only during Resolve on Unity's main thread; never keep the game objects in a profile.</summary>
public readonly struct CameraContext
{
    public CameraDriver Driver
    {
        get;
    }
    public Map Map
    {
        get;
    }

    internal CameraContext(CameraDriver driver, Map map)
    {
        Driver = driver;
        Map = map;
    }
}

public sealed class CameraPolicy
{
    public CameraProfile? Profile
    {
        get;
    }
    public bool AllowDetachedMotion
    {
        get;
    }
    public bool AllowDetachedRendering
    {
        get;
    }
    public CameraScalarOverrides? Scalars
    {
        get;
    }

    public CameraPolicy(CameraProfile? profile = null, bool allowDetachedMotion = true, CameraScalarOverrides? scalars = null)
        : this(true, profile, allowDetachedMotion, scalars)
    {
    }

    public CameraPolicy(bool allowDetachedRendering, CameraProfile? profile = null,
        bool allowDetachedMotion = true, CameraScalarOverrides? scalars = null)
    {
        Profile = profile;
        AllowDetachedMotion = allowDetachedRendering && allowDetachedMotion;
        AllowDetachedRendering = allowDetachedRendering;
        Scalars = scalars;
    }
}

public sealed class CameraResolution
{
    public string ProviderId
    {
        get;
    }
    public CameraPolicy Policy
    {
        get;
    }

    internal CameraResolution(string providerId, CameraPolicy policy)
    {
        ProviderId = providerId;
        Policy = policy;
    }
}

/// <summary>Registry of camera providers; registration is thread safe, resolution runs on Unity's main thread.</summary>
public static class CameraProviders
{
    private sealed class Entry
    {
        internal readonly string Id;
        internal readonly int Priority;
        internal readonly ICameraProvider Provider;
        internal CameraResolution? CachedResolution;

        internal Entry(ICameraProvider provider, string id, int priority)
        {
            Provider = provider;
            Id = id;
            Priority = priority;
        }
    }

    private static readonly object Gate = new object();
    private static readonly CameraResolution Vanilla = new CameraResolution("vanilla", new CameraPolicy());
    private static readonly CameraResolution External = new CameraResolution("external-config", new CameraPolicy(allowDetachedMotion: false));
    private static Entry[] entries = Array.Empty<Entry>();
    private static bool builtinsReady;

    public static void Register(ICameraProvider provider)
    {
        if (provider == null)
            throw new ArgumentNullException(nameof(provider));
        string id = provider.Id;
        int priority = provider.Priority;
        if (string.IsNullOrWhiteSpace(id))
            throw new ArgumentException("A stable provider ID is required.", nameof(provider));

        lock (Gate)
        {
            foreach (Entry entry in entries)
            {
                if (string.Equals(entry.Id, id, StringComparison.Ordinal))
                    throw new InvalidOperationException("Camera provider already registered: " + id);
            }

            var next = new Entry[entries.Length + 1];
            Array.Copy(entries, next, entries.Length);
            next[next.Length - 1] = new Entry(provider, id, priority);

            Array.Sort(next, (a, b) =>
            {
                int order = b.Priority.CompareTo(a.Priority);
                return order != 0 ? order : string.CompareOrdinal(a.Id, b.Id);
            });
            entries = next;
        }
    }

    public static bool Unregister(string id)
    {
        if (id == null)
            throw new ArgumentNullException(nameof(id));

        lock (Gate)
        {
            int index = Array.FindIndex(entries, entry => entry.Id == id);
            if (index < 0)
                return false;

            var next = new Entry[entries.Length - 1];
            Array.Copy(entries, 0, next, 0, index);
            Array.Copy(entries, index + 1, next, index, next.Length - index);
            entries = next;

            return true;
        }
    }

    internal static CameraResolution Resolve(CameraDriver driver, Map map)
    {
        RequireMainThread();
        if (driver == null || driver.config == null || map == null)
            return External;

        if (!builtinsReady)
        {
            Compat.RegisterCameraProviders();
            builtinsReady = true;
        }

        Entry[] current;
        lock (Gate)
        {
            current = entries;
        }

        if (current.Length == 0)
            return DefaultFor(driver);

        var context = new CameraContext(driver, map);
        foreach (Entry entry in current)
        {
            CameraPolicy? policy;
            try
            {
                policy = entry.Provider.Resolve(context);
            }
            catch (Exception error)
            {
                throw new InvalidOperationException("Camera provider '" + entry.Id + "' failed during Resolve.", error);
            }

            if (policy == null)
                continue;
            if (entry.CachedResolution == null || !ReferenceEquals(entry.CachedResolution.Policy, policy))
                entry.CachedResolution = new CameraResolution(entry.Id, policy);
            return entry.CachedResolution;
        }

        return DefaultFor(driver);
    }

    internal static void RequireMainThread()
    {
        if (!UnityData.IsInMainThread)
            throw new InvalidOperationException("Camera API game operations require Unity's main thread.");
    }

    private static CameraResolution DefaultFor(CameraDriver driver)
    {
        return driver.config.GetType() == typeof(CameraMapConfig_Normal) ? Vanilla : External;
    }
}
