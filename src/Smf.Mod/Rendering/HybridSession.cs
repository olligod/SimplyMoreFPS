#nullable disable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Threading;
using HarmonyLib;
using SimplyMoreFPS.Rendering.Lifecycle;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

// The persistent main-thread host: owns the native session, the capture textures and the lifecycle.
public static partial class HybridSession
{
    private static Session current;

    public static bool Installed => current != null;

    internal static bool DetachedRenderingActive => current != null && current.Core.State == Phase.Active &&
        current.Core.UserEnabled && current.Core.RenderingAllowed && !current.QuitRequested;

    internal static string LastFailure => current?.Core.LastFailure;

    internal static bool TryReadPresentation(out PresentationSample sample)
    {
        if (!Verse.UnityData.IsInMainThread)
            throw new InvalidOperationException("Presentation telemetry requires Unity main.");
        sample = default;
        Session session = current;
        if (!DetachedRenderingActive || session == null)
            return false;
        if (!(session.Native is IPresentationTelemetry telemetry))
            throw new NotSupportedException("Active renderer has no presentation telemetry capability.");

        return telemetry.TryReadPresentation(out sample) && sample.Session == session.Core.Session &&
            sample.Generation == session.ActiveGeneration && sample.Generation != 0 && sample.Frequency > 0;
    }

    public static void InstallWindows(string harmonyOwner, string nativeLibraryPath, string kernelLibraryPath,
        ulong unityWindow, string builtinExtraPath)
    {
        InstallPlatform(harmonyOwner, nativeLibraryPath, kernelLibraryPath, builtinExtraPath,
            () => new WindowsRendererApi(nativeLibraryPath, unityWindow));
    }

    public static void InstallLinux(string harmonyOwner, string nativeLibraryPath, string kernelLibraryPath,
        ulong unityWindow, string builtinExtraPath)
    {
        InstallPlatform(harmonyOwner, nativeLibraryPath, kernelLibraryPath, builtinExtraPath,
            () => new LinuxRendererApi(nativeLibraryPath, unityWindow));
    }

    public static void InstallLinux(string harmonyOwner, string nativeLibraryPath, string kernelLibraryPath,
        string builtinExtraPath)
    {
        int result = LinuxRendererApi.FindOriginalWindow(nativeLibraryPath, out ulong window);
        if (result != 0 || window == 0)
            throw new InvalidOperationException("The original Unity X11 window is not uniquely available: " + result);
        InstallLinux(harmonyOwner, nativeLibraryPath, kernelLibraryPath, window, builtinExtraPath);
    }

    public static void InstallMac(string harmonyOwner, string nativeBundleDirectory, string kernelLibraryPath,
        ulong unityWindow, string builtinExtraPath)
    {
        InstallPlatform(harmonyOwner, nativeBundleDirectory, kernelLibraryPath, builtinExtraPath,
            () => new MacRendererApi(nativeBundleDirectory, unityWindow));
    }

    public static void InstallMac(string harmonyOwner, string nativeBundleDirectory, string kernelLibraryPath,
        string builtinExtraPath)
    {
        int result = MacRendererApi.FindOriginalWindow(nativeBundleDirectory, out ulong window);
        if (result != 0 || window == 0)
            throw new InvalidOperationException("The original Unity Metal window is not uniquely available: " + result);
        InstallMac(harmonyOwner, nativeBundleDirectory, kernelLibraryPath, window, builtinExtraPath);
    }

    private static void InstallPlatform(string harmonyOwner, string nativeLibraryPath, string kernelLibraryPath,
        string builtinExtraPath, Func<INativeSession> createNative)
    {
        if (current != null)
            throw new InvalidOperationException("The persistent session is already installed.");

        var shaders = new GuiShaderResources(harmonyOwner + ".alpha", builtinExtraPath);

        try
        {
            INativeSession native = createNative();
            var scene = new RimWorldSceneOwner(nativeLibraryPath, kernelLibraryPath, shaders);
            Install(harmonyOwner, native, scene, shaders.PremultCopy);

            try
            {
                current.BindSelectionOverlay(nativeLibraryPath);
            }
            catch (Exception error)
            {
                current.Fail(error);
                throw;
            }
        }
        catch (Exception installError)
        {
            // Once a Session exists it owns the shader rollback; before that we do it here.
            if (current == null)
            {
                try
                {
                    shaders.Restore();
                }
                catch (Exception restoreError)
                {
                    throw new AggregateException("Session installation and shader rollback both failed.", installError, restoreError);
                }
            }

            throw;
        }
    }

