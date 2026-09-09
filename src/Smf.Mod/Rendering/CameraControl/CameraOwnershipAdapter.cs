#nullable disable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Threading;
using HarmonyLib;
using RimWorld.Planet;
using SimplyMoreFPS.API;
using UnityEngine;
using UnityEngine.LowLevel;
using Verse;

namespace SimplyMoreFPS.Rendering.CameraControl;

// Hands the game's CameraDriver pose to the native worker and applies the
// worker's poses back. Only PODs cross to native; nothing is delegated.
public static partial class CameraOwnershipAdapter
{
    private const string OwnerId = "olli.simplymorefps.rendering.camera-ownership.v1";

    private static readonly AccessTools.FieldRef<CameraDriver, Vector3> Position = AccessTools.FieldRefAccess<CameraDriver, Vector3>("rootPos");
    private static readonly AccessTools.FieldRef<CameraDriver, Vector3> Velocity = AccessTools.FieldRefAccess<CameraDriver, Vector3>("velocity");
    private static readonly AccessTools.FieldRef<CameraDriver, Vector2> Dolly = AccessTools.FieldRefAccess<CameraDriver, Vector2>("desiredDolly");
    private static readonly AccessTools.FieldRef<CameraDriver, Vector2> RawDolly = AccessTools.FieldRefAccess<CameraDriver, Vector2>("desiredDollyRaw");
    private static readonly AccessTools.FieldRef<CameraDriver, float> DesiredSize = AccessTools.FieldRefAccess<CameraDriver, float>("desiredSize");
    private static readonly AccessTools.FieldRef<CameraDriver, float> BottomEdgeTime = AccessTools.FieldRefAccess<CameraDriver, float>("mouseTouchingScreenBottomEdgeStartTime");
    private static readonly AccessTools.FieldRef<CameraDriver, List<CameraDriver.DragTimeStamp>> Drags = AccessTools.FieldRefAccess<CameraDriver, List<CameraDriver.DragTimeStamp>>("dragTimeStamps");
    private static readonly AccessTools.FieldRef<CameraDriver, CameraPanner> Panner = AccessTools.FieldRefAccess<CameraDriver, CameraPanner>("panner");
    private static readonly Func<bool> ReadTextCapture = (Func<bool>)Delegate.CreateDelegate(
        typeof(Func<bool>), AccessTools.PropertyGetter(typeof(GUIUtility), "textFieldInput"));

    private static Harmony harmony;
    private static ICameraWorker worker;
    private static ForeignCameraChanges foreignChanges;
    private static CameraAdapterLifetime lifetime;
    private static CameraDriver driver;
    private static Map map;
    private static int mainThread;
    private static int beforeFrame = -1;
    private static int afterFrame = -1;
    private static int applying;
    private static int externalDepth;
    private static int pendingRemoval;
    private static Exception pendingFailure;
    private static ulong epoch;
    private static ulong acceptedSequence;
    private static ulong completedSequence;
    private static bool owned;
    private static bool knownPose;
    private static bool lastEligible;
    private static bool needsInputClear;
    private static bool contextKnown;
    private static bool relinquishConfirmed = true;
    private static Vector3 expectedPosition;
    private static float expectedSize;
    private static ReleaseReason reason;

    public static bool Installed { get; private set; }
    public static string LastError { get; private set; }
    public static long BusyPublications { get; private set; }
    public static int LastPublicationResult { get; private set; }
    internal static CameraResolution CurrentResolution { get; private set; }

    public static bool Released => !Installed && !owned && !needsInputClear && relinquishConfirmed;

