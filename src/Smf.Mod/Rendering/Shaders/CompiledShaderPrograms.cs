#nullable disable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using AssetsTools.NET;
using AssetsTools.NET.Extra;
using LZ4ps;

namespace SimplyMoreFPS.Rendering.Shaders;

internal sealed class CompiledShaderPrograms
{
    private const int MaximumBundleBytes = 64 * 1024 * 1024;
    private const int MaximumProgramBytes = 16 * 1024 * 1024;

    internal string Name;
    internal Pass[] Passes;

    internal sealed class Pass
    {
        internal Variant[] Variants;
    }

    internal sealed class Variant
    {
        internal string[] Keywords;
        internal Program Vertex;
        internal Program Fragment;
    }

    internal sealed class Program
    {
        internal int BlobIndex;
        internal byte[] Bytecode;
        internal TextureBinding[] Textures;
        internal ConstantBuffer[] Buffers;
    }

    internal sealed class TextureBinding
    {
        internal string Name;
        internal int TextureSlot;
        internal int SamplerSlot;
    }

    internal sealed class ConstantBuffer
    {
        internal string Name;
        internal int Slot;
        internal int Size;
        internal Parameter[] Parameters;
    }

    internal sealed class Parameter
    {
        internal string Name;
        internal int Offset;
        internal int Rows;
        internal int Columns;
    }

    private sealed class Blob
    {
        internal int Type;
        internal string[] Keywords;
        internal byte[] Bytecode;
        internal int BufferCount;
        internal int TextureCount;
        internal int SamplerCount;
    }

    // Configuration-time only; the returned programs no longer depend on the bundle reader.
    internal static CompiledShaderPrograms Load(string bundlePath, string shaderName)
    {
        if (string.IsNullOrWhiteSpace(bundlePath) || string.IsNullOrWhiteSpace(shaderName))
        {
            throw new ArgumentException("A shader bundle path and shader name are required.");
        }

        byte[] bytes;
        using (var input = File.OpenRead(bundlePath))
        {
            Require(input.Length >= 64 && input.Length <= MaximumBundleBytes, "Invalid shader bundle length.");
            using (var reader = new BinaryReader(input))
            {
                bytes = reader.ReadBytes(checked((int)input.Length));
                Require(bytes.Length == input.Length, "Truncated shader bundle.");
            }
        }

        ValidateBundleHeader(bytes);
        var manager = new AssetsManager();
        try
        {
            var bundle = manager.LoadBundleFile(new MemoryStream(bytes, false), bundlePath, false);
            long unpacked = 0;
            long packed = 0;
            foreach (AssetBundleBlockInfo block in bundle.file.BlockAndDirInfo.BlockInfos)
            {
                unpacked += block.DecompressedSize;
                packed += block.CompressedSize;
                Require(block.DecompressedSize > 0 && block.CompressedSize > 0 &&
                    unpacked <= 4L * MaximumBundleBytes && packed <= bytes.Length,
                    "Shader bundle block sizes exceed their limits.");
            }

            if (bundle.file.DataIsCompressed)
            {
                bundle.file = BundleHelper.UnpackBundle(bundle.file);
            }

            CompiledShaderPrograms found = null;
            var entries = bundle.file.BlockAndDirInfo.DirectoryInfos;
            Require(entries.Count > 0 && entries.Count <= 64, "Invalid shader bundle directory.");
            for (int index = 0; index < entries.Count; index++)
            {
                Require(entries[index].Offset >= 0 && entries[index].DecompressedSize > 0 &&
                    entries[index].Offset <= unpacked - entries[index].DecompressedSize,
                    "Shader bundle entry is outside its data.");
                if (!bundle.file.IsAssetsFile(index))
                {
                    continue;
                }

                var instance = manager.LoadAssetsFileFromBundle(bundle, index, false);
                AssetsFile file = instance.file;
                Require((file.Header.Version == 21 || file.Header.Version == 22) && !file.Header.Endianness &&
                    file.Metadata.TypeTreeEnabled && file.Header.FileSize == entries[index].DecompressedSize,
                    "Shader bundle has an unsupported serialized layout.");

                foreach (AssetFileInfo info in file.GetAssetsOfType(AssetClassID.Shader))
                {
                    long offset = info.GetAbsoluteByteOffset(file);
                    Require(info.ByteSize > 0 && info.ByteSize <= MaximumProgramBytes &&
                        offset >= file.Header.DataOffset && offset <= file.Header.FileSize - info.ByteSize,
                        "Shader object is outside its serialized file.");

                    AssetTypeTemplateField template = manager.GetTemplateBaseField(instance, info);
                    file.Reader.Position = offset;
                    byte[] data = file.Reader.ReadBytes(checked((int)info.ByteSize));
                    Require(data.Length == info.ByteSize, "Truncated shader object.");
                    using (var reader = new AssetsFileReader(new MemoryStream(data, false)))
                    {
                        AssetTypeValueField value = template.MakeValue(reader);
                        Require(reader.Position == data.Length, "Shader schema did not consume the complete object.");
                        if (Field(value, "m_ParsedForm/m_Name").AsString != shaderName)
                        {
                            continue;
                        }

                        Require(found == null, "Shader name is ambiguous in its bundle.");
                        found = ReadShader(value);
                    }
                }
            }

            Require(found != null, "Shader is absent from its installed bundle: " + shaderName);
            return found;
        }
        finally
        {
            manager.UnloadAll();
        }
    }

