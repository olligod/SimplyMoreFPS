#nullable disable
using System.Threading;

namespace SimplyMoreFPS.Rendering.CameraControl;

// One instance per installation. Off-thread setter hooks hold only this
// object, so a late finalizer cannot touch a newer session.
internal sealed class ForeignCameraChanges
{
    private int active;
    private int dirty;

    internal bool Pending => Volatile.Read(ref active) != 0 || Volatile.Read(ref dirty) != 0;

    internal bool InFlight => Volatile.Read(ref active) != 0;

    internal void Begin()
    {
        Interlocked.Increment(ref active);
        Volatile.Write(ref dirty, 1);
    }

    internal void Complete()
    {
        // Stay dirty until the main thread consumes the completion.
        Volatile.Write(ref dirty, 1);
        Interlocked.Decrement(ref active);
    }

    internal bool Consume()
    {
        return Volatile.Read(ref dirty) != 0 && Interlocked.Exchange(ref dirty, 0) != 0;
    }
}
