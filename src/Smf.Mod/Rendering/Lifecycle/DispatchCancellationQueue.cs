#nullable disable
using System;
using System.Collections.Generic;

namespace SimplyMoreFPS.Rendering.Lifecycle;

// Main-owned list of dispatches still waiting to be cancelled. Capacity matches
// the native ticket pool; the host queues nothing new while any entry remains.
internal sealed class DispatchCancellationQueue<T> where T : struct
{
    private readonly T[] tickets = new T[32];

    internal int Count { get; private set; }

    internal void Add(T ticket)
    {
        for (int i = 0; i < Count; ++i)
        {
            if (EqualityComparer<T>.Default.Equals(tickets[i], ticket)) return;
        }

        if (Count == tickets.Length) throw new InvalidOperationException("Cancellation queue exceeds the native ticket capacity.");

        tickets[Count++] = ticket;
    }

    // One non-blocking attempt per ticket. Busy (1) keeps the ticket, any other
    // nonzero result throws, and a throwing delegate keeps the ticket as well.
    internal void Retry(Func<T, int> cancel)
    {
        for (int i = Count - 1; i >= 0; --i)
        {
            int result = cancel(tickets[i]);
            if (result == 1) continue;
            if (result != 0) throw new InvalidOperationException("Native ticket cancellation failed: 0x" + result.ToString("X8"));

            tickets[i] = tickets[--Count];
            tickets[Count] = default;
        }
    }
}