    private static CompiledShaderPrograms ReadShader(AssetTypeValueField shader)
    {
        var platforms = Array(shader, "platforms");
        Require(platforms.Count == 1 && platforms[0].AsInt == 4, "Shader requires an unsupported compiler platform.");
        Blob[] blobs = ReadProgramTable(DecompressPrograms(shader));
        var subshaders = Array(shader, "m_ParsedForm/m_SubShaders");
        Require(subshaders.Count == 1, "Shader requires subshader selection.");
        var passes = Array(subshaders[0], "m_Passes");
        Require(passes.Count > 0 && passes.Count <= 16, "Invalid shader pass count.");

        var result = new CompiledShaderPrograms
        {
            Name = Field(shader, "m_ParsedForm/m_Name").AsString,
            Passes = new Pass[passes.Count]
        };

        for (int passIndex = 0; passIndex < passes.Count; passIndex++)
        {
            AssetTypeValueField pass = passes[passIndex];
            ValidatePass(pass);
            var names = new Dictionary<int, string>();
            foreach (AssetTypeValueField pair in Array(pass, "m_NameIndices"))
            {
                int index = Field(pair, "second").AsInt;
                string name = Field(pair, "first").AsString;
                Require(index >= 0 && !names.ContainsKey(index) && !names.ContainsValue(name) && ValidName(name),
                    "Invalid shader name table.");
                names.Add(index, name);
            }

            var vertices = ReadStage(pass, "progVertex", 15, names, blobs);
            var fragments = ReadStage(pass, "progFragment", 17, names, blobs);
            Require(vertices.Count == fragments.Count && vertices.Count > 0,
                "Shader stages have different variant sets.");
            var variants = new List<Variant>();
            foreach (var entry in vertices)
            {
                Require(fragments.TryGetValue(entry.Key, out Program fragment), "Shader variant has no matching fragment program.");
                variants.Add(new Variant
                {
                    Keywords = blobs[entry.Value.BlobIndex].Keywords,
                    Vertex = entry.Value,
                    Fragment = fragment
                });
            }

            result.Passes[passIndex] = new Pass { Variants = variants.ToArray() };
        }

        return result;
    }

