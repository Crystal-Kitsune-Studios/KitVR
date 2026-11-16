#pragma once

// Logo slot for the connecting scene.
//
// The scene floats a square panel 2.5 m in front of you, above the status text, and calls
// kitvrLogo() for every pixel on it. Replace the body of this function with the real logo
// animation; nothing else needs to change.
//
//   uv     -1..1 across the panel, (0, 0) in the middle, +y up
//   time   seconds since the scene appeared
//   phase  0 connecting to the PC, 1 starting the stream, 2 connected (the scene is fading out)
//
// Return premultiplied colour: rgb already multiplied by alpha. Alpha 0 shows the space behind.
// The panel size and position are kLogoCenter and kLogoHalfSize in wait_scene.cpp.
inline const char* kWaitLogoShader = R"(
vec4 kitvrLogo(vec2 uv, float time, int phase) {
    // Placeholder: a pulsing crystal with a moon circling it on a tilted orbit.
    float speed = phase == 0 ? 1.1 : (phase == 1 ? 2.2 : 4.5);
    float angle = time * speed;

    float diamond = abs(uv.x) * 1.7 + abs(uv.y);
    float edge = 1.0 - smoothstep(0.0, 0.03, abs(diamond - 0.55));
    float fill = (1.0 - smoothstep(0.53, 0.57, diamond)) *
                 (0.22 + 0.10 * sin(time * 2.0 + uv.y * 7.0));
    vec3 rgb = vec3(0.60, 0.88, 1.00) * edge + vec3(0.30, 0.50, 0.95) * fill;
    float alpha = max(edge, fill);

    vec2 orbit = vec2(uv.x, uv.y * 3.4);
    float ring = 1.0 - smoothstep(0.0, 0.06, abs(length(orbit) - 0.90));
    float behind = atan(orbit.y, orbit.x);
    float trail = exp(-6.0 * fract((angle - behind) / 6.2831853));
    rgb += vec3(0.95, 0.55, 0.25) * ring * (0.12 + 0.88 * trail);
    alpha = max(alpha, ring * (0.12 + 0.88 * trail));

    vec2 moon = vec2(cos(angle), sin(angle) / 3.4) * 0.90;
    float moonDot = 1.0 - smoothstep(0.035, 0.065, length(uv - moon));
    rgb += vec3(1.0, 0.80, 0.55) * moonDot;
    alpha = max(alpha, moonDot);

    // Fade the glow out before the panel edge so the slot itself stays invisible.
    float halo = exp(-3.0 * length(uv)) * 0.25 * (1.0 - smoothstep(0.6, 1.0, length(uv)));
    rgb += vec3(0.95, 0.55, 0.25) * halo;
    alpha = max(alpha, halo);
    return vec4(rgb, clamp(alpha, 0.0, 1.0));
}
)";