    public static void Install(string harmonyOwner, INativeSession native, IMainSceneOwner scene, Material premultCopy)
    {
        if (current != null)
            throw new InvalidOperationException("A persistent host is already installed.");
        if (native == null || scene == null || premultCopy == null || premultCopy.shader == null || !premultCopy.shader.isSupported)
            throw new ArgumentException("Native owner, scene owner and a supported premultiplied-copy material are required.");
        if (native.RenderEvent == IntPtr.Zero || native.ClockFrequency <= 0)
            throw new ArgumentException("Native render callback and platform clock are required.");

        current = new Session(harmonyOwner, native, scene, premultCopy);

        try
        {
            current.Start();
        }
        catch (Exception error)
        {
            current.Fail(error);
            throw;
        }
    }

    // Stores intent only, so it is safe from the settings checkbox's OnGUI.
    public static void SetEnabled(bool enabled)
    {
        RequireInstalled().Core.SetEnabled(enabled);
    }

    internal static void SetRenderingAllowed(bool allowed)
    {
        RequireInstalled().Core.SetRenderingAllowed(allowed);
    }

    internal static void ReportFailure(Exception error, string source)
    {
        if (current != null)
            current.Fail(error, source);
        else
            RendererDiagnostics.Error(source, error);
    }

    public static void RequestQuit()
    {
        Session session = RequireInstalled();
        session.CheckMain();
        session.Core.RequestQuit();

        // Either Unity quit callback can run first; record the intent before the OnDisable that follows.
        session.QuitRequested = true;

        try
        {
            if (session.Native is IProcessExitFence exit)
                exit.FenceProcessExit();
        }
        catch (Exception error)
        {
            RendererDiagnostics.Error("Renderer process-exit fence failed", error);
            throw;
        }
        finally
        {
            // Unity may never render another frame: restore main-owned routing, keep uncertain resources alive.
            session.EmergencyRestore();
            session.CameraEdges?.Dispose();
            session.ReportQuitCleanup();
        }
    }

    public static void RemoveDisabledHost()
    {
        Session session = RequireInstalled();
        session.CheckMain();
        // Edge snapshots own no generation resources, but must finish before the host destroys their materials.
        if (session.Core.State != Phase.Off || session.Core.UserEnabled || session.Generations.Count != 0 ||
            session.Core.DrawDepth != 0 || !session.TargetsRestored || session.PendingSubmission.HasValue ||
            session.PendingRouting.HasValue || session.pendingCancellations.Count != 0 ||
            (session.CameraEdges != null && session.CameraEdges.Pending))
            throw new InvalidOperationException("Only a fully disabled and retired host can be removed.");

        if (session.Patches != null)
            session.Patches.UnpatchAll(session.OwnerId);
        session.CameraEdges?.Dispose();
        session.PublishSelectionState(true);
        MapCoverageCapture.RemoveHooks();
        if (session.Scene is IMainSceneLifetime sceneLifetime)
            sceneLifetime.Stop();
        current = null;
        if (session.Owner != null)
            UnityEngine.Object.Destroy(session.Owner);
    }

    public static void PumpAfterOwnerRecovery()
    {
        Session session = RequireInstalled();
        session.CheckMain();
        session.Core.RetryAfterOwnerRecovery();
        session.PendingSubmission = null;
        session.Tick();
    }

    // Consumed as JSON by the dev API; keep the keys stable.
    public static object Snapshot()
    {
        Session session = RequireInstalled();
        session.CheckMain();

