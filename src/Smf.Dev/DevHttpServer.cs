using System;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using Newtonsoft.Json;
using Newtonsoft.Json.Serialization;
using UnityEngine;

namespace SimplyMoreFPS.Dev;

// Loopback-only HTTP server. Cheap routes answer on the accept thread; the rest get a worker each.
internal sealed class DevHttpServer : IDisposable
{
    private const int BodyLimit = 128 * 1024;
    private const int WorkerLimit = 8;
    private static readonly JsonSerializerSettings JsonSettings = new JsonSerializerSettings
    {
        ContractResolver = new CamelCasePropertyNamesContractResolver(),
        NullValueHandling = NullValueHandling.Ignore
    };

    private readonly HttpListener _listener = new HttpListener();
    private readonly string _origin;
    private volatile bool _stopped;
    private int _workers;

    internal DevHttpServer(int port)
    {
        _origin = "http://127.0.0.1:" + port;
        _listener.Prefixes.Add(_origin + "/");
        _listener.IgnoreWriteExceptions = true;
    }

    internal void Start()
    {
        _listener.Start();
        new Thread(Accept)
        {
            Name = "smf-dev-http",
            IsBackground = true
        }.Start();
    }

    public void Dispose()
    {
        _stopped = true;
        _listener.Close();
    }

    private void Accept()
    {
        while (!_stopped)
        {
            HttpListenerContext context;
            try
            {
                context = _listener.GetContext();
            }
            catch (HttpListenerException ex)
            {
                if (!_stopped) DevLog.Error("HTTP accept", ex);
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }

            try
            {
                Dispatch(context);
            }
            catch (Exception ex)
            {
                DevLog.Error("HTTP dispatch", ex);
                TryError(context, 500, ex);
            }
        }
    }

    private void Dispatch(HttpListenerContext context)
    {
        if (!IPAddress.IsLoopback(context.Request.RemoteEndPoint.Address))
        {
            WriteJson(context, 403, new { ok = false, error = "Only loopback clients are accepted." });
            return;
        }

        string? origin = context.Request.Headers["Origin"];
        if (origin != null && origin != _origin)
        {
            WriteJson(context, 403, new { ok = false, error = "Cross-origin browser requests are not accepted." });
            return;
        }

        string path = context.Request.Url!.AbsolutePath.TrimEnd('/').ToLowerInvariant();
        string verb = context.Request.HttpMethod;
        string? expected = ExpectedVerb(path);
        if (expected == null)
        {
            WriteJson(context, 404, new { ok = false, error = "Unknown route. GET / lists routes." });
            return;
        }

        if (verb != expected)
        {
            context.Response.Headers["Allow"] = expected;
            WriteJson(context, 405, new { ok = false, error = "Use " + expected + " for this route." });
            return;
        }

        switch (path)
        {
            case "":
                WriteJson(context, 200, Routes());
                return;

            case "/ping":
                WriteJson(context, 200, new { ok = true, pong = true, utc = DateTime.UtcNow.ToString("O") });
                return;

            case "/status":
                WriteJson(context, 200, SmfDev.CachedStatus());
                return;

            case "/logs":
                WriteJson(context, 200, DevLog.Snapshot());
                return;
        }

        if (Interlocked.Increment(ref _workers) > WorkerLimit)
        {
            Interlocked.Decrement(ref _workers);
            WriteJson(context, 503, new { ok = false, error = "Development API has eight active requests. Retry after one finishes." });
            return;
        }

        new Thread(() => Work(context, path))
        {
            Name = "smf-dev-request",
            IsBackground = true
        }.Start();
    }

