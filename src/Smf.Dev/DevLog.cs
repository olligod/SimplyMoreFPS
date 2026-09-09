using System;
using System.Collections.Generic;
using System.Threading;
using UnityEngine;

namespace SimplyMoreFPS.Dev;

internal static class DevLog
{
    private const int Capacity = 400;
    private static readonly object Gate = new object();
    private static readonly Queue<object> Entries = new Queue<object>();
    private static long _sequence;
    private static long _errors;
    private static bool _started;

    internal static long ErrorCount => Interlocked.Read(ref _errors);

    internal static void Start()
    {
        if (_started)
            return;

        _started = true;
        Application.logMessageReceivedThreaded += Capture;
    }

    internal static void Stop()
    {
        if (!_started)
            return;

        Application.logMessageReceivedThreaded -= Capture;
        _started = false;
    }

    internal static void Info(string message) => Debug.Log("[SimplyMoreFPS Dev] " + message);

    internal static void Error(string operation, Exception error)
        => Debug.LogError("[SimplyMoreFPS Dev] " + operation + ": " + error);

    private static void Capture(string message, string stack, LogType type)
    {
        if (type == LogType.Error || type == LogType.Exception || type == LogType.Assert)
            Interlocked.Increment(ref _errors);

        lock (Gate)
        {
            if (Entries.Count >= Capacity)
                Entries.Dequeue();

            Entries.Enqueue(new
            {
                sequence = ++_sequence,
                utc = DateTime.UtcNow.ToString("O"),
                type = type.ToString(),
                message,
                stack
            });
        }
    }

    internal static object Snapshot()
    {
        lock (Gate)
            return new
            {
                ok = true,
                sequence = _sequence,
                errors = ErrorCount,
                entries = Entries.ToArray()
            };
    }
}
