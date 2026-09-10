#nullable disable
using System;
using System.Collections.Generic;
using SimplyMoreFPS.Rendering.Lifecycle;

namespace SimplyMoreFPS.Rendering;

// Main-thread bookkeeping; native status reads and frame selection stay in each client.
internal sealed class RendererAcknowledgementState
{
    private readonly Dictionary<ulong, ulong> warmupMarkers = new Dictionary<ulong, ulong>();
    private Evidence routingEvidence;

    internal Command? Pending
    {
        get; private set;
    }
    internal ulong RoutingFrame
    {
        get; private set;
    }

    internal void ClearWarmupMarkers()
    {
        warmupMarkers.Clear();
    }

    internal void Accept(Command command)
    {
        Pending = command;
        routingEvidence = Evidence.None;
        RoutingFrame = 0;
    }

    internal void ValidateTicket(ulong session, ulong serial, ulong generation, ulong content, uint operation, uint disposition)
    {
        if (!Pending.HasValue || session != Pending.Value.Session || serial != Pending.Value.Serial ||
            generation != Pending.Value.Generation || content != Pending.Value.ContentRevision ||
            operation != (uint)Pending.Value.Operation)
        {
            throw new InvalidOperationException("Native acknowledgment does not match the pending ticket.");
        }

        if (disposition < 1 || disposition > 3)
        {
            throw new InvalidOperationException("Unknown native acknowledgment disposition.");
        }
    }

    internal bool TryReadEvidence(uint nativeEvidence, bool success, out Evidence evidence)
    {
        evidence = (Evidence)(nativeEvidence & 0x1FFFu);
        if (success && Pending.Value.Operation == Operation.RestoreNativeRouting)
        {
            // Bit 16 fences native work; Unity main supplies the routing evidence.
            if ((nativeEvidence & (1u << 16)) == 0 || routingEvidence == Evidence.None)
            {
                return false;
            }

            evidence |= routingEvidence;
        }

        return true;
    }

    internal void RoutingRestored(Command ticket, ulong frame, Evidence evidence)
    {
        if (!Pending.HasValue || Pending.Value.Session != ticket.Session || Pending.Value.Serial != ticket.Serial ||
            Pending.Value.Operation != Operation.RestoreNativeRouting || frame == 0 ||
            (evidence & ticket.Required) != ticket.Required)
        {
            throw new InvalidOperationException("Main routing report does not match the accepted cancellation fence.");
        }

        RoutingFrame = frame;
        routingEvidence = evidence;
    }

    internal void RecordWarmupMarker(NativeFrameMarker marker)
    {
        if (!marker.BeginOnly && marker.RestoreSerial == 0 && marker.Generation != 0 && !warmupMarkers.ContainsKey(marker.Generation))
        {
            warmupMarkers.Add(marker.Generation, marker.SourceFrame);
        }
    }

    internal bool WarmupRevealed(ulong sourceFrame, uint flags, ulong revealFrame,
        ulong submittedGeneration, ulong submittedContent, ulong submittedRestoreSerial)
    {
        Command wanted = Pending.Value;
        return warmupMarkers.TryGetValue(wanted.Generation, out ulong markerFrame) &&
            (flags & 2u) != 0 && revealFrame >= sourceFrame && revealFrame >= markerFrame &&
            submittedGeneration == wanted.Generation && submittedContent == wanted.ContentRevision &&
            submittedRestoreSerial == 0;
    }

    internal bool TrySupersedePreparation(ulong sourceFrame, Evidence evidence,
        ulong contentFence, ulong contentAcknowledged, out Acknowledgement acknowledgement)
    {
        acknowledgement = default;
        Command wanted = Pending.Value;
        if (wanted.Operation != Operation.PrepareHiddenGeneration || sourceFrame == 0 ||
            (evidence & Evidence.CompositeComplete) == 0 ||
            contentAcknowledged <= wanted.ContentRevision || contentAcknowledged > contentFence)
        {
            return false;
        }

        // A completed native ACK survives later content fences. That old scene can no
        // longer reveal; retire its prepared generation through the normal owner ACKs.
        Complete(2, 0, evidence, sourceFrame, out acknowledgement);
        acknowledgement.Error = "Prepared content " + wanted.ContentRevision +
            " was superseded by acknowledged content " + contentAcknowledged + ".";
        return true;
    }

    internal int Complete(uint disposition, int result, Evidence evidence, ulong sourceFrame, out Acknowledgement acknowledgement)
    {
        acknowledgement = default;
        Command wanted = Pending.Value;
        bool success = disposition == 1 && result == 0;
        if (success && (evidence & wanted.Required) != wanted.Required)
        {
            return 1;
        }

        acknowledgement = new Acknowledgement
        {
            Session = wanted.Session,
            Serial = wanted.Serial,
            Generation = wanted.Generation,
            Operation = wanted.Operation,
            Evidence = evidence,
            Frame = sourceFrame,
            Success = success,
            Superseded = disposition == 2,
            Error = success ? null : "Native operation: disposition " + disposition + ", HRESULT 0x" + result.ToString("X8"),
        };

        Pending = null;
        return 0;
    }
}
