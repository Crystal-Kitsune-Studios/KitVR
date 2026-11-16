//! Posing and skinning.
//!
//! Bone rotations are given in "avatar space": +Y up, +Z the way the avatar faces, +X the avatar's
//! left (the viewer's right). A rotation is applied about the bone's pivot as if the rest of the
//! skeleton were still in its rest T-pose, which is what VRM's normalized humanoid means, so the
//! same numbers work whatever the bones' own axes are.

use std::collections::HashMap;
use std::f32::consts::{PI, TAU};

use glam::{Mat4, Quat, Vec3};

use crate::vrm::{Expression, Model, Primitive};

#[derive(Debug, Clone, Default)]
pub struct Pose {
    pub rotations: HashMap<&'static str, Quat>,
    /// Avatar-space offset of the hips, in metres.
    pub hips_offset: Vec3,
    /// Turn of the whole avatar about +Y, in radians. Positive turns toward the viewer's right.
    pub body_yaw: f32,
}

pub struct Skeleton {
    rest_world: Vec<Mat4>,
    /// Rest world rotation of each node's parent (the avatar root for scene roots).
    parent_rest: Vec<Quat>,
    hips: usize,
    bones: HashMap<String, usize>,
    /// How far the hips drop for a floor sit: hips height above the lowest foot.
    pub sit_drop: f32,
}

impl Skeleton {
    pub fn new(model: &Model) -> Self {
        let rest_world = world_matrices(model, &HashMap::new(), Vec3::ZERO, model.root, &[]);
        let (_, root_rotation, _) = model.root.to_scale_rotation_translation();
        let parent_rest = model
            .nodes
            .iter()
            .map(|node| match node.parent {
                Some(parent) => rest_world[parent].to_scale_rotation_translation().1,
                None => root_rotation,
            })
            .collect();
        let bones = model.meta.bones.clone();
        let hips = bones["hips"];
        let height = |name: &str| bones.get(name).map(|&i| rest_world[i].w_axis.y);
        let hips_y = rest_world[hips].w_axis.y;
        let feet_y = [height("leftFoot"), height("rightFoot"), height("leftLowerLeg"), height("rightLowerLeg")]
            .into_iter()
            .flatten()
            .fold(f32::INFINITY, f32::min);
        let sit_drop = if feet_y.is_finite() { (hips_y - feet_y).max(0.0) } else { hips_y * 0.85 };
        Self { rest_world, parent_rest, hips, bones, sit_drop }
    }

    /// World matrix of every node for `pose`.
    pub fn pose(&self, model: &Model, pose: &Pose) -> Vec<Mat4> {
        let mut deltas = HashMap::new();
        for (name, rotation) in &pose.rotations {
            if let Some(&node) = self.bones.get(*name) {
                let parent = self.parent_rest[node];
                deltas.insert(node, parent.inverse() * *rotation * parent);
            }
        }
        let hips_local = {
            let parent = model.nodes[self.hips].parent.map_or(model.root, |p| self.rest_world[p]);
            parent.inverse().transform_vector3(pose.hips_offset)
        };
        let root = Mat4::from_rotation_y(pose.body_yaw) * model.root;
        world_matrices(model, &deltas, hips_local, root, &[self.hips])
    }

    /// Lowest and highest points and the hips' x of the avatar in `world`, for framing.
    pub fn bounds(model: &Model, world: &[Mat4]) -> (Vec3, Vec3) {
        let mut min = Vec3::splat(f32::INFINITY);
        let mut max = Vec3::splat(f32::NEG_INFINITY);
        let mut out = Vec::new();
        for primitive in &model.primitives {
            skin_primitive(model, world, primitive, &[], &mut out);
            for vertex in out.chunks_exact(8) {
                let p = Vec3::new(vertex[0], vertex[1], vertex[2]);
                min = min.min(p);
                max = max.max(p);
            }
        }
        (min, max)
    }
}

fn world_matrices(
    model: &Model,
    deltas: &HashMap<usize, Quat>,
    hips_offset: Vec3,
    root: Mat4,
    offset_nodes: &[usize],
) -> Vec<Mat4> {
    let mut world = vec![Mat4::IDENTITY; model.nodes.len()];
    for &i in &model.order {
        let node = &model.nodes[i];
        let rotation = deltas.get(&i).map_or(node.rotation, |delta| *delta * node.rotation);
        let translation = if offset_nodes.contains(&i) { node.translation + hips_offset } else { node.translation };
        let local = Mat4::from_scale_rotation_translation(node.scale, rotation, translation);
        world[i] = match node.parent {
            Some(parent) => world[parent] * local,
            None => root * local,
        };
    }
    world
}

