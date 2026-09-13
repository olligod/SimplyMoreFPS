#nullable disable
using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using RimWorld;
using SimplyMoreFPS.API;
using SimplyMoreFPS.Compatibility;
using SimplyMoreFPS.Rendering.Lifecycle;
using UnityEngine;
using UnityEngine.Rendering;
using Verse;

namespace SimplyMoreFPS.Rendering;

// GUI capture: redirects Unity's IMGUI repaint into the HUD/world textures and hands the frame to native at EOF.
public static partial class HybridSession
{
    internal struct GuiScope
    {
        internal Session Owner;
        internal long Token;
        internal bool TopLevel;
        internal bool Redirected;
        internal EventType Type;
        internal RenderTexture Previous;
    }

    internal struct WorldScope
    {
        internal Session Owner;
        internal RenderTexture Previous;
        internal long Token;
    }

    private static void BeginBefore(out Session __state)
    {
        __state = current;
    }

    private static void BeginAfter(int instanceID, int useGUILayout, Session __state)
    {
        if (__state != null && ReferenceEquals(current, __state))
        {
            __state.BeginContext();
            if (Event.current != null && Event.current.type == EventType.Repaint && __state.ContextDepth <= 1)
            {
                __state.ReplayScreenMeshes();
            }
        }
    }

    internal static bool TryCaptureScreenMesh(Mesh mesh, Matrix4x4 matrix, Material material, int layer)
    {
        Session session = current;
        if (session == null || !session.CaptureRouting || session.ScreenMeshes == null)
        {
            return false;
        }

        session.CheckMain();
        try
        {
            return session.ScreenMeshes.Capture(mesh, matrix, material, layer);
        }
        catch (Exception error)
        {
            try
            {
                session.ScreenMeshes.RestoreWorldDraws();
            }
            catch (Exception restoreError)
            {
                error = new AggregateException("Screen mesh capture and restoration both failed.", error, restoreError);
            }

            session.Fail(error);
            return false;
        }
    }

    private static Exception BeginFinally(Exception __exception, Session __state)
    {
        if (__exception != null && __state != null)
            __state.Fail(__exception);
        return __exception;
    }

    private static void EndBefore(out GuiScope __state)
    {
        __state = current == null ? default : current.PeekContext();
    }

    private static Exception EndFinally(Exception __exception, GuiScope __state)
    {
        if (__state.Owner != null)
            __state.Owner.EndContext(__state, __exception);
        return __exception; // never swallow or replace the game's own exception
    }

    private static void ExceptionalEnd(Exception exception)
    {
        if (current != null)
            current.ExceptionalEnd(exception);
    }

    private static void BeforeWorld(ThingOverlays __instance, out WorldScope __state)
    {
        __state = default;
        Session session = current;
        if (session == null || Event.current == null || Event.current.type != EventType.Repaint || !session.TargetHeld)
            return;

        try
        {
            __state = session.BeginWorld(__instance);
        }
        catch (Exception error)
        {
            session.Fail(error);
        }
    }

    private static Exception AfterWorld(Exception __exception, WorldScope __state)
    {
        if (__state.Owner != null)
            __state.Owner.EndWorld(__state, __exception);
        else if (__exception != null && current != null)
            current.Fail(__exception);
        return __exception;
    }

    private static void BeforeRegisteredWorld(MethodBase __originalMethod, out WorldScope __state)
    {
        __state = default;
        Session session = current;
        if (session == null || Event.current == null || Event.current.type != EventType.Repaint ||
            !session.TargetHeld || !session.HasWorldMap)
            return;

        try
        {
            __state = session.BeginRegisteredWorld();
        }
        catch (Exception error)
        {
            session.Fail(session.WorldOverlayError(__originalMethod, "begin draw", error));
        }
    }

    private static Exception AfterRegisteredWorld(MethodBase __originalMethod, Exception __exception, WorldScope __state)
    {
        Session session = __state.Owner ?? current;
        if (__state.Owner != null)
            __state.Owner.EndWorld(__state, null);
        if (__exception != null && session != null)
            session.Fail(session.WorldOverlayError(__originalMethod, "draw", __exception));
        return __exception; // keep the original exception, also in nested scopes
    }

