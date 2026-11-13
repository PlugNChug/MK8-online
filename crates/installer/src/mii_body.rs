//! MK8's own Mii body (`romfs/UI3D/MiiBodyMiddle`, the lobby and MKTV body) for the launcher's Mii creator preview.
//! Read from the user's extracted game on request and returned as a GLB; nothing derived is written to disk.
//! Only the verified MK8 4.0.1 file is accepted, and every offset, count and format is checked before use.
use mk8_manifest::Result;
use sha2::{Digest, Sha256};
use std::{fs, path::Path};

pub const RELATIVE: &str = "romfs/UI3D/MiiBodyMiddle/MiiBodyMiddle.bfres";
const SHA256: &str = "d7bc2730168dba59e6e9567af703db19b66bdec3e19b51d9087ea78047d93208";
const MODELS: [&str; 2] = ["MaleBody", "FemaleBody"];

struct Reader<'a>(&'a [u8]);

impl Reader<'_> {
    fn bytes(&self, at: u64, n: u64) -> Result<&[u8]> {
        let end = at
            .checked_add(n)
            .ok_or("Corpo Mii: deslocamento inválido")?;
        if end > self.0.len() as u64 {
            return Err("Corpo Mii: dado fora do arquivo".into());
        }
        Ok(&self.0[at as usize..end as usize])
    }
    fn u8(&self, at: u64) -> Result<u8> {
        Ok(self.bytes(at, 1)?[0])
    }
    fn u16(&self, at: u64) -> Result<u16> {
        Ok(u16::from_le_bytes(self.bytes(at, 2)?.try_into().unwrap()))
    }
    fn i16(&self, at: u64) -> Result<i16> {
        Ok(self.u16(at)? as i16)
    }
    fn u32(&self, at: u64) -> Result<u32> {
        Ok(u32::from_le_bytes(self.bytes(at, 4)?.try_into().unwrap()))
    }
    fn u64(&self, at: u64) -> Result<u64> {
        Ok(u64::from_le_bytes(self.bytes(at, 8)?.try_into().unwrap()))
    }
    fn f32(&self, at: u64) -> Result<f32> {
        let v = f32::from_bits(self.u32(at)?);
        if !v.is_finite() || v.abs() > 100.0 {
            return Err("Corpo Mii: valor fora do esperado".into());
        }
        Ok(v)
    }
    /// BFRES/BNTX strings: u16 length, then the characters.
    fn string(&self, at: u64) -> Result<String> {
        let n = self.u16(at)? as u64;
        String::from_utf8(self.bytes(at + 2, n)?.to_vec())
            .map_err(|_| "Corpo Mii: nome inválido".into())
    }
    fn magic(&self, at: u64, magic: &[u8]) -> Result<()> {
        if self.bytes(at, magic.len() as u64)? != magic {
            return Err(format!(
                "Corpo Mii: bloco {} ausente",
                String::from_utf8_lossy(magic)
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Default)]
struct Part {
    material: String,
    positions: Vec<[f32; 3]>,
    normals: Vec<[f32; 3]>,
    uv: Vec<[f32; 2]>,
    indices: Vec<u16>,
}

#[derive(Debug)]
struct Model {
    name: String,
    head: [f32; 3],
    parts: Vec<Part>,
}

/// The GLB of both bodies (nodes `MaleBody` and `FemaleBody`, each with its `head` bone position in `extras`).
pub fn glb(extracted: &Path) -> Result<Vec<u8>> {
    let data = fs::read(extracted.join(RELATIVE))
        .map_err(|e| format!("Corpo Mii do jogo não encontrado ({e}). Prepare o jogo primeiro."))?;
    let digest: String = Sha256::digest(&data)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    if digest != SHA256 {
        return Err("MiiBodyMiddle.bfres não corresponde ao MK8 4.0.1 verificado.".into());
    }
    let (models, albedo) = parse(&data)?;
    Ok(write_glb(&models, &albedo))
}

fn parse(data: &[u8]) -> Result<(Vec<Model>, Vec<u8>)> {
    let r = Reader(data);
    r.magic(0, b"FRES    ")?;
    if r.u32(8)? != 0x0005_0003 {
        return Err("Corpo Mii: versão BFRES inesperada".into());
    }
    let model_array = r.u64(0x28)?;
    let model_count = r.u32(r.u64(0x30)? + 4)? as u64;
    let buffer_info = r.u64(0x90)?;
    let pool_size = r.u32(buffer_info + 4)? as u64;
    let pool = r.u64(buffer_info + 8)?;
    r.bytes(pool, pool_size)?;
    let mut models = Vec::new();
    for i in 0..model_count.min(8) {
        let m = model_array + i * 0x78;
        r.magic(m, b"FMDL")?;
        let name = r.string(r.u64(m + 0x10)?)?;
        if MODELS.contains(&name.as_str()) {
            models.push(model(&r, m, name, pool, pool + pool_size)?);
        }
    }
    if models.len() != MODELS.len() {
        return Err("Corpo Mii: MaleBody/FemaleBody ausentes".into());
    }
    Ok((models, albedo(&r)?))
}

fn model(r: &Reader, m: u64, name: String, pool: u64, pool_end: u64) -> Result<Model> {
    // Skeleton: every bone of these bodies has unit scale and no rotation, so a bone's world position is the sum
    // of its parents' translations. Anything else is rejected rather than drawn wrong.
    let skeleton = r.u64(m + 0x20)?;
    r.magic(skeleton, b"FSKL")?;
    let bones = r.u64(skeleton + 0x18)?;
    let matrix_to_bone = r.u64(skeleton + 0x20)?;
    let bone_count = r.u16(skeleton + 0x3c)? as u64;
    let matrices = (r.u16(skeleton + 0x3e)? + r.u16(skeleton + 0x40)?) as u64;
    if bone_count == 0 || bone_count > 64 {
        return Err("Corpo Mii: esqueleto inesperado".into());
    }
    let mut world: Vec<[f32; 3]> = Vec::new();
    let mut head = None;
    for b in 0..bone_count {
        let bone = bones + b * 0x50;
        let parent = r.i16(bone + 0x1a)?;
        let scale = [
            r.f32(bone + 0x28)?,
            r.f32(bone + 0x2c)?,
            r.f32(bone + 0x30)?,
        ];
        let rotation = [
            r.f32(bone + 0x34)?,
            r.f32(bone + 0x38)?,
            r.f32(bone + 0x3c)?,
            r.f32(bone + 0x40)?,
        ];
        if scale != [1.0; 3] || rotation != [0.0, 0.0, 0.0, 1.0] {
            return Err("Corpo Mii: osso com rotação/escala não suportada".into());
        }
        let mut position = [
            r.f32(bone + 0x44)?,
            r.f32(bone + 0x48)?,
            r.f32(bone + 0x4c)?,
        ];
        if parent >= 0 {
            let p = world
                .get(parent as usize)
                .ok_or("Corpo Mii: hierarquia de ossos inválida")?;
            position = [position[0] + p[0], position[1] + p[1], position[2] + p[2]];
        } else if b != 0 {
            return Err("Corpo Mii: mais de uma raiz no esqueleto".into());
        }
        if r.string(r.u64(bone)?)? == "head" {
            head = Some(position);
        }
        world.push(position);
    }
    let head = head.ok_or("Corpo Mii: osso head ausente")?;
    let vertex_buffers = r.u64(m + 0x28)?;
    let shapes = r.u64(m + 0x30)?;
    let (vb_count, shape_count) = (r.u16(m + 0x68)? as u64, r.u16(m + 0x6a)? as u64);
    if shape_count == 0 || shape_count > 8 {
        return Err("Corpo Mii: número de partes inesperado".into());
    }
    let mut parts = Vec::new();
    for s in 0..shape_count {
        let shape = shapes + s * 0x70;
        r.magic(shape, b"FSHP")?;
        let shape_name = r.string(r.u64(shape + 0x10)?)?;
        let material = shape_name
            .split("__")
            .nth(1)
            .ok_or("Corpo Mii: nome de parte inesperado")?;
        let vb_index = r.u16(shape + 0x62)? as u64;
        let skin = r.u8(shape + 0x66)?;
        if vb_index >= vb_count || r.u8(shape + 0x67)? == 0 {
            return Err("Corpo Mii: parte sem malha".into());
        }
        let vb = vertex_buffers + vb_index * 0x60;
        r.magic(vb, b"FVTX")?;
        let count = r.u32(vb + 0x58)? as u64;
        if count == 0 || count > 20000 || r.u32(vb + 0x5c)? != skin as u32 {
            return Err("Corpo Mii: buffer de vértices inesperado".into());
        }
        // Buffers follow each other in the memory pool, 8-byte aligned, from the vertex buffer's pool offset.
        let (sizes, strides) = (r.u64(vb + 0x38)?, r.u64(vb + 0x40)?);
        let mut start = pool
            .checked_add_signed(r.u32(vb + 0x50)? as i32 as i64)
            .ok_or("Corpo Mii: deslocamento inválido")?;
        let mut buffers = Vec::new();
        for k in 0..r.u8(vb + 0x55)? as u64 {
            let (size, stride) = (
                r.u32(sizes + k * 16)? as u64,
                r.u32(strides + k * 16)? as u64,
            );
            if stride == 0 || size != stride * count || start + size > pool_end {
                return Err("Corpo Mii: tamanho de buffer inesperado".into());
            }
            buffers.push((start, stride));
            start += size.div_ceil(8) * 8;
        }
        let attributes = r.u64(vb + 0x10)?;
        let mut part = Part {
            material: material.into(),
            ..Default::default()
        };
        let mut rigid = vec![0usize; count as usize];
        for a in 0..r.u8(vb + 0x54)? as u64 {
            let attribute = attributes + a * 16;
            let name = r.string(r.u64(attribute)?)?;
            let format = r.u32(attribute + 8)?;
            let offset = r.u16(attribute + 12)? as u64;
            let &(base, stride) = buffers
                .get(r.u16(attribute + 14)? as usize)
                .ok_or("Corpo Mii: atributo sem buffer")?;
            let at = |v: u64| base + v * stride + offset;
            match (name.as_str(), format) {
                ("_p0", 0x1805) => {
                    for v in 0..count {
                        part.positions
                            .push([r.f32(at(v))?, r.f32(at(v) + 4)?, r.f32(at(v) + 8)?]);
                    }
                }
                ("_n0", 0x0e02) => {
                    for v in 0..count {
                        part.normals.push(normal(r.u32(at(v))?)?);
                    }
                }
                ("_u0", 0x1201) => {
                    for v in 0..count {
                        let (x, y) = (r.u16(at(v))?, r.u16(at(v) + 2)?);
                        part.uv.push([x as f32 / 65535.0, y as f32 / 65535.0]);
                    }
                }
                ("_i0", 0x0203) if skin == 1 => {
                    for (v, bone) in rigid.iter_mut().enumerate() {
                        let matrix = r.u8(at(v as u64))? as u64;
                        if matrix >= matrices {
                            return Err("Corpo Mii: índice de osso fora do esqueleto".into());
                        }
                        *bone = r.u16(matrix_to_bone + matrix * 2)? as usize;
                    }
                }
                ("_p0" | "_n0" | "_u0", _) => {
                    return Err(format!("Corpo Mii: formato {format:#x} de {name}"))
                }
                _ => {}
            }
        }
        if part.positions.len() != count as usize
            || part.normals.len() != part.positions.len()
            || part.uv.len() != part.positions.len()
        {
            return Err("Corpo Mii: atributos de vértice ausentes".into());
        }
        // Smoothly skinned vertices are stored in model space (bind pose); rigid ones relative to their bone.
        if skin <= 1 {
            let fixed = r.u16(shape + 0x60)? as usize;
            for (v, p) in part.positions.iter_mut().enumerate() {
                let bone = if skin == 1 { rigid[v] } else { fixed };
                let w = world.get(bone).ok_or("Corpo Mii: osso fora do esqueleto")?;
                *p = [p[0] + w[0], p[1] + w[1], p[2] + w[2]];
            }
        }
        let mesh = r.u64(shape + 0x20)?;
        if r.u32(mesh + 0x24)? != 3 || r.u32(mesh + 0x28)? != 1 || r.u32(mesh + 0x30)? != 0 {
            return Err("Corpo Mii: malha não é lista de triângulos u16".into());
        }
        let index_count = r.u32(mesh + 0x2c)? as u64;
        let index_start = pool + r.u32(mesh + 0x20)? as u64;
        if index_count == 0
            || !index_count.is_multiple_of(3)
            || index_start + index_count * 2 > pool_end
        {
            return Err("Corpo Mii: índices inválidos".into());
        }
        for i in 0..index_count {
            let index = r.u16(index_start + i * 2)?;
            if index as u64 >= count {
                return Err("Corpo Mii: índice fora dos vértices".into());
            }
            part.indices.push(index);
        }
        parts.push(part);
    }
    Ok(Model { name, head, parts })
}

/// nn::gfx 10_10_10_2 SNorm.
fn normal(packed: u32) -> Result<[f32; 3]> {
    let c = |shift: u32| {
        let v = ((packed >> shift) & 1023) as i32;
        let v = if v > 511 { v - 1024 } else { v };
        (v as f32 / 511.0).max(-1.0)
    };
    let n = [c(0), c(10), c(20)];
    let length = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
    if !(0.9..=1.1).contains(&length) {
        return Err("Corpo Mii: normal inválida".into());
    }
    Ok(n)
}

/// `Mii_Alb` (BC3 sRGB, 384x384) from the embedded BNTX, decoded to RGBA mip 0.
fn albedo(r: &Reader) -> Result<Vec<u8>> {
    let files = r.u64(0x98)?;
    let count = r.u32(r.u64(0xa0)? + 4)? as u64;
    for i in 0..count.min(8) {
        let (offset, size) = (r.u64(files + i * 16)?, r.u32(files + i * 16 + 8)? as u64);
        let file = r.bytes(offset, size)?;
        if !file.starts_with(b"BNTX") {
            continue;
        }
        let b = Reader(file);
        b.magic(0x20, b"NX  ")?;
        let (textures, n) = (b.u64(0x28)?, b.u32(0x24)? as u64);
        for t in 0..n.min(16) {
            let info = b.u64(textures + t * 8)?;
            b.magic(info, b"BRTI")?;
            if b.string(b.u64(info + 0x60)?)? != "Mii_Alb" {
                continue;
            }
            let (format, width, height) = (
                b.u32(info + 0x1c)?,
                b.u32(info + 0x24)?,
                b.u32(info + 0x28)?,
            );
            let block_height = b.u32(info + 0x34)?;
            if format != 0x1c06 || width != 384 || height != 384 || block_height > 5 {
                return Err("Corpo Mii: textura Mii_Alb inesperada".into());
            }
            let mip0 = b.u64(b.u64(info + 0x70)?)?;
            let (bw, bh) = (width / 4, height / 4);
            let swizzled_size =
                ((bw * 16).div_ceil(64) as u64 * 512 * (bh as u64).div_ceil(8 << block_height))
                    << block_height;
            let blocks = deswizzle(
                b.bytes(mip0, swizzled_size)?,
                bw as usize,
                bh as usize,
                16,
                block_height,
            );
            return Ok(decode_bc3(&blocks, width as usize, height as usize));
        }
    }
    Err("Corpo Mii: textura Mii_Alb ausente".into())
}

/// Tegra block-linear (GOB 64x8 bytes) to pitch-linear, as toolchain/mii_resource_nx.py `deswizzle`.
fn deswizzle(data: &[u8], width: usize, height: usize, bpp: usize, bh_log2: u32) -> Vec<u8> {
    let row = width * bpp;
    let width_gobs = row.div_ceil(64);
    let block = 512usize << bh_log2;
    let mut out = vec![0; row * height];
    for y in 0..height {
        let gob_y = y >> 3;
        let base = (gob_y >> bh_log2) * width_gobs * block + (gob_y & ((1 << bh_log2) - 1)) * 512;
        let yi = y & 7;
        for x in (0..row).step_by(16) {
            let xi = x & 63;
            let o = base
                + (x >> 6) * block
                + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4));
            let n = 16.min(row - x);
            out[y * row + x..y * row + x + n].copy_from_slice(&data[o..o + n]);
        }
    }
    out
}

