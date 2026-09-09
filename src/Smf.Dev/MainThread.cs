using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace SimplyMoreFPS.Dev;

// Work queue drained by DevDriver.Update. Pending work is cancelled on timeout; running work is reported as unobserved.
internal static class MainThread
{
    private const int Capacity = 64;
    private static readonly ConcurrentQueue<IJob> Queue = new ConcurrentQueue<IJob>();
    private static int _count;
    private static long _completed;
    private static long _cancelled;

    internal static int Pending => Volatile.Read(ref _count);
    internal static long Completed => Interlocked.Read(ref _completed);
    internal static long Cancelled => Interlocked.Read(ref _cancelled);

    internal static Job<T> Enqueue<T>(Action<Job<T>> work)
    {
        if (Interlocked.Increment(ref _count) > Capacity)
        {
            Interlocked.Decrement(ref _count);
            throw new BusyException("Main-thread queue is full.");
        }

        var job = new Job<T>(work);
        Queue.Enqueue(job);
        return job;
    }

    internal static T Submit<T>(Func<T> work, int timeoutMs = 30000)
    {
        return Enqueue<T>(job => job.Complete(work())).Wait(timeoutMs);
    }

    // At most four jobs or two milliseconds per frame so the game keeps moving.
    internal static void Drain()
    {
        long begin = Stopwatch.GetTimestamp();

        for (int i = 0; i < 4 && Queue.TryDequeue(out IJob? job); i++)
        {
            Interlocked.Decrement(ref _count);
            job.Run();
            if ((Stopwatch.GetTimestamp() - begin) * 1000.0 / Stopwatch.Frequency >= 2) break;
        }
    }

    private interface IJob
    {
        void Run();
    }

    internal sealed class Job<T> : IJob
    {
        private readonly Action<Job<T>> _work;
        private readonly TaskCompletionSource<T> _result = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _state; // 0 pending, 1 running, 2 cancelled, 3 finished.

        internal Job(Action<Job<T>> work)
        {
            _work = work;
        }

        public void Run()
        {
            if (Interlocked.CompareExchange(ref _state, 1, 0) != 0) return;
            try
            {
                _work(this);
            }
            catch (Exception ex)
            {
                Fail(ex);
            }
        }

        internal void Complete(T value)
        {
            if (Interlocked.CompareExchange(ref _state, 3, 1) != 1) return;
            Interlocked.Increment(ref _completed);
            _result.TrySetResult(value);
        }

        internal void Fail(Exception error)
        {
            if (Interlocked.CompareExchange(ref _state, 3, 1) != 1) return;
            DevLog.Error("main-thread work", error);
            Interlocked.Increment(ref _completed);
            _result.TrySetException(error);
        }

        internal T Wait(int milliseconds)
        {
            try
            {
                if (_result.Task.Wait(milliseconds)) return _result.Task.GetAwaiter().GetResult();
            }
            catch (AggregateException)
            {
                return _result.Task.GetAwaiter().GetResult();
            }

            if (Interlocked.CompareExchange(ref _state, 2, 0) == 0)
            {
                Interlocked.Increment(ref _cancelled);
                throw new TimeoutException("Main thread did not start the request. It was cancelled and will not execute.");
            }

            if (_result.Task.IsCompleted) return _result.Task.GetAwaiter().GetResult();
            throw new UnobservedWorkException("Request started but its result was not observed before timeout. "
                + "Its effects may still occur; do not blindly retry.");
        }
    }
}

internal sealed class BusyException : Exception
{
    internal BusyException(string message) : base(message)
    {
    }
}

internal sealed class UnobservedWorkException : Exception
{
    internal UnobservedWorkException(string message) : base(message)
    {
    }
}
