using System;
using System.Reflection;
using System.Threading;

namespace SimplyMoreFPS.API;

/// <summary>Registers whole GUI draw methods whose output is captured and drawn anchored to the world.</summary>
public static class WorldOverlayApi
{
    internal sealed class Registration
    {
        internal readonly string Id;
        internal readonly MethodInfo DrawMethod;

        internal Registration(string id, MethodInfo drawMethod)
        {
            Id = id;
            DrawMethod = drawMethod;
        }
    }

    internal sealed class Snapshot
    {
        internal readonly long Revision;
        internal readonly Registration[] Registrations;

        internal Snapshot(long revision, Registration[] registrations)
        {
            Revision = revision;
            Registrations = registrations;
        }
    }

    private static readonly object Gate = new object();
    private static Snapshot snapshot = new Snapshot(0, Array.Empty<Registration>());

    internal static Snapshot Current => Volatile.Read(ref snapshot);

    /// <summary>Thread safe. The method must draw only world-anchored UI, never fixed HUD.</summary>
    public static void Register(string id, MethodInfo drawMethod)
    {
        if (string.IsNullOrWhiteSpace(id))
            throw new ArgumentException("A stable world-overlay ID is required.", nameof(id));
        if (drawMethod == null) throw new ArgumentNullException(nameof(drawMethod));
        if (drawMethod.IsAbstract || drawMethod.ContainsGenericParameters || drawMethod.ReturnType != typeof(void) || drawMethod.DeclaringType == null)
            throw new ArgumentException("World-overlay drawing requires a concrete, closed void method.", nameof(drawMethod));

        lock (Gate)
        {
            Snapshot old = snapshot;
            foreach (Registration entry in old.Registrations)
            {
                if (entry.Id == id)
                    throw new InvalidOperationException("World overlay already registered: " + id);
                if (entry.DrawMethod.Equals(drawMethod))
                    throw new InvalidOperationException("World draw method already registered by: " + entry.Id);
            }

            var next = new Registration[old.Registrations.Length + 1];
            Array.Copy(old.Registrations, next, old.Registrations.Length);
            next[next.Length - 1] = new Registration(id, drawMethod);
            Volatile.Write(ref snapshot, new Snapshot(checked(old.Revision + 1), next));
        }
    }

    /// <summary>Takes effect at the next top-level GUI sync point.</summary>
    public static bool Unregister(string id)
    {
        if (id == null) throw new ArgumentNullException(nameof(id));

        lock (Gate)
        {
            Snapshot old = snapshot;
            int index = Array.FindIndex(old.Registrations, entry => entry.Id == id);
            if (index < 0) return false;

            var next = new Registration[old.Registrations.Length - 1];
            Array.Copy(old.Registrations, 0, next, 0, index);
            Array.Copy(old.Registrations, index + 1, next, index, next.Length - index);
            Volatile.Write(ref snapshot, new Snapshot(checked(old.Revision + 1), next));

            return true;
        }
    }
}