    private void Work(HttpListenerContext context, string path)
    {
        try
        {
            switch (path)
            {
                case "/eval":
                    WriteJson(context, 200, Eval.Run(ReadBody(context)));
                    break;

                case "/screenshot":
                {
                    string target = context.Request.QueryString["target"] ?? "screen";
                    if (target != "screen") throw new FormatException("Only the screen screenshot target is supported.");
                    WriteBytes(context, 200, "image/png", SmfDev.CaptureScreenshot().Wait(30000));
                    break;
                }

                case "/stall":
                {
                    string body = ReadBody(context).Trim();
                    if (!int.TryParse(body, out int milliseconds) || milliseconds < 1 || milliseconds > 60000)
                        throw new FormatException("Provide an integer stall duration between 1 and 60000 milliseconds.");

                    MainThread.Enqueue<bool>(job =>
                    {
                        Thread.Sleep(milliseconds);
                        job.Complete(true);
                    });

                    WriteJson(context, 202, new { ok = true, accepted = true, milliseconds });
                    break;
                }

                case "/quit":
                    MainThread.Enqueue<bool>(job =>
                    {
                        job.Complete(true);
                        Application.Quit();
                    });

                    WriteJson(context, 202, new { ok = true, accepted = true });
                    break;
            }
        }
        catch (FormatException ex)
        {
            TryError(context, 400, ex);
        }
        catch (JsonException ex)
        {
            TryError(context, 400, ex);
        }
        catch (BusyException ex)
        {
            TryError(context, 503, ex);
        }
        catch (TimeoutException ex)
        {
            TryError(context, 504, ex);
        }
        catch (UnobservedWorkException ex)
        {
            TryError(context, 504, ex);
        }
        catch (NotSupportedException ex)
        {
            TryError(context, 501, ex);
        }
        catch (Exception ex)
        {
            DevLog.Error("HTTP " + path, ex);
            TryError(context, 500, ex);
        }
        finally
        {
            Interlocked.Decrement(ref _workers);
        }
    }

    private static string? ExpectedVerb(string path)
    {
        switch (path)
        {
            case "":
            case "/ping":
            case "/status":
            case "/logs":
            case "/screenshot":
                return "GET";
            case "/eval":
            case "/stall":
            case "/quit":
                return "POST";
            default:
                return null;
        }
    }

    private static object Routes()
    {
        return new
        {
            ok = true,
            routes = new[]
            {
                "GET /",
                "GET /ping",
                "GET /status",
                "GET /logs",
                "POST /eval (raw synchronous C#)",
                "GET /screenshot (end-of-frame PNG)",
                "POST /stall (milliseconds)",
                "POST /quit"
            }
        };
    }

    private static string ReadBody(HttpListenerContext context)
    {
        if (context.Request.ContentLength64 > BodyLimit) throw new FormatException("Request body exceeds 128 KiB.");

        using (var output = new MemoryStream())
        {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = context.Request.InputStream.Read(buffer, 0, buffer.Length)) != 0)
            {
                if (output.Length + count > BodyLimit) throw new FormatException("Request body exceeds 128 KiB.");
                output.Write(buffer, 0, count);
            }

            return new UTF8Encoding(false, true).GetString(output.ToArray());
        }
    }

    private static void WriteJson(HttpListenerContext context, int status, object value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(JsonConvert.SerializeObject(value, JsonSettings));
        WriteBytes(context, status, "application/json; charset=utf-8", bytes);
    }

    private static void WriteBytes(HttpListenerContext context, int status, string contentType, byte[] bytes)
    {
        try
        {
            context.Response.StatusCode = status;
            context.Response.ContentType = contentType;
            context.Response.ContentLength64 = bytes.Length;
            context.Response.Headers["Cache-Control"] = "no-store";
            context.Response.OutputStream.Write(bytes, 0, bytes.Length);
        }
        finally
        {
            context.Response.Close();
        }
    }

    private static void TryError(HttpListenerContext context, int status, Exception error)
    {
        try
        {
            WriteJson(context, status, new { ok = false, error = error.Message, errorType = error.GetType().Name });
        }
        catch (Exception ex)
        {
            DevLog.Error("HTTP error response", ex);
        }
    }
}
