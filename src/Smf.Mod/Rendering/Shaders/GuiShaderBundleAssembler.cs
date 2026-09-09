#nullable disable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using AssetsTools.NET;
using AssetsTools.NET.Extra;

namespace SimplyMoreFPS.Rendering.Shaders;

public sealed class BundleOptions
{
    public string BundleName = "smf-gui-alpha";
    public string GuiNamePrefix = "Hidden/SMF/GuiAlpha/";
    public bool IncludePremultipliedCopy;
    public string CopyShaderName = "Hidden/SMF/GuiCopy/BlitCopy";
}

public sealed class ShaderResult
{
    public long SourcePathId;
    public string OriginalName;
    public string ReplacementName;
    public string AssetName;
    public string SourceObjectSha256;
    public string OutputObjectSha256;
    public string CompiledBlobSha256;
    public string[] ChangedFields;
    public int[] ShaderPlatforms;
    public bool UntouchedRoundtripExact;
    public bool InverseEditsRestoreSourceExact;
}

public sealed class BundleResult
{
    public byte[] Bytes;
    public string SourceSha256;
    public string BundleSha256;
    public string CabName;
    public uint TargetPlatform;
    public ShaderResult[] Shaders;
}

// Builds a validated AssetBundle out of the user's own unity_builtin_extra bytes. No Unity API, files or network.
public static class GuiShaderBundleAssembler
{
    public const string UnityVersion = "2022.3.35f1";
    public const string ShaderTypeHash = "EA424A5FC1E48DB62144DF956F9E1005";
    public const string IndexTypeHash = "97DA5F4688E45A57C8B42D4F42497297";
    public const string ShaderSchemaSha256 = "86751B5763AD8221AEEADBA3EF1703B789CD7A9631EC6CDBA34C07F97E33FFDF";
    public const string IndexSchemaSha256 = "2DFFB8F817A04511817851D1CA8748F90E9EBE0CF0FAB24F004C8CC2D3E9D5A0";
    private const string BlendStatePath = "m_ParsedForm/m_SubShaders/Array/0/m_Passes/Array/0/m_State/rtBlend0/";

