#nullable disable
using System;
using System.Runtime.InteropServices;
using System.Threading;
using HarmonyLib;
using RimWorld;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

// Drag-selection box: published to native as state so it can be drawn there instead of into the HUD capture.
public static partial class HybridSession
{
    private static ulong selectionPublication;
    private static ulong selectionDrag;

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct SelectionState
    {
        internal uint Size;
        internal uint Version;
        internal ulong Session;
        internal ulong Content;
        internal ulong Drag;
        internal ulong Publication;
        internal int MapId;
        internal uint Active;
        internal double X;
        internal double Z;
        internal float UiScale;
        internal float R;
        internal float G;
        internal float B;
        internal float A;
        internal uint Reserved;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int PublishSelection(ref SelectionState state, uint bytes);

    private static void AfterSelectionInput()
    {
        Session session = current;

        // Starting dialogs can open on the loading thread; Update publishes their state safely.
        if (session == null || Thread.CurrentThread.ManagedThreadId != session.MainThread)
            return;

        try
        {
            session.PublishSelectionState();
        }
        catch (Exception error)
        {
            session.Fail(error);
        }
    }

    private static bool BeforeSelectionDraw(DragBox __instance)
    {
        Session session = current;
        return session == null || session.BeforeSelectionDraw(__instance);
    }

    internal sealed partial class Session
    {
        private PublishSelection publishSelection;
        private bool selectionWasActive;
        private bool redrawingSelection;
        private bool selectionSessionStarted;
        private Vector3 selectionStart;
        private ulong selectionSerial;
        private Color selectionColor = Color.white;

        internal bool BeforeSelectionDraw(DragBox instance)
        {
            if (redrawingSelection || publishSelection == null ||
                Event.current == null || Event.current.type != EventType.Repaint ||
                !TargetHeld || !CaptureRouting || Capture == null ||
                !ReferenceEquals(instance, Find.Selector?.dragBox))
                return true;

            try
            {
                selectionColor = GUI.color;
                if (!PublishSelectionState())
                {
                    // Native did not take the state, so this frame must not be sealed without the outline.
                    frameUsable = false;
                    return true;
                }

                if (ActiveGeneration == 0)
                {
                    // The original window is still on screen during preparation; draw the box there, not into the HUD.
                    RenderTexture previous = RenderTexture.active;
                    redrawingSelection = true;

                    try
                    {
                        RenderTexture.active = originalTarget;
                        instance.DragBoxOnGUI();
                    }
                    finally
                    {
                        RenderTexture.active = previous;
                        redrawingSelection = false;
                    }
                }

                return false;
            }
            catch (Exception error)
            {
                Fail(error);
                return true;
            }
        }

        internal void BindSelectionOverlay(string path)
        {
            CheckMain();
            if (Marshal.SizeOf(typeof(SelectionState)) != 88)
                throw new InvalidOperationException("Selection overlay ABI mismatch.");

            try
            {
                publishSelection = NativeModule.Open(path).Bind<PublishSelection>("smf_selection_publish");
            }
            catch (EntryPointNotFoundException)
            {
                return; // this renderer build draws the box the vanilla way
            }

            Patches.Patch(AccessTools.Method(typeof(Selector), nameof(Selector.SelectorOnGUI)),
                postfix: new HarmonyMethod(typeof(HybridSession), nameof(AfterSelectionInput)) { priority = Priority.Last });
            Patches.Patch(AccessTools.Method(typeof(Selector), nameof(Selector.Notify_DialogOpened)),
                postfix: new HarmonyMethod(typeof(HybridSession), nameof(AfterSelectionInput)) { priority = Priority.Last });
            Patches.Patch(AccessTools.Method(typeof(DragBox), nameof(DragBox.DragBoxOnGUI)),
                prefix: new HarmonyMethod(typeof(HybridSession), nameof(BeforeSelectionDraw)) { priority = Priority.Last });
        }

        internal bool PublishSelectionState(bool clear = false)
        {
            CheckMain();

            // The native session only starts with the first accepted prepare; before that there is nobody to publish to.
            if (publishSelection == null || !selectionSessionStarted) return false;

            // Find.Selector casts Find.Root to Root_Play, so guard the root type first.
            DragBox drag = !clear && Context.HasMap != 0 && MapSceneReadiness.Ready && Find.Root is Root_Play
                ? Find.Selector?.dragBox
                : null;
            bool active = !clear && Core.UserEnabled && CaptureRouting && Capture != null &&
                Context.HasMap != 0 && Capture.Context.Equals(Context) && drag != null && drag.active;

            if (active && (!selectionWasActive || selectionStart != drag.start))
            {
                selectionSerial = checked(++selectionDrag);
                selectionStart = drag.start;
            }

            selectionWasActive = active;

            var state = new SelectionState
            {
                Size = 88,
                Version = 1,
                Session = Core.Session,
                Content = Context.Revision,
                Drag = selectionSerial,
                Publication = checked(++selectionPublication),
                MapId = Context.HasMap != 0 ? Context.MapId : -1,
                Active = active ? 1u : 0u,
                X = active ? selectionStart.x : 0,
                Z = active ? selectionStart.z : 0,
                UiScale = Context.UiScale,
                R = Mathf.Clamp01(selectionColor.r),
                G = Mathf.Clamp01(selectionColor.g),
                B = Mathf.Clamp01(selectionColor.b),
                A = Mathf.Clamp01(selectionColor.a),
            };

            return Accepted(publishSelection(ref state, 88), "selection overlay publication");
        }
    }
}