    public static void Install(GameObject rootObject, ICameraWorker cameraWorker)
    {
        if (Installed) throw new InvalidOperationException("Camera adapter already installed.");
        if (rootObject == null) throw new ArgumentException("A persistent session owner is required.");
        if (cameraWorker == null) throw new ArgumentNullException(nameof(cameraWorker));
        if (Marshal.SizeOf(typeof(DesiredPose)) != DesiredPose.ByteSize
            || Marshal.SizeOf(typeof(MainCameraState)) != MainCameraState.ByteSize)
            throw new InvalidOperationException("Camera POD layout mismatch.");

        mainThread = Thread.CurrentThread.ManagedThreadId;
        worker = cameraWorker;
        LastError = null;
        BusyPublications = 0;
        LastPublicationResult = 0;

        beforeFrame = -1;
        afterFrame = -1;
        acceptedSequence = 0;
        completedSequence = 0;
        applying = 0;
        externalDepth = 0;
        pendingRemoval = 0;
        pendingFailure = null;

        Volatile.Write(ref foreignChanges, new ForeignCameraChanges());
        knownPose = false;
        owned = false;
        lastEligible = false;
        needsInputClear = false;
        contextKnown = false;
        driver = null;
        map = null;

        harmony = new Harmony(OwnerId);
        Installed = true;
        try
        {
            Patch(AccessTools.PropertyGetter(typeof(CameraDriver), "AnythingPreventsCameraMotion"), nameof(BlockNativeCamera));
            Patch(AccessTools.Method(typeof(CameraDriver), "Update"), nameof(BeforeCameraUpdate), null, nameof(AfterCameraUpdate));
            Patch(AccessTools.Method(typeof(PlayerLoop), "SetPlayerLoop"), null, nameof(LoopChanged));

            string[] setters = { "JumpToCurrentMapLoc", "SetRootPosAndSize", "SetRootSize", "ResetSize", "PanToMapLoc", "PanToMapLocAndSize" };
            foreach (string name in setters)
            {
                foreach (MethodInfo method in typeof(CameraDriver).GetMethods(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly))
                {
                    if (method.Name == name) Patch(method, nameof(BeforeExternalCall), null, nameof(AfterExternalCall));
                }
            }

            CameraPoseLoop.Install(BeforeNativeInput, AfterNativeInput);
            lifetime = rootObject.AddComponent<CameraAdapterLifetime>();
            lifetime.hideFlags = HideFlags.HideAndDontSave;
            RefreshContext();
            Publish();
        }
        catch (Exception error)
        {
            // Undo the partial install before the caller sees the fault.
            try
            {
                Remove();
            }
            catch (Exception cleanup)
            {
                var failure = new AggregateException("Camera adapter installation and removal failed.", error, cleanup);
                LastError = RendererDiagnostics.FormatException(failure);
                throw failure;
            }

            LastError = RendererDiagnostics.FormatException(error);
            throw;
        }
    }

    private static void Patch(MethodBase method, string prefix = null, string postfix = null, string finalizer = null)
    {
        if (method == null) throw new MissingMethodException("Required camera adapter hook is absent.");
        harmony.Patch(
            method,
            prefix == null ? null : new HarmonyMethod(typeof(CameraOwnershipAdapter), prefix),
            postfix == null ? null : new HarmonyMethod(typeof(CameraOwnershipAdapter), postfix),
            null,
            finalizer == null ? null : new HarmonyMethod(typeof(CameraOwnershipAdapter), finalizer));
    }