    // Inputs are never modified. Any validation failure throws before a bundle is returned.
    public static BundleResult Build(byte[] builtinExtra, byte[] shaderSchemaJson,
        byte[] indexSchemaJson, BundleOptions options = null)
    {
        if (options == null) options = new BundleOptions();

        ValidateName(options.BundleName, "bundle name");
        ValidateName(options.GuiNamePrefix, "GUI name prefix");
        ValidateName(options.CopyShaderName, "copy shader name");
        Require(
            options.GuiNamePrefix.StartsWith("Hidden/", StringComparison.Ordinal) &&
            options.GuiNamePrefix.EndsWith("/", StringComparison.Ordinal),
            "Invalid GUI shader prefix.");
        Require(
            builtinExtra != null &&
            builtinExtra.Length >= 80 &&
            builtinExtra.Length <= 16 * 1024 * 1024,
            "Unexpected builtinextra length.");

        AssetTypeTemplateField shaderTemplate = ReadSchema(shaderSchemaJson, 48, ShaderTypeHash, ShaderSchemaSha256);
        AssetTypeTemplateField indexTemplate = ReadSchema(indexSchemaJson, 142, IndexTypeHash, IndexSchemaSha256);

        byte[] input = (byte[])builtinExtra.Clone();
        Preflight(input);

        var requests = new List<Request>
        {
            new Request(9000, "Hidden/Internal-GUITextureClip", options.GuiNamePrefix + "GUITextureClip", "guitextureclip", false),
            new Request(9002, "Hidden/Internal-GUITexture", options.GuiNamePrefix + "GUITexture", "guitexture", false),
            new Request(9003, "Hidden/Internal-GUITextureBlit", options.GuiNamePrefix + "GUITextureBlit", "guitextureblit", false)
        };
        if (options.IncludePremultipliedCopy)
        {
            requests.Add(new Request(66, "Hidden/BlitCopy", options.CopyShaderName, "blitcopy", true));
        }

        Require(requests.Select(r => r.NewName).Distinct(StringComparer.Ordinal).Count() == requests.Count,
            "Replacement shader names must be distinct.");

        using (var sourceReader = new AssetsFileReader(new MemoryStream(input, false)))
        {
            var source = new AssetsFile();
            source.Read(sourceReader);
            ValidateSource(source, input.Length);

            var rawObjects = new Dictionary<long, byte[]>();
            var reports = new List<ShaderResult>();

            foreach (Request request in requests)
            {
                AssetFileInfo info = source.Metadata.GetAssetInfo(request.PathId);
                Require(info != null && info.TypeIdOrIndex == 0, "Expected shader object is absent.");

                byte[] original = ReadObject(source, info);
                AssetTypeValueField value = ParseObject(shaderTemplate, original);

                RequireBytes(original, value.WriteToByteArray(), "Untouched shader roundtrip changed bytes.");
                Require(Field(value, "m_ParsedForm/m_Name").AsString == request.OldName, "Shader name mismatch.");
                Require(
                    Field(value, "m_Dependencies/Array").Children.Count == 0 &&
                    Field(value, "m_NonModifiableTextures/Array").Children.Count == 0 &&
                    Field(value, "m_ParsedForm/m_Dependencies/Array").Children.Count == 0 &&
                    Field(value, "m_ParsedForm/m_FallbackName").AsString.Length == 0,
                    "Shader requires external objects or textures.");
                ValidatePrograms(source.Metadata.TargetPlatform, value);
                ValidateBlend(value, request);

                // Apply the edits, then undo them on a fresh parse: that must give back the source bytes exactly.
                ApplyEdits(value, request, false);
                byte[] changed = value.WriteToByteArray();
                AssetTypeValueField reparsed = ParseObject(shaderTemplate, changed);
                ApplyEdits(reparsed, request, true);
                RequireBytes(original, reparsed.WriteToByteArray(), "Shader changed outside its declared edits.");
                rawObjects.Add(request.PathId, changed);

                reports.Add(new ShaderResult
                {
                    SourcePathId = request.PathId,
                    OriginalName = request.OldName,
                    ReplacementName = request.NewName,
                    AssetName = request.AssetName,
                    SourceObjectSha256 = Digest(original),
                    OutputObjectSha256 = Digest(changed),
                    CompiledBlobSha256 = Digest(Field(value, "compressedBlob/Array").AsByteArray),
                    ShaderPlatforms = Field(value, "platforms/Array").Children.Select(x => x.AsInt).ToArray(),
                    ChangedFields = request.IsCopy
                        ? new[] { "m_ParsedForm/m_Name", BlendStatePath + "destBlend/val", BlendStatePath + "destBlendAlpha/val" }
                        : new[] { "m_ParsedForm/m_Name", BlendStatePath + "destBlendAlpha/val" },
                    UntouchedRoundtripExact = true,
                    InverseEditsRestoreSourceExact = true
                });
            }

            byte[] indexBytes = BuildIndex(indexTemplate, requests, options.BundleName);
            RequireBytes(indexBytes, ParseObject(indexTemplate, indexBytes).WriteToByteArray(), "Index schema roundtrip failed.");

            AssetsFile assets = NewAssetsFile(source.Metadata.TargetPlatform, source.Metadata.TypeTreeTypes[0]);
            foreach (Request request in requests)
            {
                AddObject(assets, request.PathId, 48, rawObjects[request.PathId]);
            }

            AddObject(assets, 1, 142, indexBytes);

            byte[] assetsBytes;
            using (var stream = new MemoryStream())
            using (var writer = new AssetsFileWriter(stream))
            {
                assets.Write(writer);
                assetsBytes = stream.ToArray();
            }

            ValidateOutputAssets(assetsBytes, requests, rawObjects, source.Metadata.TypeTreeTypes[0], indexBytes);

            string cab = "CAB-" + Digest(assetsBytes).Substring(0, 32).ToLowerInvariant();
            byte[] packed = PackBundle(assetsBytes, cab);
            ValidateContainer(packed, assetsBytes, cab);

            return new BundleResult
            {
                Bytes = packed,
                SourceSha256 = Digest(input),
                BundleSha256 = Digest(packed),
                CabName = cab,
                TargetPlatform = source.Metadata.TargetPlatform,
                Shaders = reports.ToArray()
            };
        }
    }

