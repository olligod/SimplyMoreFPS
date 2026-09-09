using System;
using System.Collections.Generic;

namespace SimplyMoreFPS.Rendering.Mac;

// Pure path arithmetic on canonical POSIX paths (from realpath). The result
// is used verbatim as a DllImport name, so it is never URL-encoded.
public static class RelativeBundleRoute
{
    public static string Compute(string pluginDirectory, string bundleDirectory)
    {
        string[] from = SplitPath(pluginDirectory);
        string[] to = SplitPath(bundleDirectory);
        if (to.Length == 0) throw new ArgumentException("An exact named .bundle directory is required.");
        string last = to[to.Length - 1];
        if (!last.EndsWith(".bundle", StringComparison.Ordinal) || last.Length == 7)
            throw new ArgumentException("An exact named .bundle directory is required.");

        int common = 0;
        while (common < from.Length && common < to.Length && from[common] == to[common])
        {
            ++common;
        }

        var route = new List<string>();
        for (int i = common; i < from.Length; ++i)
        {
            route.Add("..");
        }

        for (int i = common; i < to.Length; ++i)
        {
            route.Add(to[i]);
        }

        if (route.Count == 0) throw new ArgumentException("Plugin root is not a bundle.");

        string name = string.Join("/", route.ToArray());
        return name.Substring(0, name.Length - 7);
    }

    private static string[] SplitPath(string value)
    {
        if (string.IsNullOrEmpty(value)
            || value[0] != '/'
            || value.IndexOf('\0') >= 0
            || value.IndexOf('\\') >= 0
            || (value.Length > 1 && value.EndsWith("/", StringComparison.Ordinal)))
            throw new ArgumentException("A canonical absolute POSIX path is required.");
        if (value == "/") return new string[0];

        string[] parts = value.Substring(1).Split('/');
        foreach (string part in parts)
        {
            if (part.Length == 0 || part == "." || part == "..")
                throw new ArgumentException("Input path is not canonical.");
        }

        return parts;
    }
}