    internal sealed partial class Session
    {
        private readonly GuiScope[] contexts = new GuiScope[64];
        internal readonly DispatchCancellationQueue<NativeDispatch> pendingCancellations = new DispatchCancellationQueue<NativeDispatch>();
        private long token;
        private WorldScope worldScope;
        private long worldToken;
        private readonly Dictionary<MethodBase, string> registeredWorldDraws = new Dictionary<MethodBase, string>();
        private readonly HashSet<MethodBase> vanillaWorldDraws = new HashSet<MethodBase>();
        private long worldOverlayRevision = -1;
        private int worldOverlaySyncFrame = -1;
        private RenderTexture originalTarget;
        private FrameBundle frameBundle;
        private int frameWorldCalls;
        private int frameWorldReturns;
        private bool frameUsable;
        private readonly CommandBuffer dispatchCommands = new CommandBuffer { name = "SMF persistent session ordered callback" };
        private int attemptedFrame = -1;
        internal ScreenMeshCapture ScreenMeshes;

        internal int ContextDepth;
        internal int WorldDepth;
        internal int Frame = -1;
        internal bool TargetHeld;
        internal long FramesQueued;
        internal long BusyFrames;
        internal long CompositeBackFrames;
        internal long Repaints;
        internal long WorldCalls;
        internal long NativeFrameMarkers;

        internal bool HasWorldMap => Context.HasMap != 0;

        private bool CaptureMatchesContext => Capture != null && Capture.Context.Revision == Context.Revision &&
            Capture.Context.Equals(Context);

        internal bool TargetsRestored
        {
            get
            {
                if (TargetHeld || worldScope.Owner != null)
                    return false;
                foreach (Generation generation in Generations.Values)
                {
                    if (generation.Coverage != null && !generation.Coverage.TargetsRestored)
                        return false;
                    if (generation.Space != null && !generation.Space.TargetsRestored)
                        return false;
                }
                return true;
            }
        }

        private void InstallRasterHooks()
        {
            Patches = new Harmony(OwnerId);

            Patches.Patch(AccessTools.Method(typeof(GUIUtility), "BeginGUI", new[] { typeof(int), typeof(int), typeof(int) }),
                prefix: new HarmonyMethod(typeof(HybridSession), nameof(BeginBefore)),
                postfix: new HarmonyMethod(typeof(HybridSession), nameof(BeginAfter)) { priority = Priority.Last },
                finalizer: new HarmonyMethod(typeof(HybridSession), nameof(BeginFinally)));
            Patches.Patch(AccessTools.Method(typeof(GUIUtility), "EndGUI", new[] { typeof(int) }),
                prefix: new HarmonyMethod(typeof(HybridSession), nameof(EndBefore)),
                finalizer: new HarmonyMethod(typeof(HybridSession), nameof(EndFinally)) { priority = Priority.Last });
            Patches.Patch(AccessTools.Method(typeof(GUIUtility), "EndGUIFromException", new[] { typeof(Exception) }),
                prefix: new HarmonyMethod(typeof(HybridSession), nameof(ExceptionalEnd)));
            MethodInfo thingOverlays = AccessTools.Method(typeof(ThingOverlays), nameof(ThingOverlays.ThingOverlaysOnGUI));
            Patches.Patch(thingOverlays,
                prefix: new HarmonyMethod(typeof(HybridSession), nameof(BeforeWorld)) { priority = Priority.First },
                finalizer: new HarmonyMethod(typeof(HybridSession), nameof(AfterWorld)) { priority = Priority.Last });
            vanillaWorldDraws.Add(thingOverlays);

            MethodInfo[] mapLabels =
            {
                AccessTools.Method(typeof(SubstructureGrid), nameof(SubstructureGrid.DrawSubstructureCountOnGUI)),
                AccessTools.Method(typeof(BeautyDrawer), nameof(BeautyDrawer.BeautyDrawerOnGUI)),
                AccessTools.Method(typeof(DeepResourceGrid), "RenderMouseAttachments"),
                AccessTools.Method(typeof(DesignationDragger), nameof(DesignationDragger.DraggerOnGUI))
            };
            foreach (MethodInfo method in mapLabels)
            {
                Patches.Patch(method,
                    prefix: new HarmonyMethod(typeof(HybridSession), nameof(BeforeRegisteredWorld)) { priority = Priority.First },
                    finalizer: new HarmonyMethod(typeof(HybridSession), nameof(AfterRegisteredWorld)) { priority = Priority.Last });
                vanillaWorldDraws.Add(method);
            }

            Compat.RegisterWorldOverlays();
            SyncWorldOverlays();
            ScreenMeshes = Compat.InstallScreenMeshHooks(Patches) ? new ScreenMeshCapture() : null;
        }

