using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace SimplyMoreFPS.Rendering;

internal static class WindowsGameWindow
{
    internal static RendererWindow Find()
    {
        uint process = GetCurrentProcessId();
        IntPtr candidate = IntPtr.Zero;
        uint candidateThread = 0;
        int matches = 0;
        int unityWindows = 0;
        int processWindows = 0;
        var details = new StringBuilder();

        bool enumerated = EnumWindows((window, unused) =>
        {
            uint thread = GetWindowThreadProcessId(window, out uint owner);
            if (owner != process)
            {
                return true;
            }

            processWindows++;
            var name = new StringBuilder(128);
            GetClassName(window, name, name.Capacity);
            bool unity = name.ToString() == "UnityWndClass";
            bool visible = IsWindowVisible(window);
            bool minimized = IsIconic(window);
            bool hasRect = GetClientRect(window, out ClientRect client);
            bool drawable = visible && !minimized && hasRect && client.Right > client.Left && client.Bottom > client.Top;

            // Include rejected windows so a failure explains what Windows actually reported.
            if (unity || processWindows <= 16)
            {
                details.Append(" [hwnd=0x").Append(window.ToInt64().ToString("X"))
                    .Append(" class=").Append(name).Append(" thread=").Append(thread)
                    .Append(" visible=").Append(visible).Append(" minimized=").Append(minimized)
                    .Append(" client=").Append(hasRect ? (client.Right - client.Left) + "x" + (client.Bottom - client.Top) : "unavailable")
                    .Append(']');
            }

            if (unity)
            {
                unityWindows++;
                if (drawable)
                {
                    matches++;
                    candidate = window;
                    candidateThread = thread;
                }
            }

            return true;
        }, IntPtr.Zero);

        if (!enumerated)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not enumerate this game's windows.");
        }

        string reason;
        if (matches == 1)
        {
            // A resize or replacement can invalidate the enumeration before installation.
            uint thread = GetWindowThreadProcessId(candidate, out uint owner);
            var name = new StringBuilder(128);
            GetClassName(candidate, name, name.Capacity);
            if (owner == process && thread == candidateThread && name.ToString() == "UnityWndClass" &&
                IsWindowVisible(candidate) && !IsIconic(candidate) &&
                GetClientRect(candidate, out ClientRect client) && client.Right > client.Left && client.Bottom > client.Top)
            {
                return new RendererWindow(unchecked((ulong)candidate.ToInt64()));
            }

            reason = "The Unity window changed while renderer startup was checking it.";
        }
        else if (matches > 1)
        {
            reason = "Multiple drawable Unity windows belong to this game process; the renderer cannot choose safely.";
        }
        else if (unityWindows > 0)
        {
            // Wait for hidden, minimized or empty windows to become drawable.
            return new RendererWindow("Waiting for the Unity window to become visible, restored and non-empty.",
                details.ToString(), true);
        }
        else
        {
            reason = "No top-level UnityWndClass window belongs to this game process.";
        }

        string diagnostic = " process=" + process + " processWindows=" + processWindows +
            " unityWindows=" + unityWindows + " drawableUnityWindows=" + matches + details;
        return new RendererWindow(reason, diagnostic);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ClientRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private delegate bool EnumWindowCallback(IntPtr window, IntPtr parameter);

    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern uint GetCurrentProcessId();

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowCallback callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder name, int maximum);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(IntPtr window, out ClientRect rect);
}
