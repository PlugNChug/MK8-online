# Provider fixes needed before the harness can be green

The harness found four provider-side defects that prevent the real frame-generation path from working.
They are documented with evidence in [`docs/FG-HARNESS.md`](../../docs/FG-HARNESS.md). This file keeps
the minimal changes in one place so the provider/toolchain owner can apply them; **none of them are
applied to this tree** (the harness only owns `tools/fg-harness/`).

## 1. Reflect Vulkan `Uniform`/`StorageBuffer` variables in `tools/gen_fg_blobs.py`

`classify()` only walks `OpVariable ... UniformConstant`, which misses every constant buffer and
buffer SRV/UAV. Replace the pointer/type walk with:

```python
ptr_target = {}
for tid, (op, rest) in types.items():
    if op == "OpTypePointer":
        toks = rest.split()
        if toks:
            ptr_target[tid] = (toks[0], toks[1].lstrip("%"))

nonwritable = set(re.findall(r"OpDecorate %(\w+) NonWritable", text))
readonly_structs = set(re.findall(r"OpMemberDecorate %(\w+) \d+ NonWritable", text))
block_decorated = set(re.findall(r"OpDecorate %(\w+) Block", text))

for m in re.finditer(r"^\s*%(\w+) = OpVariable %(\w+) (\w+)", text, re.M):
    var, ptr, storage = m.group(1), m.group(2), m.group(3)
    if storage not in ("UniformConstant", "Uniform", "StorageBuffer"):
        continue
    target = ptr_target.get(ptr)
    if target is None or target[1] not in types:
        continue
    pointee = target[1]
    op, rest = types[pointee]
    if op == "OpTypeImage":
        sampled = int(rest.split()[5]) if len(rest.split()) > 5 else 1
        kind = "srv_tex" if sampled == 1 else "uav_tex"
    elif op == "OpTypeSampler":
        kind = "sampler"
    elif op == "OpTypeStruct":
        members = [t.lstrip("%") for t in rest.split()]
        has_runtime_array = any(types.get(t, ("", ""))[0] == "OpTypeRuntimeArray" for t in members)
        is_buffer = has_runtime_array or storage == "StorageBuffer" or pointee not in block_decorated
        if is_buffer:
            kind = "srv_buf" if (var in nonwritable or pointee in readonly_structs) else "uav_buf"
        else:
            kind = "cbv"
    else:
        raise RuntimeError(f"unclassified descriptor {var} ({op})")
    ...
```

The rest of the binding rewrite and table emission can stay as it is. The important results:
`cbFI` becomes a CBV at 128, `rw_counters` a UAV buffer at 224, and `r_counters` an SRV buffer.

## 2. Write four components to the interpolation output

`ffx_frameinterpolation_callbacks_hlsl.h`, in the branch with
`FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_MASK && FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT`:

```hlsl
RWTexture2D<FfxFloat32x4> rw_output          FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT);
RWTexture2D<FfxFloat32>   rw_inpainting_mask FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_MASK);

FfxFloat32x4 RWLoadFrameinterpolationOutput(FfxInt32x2 iPxPos)
{
    return rw_output[iPxPos];
}

void StoreFrameinterpolationOutput(FfxInt32x2 iPxPos, FfxFloat32x4 val)
{
    rw_output[iPxPos] = val;
    rw_inpainting_mask[iPxPos] = val.a;
}
```

Without this, validation reports
`VK_FORMAT_B8G8R8A8_UNORM ... has 4 components, but the OpImageWrite Texel operand only contains 3
components`. D3D12 accepts the three-component write; Vulkan does not.

## 3. Remove the duplicate `frameRenderedSemaphores` creation

`FrameInterpolationSwapchainVK::init` creates the per-image present semaphores twice: once before the
real swapchain image count is known and once after. The first set is overwritten and leaks at
`vkDestroyDevice` (`VkDevice ... has 4 leaked objects ... AMD FSR FrameRenderedSemaphore 0..3`).
Remove the first loop (the one named `"AMD FSR FrameRenderedSemaphore %d"`); keep the loop after
`vkGetSwapchainImagesKHR` that names them `"AMD swapchain image semaphore %d"`.

## 4. (Next) match the storage-image formats

The following remain validation warnings, not errors:

- `rw_dilated_motion_vectors`: shader `Rg32f`, SDK resource `R16G16_SFLOAT`;
- `rw_optical_flow_input` and pyramid levels: shader `R32ui`, SDK resource `R8_UINT`.

Either allocate the SDK resources with the formats DXC inferred, or emit `Unknown` for those storage
images. `Unknown` needs `shaderStorageImageReadWithoutFormat`/`shaderStorageImageWriteWithoutFormat`
enabled, and must not be used for images accessed through `OpImageTexelPointer` (the SCD histogram
requires a concrete `R32ui`).

## Rebuild and check (scratch copy)

The evidence run used a copy of the build tree and the prepared SDK under
`~/.cache/mk8online/fg-harness-scratch`, with the generated blobs, the HLSL header and the swapchain
source patched as above:

```sh
python3 gen_fg_blobs.py --sdk <prepared-SDK> --dxc <dxc> --out blobfix/fg_blobs.cpp --tmp blobgen
cp blobfix/fg_blob_*.cpp src/ && cp blobfix/fg_blobs.cpp src/
FG_SDK=<prepared-SDK> cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
nice -n 15 ninja -C build -j 4
```

Then:

```sh
MK8_FG_HARNESS_LIB=$HOME/.cache/mk8online/fg-harness-scratch/build/libamd_fidelityfx_framegeneration_vk.so \
  MK8_FG_HARNESS_REQUIRE_PROVIDER=1 python3 -m unittest tests.test_fg_harness -v
```
