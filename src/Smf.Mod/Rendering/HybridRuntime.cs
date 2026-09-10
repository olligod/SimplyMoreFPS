using System;
using System.IO;
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
    private readonly RendererWindowStartup gameWindow = new RendererWindowStartup(message =>
        Verse.Log.Message("[Simply More FPS] " + message));

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
        if (enabled && !instance.wanted)
        {
            instance.startupFailed = false;
            instance.gameWindow.Reset();
        }

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

                if (!gameWindow.TryFind(WindowsGameWindow.Find, out ulong window))
                {
                    return;
                }

                RequireFiles(renderer, camera, builtin);
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
                if (!gameWindow.TryFind(() =>
                    {
                        int result = LinuxRendererApi.FindOriginalWindow(renderer, out ulong handle);
                        return RendererWindow.FromNative("Linux/X11", result, handle);
                    }, out ulong window))
                {
                    return;
                }

                HybridSession.InstallLinux(owner, renderer, camera, window, builtin);
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
                if (!gameWindow.TryFind(() =>
                    {
                        int result = MacRendererApi.FindOriginalWindow(renderer, out ulong handle);
                        return RendererWindow.FromNative("macOS/Metal", result, handle);
                    }, out ulong window))
                {
                    return;
                }

                HybridSession.InstallMac(owner, renderer, camera, window, builtin);
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
}