    private static Dictionary<string, Program> ReadStage(AssetTypeValueField pass, string stage, int type,
        Dictionary<int, string> names, Blob[] blobs)
    {
        var result = new Dictionary<string, Program>(StringComparer.Ordinal);
        var programs = Array(pass, stage + "/m_SubPrograms");
        Require(programs.Count > 0 && programs.Count <= 64, "Unsupported shader stage layout.");
        foreach (AssetTypeValueField program in programs)
        {
            int index = Field(program, "m_BlobIndex").AsInt;
            Require(index >= 0 && index < blobs.Length && Field(program, "m_GpuProgramType").AsInt == type &&
                blobs[index].Type == type, "Shader program index or stage is invalid.");
            string[] keywords = Array(program, "m_GlobalKeywordIndices")
                .Concat(Array(program, "m_LocalKeywordIndices"))
                .Select(value => ReadName(names, value.AsInt)).OrderBy(value => value, StringComparer.Ordinal).ToArray();
            Require(keywords.SequenceEqual(blobs[index].Keywords), "Shader program keywords disagree with its metadata.");

            string key = string.Join("\n", keywords);
            Require(!result.ContainsKey(key), "Shader variant is ambiguous.");
            var textures = new List<TextureBinding>();
            foreach (AssetTypeValueField texture in Array(program, "m_TextureParams"))
            {
                int slot = Field(texture, "m_Index").AsInt;
                int sampler = Field(texture, "m_SamplerIndex").AsInt;
                Require(slot >= 0 && slot < 128 && sampler >= 0 && sampler < 16 &&
                    !Field(texture, "m_MultiSampled").AsBool && Field(texture, "m_Dim").AsInt == 2 &&
                    textures.All(value => value.TextureSlot != slot), "Unsupported shader texture binding.");
                textures.Add(new TextureBinding
                {
                    Name = ReadName(names, Field(texture, "m_NameIndex").AsInt),
                    TextureSlot = slot,
                    SamplerSlot = sampler
                });
            }

            ConstantBuffer[] buffers = ReadBuffers(program, names);
            Require(textures.Count == blobs[index].TextureCount && buffers.Length == blobs[index].BufferCount &&
                textures.Select(value => value.SamplerSlot).Distinct().Count() == blobs[index].SamplerCount,
                "Shader resource counts disagree with its compiled header.");
            foreach (string field in new[] { "m_VectorParams", "m_MatrixParams", "m_BufferParams", "m_UAVParams", "m_Samplers" })
            {
                Require(Array(program, field).Count == 0, "Shader requires unsupported loose parameters or resources.");
            }

            result.Add(key, new Program
            {
                BlobIndex = index,
                Bytecode = blobs[index].Bytecode,
                Textures = textures.ToArray(),
                Buffers = buffers
            });
        }

        return result;
    }

