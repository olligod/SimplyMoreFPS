#nullable disable
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using HarmonyLib;
using RimWorld;
using SimplyMoreFPS.API;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering.CameraControl;

// Publishes the wheel policy (window rects, focus) to the native side and
// queues zoom key impulses. Unity's own wheel events are left alone so menus
// keep scrolling; the native side owns the camera wheel exclusively.
public static class NativeCameraControls
{
    private const string Owner = "olli.simplymorefps.rendering.camera-controls";
    private static readonly Dictionary<string, NativeApi> Modules = new Dictionary<string, NativeApi>(NativeModule.PathComparer);
    private static NativeApi api;
    private static Harmony harmony;
    private static int mainThread;
    private static int lastHotControl;
    private static bool installed;
    private static long acceptedZoomKeys;
    private static long consumedWheel;
    private static long busyPolicy;
    private static long independentlyOwnedWheel;
    private static ControlPolicy policy = new ControlPolicy { Rects = new ControlRect[64] };

    public static string LastError { get; private set; }

    internal static void Install(string path)
    {
        if (installed) throw new InvalidOperationException("Controls already installed.");
        mainThread = Thread.CurrentThread.ManagedThreadId;

        if (Marshal.SizeOf(typeof(ControlPolicy)) != 1088
            || Marshal.OffsetOf(typeof(ControlPolicy), nameof(ControlPolicy.Rects)).ToInt32() != 64
            || Marshal.SizeOf(typeof(Impulse)) != 48
            || Marshal.SizeOf(typeof(Status)) != 72
            || Marshal.SizeOf(typeof(WheelStatus)) != 96)
            throw new InvalidOperationException("Control ABI mismatch.");

        if (!Modules.TryGetValue(path, out api))
        {
            api = new NativeApi(path);
            Modules.Add(path, api);
        }

        harmony = new Harmony(Owner);
        try
        {
            harmony.Patch(
                AccessTools.Method(typeof(CameraDriver), nameof(CameraDriver.CameraDriverOnGUI)),
                new HarmonyMethod(typeof(NativeCameraControls), nameof(BeforeCameraGui)));
        }
        catch (Exception error)
        {
            try
            {
                harmony.UnpatchAll(Owner);
            }
            catch (Exception cleanup)
            {
                var failure = new AggregateException("Camera controls installation and removal failed.", error, cleanup);
                LastError = RendererDiagnostics.FormatException(failure);
                throw failure;
            }
            finally
            {
                harmony = null;
            }

            LastError = RendererDiagnostics.FormatException(error);
            throw;
        }

        lastHotControl = 0;
        LastError = null;
        installed = true;
    }

    internal static void Remove()
    {
        RequireMainThread();
        installed = false;
        harmony?.UnpatchAll(Owner);
        harmony = null;
    }

    private static void RequireMainThread()
    {
        if (mainThread != 0 && mainThread != Thread.CurrentThread.ManagedThreadId)
            throw new InvalidOperationException("Native controls export must run on Unity main.");
    }

    private static void BeforeCameraGui(CameraDriver __instance)
    {
        if (!installed) return;
        try
        {
            RequireMainThread();
            lastHotControl = GUIUtility.hotControl;
            if (!CameraOwnershipAdapter.TryGetInputEpoch(__instance, out ulong epoch)) return;

            Event current = Event.current;
            if (current == null) return;

            if (current.rawType == EventType.ScrollWheel && current.type != EventType.ScrollWheel) consumedWheel++;
            // A GUI wheel event may still scroll a widget, but it never becomes
            // a second camera impulse; the native side already has the wheel.
            if (current.type == EventType.ScrollWheel && current.delta.y != 0) independentlyOwnedWheel++;

            uint flags = (KeyBindingDefOf.MapZoom_In.KeyDownEvent ? 1u : 0u)
                | (KeyBindingDefOf.MapZoom_Out.KeyDownEvent ? 2u : 0u);
            if (flags == 0) return;

            var pulse = new Impulse
            {
                Size = 48,
                Version = 1,
                Epoch = epoch,
                Sequence = checked(++api.ImpulseSequence),
                SourceFrame = (ulong)Math.Max(0, Time.frameCount),
                WheelDelta = 0,
                Flags = flags
            };

            int result = api.Queue(ref pulse, 48);
            if (result != 0) throw new InvalidOperationException("Native camera impulse was not queued: " + result);
            acceptedZoomKeys++;
            // Event.current is not used or replayed; the patched motion getter
            // already stops the game from zooming a second time.
        }
        catch (Exception error)
        {
            var failures = new List<Exception> { error };
            try
            {
                Remove();
            }
            catch (Exception cleanup)
            {
                failures.Add(cleanup);
            }
            finally
            {
                // Remove the adapter even if unpatching the controls failed.
                try
                {
                    CameraOwnershipAdapter.Remove();
                }
                catch (Exception cleanup)
                {
                    failures.Add(cleanup);
                }
            }

            Exception failure = failures.Count == 1 ? error : new AggregateException("Camera controls and cleanup failed.", failures);
            LastError = RendererDiagnostics.FormatException(failure);
            HybridSession.ReportFailure(failure, "Native camera controls");
        }
    }

