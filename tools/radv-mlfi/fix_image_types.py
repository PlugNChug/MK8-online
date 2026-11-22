#!/usr/bin/env python3
"""Legalize SPIR-V image types for RADV before creating a compute pipeline.

Why this exists
---------------
AMD's FSR 4 MLFI modules declare images with a 16-bit sampled type:

    %type_2d_image = OpTypeImage %half 2D 2 0 0 2 Unknown

Vulkan forbids that (VUID-StandaloneSpirv-OpTypeImage-04656: a sampled type must
be a 32-bit int, 64-bit int, or 32-bit float scalar), and RADV does not reject
it: `vkCreateComputePipelines` segfaults.  See docs/RADV-MLFI-CRASH.md.

This tool rewrites such a module into the equivalent legal form:

  * the image's sampled type becomes 32-bit (float for Float16, int for Int16);
  * an `OpImageWrite` whose value is a 16-bit vector gets an `OpFConvert` to the
    32-bit vector right before it;
  * an `OpImageSample*`/`OpImageFetch`/`OpImageRead` that returned a 16-bit
    vector keeps its old result id: the fetch is re-typed to the 32-bit vector
    and an `OpFConvert` back to the old type is inserted, so no consumer changes.

For a 16-bit float image view (R16G16B16A16_SFLOAT, R16G16_SFLOAT, ...) the
rewrite is exact: f16 -> f32 -> f16 is lossless, and the hardware store converts
f32 to the view format with the same rounding as `OpFConvert`.  For other view
formats the values are identical up to the rounding of the final f32 -> f16
conversion the shader already asked for.

Everything happens on `spirv-dis` text; `spirv-tools` is required (the project
already depends on it for module validation).  The input module is never
modified; the rewritten module is written next to the output path given.

Usage:
    tools/radv-mlfi/fix_image_types.py --in mod.spv --out mod.fixed.spv [--keep-asm]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ID_RE = re.compile(r"%([A-Za-z0-9_]+)")
ASSIGN_RE = re.compile(r"^\s*%([A-Za-z0-9_]+)\s*=\s*(Op[A-Za-z0-9_]+)\b(.*)$")
PLAIN_RE = re.compile(r"^\s*(Op[A-Za-z0-9_]+)\b(.*)$")

# Result-valued opcodes whose second operand is the result type.  OpType* is
# handled separately because there the type *is* the result.
UNTYPED_OPS = {
    "OpLabel",
    "OpFunction",
    "OpFunctionParameter",  # typed, but the type is operand 2 too; harmless
    "OpString",
    "OpExtInstImport",
    "OpDecorationGroup",
    "OpTypeVoid",
}

FORMAT_IDS = {
    "Unknown": 0, "Rgba32f": 1, "Rgba16f": 2, "R32f": 3, "Rgba8": 4, "Rgba8Snorm": 5, "Rg32f": 6,
    "Rg16f": 7, "R11fG11fB10f": 8, "R16f": 9, "Rgba16": 10, "Rgb10A2": 11, "Rg16": 12, "Rg8": 13,
    "R16": 14, "R8": 15, "Rgba16Snorm": 16, "Rg16Snorm": 17, "Rg8Snorm": 18, "R16Snorm": 19,
    "R8Snorm": 20, "Rgba32i": 21, "Rgba32ui": 22, "Rgba16i": 23, "Rgba16ui": 24, "Rgba8i": 25,
    "Rgba8ui": 26, "R32i": 27, "R32ui": 28, "Rg32i": 29, "Rg32ui": 30, "Rg16i": 31, "Rg16ui": 32,
    "Rg8i": 33, "Rg8ui": 34, "R16i": 35, "R16ui": 36, "R8i": 37, "R8ui": 38, "R64ui": 39,
    "R64i": 40,
}

IMAGE_RESULT_PREFIXES = (
    "OpImageSample",
    "OpImageFetch",
    "OpImageRead",
    "OpImageGather",
    "OpImageDrefGather",
    "OpImageSparse",
)


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def disassemble(spv, asm_path):
    run(["spirv-dis", "--no-color", "--no-indent", spv, "-o", asm_path])


def assemble(asm_path, spv_path):
    run(["spirv-as", "--target-env", "vulkan1.2", asm_path, "-o", spv_path])


class Instruction:
    __slots__ = ("result", "opcode", "rest", "raw")

    def __init__(self, raw):
        self.raw = raw
        m = ASSIGN_RE.match(raw)
        if m:
            self.result, self.opcode, self.rest = m.group(1), m.group(2), m.group(3)
        else:
            self.result = None
            m = PLAIN_RE.match(raw)
            self.opcode = m.group(1) if m else None
            self.rest = m.group(2) if m else raw

    def tokens(self):
        return self.rest.split()

    def render(self, rest=None):
        if rest is None:
            rest = self.rest
        if self.result is not None:
            return f"%{self.result} = {self.opcode}{rest}"
        return f"{self.opcode}{rest}" if self.opcode else self.raw


def parse(text):
    """Split disassembly into a header (comments) and Instruction list."""
    header, body = [], []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith(";"):
            header.append(stripped)
            continue
        body.append(Instruction(line))
    return header, body


def scalar_kind(body):
    """Map type id -> ('float'|'int'|'other', width) for scalar OpType declarations."""
    kinds = {}
    for ins in body:
        if ins.opcode == "OpTypeFloat":
            kinds[ins.result] = ("float", int(ins.tokens()[0]))
        elif ins.opcode == "OpTypeInt":
            toks = ins.tokens()
            signed = len(toks) > 1 and toks[1] == "1"
            kinds[ins.result] = ("int", int(toks[0]), signed)
    return kinds


# Type declarations synthesized by the rewrite.  They cannot be appended to the instruction list
# because SPIR-V requires a type to be declared before its first use; they are spliced in before
# the first OpFunction once all rewrites are done.
EXTRA_TYPES = []


def find_or_add_type(body, opcode, operands, kinds=None):
    """Return the id of an existing declaration, else queue a new one."""
    for ins in list(body) + EXTRA_TYPES:
        if ins.opcode == opcode and ins.tokens() == operands:
            return ins.result
    used = {ins.result for ins in list(body) + EXTRA_TYPES if ins.result}
    for ins in list(body) + EXTRA_TYPES:
        used.update(ID_RE.findall(ins.rest))
    n = 1
    while str(n) in used:
        n += 1
    new_id = str(n)
    EXTRA_TYPES.append(Instruction(f"%{new_id} = {opcode} " + " ".join(operands)))
    return new_id


def vector_type(body, component_id, width):
    if width == 1:
        return component_id
    return find_or_add_type(body, "OpTypeVector", [f"%{component_id}", str(width)])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="inp", required=True, help="input .spv")
    ap.add_argument("--out", dest="out", required=True, help="output .spv (rewritten)")
    ap.add_argument("--keep-asm", action="store_true", help="keep the .spvasm next to the output")
    ap.add_argument("--i64-sampled-to-storage", action="store_true",
                    help="rewrite 64-bit sampled images (Mesa cannot build a texture type for them) to "
                         "R64ui/R64i storage images read with OpImageRead; the view must have STORAGE usage")
    args = ap.parse_args()

    if not shutil.which("spirv-dis") or not shutil.which("spirv-as"):
        sys.exit("spirv-dis/spirv-as not found (install spirv-tools)")

    tmpdir = tempfile.mkdtemp(prefix="radv-mlfi-fix-")
    asm_in = os.path.join(tmpdir, "in.spvasm")
    asm_out = os.path.join(tmpdir, "out.spvasm")
    disassemble(args.inp, asm_in)
    header, body = parse(open(asm_in).read())

    kinds = scalar_kind(body)

    # ---- 1. Collect image types and their legal replacement. -----------------
    image_decl = {}  # id -> (sampled_type_id, dim, depth, arrayed, ms, sampled, fmt)
    for ins in body:
        if ins.opcode == "OpTypeImage":
            t = ins.tokens()
            image_decl[ins.result] = (t[0].lstrip("%"), *t[1:])
    image_order = [ins.result for ins in body if ins.opcode == "OpTypeImage"]

    # Values: id -> type id, for code results (OpLoad, OpIAdd, ...).
    value_type = {}
    for ins in body:
        if ins.result is None or ins.opcode is None:
            continue
        if ins.opcode.startswith("OpType") or ins.opcode in UNTYPED_OPS:
            continue
        t = ins.tokens()
        if t:
            value_type[ins.result] = t[0].lstrip("%")

    # Replace a 16-bit sampled type with the 32-bit equivalent.
    def wide_type(comp_id):
        kind = kinds.get(comp_id)
        if kind is None:
            return None
        if kind[0] == "float" and kind[1] == 16:
            return find_or_add_type(body, "OpTypeFloat", ["32"])
        if kind[0] == "int" and kind[1] == 16:
            signed = kind[2] if len(kind) > 2 else False
            return find_or_add_type(body, "OpTypeInt", ["32", "1" if signed else "0"])
        return None

    rewrites = {}  # image type id -> (old component id, new component id)
    for image_id, decl in list(image_decl.items()):
        comp = decl[0]
        new_comp = wide_type(comp)
        if new_comp and new_comp != comp:
            rewrites[image_id] = (comp, new_comp)

    # 64-bit integer sampled images: Mesa's glsl type system has no texture type for them
    # (vtn_fail "glsl_type_is_texture").  A fetch at an explicit lod of a raw integer image is a
    # storage read, so turn it into one.  The caller must create the view with STORAGE usage.
    to_storage = {}  # image type id -> (new sampled, new fmt)
    if args.i64_sampled_to_storage:
        for image_id, decl in image_decl.items():
            comp = decl[0]
            kind = kinds.get(comp)
            sampled = int(decl[5])
            fmt = FORMAT_IDS.get(decl[6])
            if kind and kind[0] == "int" and kind[1] == 64 and sampled == 1 and fmt == 0:
                to_storage[image_id] = ("2", "R64i" if kind[2] else "R64ui")

    if not rewrites and not to_storage:
        print("no rewritable image types found; copying input unchanged")
        shutil.copyfile(args.inp, args.out)
        return

    # ---- 2. Change OpTypeImage in place, deduplicating identical declarations. ----
    # Redirects: old type id -> canonical type id (both for the image and for the
    # pointer / sampled-image wrappers that become identical after the rewrite).
    redirect = {}

    def canon(tid):
        seen = set()
        while tid in redirect and tid not in seen:
            seen.add(tid)
            tid = redirect[tid]
        return tid

    def image_key(decl):
        comp, *rest = decl
        return (canon(comp), tuple(rest))

    # Rename the component id of each rewritten image, then look for a twin.
    for image_id, (old, new) in rewrites.items():
        decl = list(image_decl[image_id])
        decl[0] = new
        image_decl[image_id] = tuple(decl)

    for image_id, (sampled, fmt) in to_storage.items():
        decl = list(image_decl[image_id])
        decl[5] = sampled
        decl[6] = fmt
        image_decl[image_id] = tuple(decl)

    for image_id in [i for i in image_order if i in rewrites or i in to_storage]:
        mine = image_key(image_decl[image_id])
        earlier = image_order[: image_order.index(image_id)]
        for other in earlier:
            if other in redirect:
                continue
            if image_key(image_decl[other]) == mine:
                redirect[image_id] = other
                break
        else:
            # Keep this id; rewrite its declaration text.
            comp, *rest = image_decl[image_id]
            for ins in body:
                if ins.result == image_id and ins.opcode == "OpTypeImage":
                    ins.rest = " %" + comp + " " + " ".join(rest)
                    break

    # Pointers / sampled images that referenced a redirected id: recompute keys.
    for opcode in ("OpTypePointer", "OpTypeSampledImage"):
        order = [ins.result for ins in body if ins.opcode == opcode]
        keys = {}
        for tid in order:
            ins = next(i for i in body if i.result == tid)
            t = [x.lstrip("%") for x in ins.tokens()]
            if opcode == "OpTypePointer":
                t = (t[0], canon(t[1]))
            else:
                t = (canon(t[0]),)
            if t in keys:
                redirect[tid] = keys[t]
            else:
                keys[t] = tid
                ins.rest = (" " + t[0] + " %" + t[1]) if opcode == "OpTypePointer" else (" %" + t[0])

    # ---- 3. Fix the operations that consumed 16-bit image values. ------------
    # Map an image value id -> the type id of the image it holds.
    def image_of_value(value_id):
        tid = canon(value_type.get(value_id, ""))
        if tid in image_decl:
            return tid
        # Through OpSampledImage / OpImage: the result type is the sampled image
        # type, whose first operand is the image type.
        for ins in body:
            if ins.result == value_id and ins.opcode in ("OpSampledImage", "OpImage"):
                return image_of_value(ins.tokens()[0].lstrip("%"))
        return None

    new_lines = []
    fresh = 100000
    changed = 0
    for ins in list(body):
        if ins.opcode is None:
            new_lines.append(ins)
            continue
        toks = ins.tokens()

        if ins.opcode == "OpImageWrite" and len(toks) >= 3:
            image = image_of_value(toks[0].lstrip("%"))
            if image in rewrites:
                comp, new_comp = rewrites[image]
                val = toks[2].lstrip("%")
                vtype = value_type.get(val)
                # Width of the stored vector.
                width = 1
                for d in body:
                    if d.result == vtype and d.opcode == "OpTypeVector":
                        width = int(d.tokens()[1])
                new_vtype = vector_type(body, new_comp, width)
                kind = kinds.get(comp, ("float", 16))
                conv_op = "OpFConvert" if kind[0] == "float" else ("OpSConvert" if kind[-1] else "OpUConvert")
                fresh += 1
                tmp = f"%fix{fresh}"
                new_lines.append(Instruction(f"{tmp} = {conv_op} %{new_vtype} %{val}"))
                toks[2] = tmp
                ins.rest = " " + " ".join(toks)
                changed += 1

        elif ins.opcode in ("OpImageFetch", "OpImageSparseFetch") and len(toks) >= 4:
            image = image_of_value(toks[1].lstrip("%"))
            if image in to_storage:
                # OpImageFetch %T %img %coord Lod %lod [extra]  ->  OpImageRead %T %img %coord
                ins.opcode = "OpImageRead"
                ins.rest = " " + " ".join(toks[:3])
                changed += 1

        elif ins.result and ins.opcode.startswith(IMAGE_RESULT_PREFIXES):
            rtype = toks[0].lstrip("%")
            decl = next((d for d in body if d.result == rtype), None)
            if decl is not None and decl.opcode in ("OpTypeVector", "OpTypeFloat") :
                if decl.opcode == "OpTypeFloat" and kinds.get(rtype, ("", 0))[1] == 16:
                    new_rtype = find_or_add_type(body, "OpTypeFloat", ["32"])
                    width = 1
                elif decl.opcode == "OpTypeVector":
                    comp = decl.tokens()[0].lstrip("%")
                    width = int(decl.tokens()[1])
                    if kinds.get(comp, ("", 0))[1] != 16:
                        new_lines.append(ins)
                        continue
                    new_comp = wider_component(body, comp, kinds)
                    new_rtype = vector_type(body, new_comp, width)
                else:
                    new_lines.append(ins)
                    continue
                fresh += 1
                tmp = f"%fix{fresh}"
                new_lines.append(Instruction(f"{tmp} = {ins.opcode} %{new_rtype}" + " " + " ".join(toks[1:])))
                new_lines.append(Instruction(f"%{ins.result} = OpFConvert %{rtype} {tmp}"))
                changed += 1
                continue

        new_lines.append(ins)

    body = new_lines

    if EXTRA_TYPES:
        # SPIR-V requires a type to be declared before its first use, so splice the new
        # declarations in immediately before whatever instruction references them first (for the
        # 16-bit rewrite that is usually the OpTypeImage that was just re-typed).  EXTRA_TYPES is
        # in dependency order, which keeps a new OpTypeVector after its component type.
        new_ids = {i.result for i in EXTRA_TYPES}
        pos = len(body)
        for n, ins in enumerate(body):
            if any(tok.startswith("%") and tok[1:] in new_ids for tok in ins.rest.split()):
                pos = n
                break
        body[pos:pos] = EXTRA_TYPES
        EXTRA_TYPES.clear()

    # ---- 4. Apply redirects everywhere and drop the dead declarations. -------
    if redirect:
        def subst(text):
            def repl(m):
                tid = m.group(1)
                return "%" + canon(tid)
            return ID_RE.sub(repl, text)

        kept = []
        for ins in body:
            if ins.result and ins.result in redirect:
                continue  # dropped declaration
            if ins.opcode in ("OpName", "OpMemberName") and ins.tokens():
                target = ins.tokens()[0].lstrip("%")
                if target in redirect:
                    continue
            ins.rest = subst(ins.rest)
            kept.append(ins)
        body = kept

    if to_storage:
        have_cap = any(i.opcode == "OpCapability" and i.tokens() == ["Int64ImageEXT"] for i in body)
        have_ext = any(i.opcode == "OpExtension" and "SPV_EXT_shader_image_int64" in i.raw for i in body)
        last_cap = max((n for n, i in enumerate(body) if i.opcode == "OpCapability"), default=-1)
        inject = []
        if not have_cap:
            inject.append(Instruction("OpCapability Int64ImageEXT"))
        if not have_ext:
            inject.append(Instruction('OpExtension "SPV_EXT_shader_image_int64"'))
        body[last_cap + 1:last_cap + 1] = inject

    out_text = "\n".join(header) + "\n" + "\n".join(i.render() for i in body) + "\n"
    open(asm_out, "w").write(out_text)
    assemble(asm_out, args.out)
    if args.keep_asm:
        shutil.copyfile(asm_out, os.path.splitext(args.out)[0] + ".spvasm")
    print(f"rewrote {len(rewrites)} 16-bit image type(s), {len(to_storage)} 64-bit image type(s), "
          f"{changed} operation(s); {len(redirect)} id(s) merged")
    print(f"output: {args.out}")


def wider_component(body, comp, kinds):
    kind = kinds.get(comp)
    if kind and kind[0] == "float":
        return find_or_add_type(body, "OpTypeFloat", ["32"])
    if kind and kind[0] == "int":
        signed = kind[2] if len(kind) > 2 else False
        return find_or_add_type(body, "OpTypeInt", ["32", "1" if signed else "0"])
    raise SystemExit(f"cannot widen component type %{comp}")


if __name__ == "__main__":
    main()
