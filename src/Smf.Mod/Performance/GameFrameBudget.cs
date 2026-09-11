using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using SimplyMoreFPS.Compatibility;
using Verse;

namespace SimplyMoreFPS.Performance;

// Optional TPS boost: swaps the per-frame simulation budget in TickManagerUpdate for 1000 / target FPS.
internal static class GameFrameBudget
{
    internal const string OwnerId = "olli.simplymorefps.game-frame-budget";

    private static Harmony? harmony;
    private static bool lastRequested;
    private static bool rendererRunning;
    private static bool matched;
    private static float budgetMs;

    internal static bool Installed { get; private set; }
    internal static string? Failure { get; private set; }
    internal static bool Active => Installed && matched && rendererRunning && Failure == null;

    internal static void Update(bool requested, int targetFps, bool rendererActive)
    {
        if (!UnityData.IsInMainThread) throw new InvalidOperationException("Game frame budget requires Unity main.");
        if (targetFps < 1 || targetFps > 10000) throw new ArgumentOutOfRangeException(nameof(targetFps));

        rendererRunning = rendererActive;
        budgetMs = 1000f / targetFps;
        if (requested == lastRequested) return;
        lastRequested = requested;

        MethodInfo target = AccessTools.Method(typeof(TickManager), "TickManagerUpdate");

        if (!requested)
        {
            rendererRunning = false;

            // Only this transpiler is removed; other mods' patches stay.
            try
            {
                harmony?.Unpatch(target, HarmonyPatchType.Transpiler, OwnerId);
                Installed = false;
                matched = false;
                Failure = null;
            }
            catch (Exception error)
            {
                Report("removal failed: " + error, target);
            }

            return;
        }

        Failure = null;
        matched = false;

        try
        {
            harmony = harmony ?? new Harmony(OwnerId);
            var transpiler = new HarmonyMethod(typeof(GameFrameBudget), nameof(Transpiler))
            {
                priority = Priority.Last,
                after = GameFrameBudgetCompatibility.PatchOwners,
            };

            harmony.Patch(target, transpiler: transpiler);
            Installed = true;
        }
        catch (Exception error)
        {
            Report("installation failed: " + error, target);
        }
    }

    internal static float SelectBudget(float upstream) => Active ? budgetMs : upstream;

    internal static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> instructions, MethodBase __originalMethod)
    {
        var codes = instructions.ToList();
        var matches = new List<int>();
        MethodInfo elapsed = AccessTools.PropertyGetter(typeof(Stopwatch), nameof(Stopwatch.ElapsedMilliseconds));

        for (int i = 0; i + 3 < codes.Count; i++)
        {
            CodeInstruction call = codes[i];
            if (call.opcode != OpCodes.Callvirt && call.opcode != OpCodes.Call) continue;
            if (!Equals(call.operand, elapsed)) continue;
            if (codes[i + 1].opcode != OpCodes.Conv_R4 || !IsBudget(codes[i + 2])) continue;
            if (codes[i + 3].opcode != OpCodes.Bgt && codes[i + 3].opcode != OpCodes.Bgt_S) continue;
            matches.Add(i + 2);
        }

        if (matches.Count != 1)
        {
            matched = false;
            Report("expected exactly one Stopwatch.ElapsedMilliseconds / conv.r4 / "
                + "[45.454544f, " + GameFrameBudgetCompatibility.BudgetOperands + "] / bgt budget comparison; found "
                + matches.Count, __originalMethod);
            return codes;
        }

        // The original budget load stays in place for other transpilers; SelectBudget consumes it.
        codes.Insert(matches[0] + 1, new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(GameFrameBudget), nameof(SelectBudget))));
        matched = true;
        Failure = null;
        return codes;
    }

    private static bool IsBudget(CodeInstruction instruction)
    {
        if (instruction.opcode == OpCodes.Ldc_R4)
            return instruction.operand is float value && value == 45.454544f;

        return GameFrameBudgetCompatibility.IsBudgetLoad(instruction);
    }

    private static void Report(string detail, MethodBase target)
    {
        var owners = Harmony.GetPatchInfo(target)?.Owners;
        string ownerList = owners == null ? "none" : string.Join(", ", owners);
        Failure = "Simulation frame budget unavailable: " + detail + ". Patch owners: " + ownerList + ".";
        Log.Error("[Simply More FPS] " + Failure);
    }
}