    internal static int Publish(MainCameraState state)
    {
        if (!installed) return 0;
        RequireMainThread();

        policy.Size = 1088;
        policy.Version = 1;
        policy.Epoch = state.Epoch;
        policy.Revision = checked(++api.PolicyRevision);
        policy.SourceFrame = state.SourceFrame;
        policy.Width = state.PixelWidth;
        policy.Height = state.PixelHeight;
        policy.UiScale = state.UiScale;
        policy.InspectHeight = 250;
        policy.RectCount = 0;

        var wheelStatus = new WheelStatus { Size = 96, Version = 1 };
        int wheelRead = api.ReadWheel(ref wheelStatus, 96);
        if (wheelRead != 0 || wheelStatus.State == 4)
            throw new InvalidOperationException("Independent camera wheel observation fault: " + wheelRead + "/" + wheelStatus.Error);

        if ((state.Flags & (uint)MainFlags.Eligible) == 0)
        {
            policy.Flags = 4;
            int inactiveResult = api.Publish(ref policy, 1088);
            if (inactiveResult == 1) busyPolicy++;
            return inactiveResult;
        }

        bool full = Screen.fullScreen || ResolutionUtility.BorderlessFullscreen;
        policy.Flags = (full || UnityData.isEditor ? 1u : 0u) | (full ? 2u : 0u);

        CameraGeometryPolicy geometry = CameraGeometry.GetPolicy(new CameraContext(Find.CameraDriver, Find.CurrentMap))
            ?? throw new InvalidOperationException("Camera geometry policy cannot be null.");
        policy.Flags |= (uint)geometry.ReservedWheelModifiers << 4;

        // Flag 4 denies the wheel. Only an owned camera may zoom, and never
        // while text, search, a designator or a hot control has the input.
        if ((state.Flags & (uint)MainFlags.Owned) != 0)
            policy.Flags |= 8;
        else
            policy.Flags |= 4;

        if ((state.Flags & ((uint)MainFlags.MotionBlocked | (uint)MainFlags.TextCaptured | (uint)MainFlags.SearchFocused)) != 0
            || lastHotControl != 0
            || Find.DesignatorManager.SelectedDesignator != null)
            policy.Flags |= 4;

        if (Find.MainTabsRoot.OpenTab == MainButtonDefOf.Inspect
            && MainButtonDefOf.Inspect.TabWindow is MainTabWindow_Inspect inspect
            && inspect.RecentHeight > 255)
            policy.InspectHeight = inspect.RecentHeight;

        var windows = Find.WindowStack;
        if (windows.Count > policy.Rects.Length) policy.Flags = (policy.Flags & ~1u) | 4u;

        for (int i = 0; i < windows.Count && i < policy.Rects.Length; ++i)
        {
            Rect rect = windows[i].windowRect;
            policy.Rects[policy.RectCount++] = new ControlRect
            {
                Left = rect.xMin * (float)state.UiScale,
                Top = rect.yMin * (float)state.UiScale,
                Right = rect.xMax * (float)state.UiScale,
                Bottom = rect.yMax * (float)state.UiScale
            };

            if (windows[i].absorbInputAroundWindow) policy.Flags |= 4;
        }

        int result = api.Publish(ref policy, 1088);
        if (result == 1) busyPolicy++;
        return result;
    }

    public static object Snapshot()
    {
        RequireMainThread();
        var status = new Status { Size = 72, Version = 1 };
        int result = api == null ? 1 : api.Read(ref status, 72);

        var wheel = new WheelStatus { Size = 96, Version = 1 };
        int wheelResult = api == null ? 1 : api.ReadWheel(ref wheel, 96);

