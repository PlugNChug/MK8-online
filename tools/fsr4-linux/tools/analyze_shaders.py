#!/usr/bin/env python3
"""Analyze all extracted SPIR-V modules and build a catalog."""
import collections
import json
import sys
from pathlib import Path

import spv

CAP = {
    0: "Matrix", 1: "Shader", 2: "Geometry", 3: "Tessellation", 4: "Addresses",
    5: "Linkage", 6: "Kernel", 7: "Vector16", 8: "Float16Buffer", 9: "Float16",
    10: "Float64", 11: "Int64", 12: "Int64Atomics", 13: "ImageBasic",
    14: "ImageReadWrite", 15: "ImageMipmap", 17: "Pipes", 18: "Groups",
    19: "DeviceEnqueue", 20: "LiteralSampler", 21: "AtomicStorage", 22: "Int16",
    23: "TessellationPointSize", 24: "GeometryPointSize", 25: "ImageGatherExtended",
    27: "StorageImageMultisample", 28: "UniformBufferArrayDynamicIndexing",
    29: "SampledImageArrayDynamicIndexing", 30: "StorageBufferArrayDynamicIndexing",
    31: "StorageImageArrayDynamicIndexing", 32: "ClipDistance", 33: "CullDistance",
    34: "ImageCubeArray", 35: "SampleRateShading", 36: "ImageRect",
    37: "SampledRect", 38: "GenericPointer", 39: "Int8", 40: "InputAttachment",
    41: "SparseResidency", 42: "MinLod", 43: "Sampled1D", 44: "Image1D",
    45: "SampledCubeArray", 46: "SampledBuffer", 47: "ImageBuffer",
    48: "ImageMSArray", 49: "StorageImageExtendedFormats", 50: "ImageQuery",
    51: "DerivativeControl", 52: "InterpolationFunction", 53: "TransformFeedback",
    54: "GeometryStreams", 55: "StorageImageReadWithoutFormat",
    56: "StorageImageWriteWithoutFormat", 57: "MultiViewport", 58: "SubgroupDispatch",
    59: "NamedBarrier", 60: "PipeStorage", 61: "GroupNonUniform",
    62: "GroupNonUniformVote", 63: "GroupNonUniformArithmetic",
    64: "GroupNonUniformBallot", 65: "GroupNonUniformShuffle",
    66: "GroupNonUniformShuffleRelative", 67: "GroupNonUniformClustered",
    68: "GroupNonUniformQuad", 69: "ShaderLayer", 70: "ShaderViewportIndex",
    71: "UniformDecoration", 4423: "SubgroupBallotKHR",
    4424: "DrawParameters", 4425: "WorkgroupMemoryExplicitLayoutKHR",
    4438: "SubgroupVoteKHR", 4445: "StorageBuffer16BitAccess",
    4446: "UniformAndStorageBuffer16BitAccess", 4447: "StoragePushConstant16",
    4448: "StorageInputOutput16", 4451: "DeviceGroup", 4453: "MultiView",
    4461: "VariablePointersStorageBuffer", 4462: "VariablePointers",
    4472: "AtomicStorageOps", 4478: "SampleMaskPostDepthCoverage",
    4479: "StorageBuffer8BitAccess", 4480: "UniformAndStorageBuffer8BitAccess",
    4481: "StoragePushConstant8", 4487: "DenormPreserve", 4488: "DenormFlushToZero",
    4489: "SignedZeroInfNanPreserve", 4490: "RoundingModeRTE",
    4491: "RoundingModeRTZ", 4494: "RayQueryProvisionalKHR", 4471: "RayQueryKHR",
    4433: "RayTraced?",
    6016: "ShaderClockKHR", 6018: "Int8?", 6019: "Int8Packed?",
    6020: "Float8?",
    # Vulkan/SPIR-V 1.4+ / vendor
    4421: "StencilExportEXT",
    5263: "MeshShadingNV",
    5281: "FragmentShaderPixelInterlockEXT",
    5347: "ShaderNonUniform",
    5349: "RuntimeDescriptorArray",
    5350: "RayTracing?",
    5364: "MeshShadingEXT",
    5386: "FragmentShadingRateKHR",
    5390: "ShaderTerminateInvocation",
    5586: "GroupNonUniformQuad?",
    6090: "IntegerDotProduct?",
    6091: "DotProductInputAll?",
    6094: "DotProductInput4x8BitPacked",
    6019: "",
    6021: "",
    6022: "",
    6023: "",
}


def cap_name(c):
    if c in CAP and CAP[c]:
        return CAP[c]
    if c == 6019:
        return "DotProductInput4x8BitPacked? or Int8CooperativeMatrix?"
    return f"Capability{c}"


def classify(name):
    """Map an OpEntryPoint name to a pass family + variant attributes."""
    n = name
    fam = "unknown"
    if "rcas" in n:
        fam = "rcas"
    elif "exposure" in n:
        fam = "auto_exposure"
    elif "wmma" in n:
        fam = "wmma"
    elif "prepass" in n:
        fam = "prepass"
    elif "postpass" in n:
        fam = "postpass"
    elif "pass" in n:
        fam = "pass"
    return fam


def main(root):
    root = Path(root)
    index = json.loads((root / "index.json").read_text())
    rows = []
    for b in index["blobs"]:
        path = root / b["file"]
        data = path.read_bytes()
        try:
            m = spv.Module(data)
        except Exception as exc:  # noqa: BLE001
            rows.append({"id": b["id"], "file": b["file"], "error": str(exc)})
            continue
        eps = m.entry_points
        rows.append({
            "id": b["id"],
            "file": b["file"],
            "file_offset": b["file_offset"],
            "rva": b["rva"],
            "size": b["uncompressed_size"],
            "sha256": b["sha256"],
            "version": f"{m.version >> 16}.{(m.version >> 8) & 0xFF}",
            "generator": m.generator,
            "entries": eps,
            "entry_names": [e["name"] for e in eps],
            "entry_models": sorted({e["model"] for e in eps}),
            "capabilities": sorted({cap_name(c) for c in m.capabilities}),
            "capability_ids": sorted(set(m.capabilities)),
            "extensions": sorted(set(m.extensions)),
            "ext_imports": sorted(set(m.ext_imports)),
            "execution_modes": m.execution_modes,
            "bindings": m.descriptor_bindings(),
            "push_constants": m.push_constants(),
            "spec_constants": m.spec_constants,
            "names": sorted({v for k, v in m.names.items() if k in m.variables}),
        })
    (root / "catalog_raw.json").write_text(json.dumps(rows, indent=1) + "\n")
    print("modules:", len(rows))
    print("errors:", sum(1 for r in rows if "error" in r))
    fams = collections.Counter()
    for r in rows:
        for n in r.get("entry_names", []):
            fams[n] += 1
    print("unique entry point names:", len(fams))
    for n, c in fams.most_common(40):
        print(f"  {c:5d} {n}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
