#nullable disable
using System;
using System.Collections.Generic;
using UnityEngine.LowLevel;
using UnityEngine.PlayerLoop;

namespace SimplyMoreFPS.Rendering.CameraControl;

// Adds a node before and after Unity's native IMGUI dispatch. The dispatch
// node itself is never wrapped, replaced or invoked by us.
internal static class CameraPoseLoop
{
    private struct BeforeNativeInput
    {
    }

    private struct AfterNativeInput
    {
    }

    private static PlayerLoopSystem.UpdateFunction before;
    private static PlayerLoopSystem.UpdateFunction after;
    private static IntPtr nativeDispatch;
    internal static bool Editing;

    internal static void Install(PlayerLoopSystem.UpdateFunction prepare, PlayerLoopSystem.UpdateFunction complete)
    {
        before = prepare;
        after = complete;

        int count = 0;
        var loop = Insert(PlayerLoop.GetCurrentPlayerLoop(), ref count);
        if (count != 1) throw new InvalidOperationException("Expected one native IMGUI dispatch node.");

        Apply(loop);
        if (!IsIntact()) throw new InvalidOperationException("Camera pose boundary was not retained.");
    }

    private static PlayerLoopSystem Insert(PlayerLoopSystem node, ref int count)
    {
        if (node.subSystemList == null) return node;

        var list = new List<PlayerLoopSystem>(node.subSystemList.Length + 2);
        foreach (var child in node.subSystemList)
        {
            if (IsOurNode(child)) throw new InvalidOperationException("Duplicate camera pose installation.");

            if (child.type == typeof(PreUpdate.IMGUISendQueuedEvents))
            {
                if (child.updateDelegate != null || child.updateFunction == IntPtr.Zero)
                    throw new InvalidOperationException("Native IMGUI dispatch has been replaced.");

                count++;
                nativeDispatch = child.updateFunction;
                list.Add(new PlayerLoopSystem { type = typeof(BeforeNativeInput), updateDelegate = before });
                list.Add(child);
                list.Add(new PlayerLoopSystem { type = typeof(AfterNativeInput), updateDelegate = after });
            }
            else
            {
                list.Add(Insert(child, ref count));
            }
        }

        node.subSystemList = list.ToArray();
        return node;
    }

    internal static bool IsIntact()
    {
        int befores = 0;
        int afters = 0;
        int dispatches = 0;
        int pairs = 0;

        Inspect(PlayerLoop.GetCurrentPlayerLoop(), ref befores, ref afters, ref dispatches, ref pairs);
        return befores == 1 && afters == 1 && dispatches == 1 && pairs == 1;
    }

    private static void Inspect(PlayerLoopSystem node, ref int befores, ref int afters, ref int dispatches, ref int pairs)
    {
        if (node.type == typeof(BeforeNativeInput)) befores++;
        if (node.type == typeof(AfterNativeInput)) afters++;
        if (node.type == typeof(PreUpdate.IMGUISendQueuedEvents)) dispatches++;

        var children = node.subSystemList;
        if (children == null) return;

        for (int i = 0; i < children.Length; i++)
        {
            var child = children[i];
            if (i > 0 && i + 1 < children.Length && IsWrappedDispatch(children[i - 1], child, children[i + 1]))
                pairs++;
            Inspect(child, ref befores, ref afters, ref dispatches, ref pairs);
        }
    }

    private static bool IsWrappedDispatch(PlayerLoopSystem previous, PlayerLoopSystem dispatch, PlayerLoopSystem next)
    {
        return dispatch.type == typeof(PreUpdate.IMGUISendQueuedEvents)
            && dispatch.updateFunction == nativeDispatch
            && dispatch.updateDelegate == null
            && previous.type == typeof(BeforeNativeInput)
            && previous.updateDelegate == before
            && previous.updateFunction == IntPtr.Zero
            && previous.subSystemList == null
            && next.type == typeof(AfterNativeInput)
            && next.updateDelegate == after
            && next.updateFunction == IntPtr.Zero
            && next.subSystemList == null;
    }

    internal static void Remove()
    {
        Apply(Strip(PlayerLoop.GetCurrentPlayerLoop()));
    }

    private static PlayerLoopSystem Strip(PlayerLoopSystem node)
    {
        if (node.subSystemList == null) return node;

        var list = new List<PlayerLoopSystem>(node.subSystemList.Length);
        foreach (var child in node.subSystemList)
        {
            if (!IsOurNode(child)) list.Add(Strip(child));
        }

        node.subSystemList = list.ToArray();
        return node;
    }

    private static bool IsOurNode(PlayerLoopSystem node)
    {
        return node.type == typeof(BeforeNativeInput) || node.type == typeof(AfterNativeInput);
    }

    private static void Apply(PlayerLoopSystem loop)
    {
        // The SetPlayerLoop postfix must not treat our own edit as tampering.
        Editing = true;

        try
        {
            PlayerLoop.SetPlayerLoop(loop);
        }
        finally
        {
            Editing = false;
        }
    }
}