/// Morph weight per target of `primitive` for the expression weights in `expressions`.
pub fn morph_weights(model: &Model, primitive: &Primitive, expressions: &HashMap<Expression, f32>) -> Vec<f32> {
    let mut weights = vec![0.0; primitive.targets.len()];
    for (expression, amount) in expressions {
        if *amount <= 0.0 {
            continue;
        }
        for bind in model.meta.expressions.get(expression).into_iter().flatten() {
            if bind.mesh == primitive.mesh && bind.index < weights.len() {
                weights[bind.index] = (weights[bind.index] + bind.weight * amount).min(1.0);
            }
        }
    }
    weights
}

/// Writes interleaved position, normal and uv (8 floats per vertex) for `primitive`.
pub fn skin_primitive(model: &Model, world: &[Mat4], primitive: &Primitive, morphs: &[f32], out: &mut Vec<f32>) {
    out.clear();
    out.reserve(primitive.positions.len() * 8);
    let joint_matrices: Option<Vec<Mat4>> = primitive.skin.and_then(|skin| {
        let skin = model.skins.get(skin)?;
        if primitive.joints.len() != primitive.positions.len() || primitive.weights.len() != primitive.positions.len() {
            return None;
        }
        Some(skin.joints.iter().zip(&skin.inverse_bind).map(|(&joint, ibm)| world[joint] * *ibm).collect())
    });
    let node_matrix = world[primitive.node];
    let active: Vec<(usize, f32)> =
        morphs.iter().copied().enumerate().filter(|(_, weight)| *weight > 0.0).collect();
    for v in 0..primitive.positions.len() {
        let mut position = primitive.positions[v];
        let mut normal = primitive.normals[v];
        for &(target, weight) in &active {
            let target = &primitive.targets[target];
            if let Some(delta) = target.positions.get(v) {
                position += *delta * weight;
            }
            if let Some(delta) = target.normals.get(v) {
                normal += *delta * weight;
            }
        }
        let matrix = match &joint_matrices {
            Some(joints) => {
                let indices = primitive.joints[v];
                let weights = primitive.weights[v];
                let mut sum = Mat4::ZERO;
                let mut total = 0.0;
                for k in 0..4 {
                    if weights[k] > 0.0 {
                        if let Some(joint) = joints.get(indices[k] as usize) {
                            sum += *joint * weights[k];
                            total += weights[k];
                        }
                    }
                }
                if total > 0.0 { sum * (1.0 / total) } else { node_matrix }
            }
            None => node_matrix,
        };
        let p = matrix.transform_point3(position);
        let n = matrix.transform_vector3(normal).normalize_or_zero();
        let uv = primitive.uvs[v];
        out.extend_from_slice(&[p.x, p.y, p.z, n.x, n.y, n.z, uv[0], uv[1]]);
    }
}

/// Animation inputs, each smoothed by the behaviour code before it gets here.
#[derive(Debug, Clone, Copy, Default)]
pub struct Motion {
    /// Seconds since start, for breathing and idle sway.
    pub time: f32,
    /// Walk cycle phase in radians, and how much of the walk to show (0..1).
    pub walk_phase: f32,
    pub walk: f32,
    /// 0 standing, 1 sitting on the floor.
    pub sit: f32,
    /// 0 normal, 1 hanging from the cursor.
    pub dangle: f32,
    /// Sideways swing while dangling, in radians (from drag velocity).
    pub swing: f32,
    /// Head turn toward the cursor: yaw (toward the viewer's right) and pitch (down), radians.
    pub look_yaw: f32,
    pub look_pitch: f32,
    pub body_yaw: f32,
    /// Arms-up cheer after a poke (0..1).
    pub cheer: f32,
    /// Hips height drop for a full sit, from `Skeleton::sit_drop`.
    pub sit_drop: f32,
}

fn rx(degrees: f32) -> Quat {
    Quat::from_rotation_x(degrees.to_radians())
}
fn ry(degrees: f32) -> Quat {
    Quat::from_rotation_y(degrees.to_radians())
}
fn rz(degrees: f32) -> Quat {
    Quat::from_rotation_z(degrees.to_radians())
}