    private static ConstantBuffer[] ReadBuffers(AssetTypeValueField program, Dictionary<int, string> names)
    {
        var bindings = new Dictionary<int, int>();
        foreach (AssetTypeValueField binding in Array(program, "m_ConstantBufferBindings"))
        {
            int name = Field(binding, "m_NameIndex").AsInt;
            int slot = Field(binding, "m_Index").AsInt;
            Require(slot >= 0 && slot < 14 && !bindings.ContainsKey(name) && !bindings.ContainsValue(slot),
                "Invalid shader constant buffer binding.");
            bindings.Add(name, slot);
        }

        var result = new List<ConstantBuffer>();
        foreach (AssetTypeValueField buffer in Array(program, "m_ConstantBuffers"))
        {
            int name = Field(buffer, "m_NameIndex").AsInt;
            int size = Field(buffer, "m_Size").AsInt;
            Require(bindings.TryGetValue(name, out int slot) && size > 0 && size <= 65536 && size % 16 == 0 &&
                result.All(value => value.Slot != slot) && Array(buffer, "m_StructParams").Count == 0,
                "Unsupported shader constant buffer.");
            var parameters = new List<Parameter>();
            foreach (string kind in new[] { "m_VectorParams", "m_MatrixParams" })
            {
                foreach (AssetTypeValueField parameter in Array(buffer, kind))
                {
                    int rows = kind == "m_MatrixParams" ? Field(parameter, "m_RowCount").AsInt : 1;
                    int columns = kind == "m_MatrixParams" ? 4 : Field(parameter, "m_Dim").AsInt;
                    int offset = Field(parameter, "m_Index").AsInt;
                    Require(rows > 0 && rows <= 4 && columns > 0 && columns <= 4 &&
                        Field(parameter, "m_Type").AsInt == 0 && Field(parameter, "m_ArraySize").AsInt == 0 &&
                        offset >= 0 && offset % 4 == 0 && offset <= size - rows * columns * 4,
                        "Shader constant parameter is outside its buffer.");
                    string parameterName = ReadName(names, Field(parameter, "m_NameIndex").AsInt);
                    Require(parameters.All(value => value.Name != parameterName &&
                        (offset >= value.Offset + value.Rows * value.Columns * 4 ||
                         value.Offset >= offset + rows * columns * 4)), "Shader constant parameters overlap.");
                    parameters.Add(new Parameter { Name = parameterName, Offset = offset, Rows = rows, Columns = columns });
                }
            }

            result.Add(new ConstantBuffer { Name = ReadName(names, name), Slot = slot, Size = size, Parameters = parameters.ToArray() });
        }

        Require(result.Count == bindings.Count, "Shader constant buffer has no definition.");
        return result.ToArray();
    }

    private static byte[] DecompressPrograms(AssetTypeValueField shader)
    {
        var offsets = Array(shader, "offsets");
        var lengths = Array(shader, "compressedLengths");
        var sizes = Array(shader, "decompressedLengths");
        Require(offsets.Count == 1 && lengths.Count == 1 && sizes.Count == 1,
            "Shader compression tables have different platform counts.");
        var platformOffsets = Array(offsets[0], "");
        var platformLengths = Array(lengths[0], "");
        var platformSizes = Array(sizes[0], "");
        Require(platformOffsets.Count == 1 && platformLengths.Count == 1 && platformSizes.Count == 1,
            "Shader requires multiple compiled segments.");
        byte[] compressed = Field(shader, "compressedBlob/Array").AsByteArray;
        Require(platformOffsets[0].AsUInt == 0 && platformLengths[0].AsUInt == compressed.Length &&
            compressed.Length > 0 && compressed.Length <= MaximumProgramBytes &&
            platformSizes[0].AsUInt > 0 && platformSizes[0].AsUInt <= MaximumProgramBytes,
            "Shader compression range is invalid.");
        int size = (int)platformSizes[0].AsUInt;
        byte[] result = new byte[size];
        try
        {
            Require(LZ4Codec.Decode32(compressed, 0, compressed.Length, result, 0, size, false) == size,
                "Shader decompression length is inconsistent.");
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException("Shader compressed data is invalid.", error);
        }
        return result;
    }

    private static Blob[] ReadProgramTable(byte[] bytes)
    {
        using (var reader = new BinaryReader(new MemoryStream(bytes, false)))
        {
            Require(bytes.Length >= 4 && bytes.Length <= MaximumProgramBytes, "Invalid shader program table length.");
            int count = reader.ReadInt32();
            Require(count > 0 && count <= 256 && 4 + count * 12 <= bytes.Length, "Invalid shader program count.");
            var result = new Blob[count];
            int expectedOffset = 4 + count * 12;
            for (int index = 0; index < count; index++)
            {
                reader.BaseStream.Position = 4 + index * 12;
                int offset = reader.ReadInt32();
                int length = reader.ReadInt32();
                int segment = reader.ReadInt32();
                Require(segment == 0 && offset == expectedOffset && length >= 36 && length % 4 == 0 &&
                    offset <= bytes.Length - length, "Shader program range is invalid.");
                result[index] = ReadBlob(bytes, offset, length);
                expectedOffset = checked(offset + length);
            }

            Require(expectedOffset == bytes.Length, "Shader program table leaves trailing bytes.");
            return result;
        }
    }

