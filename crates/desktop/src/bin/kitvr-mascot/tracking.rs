use std::io;
use std::net::UdpSocket;
use std::time::{Duration, Instant};

use glam::{Quat, Vec3};
use kitvr_proto::tracking::{
    BodyTracking, Hand, Pose, Tracking, BODY_TRACKING_LEN, PACKET_TYPE_BODY_TRACKING,
    PACKET_TYPE_TRACKING, TRACKING_LEN,
};
use kitvr_proto::AVATAR_TRACKING_PORT;

const TIMEOUT: Duration = Duration::from_millis(350);

pub struct Feed {
    socket: UdpSocket,
    state: TrackingState,
}

impl Feed {
    pub fn bind() -> io::Result<Self> {
        let socket = UdpSocket::bind(("127.0.0.1", AVATAR_TRACKING_PORT))?;
        socket.set_nonblocking(true)?;
        Ok(Self { socket, state: TrackingState::default() })
    }

    pub fn poll(&mut self, now: Instant) -> io::Result<Option<Frame>> {
        let mut bytes = [0; BODY_TRACKING_LEN + 1];
        for _ in 0..64 {
            match self.socket.recv_from(&mut bytes) {
                Ok((length, sender)) if sender.ip().is_loopback() => {
                    match (length, bytes[0]) {
                        (TRACKING_LEN, PACKET_TYPE_TRACKING) => {
                            if let Ok(packet) = Tracking::parse(&bytes[..length]) {
                                self.state.accept(packet, now);
                            }
                        }
                        (BODY_TRACKING_LEN, PACKET_TYPE_BODY_TRACKING) => {
                            if let Ok(packet) = BodyTracking::parse(&bytes[..length]) {
                                self.state.accept_body(packet, now);
                            }
                        }
                        _ => {}
                    }
                }
                Ok(_) => {}
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) => return Err(error),
            }
        }
        Ok(self.state.current(now))
    }
}

#[derive(Default)]
struct TrackingState {
    current: Option<(Tracking, Instant)>,
    body: Option<(BodyTracking, Instant)>,
}

#[derive(Debug, Clone, Copy)]
pub struct Frame {
    pub tracking: Tracking,
    pub body: Option<BodyTracking>,
}

fn valid_pose(pose: &mut Pose) -> bool {
    if !pose.position.iter().all(|v| v.is_finite()) ||
        !pose.orientation.iter().all(|v| v.is_finite()) {
        return false;
    }
    let q = Quat::from_array(pose.orientation);
    let length = q.length_squared();
    if !length.is_finite() || length < 1e-8 {
        return false;
    }
    pose.orientation = q.normalize().to_array();
    true
}

fn validate_hand(hand: &mut Hand) {
    if !hand.active || !valid_pose(&mut hand.pose) || !hand.stick.iter().all(|v| v.is_finite()) {
        *hand = Hand::default();
        return;
    }
    hand.stick = hand.stick.map(|value| value.clamp(-1.0, 1.0));
}

impl TrackingState {
    fn accept(&mut self, mut packet: Tracking, now: Instant) {
        if !valid_pose(&mut packet.head) {
            return;
        }
        if let Some((previous, at)) = self.current {
            if now.duration_since(at) <= TIMEOUT {
                if (packet.seq.wrapping_sub(previous.seq) as i16) <= 0 {
                    return;
                }
            } else {
                self.body = None;
            }
        }
        validate_hand(&mut packet.left);
        validate_hand(&mut packet.right);
        self.current = Some((packet, now));
    }

    fn accept_body(&mut self, mut packet: BodyTracking, now: Instant) {
        if self.current.is_none_or(|(_, at)| now.duration_since(at) > TIMEOUT) {
            return;
        }
        if let Some((previous, at)) = self.body {
            if now.duration_since(at) <= TIMEOUT &&
                (packet.seq.wrapping_sub(previous.seq) as i16) <= 0 {
                return;
            }
        }
        for (index, joint) in packet.joints.iter_mut().enumerate() {
            if packet.active_mask & (1 << index) != 0 && !valid_pose(joint) {
                packet.active_mask &= !(1 << index);
            }
        }
        self.body = Some((packet, now));
    }

    fn current(&mut self, now: Instant) -> Option<Frame> {
        if self.current.is_some_and(|(_, at)| now.duration_since(at) > TIMEOUT) {
            self.current = None;
            self.body = None;
        }
        if self.body.is_some_and(|(_, at)| now.duration_since(at) > TIMEOUT) {
            self.body = None;
        }
        self.current.map(|(tracking, _)| Frame { tracking, body: self.body.map(|(body, _)| body) })
    }
}

