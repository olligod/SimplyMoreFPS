using System;
using System.Reflection;
using HarmonyLib;
using SimplyMoreFPS.Rendering;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Compatibility;

// The live FX texture covers the screen. Coverage needs the same particles from its own projection.
internal sealed class Nivarian : IMapImageEffect
{
    internal const string CompositorName = "Nivarian_Race.Code.EfxCam.NivarianFXCompositor";

    private readonly MonoBehaviour compositor;
    private readonly Func<object, RenderTexture> readTexture;
    private readonly Func<object, Material> readMaterial;
    private Camera? particleCamera;
    private Camera? coverageCamera;
    private MonoBehaviour? bloom;
    private Action<RenderTexture, RenderTexture>? renderBloom;
    private Material? material;
    private bool applyEffects;

    internal Nivarian(MonoBehaviour compositor)
    {
        if (!ModsConfig.IsActive("keeptpa.nivarianrace") || compositor.GetType() != Compat.RequireType(CompositorName))
        {
            throw new InvalidOperationException("Unsupported Nivarian compositor.");
        }

        this.compositor = compositor;
        readTexture = Compat.Getter<RenderTexture>(
            Compat.Field(compositor.GetType(), "fxRT", typeof(RenderTexture)));
        readMaterial = Compat.Getter<Material>(
            Compat.Field(compositor.GetType(), "_mat", typeof(Material)));
    }

    public MonoBehaviour Source => compositor;

    internal static void Register()
    {
        MapImageEffects.Register(CompositorName, source => new Nivarian(source));
    }

    public bool Prepare()
    {
        RenderTexture texture = readTexture(compositor);
        Material original = readMaterial(compositor);
        if (texture == null || !texture.IsCreated() || original == null)
        {
            applyEffects = false;
            return true;
        }

        if (particleCamera == null || particleCamera.targetTexture != texture)
        {
            particleCamera = null;
            foreach (Camera camera in compositor.GetComponentsInChildren<Camera>())
            {
                if (camera != compositor.GetComponent<Camera>() && camera.targetTexture == texture)
                {
                    if (particleCamera != null)
                    {
                        throw new InvalidOperationException("Multiple Nivarian cameras share the FX texture.");
                    }

                    particleCamera = camera;
                }
            }
        }

        if (particleCamera == null || !particleCamera.isActiveAndEnabled)
        {
            return false;
        }

        applyEffects = true;

        MonoBehaviour? nextBloom = null;
        foreach (MonoBehaviour component in particleCamera.GetComponents<MonoBehaviour>())
        {
            if (component == null || !component.isActiveAndEnabled)
            {
                continue;
            }

            MethodInfo callback = AccessTools.Method(component.GetType(), "OnRenderImage",
                new[] { typeof(RenderTexture), typeof(RenderTexture) });
            if (callback == null)
            {
                continue;
            }

            if (component.GetType().FullName != "Kino.Bloom" || nextBloom != null)
            {
                throw new InvalidOperationException("Unsupported Nivarian particle camera effect: " + component.GetType().FullName);
            }

            nextBloom = component;
            if (bloom != nextBloom)
            {
                // This bloom pass has no frame history; it releases its scratch textures after each call.
                renderBloom = (Action<RenderTexture, RenderTexture>)Delegate.CreateDelegate(
                    typeof(Action<RenderTexture, RenderTexture>), component, callback);
            }
        }

        bloom = nextBloom;
        if (bloom == null)
        {
            renderBloom = null;
        }

        if (material == null || material.shader != original.shader)
        {
            if (material != null)
            {
                UnityEngine.Object.Destroy(material);
            }

            material = new Material(original) { hideFlags = HideFlags.HideAndDontSave, name = "SMF Nivarian compositor" };
        }
        else
        {
            material.CopyPropertiesFromMaterial(original);
        }

        if (coverageCamera == null)
        {
            var owner = new GameObject("SMF Nivarian coverage camera") { hideFlags = HideFlags.HideAndDontSave };
            UnityEngine.Object.DontDestroyOnLoad(owner);
            coverageCamera = owner.AddComponent<Camera>();
            coverageCamera.enabled = false;
        }

        return true;
    }

    public void Validate()
    {
    }

    public void Render(Camera projection, RenderTexture input, RenderTexture output)
    {
        if (!applyEffects)
        {
            Graphics.Blit(input, output);
            return;
        }

        if (particleCamera == null || coverageCamera == null || material == null)
        {
            throw new InvalidOperationException("Nivarian coverage was not prepared.");
        }

        RenderTextureDescriptor descriptor = particleCamera.targetTexture.descriptor;
        descriptor.width = input.width;
        descriptor.height = input.height;
        descriptor.msaaSamples = 1;
        RenderTexture particles = RenderTexture.GetTemporary(descriptor);
        RenderTexture? filtered = null;
        try
        {
            coverageCamera.CopyFrom(particleCamera);
            coverageCamera.enabled = false;
            coverageCamera.targetTexture = particles;
            coverageCamera.rect = new Rect(0, 0, 1, 1);
            coverageCamera.transform.SetPositionAndRotation(projection.transform.position, projection.transform.rotation);
            coverageCamera.orthographicSize = projection.orthographicSize;
            coverageCamera.aspect = projection.aspect;
            coverageCamera.worldToCameraMatrix = projection.worldToCameraMatrix;
            coverageCamera.projectionMatrix = projection.projectionMatrix;
            coverageCamera.cullingMatrix = projection.cullingMatrix;
            coverageCamera.Render();

            if (renderBloom != null)
            {
                filtered = RenderTexture.GetTemporary(descriptor);
                renderBloom(particles, filtered);
            }

            material.SetTexture("_FXTex", filtered != null ? filtered : particles);
            Graphics.Blit(input, output, material);
        }
        finally
        {
            coverageCamera.targetTexture = null;
            material.SetTexture("_FXTex", null);
            if (filtered != null)
            {
                RenderTexture.ReleaseTemporary(filtered);
            }

            RenderTexture.ReleaseTemporary(particles);
        }
    }

    public void Dispose()
    {
        if (coverageCamera != null)
        {
            UnityEngine.Object.Destroy(coverageCamera.gameObject);
        }

        if (material != null)
        {
            UnityEngine.Object.Destroy(material);
        }

        coverageCamera = null;
        material = null;
    }
}