    // Unity BlendMode values: 0 Zero, 1 One, 5 SrcAlpha, 10 OneMinusSrcAlpha.
    private static void ValidateBlend(AssetTypeValueField value, Request request)
    {
        Require(Field(value, BlendStatePath + "srcBlendAlpha/val").AsFloat == 1f, "Unexpected source alpha blend.");
        Require(Field(value, BlendStatePath + "destBlendAlpha/val").AsFloat == (request.IsCopy ? 0f : 1f),
            "Unexpected destination alpha blend.");

        if (request.IsCopy)
        {
            Require(
                Field(value, BlendStatePath + "srcBlend/val").AsFloat == 1f &&
                Field(value, BlendStatePath + "destBlend/val").AsFloat == 0f,
                "Unexpected standalone copy RGB blend.");
        }
        else
        {
            Require(
                Field(value, BlendStatePath + "srcBlend/val").AsFloat == 5f &&
                Field(value, BlendStatePath + "destBlend/val").AsFloat == 10f,
                "Unexpected GUI RGB blend.");
        }
    }

    private static void ApplyEdits(AssetTypeValueField value, Request request, bool inverse)
    {
        Field(value, "m_ParsedForm/m_Name").AsString = inverse ? request.OldName : request.NewName;
        Field(value, BlendStatePath + "destBlendAlpha/val").AsFloat = inverse ? (request.IsCopy ? 0f : 1f) : 10f;
        if (request.IsCopy)
        {
            Field(value, BlendStatePath + "destBlend/val").AsFloat = inverse ? 0f : 10f;
        }
    }

    // Targets 19, 24 and 2 are Windows, Linux and macOS; their programs are d3d11 (4), glcore/vulkan (15, 18) and metal (14).
    private static void ValidatePrograms(uint target, AssetTypeValueField value)
    {
        int[] platforms = Field(value, "platforms/Array").Children.Select(x => x.AsInt).ToArray();
        int[] expected = target == 19 ? new[] { 4 } : target == 24 ? new[] { 15, 18 } : new[] { 14 };

        Require(platforms.SequenceEqual(expected), "Unrecognized installed shader platform set.");
        Require(Field(value, "compressedBlob/Array").AsByteArray.Length > 0, "Shader has no compiled programs.");
    }

    private static byte[] BuildIndex(AssetTypeTemplateField template, List<Request> requests, string name)
    {
        AssetTypeValueField index = ValueBuilder.DefaultValueFieldFromTemplate(template);
        index["m_Name"].AsString = name;
        index["m_AssetBundleName"].AsString = name;
        index["m_RuntimeCompatibility"].AsUInt = 1;
        index["m_PathFlags"].AsInt = 7;

        AssetTypeValueField preload = Field(index, "m_PreloadTable/Array");
        AssetTypeValueField container = Field(index, "m_Container/Array");
        for (int i = 0; i < requests.Count; i++)
        {
            AssetTypeValueField pointer = ValueBuilder.DefaultValueFieldFromArrayTemplate(preload);
            pointer["m_FileID"].AsInt = 0;
            pointer["m_PathID"].AsLong = requests[i].PathId;
            preload.Children.Add(pointer);

            AssetTypeValueField entry = ValueBuilder.DefaultValueFieldFromArrayTemplate(container);
            entry["first"].AsString = requests[i].AssetName;
            entry["second"]["preloadIndex"].AsInt = i;
            entry["second"]["preloadSize"].AsInt = 1;
            entry["second"]["asset"]["m_FileID"].AsInt = 0;
            entry["second"]["asset"]["m_PathID"].AsLong = requests[i].PathId;
            container.Children.Add(entry);
        }

        // Everything else stays at the schema default: no dependencies, main asset or scene hashes.
        return index.WriteToByteArray();
    }

    private static AssetsFile NewAssetsFile(uint target, TypeTreeType shaderType)
    {
        var indexType = new TypeTreeType
        {
            TypeId = 142,
            IsStrippedType = false,
            ScriptTypeIndex = 0xffff,
            TypeHash = new Hash128(FromHex(IndexTypeHash)),
            ScriptIdHash = Hash128.NewBlankHash(),
            ExtTypeHash = Hash128.NewBlankHash(),
            TypeBlobIsDefinition = true,
            TypeBlob = new TypeTreeBlob { Nodes = new List<TypeTreeNode>(), StringBufferBytes = new byte[0] },
            TypeDependencies = new int[0],
            IsRefType = false
        };

        return new AssetsFile
        {
            Header = new AssetsFileHeader { Version = 22, Endianness = false },
            Metadata = new AssetsFileMetadata
            {
                UnityVersion = UnityVersion,
                TargetPlatform = target,
                TypeTreeEnabled = false,
                TypeTreeTypes = new List<TypeTreeType> { shaderType, indexType },
                AssetInfos = new List<AssetFileInfo>(),
                ScriptTypes = new List<AssetPPtr>(),
                Externals = new List<AssetsFileExternal>(),
                RefTypes = new List<TypeTreeType>(),
                UserInformation = ""
            }
        };
    }