    private static Blob ReadBlob(byte[] bytes, int offset, int length)
    {
        using (var reader = new BinaryReader(new MemoryStream(bytes, offset, length, false)))
        {
            Require(reader.ReadInt32() == 201806140, "Unsupported compiled shader record version.");
            int type = reader.ReadInt32();
            Require(type == 15 || type == 17, "Shader is not a D3D11 shader model 4 vertex or fragment program.");
            reader.BaseStream.Position = 24;
            string[] keywords = ReadKeywords(reader).Concat(ReadKeywords(reader))
                .OrderBy(value => value, StringComparer.Ordinal).ToArray();
            Require(keywords.Distinct(StringComparer.Ordinal).Count() == keywords.Length, "Duplicate shader keywords.");
            int size = reader.ReadInt32();
            Require(size >= 70 && size <= reader.BaseStream.Length - reader.BaseStream.Position,
                "Compiled shader payload is truncated.");
            byte[] payload = reader.ReadBytes(size);
            Require(payload[0] == 2 && payload[1] <= 16 && payload[2] <= 14 && payload[3] <= 128 &&
                payload.Skip(4).Take(34).All(value => value == 0), "Unsupported D3D11 shader payload header.");
            var bytecode = new byte[size - 38];
            Buffer.BlockCopy(payload, 38, bytecode, 0, bytecode.Length);
            ValidateBytecode(bytecode, type);
            return new Blob
            {
                Type = type,
                Keywords = keywords,
                Bytecode = bytecode,
                BufferCount = payload[2],
                TextureCount = payload[3],
                SamplerCount = payload[1]
            };
        }
    }

    private static string[] ReadKeywords(BinaryReader reader)
    {
        int count = reader.ReadInt32();
        Require(count >= 0 && count <= 64, "Invalid compiled shader keyword count.");
        var result = new string[count];
        for (int index = 0; index < count; index++)
        {
            int length = reader.ReadInt32();
            Require(length > 0 && length <= 160 && length <= reader.BaseStream.Length - reader.BaseStream.Position,
                "Compiled shader keyword is truncated.");
            byte[] text = reader.ReadBytes(length);
            Require(text.All(value => value >= 33 && value <= 126), "Invalid compiled shader keyword.");
            result[index] = Encoding.ASCII.GetString(text);
            while (reader.BaseStream.Position % 4 != 0)
            {
                Require(reader.ReadByte() == 0, "Invalid compiled shader keyword padding.");
            }
        }

        return result;
    }

    private static void ValidateBytecode(byte[] bytes, int type)
    {
        Require(bytes.Length >= 32 && ReadUInt(bytes, 0) == 0x43425844 && ReadUInt(bytes, 20) == 1 &&
            ReadUInt(bytes, 24) == bytes.Length, "Invalid DXBC container header.");
        uint count = ReadUInt(bytes, 28);
        Require(count > 0 && count <= 32 && 32 + count * 4 <= bytes.Length, "Invalid DXBC chunk table.");
        var ranges = new List<Tuple<int, int>>();
        int programs = 0;
        for (int index = 0; index < count; index++)
        {
            int offset = checked((int)ReadUInt(bytes, 32 + index * 4));
            Require(offset >= 32 + count * 4 && offset <= bytes.Length - 8 && offset % 4 == 0,
                "DXBC chunk offset is invalid.");
            int size = checked((int)ReadUInt(bytes, offset + 4));
            Require(size >= 0 && size <= bytes.Length - offset - 8 &&
                ranges.All(value => offset >= value.Item2 || offset + size + 8 <= value.Item1),
                "DXBC chunks overlap or exceed the container.");
            ranges.Add(Tuple.Create(offset, offset + size + 8));
            if (ReadUInt(bytes, offset) == 0x52444853 || ReadUInt(bytes, offset) == 0x58454853)
            {
                Require(size >= 8 && size % 4 == 0 && ReadUInt(bytes, offset + 12) == size / 4,
                    "Invalid DXBC instruction stream length.");
                uint version = ReadUInt(bytes, offset + 8);
                Require(version == (type == 15 ? 0x10040u : 0x40u), "DXBC shader stage disagrees with its metadata.");
                programs++;
            }
        }

        Require(programs == 1, "DXBC must contain exactly one instruction stream.");
    }

