using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using SimplyMoreFPS.Performance;
using UnityEngine;
using UnityEngine.Rendering;

namespace SimplyMoreFPS.Rendering;

// Settings only record what the player wants. Installing and enabling the native
// session happens here in Update, outside the settings window's GUI scope.
public sealed class HybridRuntime : MonoBehaviour
{
    private static HybridRuntime? instance;
    private string folder = "";
    private bool wanted;
    private bool startupFailed;

    internal static void Initialize(string modFolder, bool enabled)
    {
        if (instance != null) throw new InvalidOperationException("The renderer owner is already installed.");

        var root = new GameObject("Simply More FPS renderer");
        DontDestroyOnLoad(root);
        instance = root.AddComponent<HybridRuntime>();
        instance.folder = Path.GetFullPath(modFolder);
        instance.wanted = enabled;

        PerformanceMeter.Initialize();
    }

    internal static void SetEnabled(bool enabled)
    {
        if (instance == null) return;
        if (enabled && !instance.wanted) instance.startupFailed = false;
        instance.wanted = enabled;
    }

    private void Update()
    {
        try
        {
            if (wanted && !HybridSession.Installed && !startupFailed)
            {
                InstallPlatform();
            }

            if (HybridSession.Installed && (!startupFailed || !wanted))
            {
                HybridSession.SetEnabled(wanted);
            }
        }
        catch (Exception error)
        {
            if (!startupFailed)
            {
                RendererDiagnostics.Fallback("Renderer initialization failed", error);
            }

            startupFailed = true;
        }

        GameFrameBudget.Update(
            wanted && !startupFailed && SmfMod.Settings.TpsBoost,
            SmfMod.Settings.GameFpsTarget,
            HybridSession.DetachedRenderingActive);
        PerformanceMeter.Update();
        RendererFailureNotice.Update();
    }

    private void InstallPlatform()
    {
        if (IntPtr.Size != 8) throw new PlatformNotSupportedException("The renderer requires the 64-bit game player.");

        const string owner = "olli.simplymorefps.rendering";
        string builtin = Path.Combine(Application.dataPath, "Resources", "unity_builtin_extra");

        switch (Application.platform)
        {
            case RuntimePlatform.WindowsPlayer:
            {
                RequireGraphics(GraphicsDeviceType.Direct3D11);

                string native = Path.Combine(folder, "Native", "win-x64");
                string renderer = Path.Combine(native, "Smf.Renderer.dll");
                string camera = Path.Combine(native, "Smf.Camera.dll");

                RequireFiles(renderer, camera, builtin);
                ulong window = unchecked((ulong)FindOwnUnityWindow().ToInt64());
                HybridSession.InstallWindows(owner, renderer, camera, window, builtin);
                break;
            }

            case RuntimePlatform.LinuxPlayer:
            {
                RequireGraphics(GraphicsDeviceType.OpenGLCore);

                string native = Path.Combine(folder, "Native", "linux-x64");
                string renderer = Path.Combine(native, "libSmf.Renderer.so");
                string camera = Path.Combine(native, "libSmf.Camera.so");

                RequireFiles(renderer, camera, builtin);
                HybridSession.InstallLinux(owner, renderer, camera, builtin);
                break;
            }

            case RuntimePlatform.OSXPlayer:
            {
                RequireGraphics(GraphicsDeviceType.Metal);

                string native = Path.Combine(folder, "Native", "osx-universal");
                string renderer = Path.Combine(native, "Smf.Renderer.bundle");
                string camera = Path.Combine(native, "libSmf.Camera.dylib");

                // The mac player keeps unity_builtin_extra one level deeper inside the app bundle.
                builtin = Path.Combine(Application.dataPath, "Resources", "Data", "Resources", "unity_builtin_extra");
                RequireFiles(Path.Combine(renderer, "Contents", "MacOS", "Smf.Renderer"), camera, builtin);
                HybridSession.InstallMac(owner, renderer, camera, builtin);
                break;
            }

            default:
                throw new PlatformNotSupportedException("The renderer requires a Windows, Linux or macOS game player.");
        }
    }

    private static void RequireGraphics(GraphicsDeviceType supported)
    {
        if (SystemInfo.graphicsDeviceType != supported)
        {
            throw new PlatformNotSupportedException("This platform's renderer requires " + supported + ".");
        }
    }

    private static void RequireFiles(params string[] paths)
    {
        foreach (string path in paths)
        {
            if (!File.Exists(path)) throw new FileNotFoundException("A required renderer resource is missing.", path);
        }
    }

    private void OnApplicationQuit()
    {
        GameFrameBudget.Update(false, SmfMod.Settings.GameFpsTarget, false);
        if (HybridSession.Installed) HybridSession.RequestQuit();
    }

    private void OnDisable() => GameFrameBudget.Update(false, SmfMod.Settings.GameFpsTarget, false);

    private static IntPtr FindOwnUnityWindow()
    {
        uint process = GetCurrentProcessId();
        IntPtr result = IntPtr.Zero;
        bool ambiguous = false;

        bool enumerated = EnumWindows((window, unused) =>
        {
            GetWindowThreadProcessId(window, out uint owner);
            if (owner != process || !IsWindowVisible(window)) return true;

            var name = new StringBuilder(128);
            if (GetClassName(window, name, name.Capacity) == 0 || name.ToString() != "UnityWndClass") return true;
            if (!GetClientRect(window, out ClientRect client) || client.Right <= client.Left || client.Bottom <= client.Top) return true;

            if (result != IntPtr.Zero) ambiguous = true;
            result = window;
            return true;
        }, IntPtr.Zero);

        if (!enumerated) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        if (result == IntPtr.Zero || ambiguous)
        {
            throw new InvalidOperationException("A unique visible Unity window belonging to this game process is required.");
        }

        // Check again after the enumeration in case the window went away meanwhile.
        GetWindowThreadProcessId(result, out uint finalOwner);
        if (!IsWindow(result) || finalOwner != process)
        {
            throw new InvalidOperationException("The original Unity window changed during renderer initialization.");
        }

        return result;
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
    private static extern bool IsWindow(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(IntPtr window, out ClientRect rect);
}
