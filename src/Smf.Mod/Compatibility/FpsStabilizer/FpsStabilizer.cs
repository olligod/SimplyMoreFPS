using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;

namespace SimplyMoreFPS.Compatibility;

internal static class FpsStabilizer
{
    internal const string PatchOwner = "matvey24.FPSStabilizer";
    internal const string BudgetOperand = "FPSStabilizer.HarmonyPatcher.target_frametime";

    internal static bool IsBudgetLoad(CodeInstruction instruction)
    {
        if (instruction.opcode == OpCodes.Ldsfld && instruction.operand is FieldInfo field)
        {
            return field.IsStatic && field.FieldType == typeof(float) && field.Name == "target_frametime"
                && field.DeclaringType?.FullName == "FPSStabilizer.HarmonyPatcher";
        }

        return false;
    }
}