    private static void BeforeNativeInput()
    {
        if (!Installed) return;
        try
        {
            RequireMainThread();
            if (Interlocked.Exchange(ref pendingRemoval, 0) != 0)
            {
                CompletePendingFailure();
                return;
            }

            if (beforeFrame == Time.frameCount || beforeFrame != afterFrame)
                throw new InvalidOperationException("Camera input boundary repeated or its completion was missed.");

            beforeFrame = Time.frameCount;
            RefreshContext();
            RefreshPolicy();
            ConsumeForeignCameraChanges();
            ObservePose();

            if (!Eligible())
            {
                if (owned || lastEligible) Release(ReleaseReason.UnsupportedState);
                lastEligible = false;
                return;
            }

            lastEligible = true;
            DesiredPose pose = default;
            int result = worker.ReadDesired(ref pose);
            if (result == 1) return; // Nothing new; keep the last accepted pose.
            if (result != 0) throw new InvalidOperationException("Desired camera read failed: " + result);

            if (pose.Version != DesiredPose.VersionValue || pose.Size != DesiredPose.ByteSize || pose.Flags != 0
                || pose.Reserved != 0 || (pose.PanFlags & ~1u) != 0)
                throw new InvalidOperationException("Malformed camera packet header.");
            if (!PoseValidation.Matches(pose, epoch, map.uniqueID, acceptedSequence)) return;
            if (!MatchesScriptedPan(pose)) return;
            if (!PoseValidation.CanConvertToUnity(pose))
                throw new InvalidOperationException("Camera packet cannot be converted to a finite positive-size Unity pose.");

            applying++;
            needsInputClear = true;
            try
            {
                // SetRootPosAndSize takes raw rootSize and applies the transform
                // without a second zoom-to-mouse pass.
                driver.SetRootPosAndSize(new Vector3((float)pose.X, Position(driver).y, (float)pose.Z), (float)pose.RootSize);
                ClearCameraInput(driver);
            }
            finally
            {
                applying--;
            }

            owned = true;
            acceptedSequence = pose.Sequence;
            RememberPose();
            CompleteScriptedPan(pose);
        }
        catch (Exception error)
        {
            Fail(error);
        }
    }

    private static void AfterNativeInput()
    {
        if (!Installed) return;
        try
        {
            RequireMainThread();
            if (beforeFrame != Time.frameCount || afterFrame == beforeFrame)
                throw new InvalidOperationException("Camera input completion ran out of order.");

            ObservePose();
            if (owned) completedSequence = acceptedSequence;
            // The worker must see the pose that was actually applied this
            // frame before it advances.
            Publish();
        }
        catch (Exception error)
        {
            Fail(error);
        }
        finally
        {
            afterFrame = beforeFrame;
        }
    }

    // Prefix on CameraDriver.AnythingPreventsCameraMotion. Only this getter is
    // overridden; input events and Input.GetKey stay untouched.
    private static bool BlockNativeCamera(CameraDriver __instance, ref bool __result)
    {
        if (!Installed || !owned || ForeignChangePending || __instance != driver) return true;
        if (beforeFrame != Time.frameCount)
        {
            Fail(new InvalidOperationException("Camera ownership survived a missing pose boundary."));
            return true;
        }

        __result = true;
        return false;
    }

    private struct UpdateState
    {
        internal bool Entered;
        internal ulong Epoch;
        internal Vector3 Velocity;
    }

    private static void BeforeCameraUpdate(CameraDriver __instance, ref UpdateState __state)
    {
        if (Installed && owned && (beforeFrame != Time.frameCount || afterFrame != beforeFrame))
        {
            Fail(new InvalidOperationException("Camera update preceded coherent native GUI dispatch."));
            return;
        }

        if (Installed && owned && __instance == driver)
            __state = new UpdateState { Entered = true, Epoch = epoch, Velocity = Velocity(driver) };
    }

    private static Exception AfterCameraUpdate(CameraDriver __instance, UpdateState __state, Exception __exception)
    {
        if (!Installed || __instance != driver) return __exception;
        try
        {
            // Edge scrolling can still write velocity while the getter blocks
            // translation. Drop it so it cannot leak into the next owned frame.
            if (__state.Entered && owned && __state.Epoch == epoch)
            {
                Velocity(driver) = __state.Velocity;
                Drags(driver).Clear();
            }

            ObservePose();
        }
        catch (Exception error)
        {
            Fail(error);
        }

        return __exception; // Never mask the game's own exception.
    }

    private struct ExternalCallState
    {
        internal bool MainEntered;
        internal bool KeepScriptedPan;
        internal ForeignCameraChanges Foreign;
    }

