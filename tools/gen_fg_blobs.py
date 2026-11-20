#!/usr/bin/env python3
"""Generate the FSR 3 frame-generation shader blob tables from the MIT-licensed FidelityFX SDK sources.

The published FidelityFX-SDK source does not ship the generated `*_permutations.h` files that the
frame-interpolation and optical-flow effect implementations include. This generator produces an
equivalent table by compiling each pass from the SDK's HLSL with DXC and reflecting the resulting
SPIR-V descriptors, which is the same information the generator that produced `ffx_blobs.cpp`
would have emitted. Nothing from AMD's signed binaries is used here.

Bindings are rewritten to a stable scheme that matches the SDK's Vulkan backend:
  - samplers at 320 (point) / 321 (linear),
  - constant buffers at 128+,
  - SRV textures at 0+, UAV textures at 64+,
  - SRV buffers at 192+, UAV buffers at 224+.
The blob metadata and the SPIR-V decorations are emitted from the same reflection data, so the
descriptor set layout, the pipeline bindings and the dispatch bindings stay consistent.

Usage:
  gen_fg_blobs.py --sdk <FidelityFX-SDK tree> --dxc <dxc> --out fg_blobs.cpp
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

FI_PASSES = [
    ("FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE", "ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_SETUP", "ffx_frameinterpolation_setup_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH", "ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD", "ffx_frameinterpolation_game_motion_vector_field_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD", "ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK", "ffx_frameinterpolation_disocclusion_mask_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION", "ffx_frameinterpolation_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID", "ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_INPAINTING", "ffx_frameinterpolation_inpainting_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID", "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl"),
    ("FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW", "ffx_frameinterpolation_debug_view_pass.hlsl"),
]

OF_PASSES = [
    ("FFX_OPTICALFLOW_PASS_PREPARE_LUMA", "ffx_opticalflow_prepare_luma_pass.hlsl"),
    ("FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID", "ffx_opticalflow_compute_luminance_pyramid_pass.hlsl"),
    ("FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM", "ffx_opticalflow_generate_scd_histogram_pass.hlsl"),
    ("FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE", "ffx_opticalflow_compute_scd_divergence_pass.hlsl"),
    ("FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5", "ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl"),
    ("FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5", "ffx_opticalflow_filter_optical_flow_pass_v5.hlsl"),
    ("FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5", "ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl"),
]

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
            if toks and toks[0] == "UniformConstant":
                ptr_target[tid] = toks[1].lstrip("%")

    nonwritable = set(re.findall(r"OpDecorate %(\w+) NonWritable", text))

    out = []
    counters = {"srv_tex": 0, "uav_tex": 0, "cbv": 0, "srv_buf": 0, "uav_buf": 0}
    for m in re.finditer(r"^\s*%(\w+) = OpVariable %(\w+) UniformConstant", text, re.M):
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
            if any(types.get(t, ("", ""))[0] == "OpTypeRuntimeArray" for t in members):
                kind = "srv_buf" if var in nonwritable else "uav_buf"
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
    for d in defines:
        cmd.append(f"-D{d}")
    for inc in include_dirs:
        cmd.append(f"-I{inc}")
    cmd += [str(hlsl), "-Fo", str(spv)]
    run(cmd)

    text = disassemble(spv)
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
    return "{" + ", ".join(f'"{v}"' for v in values) + "}"


def c_u32(values):
    return "{" + ", ".join(str(v) for v in values) + "}"


def emit_blob(mod, guard):
    lines = []
    ident = guard
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
        lines.append(f"static const uint32_t {ident}_{field}_counts[] = {{{','.join(['1'] * len(values))}}};")
        lines.append(f"static const uint32_t {ident}_{field}_spaces[] = {{{','.join(['0'] * len(values))}}};")
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
            len(mod.cbv), len(mod.srv_tex), len(mod.uav_tex), len(mod.srv_buf), len(mod.uav_buf), len(mod.samplers),
            *([ident] * 24),
        )
    )
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--dxc", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--tmp", type=Path, default=Path("/tmp/fg-blobgen"))
    args = ap.parse_args()

    fsr3 = args.sdk / "Kits" / "FidelityFX" / "framegeneration" / "fsr3"
    shaders = fsr3 / "internal" / "shaders"
    includes = [
        fsr3 / "include" / "gpu",
        args.sdk / "Kits" / "FidelityFX" / "api" / "internal" / "gpu",
    ]
    args.tmp.mkdir(parents=True, exist_ok=True)

    chunks = [
        "// Generated by tools/gen_fg_blobs.py from the MIT-licensed FidelityFX SDK 2.4.0 sources.\n"
        "// Do not edit. This file contains SPIR-V compiled from AMD's HLSL; it is build output, not a shipped asset.\n"
        '#include "ffx_frameinterpolation_shaderblobs.h"\n'
        '#include "ffx_opticalflow_shaderblobs.h"\n',
    ]

    module_idents = []

    def write_module(mod, guard):
        text = (
            "// Generated by tools/gen_fg_blobs.py. Build output, not a shipped asset.\n"
            '#include "ffx_frameinterpolation_shaderblobs.h"\n'
            '#include "ffx_opticalflow_shaderblobs.h"\n\n'
            + emit_blob(mod, guard)
        )
        (args.out.parent / f"fg_blob_{guard}.cpp").write_text(text, encoding="utf-8")
        module_idents.append(guard)
        return guard

    fi_variants = {}
    for depth_inverted in (0, 1):
        for enum, hlsl in FI_PASSES:
            mod = build_module(
                f"fi_{enum.lower()}_d{depth_inverted}",
                shaders / hlsl,
                includes,
                args.dxc,
                [
                    f"FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=0",
                    f"FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0",
                    f"FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={depth_inverted}",
                ],
                args.tmp,
            )
            fi_variants.setdefault(enum, {})[depth_inverted] = mod
            write_module(mod, f"fi_{enum.lower()}_d{depth_inverted}")

    of_variants = {}
    for enum, hlsl in OF_PASSES:
        mod = build_module(
            f"of_{enum.lower()}",
            shaders / hlsl,
            includes,
            args.dxc,
            ["FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0"],
            args.tmp,
        )
        of_variants[enum] = mod
        write_module(mod, f"of_{enum.lower()}")

    fi_switch = "\n".join(
        f"        case {enum}: blob = inverted ? &fi_{enum.lower()}_d1 : &fi_{enum.lower()}_d0; break;"
        for enum, _ in FI_PASSES
    )
    of_switch = "\n".join(
        f"        case {enum}: blob = &of_{enum.lower()}; break;"
        for enum, _ in OF_PASSES
    )

    for ident in module_idents:
        chunks.append(f"extern const FfxShaderBlob {ident};\n")

    chunks.append(f'''
extern "C" FfxErrorCode frameInterpolationGetPermutationBlobByIndex(
    FfxFrameInterpolationPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)
{{
    const bool inverted = (permutationOptions & (1u << 2)) != 0; // FRAMEINTERPOLATION_SHADER_PERMUTATION_DEPTH_INVERTED
    const FfxShaderBlob* blob = nullptr;
    switch (passId)
    {{
{fi_switch}
        default: return FFX_ERROR_INVALID_ARGUMENT;
    }}
    *outBlob = *blob;
    return FFX_OK;
}}

extern "C" FfxErrorCode frameInterpolationIsWave64(uint32_t, bool& isWave64)
{{
    isWave64 = false;
    return FFX_OK;
}}

extern "C" FfxErrorCode opticalflowGetPermutationBlobByIndex(
    FfxOpticalflowPass passId, uint32_t, FfxShaderBlob* outBlob)
{{
    const FfxShaderBlob* blob = nullptr;
    switch (passId)
    {{
{of_switch}
        default: return FFX_ERROR_INVALID_ARGUMENT;
    }}
    *outBlob = *blob;
    return FFX_OK;
}}

extern "C" FfxErrorCode opticalflowIsWave64(uint32_t, bool& isWave64)
{{
    isWave64 = false;
    return FFX_OK;
}}
''')

    args.out.write_text("\n".join(chunks), encoding="utf-8")
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes, {len(fi_variants)} FI passes x2, {len(of_variants)} OF passes)")


if __name__ == "__main__":
    sys.exit(main())
