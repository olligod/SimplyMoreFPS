using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using UnityEngine;

namespace SimplyMoreFPS.Rendering.Mac;

// A renderer bundle loaded through Unity's plugin resolver. On standalone
// macOS Application.dataPath is the .app/Contents directory.
public sealed class MacUnityModule
{
    private static readonly Dictionary<string, MacUnityModule> Modules = new Dictionary<string, MacUnityModule>(StringComparer.Ordinal);
    private readonly int owner = Thread.CurrentThread.ManagedThreadId;
    private readonly NativeImports imports;
    public readonly string Bundle, Executable, RelativeName;

    public static MacUnityModule Open(string bundleDirectory)
    {
        if (Application.platform != RuntimePlatform.OSXPlayer || IntPtr.Size != 8)
            throw new PlatformNotSupportedException("The external renderer bundle requires macOS x64 or ARM64 Unity.");

        string bundle = ResolvePath(bundleDirectory);
        if (Modules.TryGetValue(bundle, out MacUnityModule existing))
        {
            existing.RequireMainThread();
            return existing;
        }

        var module = new MacUnityModule(bundle);
        Modules.Add(bundle, module); // Never unloaded.
        return module;
    }

    private MacUnityModule(string bundle)
    {
        Bundle = bundle;
        string contents = ResolvePath(Application.dataPath);
        if (Path.GetFileName(contents) != "Contents" || !Path.GetDirectoryName(contents).EndsWith(".app", StringComparison.Ordinal))
            throw new InvalidOperationException("Unexpected Unity app directory layout.");

        string plugins = ResolvePath(Path.Combine(contents, "PlugIns"));
        // Unity's resolver uses a different base when architecture subdirectories
        // exist. Refuse that layout instead of guessing the route.
        if (Directory.Exists(Path.Combine(plugins, "x86_64"))
            || Directory.Exists(Path.Combine(plugins, "arm64"))
            || Directory.Exists(Path.Combine(plugins, "ARM64")))
            throw new PlatformNotSupportedException("Unity architecture-specific plugin directories need a qualified resolver route.");

        RelativeName = RelativeBundleRoute.Compute(plugins, bundle);
        string baseName = Path.GetFileNameWithoutExtension(bundle);
        Executable = ResolvePath(Path.Combine(bundle, "Contents/MacOS/" + baseName));
        if (!File.Exists(Executable)) throw new FileNotFoundException("The exact native bundle executable is absent.", Executable);

        imports = new NativeImports(RelativeName);
    }

    public T Bind<T>(string export) where T : class
    {
        RequireMainThread();
        return imports.Bind<T>(export);
    }

    public void VerifyCallback(IntPtr callback)
    {
        RequireMainThread();
        if (callback == IntPtr.Zero
            || dladdr(callback, out DlInfo info) == 0
            || info.Image == IntPtr.Zero
            || ResolvePath(ReadUtf8(info.Image)) != Executable)
            throw new InvalidOperationException("Unity's actual render callback came from a different native image.");
    }

    private void RequireMainThread()
    {
        if (Thread.CurrentThread.ManagedThreadId != owner)
            throw new InvalidOperationException("The native bundle client belongs to Unity main.");
    }

    private static string ResolvePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.IndexOf('\0') >= 0 || path[0] != '/')
            throw new ArgumentException("An absolute Mac filesystem path is required.");

        byte[] utf8 = Encoding.UTF8.GetBytes(path + "\0");
        IntPtr resolved = realpath(utf8, IntPtr.Zero);
        if (resolved == IntPtr.Zero) throw new IOException("Cannot resolve native path: " + path);

        try
        {
            return ReadUtf8(resolved);
        }
        finally
        {
            free(resolved);
        }
    }

    private static string ReadUtf8(IntPtr text)
    {
        int count = 0;
        while (Marshal.ReadByte(text, count) != 0)
        {
            if (++count > 1024 * 1024) throw new InvalidOperationException("Unterminated native path.");
        }

        byte[] bytes = new byte[count];
        Marshal.Copy(text, bytes, 0, count);
        return new UTF8Encoding(false, true).GetString(bytes);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DlInfo
    {
        public IntPtr Image, Base, Symbol, Address;
    }

    [DllImport("/usr/lib/libSystem.B.dylib", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr realpath(byte[] path, IntPtr output);

    [DllImport("/usr/lib/libSystem.B.dylib", CallingConvention = CallingConvention.Cdecl)]
    private static extern void free(IntPtr value);

    [DllImport("/usr/lib/libSystem.B.dylib", CallingConvention = CallingConvention.Cdecl)]
    private static extern int dladdr(IntPtr value, out DlInfo info);
}