    private static void BeforeExternalCall(CameraDriver __instance, MethodBase __originalMethod, ref ExternalCallState __state)
    {
        // Unity's loader calls these setters from its own thread. Do not touch
        // Unity objects, not even for a reference compare, before this check.
        if (Thread.CurrentThread.ManagedThreadId != Volatile.Read(ref mainThread))
        {
            var changes = Volatile.Read(ref foreignChanges);
            if (changes != null)
            {
                __state.Foreign = changes;
                changes.Begin();
            }

            return;
        }

        if (!Installed || applying != 0 || __instance != driver) return;
        __state.MainEntered = true;

        if (externalDepth++ == 0)
        {
            try
            {
                bool pan = __originalMethod.Name == "PanToMapLoc" || __originalMethod.Name == "PanToMapLocAndSize";
                __state.KeepScriptedPan = pan && owned && Eligible();
                if (!__state.KeepScriptedPan) Release(ReleaseReason.ExternalCameraCall);
            }
            catch (Exception error)
            {
                Fail(error);
            }
        }
    }

    private static Exception AfterExternalCall(CameraDriver __instance, ExternalCallState __state, Exception __exception)
    {
        if (__state.Foreign != null)
        {
            __state.Foreign.Complete();
            return __exception;
        }

        if (Thread.CurrentThread.ManagedThreadId != Volatile.Read(ref mainThread) || !__state.MainEntered) return __exception;
        externalDepth = Math.Max(0, externalDepth - 1);

        if (Installed && externalDepth == 0 && __instance == driver)
        {
            try
            {
                // The call replaced the game's panner. Drop the old capture
                // before a provider change could restore it.
                if (__state.KeepScriptedPan && __exception == null && Panner(driver).Moving)
                    ReleaseScriptedPan(false);
                RefreshPolicy();

                if (__state.KeepScriptedPan)
                {
                    if (__exception == null && SupportsMotion() && Panner(driver).Moving)
                        CaptureScriptedPan();
                    else
                        Release(ReleaseReason.ExternalCameraCall);
                }

                RememberPose();
                Publish();
            }
            catch (Exception error)
            {
                Fail(error);
            }
        }

        return __exception;
    }

    private static bool ForeignChangePending => foreignChanges != null && foreignChanges.Pending;

    private static void ConsumeForeignCameraChanges()
    {
        RequireMainThread();
        var changes = foreignChanges;
        if (changes == null || !changes.Consume()) return;

        Release(ReleaseReason.ExternalCameraCall);
        knownPose = false;
        if (!changes.InFlight) RememberPose();
    }

    private static void RefreshContext()
    {
        var nextDriver = Find.CameraDriver;
        var nextMap = Find.CurrentMap;
        if (contextKnown && driver == nextDriver && map == nextMap) return;

        // The title screen has no driver or map, but the blocked input policy
        // still needs a fenced nonzero epoch, so release before switching.
        Release(ReleaseReason.MapChanged);

        driver = nextDriver;
        map = nextMap;
        contextKnown = true;
        knownPose = false;
        lastEligible = false;
        RememberPose();
    }

    private static bool MotionBlocked()
    {
        // Do not read the patched AnythingPreventsCameraMotion getter here.
        return ForeignChangePending
            || !MapSceneReadiness.Ready
            || driver == null
            || driver.config == null
            || Find.WindowStack == null
            || !Current.Game.PlayerHasControl
            || Find.WindowStack.WindowsPreventCameraMotion
            || WorldRendererUtility.WorldSelected;
    }

    private static bool Eligible()
    {
        return SupportsMotion() && !Panner(driver).Moving;
    }

    private static bool SupportsMotion()
    {
        return !ForeignChangePending
            && driver != null
            && map != null
            && MapSceneReadiness.Ready
            && Find.WindowStack != null
            && !WorldRendererUtility.WorldSelected
            && driver.config != null
            && CurrentResolution?.Policy.AllowDetachedMotion == true
            && !driver.config.followSelected
            && driver.config.autoPanSpeed == 0f
            && !driver.config.gravshipFreeCam;
    }