        internal void ReplayScreenMeshes()
        {
            try
            {
                ScreenMeshes?.Replay();
            }
            catch (Exception error)
            {
                Fail(error);
            }
        }

        private void SyncWorldOverlays()
        {
            CheckMain();
            if (ContextDepth != 0 || WorldDepth != 0)
                return;

            WorldOverlayApi.Snapshot snapshot = WorldOverlayApi.Current;
            if (snapshot.Revision == worldOverlayRevision)
                return;

            // Only this session's prefix/finalizer is removed from the methods it patched.
            foreach (MethodBase method in registeredWorldDraws.Keys)
            {
                try
                {
                    Patches.Unpatch(method, HarmonyPatchType.Prefix, OwnerId);
                    Patches.Unpatch(method, HarmonyPatchType.Finalizer, OwnerId);
                }
                catch (Exception error)
                {
                    throw WorldOverlayError(method, "unpatch", error);
                }
            }

            registeredWorldDraws.Clear();

            foreach (WorldOverlayApi.Registration entry in snapshot.Registrations)
            {
                // These vanilla entry points already have dedicated patches.
                if (vanillaWorldDraws.Contains(entry.DrawMethod))
                    continue;
                registeredWorldDraws.Add(entry.DrawMethod, entry.Id);

                try
                {
                    Patches.Patch(entry.DrawMethod,
                        prefix: new HarmonyMethod(typeof(HybridSession), nameof(BeforeRegisteredWorld)) { priority = Priority.First },
                        finalizer: new HarmonyMethod(typeof(HybridSession), nameof(AfterRegisteredWorld)) { priority = Priority.Last });
                }
                catch (Exception error)
                {
                    throw WorldOverlayError(entry.DrawMethod, "patch", error);
                }
            }

            worldOverlayRevision = snapshot.Revision;
        }

        internal Exception WorldOverlayError(MethodBase method, string operation, Exception error)
        {
            registeredWorldDraws.TryGetValue(method, out string id);
            return new InvalidOperationException("World overlay '" + (id ?? method.ToString()) + "' failed during " + operation + ".", error);
        }

        internal void BeginContext()
        {
            CheckMain();
            try
            {
                if (ContextDepth == 0 && WorldDepth == 0 && worldOverlaySyncFrame != Time.frameCount)
                {
                    SyncWorldOverlays();
                    worldOverlaySyncFrame = Time.frameCount;
                }

                if (Event.current == null || ContextDepth == contexts.Length)
                    throw new InvalidOperationException("Missing native GUI event or unsupported context depth.");

                var scope = new GuiScope
                {
                    Owner = this,
                    Token = checked(++token),
                    Type = Event.current.type,
                    TopLevel = ContextDepth == 0,
                    Previous = RenderTexture.active,
                };

                Core.EnterDraw();
                contexts[ContextDepth++] = scope;
                if (scope.Type != EventType.Repaint)
                    return;
                ++Repaints;

                bool firstTopLevel = scope.TopLevel && attemptedFrame != Time.frameCount;
                if (scope.TopLevel)
                {
                    ObserveContext(); // a content fence may be published even during GUI
                    attemptedFrame = Time.frameCount;
                }

                if (!CaptureRouting || !CaptureMatchesContext)
                    return;

                if (worldScope.Owner != null)
                {
                    if (scope.TopLevel || RenderTexture.active != Capture.World)
                        throw new InvalidOperationException("Nested world Repaint changed target ownership.");
                    scope.Redirected = true;
                    contexts[ContextDepth - 1] = scope;
                    return;
                }

                if (!TargetHeld)
                {
                    // Only the first top-level repaint of a frame may start a capture; a later
                    // one would copy earlier native GUI into the base.
                    if (!firstTopLevel)
                        return;
                    if (!BeginCapturedFrame())
                        return;
                }

                if (Frame != Time.frameCount)
                    throw new InvalidOperationException("Old held capture target reached a later GUI frame.");
                scope.Redirected = true;
                contexts[ContextDepth - 1] = scope;
                RenderTexture.active = Capture.Hud;
            }
            catch (Exception error)
            {
                Fail(error);
            }
        }

