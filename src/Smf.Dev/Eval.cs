using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Scripting;
using Microsoft.CodeAnalysis.Scripting;

namespace SimplyMoreFPS.Dev;

public sealed class EvalGlobals
{
    public Dictionary<string, object?> V { get; } = new Dictionary<string, object?>();

    internal StringBuilder Output = new StringBuilder();

    public void Print(object? value) => Output.AppendLine(value?.ToString() ?? "null");
}

// Compiles a C# snippet on the request thread and runs it on the Unity main thread.
internal static class Eval
{
    private static readonly object CompileGate = new object();
    private static readonly EvalGlobals Globals = new EvalGlobals();
    private static readonly Dictionary<Assembly, MetadataReference> References = new Dictionary<Assembly, MetadataReference>();
    private static Script<object>? _baseScript;

    internal static object Run(string code)
    {
        if (string.IsNullOrWhiteSpace(code)) throw new FormatException("Provide raw C# source in the request body.");

        ScriptRunner<object> runner;
        lock (CompileGate)
        {
            IEnumerable<Assembly> assemblies = AppDomain.CurrentDomain.GetAssemblies()
                .Where(assembly => !assembly.IsDynamic && !string.IsNullOrEmpty(assembly.Location));
            ScriptOptions options = ScriptOptions.Default
                .WithReferences(assemblies.Select(Reference))
                .WithImports("System", "System.Linq", "System.Collections.Generic", "UnityEngine", "Verse", "RimWorld", "SimplyMoreFPS", "SimplyMoreFPS.Dev")
                .WithEmitDebugInformation(false)
                .WithAllowUnsafe(false);

            // Every request continues one shared empty script so Roslyn's implicit references are created once.
            if (_baseScript == null) _baseScript = CSharpScript.Create<object>(string.Empty, options, typeof(EvalGlobals));
            Script<object> script = _baseScript.ContinueWith<object>(code, options);

            bool usesAwait = script.GetCompilation().SyntaxTrees
                .SelectMany(tree => tree.GetRoot().DescendantTokens())
                .Any(token => token.IsKind(SyntaxKind.AwaitKeyword));
            if (usesAwait)
            {
                return new
                {
                    ok = false,
                    error = "Eval scripts must be synchronous. await is rejected because Unity cannot pump continuations "
                        + "while the main thread is executing the script."
                };
            }

            Diagnostic[] errors = script.Compile().Where(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error).ToArray();
            if (errors.Length != 0)
            {
                return new
                {
                    ok = false,
                    compileErrors = errors.Select(error => error.ToString()).ToArray()
                };
            }

            runner = script.CreateDelegate();
        }

        return MainThread.Submit(() => Execute(runner));
    }

    // One in-memory image per assembly: file-backed references leak descriptors on Mono until finalized.
    private static MetadataReference Reference(Assembly assembly)
    {
        if (!References.TryGetValue(assembly, out MetadataReference? reference))
        {
            string path = assembly.Location;
            reference = MetadataReference.CreateFromImage(File.ReadAllBytes(path), filePath: path);
            References.Add(assembly, reference);
        }

        return reference;
    }

    private static object Execute(ScriptRunner<object> runner)
    {
        Globals.Output.Clear();

        using (var stdout = new StringWriter())
        {
            TextWriter previous = Console.Out;
            try
            {
                Console.SetOut(stdout);
                Task<object> task = runner(Globals);
                if (!task.IsCompleted)
                {
                    return new
                    {
                        ok = false,
                        error = "Script returned an incomplete task. It was not waited on; effects may still occur.",
                        output = Globals.Output + stdout.ToString()
                    };
                }

                object? value = task.GetAwaiter().GetResult();
                return new
                {
                    ok = true,
                    result = value?.ToString(),
                    resultType = value?.GetType().FullName,
                    output = Globals.Output + stdout.ToString()
                };
            }
            catch (Exception ex)
            {
                DevLog.Error("eval execution", ex);
                return new
                {
                    ok = false,
                    error = ex.ToString(),
                    output = Globals.Output + stdout.ToString()
                };
            }
            finally
            {
                Console.SetOut(previous);
            }
        }
    }
}
