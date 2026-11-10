//! Switch CharInfo (88 bytes). Layout references are recorded in docs/THIRD_PARTY.md.
use mk8_manifest::Result;

pub const SIZE: usize = 0x58;
// Fields from font region through padding; eyebrow Y uses the CharInfo (+3) domain.
const MAX: [u8; 50] = [
    3, 11, 1, 127, 127, 1, 3, 11, 9, 11, 11, 131, 99, 1, 59, 99, 7, 6, 7, 12, 18, 23, 99, 8, 6, 11,
    12, 18, 17, 8, 18, 35, 99, 8, 6, 18, 99, 5, 5, 8, 16, 19, 99, 7, 20, 1, 8, 16, 30, 0,
];

pub fn fields() -> Vec<serde_json::Value> {
    let names = [
        "Font region",
        "Favorite color",
        "Gender",
        "Height",
        "Build",
        "Type",
        "Region move",
        "Face shape",
        "Skin color",
        "Wrinkles",
        "Makeup",
        "Hair style",
        "Hair color",
        "Hair flip",
        "Eye style",
        "Eye color",
        "Eye size",
        "Eye aspect",
        "Eye rotation",
        "Eye spacing",
        "Eye height",
        "Eyebrow style",
        "Eyebrow color",
        "Eyebrow size",
        "Eyebrow aspect",
        "Eyebrow rotation",
        "Eyebrow spacing",
        "Eyebrow height",
        "Nose style",
        "Nose size",
        "Nose height",
        "Mouth style",
        "Mouth color",
        "Mouth size",
        "Mouth aspect",
        "Mouth height",
        "Beard color",
        "Beard style",
        "Mustache style",
        "Mustache size",
        "Mustache height",
        "Glasses style",
        "Glasses color",
        "Glasses size",
        "Glasses height",
        "Mole",
        "Mole size",
        "Mole X",
        "Mole Y",
    ];
    names
        .iter()
        .enumerate()
        .map(|(i, name)| {
            serde_json::json!({"name":name,"offset":i+0x26,
        "min":if i+0x26==0x41 {3} else {0},"max":MAX[i]})
        })
        .collect()
}

pub fn validate(data: &[u8]) -> Result<()> {
    if data.len() != SIZE {
        return Err("Importe um arquivo Switch .charinfo de exatamente 88 bytes.".into());
    }
    if data[..16].iter().all(|&v| v == 0) || data[8] & 0xc0 != 0x80 {
        return Err("Identidade Mii inválida.".into());
    }
    name(data)?;
    if data[0x24] != 0 || data[0x25] != 0 || data[0x41] < 3 {
        return Err("CharInfo contém terminador ou posição de sobrancelha inválidos.".into());
    }
    for (i, (&value, &max)) in data[0x26..].iter().zip(MAX.iter()).enumerate() {
        if value > max {
            return Err(format!(
                "Campo Mii 0x{:02x} fora do limite {max}.",
                i + 0x26
            ));
        }
    }
    Ok(())
}

pub fn name(data: &[u8]) -> Result<String> {
    if data.len() != SIZE {
        return Err("CharInfo truncado.".into());
    }
    let units: Vec<u16> = data[16..36]
        .as_chunks::<2>()
        .0
        .iter()
        .map(|b| u16::from_le_bytes([b[0], b[1]]))
        .collect();
    let end = units.iter().position(|&v| v == 0).unwrap_or(10);
    if units[end..].iter().any(|&v| v != 0) {
        return Err("Nome Mii com dados após terminador.".into());
    }
    let name = String::from_utf16(&units[..end]).map_err(|_| "Nome Mii UTF-16 inválido")?;
    if name.trim().is_empty() || name.chars().any(|c| c.is_control() || c as u32 > 0xffff) {
        return Err("Nome Mii inválido.".into());
    }
    Ok(name)
}

pub fn set_name(data: &mut [u8], name: &str) {
    data[16..38].fill(0);
    for (i, c) in name.encode_utf16().enumerate() {
        data[16 + 2 * i..18 + 2 * i].copy_from_slice(&c.to_le_bytes());
    }
}

pub fn base() -> Vec<u8> {
    let mut data = vec![0; SIZE];
    data[0x26..].copy_from_slice(&[
        0, 0, 0, 64, 64, 0, 0, 0, 0, 0, 0, 33, 1, 0, 2, 8, 4, 3, 4, 2, 12, 6, 1, 4, 3, 6, 2, 10, 1,
        4, 9, 23, 19, 4, 3, 13, 8, 0, 0, 4, 10, 0, 8, 4, 10, 0, 4, 2, 20, 0,
    ]);
    data
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reject_truncation_invalid_fields_and_names() {
        let mut d = base();
        d[8] = 0x80;
        set_name(&mut d, "João");
        validate(&d).unwrap();
        assert!(validate(&d[..87]).is_err());
        for i in 0x26..SIZE {
            let v = d[i];
            d[i] = 255;
            assert!(validate(&d).is_err());
            d[i] = v;
        }
        d[17] = 0xd8;
        assert!(validate(&d).is_err());
    }
}