        return new
        {
            phase = session.Core.State.ToString(),
            session.Core.Session,
            enabled = session.Core.UserEnabled,
            renderingAllowed = session.Core.RenderingAllowed,
            session.Core.DrawDepth,
            session.Core.WaitingForOwnerRecovery,
            session.Core.StaleAcknowledgements,
            failure = session.Core.LastFailure,
            session.CleanupError,
            session.Context,
            session.CaptureRouting,
            session.TargetHeld,
            session.Frame,
            session.ContextDepth,
            session.WorldDepth,
            session.FramesQueued,
            session.BusyFrames,
            session.CompositeBackFrames,
            session.Repaints,
            session.WorldCalls,
            session.NativeFrameMarkers,
            activeGeneration = session.ActiveGeneration,
            captureGeneration = session.Capture == null ? 0UL : session.Capture.Id,
            retainedGenerations = session.Generations.Count,
            pendingCancellations = session.pendingCancellations.Count,
            ownerAlive = session.Owner != null,
        };
    }

    private static Session RequireInstalled()
    {
        return current ?? throw new InvalidOperationException("Persistent host is not installed.");
    }

    public sealed class PersistentPump : MonoBehaviour
    {
        internal Session Session;

        public void Start()
        {
            StartCoroutine(EndFrames());
        }

        public void Update()
        {
            if (Session != null && ReferenceEquals(current, Session))
                Session.Tick();
        }

        private IEnumerator EndFrames()
        {
            var end = new WaitForEndOfFrame();
            while (Session != null && ReferenceEquals(current, Session) && Session.Core.State != Phase.Exited)
            {
                yield return end;
                Session.EndFrame();
            }
        }

        public void OnDisable()
        {
            if (Session != null && ReferenceEquals(current, Session) && !Session.QuitRequested)
                Session.Fail(new InvalidOperationException("Persistent capture pump was disabled; native routing must recover before retirement."));
        }

        public void OnApplicationQuit()
        {
            if (Session == null || !ReferenceEquals(current, Session))
                return;
            HybridSession.RequestQuit();
        }
    }

    internal sealed class Generation
    {
        internal ulong Id;
        internal ulong Session;
        internal SceneContext Context;
        internal RenderTexture Hud;
        internal RenderTexture World;
        internal ulong HudPointer;
        internal ulong WorldPointer;
        internal bool GpuRetired;
        internal bool Released;
        internal MapCoverageCapture Coverage;
    }

    internal sealed partial class Session
    {
        internal bool QuitRequested;
        internal readonly LifecycleCoordinator Core;
        internal readonly Dictionary<ulong, Generation> Generations = new Dictionary<ulong, Generation>();
        internal readonly INativeSession Native;
        internal readonly IMainSceneOwner Scene;
        internal readonly Material PremultCopy;
        internal readonly int MainThread = Thread.CurrentThread.ManagedThreadId;
        internal readonly string OwnerId;
        internal Harmony Patches;
        internal GameObject Owner;
        internal SceneContext Context;
        internal Generation Capture;
        internal ulong ActiveGeneration;
        internal ulong RestoreSerial;
        internal Command? PendingSubmission;
        internal Command? PendingRouting;
        internal WorldFence? PendingFence;
        internal bool CaptureRouting;
        internal string CleanupError;

        private bool observedContext;
        private ulong revision;
        private bool reportingFailure;
        private string reportedFailure;
        private string reportedCleanup;
        private readonly HashSet<string> cleanupFailures = new HashSet<string>();
        private Phase observedPhase;
        private ulong observedTicket;
        private long diagnosticClock;
        private double transitionWait;
        private bool reportedTransitionWait;

        internal Session(string owner, INativeSession native, IMainSceneOwner scene, Material premultCopy)
        {
            if (string.IsNullOrWhiteSpace(owner))
                throw new ArgumentException("A unique Harmony owner is required.");

            Core = new LifecycleCoordinator(native is IRetainedNativeSession retained ? retained.PreviousSession : 0);
            OwnerId = owner;
            Native = native;
            Scene = scene;
            PremultCopy = premultCopy;
        }

        internal void CheckMain()
        {
            if (Thread.CurrentThread.ManagedThreadId != MainThread)
                throw new InvalidOperationException("Only Unity main may use the persistent capture host.");
        }