    private static void ValidatePass(AssetTypeValueField pass)
    {
        Require(Field(pass, "m_Type").AsInt == 0 && Field(pass, "m_State/zTest/val").AsFloat == 8 &&
            Field(pass, "m_State/zWrite/val").AsFloat == 0 && Field(pass, "m_State/culling/val").AsFloat == 0 &&
            !Field(pass, "m_State/rtSeparateBlend").AsBool, "Shader pass requires unsupported raster state.");
        foreach (string name in new[] { "srcBlend", "srcBlendAlpha" })
        {
            Require(Field(pass, "m_State/rtBlend0/" + name + "/val").AsFloat == 1, "Shader pass requires source blending.");
        }

        foreach (string name in new[] { "destBlend", "destBlendAlpha", "blendOp", "blendOpAlpha" })
        {
            Require(Field(pass, "m_State/rtBlend0/" + name + "/val").AsFloat == 0, "Shader pass requires destination blending.");
        }

        Require(Field(pass, "m_State/rtBlend0/colMask/val").AsFloat == 15, "Shader pass does not write every color channel.");
        foreach (string stage in new[] { "progGeometry", "progHull", "progDomain" })
        {
            Require(Array(pass, stage + "/m_SubPrograms").Count == 0, "Shader pass requires additional stages.");
        }
    }

    private static void ValidateBundleHeader(byte[] bytes)
    {
        using (var reader = new AssetsFileReader(new MemoryStream(bytes, false)))
        {
            reader.BigEndian = true;
            Require(reader.ReadStringLength(8) == "UnityFS\0", "Shader bundle is not UnityFS.");
            uint version = reader.ReadUInt32();
            Require(version == 6 || version == 7 || version == 8, "Unsupported shader bundle version.");
            for (int index = 0; index < 2; index++)
            {
                int length = 0;
                while (reader.ReadByte() != 0)
                {
                    Require(++length <= 64, "Shader bundle version string is too long.");
                }
            }

            Require(reader.ReadInt64() == bytes.Length, "Shader bundle file size is inconsistent.");
            uint compressed = reader.ReadUInt32();
            uint decompressed = reader.ReadUInt32();
            Require(compressed > 0 && compressed <= bytes.Length && decompressed > 0 && decompressed <= 1024 * 1024,
                "Shader bundle block table is outside its limit.");
        }
    }

    private static uint ReadUInt(byte[] bytes, int offset)
    {
        Require(offset >= 0 && offset <= bytes.Length - 4, "Truncated shader integer.");
        return (uint)(bytes[offset] | bytes[offset + 1] << 8 | bytes[offset + 2] << 16 | bytes[offset + 3] << 24);
    }

    private static string ReadName(Dictionary<int, string> names, int index)
    {
        Require(names.TryGetValue(index, out string name), "Shader parameter name is absent from its table.");
        return name;
    }

    private static bool ValidName(string name) =>
        !string.IsNullOrEmpty(name) && name.Length <= 160 && name.All(value => value >= 33 && value <= 126);

    private static List<AssetTypeValueField> Array(AssetTypeValueField value, string path) =>
        Field(value, path.Length == 0 ? "Array" : path + "/Array").Children;

    private static AssetTypeValueField Field(AssetTypeValueField root, string path)
    {
        foreach (string part in path.Split('/'))
        {
            root = root[part];
            Require(root != null && !root.IsDummy, "Required shader field is absent: " + path);
        }

        return root;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidDataException(message);
        }
    }
}
