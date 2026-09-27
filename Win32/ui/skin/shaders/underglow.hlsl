// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transport strip's shader: its surface in the skin's material (photoreal stage C — brushed metal, a brass
// rail, black glass with a neon tube; a Flat skin keeps the plain window colour) and the animated "underglow".
//
// A fullscreen-triangle vertex shader (no vertex buffer — positions are derived
// from SV_VertexID) plus a pixel shader that fills the strip with the window
// background colour in the skin's material and adds an animated coral glow rising from the bottom edge.
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
    float4 uParams;      // effect params: x = heatHaze (0..1); y = the skin's material (common/ui/Skin.h
                         // SkinMaterial: 0 Flat, 1 Anodised, 2 Satin, 3 Brass, 4 NeonGlass); z = dpi / 96; w reserved
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

// ---- materials (photoreal stage C) --------------------------------------------------------------------
// Stable integer hashes (PCG) — the same pattern every frame (no uTime), so a material never crawls, and the same
// on every GPU (a sin() hash is outside D3D's sin accuracy range at these arguments, so it varies by driver).
uint pcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float hashU(uint v) { return float(pcg(v) >> 8) / 16777215.0; }  // 0..1, 24 bits
float hash21(float2 p) { return hashU(pcg(uint(p.x)) ^ uint(p.y) * 2654435761u); }  // p: non-negative cell
float vnoise(float2 p)
{
    float2 c = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(c), hash21(c + float2(1, 0)), u.x), lerp(hash21(c + float2(0, 1)), hash21(c + 1.0), u.x), u.y);
}

