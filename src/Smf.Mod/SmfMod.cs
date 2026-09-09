using System;
using System.IO;
using System.Reflection;
using SimplyMoreFPS.Performance;
using SimplyMoreFPS.Rendering;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS;

public sealed class SmfMod : Mod
{
    internal static string Folder = "";
    internal static SmfSettings Settings = null!;
    internal static bool NativeBaseline;

    private string fpsTargetBuffer = "";

    public SmfMod(ModContentPack content) : base(content)
    {
        Folder = content.RootDir;
        Settings = GetSettings<SmfSettings>();
        NativeBaseline = GenCommandLine.TryGetCommandLineArg("smf-native-baseline", out string baseline) && baseline == "1";

        LongEventHandler.ExecuteWhenFinished(() =>
        {
            if (!NativeBaseline) HybridRuntime.Initialize(Folder, Settings.Enabled);
            if (GenCommandLine.TryGetCommandLineArg("smf-devapi", out string port) && int.TryParse(port, out int number))
                StartDevApi(number);
        });
    }

    // The dev API is a separate assembly under Dev/ so it never loads for players.
    private static void StartDevApi(int port)
    {
        string folder = Path.Combine(Folder, "Dev");

        try
        {
            AppDomain.CurrentDomain.AssemblyResolve += (_, args) =>
            {
                string dependency = Path.Combine(folder, new AssemblyName(args.Name).Name + ".dll");
                return File.Exists(dependency) ? Assembly.LoadFrom(dependency) : null;
            };

            Assembly assembly = Assembly.LoadFrom(Path.Combine(folder, "Smf.Dev.dll"));
            assembly.GetType("SimplyMoreFPS.Dev.SmfDev", true).GetMethod("Start").Invoke(null, new object[] { port });
        }
        catch (Exception ex)
        {
            Log.Error("[SMF] Dev API could not start: " + ex);
        }
    }

    public override string SettingsCategory() => "SMF_SettingsTitle".Translate();

    public override void DoSettingsWindowContents(Rect rect)
    {
        var list = new Listing_Standard();
        list.Begin(rect);

        bool wasEnabled = Settings.Enabled;
        list.CheckboxLabeled("SMF_Enabled".Translate(), ref Settings.Enabled, "SMF_EnabledTip".Translate());
        if (Settings.Enabled)
        {
            list.Gap();
            list.CheckboxLabeled("SMF_TpsBoost".Translate(), ref Settings.TpsBoost, "SMF_TpsBoostTip".Translate());
            if (Settings.TpsBoost) DrawFpsTarget(list);
            list.Gap();
            list.CheckboxLabeled("SMF_ShowMeter".Translate(), ref Settings.ShowMeter);
        }

        list.End();
        if (wasEnabled != Settings.Enabled && !NativeBaseline) HybridRuntime.SetEnabled(Settings.Enabled);
    }

    private void DrawFpsTarget(Listing_Standard list)
    {
        if (fpsTargetBuffer.Length == 0) fpsTargetBuffer = Settings.GameFpsTarget.ToString();

        Rect row = list.GetRect(Text.LineHeight);
        row.xMin += 16f;
        string label = "SMF_FpsTarget".Translate();
        float labelWidth = Mathf.Min(Text.CalcSize(label).x + 16f, row.width * 0.45f);

        Widgets.Label(new Rect(row.x, row.y, labelWidth, row.height), label);
        Widgets.TextFieldNumeric(new Rect(row.xMax - 72f, row.y, 72f, row.height), ref Settings.GameFpsTarget, ref fpsTargetBuffer, 1, 10000);

        // The slider covers the common range; the number field accepts higher targets.
        float sliderValue = Mathf.Clamp(Settings.GameFpsTarget, 1, 60);
        var slider = new Rect(row.x + labelWidth, row.y, row.width - labelWidth - 88f, row.height);
        int selected = Mathf.RoundToInt(Widgets.HorizontalSlider(slider, sliderValue, 1f, 60f, middleAlignment: true, roundTo: 1f));
        if (selected != sliderValue)
        {
            Settings.GameFpsTarget = selected;
            fpsTargetBuffer = selected.ToString();
        }

        list.Gap(4f);
        string tip = "SMF_FpsTargetTip".Translate();
        Rect hint = list.GetRect(Text.CalcHeight(tip, row.width));
        hint.xMin += 16f;
        Widgets.Label(hint, tip);

        if (GameFrameBudget.Failure != null) list.Label("SMF_TpsBoostUnavailable".Translate());
    }
}

public sealed class SmfSettings : ModSettings
{
    public bool Enabled = true;
    public bool TpsBoost;
    public bool ShowMeter;
    public int GameFpsTarget = 15;

    public override void ExposeData()
    {
        Scribe_Values.Look(ref Enabled, "enabled", true);
        Scribe_Values.Look(ref TpsBoost, "tpsBoost", false);
        Scribe_Values.Look(ref ShowMeter, "showMeter", false);
        Scribe_Values.Look(ref GameFpsTarget, "gameFpsTarget", 15);

        if (Scribe.mode == LoadSaveMode.PostLoadInit) GameFpsTarget = Mathf.Clamp(GameFpsTarget, 1, 10000);
    }
}