fn decode_bc3(blocks: &[u8], width: usize, height: usize) -> Vec<u8> {
    let mut out = vec![0u8; width * height * 4];
    let bw = width / 4;
    for (i, block) in blocks.as_chunks::<16>().0.iter().enumerate() {
        let (bx, by) = (i % bw, i / bw);
        let alpha = bc4(&block[..8]);
        let colors = bc1(&block[8..]);
        for p in 0..16 {
            let o = ((by * 4 + p / 4) * width + bx * 4 + p % 4) * 4;
            out[o..o + 3].copy_from_slice(&colors[p]);
            out[o + 3] = alpha[p];
        }
    }
    out
}

fn bc1(block: &[u8]) -> [[u8; 3]; 16] {
    let rgb = |v: u16| {
        [
            ((v >> 11) & 31) as u32 * 255 / 31,
            ((v >> 5) & 63) as u32 * 255 / 63,
            (v & 31) as u32 * 255 / 31,
        ]
    };
    let (a, b) = (
        rgb(u16::from_le_bytes([block[0], block[1]])),
        rgb(u16::from_le_bytes([block[2], block[3]])),
    );
    let mix = |wa: u32, wb: u32| [0, 1, 2].map(|c| ((a[c] * wa + b[c] * wb) / (wa + wb)) as u8);
    // BC3's colour block always uses the four-colour mode.
    let palette = [a.map(|v| v as u8), b.map(|v| v as u8), mix(2, 1), mix(1, 2)];
    let bits = u32::from_le_bytes(block[4..8].try_into().unwrap());
    std::array::from_fn(|p| palette[(bits >> (2 * p) & 3) as usize])
}

