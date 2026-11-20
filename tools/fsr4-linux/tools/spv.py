#!/usr/bin/env python3
"""Minimal SPIR-V binary parser: entry points, capabilities, extensions,
descriptor bindings, push-constant layout, specialization constants."""
import struct
import sys

SPV_MAGIC = 0x07230203

OP = {
    5: "OpName", 6: "OpMemberName", 10: "OpExtension", 11: "OpExtInstImport",
    12: "OpExtInst", 15: "OpEntryPoint", 16: "OpExecutionMode", 17: "OpCapability",
    19: "OpTypeVoid", 21: "OpTypeInt", 22: "OpTypeFloat", 23: "OpTypeVector",
    28: "OpTypeArray", 29: "OpTypeRuntimeArray", 30: "OpTypeStruct",
    32: "OpTypePointer", 33: "OpTypeFunction", 48: "OpSpecConstantTrue",
    49: "OpSpecConstantFalse", 50: "OpSpecConstant", 51: "OpSpecConstantComposite",
    52: "OpSpecConstantOp", 59: "OpVariable", 71: "OpDecorate", 72: "OpMemberDecorate",
    332: "OpDecorateId",
}

DECOR = {
    1: "SpecId", 2: "Block", 3: "BufferBlock", 5: "RowMajor", 6: "ArrayStride",
    11: "BuiltIn", 20: "NoPerspective", 22: "Flat", 23: "Patch", 24: "NonWritable",
    25: "NonReadable", 30: "Location", 33: "Binding", 34: "DescriptorSet",
    35: "Offset", 36: "XfbBuffer", 37: "XfbStride", 44: "NoContraction",
}

EXEC_MODEL = {
    0: "Vertex", 1: "TessellationControl", 2: "TessellationEvaluation", 3: "Geometry",
    4: "Fragment", 5: "GLCompute", 6: "Kernel", 5267: "TaskNV", 5268: "MeshNV",
    5313: "RayGenerationKHR", 5314: "IntersectionKHR", 5315: "AnyHitKHR",
    5316: "ClosestHitKHR", 5317: "MissKHR", 5318: "CallableKHR",
    5364: "TaskEXT", 5365: "MeshEXT",
}

EXEC_MODE = {17: "LocalSize", 18: "LocalSizeHint", 26: "LocalSizeId", 38: "SubgroupSize"}

STORAGE = {
    0: "UniformConstant", 1: "Input", 2: "Uniform", 3: "Output", 4: "Workgroup",
    5: "CrossWorkgroup", 6: "Private", 7: "Function", 8: "Generic", 9: "PushConstant",
    10: "AtomicCounter", 11: "Image", 12: "StorageBuffer",
}

SCALAR = {21: "int", 22: "float"}


