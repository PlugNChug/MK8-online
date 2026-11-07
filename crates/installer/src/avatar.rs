//! Original procedural head. Coordinates: Y up, face +Z, neck attachment at the origin.
use mk8_manifest::Result;
use serde::{Deserialize, Serialize};
use std::f32::consts::PI;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct Face {
    pub skin: u8,
    pub shape: u8,
    pub hair: u8,
    pub hair_color: u8,
    pub eyes: u8,
    pub eye_color: u8,
    pub glasses: bool,
    pub height: u8,
    pub build: u8,
}
impl Default for Face {
    fn default() -> Self {
        Self {
            skin: 0,
            shape: 0,
            hair: 1,
            hair_color: 1,
            eyes: 0,
            eye_color: 0,
            glasses: false,
            height: 64,
            build: 64,
        }
    }
}
impl Face {
    pub fn validate(&self) -> Result<()> {
        if self.skin > 5
            || self.shape > 2
            || self.hair > 3
            || self.hair_color > 7
            || self.eyes > 2
            || self.eye_color > 5
            || self.height > 127
            || self.build > 127
        {
            return Err("Aparência fora dos limites do editor.".into());
        }
        Ok(())
    }
    pub fn bytes(&self) -> [u8; 9] {
        [
            self.skin,
            self.shape,
            self.hair,
            self.hair_color,
            self.eyes,
            self.eye_color,
            self.glasses as u8,
            self.height,
            self.build,
        ]
    }
}

#[derive(Serialize)]
pub struct Mesh {
    pub version: u32,
    pub vertices: Vec<[f32; 9]>, // position, normal, linear RGB
    pub indices: Vec<u32>,
}
impl Mesh {
    fn ellipsoid(&mut self, center: [f32; 3], scale: [f32; 3], color: [f32; 3], cap: f32) {
        let base = self.vertices.len() as u32;
        let (rings, sectors) = (12, 24);
        for y in 0..=rings {
            let t = PI * cap * y as f32 / rings as f32;
            for x in 0..=sectors {
                let p = 2.0 * PI * x as f32 / sectors as f32;
                let v = [t.sin() * p.cos(), t.cos(), t.sin() * p.sin()];
                let n = [v[0] / scale[0], v[1] / scale[1], v[2] / scale[2]];
                let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
                self.vertices.push([
                    center[0] + v[0] * scale[0],
                    center[1] + v[1] * scale[1],
                    center[2] + v[2] * scale[2],
                    n[0] / len,
                    n[1] / len,
                    n[2] / len,
                    color[0],
                    color[1],
                    color[2],
                ]);
            }
        }
        for y in 0..rings {
            for x in 0..sectors {
                let a = base + (y * (sectors + 1) + x) as u32;
                let b = a + sectors as u32 + 1;
                if y > 0 {
                    self.indices.extend([a, a + 1, b]);
                }
                if y < rings - 1 || cap < 1.0 {
                    self.indices.extend([a + 1, b + 1, b]);
                }
            }
        }
    }
}
fn rgb(hex: u32) -> [f32; 3] {
    [16, 8, 0].map(|s| {
        let v = ((hex >> s) & 255) as f32 / 255.0;
        if v <= 0.04045 {
            v / 12.92
        } else {
            ((v + 0.055) / 1.055).powf(2.4)
        }
    })
}

pub fn head(face: &Face) -> Result<Mesh> {
    face.validate()?;
    let mut m = Mesh {
        version: 1,
        vertices: vec![],
        indices: vec![],
    };
    let skin =
        rgb([0xf4d0ae, 0xe9b383, 0xc98e61, 0xa86b46, 0x7a482f, 0x4b2b20][face.skin as usize]);
    let hair = rgb([
        0x191b21, 0x513325, 0x8c532f, 0xdab971, 0xaa4e30, 0xb9b6b0, 0x374b84, 0x8f4278,
    ][face.hair_color as usize]);
    let eye =
        rgb([0x3b302b, 0x34587d, 0x3d6b4a, 0x8b6437, 0x6f7078, 0x1b1d26][face.eye_color as usize]);
    let (w, h) = match face.shape {
        1 => (0.47, 0.57),
        2 => (0.51, 0.48),
        _ => (0.49, 0.52),
    };
    m.ellipsoid([0.0, 0.58, 0.0], [w, h, 0.41], skin, 1.0);
    for sign in [-1.0, 1.0] {
        m.ellipsoid([sign * w, 0.56, 0.0], [0.085, 0.115, 0.07], skin, 1.0);
    }
    if face.hair != 0 {
        m.ellipsoid(
            [0.0, 0.60, -0.035],
            [w + 0.035, h + 0.045, 0.435],
            hair,
            0.47,
        );
        if face.hair == 2 {
            // side-swept fringe
            m.ellipsoid([-0.14, 0.94, 0.29], [0.28, 0.095, 0.15], hair, 1.0);
            m.ellipsoid([0.17, 0.86, 0.31], [0.13, 0.22, 0.12], hair, 1.0);
        }
        if face.hair == 3 {
            for sign in [-1.0, 1.0] {
                m.ellipsoid([sign * 0.40, 0.48, -0.14], [0.14, 0.40, 0.27], hair, 1.0);
            }
        }
    }
    let eye_h = match face.eyes {
        1 => 0.07,
        2 => 0.12,
        _ => 0.095,
    };
    for sign in [-1.0, 1.0] {
        let x = sign * 0.18;
        m.ellipsoid([x, 0.65, 0.365], [0.095, eye_h, 0.045], rgb(0xfffcf6), 1.0);
        m.ellipsoid([x, 0.65, 0.406], [0.042, eye_h * 0.67, 0.014], eye, 1.0);
        m.ellipsoid(
            [x, 0.65, 0.419],
            [0.023, eye_h * 0.43, 0.008],
            rgb(0x13151c),
            1.0,
        );
        m.ellipsoid(
            [x - 0.012, 0.67, 0.426],
            [0.012, 0.015, 0.004],
            rgb(0xffffff),
            1.0,
        );
        m.ellipsoid([x, 0.80, 0.352], [0.098, 0.018, 0.026], hair, 1.0);
    }
    m.ellipsoid([0.0, 0.52, 0.397], [0.063, 0.08, 0.11], skin, 1.0);
    m.ellipsoid([0.0, 0.36, 0.375], [0.12, 0.026, 0.022], rgb(0x8d453a), 1.0);
    if face.glasses {
        let c = rgb(0x242936);
        for sign in [-1.0, 1.0] {
            for y in [0.55, 0.75] {
                m.ellipsoid([sign * 0.18, y, 0.435], [0.12, 0.013, 0.013], c, 1.0);
            }
            for x in [sign * 0.07, sign * 0.29] {
                m.ellipsoid([x, 0.65, 0.435], [0.013, 0.10, 0.013], c, 1.0);
            }
        }
        m.ellipsoid([0.0, 0.67, 0.44], [0.08, 0.013, 0.013], c, 1.0);
    }
    Ok(m)
}

