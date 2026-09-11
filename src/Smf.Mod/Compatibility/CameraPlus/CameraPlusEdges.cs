#nullable disable
using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using SimplyMoreFPS.Rendering;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// Only clipped Camera+ indicators belong in the screen layer; interior markers stay in world space.
internal static class CameraPlusEdges
{
    private static readonly MethodInfo OriginalDraw = AccessTools.Method(typeof(Graphics), nameof(Graphics.DrawMesh),
        new[] { typeof(Mesh), typeof(Matrix4x4), typeof(Material), typeof(int) });

    internal static bool Install(Harmony patches)
    {
        if (!ModsConfig.IsActive(CameraPlus.PackageId))
        {
            return false;
        }

        try
        {
            Type drawer = Compat.RequireType("CameraPlus.DotDrawer");
            Type config = Compat.RequireType("CameraPlus.DotConfig");
            MethodInfo clipped = Compat.RequireMethod(drawer, "DrawClipped", typeof(void),
                typeof(Vector3), config, typeof(float), typeof(Vector2), typeof(Material));
            patches.Patch(clipped, transpiler: new HarmonyMethod(typeof(CameraPlusEdges), nameof(RouteDraw)));
            return true;
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("Could not bind edge indicators for " + CameraPlus.PackageId + ".", error);
        }
    }

    private static IEnumerable<CodeInstruction> RouteDraw(IEnumerable<CodeInstruction> instructions)
    {
        var code = new List<CodeInstruction>(instructions);
        int matches = 0;
        foreach (CodeInstruction instruction in code)
        {
            if (instruction.Calls(OriginalDraw))
            {
                instruction.operand = AccessTools.Method(typeof(CameraPlusEdges), nameof(DrawMesh));
                ++matches;
            }
        }

        if (matches != 1)
        {
            throw new InvalidOperationException("Camera+ DrawClipped must submit exactly one edge mesh.");
        }

        return code;
    }

    private static void DrawMesh(Mesh mesh, Matrix4x4 matrix, Material material, int layer)
    {
        if (!HybridSession.TryCaptureScreenMesh(mesh, matrix, material, layer))
        {
            Graphics.DrawMesh(mesh, matrix, material, layer);
        }
    }
}
