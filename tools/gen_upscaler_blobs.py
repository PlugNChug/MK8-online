#!/usr/bin/env python3
"""Generate the FSR 3.1 upscaler shader blob tables from the MIT-licensed FidelityFX SDK sources.

The published FidelityFX-SDK source does not ship the generated `*_permutations.h` files that the FSR 3
upscaler effect includes. This generator produces an equivalent table by compiling each pass from the
SDK's HLSL with DXC and reflecting the resulting SPIR-V descriptors, which is the same information the
generator that produced AMD's `ffx_blobs.cpp` emits. Nothing from AMD's signed binaries is used here.

Bindings are rewritten to a stable scheme that matches the SDK's Vulkan backend (the same one
`tools/gen_fg_blobs.py` uses for frame generation):
  - samplers at 320 (point) / 321 (linear),
  - constant buffers at 128+,
  - SRV textures at 0+, UAV textures at 64+,
  - SRV buffers at 192+, UAV buffers at 224+.
The blob metadata and the SPIR-V decorations are emitted from the same reflection data, so the
descriptor set layout, the pipeline bindings and the dispatch bindings stay consistent.

Storage-image formats: DXC infers a format from the HLSL element type (Rgba32f, R32f, R32ui, ...),
but the SDK creates its internal images with formats chosen for the effect (R8_UNORM, R16_FLOAT,
R32_UINT, the presentation format, ...). Vulkan requires the declared storage-image format to match
the image view, so every non-atomic storage image is rewritten to format Unknown with the
StorageImageRead/WriteWithoutFormat capabilities; the caller enables the matching device features.
Images accessed through OpImageTexelPointer (the SPD atomic counter) keep an explicit R32ui format,
which is the only form Vulkan allows there and matches the resource view.

Permutations: the provider is built for the options the Linux bridge uses. The FSR 3.1 effect
derives a six-bit permutation key from the context flags and the device capabilities; this table
covers `REPROJECT_USE_LANCZOS_TYPE` in both variants (the Vulkan backend reports wave 32/64 on RDNA
and 32/32 on some devices, which selects the LUT or the reference Lanczos path), with the bridge's
`LOW_RESOLUTION_MOTION_VECTORS=1`, and no HDR, jittered motion vectors or inverted depth. The
`APPLY_SHARPENING` bit selects the separately compiled accumulate-sharpen module. fp16 and wave64
lookups return the same fp32/wave32 modules, as AMD's shipped provider does: the output is correct,
only the half-float throughput optimisation is absent.

Usage:
  gen_upscaler_blobs.py --sdk <FidelityFX-SDK tree> --dxc <dxc> --out upscaler_blobs.cpp
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Pass enum -> (HLSL source, emitted identifier stem). The order follows FfxFsr3UpscalerPass.
PASSES = [
    ("FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS", "ffx_fsr3upscaler_prepare_inputs_pass.hlsl",
     "prepare_inputs"),
    ("FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID", "ffx_fsr3upscaler_luma_pyramid_pass.hlsl",
     "luma_pyramid"),
    ("FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID", "ffx_fsr3upscaler_shading_change_pyramid_pass.hlsl",
     "shading_change_pyramid"),
    ("FFX_FSR3UPSCALER_PASS_SHADING_CHANGE", "ffx_fsr3upscaler_shading_change_pass.hlsl",
     "shading_change"),
    ("FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY", "ffx_fsr3upscaler_prepare_reactivity_pass.hlsl",
     "prepare_reactivity"),
    ("FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY", "ffx_fsr3upscaler_luma_instability_pass.hlsl",
     "luma_instability"),
    ("FFX_FSR3UPSCALER_PASS_ACCUMULATE", "ffx_fsr3upscaler_accumulate_pass.hlsl",
     "accumulate"),
    ("FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN", "ffx_fsr3upscaler_accumulate_pass.hlsl",
     "accumulate_sharpen"),
    ("FFX_FSR3UPSCALER_PASS_RCAS", "ffx_fsr3upscaler_rcas_pass.hlsl",
     "rcas"),
    ("FFX_FSR3UPSCALER_PASS_DEBUG_VIEW", "ffx_fsr3upscaler_debug_view_pass.hlsl",
     "debug_view"),
    ("FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE", "ffx_fsr3upscaler_autogen_reactive_pass.hlsl",
     "generate_reactive"),
]

# Bit order must match POPULATE_PERMUTATION_KEY in ffx_fsr3upscaler_shaderblobs.cpp.
KEY_ORDER = [
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH",
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING",
]

# The fixed half of the permutation key. The Linux bridge creates the context with
# FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE only, which maps to no FSR3 initialization flag:
# display-resolution motion vectors are off (LOW_RES=1), no HDR, no jitter cancellation and
# normal depth. Keep these in the generated code and the switch in sync.
BASE_OPTIONS = {
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT": 0,
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": 1,
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": 0,
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": 0,
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE": 2,  # approximate, the SDK default
    "FFX_HALF": 0,
}

# Binding bases, mirroring the SDK Vulkan backend's fixed sampler shift.
BIND_SAMPLER_LINEAR = 321
BIND_SAMPLER_POINT = 320
BIND_SRV_TEX = 0
BIND_UAV_TEX = 64
BIND_CBV = 128
BIND_SRV_BUF = 192
BIND_UAV_BUF = 224


class Module:
    def __init__(self, name):
        self.name = name
        self.data = b""
        self.cbv = []
        self.srv_tex = []
        self.uav_tex = []
        self.srv_buf = []
        self.uav_buf = []
        self.samplers = []


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}\n{proc.stdout}\n{proc.stderr}")
    return proc.stdout


def classify(text, bindings):
    """Return descriptor variables in declaration order with their class and new binding."""
    types = {}
    for m in re.finditer(r"^\s*%(\w+) = (OpType\w+)(.*)$", text, re.M):
        types[m.group(1)] = (m.group(2), m.group(3))

    ptr_target = {}
    for tid, (op, rest) in types.items():
        if op == "OpTypePointer":
            toks = rest.split()
            if toks and toks[0] in ("UniformConstant", "Uniform", "StorageBuffer"):
                ptr_target[tid] = toks[1].lstrip("%")

    nonwritable = set(re.findall(r"OpDecorate %(\w+) NonWritable", text))
    # DXC decorates the runtime-array member of a StructuredBuffer NonWritable, not the variable.
    nonwritable_members = {}
    for m in re.finditer(r"OpMemberDecorate %(\w+) (\d+) NonWritable", text):
        nonwritable_members.setdefault(m.group(1), set()).add(int(m.group(2)))

    out = []
    counters = {"srv_tex": 0, "uav_tex": 0, "cbv": 0, "srv_buf": 0, "uav_buf": 0}
    for m in re.finditer(
            r"^\s*%(\w+) = OpVariable %(\w+) (UniformConstant|Uniform|StorageBuffer)\b", text, re.M):
        var, ptr = m.group(1), m.group(2)
        pointee = ptr_target.get(ptr)
        if pointee is None:
            continue
        op, rest = types[pointee]
        name = bindings.get(var, {}).get("name", var)
        if op == "OpTypeImage":
            toks = rest.split()
            sampled = int(toks[5]) if len(toks) > 5 else 1
            kind = "srv_tex" if sampled == 1 else "uav_tex"
        elif op == "OpTypeSampler":
            kind = "sampler"
        elif op == "OpTypeStruct":
            members = [t.lstrip("%") for t in rest.split()]
            runtime_arrays = [i for i, t in enumerate(members)
                              if types.get(t, ("", ""))[0] == "OpTypeRuntimeArray"]
            if runtime_arrays:
                # StructuredBuffer (SRV) / RWStructuredBuffer (UAV). Both live in a Uniform or
                # StorageBuffer storage class in the Vulkan 1.2 SPIR-V DXC emits; the read-only one
                # has NonWritable on the variable or on its runtime-array member.
                readonly = var in nonwritable or any(
                    i in nonwritable_members.get(pointee, ()) for i in runtime_arrays)
                kind = "srv_buf" if readonly else "uav_buf"
            else:
                kind = "cbv"
        else:
            raise RuntimeError(f"unclassified descriptor {name} ({op}) in module")
        if kind == "sampler":
            binding = BIND_SAMPLER_LINEAR if "linear" in name.lower() else BIND_SAMPLER_POINT
        else:
            binding = {
                "srv_tex": BIND_SRV_TEX,
                "uav_tex": BIND_UAV_TEX,
                "cbv": BIND_CBV,
                "srv_buf": BIND_SRV_BUF,
                "uav_buf": BIND_UAV_BUF,
            }[kind] + counters[kind]
            counters[kind] += 1
        out.append((var, name, kind, binding))
    return out


def rewrite_bindings(text, descriptors):
    new_binding = {var: binding for var, _, _, binding in descriptors}
    text = re.sub(
        r"(OpDecorate %(\w+) Binding )(\d+)",
        lambda m: f"{m.group(1)}{new_binding.get(m.group(2), m.group(3))}",
        text,
    )
    return text


def rewrite_storage_image_formats(text):
    """Declare storage image formats Unknown (except images used by atomics) and add capabilities.

    The SDK creates its internal images with formats chosen for the effect (R8_UNORM,
    R16_FLOAT, RGBA16_FLOAT, R32_UINT, the presentation format, ...), while DXC infers
    Rgba32f/R32f/R32ui/... from the HLSL element type. Rewriting to Unknown moves the format
    decision to the image view, which is the only way the same shader can serve all of them.
    OpTypeImage declarations that become identical are merged because SPIR-V forbids duplicate
    non-aggregate types.

    Vulkan requires OpImageTexelPointer (InterlockedAdd and friends) to name an explicit 32/64-bit
    format, so the images used by atomics keep the format DXC inferred. That is the SPD atomic
    counter (R32_UINT), and R32ui matches the SDK view.
    """
    var_type = {}
    for m in re.finditer(
            r"^\s*%(\w+) = OpVariable %(\w+) (?:UniformConstant|Uniform|StorageBuffer)\b", text, re.M):
        var_type[m.group(1)] = m.group(2)
    ptr_type = {}
    for m in re.finditer(r"^\s*%(\w+) = OpTypePointer \w+ %(\w+)", text, re.M):
        ptr_type[m.group(1)] = m.group(2)
    atomic_types = set()
    for m in re.finditer(r"OpImageTexelPointer \S+ %(\w+)\b", text):
        pointer = var_type.get(m.group(1))
        target = ptr_type.get(pointer) if pointer else None
        if target:
            atomic_types.add(target)

    lines = text.splitlines()
    aliases = {}
    seen = {}
    drop = set()
    changed = False
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)%(\w+) = OpTypeImage (.*)$", line)
        if not m:
            continue
        tokens = m.group(3).split()
        # Tokens: sampled type, dim, depth, arrayed, ms, sampled, format[, access qualifier]
        if len(tokens) < 7:
            continue
        if int(tokens[5]) == 2 and tokens[6] != "Unknown":
            if m.group(2) in atomic_types:
                # Vulkan's OpImageTexelPointer rule: only an explicit 32/64-bit format is valid.
                if tokens[6] not in ("R64i", "R64ui", "R32f", "R32i", "R32ui"):
                    raise RuntimeError(
                        f"atomic storage image has unsupported format {tokens[6]} (module uses OpImageTexelPointer)")
            else:
                tokens[6] = "Unknown"
                line = f"{m.group(1)}%{m.group(2)} = OpTypeImage " + " ".join(tokens)
                lines[i] = line
                changed = True
        payload = " ".join(tokens)
        if payload in seen:
            aliases[m.group(2)] = seen[payload]
            drop.add(i)
        else:
            seen[payload] = m.group(2)

    kept = [line for i, line in enumerate(lines) if i not in drop]
    text = "\n".join(kept) + "\n"
    for duplicate, keep in aliases.items():
        # Drop metadata for the merged type before repointing every use at the kept type.
        text = re.sub(rf"^\s*Op(Name|Decorate|MemberDecorate) %{duplicate}\b.*\n", "", text, flags=re.M)
        text = re.sub(rf"%{duplicate}\b", f"%{keep}", text)

    if changed or aliases:
        # DXC adds StorageImageExtendedFormats for the Rgba32f/R32f formats it inferred. It is only
        # needed for formats outside the core set; with everything but the atomic R32ui images
        # Unknown it would require an extra device feature for no reason.
        core = {"R64i", "R64ui", "R32f", "R32i", "R32ui"}
        formats = re.findall(r"OpTypeImage \S+ \S+ \d+ \d+ \d+ \d+ (\w+)", text)
        if all(fmt in core or fmt == "Unknown" for fmt in formats):
            text = re.sub(r"^\s*OpCapability StorageImageExtendedFormats\n", "", text, flags=re.M)
        # A storage image with format Unknown needs the matching read/write capability, no matter
        # whether this module had a format to rewrite (a three-component image is already Unknown).
        has_unknown_storage = any(
            int(tokens[5]) == 2 and tokens[6] == "Unknown"
            for tokens in (m.group(1).split() for m in re.finditer(r"OpTypeImage (.*)$", text, re.M))
            if len(tokens) >= 7)
        if has_unknown_storage:
            capabilities = []
            if re.search(r"OpImageRead\b", text):
                capabilities.append("StorageImageReadWithoutFormat")
            if re.search(r"OpImageWrite\b", text):
                capabilities.append("StorageImageWriteWithoutFormat")
            if capabilities and "OpCapability Shader" in text:
                anchor = re.search(r"^\s*OpCapability Shader\n", text, re.M)
                added = "".join(f"               OpCapability {capability}\n" for capability in capabilities
                                if f"OpCapability {capability}" not in text)
                text = text[:anchor.end()] + added + text[anchor.end():]
    return text


def disassemble(spv):
    return run(["spirv-dis", str(spv)])


def assemble(text, out_spv):
    asm = out_spv.with_suffix(".spvasm")
    asm.write_text(text, encoding="utf-8")
    run(["spirv-as", "--target-env", "vulkan1.2", str(asm), "-o", str(out_spv)])
    return out_spv.read_bytes()


def name_map(text):
    out = {}
    for m in re.finditer(r'OpName %(\w+) "([^"]*)"', text):
        out.setdefault(m.group(1), {})["name"] = m.group(2)
    return out


def build_module(module_name, hlsl, include_dirs, dxc, defines, tmp):
    spv = tmp / f"{module_name}.spv"
    cmd = [dxc, "-spirv", "-T", "cs_6_0", "-E", "CS", "-fspv-target-env=vulkan1.2",
           "-DFFX_GPU=1", "-DFFX_HLSL=1"]
    for name, value in defines.items():
        cmd.append(f"-D{name}={value}")
    for inc in include_dirs:
        cmd.append(f"-I{inc}")
    cmd += [str(hlsl), "-Fo", str(spv)]
    run(cmd)

    text = disassemble(spv)
    text = rewrite_storage_image_formats(text)
    bindings = name_map(text)
    descriptors = classify(text, bindings)
    text = rewrite_bindings(text, descriptors)
    data = assemble(text, spv)

    mod = Module(module_name)
    mod.data = data
    for var, name, kind, binding in descriptors:
        entry = (name, binding)
        if kind == "cbv":
            mod.cbv.append(entry)
        elif kind == "srv_tex":
            mod.srv_tex.append(entry)
        elif kind == "uav_tex":
            mod.uav_tex.append(entry)
        elif kind == "srv_buf":
            mod.srv_buf.append(entry)
        elif kind == "uav_buf":
            mod.uav_buf.append(entry)
        elif kind == "sampler":
            mod.samplers.append(entry)
    return mod


def c_bytes(data):
    return ",".join(str(b) for b in data)


def c_names(values):
    # A zero-length array is not valid C++ (MSVC rejects it); the count in the blob is 0, so the
    # contents are never read. Emit one null entry so the declaration is well-formed everywhere.
    if not values:
        return "{nullptr}"
    return "{" + ", ".join(f'"{v}"' for v in values) + "}"


def c_u32(values):
    if not values:
        return "{0}"
    return "{" + ", ".join(str(v) for v in values) + "}"


def emit_blob(mod):
    ident = mod.name
    lines = []
    lines.append(f"static const unsigned char {ident}_data[] = {{{c_bytes(mod.data)}}};")
    for field, values in (
        ("cbv_names", [n for n, _ in mod.cbv]),
        ("srv_tex_names", [n for n, _ in mod.srv_tex]),
        ("uav_tex_names", [n for n, _ in mod.uav_tex]),
        ("srv_buf_names", [n for n, _ in mod.srv_buf]),
        ("uav_buf_names", [n for n, _ in mod.uav_buf]),
        ("sampler_names", [n for n, _ in mod.samplers]),
    ):
        lines.append(f"static const char* {ident}_{field}[] = {c_names(values)};")
    for field, values in (
        ("cbv_bind", [b for _, b in mod.cbv]),
        ("srv_tex_bind", [b for _, b in mod.srv_tex]),
        ("uav_tex_bind", [b for _, b in mod.uav_tex]),
        ("srv_buf_bind", [b for _, b in mod.srv_buf]),
        ("uav_buf_bind", [b for _, b in mod.uav_buf]),
        ("sampler_bind", [b for _, b in mod.samplers]),
    ):
        lines.append(f"static const uint32_t {ident}_{field}[] = {c_u32(values)};")
        lines.append(f"static const uint32_t {ident}_{field}_counts[] = {c_u32([1] * len(values))};")
        lines.append(f"static const uint32_t {ident}_{field}_spaces[] = {c_u32([0] * len(values))};")
    lines.append(
        "extern const FfxShaderBlob %s = {\n"
        "    %s_data, sizeof(%s_data), \"CS\",\n"
        "    %d, %d, %d, %d, %d, %d, 0,\n"
        "    %s_cbv_names, %s_cbv_bind, %s_cbv_bind_counts, %s_cbv_bind_spaces,\n"
        "    %s_srv_tex_names, %s_srv_tex_bind, %s_srv_tex_bind_counts, %s_srv_tex_bind_spaces,\n"
        "    %s_uav_tex_names, %s_uav_tex_bind, %s_uav_tex_bind_counts, %s_uav_tex_bind_spaces,\n"
        "    %s_srv_buf_names, %s_srv_buf_bind, %s_srv_buf_bind_counts, %s_srv_buf_bind_spaces,\n"
        "    %s_uav_buf_names, %s_uav_buf_bind, %s_uav_buf_bind_counts, %s_uav_buf_bind_spaces,\n"
        "    %s_sampler_names, %s_sampler_bind, %s_sampler_bind_counts, %s_sampler_bind_spaces,\n"
        "    nullptr, nullptr, nullptr, nullptr\n"
        "};\n" % (
            ident, ident, ident,
            len(mod.cbv), len(mod.srv_tex), len(mod.uav_tex), len(mod.srv_buf), len(mod.uav_buf),
            len(mod.samplers),
            *([ident] * 24),
        )
    )
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--dxc", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--tmp", type=Path, default=Path("/tmp/upscaler-blobgen"))
    args = ap.parse_args()

    fsr3 = args.sdk / "Kits" / "FidelityFX" / "upscalers" / "fsr3"
    shaders = fsr3 / "internal" / "shaders"
    includes = [
        fsr3 / "include" / "gpu",
        args.sdk / "Kits" / "FidelityFX" / "api" / "internal" / "gpu",
    ]
    args.tmp.mkdir(parents=True, exist_ok=True)

    chunks = [
        "// Generated by tools/gen_upscaler_blobs.py from the MIT-licensed FidelityFX SDK 2.4.0 sources.\n"
        "// Do not edit. This file contains SPIR-V compiled from AMD's HLSL; it is build output, not a shipped asset.\n"
        '#include "ffx_fsr3upscaler_shaderblobs.h"\n\n',
    ]
    module_idents = []

    def write_module(mod):
        text = (
            "// Generated by tools/gen_upscaler_blobs.py. Build output, not a shipped asset.\n"
            '#include "ffx_fsr3upscaler_shaderblobs.h"\n\n'
            + emit_blob(mod)
        )
        (args.out.parent / f"upscaler_blob_{mod.name}.cpp").write_text(text, encoding="utf-8")
        module_idents.append(mod.name)

    def compile_variant(ident, hlsl, reproject_lanczos, apply_sharpening):
        defines = dict(BASE_OPTIONS)
        defines["FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE"] = reproject_lanczos
        defines["FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING"] = apply_sharpening
        mod = build_module(ident, shaders / hlsl, includes, args.dxc, defines, args.tmp)
        write_module(mod)
        return mod

    # One module per pass. Only the accumulate passes read the reproject Lanczos option, and only
    # the accumulate-sharpen pass sets APPLY_SHARPENING, so the other passes compile once.
    for enum, hlsl, ident in PASSES:
        if ident in ("accumulate", "accumulate_sharpen"):
            continue
        compile_variant(ident, hlsl, 0, 0)
    compile_variant("accumulate_ref", "ffx_fsr3upscaler_accumulate_pass.hlsl", 0, 0)
    compile_variant("accumulate_lut", "ffx_fsr3upscaler_accumulate_pass.hlsl", 1, 0)
    compile_variant("accumulate_sharpen_ref", "ffx_fsr3upscaler_accumulate_pass.hlsl", 0, 1)
    compile_variant("accumulate_sharpen_lut", "ffx_fsr3upscaler_accumulate_pass.hlsl", 1, 1)

    switch_lines = []
    for enum, _, ident in PASSES:
        if ident == "accumulate":
            switch_lines.append(
                f"        case {enum}: blob = lanczos ? &accumulate_lut : &accumulate_ref; break;")
        elif ident == "accumulate_sharpen":
            switch_lines.append(
                f"        case {enum}: blob = lanczos ? &accumulate_sharpen_lut : &accumulate_sharpen_ref; break;")
        else:
            switch_lines.append(f"        case {enum}: blob = &{ident}; break;")

    for ident in module_idents:
        chunks.append(f"extern const FfxShaderBlob {ident};\n")

    chunks.append(f'''
extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)
{{
    // Bit 0 is FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE. The Vulkan backend reports wave
    // 32/64 on RDNA (LUT) and 32/32 on some devices (reference), so both accumulate variants exist.
    // The other key bits are fixed by this build: LOW_RES=1, HDR/JITTERED/INVERTED=0.
    const bool lanczos = (permutationOptions & (1u << 0)) != 0;
    const FfxShaderBlob* blob = nullptr;
    switch (passId)
    {{
{chr(10).join(switch_lines)}
        default: return FFX_ERROR_INVALID_ARGUMENT;
    }}
    if (!blob) return FFX_ERROR_INVALID_ARGUMENT;
    *outBlob = *blob;
    return FFX_OK;
}}

extern "C" FfxErrorCode fsr3UpscalerIsWave64(uint32_t, bool& isWave64)
{{
    isWave64 = false;
    return FFX_OK;
}}
''')

    args.out.write_text("\n".join(chunks), encoding="utf-8")
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes, {len(PASSES) + 2} modules)")


if __name__ == "__main__":
    sys.exit(main())