    private static void RefreshPolicy()
    {
        var next = CameraProviders.Resolve(driver, map);
        if (CurrentResolution != null
            && (CurrentResolution.ProviderId != next.ProviderId
                || CurrentResolution.Policy.AllowDetachedMotion != next.Policy.AllowDetachedMotion))
            Release(ReleaseReason.UnsupportedState);
        CurrentResolution = next;
    }

    private static void RememberPose()
    {
        if (driver == null)
        {
            knownPose = false;
            return;
        }

        expectedPosition = Position(driver);
        expectedSize = driver.RootSize;
        knownPose = true;
    }

    private static void ObservePose()
    {
        if (driver == null) return;

        Vector3 current = Position(driver);
        if (knownPose && (current.x != expectedPosition.x || current.z != expectedPosition.z || driver.RootSize != expectedSize))
            Release(ReleaseReason.NativePoseChanged);
        RememberPose();
    }

    private static void ClearCameraInput(CameraDriver camera)
    {
        Velocity(camera) = Vector3.zero;
        Dolly(camera) = Vector2.zero;
        RawDolly(camera) = Vector2.zero;
        DesiredSize(camera) = camera.RootSize;
        Drags(camera).Clear();
        BottomEdgeTime(camera) = -1f;
    }

    private static void Release(ReleaseReason why)
    {
        RequireMainThread();
        bool wasOwned = owned || needsInputClear;

        owned = false; // First, so the game's own camera always works again.
        acceptedSequence = 0;
        completedSequence = 0;
        reason = why;
        epoch = checked(epoch + 1);
        relinquishConfirmed = worker == null;

        Exception clearError = null;
        try
        {
            ReleaseScriptedPan(why == ReleaseReason.Removed || why == ReleaseReason.UnsupportedState || why == ReleaseReason.ExternalCameraCall);
            if (wasOwned && driver != null) ClearCameraInput(driver);
            needsInputClear = false;
        }
        catch (Exception error)
        {
            clearError = error;
        }

        try
        {
            // Fence the worker regardless of how the Unity cleanup went.
            if (worker != null)
            {
                if (worker.Relinquish(epoch, (uint)why) != 0)
                    throw new InvalidOperationException("Worker refused camera relinquishment.");
                relinquishConfirmed = true;
            }
        }
        catch (Exception error)
        {
            if (clearError != null)
                throw new AggregateException("Clearing camera input and releasing the camera both failed.", clearError, error);
            throw;
        }

        if (clearError != null) ExceptionDispatchInfo.Capture(clearError).Throw();
    }

    private static void Publish()
    {
        RefreshContext();
        ConsumeForeignCameraChanges();
        RefreshPolicy();

        MainFlags flags = MainFlags.None;
        if (Eligible()) flags |= MainFlags.Eligible;
        if (owned) flags |= MainFlags.Owned;
        if (MotionBlocked()) flags |= MainFlags.MotionBlocked;
        if (ReadTextCapture()) flags |= MainFlags.TextCaptured;
        if (Find.WindowStack?.AnySearchWidgetFocused == true) flags |= MainFlags.SearchFocused;
        if (driver != null && Panner(driver).Moving) flags |= MainFlags.NativePan;

        Vector3 pos = driver == null ? Vector3.zero : Position(driver);
        var state = new MainCameraState
        {
            Version = MainCameraState.VersionValue,
            Size = MainCameraState.ByteSize,
            Epoch = epoch,
            AppliedSequence = completedSequence,
            SourceFrame = (ulong)Math.Max(0, Time.frameCount),
            MapId = map == null ? -1 : map.uniqueID,
            Flags = (uint)flags,
            X = pos.x,
            Z = pos.z,
            RootSize = driver == null ? 0 : driver.RootSize,
            ProjectionHalfHeight = driver == null || driver.MyCamera == null ? 0 : driver.MyCamera.orthographicSize,
            MinSize = driver == null || driver.config == null ? 0 : driver.config.sizeRange.min,
            MaxSize = driver == null || driver.config == null ? 0 : driver.config.sizeRange.max,
            UiScale = Prefs.UIScale,
            PixelWidth = (uint)Math.Max(0, Screen.width),
            PixelHeight = (uint)Math.Max(0, Screen.height),
            Reason = (uint)reason
        };

        var scalars = CurrentResolution.Policy.Scalars;
        if (scalars?.MinSize != null) state.MinSize = scalars.MinSize.Value;
        if (scalars?.MaxSize != null) state.MaxSize = scalars.MaxSize.Value;

        int result = worker.PublishMain(ref state);
        if (result == 0 && (flags & MainFlags.Eligible) != 0) relinquishConfirmed = false;
        LastPublicationResult = result;
        if (result == 1)
        {
            // Mailbox busy. The next boundary publishes a fresh state anyway.
            BusyPublications++;
            return;
        }

        if (result != 0) throw new InvalidOperationException("Main camera publication failed: " + result);
    }

