using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;

namespace SimplyMoreFPS.Compatibility;

internal static class AdaptiveTps
{
    internal const string PatchOwner = "blue.adaptivetps";
    internal const string BudgetOperand = "AdaptiveTPS.TickManagerUpdate_Patch.GetMaxFrameTimeConditional()";

    internal static bool IsBudgetLoad(CodeInstruction instruction)
    {
        if (instruction.opcode == OpCodes.Call && instruction.operand is MethodInfo method)
        {
            return method.IsStatic && !method.ContainsGenericParameters && method.ReturnType == typeof(float)
                && method.GetParameters().Length == 0
                && method.Name == "GetMaxFrameTimeConditional" && method.DeclaringType?.FullName == "AdaptiveTPS.TickManagerUpdate_Patch";
        }

        return false;
    }
}
