using System;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal static class RendererFailureNotice
{
    private static string? pending;
    private static string? explanation;
    private static Dialog_MessageBox? visible;

    internal static void Record(string message)
    {
        if (pending == null || explanation != null)
        {
            pending = message;
            explanation = null;
        }
    }

    internal static void RecordCompatibility(string reason)
    {
        if (pending == null)
        {
            pending = reason;
            explanation = "SMF_RendererCompatibility".Translate(reason);
        }
    }

    internal static void Update()
    {
        if (pending == null || LongEventHandler.AnyEventNowOrWaiting || Find.WindowStack == null)
        {
            return;
        }

        if (Current.ProgramState != ProgramState.Entry && Current.ProgramState != ProgramState.Playing)
        {
            return;
        }

        if (visible != null && Find.WindowStack.Windows.Contains(visible))
        {
            return;
        }

        // Failures can arrive inside OnGUI or loading. Open the notice later from Update.
        string report = pending;
        string message = explanation ?? "SMF_RendererFallback".Translate();
        string copyLabel = (explanation == null ? "SMF_CopyError" : "SMF_CopyDetails").Translate();
        pending = null;
        explanation = null;

        try
        {
            visible = new Dialog_MessageBox(
                message,
                buttonAText: copyLabel,
                buttonAAction: () => GUIUtility.systemCopyBuffer = report,
                buttonBText: "Close".Translate(),
                title: "SMF_SettingsTitle".Translate());
            visible.closeOnCancel = true;
            Find.WindowStack.Add(visible);
        }
        catch (Exception error)
        {
            visible = null;
            RendererDiagnostics.Error("Could not show the renderer failure notice", error);
        }
    }
}
