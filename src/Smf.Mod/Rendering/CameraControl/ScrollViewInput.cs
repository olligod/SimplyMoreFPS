#nullable disable
using System;
using System.Threading;
using HarmonyLib;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering.CameraControl;

internal static class ScrollViewInput
{
    private static readonly Rect[] Rects = new Rect[64];
    private static Func<Rect, Rect> unclip;
    private static int mainThread;
    private static int frame = -1;
    private static int count;
    private static int width;
    private static int height;
    private static float scale;
    private static bool overflow;

    internal static void Install(Harmony harmony, int thread)
    {
        mainThread = thread;
        Clear();

        Type clip = typeof(GUI).Assembly.GetType("UnityEngine.GUIClip", true);
        unclip = (Func<Rect, Rect>)Delegate.CreateDelegate(typeof(Func<Rect, Rect>),
            AccessTools.Method(clip, "UnclipToWindow", new[] { typeof(Rect) }));

        harmony.Patch(AccessTools.Method(typeof(Widgets), nameof(Widgets.BeginScrollView)),
            prefix: new HarmonyMethod(typeof(ScrollViewInput), nameof(BeforeScrollView)) { priority = Priority.Last });
    }

    internal static void Clear()
    {
        frame = -1;
        count = 0;
        overflow = false;
    }

    private static void BeforeScrollView(Rect outRect)
    {
        if (Thread.CurrentThread.ManagedThreadId != mainThread
            || Event.current == null || Event.current.type != EventType.Repaint)
        {
            return;
        }

        if (frame != Time.frameCount)
        {
            Clear();
            frame = Time.frameCount;
            width = Screen.width;
            height = Screen.height;
            scale = Prefs.UIScale;
        }

        // Unity consumes wheel input across the full view, even outside a parent clip.
        Rect rect = unclip(outRect);
        rect = Rect.MinMaxRect(Mathf.Max(0, rect.xMin), Mathf.Max(0, rect.yMin),
            Mathf.Min(width, rect.xMax), Mathf.Min(height, rect.yMax));
        if (!(rect.width > 0 && rect.height > 0))
        {
            return;
        }

        if (count == Rects.Length)
        {
            overflow = true;
            return;
        }

        Rects[count++] = rect;
    }

    internal static void Append(ref NativeCameraControls.ControlPolicy policy)
    {
        // Update uses the preceding repaint; a closed list must not leave a stale input region.
        if (frame < Time.frameCount - 1 || width != policy.Width || height != policy.Height || scale != policy.UiScale)
        {
            return;
        }

        if (overflow || policy.RectCount + count > policy.Rects.Length)
        {
            policy.Flags |= 4;
            return;
        }

        for (int i = 0; i < count; i++)
        {
            Rect rect = Rects[i];
            policy.Rects[policy.RectCount++] = new NativeCameraControls.ControlRect
            {
                Left = rect.xMin,
                Top = rect.yMin,
                Right = rect.xMax,
                Bottom = rect.yMax
            };
        }
    }
}
