"""Phase 1: extract and inventory a pinned private AFL library, not install it.

Requires a separate pristine jaames/mii-assets checkout and NumPy/Pillow.
The external extractor and the resource are not bundled with this project.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import runpy
import shutil
import struct
import sys

REVISION = 'c2811a81cad8be5330d8153ef3fc5b51e651e9e2'
RESOURCE_SHA256 = '4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722'
SOURCE_HASHES = {
    'fflExtract.py': '5930b609689c6bf977a34fa6f62b4a1506dc95d8cb761aa201d74b941eab964e',
    'ffl/FFLiResource.py': '2dad5a5c5645985499ebb0f0c468d4f63cb539d4fb7640f9f702cbcc11596ea0',
    'ffl/FFLiResourceShape.py': '4c033f9cfbaf930b11d9c23746dc4c70c6e2ff0868b3e2d4362d015110da3b0f',
    'ffl/FFLiResourceTexture.py': '9ab8dae9ab10c6a2dc2416203028448b765e8a2574f7fe3de01bd0d81d8fedfb',
    'utils/glbExporter.py': 'cc8a2da151b7d51339ad09b28525158ff16079dd955f77e3d1ba44dc4e3d57e9',
    'utils/textureImage.py': '9b6b0ac5ae6d5c66d7036cd96f07f7a10c1c748c9df4df6eead3df65ae57101f',
}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect_glb(path):
    """Check the simple single-mesh GLBs emitted by the pinned extractor."""
    data = Path(path).read_bytes()
    if len(data) < 28 or struct.unpack_from('<4sII', data) != (b'glTF', 2, len(data)):
        raise ValueError('Invalid GLB header')
    size, kind = struct.unpack_from('<I4s', data, 12)
    if kind != b'JSON' or size % 4 or 28 + size > len(data):
        raise ValueError('Invalid JSON chunk')
    doc = json.loads(data[20:20+size])
    length, kind = struct.unpack_from('<I4s', data, 20+size)
    binary = data[28+size:]
    if kind != b'BIN\0' or length != len(binary):
        raise ValueError('Invalid binary chunk')
    if len(doc['buffers']) != 1 or 'uri' in doc['buffers'][0]:
        raise ValueError('Expected embedded buffer')

    def values(index):
        a = doc['accessors'][index]
        v = doc['bufferViews'][a['bufferView']]
        fmt = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}[a['componentType']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        stride = struct.calcsize('<'+fmt*width)
        offset = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        needed = a['count'] * stride
        if (v.get('buffer', 0) != 0 or 'byteStride' in v or 'sparse' in a
                or offset < 0 or a['count'] < 0 or needed > v['byteLength']
                or offset + needed > len(binary)):
            raise ValueError('Accessor outside buffer')
        return list(struct.iter_unpack('<'+fmt*width, binary[offset:offset+needed]))

    if len(doc['meshes']) != 1 or len(doc['meshes'][0]['primitives']) != 1:
        raise ValueError('Expected a single mesh')
    primitive = doc['meshes'][0]['primitives'][0]
    if not primitive['attributes'] and not doc['accessors'] and not binary:
        return dict(vertices=0, triangles=0, attributes=[], bounds=[], empty=True)
    positions = values(primitive['attributes']['POSITION'])
    indices = values(primitive['indices'])
    if not positions or primitive.get('mode', 4) != 4 or len(indices) % 3:
        raise ValueError('Invalid triangles')
    if any(i[0] >= len(positions) for i in indices):
        raise ValueError('Index outside vertices')
    if any(not math.isfinite(x) for v in positions for x in v):
        raise ValueError('Nonfinite position')
    for key, index in primitive['attributes'].items():
        if len(values(index)) != len(positions):
            raise ValueError('Attribute count mismatch: '+key)
    return dict(vertices=len(positions), triangles=len(indices)//3,
                attributes=sorted(primitive['attributes']),
                bounds=[[min(v[i] for v in positions), max(v[i] for v in positions)]
                        for i in range(3)])


def extract(resource, extractor, output):
    resource, extractor, output = map(Path, (resource, extractor, output))
    if digest(resource) != RESOURCE_SHA256:
        raise ValueError('Unsupported resource; expected verified AFLResHigh_2_3.dat')
    for name, expected in SOURCE_HASHES.items():
        if digest(extractor/name) != expected:
            raise ValueError('Extractor changed: '+name)
    if {p.relative_to(extractor).as_posix() for p in extractor.rglob('*.py')} != set(SOURCE_HASHES):
        raise ValueError('Unexpected Python source in extractor directory')
    if shutil.disk_usage(output.parent).free < 4*1024**3:
        raise ValueError('At least 4 GiB free required')
    output.mkdir(exist_ok=False)
    import numpy as np
    from PIL import Image
    sys.path.insert(0, str(extractor.resolve()))
    from utils.textureImage import textureImage
    original_pixels, original_fromstring = textureImage.set_pixels, np.fromstring
    original_argv = sys.argv

    def pixels(self, values):
        if self.format == 2:
            self.image = Image.frombytes('RGBA', (self.shape[1], self.shape[0]), values.tobytes())
        else:
            original_pixels(self, values)

    # Preserve the old binary fromstring's writable-copy behavior on NumPy 2.
    np.fromstring = lambda *a, **k: np.frombuffer(*a, **k).copy()
    textureImage.set_pixels = pixels
    try:
        sys.argv = ['fflExtract.py', '-i', str(resource), '365', '859',
                    '-m', str(output/'meshes'), '-t', str(output/'textures')]
        runpy.run_path(str(extractor/'fflExtract.py'), run_name='__main__')
    finally:
        np.fromstring, textureImage.set_pixels = original_fromstring, original_pixels
        sys.argv = original_argv
        sys.path.pop(0)
    return inventory(output)


def inventory(output):
    from PIL import Image
    output = Path(output)
    items = []
    for path in sorted(output.glob('meshes/*.glb')):
        items.append(dict(path=path.relative_to(output).as_posix(), sha256=digest(path),
                          **inspect_glb(path)))
    for path in sorted(output.glob('textures/*.png')):
        with Image.open(path) as image:
            image.load()
            if image.width > 2048 or image.height > 2048:
                raise ValueError('Unexpected texture dimensions')
            items.append(dict(path=path.relative_to(output).as_posix(), sha256=digest(path),
                              width=image.width, height=image.height, mode=image.mode))
    meshes = sum('vertices' in item for item in items)
    if meshes != 586 or len(items)-meshes != 247:
        raise ValueError('Unexpected extraction count')
    manifest = dict(schema=1, resource_sha256=RESOURCE_SHA256, extractor_revision=REVISION,
                    meshes=meshes, textures=len(items)-meshes, items=items,
                    empty_meshes=sum(item.get('empty', False) for item in items),
                    editor_ready=False, native_integration_verified=False,
                    distribution_license_verified=False,
                    limitations=['Part IDs not mapped to editor styles',
                                 'Exported GLBs omit normals and materials',
                                 'Assembly/transforms/UV convention need validation'])
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    empty = manifest['empty_meshes']
    print(f'Validated {meshes-empty} nonempty meshes, {empty} empty entry, '
          f'and {len(items)-meshes} textures: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('resource', type=Path)
    parser.add_argument('extractor', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    extract(args.resource, args.extractor, args.output)