        internal void Start()
        {
            CheckMain();
            ObserveContext();

            Owner = new GameObject("SMF persistent capture host");
            UnityEngine.Object.DontDestroyOnLoad(Owner);
            Owner.AddComponent<PersistentPump>().Session = this;

            if (Scene is IMainSceneLifetime sceneLifetime)
                sceneLifetime.Start(Owner);
            InstallRasterHooks();
            MapCoverageCapture.InstallHooks(OwnerId);
        }

        internal void ObserveContext()
        {
            SceneContext value = Scene.ReadContext();
            if (value.Width == 0 || value.Height == 0 || value.Width > 32768 || value.Height > 32768 ||
                value.UiScale <= 0 || float.IsNaN(value.UiScale) || float.IsInfinity(value.UiScale) || value.HasMap > 1)
                throw new InvalidOperationException("Invalid coherent full-client scene dimensions.");

            if (!observedContext || !Context.Equals(value))
            {
                value.Revision = checked(++revision);
                Context = value;
                observedContext = true;
                Core.ChangeContent(value.Revision, value.HasMap != 0);
            }

            SendFence();
            PublishSelectionState();
        }

        // Native results: 0 accepted, 1 busy, negative failure.
        private static bool Accepted(int result, string action)
        {
            if (result < 0)
                throw new InvalidOperationException(action + " failed: 0x" + result.ToString("X8"));
            if (result > 1)
                throw new InvalidOperationException(action + " returned an unknown result.");
            return result == 0;
        }

        private void SendFence()
        {
            // A fence taken from the core is kept until native accepts it; a newer one simply replaces it.
            WorldFence? latest = Core.TakeWorldFence();
            if (latest.HasValue)
                PendingFence = latest;
            if (PendingFence.HasValue && Accepted(Native.PublishWorldFence(PendingFence.Value), "content fence"))
                PendingFence = null;
        }

        internal void Tick()
        {
            CheckMain();
            try
            {
                if (ContextDepth != 0 || WorldDepth != 0)
                    throw new InvalidOperationException("GUI scopes survived into Update.");
                if (TargetHeld)
                    throw new InvalidOperationException("Captured Repaint missed its EOF; preserve leases and recover native routing.");

                DrainCancellations();
                if (!Core.Faulted)
                    ObserveContext();

                for (int i = 0; i < 4; ++i)
                {
                    if (!Accepted(Native.PollWorldFence(out WorldFence fence), "fence poll"))
                        break;
                    Core.AcknowledgeWorldFence(fence);
                }

                for (int i = 0; i < 8; ++i)
                {
                    if (!Accepted(Native.PollAcknowledgement(out Acknowledgement ack), "owner poll"))
                        break;
                    RecordOwnerAck(ack);
                    Core.Acknowledge(ack);
                }

                // A stop ticket replaces any busy older operation; the generation table survives that.
                Command? next = Core.AdvanceAtSafeBoundary(TargetsRestored);
                if (next.HasValue)
                    PendingSubmission = next;
                if (PendingSubmission.HasValue)
                    Submit(PendingSubmission.Value);
                if (PendingRouting.HasValue)
                    CompleteRouting(PendingRouting.Value);

                SendFence();
                ReportLifecycleFailure();
                ObserveTransitionProgress();
            }
            catch (Exception error)
            {
                Fail(error);
            }
        }

        private void RecordOwnerAck(Acknowledgement ack)
        {
            Command? pending = Core.Pending;
            if (!pending.HasValue || !SameTicket(pending.Value, ack) || !ack.Success || ack.Superseded)
                return;
            if ((ack.Evidence & pending.Value.Required) != pending.Value.Required)
                return;
            if (ack.Operation == Operation.ActivateGeneration)
                ActiveGeneration = ack.Generation;

            if (ack.Operation == Operation.StopWorker)
            {
                ActiveGeneration = 0;
                RestoreSerial = 0;
                Capture = null;
                CaptureRouting = false;
            }

            if (ack.Operation == Operation.RetireGenerationGpu && Generations.TryGetValue(ack.Generation, out Generation one))
                one.GpuRetired = true;

            if (ack.Operation == Operation.RetireSessionGpu)
            {
                foreach (Generation generation in Generations.Values)
                    if (generation.Session == ack.Session)
                        generation.GpuRetired = true;
            }
        }