/// Approximate hand directions in avatar space; no elbow or finger tracking is inferred.
pub fn apply_pose(pose: &mut crate::pose::Pose, frame: &Frame) {
    let tracking = &frame.tracking;
    let head = Quat::from_array(tracking.head.orientation);
    let forward = head * -Vec3::Z;
    let yaw = forward.x.atan2(-forward.z);
    let world_to_avatar = Quat::from_rotation_y(std::f32::consts::PI)
        * Quat::from_rotation_y(yaw);
    let relative_head = world_to_avatar * head * world_to_avatar.inverse();
    pose.rotations.insert("neck", Quat::IDENTITY);
    pose.rotations.insert("head", relative_head);
    let head_position = Vec3::from_array(tracking.head.position);
    for (hand, name, side) in [
        (&tracking.left, "leftUpperArm", 1.0),
        (&tracking.right, "rightUpperArm", -1.0),
    ] {
        if !hand.active {
            continue;
        }
        let wrist = world_to_avatar * (Vec3::from_array(hand.pose.position) - head_position);
        let shoulder = Vec3::new(side * 0.18, -0.22, 0.0);
        let direction = (wrist - shoulder).normalize_or_zero();
        if direction.length_squared() > 0.5 {
            pose.rotations.insert(name, Quat::from_rotation_arc(Vec3::X * side, direction));
        }
    }
    if let Some(body) = &frame.body {
        for (knee, ankle, upper_name, lower_name) in [
            (2, 4, "leftUpperLeg", "leftLowerLeg"),
            (3, 5, "rightUpperLeg", "rightLowerLeg"),
        ] {
            let required = (1 << 0) | (1 << knee) | (1 << ankle);
            if body.active_mask & required != required {
                continue;
            }
            let hips = Vec3::from_array(body.joints[0].position);
            let knee_position = Vec3::from_array(body.joints[knee].position);
            let ankle_position = Vec3::from_array(body.joints[ankle].position);
            let upper = (world_to_avatar * (knee_position - hips)).normalize_or_zero();
            let lower = (world_to_avatar * (ankle_position - knee_position)).normalize_or_zero();
            if upper.length_squared() > 0.5 && lower.length_squared() > 0.5 {
                let upper_rotation = Quat::from_rotation_arc(-Vec3::Y, upper);
                let lower_rotation = Quat::from_rotation_arc(-Vec3::Y, lower);
                pose.rotations.insert(upper_name, upper_rotation);
                pose.rotations.insert(lower_name, upper_rotation.inverse() * lower_rotation);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(seq: u16) -> Tracking {
        Tracking {
            seq,
            head: Pose { position: [0.0, 1.7, 0.0], orientation: [0.0, 0.0, 0.0, 1.0] },
            left: Hand {
                active: true,
                pose: Pose { position: [-0.4, 1.2, 0.0], orientation: [0.0, 0.0, 0.0, 1.0] },
                ..Default::default()
            },
            ..Default::default()
        }
    }

    #[test]
    fn rejects_stale_and_invalid_tracking_and_resets_after_timeout() {
        let now = Instant::now();
        let mut state = TrackingState::default();
        state.accept(packet(10), now);
        state.accept(packet(9), now);
        assert_eq!(state.current(now).unwrap().tracking.seq, 10);
        let mut invalid = packet(11);
        invalid.head.orientation = [0.0; 4];
        state.accept(invalid, now);
        assert_eq!(state.current(now).unwrap().tracking.seq, 10);
        assert!(state.current(now + TIMEOUT + Duration::from_millis(1)).is_none());
        state.accept(packet(0), now + TIMEOUT + Duration::from_millis(1));
        assert_eq!(state.current(now + TIMEOUT + Duration::from_millis(1)).unwrap().tracking.seq, 0);
    }

    #[test]
    fn invalid_hand_cannot_drive_motion() {
        let now = Instant::now();
        let mut input = packet(1);
        input.left.stick = [f32::NAN, 1.0];
        let mut state = TrackingState::default();
        state.accept(input, now);
        assert!(!state.current(now).unwrap().tracking.left.active);
    }

    #[test]
    fn tracked_hand_changes_arm_direction() {
        let mut avatar = crate::pose::build(&crate::pose::Motion::default());
        apply_pose(&mut avatar, &Frame { tracking: packet(1), body: None });
        let arm = avatar.rotations["leftUpperArm"] * Vec3::X;
        assert!(arm.x > 0.2 && arm.y < -0.4, "{arm}");
    }

    #[test]
    fn receives_loopback_tracking_and_drops_bad_datagrams() {
        let socket = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        socket.set_nonblocking(true).unwrap();
        let address = socket.local_addr().unwrap();
        let mut feed = Feed { socket, state: TrackingState::default() };
        let sender = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        sender.send_to(&[2, 0], address).unwrap();
        let mut bytes = Vec::new();
        packet(15).write_to(&mut bytes);
        sender.send_to(&bytes, address).unwrap();
        assert_eq!(feed.poll(Instant::now()).unwrap().unwrap().tracking.seq, 15);
    }

    #[test]
    fn body_tracking_expires_independently_and_rejects_bad_joints() {
        let now = Instant::now();
        let mut state = TrackingState::default();
        state.accept(packet(1), now);
        let mut body = BodyTracking { seq: 4, active_mask: 1, ..Default::default() };
        body.joints[0].orientation = [0.0, 0.0, 0.0, 1.0];
        state.accept_body(body, now);
        assert_eq!(state.current(now).unwrap().body.unwrap().active_mask, 1);
        state.accept(packet(2), now + TIMEOUT);
        assert!(state.current(now + TIMEOUT + Duration::from_millis(1)).unwrap().body.is_none());
        body.seq = 5;
        body.joints[0].position[0] = f32::NAN;
        state.accept_body(body, now + TIMEOUT + Duration::from_millis(1));
        assert_eq!(state.current(now + TIMEOUT + Duration::from_millis(1)).unwrap().body.unwrap().active_mask, 0);
    }

    #[test]
    fn body_joints_pose_the_legs() {
        let mut body = BodyTracking { active_mask: 0b010101, ..Default::default() };
        body.joints[0].position = [0.0, 1.0, 0.0];
        body.joints[2].position = [-0.2, 0.6, -0.3];
        body.joints[4].position = [-0.2, 0.2, -0.3];
        let mut avatar = crate::pose::build(&crate::pose::Motion::default());
        apply_pose(&mut avatar, &Frame { tracking: packet(1), body: Some(body) });
        let direction = avatar.rotations["leftUpperLeg"] * -Vec3::Y;
        assert!(direction.z > 0.4 && direction.y < -0.4, "{direction}");
    }
}
