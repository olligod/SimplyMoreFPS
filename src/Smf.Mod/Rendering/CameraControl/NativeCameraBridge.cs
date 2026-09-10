#nullable disable
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using HarmonyLib;
using RimWorld;
using SimplyMoreFPS.API;
using UnityEngine;
using UnityEngine.Rendering;
using Verse;

namespace SimplyMoreFPS.Rendering.CameraControl;

// Main-thread client of the native camera bridge. The native worker owns the
// NativeAOT kernel; no Unity reference or managed delegate crosses over and
// no module is ever unloaded.
public static partial class NativeCameraBridge
{
    private static readonly Func<float> ReadScrollWheelZoomRate = (Func<float>)Delegate.CreateDelegate(
        typeof(Func<float>), AccessTools.PropertyGetter(typeof(CameraDriver), "ScrollWheelZoomRate"));
    private static readonly Dictionary<string, NativeApi> Modules = new Dictionary<string, NativeApi>(NativeModule.PathComparer);
    private static readonly CameraSettings settingsScratch = new CameraSettings();
    private static Session current;

    public static object Install(string nativeDllPath, string kernelDllPath, GameObject persistentOwner)
    {
        if (current != null && !current.Stopped) throw new InvalidOperationException("Stop the previous camera bridge first.");
        if (CameraOwnershipAdapter.Installed) throw new InvalidOperationException("The camera adapter is already owned by another render session.");

        bool supported =
            (Application.platform == RuntimePlatform.WindowsPlayer && SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D11)
            || (Application.platform == RuntimePlatform.LinuxPlayer && SystemInfo.graphicsDeviceType == GraphicsDeviceType.OpenGLCore)
            || (Application.platform == RuntimePlatform.OSXPlayer && SystemInfo.graphicsDeviceType == GraphicsDeviceType.Metal);
        if (!supported || persistentOwner == null)
            throw new InvalidOperationException("A supported graphics backend and persistent game owner are required.");

        string native = Path.GetFullPath(nativeDllPath);
        string kernel = Path.GetFullPath(kernelDllPath);
        bool nativeExists = Application.platform == RuntimePlatform.OSXPlayer ? Directory.Exists(native) : File.Exists(native);
        if (!nativeExists || !File.Exists(kernel)) throw new FileNotFoundException("Both exact native modules must exist.");

        Abi.Verify();
        NativeApi api;
        if (!Modules.TryGetValue(native, out api))
        {
            api = new NativeApi(native);
            Modules.Add(native, api);
        }

        current = new Session(api, native, kernel, persistentOwner);
        try
        {
            current.Start();
        }
        catch (Exception error)
        {
            try
            {
                current.Stop();
            }
            catch (Exception cleanup)
            {
                var failure = new AggregateException("Camera bridge installation and removal failed.", error, cleanup);
                current.Error = RendererDiagnostics.FormatException(failure);
                throw failure;
            }

            current.Error = RendererDiagnostics.FormatException(error);
            throw;
        }

        return Snapshot();
    }

    public static object Snapshot()
    {
        if (current == null) return new { installed = false };
        return current.Snapshot();
    }

