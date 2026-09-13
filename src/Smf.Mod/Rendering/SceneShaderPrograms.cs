using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using SimplyMoreFPS.Rendering.Shaders;

namespace SimplyMoreFPS.Rendering;

internal static class SceneShaderPrograms
{
    private static NativeModule? module;
    private static RegisterFn? register;
    private static readonly Dictionary<string, ulong> Registered = new Dictionary<string, ulong>();

    internal static void Bind(NativeModule native)
    {
        if (module != null && !ReferenceEquals(module, native))
            throw new InvalidOperationException("Scene shader registry changed its native module.");
        module = native;
    }

    internal static ulong Register(string key, IReadOnlyList<CompiledShaderPrograms.Variant> passes)
    {
        if (Registered.TryGetValue(key, out ulong existing))
            return existing;
        if (module == null)
            throw new PlatformNotSupportedException("This native renderer has no installed shader executor.");
        if (register == null)
            register = module.Bind<RegisterFn>("smf_scene_register_filter");
        if (Marshal.SizeOf<Stage>() != 40 || Marshal.SizeOf<Pass>() != 80 || Marshal.SizeOf<Definition>() != 24 ||
            Marshal.SizeOf<Buffer>() != 24 || Marshal.SizeOf<Parameter>() != 16 || Marshal.SizeOf<Texture>() != 16)
            throw new InvalidOperationException("Scene shader registry layout mismatch.");

        using (var memory = new PinnedArrays())
        {
            var nativePasses = new Pass[passes.Count];
            for (int i = 0; i < passes.Count; ++i)
            {
                nativePasses[i] = new Pass
                {
                    Vertex = Describe(passes[i].Vertex, memory),
                    Fragment = Describe(passes[i].Fragment, memory)
                };
            }
            var definition = new Definition
            {
                Size = 24,
                Version = 1,
                PassCount = checked((uint)nativePasses.Length),
                Passes = memory.Pin(nativePasses)
            };
            int result = register(ref definition, out ulong handle);
            if (result != 0 || handle == 0)
                throw new InvalidOperationException("Scene shader registration failed: 0x" + result.ToString("X8"));
            Registered.Add(key, handle);
            return handle;
        }
    }

    private static Stage Describe(CompiledShaderPrograms.Program program, PinnedArrays memory)
    {
        var textures = new Texture[program.Textures.Length];
        for (int i = 0; i < textures.Length; ++i)
        {
            var input = program.Textures[i];
            textures[i] = new Texture
            {
                Role = TextureRole(input.Name),
                TextureSlot = checked((uint)input.TextureSlot),
                SamplerSlot = checked((uint)input.SamplerSlot)
            };
        }
        var buffers = new Buffer[program.Buffers.Length];
        for (int i = 0; i < buffers.Length; ++i)
        {
            var input = program.Buffers[i];
            var parameters = new Parameter[input.Parameters.Length];
            for (int j = 0; j < parameters.Length; ++j)
            {
                var value = input.Parameters[j];
                parameters[j] = new Parameter
                {
                    Role = ParameterRole(value.Name),
                    Offset = checked((uint)value.Offset),
                    Rows = checked((uint)value.Rows),
                    Columns = checked((uint)value.Columns)
                };
            }
            buffers[i] = new Buffer
            {
                Slot = checked((uint)input.Slot),
                Size = checked((uint)input.Size),
                ParameterCount = checked((uint)parameters.Length),
                Parameters = memory.Pin(parameters)
            };
        }
        return new Stage
        {
            Bytecode = memory.Pin(program.Bytecode),
            BytecodeSize = checked((uint)program.Bytecode.Length),
            TextureCount = checked((uint)textures.Length),
            Textures = memory.Pin(textures),
            Buffers = memory.Pin(buffers),
            BufferCount = checked((uint)buffers.Length)
        };
    }

    private static uint TextureRole(string name)
    {
        switch (name)
        {
            case "_MainTex":
                return 1;
            case "_AreaTex":
                return 2;
            case "_SearchTex":
                return 3;
            case "_BlendTex":
                return 4;
            default:
                throw new InvalidOperationException("Unknown scene shader texture: " + name);
        }
    }

    private static uint ParameterRole(string name)
    {
        switch (name)
        {
            case "_MainTex_TexelSize":
                return 1;
            case "_TexelSize":
                return 2;
            case "_SubpixelBlending":
                return 3;
            case "_EdgeThreshold":
                return 4;
            case "_EdgeThresholdMin":
                return 5;
            case "_SmaaThreshold":
                return 6;
            case "_Sharpness":
                return 7;
            case "unity_ObjectToWorld":
                return 8;
            case "unity_MatrixVP":
                return 9;
            default:
                throw new InvalidOperationException("Unknown scene shader constant: " + name);
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int RegisterFn(ref Definition definition, out ulong handle);

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Texture
    {
        internal uint Role, TextureSlot, SamplerSlot, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Parameter
    {
        internal uint Role, Offset, Rows, Columns;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Buffer
    {
        internal uint Slot, Size, ParameterCount, Reserved;
        internal ulong Parameters;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Stage
    {
        internal ulong Bytecode;
        internal uint BytecodeSize, TextureCount;
        internal ulong Textures, Buffers;
        internal uint BufferCount, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Pass
    {
        internal Stage Vertex, Fragment;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct Definition
    {
        internal uint Size, Version, PassCount, Reserved;
        internal ulong Passes;
    }

    private sealed class PinnedArrays : IDisposable
    {
        private readonly List<GCHandle> pins = new List<GCHandle>();

        internal ulong Pin(Array array)
        {
            if (array.Length == 0)
                return 0;
            GCHandle pin = GCHandle.Alloc(array, GCHandleType.Pinned);
            pins.Add(pin);
            return unchecked((ulong)pin.AddrOfPinnedObject().ToInt64());
        }

        public void Dispose()
        {
            foreach (GCHandle pin in pins)
                pin.Free();
            pins.Clear();
        }
    }
}
