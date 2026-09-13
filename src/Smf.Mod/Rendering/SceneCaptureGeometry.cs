using System;
using UnityEngine;
using UnityEngine.Rendering;

namespace SimplyMoreFPS.Rendering;

internal static class SceneCaptureGeometry
{
    internal static unsafe void SetProjection(CameraPose pose, uint width, uint height, ref ScenePackets.Layer layer)
    {
        double* combined = stackalloc double[16];
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                double value = 0;
                for (int k = 0; k < 4; ++k)
                    value += (double)pose.Projection[k * 4 + row] * pose.WorldToCamera[column * 4 + k];
                combined[column * 4 + row] = value;
            }
        }

        double sx = width * .5 / combined[15];
        double sy = -(double)height * .5 / combined[15];
        layer.Affine[0] = combined[0] * sx;
        layer.Affine[1] = combined[8] * sx;
        layer.Affine[2] = combined[12] * sx + width * .5;
        layer.Affine[3] = combined[1] * sy;
        layer.Affine[4] = combined[9] * sy;
        layer.Affine[5] = combined[13] * sy + height * .5;
    }

    internal static unsafe void SetProjection(Camera camera, uint width, uint height, ref ScenePackets.Layer layer)
    {
        Matrix4x4 matrix = camera.projectionMatrix * camera.worldToCameraMatrix;
        if (!camera.orthographic || width == 0 || height == 0 ||
            Math.Abs(matrix.m30) > 1e-7 || Math.Abs(matrix.m32) > 1e-7 || Math.Abs(matrix.m33 - 1) > 1e-7)
        {
            throw new InvalidOperationException("Scene layers require an orthographic map projection.");
        }

        layer.Affine[0] = matrix.m00 * width * .5;
        layer.Affine[1] = matrix.m02 * width * .5;
        layer.Affine[2] = (matrix.m03 + 1) * width * .5;
        layer.Affine[3] = -matrix.m10 * height * .5;
        layer.Affine[4] = -matrix.m12 * height * .5;
        layer.Affine[5] = (1 - matrix.m13) * height * .5;
        for (int i = 0; i < 6; ++i)
        {
            if (!Finite(layer.Affine[i]))
                throw new InvalidOperationException("A scene layer projection is not finite.");
        }

        double determinant = layer.Affine[0] * layer.Affine[4] - layer.Affine[1] * layer.Affine[3];
        if (!Finite(determinant) || Math.Abs(determinant) < 1e-10)
            throw new InvalidOperationException("A scene layer projection is singular.");
    }

    internal static void SetDepth(Camera camera, ref ScenePackets.Layer layer)
    {
        Matrix4x4 matrix = GL.GetGPUProjectionMatrix(camera.projectionMatrix, true) * camera.worldToCameraMatrix;
        if (!camera.orthographic || Math.Abs(matrix.m30) > 1e-7 || Math.Abs(matrix.m31) > 1e-7 ||
            Math.Abs(matrix.m32) > 1e-7 || Math.Abs(matrix.m33 - 1) > 1e-7)
        {
            throw new InvalidOperationException("Scene depth requires an orthographic map projection.");
        }

        double x = matrix.m20;
        double y = matrix.m21;
        double z = matrix.m22;
        double offset = matrix.m23;
        if (SystemInfo.graphicsDeviceType == GraphicsDeviceType.OpenGLCore)
        {
            x *= .5;
            y *= .5;
            z *= .5;
            offset = offset * .5 + .5;
        }

        if (!Finite(x) || !Finite(y) || !Finite(z) || !Finite(offset) || Math.Abs(y) < 1e-12)
            throw new InvalidOperationException("Scene depth cannot recover map altitude.");

        // Keep the small X/Z terms from the actual camera matrix as well as its height.
        layer.DepthX = -x / y;
        layer.DepthZ = -z / y;
        layer.DepthScale = 1 / y;
        layer.DepthOffset = -offset / y;
    }

    private static bool Finite(double value) => !double.IsNaN(value) && !double.IsInfinity(value);
}