// The strip's surface in the skin's material, over its flat colour `col` (px: pixel coords; s: dpi / 96). The
// key light is above-left, as for every drawn thing (Win32/docs/PHOTOREAL.md). Only the strip's top and bottom
// bands and the space round the meters show: the transport controls sit over the rest.
float3 applyMaterial(float3 col, float2 px, float m, float s)
{
    if (m < 0.5 || m > 4.5) return col;                        // Flat (or unknown): exactly the old strip
    if (m < 2.5) {
        // Anodised (1) / Satin (2): brushed aluminium — fine streaks along the strip, each row its own, the grain
        // shifting every ~220 dp — and a bevelled edge under the top hairline: lit on the dark metal; on the light
        // one (a white strip, where added light is lost) the bevel's shaded side instead, a pixel further down,
        // and the streaks only ever darken.
        // The grain's segments blend into each other (no seam where one ends), and each streak fades in and out
        // along its length.
        bool satin = m > 1.5;
        float row = floor(px.y);
        float g = px.x / (220.0 * s), gi = floor(g), gf = frac(g);
        gf = gf * gf * (3.0 - 2.0 * gf);
        float streak = lerp(hash21(float2(gi, row)), hash21(float2(gi + 1.0, row)), gf);
        streak *= 0.55 + 0.45 * vnoise(float2(px.x / (40.0 * s), row));
        col += satin ? -streak * 0.030 : (streak - 0.3) * 0.024;
        float edge = 1.0 - saturate((px.y - 1.0) / max(1.0, 1.5 * s));
        if (satin) {
            float shade = saturate(1.0 - abs(px.y - (1.5 + 1.5 * s)) / max(1.0, 0.8 * s));
            col -= shade * 0.06;
        } else {
            col += edge * 0.07;
        }
        return col;
    }
    if (m < 3.5) {
        // Brass (3): a cast-iron plate (a faint mottle) under a polished brass rail along the top edge — a rod
        // lit from above, riveted every 64 dp — which also marks the strip's drag edge.
        col += (vnoise(px / (7.0 * s)) - 0.5) * 0.022;
        float railH = 5.0 * s;
        float t = (px.y - 1.0) / railH;                        // 0..1 across the rail
        if (t >= 0.0 && t <= 1.0) {
            float lit = saturate(1.0 - t * 1.25);
            float3 brass = lerp(float3(0.34, 0.22, 0.08), float3(0.90, 0.72, 0.40), lit);
            brass += pow(saturate(1.0 - abs(t - 0.28) * 4.0), 3.0) * 0.16;  // the rod's highlight
            float pitch = 64.0 * s;
            float2 d = (px - float2((floor(px.x / pitch) + 0.5) * pitch, 1.0 + railH * 0.5)) / (railH * 0.45);
            float rr = dot(d, d);
            if (rr < 1.0) {
                float lam = saturate((-d.x - d.y) * 0.55 + sqrt(1.0 - rr) * 0.63);
                brass = lerp(float3(0.28, 0.18, 0.06), float3(0.86, 0.66, 0.32), lam) + pow(lam, 10.0) * 0.30;
            }
            return brass;
        }
        if (t > 1.0 && t < 1.0 + max(1.0, 1.5 * s) / railH)
            col *= 0.55;                                       // the rail's shadow on the plate
        return col;
    }
    // NeonGlass (4): black glass — a faint diagonal sheen, as glass catches a room's light — and a neon tube in
    // the accent along the top edge, its light spilling onto the glass.
    float2 uv = px / uResolution;
    col += saturate(1.0 - abs(uv.x - uv.y * 0.35 - 0.30) * 7.0) * 0.022;
    float off = abs(px.y - 2.5 * s);
    float core = saturate(1.0 - off / (0.8 * s));
    float spill = exp(-off / (2.5 * s)) * 0.40;
    col = lerp(col, uAccent.rgb, max(core, spill));
    col += core * core * 0.30;
    return col;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 col = uBgColor.rgb;                          // the window background (a Flat skin's whole surface)
    col = applyMaterial(col, i.uv * uResolution, uParams.y, max(uParams.z, 1.0));

    // Heat-haze (Steampunk): a wavering vertical displacement scrolling upward — hot air
    // rising off brass — that ripples the underglow so it shimmers. uParams.x (heatHaze) is
    // 0 for every other skin, so wob==0, yWarp==uv.y, and the plume below is skipped: a
    // strict no-op there (the plain underglow is byte-identical).
    float hz  = saturate(uParams.x);
    float wob = 0.0;
    if (hz > 0.0) {
        wob = sin(i.uv.x * 24.0 + uTime * 3.1) * 0.5
            + sin(i.uv.x * 47.0 - uTime * 2.2) * 0.3
            + sin(i.uv.x * 11.0 + uTime * 1.5) * 0.2;   // layered ripple, ~[-1,1]
    }
    float yWarp = i.uv.y + wob * 0.06 * hz;             // displaced band coordinate

    // A soft coral/brass glow confined to the bottom ~22% of the strip — a subtle underline
    // that reads behind the transport controls, not a band filling the empty middle.
    float band    = smoothstep(0.78, 1.0, yWarp);
    float shimmer = 0.65 + 0.35 * sin(i.uv.x * 6.28318 * 1.2 + uTime * 0.9);
    float glow    = band * shimmer * saturate(uIntensity);
    col += uAccent.rgb * glow * 0.38;                   // gentle additive glow

    // Rising haze plume: a fainter warm veil that climbs higher than the band and "boils",
    // fading toward the top. Entirely gated by hz, so it exists only for Steampunk.
    if (hz > 0.0) {
        float plume = smoothstep(0.2, 1.0, yWarp);  // brightest at the bottom (hot strip), fading up
        float boil  = 0.55 + 0.45 * sin(i.uv.x * 33.0 - uTime * 5.0);
        col += uAccent.rgb * plume * boil * hz * 0.12;
    }

    // Exposure, not clipping. Light ADDED to a white surface cannot be displayed — saturate() used to
    // clip every channel back to 1, so on the Light skin the underglow was computed and then thrown
    // away in full (found by the RabbitEarsRender strip sheets). Scaling the pixel down by its
    // brightest channel instead keeps the RATIO between the channels, which is what the eye reads as
    // coloured light on a white surface: the accent survives as a warm tint. (Also the Cyberpunk strip's neon
    // tube, whose white-hot core exceeds 1 — scaled the same way.) Otherwise it is a strict no-op
    // wherever no channel exceeds 1 — every dark skin, by a wide margin (the glow peaks well under
    // 0.5 on all three). A branch rather than col /= max(peak, 1.0): D3D allows division ~2.5 ULP of
    // slack, so even dividing by exactly 1.0 is not promised to leave the dark skins' bytes alone.
    float peak = max(col.r, max(col.g, col.b));
    if (peak > 1.0)
        col /= peak;
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