class Module:
    def __init__(self, data):
        self.data = data
        self.magic, self.version, self.generator, self.bound, self.schema = \
            struct.unpack_from("<5I", data, 0)
        if self.magic != SPV_MAGIC:
            raise ValueError("not SPIR-V")
        self.names = {}
        self.member_names = {}
        self.entry_points = []
        self.capabilities = []
        self.extensions = []
        self.ext_imports = []
        self.decorations = []      # (target, decoration, operands)
        self.member_decorations = []
        self.types = {}            # id -> (opcode, operands)
        self.variables = {}        # id -> (storage class, pointee type)
        self.spec_constants = []   # dict
        self.execution_modes = []
        self._parse()

    def _parse(self):
        data = self.data
        n = len(data) // 4
        words = struct.unpack_from(f"<{n}I", data, 0)
        i = 5
        while i < n:
            w = words[i]
            wc = w >> 16
            op = w & 0xFFFF
            if wc == 0:
                raise ValueError(f"zero word count at word {i}")
            ops = words[i + 1:i + wc]
            self._dispatch(op, ops)
            i += wc

    def _dispatch(self, op, a):
        if op == 5 and len(a) >= 2:
            self.names[a[0]] = self._str(a[1:])
        elif op == 6 and len(a) >= 3:
            self.member_names.setdefault(a[0], {})[a[1]] = self._str(a[2:])
        elif op == 10:
            self.extensions.append(self._str(a))
        elif op == 11:
            self.ext_imports.append(self._str(a[1:]))
        elif op == 15 and len(a) >= 3:
            name, used = self._str_used(a[2:])
            self.entry_points.append({
                "model": EXEC_MODEL.get(a[0], f"model{a[0]}"),
                "id": a[1],
                "name": name,
                "interface": list(a[2 + used:]),
            })
        elif op == 16 and len(a) >= 2:
            self.execution_modes.append({
                "entry": a[0],
                "mode": EXEC_MODE.get(a[1], f"mode{a[1]}"),
                "literals": list(a[2:]),
            })
        elif op == 17 and a:
            self.capabilities.append(a[0])
        elif op == 30:
            self.types[a[0]] = ("struct", list(a[1:]))
        elif op == 20:
            self.types[a[0]] = ("bool", [])
        elif op == 28:
            self.types[a[0]] = ("array", list(a[1:]))
        elif op == 29:
            self.types[a[0]] = ("runtime_array", list(a[1:]))
        elif op == 25 and len(a) >= 8:
            self.types[a[0]] = ("image", list(a[1:]))
        elif op == 26:
            self.types[a[0]] = ("sampler", [])
        elif op == 27:
            self.types[a[0]] = ("sampled_image", list(a[1:]))
        elif op in (21, 22):
            self.types[a[0]] = (SCALAR[op], list(a[1:]))
        elif op == 23:
            self.types[a[0]] = ("vector", list(a[1:]))
        elif op == 32:
            self.types[a[0]] = ("pointer", list(a[1:]))
        elif op in (48, 49):
            self.spec_constants.append({"id": a[0], "kind": OP[op],
                                        "type": a[1], "value": int(op == 48)})
        elif op == 50:
            self.spec_constants.append({"id": a[0], "kind": "OpSpecConstant",
                                        "type": a[1], "value": a[2] if len(a) > 2 else None})
        elif op in (51, 52):
            self.spec_constants.append({"id": a[0], "kind": OP[op],
                                        "type": a[1], "operands": list(a[2:])})
        elif op == 59 and len(a) >= 3:
            self.variables[a[1]] = (a[2], a[0])
        elif op == 71 and len(a) >= 2:
            self.decorations.append((a[0], a[1], list(a[2:])))
        elif op == 72 and len(a) >= 3:
            self.member_decorations.append((a[0], a[1], a[2], list(a[3:])))
        elif op == 332 and len(a) >= 2:
            self.decorations.append((a[0], a[1], list(a[2:])))

    @staticmethod
    def _str(words):
        return Module._str_used(words)[0]

    @staticmethod
    def _str_used(words):
        b = struct.pack(f"<{len(words)}I", *words)
        head, _, _ = b.partition(b"\0")
        used = (len(head) + 1 + 3) // 4
        return head.decode("utf-8", "replace"), used

    # -- derived views -------------------------------------------------
    def descriptor_bindings(self):
        set_of = {}
        bind_of = {}
        for target, dec, ops in self.decorations:
            if dec == 34 and ops:
                set_of[target] = ops[0]
            elif dec == 33 and ops:
                bind_of[target] = ops[0]
        out = []
        for vid, (storage, ptr_type) in self.variables.items():
            if storage not in (0, 2, 12):
                continue
            if vid not in bind_of:
                continue
            type_name, elem = self._pointee_name(ptr_type)
            pointee = self.types[ptr_type][1][1] if ptr_type in self.types else None
            count = 1
            t = self.types.get(pointee)
            if t and t[0] == "array" and len(t[1]) > 1:
                cnt_id = t[1][1]
                count = next((c["value"] for c in self.spec_constants
                              if c["id"] == cnt_id and c.get("value") is not None), 0)
            elif t and t[0] == "runtime_array":
                count = 0
            out.append({
                "count": count,
                "id": vid,
                "name": self.names.get(vid, f"%{vid}"),
                "set": set_of.get(vid),
                "binding": bind_of.get(vid),
                "storage_class": STORAGE.get(storage, str(storage)),
                "type": type_name,
                "element": elem,
            })
        out.sort(key=lambda d: (d["set"] if d["set"] is not None else -1,
                                d["binding"] if d["binding"] is not None else -1))
        return out

    def _pointee_name(self, ptr_type):
        t = self.types.get(ptr_type)
        if not t or t[0] != "pointer":
            return None, None
        pointee = t[1][1]
        while self.types.get(pointee, ("",))[0] in ("array", "runtime_array"):
            pointee = self.types[pointee][1][0]
        pt = self.types.get(pointee)
        elem = None
        if pt and pt[0] == "struct":
            elem = [self.member_names.get(pointee, {}).get(i, f"m{i}")
                    for i in range(len(pt[1]))]
            return "struct", elem
        if pt and pt[0] == "vector":
            return "vector", pt[1]
        if pt:
            return pt[0], pt[1]
        return None, None

    def push_constants(self):
        blocks = {t for t, dec, ops in self.decorations if dec == 2}
        out = []
        for vid, (storage, ptr_type) in self.variables.items():
            if storage != 9:
                continue
            t = self.types.get(ptr_type)
            if not t or t[0] != "pointer":
                continue
            st = t[1][1]
            members = self.types.get(st, ("?", []))[1]
            offsets = {}
            for target, idx, dec, ops in self.member_decorations:
                if target == st and dec == 35 and ops:
                    offsets[idx] = ops[0]
            out.append({
                "id": vid,
                "name": self.names.get(vid, f"%{vid}"),
                "type_id": st,
                "members": [
                    {"index": i,
                     "name": self.member_names.get(st, {}).get(i, f"m{i}"),
                     "offset": offsets.get(i),
                     "type": self._type_str(members[i]) if i < len(members) else None}
                    for i in range(len(members))
                ],
            })
        return out

    def _type_str(self, tid):
        t = self.types.get(tid)
        if not t:
            return f"%{tid}"
        kind, ops = t
        if kind == "int":
            return f"int{ops[0] if ops else '?'}"
        if kind == "float":
            return f"float{ops[0] if ops else '?'}"
        if kind == "vector":
            return f"vec{ops[1] if len(ops) > 1 else '?'}<{self._type_str(ops[0])}>"
        if kind == "pointer":
            return f"ptr({STORAGE.get(ops[0], ops[0])}, {self._type_str(ops[1])})"
        if kind == "struct":
            return "struct{" + ", ".join(self._type_str(m) for m in ops) + "}"
        return kind


def load(path):
    with open(path, "rb") as f:
        return Module(f.read())


if __name__ == "__main__":
    m = load(sys.argv[1])
    print(m.entry_points)
    print("caps", m.capabilities)
    print("ext", m.extensions)
    print("bindings", m.descriptor_bindings())
    print("push", m.push_constants())
