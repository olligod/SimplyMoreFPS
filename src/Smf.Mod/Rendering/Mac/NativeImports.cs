using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Threading;

namespace SimplyMoreFPS.Rendering.Mac;

// Emits ordinary Mono P/Invoke stubs for a bundle referenced by its relative
// plugin name, so Unity's own resolver loads it and runs UnityPluginLoad.
public sealed class NativeImports
{
    private static int assemblyCount;
    private readonly int owner = Thread.CurrentThread.ManagedThreadId;
    private readonly string library;
    private readonly ModuleBuilder module;
    private readonly Dictionary<string, Delegate> imports = new Dictionary<string, Delegate>(StringComparer.Ordinal);
    private int importCount;

    public NativeImports(string library)
    {
        if (string.IsNullOrWhiteSpace(library) || library.IndexOf('\0') >= 0)
            throw new ArgumentException("A native library route is required.");

        this.library = library;
        var name = new AssemblyName("Smf.NativeImports." + Interlocked.Increment(ref assemblyCount));
        // Not collectible: the stubs must live as long as the native plugin.
        module = AppDomain.CurrentDomain.DefineDynamicAssembly(name, AssemblyBuilderAccess.Run).DefineDynamicModule(name.Name);
    }

    public T Bind<T>(string entryPoint) where T : class
    {
        RequireOwnerThread();
        if (string.IsNullOrWhiteSpace(entryPoint) || entryPoint.IndexOf('\0') >= 0)
            throw new ArgumentException("An exact export name is required.");

        Type delegateType = typeof(T);
        if (!typeof(Delegate).IsAssignableFrom(delegateType)) throw new ArgumentException("A delegate type is required.");
        string key = delegateType.AssemblyQualifiedName + ":" + entryPoint;
        if (imports.TryGetValue(key, out Delegate existing)) return (T)(object)existing;

        MethodInfo invoke = delegateType.GetMethod("Invoke");
        var calling = (UnmanagedFunctionPointerAttribute)Attribute.GetCustomAttribute(delegateType, typeof(UnmanagedFunctionPointerAttribute));
        if (calling == null || calling.CallingConvention != CallingConvention.Cdecl)
            throw new ArgumentException("The native ABI requires an explicit Cdecl delegate.");

        ParameterInfo[] parameters = invoke.GetParameters();
        Type[] types = new Type[parameters.Length];
        for (int i = 0; i < types.Length; ++i)
        {
            types[i] = parameters[i].ParameterType;
            ValidateType(types[i]);
            // Strings go through a pinned IntPtr; no marshalling metadata here.
            if (parameters[i].GetCustomAttributes(typeof(MarshalAsAttribute), false).Length != 0)
                throw new ArgumentException("MarshalAs parameters are unsupported by the POD import builder.");
        }

        ValidateType(invoke.ReturnType);

        var type = module.DefineType("Import" + ++importCount, TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed);
        MethodBuilder method = type.DefinePInvokeMethod(
            "Call",
            library,
            entryPoint,
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.PinvokeImpl,
            CallingConventions.Standard,
            invoke.ReturnType,
            types,
            CallingConvention.Cdecl,
            CharSet.Ansi);
        method.SetImplementationFlags(method.GetMethodImplementationFlags() | MethodImplAttributes.PreserveSig);
        for (int i = 0; i < parameters.Length; ++i)
        {
            method.DefineParameter(i + 1, parameters[i].Attributes & (ParameterAttributes.In | ParameterAttributes.Out), parameters[i].Name);
        }

        MethodInfo built = type.CreateType().GetMethod("Call");
        var value = Delegate.CreateDelegate(delegateType, built);
        imports.Add(key, value);
        return (T)(object)value;
    }

    private static void ValidateType(Type value)
    {
        if (value.IsByRef) value = value.GetElementType();
        if (value == typeof(void) || value == typeof(IntPtr) || value == typeof(UIntPtr)) return;
        if (value.IsEnum)
        {
            ValidateType(Enum.GetUnderlyingType(value));
            return;
        }

        if (value == typeof(bool) || value == typeof(char) || !value.IsValueType)
            throw new ArgumentException("Native imports require fixed-width POD values.");
        if (value.IsPrimitive) return;
        if (!value.IsLayoutSequential && !value.IsExplicitLayout)
            throw new ArgumentException("Native structs require explicit layout.");

        foreach (FieldInfo field in value.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
        {
            ValidateType(field.FieldType);
        }
    }

    private void RequireOwnerThread()
    {
        if (Thread.CurrentThread.ManagedThreadId != owner)
            throw new InvalidOperationException("Only the owning main thread may create native imports.");
    }
}