        private bool BeginCapturedFrame()
        {
            // Loading can replace the scene after Update and before its first Repaint.
            if (!CaptureMatchesContext || !Capture.Context.Equals(Scene.ReadContext()))
                return false;

            if (pendingCancellations.Count != 0)
            {
                ++BusyFrames;
                return false;
            }

            // The base copy must happen before any redirection; a custom top-level target is not supported.
            if (RenderTexture.active != null)
                throw new InvalidOperationException("First top-level Repaint does not target the original client framebuffer.");

            Scene.ValidateGuiAlpha();

            var bundle = new FrameBundle
            {
                Key = new FrameKey
                {
                    Session = Core.Session,
                    Content = Context.Revision,
                    Generation = Capture.Id,
                    SourceFrame = unchecked((ulong)Time.frameCount),
                },
                HudTexture = Capture.HudPointer,
                WorldTexture = Capture.WorldPointer,
                Width = Context.Width,
                Height = Context.Height,
                Flags = SystemInfo.graphicsUVStartsAtTop ? FrameFlags.FlipY : FrameFlags.None,
                PreGuiClock = Native.ClockNow(),
            };

            if (Context.HasMap != 0)
            {
                if (!Scene.TryReadMapPose(bundle.Key.SourceFrame, out bundle.Pose))
                    return false;
                ValidatePose(bundle.Pose, bundle.Key.SourceFrame);
                bundle.Flags |= FrameFlags.HasMap;
                // Coverage only attaches once it has a complete wide source; never publish an empty descriptor.
                if (Capture.Coverage != null && !Capture.Coverage.Attach(ref bundle))
                    return false;
                if (Capture.Space != null && !Capture.Space.Attach(ref bundle))
                    return false;
            }

            // A readiness transition can return to the same geometry under a newer content fence.
            if (!CaptureMatchesContext || !Capture.Context.Equals(Scene.ReadContext()))
                return false;

            if (!Accepted(Native.QueuePreGui(ref bundle, out NativeDispatch preGui), "pre-GUI base copy"))
            {
                ++BusyFrames;
                return false;
            }

            Dispatch(preGui);
            originalTarget = RenderTexture.active;
            TargetHeld = true;
            Frame = Time.frameCount;
            frameBundle = bundle;
            frameUsable = true;
            frameWorldCalls = 0;
            frameWorldReturns = 0;

            bool srgb = GL.sRGBWrite;
            try
            {
                RenderTexture.active = Capture.World;
                GL.Clear(false, true, Color.clear);
                RenderTexture.active = Capture.Hud;
                GL.Clear(false, true, Color.clear);
            }
            finally
            {
                GL.sRGBWrite = srgb;
            }

            return true;
        }

        internal GuiScope PeekContext()
        {
            return ContextDepth == 0 ? default : contexts[ContextDepth - 1];
        }

        internal void EndContext(GuiScope scope, Exception error)
        {
            CheckMain();
            if (error != null)
            {
                Fail(error);
                return;
            }

            if (ContextDepth == 0)
                return; // an exceptional boundary already handled this

            try
            {
                if (contexts[ContextDepth - 1].Token != scope.Token)
                    throw new InvalidOperationException("Native GUI Begin/End nesting mismatched.");

                contexts[--ContextDepth] = default;
                Core.LeaveDraw();
                // Windows may finish drawing after EndGUI, so the HUD target stays bound until EOF.
                if (scope.Redirected && TargetHeld)
                    RenderTexture.active = scope.TopLevel ? Capture.Hud : scope.Previous;
            }
            catch (Exception nested)
            {
                Fail(nested);
            }
        }

        internal void ExceptionalEnd(Exception error)
        {
            CheckMain();
            if (ContextDepth == 0)
                return;

            GuiScope scope = contexts[ContextDepth - 1];
            if (scope.Type == EventType.Repaint)
            {
                Fail(error);
                return;
            }

            contexts[--ContextDepth] = default;
            Core.LeaveDraw();
        }

        internal WorldScope BeginWorld(ThingOverlays overlays)
        {
            if (Find.MapUI == null || Find.MapUI.thingOverlays != overlays)
                throw new InvalidOperationException("Vanilla world GUI callback is not the current map's ThingOverlays instance.");
            return BeginRegisteredWorld();
        }

