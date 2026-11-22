// Match the SPH and all 31 instruction words of the 4.0.1 Castle extraction.
// The adjacent Rainbow Road variant uses different depth math.
static bool mk8_castle_seed(const uint8_t *blob, size_t size, size_t words) {
    if (words != 31 || size < 0x80 + words * 8) return false;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0x30; i < 0x80 + words * 8; i++)
        hash = (hash ^ blob[i]) * 0x100000001b3ull;
    return hash == 0x36467fd58ef1c9ceull;
}

// The 4.0.1 AGL bloom_reduce variant is only eleven words. Include its
// interface and entire program so the many one-texture copies stay original.
static bool mk8_castle_reduce(const uint8_t *blob, size_t size, size_t words) {
    if (words != 11 || size < 0x80 + 11 * 8) return false;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0x30; i < 0x80 + 11 * 8; i++)
        hash = (hash ^ blob[i]) * 0x100000001b3ull;
    return hash == 0x5d67f4bf0f297040ull;
}

// The nonlinear bright pass runs before averaging. This preserves the energy
// of a thin moving highlight crossing a downsampled output pixel.
static void mk8_castle_resolve(Str *s) {
    sp(s, "}\nvoid main() {\n"
          "  vec4 dx = dFdx(v0), dy = dFdy(v0);\n"
          "  vec2 size0 = vec2(textureSize(t0, 0));\n"
          "  vec2 footprint = vec2(length(dx.xy * size0), length(dy.xy * size0));\n"
          "  float spread = smoothstep(1.0, 1.95, min(footprint.x, footprint.y))\n"
          "               * (1.0 - smoothstep(2.05, 3.0, max(footprint.x, footprint.y)));\n"
          "  if (rscale[1].z > 0.5 && spread > 0.0) {\n"
          "    dx *= 0.25 * spread; dy *= 0.25 * spread;\n"
          "    mk8_castle_sample(v0 - dx - dy); vec4 sum = c0;\n"
          "    mk8_castle_sample(v0 + dx - dy); sum += c0;\n"
          "    mk8_castle_sample(v0 - dx + dy); sum += c0;\n"
          "    mk8_castle_sample(v0 + dx + dy); c0 = (sum + c0) * 0.25;\n"
          "  } else { mk8_castle_sample(v0); }\n");
}
