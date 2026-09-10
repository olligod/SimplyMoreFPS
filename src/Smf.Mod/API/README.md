# Camera API

For mods that want to work with Simply More FPS instead of fighting it. Reference
`Smf.Mod.dll` from the installed mod (do not copy it into your mod) and use the
`SimplyMoreFPS.API` namespace. Add `olli.simplymorefps` to `loadAfter` in your About.xml.
Everything below runs on Unity's main thread unless noted.

Built-in shims for Camera+, SimpleCameraSetting, Perspective Shift, Follow Me, Vehicle Map
Framework, As Above So Below and Interaction Bubbles live in `../Compatibility`. If your mod
is popular and you would rather not depend on us, a shim there works too.
Each integration has its own folder; shared binding helpers stay in `Compat.cs`.

## Read and move the camera

```csharp
using SimplyMoreFPS.API;

CameraStatus status = CameraApi.GetStatus();
if (status.DetachedMotionActive)
{
    // The native camera is moving the map right now.
}

CameraApi.Jump(new Vector3(120f, 0f, 80f), size: 24f);
CameraApi.Pan(new Vector3(120f, 0f, 80f), 24f, duration: 0.5f, completion: () => Log.Message("arrived"));
```

Same semantics as `CameraDriver.SetRootPosAndSize` and `PanToMapLocAndSize`, but the pan
keeps moving while the main thread is stalled.

## Your mod controls camera speed or zoom

Register a provider. `Resolve` runs on the main thread during camera setup and updates,
so keep it cheap and reuse unchanged policies. Return a policy, or null to let the next
provider (or vanilla) decide. Highest `Priority` wins. Do not keep `context.Driver` or
`context.Map` inside the policy.

```csharp
using SimplyMoreFPS.API;

public sealed class MyCameraProvider : ICameraProvider
{
    public string Id => "me.mymod";
    public int Priority => 100;

    private readonly CameraPolicy policy = new CameraPolicy(
        scalars: new CameraScalarOverrides(moveSpeed: 1.5, zoomSpeed: 1.2, maxSize: 80, edgeScroll: false));

    public CameraPolicy? Resolve(CameraContext context)
    {
        return MySettings.FastCamera ? policy : null;
    }
}

// Once, for example in your Mod constructor:
CameraProviders.Register(new MyCameraProvider());
```

`CameraScalarOverrides` replaces individual vanilla scalars: `minSize`, `maxSize`,
`dollyRateKeys`, `dollyRateScreenEdge`, `speedDecay`, `moveSpeed`, `zoomSpeed`,
`scrollWheelRate`, `zoomPreserveFactor`, `dragSensitivity`, `smoothZoom`, `zoomToMouse`,
`edgeScroll`. Leave a value null to keep vanilla.

For curves instead of constants, give the policy a `CameraProfile`:

```csharp
var profile = new CameraProfile(
    moveSpeed: CameraCurve.PowerRange(inputMin: 11, inputMax: 60, outputMin: 1.0, outputMax: 2.5),
    zoomSpeed: CameraCurve.Constant(1.3),
    zoomToMouse: true);
var policy = new CameraPolicy(profile: profile);
```

Curves are evaluated by the native camera worker, so they keep working during a stall.
`PowerRange`, `Polynomial2`, `Step` and `PiecewiseLinear` (max 16 points) are available.
The input is the current zoom (root size) unless you pick another `CameraCurveDomain`.

If your mod moves the camera itself every frame (a follow cam, a cinematic), tell us to stay
out of the way:

```csharp
private static readonly CameraPolicy followPolicy = new CameraPolicy(allowDetachedRendering: false);

public CameraPolicy? Resolve(CameraContext context)
{
    return Following ? followPolicy : null;
}
```

SMF hands rendering back to Unity, pauses its TPS boost, and resumes when the provider
returns null. The player's Enabled setting stays unchanged. Use `allowDetachedMotion: false`
instead if you only need camera control while keeping SMF's renderer active.

## Your mod changes map bounds or zoom limits

Register only the geometry queries your mod needs. Bounds are in map cells; return null
when your mod does not control the current map. Create reusable policies once.

```csharp
var bigMapPolicy = new CameraGeometryPolicy(
    movementBounds: new CameraMotionBounds(
        x: CameraAxisBounds.Framed(0, 500, panMargin: 10, overscrollFraction: 0.1),
        z: CameraAxisBounds.Framed(0, 500, panMargin: 10, overscrollFraction: 0.1)),
    maximumSize: 120,
    reservedWheelModifiers: CameraWheelModifiers.Control);

CameraGeometryProviders.Register("me.mymod.geometry", priority: 300,
    policy: context => MyMod.IsBigMap(context.Map) ? bigMapPolicy : null,
    movementExtent: context => MyMod.IsBigMap(context.Map)
        ? new CameraMovementExtent(500, 500) : (CameraMovementExtent?)null);
```

`reservedWheelModifiers` tells the native wheel handler to ignore scroll events while that
modifier is held, so your Ctrl+wheel binding keeps working. The optional `coverageBounds`
callback controls rendered bounds. A policy's `CoverageBounds` takes precedence over that
callback. Movement-extent queries do not evaluate policy or coverage callbacks.

Each query takes the first non-null result, ordered by descending priority and then ordinal
ID. Built-in VMF geometry has priority 200; As Above So Below has priority 100. Registration
and `Unregister(id)` are thread safe; callbacks run on Unity main and should reuse unchanged
policies. Callback errors include the provider ID and query. Existing Harmony patches of
`CameraGeometry.GetPolicy`, `GetMovementExtent` and `GetCoverageBounds` still work;
coverage queries continue through the public `GetPolicy` patch point first.

## Your mod draws things in the world with GUI code

Labels, bubbles, floating text: anything drawn in `OnGUI` at a world position freezes in
place during a stall because Unity is not running. Register the draw method and its output
is captured once per frame and moved with the camera.

```csharp
WorldOverlayApi.Register("me.mymod.labels", AccessTools.Method(typeof(MyLabels), nameof(MyLabels.Draw)));

static class MyLabels
{
    public static void Draw()
    {
        // Draw only world-anchored UI here. Fixed HUD elements must not go through this method.
    }
}
```

The method must be a concrete `void` method with no open generic parameters; instance methods also work.
Registration captures its existing calls rather than invoking it. Register from any thread.
`WorldOverlayApi.Unregister(id)` removes it at the next GUI sync.

## Errors

Every entry point throws on bad input or off-thread calls. A provider that throws in
`Resolve` turns detached motion off for that camera; the map still renders normally through
Unity, and the error (with the provider `Id`) shows up in `CameraApi.GetStatus().LastError`
and the game log.
