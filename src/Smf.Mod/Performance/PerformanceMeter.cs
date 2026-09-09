using System;
using System.Diagnostics;
using HarmonyLib;
using RimWorld;
using SimplyMoreFPS.Rendering;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Performance;

// FPS/TPS counter drawn under the date. FPS comes from the native presenter, not from Unity's Update rate.
internal static class PerformanceMeter
{
    private static bool installed;
    private static bool haveGameSample;
    private static bool havePresentation;
    private static Game? game;
    private static long previousClock;
    private static int previousTicks;
    private static PresentationSample previousPresentation;
    private static string text = "";

    internal static double? Fps { get; private set; }
    internal static double? Tps { get; private set; }
    internal static PresentationMetric Metric { get; private set; }

    internal static void Initialize()
    {
        if (installed) return;

        try
        {
            new Harmony("olli.simplymorefps.meter").Patch(
                AccessTools.Method(typeof(GlobalControlsUtility), nameof(GlobalControlsUtility.DoDate)),
                prefix: new HarmonyMethod(typeof(PerformanceMeter), nameof(Draw)));
            installed = true;
        }
        catch (Exception error)
        {
            RendererDiagnostics.Error("FPS/TPS meter could not start", error);
        }
    }

    internal static void Update()
    {
        bool showing = SmfMod.Settings.Enabled && SmfMod.Settings.ShowMeter
            && Current.ProgramState == ProgramState.Playing && Current.Game != null && Find.CurrentMap != null;

        if (!showing)
        {
            Reset();
            return;
        }

        long now = Stopwatch.GetTimestamp();
        if (!ReferenceEquals(game, Current.Game)) Reset();
        if (haveGameSample && (now - previousClock) / (double)Stopwatch.Frequency < 0.5) return;

        int ticks = Find.TickManager.TicksGame;
        if (haveGameSample && ticks >= previousTicks && now > previousClock)
            Tps = (ticks - previousTicks) * (double)Stopwatch.Frequency / (now - previousClock);
        else
            Tps = null;

        game = Current.Game;
        previousTicks = ticks;
        previousClock = now;
        haveGameSample = true;

        Fps = null;
        try
        {
            if (HybridSession.TryReadPresentation(out PresentationSample sample) && sample.Frequency > 0)
            {
                bool sameStream = havePresentation && sample.Session == previousPresentation.Session
                    && sample.Generation == previousPresentation.Generation && sample.Metric == previousPresentation.Metric
                    && sample.Frequency == previousPresentation.Frequency && sample.Count >= previousPresentation.Count
                    && sample.Timestamp > previousPresentation.Timestamp;

                if (sameStream)
                    Fps = (sample.Count - previousPresentation.Count) * (double)sample.Frequency / (sample.Timestamp - previousPresentation.Timestamp);

                previousPresentation = sample;
                Metric = sample.Metric;
                havePresentation = true;
            }
            else
            {
                havePresentation = false;
                if (Application.platform == RuntimePlatform.WindowsPlayer) WindowsCompositionCadence.Suspend();
            }
        }
        catch (Exception error)
        {
            // A broken meter must not take the renderer down with it.
            SmfMod.Settings.ShowMeter = false;
            Reset();
            RendererDiagnostics.Error("FPS/TPS meter sampling failed", error);
            return;
        }

        text = "SMF_MeterFormat".Translate(Fps?.ToString("0") ?? "--", Tps?.ToString("0") ?? "--");
    }

    private static void Reset()
    {
        if (haveGameSample && Application.platform == RuntimePlatform.WindowsPlayer) WindowsCompositionCadence.Suspend();

        haveGameSample = false;
        havePresentation = false;
        game = null;
        Fps = null;
        Tps = null;
        text = "";
    }

    // The controls stack upwards, so reserve our row before the date is drawn.
    private static void Draw(float leftX, float width, ref float curBaseY)
    {
        if (!SmfMod.Settings.Enabled || !SmfMod.Settings.ShowMeter || text.Length == 0) return;

        TextAnchor anchor = Text.Anchor;
        GameFont font = Text.Font;

        try
        {
            Text.Font = GameFont.Small;
            float height = Text.LineHeight;
            Text.Anchor = TextAnchor.UpperRight;
            Widgets.Label(new Rect(leftX, curBaseY - height, width - 7f, height), text);
            curBaseY -= height;
        }
        finally
        {
            Text.Anchor = anchor;
            Text.Font = font;
        }
    }
}
