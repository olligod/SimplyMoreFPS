using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using RimWorld;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

internal sealed class SpaceDebrisDraws
{
    private static readonly MethodInfo OriginalDraw = AccessTools.Method(typeof(Graphics), nameof(Graphics.DrawMesh),
        new[] { typeof(Mesh), typeof(Matrix4x4), typeof(Material), typeof(int) });
    private static readonly AccessTools.FieldRef<MapDrawLayer, Map> ReadMap =
        AccessTools.FieldRefAccess<MapDrawLayer, Map>("map");
    private static readonly AccessTools.FieldRef<MapDrawLayer_OrbitalDebris, Dictionary<LayerSubMesh, float>> ReadParallax =
        AccessTools.FieldRefAccess<MapDrawLayer_OrbitalDebris, Dictionary<LayerSubMesh, float>>("parallaxPer10CellLookup");
    private static readonly HashSet<Camera> Excluded = new HashSet<Camera>();
    private static Harmony? harmony;
    private static SpaceDebrisDraws? active;
    private static SpaceDebrisDraws? pending;

    private readonly Map map;
    private readonly Action<Exception> reportFailure;
    private readonly List<Draw> draws = new List<Draw>();
    private int frame = -1;
    private Exception? error;

    internal SpaceDebrisDraws(Map map, Action<Exception> reportFailure)
    {
        this.map = map;
        this.reportFailure = reportFailure;
    }

    internal static void InstallHooks(string owner)
    {
        if (harmony != null)
            return;

        harmony = new Harmony(owner + ".space-debris");
        harmony.Patch(AccessTools.Method(typeof(MapDrawLayer_OrbitalDebris), nameof(MapDrawLayer_OrbitalDebris.DrawLayer)),
            transpiler: new HarmonyMethod(typeof(SpaceDebrisDraws), nameof(RouteDraws)));
        Camera.onPreCull += BeforeCull;
    }

    internal static void RemoveHooks()
    {
        active = null;
        pending = null;
        Camera.onPreCull -= BeforeCull;
        harmony?.UnpatchAll(harmony.Id);
        harmony = null;
        Excluded.Clear();
    }

    internal static void Select(SpaceDebrisDraws? next)
    {
        active = next;
    }

    internal static void Exclude(Camera camera)
    {
        Excluded.Add(camera);
    }

    internal static void Forget(Camera camera)
    {
        Excluded.Remove(camera);
    }

    private static IEnumerable<CodeInstruction> RouteDraws(IEnumerable<CodeInstruction> instructions)
    {
        var code = new List<CodeInstruction>(instructions);
        int matches = 0;
        foreach (CodeInstruction instruction in code)
        {
            if (instruction.Calls(OriginalDraw))
                ++matches;
        }
        if (matches != 1)
            throw new InvalidOperationException("The orbital debris draw boundary changed.");

        foreach (CodeInstruction instruction in code)
        {
            if (instruction.Calls(OriginalDraw))
            {
                if (instruction.blocks.Count != 0)
                    throw new InvalidOperationException("Unexpected exception boundary in the orbital debris draw.");

                var owner = new CodeInstruction(OpCodes.Ldarg_0);
                owner.labels.AddRange(instruction.labels);
                instruction.labels.Clear();
                yield return owner;
                instruction.operand = AccessTools.Method(typeof(SpaceDebrisDraws), nameof(Record));
            }
            yield return instruction;
        }
    }

    private static void Record(Mesh mesh, Matrix4x4 matrix, Material material, int layer, MapDrawLayer_OrbitalDebris owner)
    {
        SpaceDebrisDraws? capture = active;
        if (capture == null || ReadMap(owner) != capture.map)
        {
            Graphics.DrawMesh(mesh, matrix, material, layer);
            return;
        }

        if (capture.frame != Time.frameCount)
        {
            capture.draws.Clear();
            capture.error = null;
            capture.frame = Time.frameCount;
        }

        try
        {
            if (capture.draws.Count == 4096)
                throw new InvalidOperationException("Too many orbital debris draws.");
            float factor = float.NaN;
            foreach (KeyValuePair<LayerSubMesh, float> pair in ReadParallax(owner))
            {
                if (pair.Key.mesh != mesh || pair.Key.material != material)
                    continue;
                factor = pair.Value / 10;
                break;
            }

            if (float.IsNaN(factor) || float.IsInfinity(factor))
                throw new InvalidOperationException("An orbital debris draw has no finite parallax value.");

            capture.draws.Add(new Draw(mesh, matrix, material, layer, factor, Find.Camera.transform.position));
            pending = capture;
        }
        catch (Exception failure)
        {
            capture.error = failure;
            Graphics.DrawMesh(mesh, matrix, material, layer);
        }
    }

    private static void BeforeCull(Camera camera)
    {
        SpaceDebrisDraws? capture = pending;
        if (capture == null)
            return;
        if (capture.frame != Time.frameCount)
        {
            pending = null;
            return;
        }
        if (Excluded.Contains(camera))
            return;

        try
        {
            // Finish queued source draws even if this generation was deselected during the frame.
            foreach (Draw draw in capture.draws)
                Graphics.DrawMesh(draw.Mesh, draw.Matrix, draw.Material, draw.Layer, camera);
        }
        catch (Exception failure)
        {
            capture.error = failure;
            capture.reportFailure(failure);
        }
    }

    internal IReadOnlyList<Draw> ReadFrame()
    {
        if (error != null)
            throw new InvalidOperationException("Orbital debris capture failed.", error);
        return frame == Time.frameCount ? draws : Array.Empty<Draw>();
    }

    internal readonly struct Draw
    {
        internal readonly Mesh Mesh;
        internal readonly Matrix4x4 Matrix;
        internal readonly Material Material;
        internal readonly int Layer;
        internal readonly float Parallax;
        internal readonly Vector3 CameraPosition;

        internal Draw(Mesh mesh, Matrix4x4 matrix, Material material, int layer, float parallax, Vector3 cameraPosition)
        {
            Mesh = mesh;
            Matrix = matrix;
            Material = material;
            Layer = layer;
            Parallax = parallax;
            CameraPosition = cameraPosition;
        }
    }
}
