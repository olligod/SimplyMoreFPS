using System;
using System.Diagnostics;

namespace SimplyMoreFPS.Rendering;

internal readonly struct RendererWindow
{
    internal readonly ulong Handle;
    internal readonly string Reason;
    internal readonly string Details;
    internal readonly bool Suspended;

    internal RendererWindow(ulong handle)
    {
        Handle = handle;
        Reason = "";
        Details = "";
        Suspended = false;
    }

    internal RendererWindow(string reason, string details, bool suspended = false)
    {
        Handle = 0;
        Reason = reason;
        Details = details;
        Suspended = suspended;
    }

    internal static RendererWindow FromNative(string platform, int result, ulong handle)
    {
        if (result == 0 && handle != 0)
        {
            return new RendererWindow(handle);
        }

        string details = " platform=" + platform + " result=" + result + " window=0x" + handle.ToString("X");
        switch (result)
        {
            case 1:
                return new RendererWindow("No matching game window was found.", details);
            case 2:
                return new RendererWindow("Multiple game windows match; the renderer cannot choose safely.", details);
            case 3:
                return new RendererWindow("Waiting for the game window to become visible, restored and non-empty.", details, true);
            default:
                throw new InvalidOperationException("Native game window discovery failed." + details);
        }
    }
}

// Every backend reports window availability; startup recovery follows the same policy.
internal sealed class RendererWindowStartup
{
    private const double RetryInterval = 0.25;
    private const double FailureDelay = 10;

    private readonly Action<string> reportWaiting;
    private double nextProbe;
    private double failureSince = -1;
    private string waitingReason = "";

    internal RendererWindowStartup(Action<string> reportWaiting)
    {
        this.reportWaiting = reportWaiting;
    }

    internal void Reset()
    {
        nextProbe = 0;
        failureSince = -1;
        waitingReason = "";
    }

    internal bool TryFind(Func<RendererWindow> lookup, out ulong handle)
    {
        handle = 0;
        double now = (double)Stopwatch.GetTimestamp() / Stopwatch.Frequency;
        if (now < nextProbe)
        {
            return false;
        }

        nextProbe = now + RetryInterval;
        RendererWindow window = lookup();
        if (window.Handle != 0)
        {
            handle = window.Handle;
            Reset();
            return true;
        }

        if (window.Suspended)
        {
            failureSince = -1;
        }
        else
        {
            if (failureSince < 0)
            {
                failureSince = now;
            }

            if (now - failureSince >= FailureDelay)
            {
                throw new InvalidOperationException(window.Reason + " Still unresolved after " +
                    FailureDelay + " seconds." + window.Details);
            }
        }

        if (waitingReason != window.Reason)
        {
            waitingReason = window.Reason;
            reportWaiting(window.Reason + " Renderer startup will retry." + window.Details);
        }

        return false;
    }
}
