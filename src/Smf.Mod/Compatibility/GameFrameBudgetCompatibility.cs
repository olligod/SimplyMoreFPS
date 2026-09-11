using HarmonyLib;

namespace SimplyMoreFPS.Compatibility;

internal static class GameFrameBudgetCompatibility
{
    internal const string BudgetOperands = FpsStabilizer.BudgetOperand + ", or " + AdaptiveTps.BudgetOperand;

    internal static string[] PatchOwners => new[] { FpsStabilizer.PatchOwner, AdaptiveTps.PatchOwner };

    internal static bool IsBudgetLoad(CodeInstruction instruction)
    {
        return FpsStabilizer.IsBudgetLoad(instruction) || AdaptiveTps.IsBudgetLoad(instruction);
    }
}
