# Virtual Display Driver Runtime Shaders

The driver now loads runtime shaders from a shader directory instead of relying on generated C++ shader bytecode headers.

## Where the driver looks

- `C:\VirtualDisplayDriver\Shaders` by default
- `Shaders` next to `MttVDD.dll` as a fallback
- The build also copies the sample `.hlsl` files from this folder into the packaged output folder

## Supported files

- `.hlsl` files compiled at runtime with `D3DCompileFromFile()`
- `.cso` files loaded directly as precompiled pixel shaders

Both require `d3dcompiler_47.dll` to be present. The driver loads it on demand with
`LoadLibraryEx` rather than linking it statically, so a machine without it still gets a
working driver with the shader renderer simply unavailable - instead of failing to load at
all. Note that the built-in fullscreen vertex shader is compiled from source at runtime too,
so a `.cso` does not remove the dependency.

## Behaviour worth knowing

- **`shader_file` is never silently substituted.** If it is set and the file is missing, the
  driver logs the configured name and renders nothing. It does not fall back to whichever
  `.hlsl` happens to sort first. If `shader_file` is empty, the first supported file found in
  the search path is used.
- **Each swap chain owns its own shader state.** Textures and shader objects are per
  swap chain, not per adapter, so monitors with different resolutions on one GPU do not
  re-create each other's render targets every frame.
- **Unsupported surfaces turn the shader off.** The pipeline needs a single-mip,
  single-sample, single-element surface for the frame copy to round-trip. If the acquired
  surface has a different shape, the driver logs it once and stops retrying for that swap
  chain rather than handing back unfiltered frames.
- **Hot reload is polled once per second**, not once per frame.

## Required entry point

- `PSMain`
- If you prefer, compiled `.cso` shaders can skip source-level entry points entirely

## Available shader bindings

```hlsl
cbuffer PerFrameBuffer : register(b0)
{
    float4 resolution;
    float time;
    float param0;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);
```

- `resolution.xy` contains the current frame size
- `time` is seconds since the driver process started
- `param0` is the configurable runtime parameter from `shader_param0`
- The built-in fullscreen vertex shader supplies `SV_POSITION` and `TEXCOORD0`

## Driver settings

Add or update these settings in `vdd_settings.xml`:

```xml
<shaders>
    <shader_renderer>true</shader_renderer>
    <shader_hot_reload>true</shader_hot_reload>
    <shader_directory>Shaders</shader_directory>
    <shader_file>wave_shader.hlsl</shader_file>
    <shader_param0>1.0</shader_param0>
</shaders>
```

## Included examples

- `passthrough_shader.hlsl` keeps the source frame unchanged
- `wave_shader.hlsl` overlays an animated wave and bottom bar on the source frame
- `vortex_shader.hlsl` overlays a red/blue split tint on the source frame

## Optional offline compilation

```cmd
fxc.exe /T ps_5_0 /E PSMain /Fo wave_shader.cso wave_shader.hlsl
fxc.exe /T ps_5_0 /E PSMain /Fo vortex_shader.cso vortex_shader.hlsl
```

Use `.hlsl` or `.cso` files for runtime shaders. The old generated `*.h` shader blobs have been removed.
