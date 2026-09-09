#nullable disable
using System;
using System.Threading;
using HarmonyLib;
using RimWorld.Planet;
using SimplyMoreFPS.Rendering.CameraControl;
using UnityEngine;
using UnityEngine.SceneManagement;
using Verse;

namespace SimplyMoreFPS.Rendering;

// Every callback here runs on the Unity main thread; none is ever handed to native code.
public delegate bool ReadMapPose(ulong frame, out CameraPose pose);

public sealed class RimWorldSceneOwner : IMainSceneOwner, IMainSceneLifetime
{
    private static readonly AccessTools.FieldRef<CameraDriver, Vector3> ReadRootPosition =
        AccessTools.FieldRefAccess<CameraDriver, Vector3>("rootPos");

    private static readonly AccessTools.FieldRef<CameraDriver, float> ReadRootSize =
        AccessTools.FieldRefAccess<CameraDriver, float>("rootSize");

    private readonly ReadMapPose readPose;
    private readonly Action validateAlpha;
    private readonly Action revokeCamera;
    private readonly Func<bool> cameraRevoked;
    private readonly int mainThread = Thread.CurrentThread.ManagedThreadId;
    private readonly string nativePath;
    private readonly string kernelPath;
    private readonly GuiShaderResources shaders;

    private GameObject owner;
    private Camera pendingCamera;
    private CameraPose pendingPose;
    private CameraPose completedPose;
    private bool pendingMap;
    private bool completedMap;
    private bool cameraEnabled;
    private ulong sourceSequence;
    private ulong sourceEpoch = 1;
    private int stampedMap = -1;
    private int stampedCamera;

    internal RimWorldSceneOwner(string nativeLibrary, string kernelLibrary, GuiShaderResources guiShaders)
    {
        nativePath = nativeLibrary;
        kernelPath = kernelLibrary;
        shaders = guiShaders;
        readPose = ReadCompletedPose;
        validateAlpha = shaders.Validate;
        revokeCamera = RevokeOwnedCamera;
        cameraRevoked = () => CameraOwnershipAdapter.Released;
    }

    public RimWorldSceneOwner(ReadMapPose readMapPose, Action validateGuiAlpha, Action releaseCamera, Func<bool> cameraReleased)
    {
        readPose = readMapPose ?? throw new ArgumentNullException(nameof(readMapPose));
        validateAlpha = validateGuiAlpha ?? throw new ArgumentNullException(nameof(validateGuiAlpha));
        revokeCamera = releaseCamera ?? throw new ArgumentNullException(nameof(releaseCamera));
        cameraRevoked = cameraReleased ?? throw new ArgumentNullException(nameof(cameraReleased));
    }

    private void CheckMain()
    {
        if (Thread.CurrentThread.ManagedThreadId != mainThread)
        {
            throw new InvalidOperationException("Scene, GUI material and camera ownership are Unity main-thread state.");
        }
    }

    public void Start(GameObject persistentOwner)
    {
        CheckMain();
        owner = persistentOwner;
        if (shaders == null) return;

        Camera.onPreRender += BeforeMapRender;
        Camera.onPostRender += AfterMapRender;
    }

    public void EnableCamera()
    {
        CheckMain();
        if (shaders == null || cameraEnabled) return;

        NativeCameraBridge.Install(nativePath, kernelPath, owner);
        cameraEnabled = true;
    }

    private void RevokeOwnedCamera()
    {
        CheckMain();
        try
        {
            if (cameraEnabled) NativeCameraBridge.Stop();
        }
        finally
        {
            if (CameraOwnershipAdapter.Installed) CameraOwnershipAdapter.Remove();
            cameraEnabled = false;
        }
    }

    public void Stop()
    {
        CheckMain();
        try
        {
            ReleaseCamera();
        }
        finally
        {
            Camera.onPreRender -= BeforeMapRender;
            Camera.onPostRender -= AfterMapRender;
            shaders?.Restore();
        }
    }

    private unsafe void BeforeMapRender(Camera camera)
    {
        CheckMain();
        if (camera != Find.Camera) return;

        pendingMap = false;
        completedMap = false;

        var map = Find.CurrentMap;
        var driver = Find.CameraDriver;
        if (!MapSceneReadiness.Ready || map == null || driver == null || !camera.enabled || !camera.orthographic ||
            camera.targetTexture != null || WorldRendererUtility.WorldSelected)
        {
            return;
        }

        ulong epoch = 0;
        ulong applied = 0;
        int mapId = map.uniqueID;
        if (CameraOwnershipAdapter.Installed &&
            (!CameraOwnershipAdapter.TryGetCaptureStamp(out epoch, out applied, out mapId) || mapId != map.uniqueID))
        {
            return;
        }

        int cameraId = camera.GetInstanceID();
        if (stampedMap != map.uniqueID || stampedCamera != cameraId)
        {
            sourceEpoch = checked(sourceEpoch + 1);
            stampedMap = map.uniqueID;
            stampedCamera = cameraId;
        }

        Vector3 root = ReadRootPosition(driver);
        Vector3 position = camera.transform.position;
        Rect pixel = camera.pixelRect;

        var pose = new CameraPose
        {
            Size = 264,
            Version = 2,
            FrameId = checked(++sourceSequence),
            UnityFrame = unchecked((ulong)Time.frameCount),
            CameraId = unchecked((ulong)cameraId),
            Epoch = sourceEpoch,
            X = position.x,
            Y = position.y,
            Z = position.z,
            OrthographicSize = camera.orthographicSize,
            PixelX = pixel.x,
            PixelY = pixel.y,
            PixelWidth = pixel.width,
            PixelHeight = pixel.height,
            CameraEpoch = epoch,
            AppliedSequence = applied,
            ModelRevision = 1,
            MapId = map.uniqueID,
            RootX = root.x,
            RootY = root.y,
            RootZ = root.z,
            RootSize = ReadRootSize(driver)
        };

        Matrix4x4 view = camera.worldToCameraMatrix;
        Matrix4x4 projection = camera.projectionMatrix;

        for (int i = 0; i < 16; ++i)
        {
            pose.WorldToCamera[i] = view[i];
            pose.Projection[i] = projection[i];
        }

        pendingPose = pose;
        pendingCamera = camera;
        pendingMap = true;
    }

