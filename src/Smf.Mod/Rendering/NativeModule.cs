#nullable disable
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using SimplyMoreFPS.Rendering.Mac;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

// A loaded module stays in the process for good, so the wrapper is cached and never freed.
internal sealed class NativeModule
{
    internal static readonly StringComparer PathComparer = Path.DirectorySeparatorChar == '\\'
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    private static readonly Dictionary<string, NativeModule> Modules = new Dictionary<string, NativeModule>(PathComparer);

    private readonly int ownerThread = Thread.CurrentThread.ManagedThreadId;
    private readonly IntPtr handle;
    private readonly MacUnityModule mac;
    private readonly bool linux;

    internal bool IsMac => mac != null;

    internal static NativeModule Open(string exactPath)
    {
        if (string.IsNullOrWhiteSpace(exactPath) || exactPath.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("An exact native module path is required.");
        }

        string path = Path.GetFullPath(exactPath);
        if (Modules.TryGetValue(path, out NativeModule existing))
        {
            existing.CheckMain();
            return existing;
        }

        var module = new NativeModule(path);
        Modules.Add(path, module);
        return module;
    }

    private NativeModule(string path)
    {
        if (Application.platform == RuntimePlatform.OSXPlayer)
        {
            mac = MacUnityModule.Open(path);
            return;
        }

        linux = Application.platform == RuntimePlatform.LinuxPlayer;
        if (!linux && Application.platform != RuntimePlatform.WindowsPlayer)
        {
            throw new PlatformNotSupportedException("The native renderer requires a standalone game player.");
        }

        if (!File.Exists(path)) throw new FileNotFoundException("Native module is absent.", path);

        if (linux)
        {
            // dlopen takes a NUL-terminated UTF-8 path; flag 2 is RTLD_NOW.
            byte[] bytes = Encoding.UTF8.GetBytes(path + "\0");
            var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            try
            {
                handle = dlopen(pin.AddrOfPinnedObject(), 2);
            }
            finally
            {
                pin.Free();
            }
        }
        else
        {
            handle = LoadLibraryW(path);
        }

        if (handle == IntPtr.Zero)
        {
            string reason = linux
                ? ": " + Marshal.PtrToStringAnsi(dlerror())
                : " (Win32 " + Marshal.GetLastWin32Error() + ")";
            throw new InvalidOperationException("Native module load failed: " + path + reason);
        }
    }

    internal T Bind<T>(string export) where T : class
    {
        CheckMain();
        if (mac != null) return mac.Bind<T>(export);
        if (string.IsNullOrWhiteSpace(export) || export.IndexOf('\0') >= 0)
        {
            throw new ArgumentException("An exact native export is required.");
        }

        IntPtr address = linux ? dlsym(handle, export) : GetProcAddress(handle, export);
        if (address == IntPtr.Zero) throw new EntryPointNotFoundException(export);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
    }

    private void CheckMain()
    {
        if (ownerThread != Thread.CurrentThread.ManagedThreadId)
        {
            throw new InvalidOperationException("Native modules belong to the Unity main thread.");
        }
    }

    [DllImport("libdl.so.2", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr dlopen(IntPtr path, int flags);

    [DllImport("libdl.so.2", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr dlsym(IntPtr module, string name);

    [DllImport("libdl.so.2", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr dlerror();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryW(string path);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);
}
