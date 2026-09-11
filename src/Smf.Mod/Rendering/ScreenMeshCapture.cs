#nullable disable
using System;
using System.Collections.Generic;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

// Keep screen-relative meshes fixed to the source view when replaying them into the HUD.
internal sealed class ScreenMeshCapture
{
    private const int MaximumDraws = 4096;

    private sealed class Draw
    {
        internal Mesh Mesh;
        internal Matrix4x4 Matrix;
        internal Material Material;
        internal int Layer;
    }

    private readonly List<Draw> draws = new List<Draw>();
    private int frame = -1;
    private int fallbackFrame = -1;
    private int count;
    private int mapId;
    private int width;
    private int height;
    private Matrix4x4 view;
    private Matrix4x4 projection;

    internal bool Pending
    {
        get
        {
            if (frame != Time.frameCount)
            {
                // A completed source frame cannot contribute markers to a later scene or frame.
                count = 0;
            }

            return count != 0;
        }
    }

    internal bool Capture(Mesh mesh, Matrix4x4 matrix, Material material, int layer)
    {
        if (frame != Time.frameCount)
        {
            count = 0;
            frame = Time.frameCount;
        }

        if (fallbackFrame == frame || mesh == null || material == null)
        {
            return false;
        }

        if (count == MaximumDraws)
        {
            RestoreWorldDraws();
            return false;
        }

        if (count == 0)
        {
            Camera camera = Find.Camera;
            Map map = Find.CurrentMap;
            if (camera == null || map == null || !camera.enabled || !camera.orthographic)
            {
                return false;
            }

            mapId = map.uniqueID;
            width = Screen.width;
            height = Screen.height;
            view = camera.worldToCameraMatrix;
            projection = camera.projectionMatrix;
        }

        if (count == draws.Count)
        {
            draws.Add(new Draw { Material = new Material(material) { hideFlags = HideFlags.HideAndDontSave } });
        }
        else
        {
            // Callers can mutate materials between submissions; each draw needs its own snapshot.
            if (draws[count].Material.shader != material.shader)
            {
                draws[count].Material.shader = material.shader;
            }

            draws[count].Material.CopyPropertiesFromMaterial(material);
        }

        Draw draw = draws[count];
        draw.Mesh = mesh;
        draw.Matrix = matrix;
        draw.Layer = layer;
        ++count;
        return true;
    }

    internal void RestoreWorldDraws()
    {
        fallbackFrame = Time.frameCount;
        if (Pending)
        {
            for (int i = 0; i < count; ++i)
            {
                Draw draw = draws[i];
                Graphics.DrawMesh(draw.Mesh, draw.Matrix, draw.Material, draw.Layer);
            }
        }

        count = 0;
    }

    internal void Replay()
    {
        if (!Pending)
        {
            return;
        }

        if (Find.CurrentMap == null || Find.CurrentMap.uniqueID != mapId || Screen.width != width || Screen.height != height)
        {
            // The source scene was replaced before GUI; its old markers must not enter the new scene.
            count = 0;
            return;
        }

        RenderTexture target = RenderTexture.active;
        bool srgb = GL.sRGBWrite;
        GL.PushMatrix();
        try
        {
            GL.modelview = view;
            GL.LoadProjectionMatrix(projection);
            for (int i = 0; i < count; ++i)
            {
                Draw draw = draws[i];
                if (!draw.Material.SetPass(0))
                {
                    throw new InvalidOperationException("Screen mesh material could not render into the HUD.");
                }

                Graphics.DrawMeshNow(draw.Mesh, draw.Matrix);
            }

            count = 0;
        }
        finally
        {
            GL.PopMatrix();
            RenderTexture.active = target;
            GL.sRGBWrite = srgb;
        }
    }

    internal void Dispose()
    {
        count = 0;
        foreach (Draw draw in draws)
        {
            UnityEngine.Object.Destroy(draw.Material);
        }

        draws.Clear();
    }
}