        return new
        {
            installed,
            LastError,
            acceptedZoomKeys,
            consumedWheel,
            busyPolicy,
            lastHotControl,
            policy,
            statusResult = result,
            native = status,
            independentlyOwnedWheel,
            wheelStatusResult = wheelResult,
            wheel,
            independentWheelBound = wheel.State == 2,
            wheelOwnership = "Exclusive OS camera channel; native Unity menu events unchanged"
        };
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct ControlRect
    {
        public float Left, Top, Right, Bottom;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct ControlPolicy
    {
        public uint Size, Version;
        public ulong Epoch, Revision, SourceFrame;
        public uint Flags, RectCount;
        public double UiScale, InspectHeight;
        public uint Width, Height;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
        public ControlRect[] Rects;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct Impulse
    {
        public uint Size, Version;
        public ulong Epoch, Sequence, SourceFrame;
        public double WheelDelta;
        public uint Flags, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct Status
    {
        public uint Size, Version, Queued, HighWater;
        public ulong Accepted, Consumed, Stale, Blocked, Full, Busy, PolicyRevision;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct WheelStatus
    {
        public uint Size, Version, State;
        public int Error;
        public ulong SourceWindow, Generation, Observed, Queued, Consumed, Denied, Stale, Overflow, ActiveEpoch;
        public uint ThreadId, Reserved;
    }

    // Blittable copy of ControlPolicy for the Mac import builder, which
    // cannot marshal a ByValArray.
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private unsafe struct PolicyPod
    {
        public uint Size, Version;
        public ulong Epoch, Revision, SourceFrame;
        public uint Flags, RectCount;
        public double UiScale, InspectHeight;
        public uint Width, Height;
        public fixed float Rects[256];
    }

    private sealed class NativeApi
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int PublishDelegate(ref ControlPolicy p, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int QueueDelegate(ref Impulse p, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int ReadDelegate(ref Status p, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int ReadWheelDelegate(ref WheelStatus p, uint size);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int PolicyDelegate(ref PolicyPod p, uint size);

        private readonly NativeModule module;
        internal readonly PublishDelegate Publish;
        internal readonly QueueDelegate Queue;
        internal readonly ReadDelegate Read;
        internal readonly ReadWheelDelegate ReadWheel;
        internal ulong PolicyRevision;
        internal ulong ImpulseSequence;

        internal unsafe NativeApi(string path)
        {
            module = NativeModule.Open(path);
            if (module.IsMac)
            {
                if (Marshal.SizeOf(typeof(PolicyPod)) != 1088 || Marshal.OffsetOf(typeof(PolicyPod), "Rects").ToInt32() != 64)
                    throw new InvalidOperationException("Native control POD ABI differs.");

                var nativePolicy = module.Bind<PolicyDelegate>("smf_camera_control_policy");
                Publish = (ref ControlPolicy p, uint size) =>
                {
                    if (size != 1088 || p.Rects == null || p.RectCount > 64 || p.Rects.Length < p.RectCount)
                        throw new ArgumentException("Invalid native control rectangle array.");

                    var pod = new PolicyPod
                    {
                        Size = p.Size,
                        Version = p.Version,
                        Epoch = p.Epoch,
                        Revision = p.Revision,
                        SourceFrame = p.SourceFrame,
                        Flags = p.Flags,
                        RectCount = p.RectCount,
                        UiScale = p.UiScale,
                        InspectHeight = p.InspectHeight,
                        Width = p.Width,
                        Height = p.Height
                    };

                    for (int i = 0; i < p.RectCount; ++i)
                    {
                        pod.Rects[i * 4] = p.Rects[i].Left;
                        pod.Rects[i * 4 + 1] = p.Rects[i].Top;
                        pod.Rects[i * 4 + 2] = p.Rects[i].Right;
                        pod.Rects[i * 4 + 3] = p.Rects[i].Bottom;
                    }

                    return nativePolicy(ref pod, 1088);
                };
            }
            else
            {
                Publish = module.Bind<PublishDelegate>("smf_camera_control_policy");
            }

            Queue = module.Bind<QueueDelegate>("smf_camera_control_impulse");
            Read = module.Bind<ReadDelegate>("smf_camera_control_status");
            ReadWheel = module.Bind<ReadWheelDelegate>("smf_camera_control_wheel_status");
        }
    }
}
