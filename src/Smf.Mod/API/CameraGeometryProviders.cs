using System;
using System.Threading;
using SimplyMoreFPS.Compatibility;

namespace SimplyMoreFPS.API;

/// <summary>Registers independent geometry queries, evaluated on Unity's main thread.</summary>
public static class CameraGeometryProviders
{
    private sealed class Entry
    {
        internal readonly string Id;
        internal readonly int Priority;
        internal readonly Func<CameraContext, CameraGeometryPolicy?>? Policy;
        internal readonly Func<CameraContext, CameraMovementExtent?>? MovementExtent;
        internal readonly Func<CameraContext, CameraCoverageBounds?>? CoverageBounds;

        internal Entry(string id, int priority, Func<CameraContext, CameraGeometryPolicy?>? policy,
            Func<CameraContext, CameraMovementExtent?>? movementExtent,
            Func<CameraContext, CameraCoverageBounds?>? coverageBounds)
        {
            Id = id;
            Priority = priority;
            Policy = policy;
            MovementExtent = movementExtent;
            CoverageBounds = coverageBounds;
        }
    }

    private static readonly object Gate = new object();
    private static Entry[] entries = Array.Empty<Entry>();

    static CameraGeometryProviders()
    {
        // Registration is managed-only; foreign mods bind later, inside the requested query.
        Compat.RegisterGeometryProviders();
    }

    /// <summary>Thread safe. Each query takes the first non-null result, ordered by descending priority then ID.</summary>
    public static void Register(string id, int priority,
        Func<CameraContext, CameraGeometryPolicy?>? policy = null,
        Func<CameraContext, CameraMovementExtent?>? movementExtent = null,
        Func<CameraContext, CameraCoverageBounds?>? coverageBounds = null)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new ArgumentException("A stable geometry provider ID is required.", nameof(id));
        }

        if (policy == null && movementExtent == null && coverageBounds == null)
        {
            throw new ArgumentException("At least one geometry query is required.");
        }

        lock (Gate)
        {
            Entry[] old = entries;
            foreach (Entry entry in old)
            {
                if (string.Equals(entry.Id, id, StringComparison.Ordinal))
                {
                    throw new InvalidOperationException("Camera geometry already registered: " + id);
                }
            }

            var next = new Entry[old.Length + 1];
            Array.Copy(old, next, old.Length);
            next[old.Length] = new Entry(id, priority, policy, movementExtent, coverageBounds);
            Array.Sort(next, (a, b) =>
            {
                int order = b.Priority.CompareTo(a.Priority);
                return order != 0 ? order : string.CompareOrdinal(a.Id, b.Id);
            });
            Volatile.Write(ref entries, next);
        }
    }

    /// <summary>Thread safe. A query already in progress keeps its registration snapshot.</summary>
    public static bool Unregister(string id)
    {
        if (id == null)
        {
            throw new ArgumentNullException(nameof(id));
        }

        lock (Gate)
        {
            Entry[] old = entries;
            int index = Array.FindIndex(old, entry => string.Equals(entry.Id, id, StringComparison.Ordinal));
            if (index < 0)
            {
                return false;
            }

            var next = new Entry[old.Length - 1];
            Array.Copy(old, 0, next, 0, index);
            Array.Copy(old, index + 1, next, index, next.Length - index);
            Volatile.Write(ref entries, next);
            return true;
        }
    }

    private static Entry[] Current()
    {
        CameraProviders.RequireMainThread();
        return Volatile.Read(ref entries);
    }

    internal static CameraGeometryPolicy? ResolvePolicy(CameraContext context)
    {
        foreach (Entry entry in Current())
        {
            if (entry.Policy == null)
            {
                continue;
            }

            try
            {
                CameraGeometryPolicy? value = entry.Policy(context);
                if (value != null)
                {
                    return value;
                }
            }
            catch (Exception error)
            {
                throw QueryError(entry, "policy", error);
            }
        }

        return null;
    }

    internal static CameraMovementExtent? ResolveMovementExtent(CameraContext context)
    {
        foreach (Entry entry in Current())
        {
            if (entry.MovementExtent == null)
            {
                continue;
            }

            try
            {
                CameraMovementExtent? value = entry.MovementExtent(context);
                if (!value.HasValue)
                {
                    continue;
                }

                value.Value.RequireValid();
                return value;
            }
            catch (Exception error)
            {
                throw QueryError(entry, "movement extent", error);
            }
        }

        return null;
    }

    internal static CameraCoverageBounds? ResolveCoverageBounds(CameraContext context)
    {
        foreach (Entry entry in Current())
        {
            if (entry.CoverageBounds == null)
            {
                continue;
            }

            try
            {
                CameraCoverageBounds? value = entry.CoverageBounds(context);
                if (!value.HasValue)
                {
                    continue;
                }

                value.Value.RequireValid();
                return value;
            }
            catch (Exception error)
            {
                throw QueryError(entry, "coverage bounds", error);
            }
        }

        return null;
    }

    private static Exception QueryError(Entry entry, string query, Exception error)
    {
        return new InvalidOperationException("Camera geometry '" + entry.Id + "' failed during " + query + ".", error);
    }
}
