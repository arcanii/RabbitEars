// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase-1 theme-engine spike shader: the transport-strip "underglow".
//
// A fullscreen-triangle vertex shader (no vertex buffer — positions are derived
// from SV_VertexID) plus a pixel shader that fills the strip with the window
// background colour and adds an animated coral glow rising from the bottom edge.
// This proves the whole GPU path end-to-end: D3D11 device + flip-model swapchain,
// offline fxc -> .cso -> embedded bytecode, a per-frame constant buffer, and (in
// the C++ that drives it) Direct2D drawing onto the SAME back-buffer afterwards.
//
// Compiled offline by fxc into two .cso blobs (VSMain @ vs_4_0, PSMain @ ps_4_0)
// which the build embeds as C byte arrays — see Win32/ui/skin/bin2h.cmake. Kept
// at Shader Model 4 so it runs on the D3D_FEATURE_LEVEL_10_x fallback path too.

cbuffer Constants : register(b0)
{
    float2 uResolution;  // strip size in pixels
    float  uTime;        // seconds since the surface was created
    float  uIntensity;   // 0..1 master strength (lets the effect fade in/out)
    float4 uBgColor;     // window background (straight RGBA, matches the flat strip today)
    float4 uAccent;      // coral accent driving the glow
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;   // (0,0) top-left .. (1,1) bottom-right
};

// Fullscreen triangle: vertex ids 0,1,2 -> uv (0,0),(2,0),(0,2), which covers the
// [0,1]^2 viewport with a single oversized triangle (cheaper than a quad, no VB).
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);        // 0,0 / 2,0 / 0,2
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 col = uBgColor.rgb;                          // parity: flat window background

    // A soft coral glow confined to the bottom ~22% of the strip — a subtle underline that
    // reads behind the transport controls, not a band filling the empty middle.
    float fromBottom = i.uv.y;                          // 0 top .. 1 bottom
    float band       = smoothstep(0.78, 1.0, fromBottom);
    float shimmer    = 0.65 + 0.35 * sin(i.uv.x * 6.28318 * 1.2 + uTime * 0.9);
    float glow       = band * shimmer * saturate(uIntensity);

    col += uAccent.rgb * glow * 0.38;                   // gentle additive coral
    return float4(saturate(col), 1.0);
}

// Edge glow (Phase 4b-2): a per-skin neon "tube" for the dock gutters — the thin
// dividers between the nav / video / grid panels. The bar's orientation is inferred
// from the texture aspect (a vertical gutter is taller than wide), and the glow is a
// smooth bloom across the SHORT axis, brightest down the centreline and fading to the
// window background at the bar's edges so it seats into the panels. Static (no uTime):
// the gutters render on WM_PAINT, not the animation tick, so there is nothing to move.
float4 PSEdge(VSOut i) : SV_Target
{
    float across = (uResolution.x < uResolution.y) ? i.uv.x : i.uv.y;  // coord across the thin axis
    float d      = abs(across - 0.5) * 2.0;                            // 0 centreline .. 1 edge
    float glow   = (1.0 - smoothstep(0.0, 1.0, d)) * saturate(uIntensity);

    float3 col = lerp(uBgColor.rgb, uAccent.rgb, glow);  // bg at the edges -> accent at the core
    col += uAccent.rgb * pow(glow, 3.0) * 0.35;          // a brighter neon core down the middle
    return float4(saturate(col), 1.0);
}
