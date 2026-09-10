using System;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal static class RendererFailureNotice
{
    private static string? pending;
    private static Dialog_MessageBox? visible;

    internal static void Record(string message)
    {
        pending ??= message;
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
        pending = null;

        try
        {
            visible = new Dialog_MessageBox(
                "SMF_RendererFallback".Translate(),
                buttonAText: "SMF_CopyError".Translate(),
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
