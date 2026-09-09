#nullable disable
using Verse;

namespace SimplyMoreFPS.Rendering;

// The world and map exist before loading has built their renderer and generator;
// nothing may ask about world rendering until both are there.
internal static class MapSceneReadiness
{
    internal static bool Ready =>
        Current.ProgramState == ProgramState.Playing &&
        !LongEventHandler.ShouldWaitForEvent &&
        Current.Game?.World?.renderer != null &&
        Find.CurrentMap?.generatorDef != null;
}
