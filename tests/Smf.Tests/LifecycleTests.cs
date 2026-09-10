using System;
using SimplyMoreFPS.Rendering.Lifecycle;

internal static class LifecycleTests
{
    private static int checks;

    public static int Run()
    {
        checks = 0;
        SupersededPreparation(false);
        SupersededPreparation(true);
        RejectInvalidSupersession();
        RenderingSuspension();
        return checks;
    }

    private static void Check(bool value, string message)
    {
        if (!value)
        {
            throw new InvalidOperationException(message);
        }

        ++checks;
    }

    private static Command Next(LifecycleCoordinator core, Operation operation)
    {
        Command? command = core.AdvanceAtSafeBoundary(true);
        Check(command.HasValue && command.Value.Operation == operation, "Expected " + operation);
        return command!.Value;
    }

    private static void Reply(LifecycleCoordinator core, Command command, bool superseded = false, ulong frame = 100)
    {
        core.Acknowledge(new Acknowledgement
        {
            Session = command.Session,
            Serial = command.Serial,
            Generation = command.Generation,
            Operation = command.Operation,
            Frame = superseded ? 0ul : frame,
            Evidence = superseded ? Evidence.None : command.Required,
            Success = true,
            Superseded = superseded
        });
    }

    private static LifecycleCoordinator Start()
    {
        var core = new LifecycleCoordinator();
        core.ChangeContent(1, false);
        core.SetEnabled(true);
        return core;
    }

    private static void PublishFence(LifecycleCoordinator core)
    {
        WorldFence? fence = core.TakeWorldFence();
        Check(fence.HasValue, "Latest fence published");
        core.AcknowledgeWorldFence(fence!.Value);
    }

    private static void SupersededPreparation(bool replacement)
    {
        LifecycleCoordinator core = Start();
        Command prepare = Next(core, Operation.PrepareHiddenGeneration);
        core.MarkPreparationSubmitted(prepare);

        if (replacement)
        {
            PublishFence(core);
            Reply(core, prepare);
            Command activate = Next(core, Operation.ActivateGeneration);
            Reply(core, activate);
            Check(!core.AdvanceAtSafeBoundary(true).HasValue && core.State == Phase.Active, "Initial generation active");
            core.ChangeContent(6, false);
            PublishFence(core);
            Reply(core, Next(core, Operation.InvalidateWorld));
            prepare = Next(core, Operation.PrepareReplacementGeneration);
            core.MarkPreparationSubmitted(prepare);
        }

        // No frame is required to obsolete an accepted prepare. The host may
        // still own queued copies or an in-flight hidden draw at this point.
        core.ChangeContent(7, false);
        PublishFence(core);
        Check(!core.AdvanceAtSafeBoundary(true).HasValue, "Content changes cannot invent an owner reply");

        bool rejected = false;
        try
        {
            core.SupersedeUnsubmittedPreparation(prepare, true);
        }
        catch (InvalidOperationException)
        {
            rejected = true;
        }

        Check(rejected, "An accepted prepare cannot be cancelled locally");

        Reply(core, prepare, true);
        core.EnterDraw();
        Check(!core.AdvanceAtSafeBoundary(true).HasValue, "Supersession waits for draw exit");
        core.LeaveDraw();
        Command retire = Next(core, Operation.RetireGenerationGpu);
        Check(retire.Generation == prepare.Generation, "Retire the obsolete generation");

        for (ulong revision = 8; revision <= 141; ++revision)
        {
            core.ChangeContent(revision, false);
            PublishFence(core);
            Check(!core.AdvanceAtSafeBoundary(true).HasValue, "Resize must not bypass pending GPU retirement");
        }

        // A late completion for the rejected prepare cannot retire resources.
        long stale = core.StaleAcknowledgements;
        Reply(core, prepare);
        Check(core.StaleAcknowledgements == stale + 1, "Late prepare reply is stale");
        Check(!core.AdvanceAtSafeBoundary(true).HasValue, "GPU retirement still owns the resources");
        Reply(core, retire);
        Check(!core.AdvanceAtSafeBoundary(false).HasValue, "Main release waits for restored capture targets");
        Command release = Next(core, Operation.ReleaseGenerationMain);
        Check(release.Generation == prepare.Generation, "Only retired generation released");
        Reply(core, release);
        Command latest = Next(core, replacement ? Operation.PrepareReplacementGeneration : Operation.PrepareHiddenGeneration);
        Check(latest.Generation > prepare.Generation && latest.ContentRevision == 141, "Replacement uses the latest resize, not the obsolete content");
        core.MarkPreparationSubmitted(latest);
        Reply(core, latest);
        Check(Next(core, Operation.ActivateGeneration).Generation == latest.Generation, "Latest preparation can activate");
        Check(!core.Faulted, "Supersession does not latch an owner failure");
    }

    private static void RejectInvalidSupersession()
    {
        LifecycleCoordinator core = Start();
        Command prepare = Next(core, Operation.PrepareHiddenGeneration);
        core.MarkPreparationSubmitted(prepare);
        Command foreign = prepare;
        ++foreign.Serial;
        Reply(core, foreign, true);
        Check(core.StaleAcknowledgements == 1 && !core.AdvanceAtSafeBoundary(true).HasValue, "Wrong-ticket supersession ignored");
        Reply(core, prepare, true);
        Next(core, Operation.RestoreNativeRouting);
        Check(core.Faulted, "A current-content prepare cannot claim supersession");
    }

    private static void RenderingSuspension()
    {
        LifecycleCoordinator core = Start();
        Command prepare = Next(core, Operation.PrepareHiddenGeneration);
        core.MarkPreparationSubmitted(prepare);
        PublishFence(core);
        Reply(core, prepare);
        Reply(core, Next(core, Operation.ActivateGeneration));
        Check(!core.AdvanceAtSafeBoundary(true).HasValue && core.State == Phase.Active, "Renderer starts active");

        core.EnterDraw();
        core.SetRenderingAllowed(false);
        Check(core.UserEnabled && !core.AdvanceAtSafeBoundary(true).HasValue, "Suspension preserves user intent and waits for draw exit");
        core.LeaveDraw();

        Operation[] retirement =
        {
            Operation.RestoreNativeRouting,
            Operation.AwaitNativeFrame,
            Operation.DetachComposite,
            Operation.RetireSessionGpu,
            Operation.ReleaseSessionMain,
            Operation.StopWorker,
        };
        foreach (Operation operation in retirement)
        {
            Command command = Next(core, operation);
            Reply(core, command, frame: command.AfterFrame + 1);
        }

        Check(!core.AdvanceAtSafeBoundary(true).HasValue && core.State == Phase.Off && core.UserEnabled, "Suspended renderer stays off after complete retirement");
        core.SetRenderingAllowed(true);
        Check(Next(core, Operation.PrepareHiddenGeneration).Session > prepare.Session, "Free camera can start a fresh session");

        core.ReportFailure("fixture failure");
        core.SetRenderingAllowed(false);
        core.SetRenderingAllowed(true);
        Check(core.Faulted && core.LastFailure == "fixture failure", "Automatic mode switches cannot clear a fault");
        Next(core, Operation.RestoreNativeRouting);
        core.SetEnabled(false);
        core.SetEnabled(true);
        Check(!core.Faulted, "An explicit user off/on still permits retry");
    }
}