        private static bool SameTicket(Command command, Acknowledgement ack)
        {
            return command.Session == ack.Session && command.Serial == ack.Serial &&
                command.Generation == ack.Generation && command.Operation == ack.Operation;
        }

        private void LocalAck(Command command, Evidence evidence, ulong frame)
        {
            Core.Acknowledge(new Acknowledgement
            {
                Session = command.Session,
                Serial = command.Serial,
                Generation = command.Generation,
                Operation = command.Operation,
                Evidence = evidence,
                Frame = frame,
                Success = true,
            });
        }

        private void Submit(Command command)
        {
            if (command.Operation == Operation.ReleaseGenerationMain || command.Operation == Operation.ReleaseSessionMain)
            {
                if (!TargetsRestored)
                    return;

                if (command.Operation == Operation.ReleaseGenerationMain)
                {
                    Release(RequireGeneration(command.Generation));
                }
                else
                {
                    foreach (Generation generation in new List<Generation>(Generations.Values))
                        if (generation.Session == command.Session)
                            Release(generation);
                }

                PendingSubmission = null;
                LocalAck(command, Evidence.MainResourcesReleased, unchecked((ulong)Time.frameCount));
                return;
            }

            if (command.Operation == Operation.PrepareHiddenGeneration || command.Operation == Operation.PrepareReplacementGeneration)
            {
                if (command.ContentRevision != Context.Revision)
                {
                    Core.SupersedeUnsubmittedPreparation(command, Generations.ContainsKey(command.Generation));
                    PendingSubmission = null;
                    return;
                }

                if (!Generations.TryGetValue(command.Generation, out Generation created))
                    created = CreateGeneration(command);
                if (!Accepted(Native.Submit(command, created.Context), "prepare generation"))
                    return;

                selectionSessionStarted = true;
                Core.MarkPreparationSubmitted(command);
                if (Scene is IMainSceneLifetime sceneLifetime)
                    sceneLifetime.EnableCamera();
                Capture = created;
                CaptureRouting = true;
                PendingSubmission = null;
                MapCoverageCapture.Select(created.Coverage);
                return;
            }

            if (!Accepted(Native.Submit(command, Context), "session operation " + command.Operation))
                return;

            PendingSubmission = null;
            if (command.Operation == Operation.RestoreNativeRouting)
            {
                // Native has fenced all old activation work before main routing changes; nothing is retired here.
                PendingRouting = command;
                CompleteRouting(command);
            }
        }

        private void CompleteRouting(Command command)
        {
            CaptureRouting = false;
            bool targets = false;

            try
            {
                try
                {
                    MapCoverageCapture.Select(null);
                }
                finally
                {
                    targets = RestoreTargets();
                }
            }
            finally
            {
                Scene.ReleaseCamera();
            }

            if (!targets || ContextDepth != 0 || WorldDepth != 0)
                return;
            if (!Scene.CameraReleased)
                return;

            ulong frame = unchecked((ulong)Time.frameCount);
            Evidence facts = Evidence.NativeRoutingRestored | Evidence.CaptureScopesClosed | Evidence.CameraReleased;
            if (!Accepted(Native.RoutingRestored(command, frame, facts), "routing restoration report"))
                return;

            RestoreSerial = command.Serial;
            PendingRouting = null;
        }

        private Generation RequireGeneration(ulong id)
        {
            if (!Generations.TryGetValue(id, out Generation generation))
                throw new InvalidOperationException("Unknown main resource generation.");
            return generation;
        }

