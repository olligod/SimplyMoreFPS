#nullable disable
using System;
using System.Diagnostics;
using System.Reflection;
using System.Text;
using UnityEngine;
using Verse;

namespace SimplyMoreFPS.Rendering;

// Call Error only on Unity's main thread. Formatting itself touches no game state.
internal static class RendererDiagnostics
{
    private static string buildInfo;

    internal static string FormatException(Exception error)
    {
        var text = new StringBuilder();
        AppendException(text, error, "");
        return text.ToString();
    }

    internal static void AppendPacket(StringBuilder text, object packet, string prefix = "", int depth = 0)
    {
        if (packet == null) return;

        foreach (var field in packet.GetType().GetFields())
        {
            object value = field.GetValue(packet);
            string name = prefix + field.Name;
            Type type = field.FieldType;

            if (depth < 2 && type.IsValueType && !type.IsPrimitive && !type.IsEnum && type.Namespace != "System")
            {
                AppendPacket(text, value, name + ".", depth + 1);
            }
            else
            {
                text.Append(' ').Append(name).Append('=').Append(value);
            }
        }
    }

    private static void AppendException(StringBuilder text, Exception error, string indent)
    {
        if (error == null)
        {
            text.Append(indent).AppendLine("<no exception supplied>");
            return;
        }

        text.Append(indent).Append(error.GetType().FullName).Append(": ").AppendLine(error.Message);

        // RimWorld patches Exception.StackTrace to drop repeated stacks, so walk the raw frames.
        try
        {
            var frames = new StackTrace(error, false).GetFrames();
            if (frames == null || frames.Length == 0)
            {
                text.Append(indent).AppendLine("  <no captured stack>");
            }
            else
            {
                foreach (var frame in frames)
                {
                    MethodBase method = frame.GetMethod();
                    text.Append(indent).Append("  at ")
                        .Append(method?.DeclaringType?.FullName ?? "<unknown type>")
                        .Append('.').Append(method == null ? "<unknown method>" : method.ToString())
                        .Append(" [IL ").Append(frame.GetILOffset()).AppendLine("]");
                }
            }
        }
        catch (Exception formatting)
        {
            text.Append(indent).Append("  <stack unavailable: ").Append(formatting.GetType().Name).AppendLine(">");
        }

        if (error is AggregateException aggregate)
        {
            for (int i = 0; i < aggregate.InnerExceptions.Count; i++)
            {
                text.Append(indent).Append("Inner ").Append(i + 1).AppendLine(":");
                AppendException(text, aggregate.InnerExceptions[i], indent + "  ");
            }
        }
        else if (error.InnerException != null)
        {
            text.Append(indent).AppendLine("Inner:");
            AppendException(text, error.InnerException, indent + "  ");
        }
    }

    internal static void Error(string context, Exception error) => Error(context, FormatException(error));

    internal static void Fallback(string context, Exception error) => Fallback(context, FormatException(error));

    internal static void Fallback(string context, string detail)
    {
        string message = FormatMessage(context, detail);
        WriteError(message);
        RendererFailureNotice.Record(message);
    }

    // The lifecycle dedupes repeated failures; nothing is suppressed here.
    internal static void Error(string context, string detail) => WriteError(FormatMessage(context, detail));

    private static string FormatMessage(string context, string detail)
    {
        if (buildInfo == null) buildInfo = ReadBuildInfo();

        return "[Simply More FPS] " + context + "\n" + buildInfo + "\n" + ReadDisplayConfiguration() + "\n" + detail;
    }

    private static void WriteError(string message)
    {
        try
        {
            Log.Error(message);
        }
        catch
        {
            UnityEngine.Debug.LogError(message);
        }
    }

    private static string ReadBuildInfo()
    {
        var assembly = typeof(RendererDiagnostics).Assembly;
        return "build=" + Safe(() => assembly.GetName().Version.ToString()) +
            " mvid=" + Safe(() => assembly.ManifestModule.ModuleVersionId.ToString("D")) +
            " platform=" + Safe(() => Application.platform.ToString()) +
            " unity=" + Safe(() => Application.unityVersion) +
            " graphics=" + Safe(() => SystemInfo.graphicsDeviceType.ToString()) +
            " device=" + Safe(() => SystemInfo.graphicsDeviceName);
    }

    // Resolution and fullscreen mode change mid-session, so sample them at the failure.
    private static string ReadDisplayConfiguration()
    {
        return "display image=" + Safe(() => Screen.width + "x" + Screen.height) +
            " mode=" + Safe(() => Screen.fullScreenMode.ToString()) +
            " resolution=" + Safe(() => Screen.currentResolution.ToString()) +
            " rendering=" + Safe(() => Display.main.renderingWidth + "x" + Display.main.renderingHeight) +
            " system=" + Safe(() => Display.main.systemWidth + "x" + Display.main.systemHeight) +
            " safeArea=" + Safe(() => Screen.safeArea.ToString());
    }

    private static string Safe(Func<string> read)
    {
        try
        {
            return read();
        }
        catch (Exception error)
        {
            return "unavailable(" + error.GetType().Name + ")";
        }
    }
}