        internal WorldScope BeginRegisteredWorld()
        {
            CheckMain();
            if (Capture == null || !TargetHeld || Context.HasMap == 0 || Find.CurrentMap == null || Find.CurrentMap.uniqueID != Context.MapId ||
                ContextDepth == 0 || Frame != Time.frameCount || !CaptureMatchesContext)
                throw new InvalidOperationException("Whole world GUI callback is outside its coherent map frame.");

            if (worldScope.Owner != null)
            {
                if (WorldDepth != 1 || RenderTexture.active != Capture.World)
                    throw new InvalidOperationException("Nested world GUI callback changed target ownership.");
                return default; // the outer owner restores the target and balances the counts
            }

            if (RenderTexture.active != Capture.Hud)
                throw new InvalidOperationException("World GUI callback did not enter from the held HUD target.");

            if (!Scene.TryReadMapPose(frameBundle.Key.SourceFrame, out CameraPose pose) || !SamePose(frameBundle.Pose, pose))
                frameUsable = false;

            var scope = new WorldScope { Owner = this, Previous = RenderTexture.active, Token = checked(++worldToken) };
            worldScope = scope;
            WorldDepth = 1;
            ++frameWorldCalls;
            ++WorldCalls;
            RenderTexture.active = Capture.World;

            return scope;
        }

        internal void EndWorld(WorldScope scope, Exception error)
        {
            if (worldScope.Owner == null)
                return; // failure cleanup already released this scope

            if (!ReferenceEquals(worldScope.Owner, scope.Owner) || worldScope.Token != scope.Token)
            {
                Fail(new InvalidOperationException("World GUI finalizer does not own the current target scope."));
                return;
            }

            ++frameWorldReturns;
            if (!RestoreWorld())
                return;
            if (error != null)
                Fail(error);
        }

        private bool RestoreWorld()
        {
            if (worldScope.Owner == null)
                return true;
            try
            {
                RenderTexture.active = worldScope.Previous;
                worldScope = default;
                WorldDepth = 0;
                return true;
            }
            catch (Exception error)
            {
                CaptureRouting = false;
                RecordCleanup("World target restore", error);
                return false;
            }
        }

        internal bool RestoreTargets()
        {
            bool restoredWorld = RestoreWorld();
            if (!TargetHeld)
                return restoredWorld;

            try
            {
                RenderTexture.active = originalTarget;
                // If the world restore failed, keep the obligation so a retry goes world -> HUD -> original.
                if (restoredWorld)
                {
                    originalTarget = null;
                    TargetHeld = false;
                }

                return restoredWorld;
            }
            catch (Exception error)
            {
                CaptureRouting = false;
                RecordCleanup("Original target restore", error);
                return false;
            }
        }

        private void ClearContextsAfterFailure()
        {
            Array.Clear(contexts, 0, contexts.Length);
            ContextDepth = 0;
            frameUsable = false;
        }

        internal void EndFrame()
        {
            CheckMain();
            try
            {
                Capture?.Coverage?.FinishFrame();
                bool captured = TargetHeld && Frame == Time.frameCount;
                // A frame without IMGUI still needs its deferred edge indicators on the original target.
                if (ScreenMeshes != null && ScreenMeshes.Pending && RestoreTargets())
                {
                    ReplayScreenMeshes();
                }

                if (Core.State == Phase.Off || Core.State == Phase.Exited)
                    return;

                if (ContextDepth != 0 || WorldDepth != 0)
                    throw new InvalidOperationException("Incomplete GUI cannot become a completed frame bundle.");

                if (!RestoreTargets())
                    return;
                DrainCancellations();
                if (pendingCancellations.Count != 0)
                {
                    // Render owns the tickets, so keep scheduling its drain while cancellation retries.
                    IssueNativeEvent(0, IntPtr.Zero);
                    return;
                }

                bool completeNativeUi = !captured && !CaptureRouting;
                NativeDispatch frame = default;
                bool frameQueued = false;
                if (captured)
                {
                    if (frameWorldCalls != frameWorldReturns)
                        frameUsable = false;
                    if (!CaptureMatchesContext || frameBundle.Key.Content != Context.Revision ||
                        !Capture.Context.Equals(Scene.ReadContext()))
                        frameUsable = false;
                    if (Context.HasMap != 0 && (!Scene.TryReadMapPose(frameBundle.Key.SourceFrame, out CameraPose finalPose) ||
                        !SamePose(frameBundle.Pose, finalPose)))
                        frameUsable = false;

                    frameBundle.EofClock = Native.ClockNow();
                    frameBundle.WorldDispatches = unchecked((uint)frameWorldCalls);
                    frameBundle.Flags |= frameWorldCalls == 0 ? FrameFlags.WorldDispatchAbsent : FrameFlags.WorldDispatchCompleted;

                    // While a generation is only being prepared the original window is still shown,
                    // so composite the two finished planes back into it once.
                    if (ActiveGeneration == 0)
                    {
                        CompositeNative(Capture.World);
                        CompositeNative(Capture.Hud);
                        frameBundle.Flags |= FrameFlags.CompositeBackQueued;
                        completeNativeUi = true;
                        ++CompositeBackFrames;
                    }

                    if (frameUsable)
                    {
                        if (Accepted(Native.QueueFrame(ref frameBundle, out frame), "atomic base/world/HUD frame"))
                            frameQueued = true;
                        else
                            ++BusyFrames;
                    }
                }

                // Every source frame expires the previous Present candidate; only a complete native GUI frame can replace it.
                var marker = new NativeFrameMarker
                {
                    Session = Core.Session,
                    BeginOnly = !completeNativeUi,
                    Content = captured ? frameBundle.Key.Content : Context.Revision,
                    Generation = captured ? frameBundle.Key.Generation : 0,
                    SourceFrame = unchecked((ulong)Time.frameCount),
                    RestoreSerial = RestoreSerial,
                    Clock = Native.ClockNow(),
                    Width = unchecked((uint)Screen.width),
                    Height = unchecked((uint)Screen.height),
                };

                NativeDispatch markerDispatch = default;
                bool markerQueued = false;

                try
                {
                    // Reserve both tickets before issuing render work; execution order stays frame, then marker.
                    markerQueued = Accepted(Native.QueueNativeFrame(ref marker, out markerDispatch), "original native frame marker");

                    if (frameQueued)
                    {
                        frameQueued = false;
                        Dispatch(frame);
                        ++FramesQueued;
                    }

                    if (markerQueued)
                    {
                        markerQueued = false;
                        Dispatch(markerDispatch, true);
                        ++NativeFrameMarkers;
                    }
                }
                finally
                {
                    if (frameQueued)
                        pendingCancellations.Add(frame);
                    if (markerQueued)
                        pendingCancellations.Add(markerDispatch);
                    DrainCancellations();
                }

                // Scheduling only; this does not stand in for a Present or a retirement ack.
                IssueNativeEvent(0, IntPtr.Zero);
            }
            catch (Exception error)
            {
                Fail(error);
            }
        }