        private Generation CreateGeneration(Command command)
        {
            Scene.ValidateGuiAlpha();
            var generation = new Generation { Id = command.Generation, Session = command.Session, Context = Context };
            // Register first so a partially allocated generation is still released later.
            Generations.Add(generation.Id, generation);

            generation.Hud = NewTexture("fixed GUI", generation);
            generation.World = NewTexture("world GUI", generation);

            if (Context.HasMap != 0)
            {
                generation.Coverage = new MapCoverageCapture(Context,
                    () => CaptureRouting && ReferenceEquals(Capture, generation) && generation.Context.Equals(Context), Fail);
                generation.Coverage.Create();
            }

            generation.HudPointer = unchecked((ulong)generation.Hud.GetNativeTexturePtr().ToInt64());
            generation.WorldPointer = unchecked((ulong)generation.World.GetNativeTexturePtr().ToInt64());
            if (generation.HudPointer == 0 || generation.WorldPointer == 0)
                throw new InvalidOperationException("Native capture texture is absent.");

            return generation;
        }

        private static RenderTexture NewTexture(string layer, Generation generation)
        {
            var texture = new RenderTexture((int)generation.Context.Width, (int)generation.Context.Height, 0,
                RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default)
            {
                name = "SMF session " + generation.Session + " generation " + generation.Id + " " + layer,
                antiAliasing = 1,
                useMipMap = false,
                autoGenerateMips = false,
                filterMode = FilterMode.Point,
                wrapMode = TextureWrapMode.Clamp,
            };

            if (!texture.Create())
            {
                UnityEngine.Object.Destroy(texture);
                throw new InvalidOperationException("Capture texture creation failed.");
            }

            return texture;
        }

        private void Release(Generation generation)
        {
            if (generation.Released)
                return;
            if (!generation.GpuRetired || !TargetsRestored ||
                (generation.Hud != null && RenderTexture.active == generation.Hud) ||
                (generation.World != null && RenderTexture.active == generation.World))
                throw new InvalidOperationException("Main release requires render-owner retirement and unbound targets.");

            if (ReferenceEquals(Capture, generation))
            {
                Capture = null;
                CaptureRouting = false;
            }

            if (generation.Coverage != null)
            {
                generation.Coverage.Release();
                generation.Coverage = null;
            }

            // Each field clears only after its own release succeeds, so a throw leaves the rest on the table.
            if (generation.World != null)
            {
                generation.World.Release();
                UnityEngine.Object.Destroy(generation.World);
                generation.World = null;
            }

            if (generation.Hud != null)
            {
                generation.Hud.Release();
                UnityEngine.Object.Destroy(generation.Hud);
                generation.Hud = null;
            }

            generation.Released = true;
            Generations.Remove(generation.Id);
        }

        internal void EmergencyRestore()
        {
            CheckMain();
            CaptureRouting = false;

            try
            {
                PublishSelectionState(true);
            }
            catch (Exception error)
            {
                RecordCleanup("Selection clearing", error);
            }

            try
            {
                MapCoverageCapture.Select(null);
            }
            catch (Exception error)
            {
                RecordCleanup("Map capture restoration", error);
            }

            try
            {
                RestoreTargets();
            }
            catch (Exception error)
            {
                RecordCleanup("Render target restoration", error);
            }

            try
            {
                Scene.ReleaseCamera();
            }
            catch (Exception error)
            {
                RecordCleanup("Camera release", error);
            }
        }

        private void RecordCleanup(string source, Exception error)
        {
            string detail = source + "\n" + RendererDiagnostics.FormatException(error);
            if (cleanupFailures.Add(detail))
                CleanupError = CleanupError == null ? detail : CleanupError + "\n" + detail;
        }

        internal void ReportQuitCleanup()
        {
            if (CleanupError == null || reportedCleanup == CleanupError)
                return;
            reportedCleanup = CleanupError;
            RendererDiagnostics.Error(DiagnosticContext("Renderer cleanup failed during application quit; resources remain retained"), CleanupError);
        }

        private string DiagnosticContext(string source)
        {
            string pending = Core.Pending.HasValue ? Core.Pending.Value.Operation + "/" + Core.Pending.Value.Serial : "none";
            return source + "; phase=" + Core.State +
                "; session=" + Core.Session + "; content=" + Context.Revision + "; map=" + Context.MapId +
                "; active=" + ActiveGeneration + "; capture=" + (Capture == null ? 0UL : Capture.Id) +
                "; pending=" + pending +
                "; routing=" + CaptureRouting + "; targetHeld=" + TargetHeld + "; frame=" + Time.frameCount;
        }

