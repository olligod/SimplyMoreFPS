using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace SimplyMoreFPS.Rendering;

// Counts DWM presents on a background thread so the meter keeps ticking while Unity
// main is stalled. DwmFlush blocks until the next Present; nothing here renders.
internal static class WindowsCompositionCadence
{
    private static readonly AutoResetEvent Wake = new AutoResetEvent(false);
    private static readonly long Frequency = ReadFrequency();
    private static Thread? worker;
    private static long completions;
    private static int sampling;
    private static int failure;
    private static string? failureDetail;

    internal static void Read(out ulong count, out long timestamp)
    {
        if (worker == null) worker = StartWorker();

        // Report a failure once. Re-ticking the meter checkbox wakes the same sampler again,
        // which is the retry after a transient compositor or remote-session failure.
        int error = Interlocked.Exchange(ref failure, 0);
        if (error != 0)
        {
            throw new InvalidOperationException("DWM presentation meter failed: " + Volatile.Read(ref failureDetail) + " (0x" + error.ToString("X8") + ").");
        }

        timestamp = Now();
        Volatile.Write(ref sampling, 1);
        Wake.Set();
        count = (ulong)Interlocked.Read(ref completions);
    }

    internal static void Suspend() => Volatile.Write(ref sampling, 0);

    private static Thread StartWorker()
    {
        var thread = new Thread(Run)
        {
            IsBackground = true,
            Name = "SMF display meter"
        };
        thread.Start();
        return thread;
    }

    private static void Run()
    {
        while (true)
        {
            Wake.WaitOne();
            if (Volatile.Read(ref failure) != 0) continue;

            try
            {
                int immediate = 0;

                while (Volatile.Read(ref sampling) != 0)
                {
                    long start = Now();
                    int result = DwmFlush();
                    if (result != 0)
                    {
                        Fail(result, "DwmFlush returned an error");
                        break;
                    }

                    // DWM counters are fake or missing in some RDP sessions and DwmFlush returns at
                    // once. Sixteen instant returns in a row is a failure, not thousands of FPS.
                    if (Now() - start < Frequency / 10000)
                    {
                        if (++immediate >= 16)
                        {
                            Fail(unchecked((int)0x80004005), "DwmFlush returned without a presentation wait");
                            break;
                        }

                        Thread.Sleep(1);
                        continue;
                    }

                    immediate = 0;
                    Interlocked.Increment(ref completions);
                }
            }
            catch (Exception error)
            {
                Fail(unchecked((int)0x80004005), error.ToString());
            }
        }
    }

    private static void Fail(int result, string detail)
    {
        Volatile.Write(ref failureDetail, detail);
        Volatile.Write(ref failure, result);
    }

    private static long ReadFrequency()
    {
        if (!QueryPerformanceFrequency(out long value) || value <= 0)
        {
            throw new InvalidOperationException("Windows display meter clock is unavailable.");
        }

        return value;
    }

    private static long Now()
    {
        if (!QueryPerformanceCounter(out long value)) throw new InvalidOperationException("Windows display meter clock read failed.");
        return value;
    }

    [DllImport("dwmapi.dll", ExactSpelling = true)]
    private static extern int DwmFlush();

    [DllImport("kernel32.dll")]
    private static extern bool QueryPerformanceFrequency(out long value);

    [DllImport("kernel32.dll")]
    private static extern bool QueryPerformanceCounter(out long value);
}
