using System;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

internal interface IMapImageEffect : IDisposable
{
    MonoBehaviour Source { get; }

    bool Prepare();
    void Validate();
    void Render(Camera projection, RenderTexture input, RenderTexture output);
}
