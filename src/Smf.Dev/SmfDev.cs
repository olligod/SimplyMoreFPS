using System;
using System.Collections;
using System.Diagnostics;
using System.Threading;
using HarmonyLib;
using SimplyMoreFPS.Rendering;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Dev;

// Loaded by SmfMod through reflection when the game runs with -smf-devapi=PORT.
public static class SmfDev
{
    internal static readonly long StartedAt = Stopwatch.GetTimestamp();
    internal static DevDriver? Driver;

    private static readonly int ProcessId = GetProcessId();
    private static DevHttpServer? _server;
    private static StatusSnapshot _snapshot = new StatusSnapshot();
    private static long _frames;
    private static long _lastSample;
    private static int _lastUnityFrame = -1;

    public static int Port { get; private set; }

    public static void Start(int port)
    {
        if (_server != null) return;
        if (port < 1024 || port > 65535) throw new ArgumentOutOfRangeException(nameof(port));

        DevLog.Start();
        DevBootPatches.Register();

        var host = new GameObject("SimplyMoreFPS development API");
        UnityEngine.Object.DontDestroyOnLoad(host);
        Driver = host.AddComponent<DevDriver>();
        Application.runInBackground = true;
        Port = port;

        try
        {
            _server = new DevHttpServer(port);
            _server.Start();
            PublishStatus(true);
            DevLog.Info("Development API listening on http://127.0.0.1:" + port + "/");
        }
        catch
        {
            _server?.Dispose();
            _server = null;
            UnityEngine.Object.Destroy(host);
            Driver = null;
            throw;
        }
    }

    internal static void Stop()
    {
        _server?.Dispose();
        _server = null;
        DevLog.Stop();
    }

    internal static object CachedStatus()
    {
        StatusSnapshot sample = Volatile.Read(ref _snapshot);
        return new
        {
            ok = true,
            processId = ProcessId,
            port = Port,
            sample,
            sampleAgeMs = ElapsedMs(sample.SampledAt),
            uptimeMs = ElapsedMs(StartedAt),
            pending = MainThread.Pending,
            completed = MainThread.Completed,
            cancelled = MainThread.Cancelled,
            errors = DevLog.ErrorCount
        };
    }

    // Sampled on the main thread at most ten times a second; /status serves the last sample.
    internal static void PublishStatus(bool force = false)
    {
        int unityFrame = Time.frameCount;
        if (unityFrame != _lastUnityFrame)
        {
            _lastUnityFrame = unityFrame;
            _frames++;
        }

        long now = Stopwatch.GetTimestamp();
        if (!force && (now - _lastSample) * 1000.0 / Stopwatch.Frequency < 100) return;
        _lastSample = now;

        Map? map = Find.CurrentMap;
        CameraDriver? driver = Find.CameraDriver;
        Vector3 position = driver == null ? Vector3.zero : driver.rootPos;

        var sample = new StatusSnapshot
        {
            SampledAt = now,
            Frames = _frames,
            UnityFrame = unityFrame,
            Ticks = Current.Game?.tickManager?.TicksGame ?? -1,
            ProgramState = Current.ProgramState.ToString(),
            LongEvent = LongEventHandler.ShouldWaitForEvent,
            MapId = map?.uniqueID ?? -1,
            MapSizeX = map?.Size.x ?? 0,
            MapSizeZ = map?.Size.z ?? 0,
            Pawns = map?.mapPawns?.AllPawnsSpawned.Count ?? 0,
            Colonists = map?.mapPawns?.FreeColonistsSpawnedCount ?? 0,
            CameraX = position.x,
            CameraY = position.y,
            CameraZ = position.z,
            CameraSize = driver == null ? 0 : driver.RootSize,
            Width = Screen.width,
            Height = Screen.height,
            Smf = SampleMod()
        };

        Volatile.Write(ref _snapshot, sample);
    }

    internal static MainThread.Job<byte[]> CaptureScreenshot()
    {
        return MainThread.Enqueue<byte[]>(job =>
        {
            if (Driver == null) throw new InvalidOperationException("Development driver is unavailable.");
            Driver.StartCoroutine(CaptureAtEndOfFrame(job));
        });
    }

    private static double ElapsedMs(long then) => (Stopwatch.GetTimestamp() - then) * 1000.0 / Stopwatch.Frequency;

    private static int GetProcessId()
    {
        using (Process process = Process.GetCurrentProcess())
        {
            return process.Id;
        }
    }

    private static object? SampleMod()
    {
        if (!HybridSession.Installed) return null;
        return new { renderer = "native-window", pipeline = HybridSession.Snapshot() };
    }

    private static IEnumerator CaptureAtEndOfFrame(MainThread.Job<byte[]> job)
    {
        yield return new WaitForEndOfFrame();
        Texture2D? texture = null;

        try
        {
            texture = ScreenCapture.CaptureScreenshotAsTexture();
            if (texture == null) throw new InvalidOperationException("Unity returned no screenshot texture.");
            byte[] png = ImageConversion.EncodeToPNG(texture);
            if (png == null || png.Length == 0) throw new InvalidOperationException("Unity returned an empty PNG.");
            job.Complete(png);
        }
        catch (Exception ex)
        {
            job.Fail(ex);
        }
        finally
        {
            if (texture != null) UnityEngine.Object.Destroy(texture);
        }
    }
}

internal sealed class StatusSnapshot
{
    public long SampledAt;
    public long Frames;
    public int UnityFrame;
    public int Ticks = -1;
    public string ProgramState = "Starting";
    public bool LongEvent;
    public int MapId = -1;
    public int MapSizeX;
    public int MapSizeZ;
    public int Pawns;
    public int Colonists;
    public float CameraX;
    public float CameraY;
    public float CameraZ;
    public float CameraSize;
    public int Width;
    public int Height;
    public object? Smf;
}

// -smf-test-map-size=N makes the quick-test map N x N cells.
internal static class DevBootPatches
{
    private static int _mapSize;

    internal static void Register()
    {
        if (!GenCommandLine.TryGetCommandLineArg("smf-test-map-size", out string value)) return;
        if (!int.TryParse(value, out _mapSize) || _mapSize < 100 || _mapSize > 250)
            throw new ArgumentException("-smf-test-map-size must be an integer from 100 through 250.");

        new Harmony("olli.simplymorefps.dev.quicktest").Patch(
            AccessTools.Method(typeof(Root_Play), nameof(Root_Play.SetupForQuickTestPlay)),
            postfix: new HarmonyMethod(typeof(DevBootPatches), nameof(QuickTestPostfix)));
        DevLog.Info("Quick-test map size set to " + _mapSize + ".");
    }

    private static void QuickTestPostfix()
    {
        Find.GameInitData.mapSize = _mapSize;
    }
}

public sealed class DevDriver : MonoBehaviour
{
    private bool _reportedStatusError;

    private void Update()
    {
        MainThread.Drain();

        try
        {
            SmfDev.PublishStatus();
        }
        catch (Exception ex)
        {
            if (!_reportedStatusError) DevLog.Error("status sampling", ex);
            _reportedStatusError = true;
        }
    }

    private void OnApplicationQuit() => SmfDev.Stop();
}