    // Identity of the pose the current source frame was rendered with.
    // Sequence zero means no worker pose has been applied yet in this epoch.
    public static bool TryGetCaptureStamp(out ulong cameraEpoch, out ulong appliedSequence, out int mapId)
    {
        cameraEpoch = 0;
        appliedSequence = 0;
        mapId = -1;

        if (!Installed) return false;
        RequireMainThread();
        if (ForeignChangePending) return false;
        if (driver == null || map == null || Find.CameraDriver != driver || Find.CurrentMap != map
            || !knownPose || epoch == 0 || beforeFrame != Time.frameCount || afterFrame != beforeFrame)
            return false;

        Vector3 position = Position(driver);
        if (position.x != expectedPosition.x || position.z != expectedPosition.z || driver.RootSize != expectedSize)
            return false;

        cameraEpoch = epoch;
        appliedSequence = owned ? acceptedSequence : 0;
        mapId = map.uniqueID;
        return true;
    }

    // Called from CameraDriverOnGUI, which runs inside the input dispatch and
    // before the frame stamp is final. It only says whether input may be sent.
    public static bool TryGetInputEpoch(CameraDriver camera, out ulong cameraEpoch)
    {
        cameraEpoch = 0;
        if (!Installed) return false;
        RequireMainThread();
        if (!owned || camera != driver || Find.CameraDriver != driver || Find.CurrentMap != map
            || beforeFrame != Time.frameCount || !Eligible() || MotionBlocked())
            return false;

        cameraEpoch = epoch;
        return true;
    }

    public static object Snapshot()
    {
        if (mainThread != 0) RequireMainThread();
        return new
        {
            Installed,
            LastError,
            owned,
            epoch,
            acceptedSequence,
            completedSequence,
            beforeFrame,
            afterFrame,
            reason,
            BusyPublications,
            LastPublicationResult,
            mapId = map == null ? -1 : map.uniqueID,
            provider = CurrentResolution?.ProviderId,
            trajectory = Trajectory,
            knownPose,
            expectedX = expectedPosition.x,
            expectedZ = expectedPosition.z,
            expectedSize
        };
    }

    public static CameraStatus GetPublicStatus()
    {
        if (!UnityData.IsInMainThread) throw new InvalidOperationException("Camera status requires Unity's main thread.");
        return new CameraStatus(
            owned,
            Installed,
            CurrentResolution?.ProviderId ?? "vanilla",
            LastError ?? HybridSession.LastFailure ?? "",
            map == null ? -1 : map.uniqueID);
    }

    internal static void EndFrame(CameraAdapterLifetime caller)
    {
        if (!Installed || caller != lifetime) return;
        try
        {
            RequireMainThread();
            RefreshContext();
            ObservePose();
            Publish();
        }
        catch (Exception error)
        {
            Fail(error);
        }
    }