    private static void AddObject(AssetsFile file, long id, int type, byte[] bytes)
    {
        AssetFileInfo info = AssetFileInfo.Create(file, id, type, 0xffff);
        Require(info != null, "Cannot construct asset descriptor.");

        info.SetNewData(bytes);
        file.Metadata.AddAssetInfo(info);
    }

    private static byte[] PackBundle(byte[] assets, string cab)
    {
        AssetBundleDirectoryInfo directory = AssetBundleDirectoryInfo.Create(cab, true);
        directory.SetNewData(assets);

        var bundle = new AssetBundleFile
        {
            Header = new AssetBundleHeader
            {
                Signature = "UnityFS",
                Version = 8,
                GenerationVersion = "5.x.x",
                EngineVersion = UnityVersion,
                // 0x40: directory info present, no compression.
                FileStreamHeader = new AssetBundleFSHeader { Flags = (AssetBundleFSHeaderFlags)0x40 }
            },
            BlockAndDirInfo = new AssetBundleBlockAndDirInfo
            {
                Hash = Hash128.NewBlankHash(),
                BlockInfos = new AssetBundleBlockInfo[0],
                DirectoryInfos = new List<AssetBundleDirectoryInfo> { directory }
            }
        };

        using (var stream = new MemoryStream())
        using (var writer = new AssetsFileWriter(stream))
        {
            bundle.Write(writer);
            return stream.ToArray();
        }
    }

    private static void ValidateOutputAssets(byte[] bytes, List<Request> requests, Dictionary<long, byte[]> raw,
        TypeTreeType originalType, byte[] indexBytes)
    {
        using (var reader = new AssetsFileReader(new MemoryStream(bytes, false)))
        {
            var file = new AssetsFile();
            file.Read(reader);

            Require(
                file.AssetInfos.Count == requests.Count + 1 &&
                file.Metadata.Externals.Count == 0 &&
                file.Metadata.ScriptTypes.Count == 0 &&
                file.Metadata.RefTypes.Count == 0,
                "Unexpected output dependencies.");
            RequireBytes(TypeDescriptor(originalType), TypeDescriptor(file.Metadata.TypeTreeTypes[0]),
                "Shader serialized type descriptor changed.");

            foreach (Request request in requests)
            {
                RequireBytes(raw[request.PathId], ReadObject(file, file.Metadata.GetAssetInfo(request.PathId)),
                    "Written shader object differs from validated bytes.");
            }

            RequireBytes(indexBytes, ReadObject(file, file.Metadata.GetAssetInfo(1)), "Written index changed.");
        }
    }

    private static void ValidateContainer(byte[] packed, byte[] assets, string cab)
    {
        using (var reader = new AssetsFileReader(new MemoryStream(packed, false)))
        {
            var bundle = new AssetBundleFile();
            bundle.Read(reader);

            Require(
                bundle.Header.Signature == "UnityFS" &&
                bundle.Header.Version == 8 &&
                bundle.BlockAndDirInfo.DirectoryInfos.Count == 1 &&
                !bundle.DataIsCompressed,
                "Invalid generated UnityFS container.");

            AssetBundleDirectoryInfo entry = bundle.BlockAndDirInfo.DirectoryInfos[0];
            Require(
                entry.Name == cab &&
                entry.IsSerialized &&
                entry.DecompressedSize == assets.Length,
                "Invalid generated CAB index.");

            bundle.DataReader.Position = entry.Offset;
            RequireBytes(assets, bundle.DataReader.ReadBytes(assets.Length), "Container data changed.");
        }
    }