fn bc4(block: &[u8]) -> [u8; 16] {
    let (a0, a1) = (block[0] as u32, block[1] as u32);
    let mut palette = [a0, a1, 0, 0, 0, 0, 0, 255];
    if a0 > a1 {
        for i in 0..6 {
            palette[i + 2] = ((6 - i as u32) * a0 + (i as u32 + 1) * a1) / 7;
        }
    } else {
        for i in 0..4 {
            palette[i + 2] = ((4 - i as u32) * a0 + (i as u32 + 1) * a1) / 5;
        }
    }
    let mut bits = [0u8; 8];
    bits[..6].copy_from_slice(&block[2..8]);
    let bits = u64::from_le_bytes(bits);
    std::array::from_fn(|p| palette[(bits >> (3 * p) & 7) as usize] as u8)
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xedb8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

/// RGBA8 PNG with stored (uncompressed) deflate blocks: no compression library is needed for a local preview.
fn png(rgba: &[u8], width: usize, height: usize) -> Vec<u8> {
    let mut raw = Vec::with_capacity((width * 4 + 1) * height);
    for row in rgba.chunks_exact(width * 4) {
        raw.push(0);
        raw.extend_from_slice(row);
    }
    let mut zlib = vec![0x78, 0x01];
    let chunks: Vec<&[u8]> = raw.chunks(65535).collect();
    for (i, chunk) in chunks.iter().enumerate() {
        zlib.push((i + 1 == chunks.len()) as u8);
        zlib.extend_from_slice(&(chunk.len() as u16).to_le_bytes());
        zlib.extend_from_slice(&(!(chunk.len() as u16)).to_le_bytes());
        zlib.extend_from_slice(chunk);
    }
    let (mut a, mut b) = (1u32, 0u32);
    for &byte in &raw {
        a = (a + byte as u32) % 65521;
        b = (b + a) % 65521;
    }
    zlib.extend_from_slice(&((b << 16) | a).to_be_bytes());
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut header = (width as u32).to_be_bytes().to_vec();
    header.extend_from_slice(&(height as u32).to_be_bytes());
    header.extend_from_slice(&[8, 6, 0, 0, 0]);
    for (kind, body) in [
        (&b"IHDR"[..], header),
        (b"IDAT", zlib),
        (b"IEND", Vec::new()),
    ] {
        out.extend_from_slice(&(body.len() as u32).to_be_bytes());
        let mut crc = kind.to_vec();
        crc.extend_from_slice(&body);
        out.extend_from_slice(&crc);
        out.extend_from_slice(&crc32(&crc).to_be_bytes());
    }
    out
}

struct Glb {
    bin: Vec<u8>,
    views: Vec<serde_json::Value>,
    accessors: Vec<serde_json::Value>,
}

impl Glb {
    fn view(&mut self, bytes: &[u8], target: Option<u32>) -> usize {
        while !self.bin.len().is_multiple_of(4) {
            self.bin.push(0);
        }
        let mut v =
            serde_json::json!({"buffer":0,"byteOffset":self.bin.len(),"byteLength":bytes.len()});
        if let Some(target) = target {
            v["target"] = target.into();
        }
        self.bin.extend_from_slice(bytes);
        self.views.push(v);
        self.views.len() - 1
    }
    fn accessor(
        &mut self,
        bytes: &[u8],
        kind: &str,
        component: u32,
        count: usize,
        target: u32,
    ) -> usize {
        let view = self.view(bytes, Some(target));
        self.accessors
            .push(serde_json::json!({"bufferView":view,"componentType":component,"count":count,"type":kind}));
        self.accessors.len() - 1
    }
}

fn write_glb(models: &[Model], albedo: &[u8]) -> Vec<u8> {
    use serde_json::json;
    let floats = |values: Vec<f32>| {
        values
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>()
    };
    let mut glb = Glb {
        bin: Vec::new(),
        views: Vec::new(),
        accessors: Vec::new(),
    };
    let image = glb.view(&png(albedo, 384, 384), None);
    let mut materials: Vec<String> = Vec::new();
    let mut meshes = Vec::new();
    let mut nodes = Vec::new();
    for model in models {
        let mut primitives = Vec::new();
        for part in &model.parts {
            let mut min = [f32::MAX; 3];
            let mut max = [f32::MIN; 3];
            for p in &part.positions {
                for c in 0..3 {
                    min[c] = min[c].min(p[c]);
                    max[c] = max[c].max(p[c]);
                }
            }
            let count = part.positions.len();
            let position =
                glb.accessor(&floats(part.positions.concat()), "VEC3", 5126, count, 34962);
            glb.accessors[position]["min"] = json!(min);
            glb.accessors[position]["max"] = json!(max);
            let normal = glb.accessor(&floats(part.normals.concat()), "VEC3", 5126, count, 34962);
            let uv = glb.accessor(&floats(part.uv.concat()), "VEC2", 5126, count, 34962);
            let indices: Vec<u8> = part.indices.iter().flat_map(|i| i.to_le_bytes()).collect();
            let index = glb.accessor(&indices, "SCALAR", 5123, part.indices.len(), 34963);
            let material = materials
                .iter()
                .position(|m| *m == part.material)
                .unwrap_or_else(|| {
                    materials.push(part.material.clone());
                    materials.len() - 1
                });
            primitives.push(
                json!({"attributes":{"POSITION":position,"NORMAL":normal,"TEXCOORD_0":uv},
                "indices":index,"material":material}),
            );
        }
        meshes.push(json!({"name":model.name,"primitives":primitives}));
        nodes.push(json!({"name":model.name,"mesh":meshes.len()-1,"extras":{"head":model.head}}));
    }
    let Glb {
        mut bin,
        views,
        accessors,
    } = glb;
    while bin.len() % 4 != 0 {
        bin.push(0);
    }
    let materials: Vec<_> = materials
        .iter()
        .map(|name| json!({"name":name,"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":0.8}}))
        .collect();
    let document = json!({
        "asset":{"version":"2.0","generator":"MK8-online mii_body (preview only; game asset, not redistributable)"},
        "scene":0,"scenes":[{"nodes":(0..nodes.len()).collect::<Vec<_>>()}],"nodes":nodes,"meshes":meshes,
        "materials":materials,"textures":[{"source":0}],"images":[{"bufferView":image,"mimeType":"image/png"}],
        "accessors":accessors,"bufferViews":views,"buffers":[{"byteLength":bin.len()}]
    });
    let mut text = document.to_string().into_bytes();
    while !text.len().is_multiple_of(4) {
        text.push(b' ');
    }
    let mut out = b"glTF".to_vec();
    out.extend_from_slice(&2u32.to_le_bytes());
    out.extend_from_slice(&((12 + 8 + text.len() + 8 + bin.len()) as u32).to_le_bytes());
    out.extend_from_slice(&(text.len() as u32).to_le_bytes());
    out.extend_from_slice(b"JSON");
    out.extend_from_slice(&text);
    out.extend_from_slice(&(bin.len() as u32).to_le_bytes());
    out.extend_from_slice(b"BIN\0");
    out.extend_from_slice(&bin);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_block_formats_and_packs_png() {
        // BC1 four-colour block: pure red and pure blue endpoints, texel 1 uses colour 1, texel 2 the 2/3 mix.
        let mut block = [0u8; 16];
        block[0] = 0xff; // BC4 alpha endpoints 255 / 0: index 0 = 255, index 1 = 0
        block[2] = 0b0000_1000; // texel 0 -> 0 (255), texel 1 -> 1 (0)
        block[8..10].copy_from_slice(&0xf800u16.to_le_bytes());
        block[10..12].copy_from_slice(&0x001fu16.to_le_bytes());
        block[12] = 0b0010_0100; // texel 0 -> 0, 1 -> 1, 2 -> 2
        let rgba = decode_bc3(&block, 4, 4);
        assert_eq!(&rgba[0..4], &[255, 0, 0, 255]);
        assert_eq!(&rgba[4..8], &[0, 0, 255, 0]);
        assert_eq!(&rgba[8..11], &[170, 0, 85]);
        assert!((normal(511 << 10).unwrap()[1] - 1.0).abs() < 1e-6);
        assert!(normal(0).is_err());
        // The deswizzle is the inverse of mii_resource_nx.py's swizzle for one GOB.
        let gob: Vec<u8> = (0..=255u8).cycle().take(512).collect();
        let linear = deswizzle(&gob, 4, 8, 16, 0);
        assert_eq!(&linear[16..32], &gob[32..48]); // x 16..32 of row 0 sits at GOB offset 32
        let image = png(&[1, 2, 3, 4, 5, 6, 7, 8], 2, 1);
        assert!(image.starts_with(b"\x89PNG\r\n\x1a\n"));
        assert_eq!(crc32(b"IEND"), 0xae42_6082);
    }

    #[test]
    fn rejects_other_files() {
        let folder = std::env::temp_dir().join(format!("mk8-body-{}", std::process::id()));
        let file = folder.join(RELATIVE);
        fs::create_dir_all(file.parent().unwrap()).unwrap();
        fs::write(&file, b"FRES    not the verified body").unwrap();
        assert!(glb(&folder).unwrap_err().contains("não corresponde"));
        fs::remove_dir_all(&folder).unwrap();
        assert!(glb(&folder).is_err());
    }

    /// With the developer's private extracted game: both bodies, their head bone and the decoded texture.
    #[test]
    fn reads_the_verified_game_body_when_present() {
        let candidate = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../local/candidate.json");
        let Ok(text) = fs::read_to_string(candidate) else {
            return;
        };
        let root: serde_json::Value = serde_json::from_str(&text).unwrap();
        let extracted = Path::new(root["data_root"].as_str().unwrap()).join("extracted");
        let Ok(data) = fs::read(extracted.join(RELATIVE)) else {
            return;
        };
        let (models, albedo) = parse(&data).unwrap();
        assert_eq!(
            models.iter().map(|m| m.name.as_str()).collect::<Vec<_>>(),
            ["FemaleBody", "MaleBody"]
        );
        for model in &models {
            assert!(
                (model.head[1] - (6.47 + 0.328 + 3.762)).abs() < 1e-4
                    && (model.head[2] + 0.4).abs() < 1e-4
            );
            let counts: Vec<_> = model
                .parts
                .iter()
                .map(|p| (p.material.as_str(), p.positions.len()))
                .collect();
            assert_eq!(
                counts,
                [("mt_body", 808), ("mt_nocolor", 678), ("mt_scarf", 175)]
            );
            // The scarf is rigid on the chest: its vertices sit between the shoulders and the head.
            let scarf = &model.parts[2];
            assert!(scarf
                .positions
                .iter()
                .all(|p| p[1] > 6.0 && p[1] < model.head[1] + 1.0));
        }
        assert_eq!(albedo.len(), 384 * 384 * 4);
        assert!(
            albedo
                .as_chunks::<4>()
                .0
                .iter()
                .filter(|p| p[0] > 200)
                .count()
                > 384 * 100
        ); // mostly light suit
        let glb = glb(&extracted).unwrap();
        assert_eq!(&glb[0..4], b"glTF");
        assert_eq!(
            u32::from_le_bytes(glb[8..12].try_into().unwrap()) as usize,
            glb.len()
        );
    }
}
