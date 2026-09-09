using System;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

internal static class PerspectiveShift
{
    internal const string PackageId = "ferny.perspectiveshift";

    internal static void Register()
    {
        if (!ModsConfig.IsActive(PackageId)) return;

        try
        {
            CameraProviders.Register(new Provider(Compat.RequireType("PerspectiveShift.CameraMapConfig_Avatar")));
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("Camera provider '" + PackageId + "' failed during binding.", error);
        }
    }

    // Perspective Shift swaps in its own camera config while following an avatar; leave that camera to it.
    private sealed class Provider : ICameraProvider
    {
        private readonly Type avatarConfig;
        private readonly CameraPolicy policy = new CameraPolicy(allowDetachedMotion: false);

        public string Id => PackageId;
        public int Priority => 300;

        internal Provider(Type avatarConfig)
        {
            this.avatarConfig = avatarConfig;
        }

        public CameraPolicy? Resolve(CameraContext context)
        {
            return context.Driver.config.GetType() == avatarConfig ? policy : null;
        }
    }
}