    private static byte[] TypeDescriptor(TypeTreeType type)
    {
        using (var stream = new MemoryStream())
        using (var writer = new AssetsFileWriter(stream))
        {
            type.Write(writer, 22, false);
            return stream.ToArray();
        }
    }

    private static void ValidateSource(AssetsFile source, int length)
    {
        AssetsFileMetadata metadata = source.Metadata;

        Require(
            source.Header.Version == 22 &&
            !source.Header.Endianness &&
            source.Header.FileSize == length &&
            metadata.UnityVersion == UnityVersion &&
            !metadata.TypeTreeEnabled &&
            metadata.TypeTreeTypes.Count == 1,
            "Unsupported installed Unity asset format.");
        Require(metadata.TargetPlatform == 19 || metadata.TargetPlatform == 24 || metadata.TargetPlatform == 2,
            "Unsupported target platform.");

        TypeTreeType type = metadata.TypeTreeTypes[0];

        Require(
            type.TypeId == 48 &&
            ToHex(type.TypeHash.data) == ShaderTypeHash &&
            !type.IsStrippedType &&
            type.ScriptTypeIndex == 0xffff,
            "Unknown Shader serialization type.");
        Require(
            metadata.Externals.Count == 0 &&
            metadata.ScriptTypes.Count == 0 &&
            metadata.RefTypes.Count == 0,
            "Builtin shader file has external dependencies.");
        Require(metadata.AssetInfos.Count > 0 && metadata.AssetInfos.Count < 4096, "Unexpected shader object count.");

        var ids = new HashSet<long>();

        foreach (AssetFileInfo info in metadata.AssetInfos)
        {
            Require(
                ids.Add(info.PathId) &&
                info.TypeIdOrIndex == 0 &&
                info.ByteOffset >= 0 &&
                info.ByteSize > 0 &&
                info.GetAbsoluteByteOffset(source) >= source.Header.DataOffset &&
                info.GetAbsoluteByteOffset(source) <= length - (long)info.ByteSize,
                "Invalid shader object bounds.");
        }
    }

    // Walks the raw header and type table before AssetsFile parses the whole file.
    private static void Preflight(byte[] input)
    {
        using (var reader = new AssetsFileReader(new MemoryStream(input, false)))
        {
            var header = new AssetsFileHeader();
            header.Read(reader);

            Require(
                header.Version == 22 &&
                !header.Endianness &&
                header.FileSize == input.Length &&
                header.MetadataSize > 0 &&
                header.MetadataSize < input.Length &&
                header.DataOffset >= 48 &&
                header.DataOffset <= input.Length,
                "Unsupported or damaged serialized header.");

            reader.BigEndian = false;
            Require(reader.ReadStringLength(UnityVersion.Length + 1) == UnityVersion + "\0", "Unsupported Unity version.");
            uint target = reader.ReadUInt32();
            Require(target == 19 || target == 24 || target == 2, "Unsupported serialized target.");
            Require(
                reader.ReadByte() == 0 &&
                reader.ReadInt32() == 1 &&
                reader.ReadInt32() == 48,
                "Unexpected stripped type layout.");
            Require(
                reader.ReadByte() == 0 &&
                reader.ReadUInt16() == 0xffff &&
                ToHex(reader.ReadBytes(16)) == ShaderTypeHash,
                "Unknown Shader type hash.");

            // Object table entries are 24 bytes each in format 22.
            int count = reader.ReadInt32();
            Require(count > 0 && count < 4096 && reader.Position + 24L * count + 12 <= header.DataOffset,
                "Invalid serialized object table.");

            reader.Align();
            reader.Position += 24L * count;
            Require(
                reader.ReadInt32() == 0 &&
                reader.ReadInt32() == 0 &&
                reader.ReadInt32() == 0,
                "Unsupported scripts, externals, or reference types.");
        }
    }

    private static byte[] ReadObject(AssetsFile file, AssetFileInfo info)
    {
        Require(info != null, "Missing output object.");

        file.Reader.Position = info.GetAbsoluteByteOffset(file);
        byte[] result = file.Reader.ReadBytes(checked((int)info.ByteSize));
        Require(result.Length == info.ByteSize, "Truncated object.");
        return result;
    }

    private static AssetTypeValueField ParseObject(AssetTypeTemplateField template, byte[] bytes)
    {
        using (var reader = new AssetsFileReader(new MemoryStream(bytes, false)))
        {
            AssetTypeValueField value = template.MakeValue(reader);
            Require(reader.Position == bytes.Length, "Matched schema did not consume the exact object.");
            return value;
        }
    }