pub fn glb(mesh: &Mesh) -> Result<Vec<u8>> {
    use serde_json::json;
    let mut binary = Vec::new();
    for v in &mesh.vertices {
        for f in v {
            binary.extend(f.to_le_bytes());
        }
    }
    let index_offset = binary.len();
    for i in &mesh.indices {
        binary.extend(i.to_le_bytes());
    }
    let mut min = [f32::INFINITY; 3];
    let mut max = [f32::NEG_INFINITY; 3];
    for v in &mesh.vertices {
        for i in 0..3 {
            min[i] = min[i].min(v[i]);
            max[i] = max[i].max(v[i]);
        }
    }
    let doc = json!({"asset":{"version":"2.0","generator":"MK8-online original procedural head v1"},
        "scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"head","mesh":0}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"COLOR_0":2},"indices":3,"material":0}]}],
        "materials":[{"pbrMetallicRoughness":{"metallicFactor":0,"roughnessFactor":0.8},"doubleSided":true}],
        "buffers":[{"byteLength":binary.len()}],
        "bufferViews":[{"buffer":0,"byteLength":index_offset,"byteStride":36,"target":34962},
                       {"buffer":0,"byteOffset":index_offset,"byteLength":binary.len()-index_offset,"target":34963}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":mesh.vertices.len(),"type":"VEC3","min":min,"max":max},
                     {"bufferView":0,"byteOffset":12,"componentType":5126,"count":mesh.vertices.len(),"type":"VEC3"},
                     {"bufferView":0,"byteOffset":24,"componentType":5126,"count":mesh.vertices.len(),"type":"VEC3"},
                     {"bufferView":1,"componentType":5125,"count":mesh.indices.len(),"type":"SCALAR"}]});
    let mut js = serde_json::to_vec(&doc).map_err(|e| e.to_string())?;
    while js.len() % 4 != 0 {
        js.push(b' ');
    }
    let mut out = Vec::new();
    for n in [
        0x46546c67u32,
        2,
        (28 + js.len() + binary.len()) as u32,
        js.len() as u32,
        0x4e4f534a,
    ] {
        out.extend(n.to_le_bytes());
    }
    out.extend(js);
    out.extend((binary.len() as u32).to_le_bytes());
    out.extend(0x004e4942u32.to_le_bytes());
    out.extend(binary);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn geometry_is_finite_bounded_and_personalized() {
        for shape in 0..3 {
            for hair in 0..4 {
                for eyes in 0..3 {
                    let f = Face {
                        shape,
                        hair,
                        eyes,
                        glasses: true,
                        ..Face::default()
                    };
                    let m = head(&f).unwrap();
                    assert!(m.vertices.len() < 16000 && m.indices.len().is_multiple_of(3));
                    for v in &m.vertices {
                        assert!(v.iter().all(|x| x.is_finite()));
                        assert!((v[3] * v[3] + v[4] * v[4] + v[5] * v[5] - 1.0).abs() < 0.0001);
                    }
                    for i in &m.indices {
                        assert!((*i as usize) < m.vertices.len());
                    }
                    let b = glb(&m).unwrap();
                    assert_eq!(
                        u32::from_le_bytes(b[8..12].try_into().unwrap()) as usize,
                        b.len()
                    );
                    let n = u32::from_le_bytes(b[12..16].try_into().unwrap()) as usize;
                    let d: serde_json::Value = serde_json::from_slice(&b[20..20 + n]).unwrap();
                    assert_eq!(d["nodes"][0]["name"], "head");
                }
            }
        }
        let a = glb(&head(&Face::default()).unwrap()).unwrap();
        assert_eq!(a, glb(&head(&Face::default()).unwrap()).unwrap());
        assert_ne!(
            a,
            glb(&head(&Face {
                skin: 4,
                ..Face::default()
            })
            .unwrap())
            .unwrap()
        );
        assert!(head(&Face {
            hair: 255,
            ..Face::default()
        })
        .is_err());
    }
}