        private void Dispatch(NativeDispatch dispatch, bool bindOriginal = false)
        {
            if (dispatch.Ticket == IntPtr.Zero || dispatch.Token == 0)
                throw new InvalidOperationException("Accepted native queue returned an invalid ticket.");

            try
            {
                IssueNativeEvent(dispatch.Token, dispatch.Ticket, bindOriginal);
            }
            catch
            {
                // ExecuteCommandBuffer may have thrown before or after enqueueing; native decides which.
                pendingCancellations.Add(dispatch);

                try
                {
                    DrainCancellations();
                }
                catch (Exception cancelError)
                {
                    CleanupError = "Unissued native ticket: " + cancelError;
                }

                throw;
            }
        }

        private void IssueNativeEvent(int eventId, IntPtr ticket, bool bindOriginal = false)
        {
            dispatchCommands.Clear();
            if (bindOriginal)
                dispatchCommands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            dispatchCommands.IssuePluginEventAndData(Native.RenderEvent, eventId, ticket);
            Graphics.ExecuteCommandBuffer(dispatchCommands);
        }

        private void DrainCancellations()
        {
            // Skip the delegate allocation on the healthy path.
            if (pendingCancellations.Count != 0)
                pendingCancellations.Retry(Native.Cancel);
        }

        private void CompositeNative(RenderTexture source)
        {
            RenderTexture previous = RenderTexture.active;
            bool srgb = GL.sRGBWrite;

            try
            {
                Graphics.Blit(source, (RenderTexture)null, PremultCopy, 0);
            }
            finally
            {
                try
                {
                    RenderTexture.active = previous;
                }
                finally
                {
                    GL.sRGBWrite = srgb;
                }
            }
        }

        private void ValidatePose(CameraPose pose, ulong frame)
        {
            if (pose.Size != 264 || pose.Version != 2 || pose.UnityFrame != frame || pose.MapId != Context.MapId ||
                pose.CameraId != unchecked((ulong)Context.CameraId) || pose.Reserved != 0 || pose.RootSize <= 0 ||
                double.IsNaN(pose.RootSize) || double.IsInfinity(pose.RootSize))
                throw new InvalidOperationException("Map camera stamp is not coherent with the native GUI frame.");
        }

        // Byte compare of the blittable pose, matrices included.
        private static unsafe bool SamePose(CameraPose first, CameraPose second)
        {
            byte* a = (byte*)&first;
            byte* b = (byte*)&second;

            for (int i = 0; i < sizeof(CameraPose); ++i)
                if (a[i] != b[i])
                    return false;
            return true;
        }
    }
}
