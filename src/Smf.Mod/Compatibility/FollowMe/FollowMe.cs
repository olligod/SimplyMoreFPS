using System;
using SimplyMoreFPS.API;
using Verse;

namespace SimplyMoreFPS.Compatibility;

internal static class FollowMe
{
    internal const string PackageId = "fluffy.followme";

    internal static void Register()
    {
        if (!ModsConfig.IsActive(PackageId))
            return;

        try
        {
            CameraProviders.Register(new Provider(Compat.RequireType("FollowMe.FollowMe")));
        }
        catch (Exception error)
        {
            throw new InvalidOperationException("Camera provider '" + PackageId + "' failed during binding.", error);
        }
    }

    // Follow Me rewrites the camera position every Update, so detached motion would fight it.
    private sealed class Provider : ICameraProvider
    {
        private readonly Func<object, bool> enabled;
        private readonly Func<object, bool> following;
        private readonly CameraPolicy policy = new CameraPolicy(allowDetachedRendering: false);

        public string Id => PackageId;
        public int Priority => 250;

        internal Provider(Type type)
        {
            enabled = Compat.Getter<bool>(Compat.Field(type, "_enabled", typeof(bool), true));
            following = Compat.Getter<bool>(Compat.Field(type, "_currentlyFollowing", typeof(bool), true));
        }

        public CameraPolicy? Resolve(CameraContext context)
        {
            return enabled(null!) && following(null!) ? policy : null;
        }
    }
}
