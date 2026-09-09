#nullable disable
using System;
using System.Collections.Generic;

namespace SimplyMoreFPS.Rendering;

// Scoped, main-thread-only borrowing of a static cache value. Nothing hooks the getter;
// the owner binds the reader and writer once and every borrow is undone in order.
internal sealed class BorrowedViewCache<T>
{
    internal struct Checkpoint
    {
        internal ulong Generation;
        internal ulong LastToken;
    }

    internal struct Lease
    {
        internal ulong Generation;
        internal ulong Token;
    }

    private struct Entry
    {
        internal ulong Token;
        internal T Before;
        internal T Written;
    }

    private readonly Func<T> read;
    private readonly Action<T> write;
    private readonly Entry[] entries = new Entry[64];
    private ulong generation = 1;
    private ulong nextToken;

    internal int Count { get; private set; }

    internal BorrowedViewCache(Func<T> reader, Action<T> writer)
    {
        read = reader ?? throw new ArgumentNullException(nameof(reader));
        write = writer ?? throw new ArgumentNullException(nameof(writer));
    }

    internal Checkpoint Mark() => new Checkpoint { Generation = generation, LastToken = nextToken };

    internal bool IsCurrent(Checkpoint mark) => mark.Generation == generation;

    internal Lease Enter(T value)
    {
        if (Count == entries.Length) throw new InvalidOperationException("Map view cache scope depth exceeded.");

        // Snapshot the current value first so restoring can tell whether anyone else wrote in between.
        T before = read();
        if (Count != 0 && !EqualityComparer<T>.Default.Equals(before, entries[Count - 1].Written))
        {
            throw new InvalidOperationException("Another owner changed the borrowed map view cache.");
        }

        ulong token = checked(++nextToken);
        entries[Count++] = new Entry { Token = token, Before = before, Written = value };
        write(value);
        return new Lease { Generation = generation, Token = token };
    }

    internal bool Exit(Lease lease)
    {
        if (lease.Generation != generation) return true;

        int index = Count - 1;
        while (index >= 0 && entries[index].Token != lease.Token)
        {
            --index;
        }

        return index < 0 || RestoreCount(index);
    }

    internal bool RestoreAfter(Checkpoint mark)
    {
        if (mark.Generation != generation) return true;

        int count = Count;
        while (count > 0 && entries[count - 1].Token > mark.LastToken)
        {
            --count;
        }

        return RestoreCount(count);
    }

    internal bool Cancel()
    {
        bool clean = RestoreCount(0);

        // A new generation makes every outstanding lease and checkpoint stale.
        generation = checked(generation + 1);
        return clean;
    }

    private bool RestoreCount(int count)
    {
        bool clean = true;

        while (Count > count)
        {
            Entry entry = entries[Count - 1];
            T current = read();

            // Leave a foreign write in place and report it; the caller drops the frame instead.
            if (EqualityComparer<T>.Default.Equals(current, entry.Written))
            {
                write(entry.Before);
            }
            else
            {
                clean = false;
            }

            entries[--Count] = default;
        }

        return clean;
    }
}