    internal static void CheckLifecycle(CameraAdapterLifetime caller)
    {
        if (!Installed || caller != lifetime) return;
        try
        {
            RequireMainThread();
            if (Interlocked.Exchange(ref pendingRemoval, 0) != 0)
            {
                CompletePendingFailure();
                return;
            }

            if (lifetime == null || !lifetime.gameObject.activeInHierarchy)
                throw new InvalidOperationException("Camera adapter root lifetime ended.");
        }
        catch (Exception error)
        {
            Fail(error);
        }
    }

    internal static void LifetimeEnded(CameraAdapterLifetime caller)
    {
        if (!Installed || caller != lifetime) return;
        Fail(new InvalidOperationException("Camera adapter lifetime ended unexpectedly."));
    }

    private static void LoopChanged()
    {
        if (!Installed || CameraPoseLoop.Editing) return;
        try
        {
            RequireMainThread();
            if (!CameraPoseLoop.IsIntact()) throw new InvalidOperationException("Native camera input boundary was replaced.");
        }
        catch (Exception error)
        {
            Fail(error);
        }
    }

    private static void RequireMainThread()
    {
        if (Thread.CurrentThread.ManagedThreadId != mainThread)
            throw new InvalidOperationException("Camera adapter requires Unity's main thread.");
    }

    private static void Fail(Exception error)
    {
        if (Thread.CurrentThread.ManagedThreadId != mainThread)
        {
            // Off-thread (a foreign SetPlayerLoop): stop suppressing now and
            // let the main-thread lifecycle do the removal.
            owned = false;
            Interlocked.CompareExchange(ref pendingFailure, error, null);
            Interlocked.Exchange(ref pendingRemoval, 1);
            return;
        }

        try
        {
            Remove();
        }
        catch (Exception cleanup)
        {
            error = new AggregateException("Camera adapter failure and removal failure.", error, cleanup);
        }

        LastError = RendererDiagnostics.FormatException(error);
        HybridSession.ReportFailure(error, "Camera ownership adapter");
    }

    private static void CompletePendingFailure()
    {
        var error = Interlocked.Exchange(ref pendingFailure, null);
        if (error == null)
            Remove();
        else
            Fail(error);
    }

    public static void Remove()
    {
        RequireMainThread();
        if (!Installed) return;

        Installed = false;
        Interlocked.Exchange(ref foreignChanges, null);

        var failures = new List<Exception>();
        // Each step runs even if an earlier one failed. Other mods' loop nodes
        // and patches are left alone.
        TryCleanup(() => Release(ReleaseReason.Removed), failures);
        TryCleanup(CameraPoseLoop.Remove, failures);
        TryCleanup(() => harmony.UnpatchAll(OwnerId), failures);
        var oldLifetime = lifetime;
        lifetime = null;
        if (oldLifetime != null) TryCleanup(() => UnityEngine.Object.Destroy(oldLifetime), failures);

        worker = null;
        driver = null;
        map = null;
        knownPose = false;

        if (failures.Count != 0)
        {
            var failure = new AggregateException("Camera ownership removal failed.", failures);
            LastError = (LastError ?? "") + "\n" + RendererDiagnostics.FormatException(failure);
            throw failure;
        }
    }

    private static void TryCleanup(Action action, List<Exception> failures)
    {
        try
        {
            action();
        }
        catch (Exception error)
        {
            failures.Add(error);
        }
    }
}

// Lives on the session root object and drives the adapter's end-of-frame
// and lifecycle checks. No native callback enters it.
public sealed class CameraAdapterLifetime : MonoBehaviour
{
    private IEnumerator Start()
    {
        var endOfFrame = new WaitForEndOfFrame();
        while (CameraOwnershipAdapter.Installed)
        {
            yield return endOfFrame;
            CameraOwnershipAdapter.EndFrame(this);
        }
    }

    private void Update()
    {
        CameraOwnershipAdapter.CheckLifecycle(this);
    }

    private void OnDisable()
    {
        CameraOwnershipAdapter.LifetimeEnded(this);
    }

    private void OnDestroy()
    {
        CameraOwnershipAdapter.LifetimeEnded(this);
    }
}