    private unsafe void AfterMapRender(Camera camera)
    {
        CheckMain();

        // onPostRender runs after the camera's AfterEverything commands. Only the pre-render
        // stamp gets promoted; if anything moved since then the frame is dropped, not re-read.
        if (!pendingMap || camera != pendingCamera || pendingPose.UnityFrame != unchecked((ulong)Time.frameCount)) return;

        var map = Find.CurrentMap;
        var driver = Find.CameraDriver;
        if (map == null || driver == null || map.uniqueID != pendingPose.MapId)
        {
            pendingMap = false;
            return;
        }

        Vector3 position = camera.transform.position;
        Vector3 root = ReadRootPosition(driver);
        Rect pixel = camera.pixelRect;
        if (pendingPose.X != position.x || pendingPose.Y != position.y || pendingPose.Z != position.z ||
            pendingPose.RootX != root.x || pendingPose.RootY != root.y || pendingPose.RootZ != root.z ||
            pendingPose.RootSize != ReadRootSize(driver) ||
            pendingPose.OrthographicSize != camera.orthographicSize ||
            pendingPose.PixelX != pixel.x || pendingPose.PixelY != pixel.y ||
            pendingPose.PixelWidth != pixel.width || pendingPose.PixelHeight != pixel.height)
        {
            pendingMap = false;
            return;
        }

        // Fixed buffers can only be indexed on a local copy.
        CameraPose stamp = pendingPose;
        Matrix4x4 view = camera.worldToCameraMatrix;
        Matrix4x4 projection = camera.projectionMatrix;

        for (int i = 0; i < 16; ++i)
        {
            if (stamp.WorldToCamera[i] != view[i] || stamp.Projection[i] != projection[i])
            {
                pendingMap = false;
                return;
            }
        }

        completedPose = pendingPose;
        completedMap = true;
        pendingMap = false;
    }

    private bool ReadCompletedPose(ulong frame, out CameraPose pose)
    {
        pose = default;
        if (!completedMap || completedPose.UnityFrame != frame || Find.CurrentMap == null || Find.Camera == null ||
            completedPose.MapId != Find.CurrentMap.uniqueID ||
            completedPose.CameraId != unchecked((ulong)Find.Camera.GetInstanceID()))
        {
            return false;
        }

        pose = completedPose;
        return true;
    }

    public SceneContext ReadContext()
    {
        CheckMain();
        var root = Find.Root;
        var map = Find.CurrentMap;
        var camera = Find.Camera;

        // This only decides whether a colony-camera pose exists. Title, planet and custom
        // backgrounds still use the pre-GUI base copy and the HUD in the same session.
        bool mapPose = MapSceneReadiness.Ready && map != null && camera != null && camera.enabled && !WorldRendererUtility.WorldSelected;

        return new SceneContext
        {
            SceneHandle = SceneManager.GetActiveScene().handle,
            RootId = root == null ? 0 : root.GetInstanceID(),
            MapId = mapPose ? map.uniqueID : -1,
            CameraId = mapPose ? camera.GetInstanceID() : 0,
            HasMap = mapPose ? 1u : 0u,
            Width = unchecked((uint)Screen.width),
            Height = unchecked((uint)Screen.height),
            UiWidth = unchecked((uint)UI.screenWidth),
            UiHeight = unchecked((uint)UI.screenHeight),
            UiScale = Prefs.UIScale,
            ColorSpace = unchecked((uint)QualitySettings.activeColorSpace),
            GraphicsApi = unchecked((uint)SystemInfo.graphicsDeviceType)
        };
    }

    public bool TryReadMapPose(ulong frame, out CameraPose pose)
    {
        CheckMain();
        return readPose(frame, out pose);
    }

    public void ValidateGuiAlpha()
    {
        CheckMain();
        validateAlpha();
    }

    public void ReleaseCamera()
    {
        CheckMain();
        revokeCamera();
    }

    public bool CameraReleased
    {
        get
        {
            CheckMain();
            return cameraRevoked();
        }
    }
}