    private static AssetTypeValueField Field(AssetTypeValueField root, string path)
    {
        AssetTypeValueField value = root;
        foreach (string part in path.Split('/'))
        {
            value = int.TryParse(part, out int index) ? value[index] : value[part];
            Require(value != null && !value.IsDummy, "Matched shader field is absent: " + path);
        }
        return value;
    }

    private static AssetTypeTemplateField ReadSchema(byte[] json, int id, string typeHash, string digest)
    {
        Require(json != null && json.Length <= 256 * 1024 && Digest(json) == digest, "Unknown matched schema bytes.");

        Schema schema;
        using (var stream = new MemoryStream(json, false))
        {
            schema = (Schema)new DataContractJsonSerializer(typeof(Schema)).ReadObject(stream);
        }

        Require(
            schema.Format == 1 &&
            schema.ClassId == id &&
            schema.TypeHash == typeHash &&
            schema.UnityVersion == UnityVersion &&
            schema.SerializedVersion == 22,
            "Mismatched schema descriptor.");
        return ToTemplate(schema.Root);
    }

    private static AssetTypeTemplateField ToTemplate(Node node)
    {
        AssetValueType valueType = AssetTypeValueField.GetValueTypeByTypeName(node.Type);

        var field = new AssetTypeTemplateField
        {
            Name = node.Name,
            Type = node.Type,
            Version = checked((ushort)node.Version),
            IsArray = (node.Flags & 1) != 0,
            IsAligned = (node.Meta & 0x4000) != 0,
            ValueType = valueType,
            HasValue = valueType != AssetValueType.None,
            Children = node.Children.Select(ToTemplate).ToList()
        };

        if (field.IsArray)
        {
            field.ValueType = field.Children[1].ValueType == AssetValueType.UInt8 ? AssetValueType.ByteArray : AssetValueType.Array;
        }
        return field;
    }

    private static void ValidateName(string name, string what)
    {
        if (string.IsNullOrWhiteSpace(name) || name.Length > 160 || name.Any(c => c < 32 || c > 126 || c == '\\'))
        {
            throw new ArgumentException("Invalid " + what + ".");
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException(message);
    }

    private static void RequireBytes(byte[] a, byte[] b, string message)
    {
        Require(a.SequenceEqual(b), message);
    }

    public static string Digest(byte[] bytes)
    {
        using (var hash = SHA256.Create())
        {
            return ToHex(hash.ComputeHash(bytes));
        }
    }

    private static string ToHex(byte[] bytes)
    {
        return BitConverter.ToString(bytes).Replace("-", "");
    }

    private static byte[] FromHex(string value)
    {
        return Enumerable.Range(0, value.Length / 2)
            .Select(i => Convert.ToByte(value.Substring(i * 2, 2), 16))
            .ToArray();
    }

    private sealed class Request
    {
        public readonly long PathId;
        public readonly string OldName;
        public readonly string NewName;
        public readonly string AssetName;

        // The BlitCopy shader also gets its RGB destination blend changed, not just alpha.
        public readonly bool IsCopy;

        public Request(long pathId, string oldName, string newName, string key, bool isCopy)
        {
            PathId = pathId;
            OldName = oldName;
            NewName = newName;
            AssetName = "assets/smf-gui/" + key + ".shader";
            IsCopy = isCopy;
        }
    }

    [DataContract]
    private sealed class Schema
    {
        [DataMember] public int Format { get; set; }
        [DataMember] public string UnityVersion { get; set; }
        [DataMember] public int SerializedVersion { get; set; }
        [DataMember] public int ClassId { get; set; }
        [DataMember] public string TypeHash { get; set; }
        [DataMember] public Node Root { get; set; }
    }

    [DataContract]
    private sealed class Node
    {
        [DataMember(Name = "n")] public string Name { get; set; }
        [DataMember(Name = "t")] public string Type { get; set; }
        [DataMember(Name = "v")] public int Version { get; set; }
        [DataMember(Name = "f")] public int Flags { get; set; }
        [DataMember(Name = "m")] public int Meta { get; set; }
        [DataMember(Name = "c")] public Node[] Children { get; set; }
    }
}
