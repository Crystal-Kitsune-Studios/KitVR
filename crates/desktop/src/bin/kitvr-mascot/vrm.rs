//! VRM loading: a glTF binary with the VRM 0.x (`VRM`) or 1.0 (`VRMC_vrm`) extension.
//!
//! Everything needed to draw and pose the avatar on the CPU is pulled out up front: node rest
//! transforms, skins, primitives with their morph targets, textures, the humanoid bone map and the
//! expression bindings. The scene is turned to face +Z for both VRM versions (0.x avatars face -Z).

use std::collections::HashMap;

use glam::{Mat4, Quat, Vec3};
use serde_json::Value;

#[derive(Debug, Clone)]
pub struct Node {
    pub translation: Vec3,
    pub rotation: Quat,
    pub scale: Vec3,
    pub parent: Option<usize>,
}

#[derive(Debug, Clone)]
pub struct Skin {
    pub joints: Vec<usize>,
    pub inverse_bind: Vec<Mat4>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AlphaMode {
    Opaque,
    Mask,
    Blend,
}

#[derive(Debug, Clone)]
pub struct Material {
    pub base_color: [f32; 4],
    pub texture: Option<usize>,
    pub alpha_mode: AlphaMode,
    pub cutoff: f32,
    pub double_sided: bool,
}

impl Default for Material {
    fn default() -> Self {
        Self { base_color: [1.0; 4], texture: None, alpha_mode: AlphaMode::Opaque, cutoff: 0.5, double_sided: false }
    }
}

#[derive(Debug, Clone)]
pub struct MorphTarget {
    pub positions: Vec<Vec3>,
    pub normals: Vec<Vec3>,
}

#[derive(Debug, Clone)]
pub struct Primitive {
    pub node: usize,
    pub mesh: usize,
    pub skin: Option<usize>,
    pub positions: Vec<Vec3>,
    pub normals: Vec<Vec3>,
    pub uvs: Vec<[f32; 2]>,
    pub joints: Vec<[u16; 4]>,
    pub weights: Vec<[f32; 4]>,
    pub indices: Vec<u32>,
    pub material: Option<usize>,
    pub targets: Vec<MorphTarget>,
}

#[derive(Debug, Clone)]
pub struct Texture {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

/// One morph target driven by an expression, already scaled to a 0..1 weight.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MorphBind {
    pub mesh: usize,
    pub index: usize,
    pub weight: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Expression {
    Blink,
    Happy,
    Surprised,
}

#[derive(Debug, Clone, Default)]
pub struct VrmMeta {
    /// VRM 1.0 avatars face +Z; 0.x avatars face -Z.
    pub faces_positive_z: bool,
    /// Humanoid bone name (VRM 1.0 spelling, for example `leftUpperArm`) to node index.
    pub bones: HashMap<String, usize>,
    pub expressions: HashMap<Expression, Vec<MorphBind>>,
}

#[derive(Debug, Clone)]
pub struct Model {
    pub nodes: Vec<Node>,
    /// Node indices with every parent before its children.
    pub order: Vec<usize>,
    pub skins: Vec<Skin>,
    pub primitives: Vec<Primitive>,
    pub materials: Vec<Material>,
    pub textures: Vec<Texture>,
    pub meta: VrmMeta,
    /// Applied above the scene roots so the avatar faces +Z.
    pub root: Mat4,
}

pub fn load(bytes: &[u8]) -> Result<Model, String> {
    let json = glb_json(bytes)?;
    let meta = parse_vrm_meta(&json)?;
    let (document, buffers, images) =
        gltf::import_slice(bytes).map_err(|error| format!("not a readable glTF/VRM file: {error}"))?;

    let mut nodes: Vec<Node> = document
        .nodes()
        .map(|node| {
            let (t, r, s) = node.transform().decomposed();
            Node {
                translation: Vec3::from(t),
                rotation: Quat::from_array(r).normalize(),
                scale: Vec3::from(s),
                parent: None,
            }
        })
        .collect();
    for node in document.nodes() {
        for child in node.children() {
            nodes[child.index()].parent = Some(node.index());
        }
    }
    let order = topological_order(&nodes);

    let skins = document
        .skins()
        .map(|skin| {
            let joints: Vec<usize> = skin.joints().map(|joint| joint.index()).collect();
            let reader = skin.reader(|buffer| Some(&buffers[buffer.index()]));
            let inverse_bind = match reader.read_inverse_bind_matrices() {
                Some(matrices) => matrices.map(|m| Mat4::from_cols_array_2d(&m)).collect(),
                None => vec![Mat4::IDENTITY; joints.len()],
            };
            Skin { joints, inverse_bind }
        })
        .collect();

    let mut primitives = Vec::new();
    for node in document.nodes() {
        let Some(mesh) = node.mesh() else { continue };
        for primitive in mesh.primitives() {
            if primitive.mode() != gltf::mesh::Mode::Triangles {
                continue;
            }
            let reader = primitive.reader(|buffer| Some(&buffers[buffer.index()]));
            let Some(positions) = reader.read_positions() else { continue };
            let positions: Vec<Vec3> = positions.map(Vec3::from).collect();
            let count = positions.len();
            let normals = reader
                .read_normals()
                .map(|n| n.map(Vec3::from).collect())
                .unwrap_or_else(|| vec![Vec3::Z; count]);
            let uvs = reader
                .read_tex_coords(0)
                .map(|uv| uv.into_f32().collect())
                .unwrap_or_else(|| vec![[0.0; 2]; count]);
            let joints = reader.read_joints(0).map(|j| j.into_u16().collect()).unwrap_or_default();
            let weights = reader.read_weights(0).map(|w| w.into_f32().collect()).unwrap_or_default();
            let indices = reader
                .read_indices()
                .map(|i| i.into_u32().collect())
                .unwrap_or_else(|| (0..count as u32).collect());
            let targets = reader
                .read_morph_targets()
                .map(|(p, n, _)| MorphTarget {
                    positions: p.map(|p| p.map(Vec3::from).collect()).unwrap_or_default(),
                    normals: n.map(|n| n.map(Vec3::from).collect()).unwrap_or_default(),
                })
                .collect();
            primitives.push(Primitive {
                node: node.index(),
                mesh: mesh.index(),
                skin: node.skin().map(|skin| skin.index()),
                positions,
                normals,
                uvs,
                joints,
                weights,
                indices,
                material: primitive.material().index(),
                targets,
            });
        }
    }
    if primitives.is_empty() {
        return Err("the VRM file has no triangle meshes".to_string());
    }

    let materials = document
        .materials()
        .map(|material| {
            let pbr = material.pbr_metallic_roughness();
            Material {
                base_color: pbr.base_color_factor(),
                texture: pbr.base_color_texture().map(|info| info.texture().source().index()),
                alpha_mode: match material.alpha_mode() {
                    gltf::material::AlphaMode::Opaque => AlphaMode::Opaque,
                    gltf::material::AlphaMode::Mask => AlphaMode::Mask,
                    gltf::material::AlphaMode::Blend => AlphaMode::Blend,
                },
                cutoff: material.alpha_cutoff().unwrap_or(0.5),
                double_sided: material.double_sided(),
            }
        })
        .collect();

    let textures = images.into_iter().map(to_rgba).collect();
    let root = if meta.faces_positive_z { Mat4::IDENTITY } else { Mat4::from_rotation_y(std::f32::consts::PI) };

    Ok(Model { nodes, order, skins, primitives, materials, textures, meta, root })
}

/// Returns the JSON chunk of a glTF binary.
pub fn glb_json(bytes: &[u8]) -> Result<Value, String> {
    let u32_at = |offset: usize| -> Option<u32> {
        bytes.get(offset..offset + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    if bytes.get(0..4) != Some(b"glTF") {
        return Err("not a VRM file (VRM avatars are binary glTF, starting with \"glTF\")".to_string());
    }
    let length = u32_at(12).ok_or("truncated VRM file")? as usize;
    if u32_at(16) != Some(0x4E4F_534A) {
        return Err("the VRM file's first chunk is not JSON".to_string());
    }
    let chunk = bytes.get(20..20 + length).ok_or("truncated VRM file")?;
    serde_json::from_slice(chunk).map_err(|error| format!("invalid glTF JSON: {error}"))
}

pub fn parse_vrm_meta(json: &Value) -> Result<VrmMeta, String> {
    let extensions = &json["extensions"];
    let mut meta = VrmMeta::default();
    if let Some(vrm) = extensions.get("VRMC_vrm") {
        meta.faces_positive_z = true;
        if let Some(bones) = vrm["humanoid"]["humanBones"].as_object() {
            for (name, bone) in bones {
                if let Some(node) = bone["node"].as_u64() {
                    meta.bones.insert(name.clone(), node as usize);
                }
            }
        }
        let node_mesh = |node: u64| json["nodes"][node as usize]["mesh"].as_u64();
        let presets = &vrm["expressions"]["preset"];
        for (expression, name) in [
            (Expression::Blink, "blink"),
            (Expression::Happy, "happy"),
            (Expression::Surprised, "surprised"),
        ] {
            let binds: Vec<MorphBind> = presets[name]["morphTargetBinds"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|bind| {
                    Some(MorphBind {
                        mesh: node_mesh(bind["node"].as_u64()?)? as usize,
                        index: bind["index"].as_u64()? as usize,
                        weight: bind["weight"].as_f64().unwrap_or(1.0) as f32,
                    })
                })
                .collect();
            if !binds.is_empty() {
                meta.expressions.insert(expression, binds);
            }
        }
    } else if let Some(vrm) = extensions.get("VRM") {
        meta.faces_positive_z = false;
        for bone in vrm["humanoid"]["humanBones"].as_array().into_iter().flatten() {
            if let (Some(name), Some(node)) = (bone["bone"].as_str(), bone["node"].as_u64()) {
                meta.bones.insert(name.to_string(), node as usize);
            }
        }
        for group in vrm["blendShapeMaster"]["blendShapeGroups"].as_array().into_iter().flatten() {
            let preset = group["presetName"].as_str().unwrap_or_default().to_ascii_lowercase();
            let expression = match preset.as_str() {
                "blink" => Expression::Blink,
                "joy" => Expression::Happy,
                "fun" if !meta.expressions.contains_key(&Expression::Happy) => Expression::Happy,
                "surprised" => Expression::Surprised,
                _ => continue,
            };
            let binds: Vec<MorphBind> = group["binds"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|bind| {
                    Some(MorphBind {
                        mesh: bind["mesh"].as_u64()? as usize,
                        index: bind["index"].as_u64()? as usize,
                        // VRM 0.x weights are percentages.
                        weight: bind["weight"].as_f64().unwrap_or(100.0) as f32 / 100.0,
                    })
                })
                .collect();
            if !binds.is_empty() {
                meta.expressions.insert(expression, binds);
            }
        }
    } else {
        return Err("this glTF file has no VRM humanoid (expected a VRM 0.x or 1.0 avatar)".to_string());
    }
    for required in ["hips", "head"] {
        if !meta.bones.contains_key(required) {
            return Err(format!("the VRM humanoid has no {required} bone"));
        }
    }
    Ok(meta)
}

fn topological_order(nodes: &[Node]) -> Vec<usize> {
    let mut depth = vec![usize::MAX; nodes.len()];
    fn depth_of(i: usize, nodes: &[Node], depth: &mut [usize], guard: usize) -> usize {
        if depth[i] != usize::MAX {
            return depth[i];
        }
        let d = match nodes[i].parent {
            Some(parent) if guard < nodes.len() => depth_of(parent, nodes, depth, guard + 1) + 1,
            _ => 0,
        };
        depth[i] = d;
        d
    }
    for i in 0..nodes.len() {
        depth_of(i, nodes, &mut depth, 0);
    }
    let mut order: Vec<usize> = (0..nodes.len()).collect();
    order.sort_by_key(|&i| depth[i]);
    order
}

fn to_rgba(image: gltf::image::Data) -> Texture {
    use gltf::image::Format;
    let pixels = (image.width * image.height) as usize;
    let rgba = match image.format {
        Format::R8G8B8A8 => image.pixels,
        Format::R8G8B8 => image.pixels.chunks_exact(3).flat_map(|p| [p[0], p[1], p[2], 255]).collect(),
        Format::R8G8 => image.pixels.chunks_exact(2).flat_map(|p| [p[0], p[0], p[0], p[1]]).collect(),
        Format::R8 => image.pixels.iter().flat_map(|&p| [p, p, p, 255]).collect(),
        // 16-bit and float formats are rare in avatars; draw them white rather than fail.
        _ => vec![255; pixels * 4],
    };
    Texture { width: image.width, height: image.height, rgba }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn reads_vrm1_bones_and_expressions() {
        let json = json!({
            "nodes": [{}, {}, {"mesh": 3}],
            "extensions": {"VRMC_vrm": {
                "humanoid": {"humanBones": {"hips": {"node": 0}, "head": {"node": 1}}},
                "expressions": {"preset": {
                    "blink": {"morphTargetBinds": [{"node": 2, "index": 7, "weight": 1.0}]},
                    "happy": {"morphTargetBinds": [{"node": 2, "index": 2, "weight": 0.5}]}
                }}
            }}
        });
        let meta = parse_vrm_meta(&json).unwrap();
        assert!(meta.faces_positive_z);
        assert_eq!(meta.bones["head"], 1);
        assert_eq!(meta.expressions[&Expression::Blink], vec![MorphBind { mesh: 3, index: 7, weight: 1.0 }]);
        assert_eq!(meta.expressions[&Expression::Happy][0].weight, 0.5);
    }

    #[test]
    fn reads_vrm0_bones_and_percent_weights() {
        let json = json!({"extensions": {"VRM": {
            "humanoid": {"humanBones": [{"bone": "hips", "node": 4}, {"bone": "head", "node": 9}]},
            "blendShapeMaster": {"blendShapeGroups": [
                {"presetName": "blink", "binds": [{"mesh": 1, "index": 12, "weight": 100}]},
                {"presetName": "Joy", "binds": [{"mesh": 1, "index": 3, "weight": 50}]}
            ]}
        }}});
        let meta = parse_vrm_meta(&json).unwrap();
        assert!(!meta.faces_positive_z);
        assert_eq!(meta.bones["hips"], 4);
        assert_eq!(meta.expressions[&Expression::Blink][0], MorphBind { mesh: 1, index: 12, weight: 1.0 });
        assert_eq!(meta.expressions[&Expression::Happy][0].weight, 0.5);
    }

    #[test]
    fn rejects_plain_gltf_and_missing_bones() {
        assert!(parse_vrm_meta(&json!({})).is_err());
        let json = json!({"extensions": {"VRMC_vrm": {"humanoid": {"humanBones": {"hips": {"node": 0}}}}}});
        assert!(parse_vrm_meta(&json).unwrap_err().contains("head"));
    }

    #[test]
    fn glb_header_is_checked() {
        assert!(glb_json(b"not a glb").is_err());
        let body = br#"{"asset":{"version":"2.0"}}  "#;
        let mut glb = Vec::new();
        glb.extend_from_slice(b"glTF");
        glb.extend_from_slice(&2u32.to_le_bytes());
        glb.extend_from_slice(&((20 + body.len()) as u32).to_le_bytes());
        glb.extend_from_slice(&(body.len() as u32).to_le_bytes());
        glb.extend_from_slice(&0x4E4F_534Au32.to_le_bytes());
        glb.extend_from_slice(body);
        assert_eq!(glb_json(&glb).unwrap()["asset"]["version"], "2.0");
    }
}