    public static object Stop()
    {
        if (current == null) return new { installed = false };
        current.Stop();
        return current.Snapshot();
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct KernelSettings
    {
        public uint Version, Size;
        public ulong Revision;
        public uint Flags, Reserved;
        public double MapWidth, MapHeight, PixelWidth, PixelHeight, UiScale;
        public double MinSize, MaxSize, DollyRateKeys, DollyRateScreenEdge, SpeedDecay;
        public double MoveSpeed, ZoomSpeed, ScrollWheelRate, ZoomPreserveFactor, DragSensitivity;
        public KernelProfilePack Profile;
        public KernelMovementBounds Bounds;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct Bindings
    {
        public uint Version, Size;
        public ulong Revision;
        public int Up, Up2, Down, Down2, Left, Left2, Right, Right2;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct MainBundle
    {
        public uint Size, Version;
        public ulong Publication;
        public MainCameraState State;
        public KernelSettings Settings;
        public Bindings Bindings;
        public KernelTrajectory Trajectory;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct NativeStatus
    {
        public uint Size, Version, State;
        public int Result;
        public ulong FenceEpoch, MainRevision, WorkerEpoch, SeedSequence, AppliedSequence, KernelSession;
        public ulong Steps, Adopts, Configs, MailboxDrops, CommitSequence;
        public long QpcFrequency, StepQpc, CommitQpc;
        public uint MainThread, WorkerThread, Flags, Reserved;
        public DesiredPose Desired;
    }

    private sealed class Session : ICameraWorker
    {
        private readonly NativeApi api;
        private readonly int mainThread = Thread.CurrentThread.ManagedThreadId;
        private readonly string nativePath;
        private readonly string kernelPath;
        private readonly GameObject persistentOwner;
        private ulong settingsRevision;
        private ulong bindingsRevision;
        private KernelSettings settings;
        private Bindings bindings;
        private MainBundle lastBundle;
        private long publications;
        private long busy;
        private long reads;
        private long relinquishes;
        private int lastPublishResult;
        private int lastControlPublishResult;
        private int lastReadResult;
        private int lastRelinquishResult;
        private bool adapterStarted;
        private bool controlsStarted;
        internal bool Stopped;
        internal string Error;

        internal Session(NativeApi api, string nativePath, string kernelPath, GameObject persistentOwner)
        {
            this.api = api;
            this.nativePath = nativePath;
            this.kernelPath = kernelPath;
            this.persistentOwner = persistentOwner;
        }

        private void RequireMainThread()
        {
            if (Thread.CurrentThread.ManagedThreadId != mainThread)
                throw new InvalidOperationException("The bridge client only runs on Unity's main thread.");
        }

        internal double Now()
        {
            RequireMainThread();
            return api.Now();
        }

        internal void Start()
        {
            RequireMainThread();
            int result = api.Init(kernelPath, checked((uint)kernelPath.Length));
            if (result != 0) throw new InvalidOperationException("Native camera bridge initialization failed: " + result);

            NativeCameraControls.Install(nativePath);
            controlsStarted = true;

            CameraOwnershipAdapter.Install(persistentOwner, this);
            adapterStarted = true;
        }

        internal void Stop()
        {
            RequireMainThread();
            if (Stopped) return;

            // Removes the controls and the adapter only. The compositor worker
            // and its images are not touched here.
            var failures = new List<Exception>();
            try
            {
                if (controlsStarted) NativeCameraControls.Remove();
            }
            catch (Exception cleanup)
            {
                failures.Add(cleanup);
            }
            finally
            {
                controlsStarted = false;

                try
                {
                    if (adapterStarted && CameraOwnershipAdapter.Installed) CameraOwnershipAdapter.Remove();
                }
                catch (Exception cleanup)
                {
                    failures.Add(cleanup);
                }
                finally
                {
                    adapterStarted = false;
                    Stopped = true;
                }
            }

            if (failures.Count != 0)
            {
                var failure = new AggregateException("Camera bridge removal failed.", failures);
                Error = (Error ?? "") + "\n" + RendererDiagnostics.FormatException(failure);
                throw failure;
            }
        }

        public int ReadDesired(ref DesiredPose pose)
        {
            RequireMainThread();
            if (Stopped) return 1;

            pose = new DesiredPose { Version = DesiredPose.VersionValue, Size = DesiredPose.ByteSize };
            reads++;
            lastReadResult = api.Desired(ref pose, DesiredPose.ByteSize);
            return lastReadResult;
        }

        public int PublishMain(ref MainCameraState state)
        {
            RequireMainThread();
            if (Stopped) return unchecked((int)0x80004005);

            int controlResult = NativeCameraControls.Publish(state);
            lastControlPublishResult = controlResult;
            if (controlResult < 0) return controlResult;

            KernelSettings nextSettings = ReadSettings(state);
            Bindings nextBindings = ReadBindings();
            if (settingsRevision == 0 || !SameSettings(settings, nextSettings)) settingsRevision = checked(settingsRevision + 1);
            if (bindingsRevision == 0 || !SameBindings(bindings, nextBindings)) bindingsRevision = checked(bindingsRevision + 1);

            nextSettings.Revision = settingsRevision;
            nextBindings.Revision = bindingsRevision;
            settings = nextSettings;
            bindings = nextBindings;

            lastBundle = new MainBundle
            {
                Size = 2176,
                Version = 2,
                Publication = api.NextPublication(),
                State = state,
                Settings = settings,
                Bindings = bindings,
                Trajectory = CameraOwnershipAdapter.Trajectory
            };

            publications++;
            lastPublishResult = api.Publish(ref lastBundle, 2176);
            if (lastPublishResult == 1) busy++;
            // Busy is not retried here; the next adapter boundary publishes fresh state.
            return lastPublishResult;
        }

        public int Relinquish(ulong epoch, uint reason)
        {
            RequireMainThread();
            relinquishes++;
            lastRelinquishResult = api.Revoke(epoch, reason);
            return lastRelinquishResult;
        }

        internal object Snapshot()
        {
            RequireMainThread();
            var status = new NativeStatus { Size = 232, Version = 2 };
            int result = api.Status(ref status, 232);
            if (result == 0 && (status.Size != 232 || status.Version != 2))
                throw new InvalidOperationException("Native camera status ABI mismatch.");

            return new
            {
                installed = adapterStarted && CameraOwnershipAdapter.Installed,
                Stopped,
                Error,
                nativePath,
                kernelPath,
                mainThread,
                publications,
                busy,
                reads,
                relinquishes,
                lastPublishResult,
                lastControlPublishResult,
                lastReadResult,
                lastRelinquishResult,
                statusResult = result,
                lastBundle,
                native = status,
                adapter = CameraOwnershipAdapter.Snapshot(),
                controls = NativeCameraControls.Snapshot()
            };
        }
    }

    private static KernelSettings ReadSettings(MainCameraState state)
    {
        // Read on the same publication as the pose. Without a matching map the
        // bundle stays header-only; native only configures the kernel when Eligible.
        var result = new KernelSettings
        {
            Version = 2,
            Size = 1920,
            PixelWidth = state.PixelWidth,
            PixelHeight = state.PixelHeight,
            UiScale = state.UiScale,
            MinSize = state.MinSize,
            MaxSize = state.MaxSize
        };

        // Loading still publishes an inactive bundle; foreign geometry needs a ready map.
        if (!MapSceneReadiness.Ready)
        {
            return result;
        }

        Map map = Find.CurrentMap;
        CameraDriver driver = Find.CameraDriver;
        if (map == null || driver == null || driver.config == null || map.uniqueID != state.MapId) return result;

        CameraMapConfig config = driver.config;
        var context = new CameraContext(driver, map);
        var extent = CameraGeometry.GetMovementExtent(context);
        extent.RequireValid();

        result.MapWidth = extent.Width;
        result.MapHeight = extent.Height;

        var values = settingsScratch;
        values.MinSize = result.MinSize;
        values.MaxSize = result.MaxSize;
        values.DollyRateKeys = config.dollyRateKeys;
        values.DollyRateScreenEdge = config.dollyRateScreenEdge;
        values.SpeedDecay = config.camSpeedDecayFactor;
        values.MoveSpeed = config.moveSpeedScale;
        values.ZoomSpeed = config.zoomSpeed;
        values.ScrollWheelRate = ReadScrollWheelZoomRate();
        values.ZoomPreserveFactor = config.zoomPreserveFactor;
        values.DragSensitivity = Prefs.MapDragSensitivity;
        values.SmoothZoom = config.smoothZoom;
        values.ZoomToMouse = Prefs.ZoomToMouse;
        values.EdgeScroll = Prefs.EdgeScreenScroll;

        var policy = CameraOwnershipAdapter.CurrentResolution.Policy;
        policy.Scalars?.ApplyTo(values);

        CameraGeometryPolicy geometry = CameraGeometry.GetPolicy(context)
            ?? throw new InvalidOperationException("Camera geometry policy cannot be null.");
        geometry.ApplyTo(values);
        var profile = policy.Profile;

        result.MinSize = values.MinSize;
        result.MaxSize = values.MaxSize;
        result.DollyRateKeys = values.DollyRateKeys;
        result.DollyRateScreenEdge = values.DollyRateScreenEdge;
        result.SpeedDecay = values.SpeedDecay;
        result.MoveSpeed = values.MoveSpeed;
        result.ZoomSpeed = values.ZoomSpeed;
        result.ScrollWheelRate = values.ScrollWheelRate;
        result.ZoomPreserveFactor = values.ZoomPreserveFactor;
        result.DragSensitivity = values.DragSensitivity;

        result.Flags = (values.SmoothZoom ? 1u : 0u)
            | ((profile?.ZoomToMouse ?? values.ZoomToMouse) ? 2u : 0u)
            | (values.EdgeScroll ? 4u : 0u)
            | (profile?.DisableZoomToMouseWhileShiftHeld == true ? 8u : 0u);
        result.Profile = EncodeProfile(profile);
        result.Bounds = EncodeBounds(values.Bounds);

        return result;
    }

    private static Bindings ReadBindings()
    {
        // Text focus is handled by the worker through the main flags; it must
        // not change bindings or their revision.
        var result = new Bindings { Version = 1, Size = 48 };
        ReadKeys(KeyBindingDefOf.MapDolly_Up, out result.Up, out result.Up2);
        ReadKeys(KeyBindingDefOf.MapDolly_Down, out result.Down, out result.Down2);
        ReadKeys(KeyBindingDefOf.MapDolly_Left, out result.Left, out result.Left2);
        ReadKeys(KeyBindingDefOf.MapDolly_Right, out result.Right, out result.Right2);

        return result;
    }

    private static void ReadKeys(KeyBindingDef definition, out int primary, out int secondary)
    {
        primary = 0;
        secondary = 0;

        if (definition == null || KeyPrefs.KeyPrefsData == null || KeyPrefs.KeyPrefsData.keyPrefs == null) return;
        if (KeyPrefs.KeyPrefsData.keyPrefs.TryGetValue(definition, out var binding))
        {
            primary = (int)binding.keyBindingA;
            secondary = (int)binding.keyBindingB;
        }
    }

    private static bool SameBindings(Bindings a, Bindings b)
    {
        return a.Up == b.Up && a.Up2 == b.Up2
            && a.Down == b.Down && a.Down2 == b.Down2
            && a.Left == b.Left && a.Left2 == b.Left2
            && a.Right == b.Right && a.Right2 == b.Right2;
    }

    private static bool SameSettings(KernelSettings a, KernelSettings b)
    {
        return a.Flags == b.Flags
            && a.MapWidth == b.MapWidth
            && a.MapHeight == b.MapHeight
            && a.PixelWidth == b.PixelWidth
            && a.PixelHeight == b.PixelHeight
            && a.UiScale == b.UiScale
            && a.MinSize == b.MinSize
            && a.MaxSize == b.MaxSize
            && a.DollyRateKeys == b.DollyRateKeys
            && a.DollyRateScreenEdge == b.DollyRateScreenEdge
            && a.SpeedDecay == b.SpeedDecay
            && a.MoveSpeed == b.MoveSpeed
            && a.ZoomSpeed == b.ZoomSpeed
            && a.ScrollWheelRate == b.ScrollWheelRate
            && a.ZoomPreserveFactor == b.ZoomPreserveFactor
            && a.DragSensitivity == b.DragSensitivity
            && SameProfile(a.Profile, b.Profile)
            && SameBounds(a.Bounds, b.Bounds);
    }

    // Sizes and offsets are a contract with the native side.
    private static class Abi
    {
        internal static void Verify()
        {
            Size<DesiredPose>(88);
            Size<MainCameraState>(112);
            Size<KernelSettings>(1920);
            Size<KernelCurve>(320);
            Size<KernelProfilePack>(1608);
            Size<KernelTrajectory>(80);
            Size<KernelAxisBounds>(80);
            Size<KernelMovementBounds>(168);

            Offset<KernelSettings>(nameof(KernelSettings.Bounds), 1752);
            Offset<KernelAxisBounds>(nameof(KernelAxisBounds.CollapsePosition), 72);
            Offset<KernelMovementBounds>(nameof(KernelMovementBounds.Z), 80);
            Offset<KernelMovementBounds>(nameof(KernelMovementBounds.MaximumSize), 160);

            Size<Bindings>(48);
            Size<MainBundle>(2176);
            Size<NativeStatus>(232);

            Offset<MainBundle>(nameof(MainBundle.State), 16);
            Offset<MainBundle>(nameof(MainBundle.Settings), 128);
            Offset<MainBundle>(nameof(MainBundle.Bindings), 2048);
            Offset<MainBundle>(nameof(MainBundle.Trajectory), 2096);
            Offset<NativeStatus>(nameof(NativeStatus.Desired), 144);
            Offset<Bindings>(nameof(Bindings.Up), 16);
            Offset<KernelSettings>(nameof(KernelSettings.MapWidth), 24);
        }

        private static void Size<T>(int bytes)
        {
            if (Marshal.SizeOf(typeof(T)) != bytes)
                throw new InvalidOperationException("Bridge size mismatch: " + typeof(T).Name);
        }

        private static void Offset<T>(string field, int bytes)
        {
            if (Marshal.OffsetOf(typeof(T), field).ToInt64() != bytes)
                throw new InvalidOperationException("Bridge offset mismatch: " + typeof(T).Name + "." + field);
        }
    }

    // Bound once per module and kept for the process lifetime. Only the main
    // thread calls these; native never calls back into them.
    private sealed class NativeApi
    {
        internal delegate int InitDelegate(string kernelPath, uint characters);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int InitWideDelegate([MarshalAs(UnmanagedType.LPWStr)] string kernelPath, uint characters);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int InitUtf8Delegate(IntPtr kernelPath, uint bytes);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int PublishDelegate(ref MainBundle value, uint bytes);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int RevokeDelegate(ulong epoch, uint reason);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int DesiredDelegate(ref DesiredPose value, uint bytes);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int StatusDelegate(ref NativeStatus value, uint bytes);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate double NowDelegate();

        private readonly NativeModule module;
        internal readonly InitDelegate Init;
        internal readonly PublishDelegate Publish;
        internal readonly RevokeDelegate Revoke;
        internal readonly DesiredDelegate Desired;
        internal readonly StatusDelegate Status;
        internal readonly NowDelegate Now;
        private ulong publication;

        internal NativeApi(string path)
        {
            module = NativeModule.Open(path);
            if (Application.platform == RuntimePlatform.WindowsPlayer)
            {
                var wide = module.Bind<InitWideDelegate>("smf_camera_bridge_init");
                Init = (kernel, characters) => wide(kernel, characters);
            }
            else
            {
                var utf8 = module.Bind<InitUtf8Delegate>("smf_camera_bridge_init");
                Init = (kernel, unused) =>
                {
                    if (kernel.IndexOf('\0') >= 0) throw new ArgumentException("Invalid native kernel path.");

                    byte[] bytes = Encoding.UTF8.GetBytes(kernel);
                    var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);

                    try
                    {
                        return utf8(pin.AddrOfPinnedObject(), checked((uint)bytes.Length));
                    }
                    finally
                    {
                        pin.Free();
                    }
                };
            }

            Publish = module.Bind<PublishDelegate>("smf_camera_bridge_publish");
            Revoke = module.Bind<RevokeDelegate>("smf_camera_bridge_revoke");
            Desired = module.Bind<DesiredDelegate>("smf_camera_bridge_desired");
            Status = module.Bind<StatusDelegate>("smf_camera_bridge_status");
            Now = module.Bind<NowDelegate>("smf_camera_bridge_now");
        }

        // The native module keeps its state across Stop/Install of the same
        // path, so the publication counter lives here rather than per Session.
        internal ulong NextPublication()
        {
            return checked(++publication);
        }
    }
}