        // Failure reports only; the normal frame path never polls a second status copy.
        private string NativeDiagnostics()
        {
            try
            {
                const BindingFlags any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                MethodInfo read = Native.GetType().GetMethod("ReadStatus", any);
                if (read == null)
                    return "native=" + Native.GetType().Name;

                var args = new object[] { null };
                object result = read.Invoke(Native, args);
                var text = new StringBuilder("native=").Append(Native.GetType().Name).Append(" result=").Append(result);
                RendererDiagnostics.AppendPacket(text, args[0]);

                MethodInfo detail = Native.GetType().GetMethod("ReadFailureDiagnostics", any);
                if (detail != null)
                {
                    try
                    {
                        text.Append('\n').Append(detail.Invoke(Native, null));
                    }
                    catch (Exception optionalError)
                    {
                        text.Append("\nOptional native diagnostic unavailable: ").Append(optionalError.GetType().Name);
                    }
                }

                return text.ToString();
            }
            catch (Exception error)
            {
                return "Native diagnostic read failed: " + RendererDiagnostics.FormatException(error);
            }
        }

        private void ObserveTransitionProgress()
        {
            long now = Stopwatch.GetTimestamp();
            double elapsed = diagnosticClock == 0 ? 0 : (double)(now - diagnosticClock) / Stopwatch.Frequency;
            diagnosticClock = now;
            ulong ticket = Core.Pending.HasValue ? Core.Pending.Value.Serial : 0;

            if (Core.State != observedPhase || ticket != observedTicket ||
                Core.State == Phase.Off || Core.State == Phase.Active || Core.State == Phase.Exited)
            {
                observedPhase = Core.State;
                observedTicket = ticket;
                transitionWait = 0;
                reportedTransitionWait = false;
                return;
            }

            // Count responsive main-thread time only; a game load is not a renderer stall.
            if (Verse.LongEventHandler.AnyEventNowOrWaiting)
                return;

            transitionWait += Math.Min(elapsed, .1);
            if (transitionWait < 15 || reportedTransitionWait)
                return;

            reportedTransitionWait = true;
            // Report only. A timeout cannot decide GPU ownership, so leases and the ack path stay intact.
            RendererDiagnostics.Error(DiagnosticContext("Renderer transition stalled while the game is responding"), NativeDiagnostics());
        }

        private void ReportLifecycleFailure()
        {
            if (!Core.Faulted)
            {
                reportedFailure = null;
                reportedCleanup = null;
                CleanupError = null;
                cleanupFailures.Clear();
                return;
            }

            if (Core.LastFailure != null && reportedFailure != Core.LastFailure)
            {
                reportedFailure = Core.LastFailure;
                RendererDiagnostics.Fallback(DiagnosticContext("Renderer failed; restoring normal game rendering"), reportedFailure + "\n" + NativeDiagnostics());
            }

            if (CleanupError != null && reportedCleanup != CleanupError)
            {
                reportedCleanup = CleanupError;
                RendererDiagnostics.Error(DiagnosticContext("Renderer cleanup failed; resources remain retained"), reportedCleanup);
            }
        }

        internal void Fail(Exception error)
        {
            Fail(error, "Renderer capture/lifecycle");
        }

        internal void Fail(Exception error, string source)
        {
            CheckMain();
            if (reportingFailure)
                return;

            string failure = source + "\n" + RendererDiagnostics.FormatException(error);
            if (Core.Faulted && Core.LastFailure != failure)
                RecordCleanup(source, error);
            Core.ReportFailure(failure);
            PendingSubmission = null;
            reportingFailure = true;

            try
            {
                ReportLifecycleFailure();

                try
                {
                    EmergencyRestore();
                }
                catch (Exception cleanup)
                {
                    RecordCleanup("Emergency restoration", cleanup);
                }

                // Balance the core even if restore failed; TargetHeld still blocks releasing uncertain resources.
                while (Core.DrawDepth != 0)
                    Core.LeaveDraw();
                ClearContextsAfterFailure();
                ReportLifecycleFailure();
            }
            finally
            {
                reportingFailure = false;
            }
        }
    }
}