pub fn build(m: &Motion) -> Pose {
    let mut pose = Pose { body_yaw: m.body_yaw, ..Pose::default() };
    let r = &mut pose.rotations;
    let breathe = (m.time * TAU / 4.0).sin();
    let walk = m.walk * (1.0 - m.sit);
    let stride = m.walk_phase.sin();

    // Arms: from the T-pose down to the sides, up when dangling or cheering, swinging when walking.
    let raise = m.dangle.max(m.cheer);
    let arm_down = -72.0 + 8.0 * m.sit;
    let arm_angle = arm_down + (145.0 - arm_down) * raise * 0.9;
    let arm_swing = 22.0 * walk * stride;
    r.insert("leftUpperArm", rx(arm_swing) * rz(arm_angle + 1.5 * breathe));
    r.insert("rightUpperArm", rx(-arm_swing) * rz(-(arm_angle + 1.5 * breathe)));
    let elbow = 12.0 + 18.0 * walk + 25.0 * m.sit;
    r.insert("leftLowerArm", ry(-elbow * (1.0 - raise)));
    r.insert("rightLowerArm", ry(elbow * (1.0 - raise)));

    // Legs: walking stride, a floor sit with legs forward, or kicking while dangling.
    let kick = 14.0 * m.dangle * (m.time * 5.0).sin();
    let thigh = 26.0 * walk * stride;
    let sit = -88.0 * m.sit;
    r.insert("leftUpperLeg", rx(-thigh + sit - kick - 8.0 * m.dangle));
    r.insert("rightUpperLeg", rx(thigh + sit + kick - 8.0 * m.dangle));
    let knee = |phase: f32| walk * (6.0 + 34.0 * (-phase.cos()).max(0.0) * (phase.sin() < 0.0) as u8 as f32);
    r.insert("leftLowerLeg", rx(knee(m.walk_phase) + 20.0 * m.dangle));
    r.insert("rightLowerLeg", rx(knee(m.walk_phase + PI) + 20.0 * m.dangle));

    // Torso: breathing, a little twist with the stride, leaning back slightly when sitting.
    r.insert("spine", ry(6.0 * walk * stride) * rx(1.2 * breathe - 4.0 * m.sit) * rz(m.swing.to_degrees() * 0.3));
    r.insert("chest", rx(1.5 * breathe));
    r.insert("hips", rz(m.swing.to_degrees() * 0.7));

    // Head follows the cursor, split over neck and head.
    let yaw = m.look_yaw.to_degrees();
    let pitch = m.look_pitch.to_degrees();
    let tilt = 3.0 * (m.time * TAU / 7.0).sin() * (1.0 - walk);
    r.insert("neck", ry(yaw * 0.4) * rx(pitch * 0.4));
    r.insert("head", ry(yaw * 0.6) * rx(pitch * 0.6) * rz(tilt));

    pose.hips_offset = Vec3::new(0.0, -m.sit * m.sit_drop - 0.02 * walk * stride.abs(), 0.0);
    pose
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rotate(q: Quat, v: Vec3) -> Vec3 {
        q * v
    }

    #[test]
    fn arms_hang_down_from_the_t_pose() {
        let pose = build(&Motion::default());
        // The left arm points along +X in the T-pose; at rest it should point mostly down.
        let left = rotate(pose.rotations["leftUpperArm"], Vec3::X);
        let right = rotate(pose.rotations["rightUpperArm"], -Vec3::X);
        assert!(left.y < -0.9 && left.x > 0.0, "{left}");
        assert!(right.y < -0.9 && right.x < 0.0, "{right}");
    }

    #[test]
    fn dangling_raises_the_arms() {
        let pose = build(&Motion { dangle: 1.0, ..Motion::default() });
        assert!(rotate(pose.rotations["leftUpperArm"], Vec3::X).y > 0.5);
        assert!(rotate(pose.rotations["rightUpperArm"], -Vec3::X).y > 0.5);
    }

    #[test]
    fn sitting_points_the_thighs_forward_and_lowers_the_hips() {
        let pose = build(&Motion { sit: 1.0, sit_drop: 0.8, ..Motion::default() });
        let thigh = rotate(pose.rotations["leftUpperLeg"], -Vec3::Y);
        assert!(thigh.z > 0.9, "thigh should point toward +Z (forward): {thigh}");
        assert!((pose.hips_offset.y + 0.8).abs() < 1e-4);
    }

    #[test]
    fn looking_right_turns_the_face_toward_plus_x() {
        let pose = build(&Motion { look_yaw: 0.6, ..Motion::default() });
        let face = rotate(pose.rotations["neck"] * pose.rotations["head"], Vec3::Z);
        assert!(face.x > 0.3, "{face}");
    }

    #[test]
    fn walking_swings_legs_in_opposite_directions() {
        let pose = build(&Motion { walk: 1.0, walk_phase: PI / 2.0, ..Motion::default() });
        let left = rotate(pose.rotations["leftUpperLeg"], -Vec3::Y);
        let right = rotate(pose.rotations["rightUpperLeg"], -Vec3::Y);
        assert!(left.z > 0.2 && right.z < -0.2, "left {left} right {right}");
    }
}
