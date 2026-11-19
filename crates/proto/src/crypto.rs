//! Authenticated transport primitives shared by the KitVR protocol.

use ring::rand::SecureRandom;
use ring::{aead, hkdf, hmac, rand};

const KEY_LEN: usize = 32;
const TAG_LEN: usize = 16;
const AUTH_CLIENT_LABEL: &[u8] = b"KitVR v2 client authentication";
const AUTH_SERVER_LABEL: &[u8] = b"KitVR v2 server authentication";
const MEDIA_MAGIC: &[u8; 4] = b"KVE2";
const CONTROL_AAD: &[u8] = b"KitVR v2 control";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionKeys {
    pub client_to_server_control: [u8; KEY_LEN],
    pub server_to_client_control: [u8; KEY_LEN],
    pub client_to_server_media: [u8; KEY_LEN],
    pub server_to_client_media: [u8; KEY_LEN],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CryptoError {
    Authentication,
    InvalidKey,
    InvalidCiphertext,
}

impl std::fmt::Display for CryptoError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Authentication => write!(f, "pairing authentication failed"),
            Self::InvalidKey => write!(f, "invalid transport key"),
            Self::InvalidCiphertext => write!(f, "packet authentication failed"),
        }
    }
}

impl std::error::Error for CryptoError {}

struct Aes256Key;

impl hkdf::KeyType for Aes256Key {
    fn len(&self) -> usize {
        KEY_LEN
    }
}

pub fn client_authenticator(
    pairing_key: &[u8; KEY_LEN],
    client_nonce: &[u8; 32],
) -> [u8; 32] {
    let key = hmac::Key::new(hmac::HMAC_SHA256, pairing_key);
    hmac::sign(&key, &[AUTH_CLIENT_LABEL, client_nonce].concat())
        .as_ref()
        .try_into()
        .expect("HMAC-SHA-256 output is 32 bytes")
}

pub fn random_bytes(output: &mut [u8]) -> Result<(), CryptoError> {
    rand::SystemRandom::new()
        .fill(output)
        .map_err(|_| CryptoError::InvalidKey)
}

pub fn verify_client_authenticator(
    pairing_key: &[u8; KEY_LEN],
    client_nonce: &[u8; 32],
    authenticator: &[u8],
) -> Result<(), CryptoError> {
    let key = hmac::Key::new(hmac::HMAC_SHA256, pairing_key);
    hmac::verify(
        &key,
        &[AUTH_CLIENT_LABEL, client_nonce].concat(),
        authenticator,
    )
    .map_err(|_| CryptoError::Authentication)
}

pub fn server_authenticator(
    pairing_key: &[u8; KEY_LEN],
    client_nonce: &[u8; 32],
    server_nonce: &[u8; 32],
) -> [u8; 32] {
    let key = hmac::Key::new(hmac::HMAC_SHA256, pairing_key);
    hmac::sign(
        &key,
        &[AUTH_SERVER_LABEL, client_nonce, server_nonce].concat(),
    )
    .as_ref()
    .try_into()
    .expect("HMAC-SHA-256 output is 32 bytes")
}

pub fn verify_server_authenticator(
    pairing_key: &[u8; KEY_LEN],
    client_nonce: &[u8; 32],
    server_nonce: &[u8; 32],
    authenticator: &[u8],
) -> Result<(), CryptoError> {
    let key = hmac::Key::new(hmac::HMAC_SHA256, pairing_key);
    hmac::verify(
        &key,
        &[AUTH_SERVER_LABEL, client_nonce, server_nonce].concat(),
        authenticator,
    )
    .map_err(|_| CryptoError::Authentication)
}

pub fn derive_session_keys(
    pairing_key: &[u8; KEY_LEN],
    client_nonce: &[u8; 32],
    server_nonce: &[u8; 32],
) -> Result<SessionKeys, CryptoError> {
    let mut salt_bytes = [0; 64];
    salt_bytes[..32].copy_from_slice(client_nonce);
    salt_bytes[32..].copy_from_slice(server_nonce);
    let salt = hkdf::Salt::new(hkdf::HKDF_SHA256, &salt_bytes);
    let prk = salt.extract(pairing_key);
    let expand = |label: &[u8]| -> Result<[u8; KEY_LEN], CryptoError> {
        let info = [b"KitVR v2 ".as_slice(), label];
        let output = prk
            .expand(&info, Aes256Key)
            .map_err(|_| CryptoError::InvalidKey)?;
        let mut key = [0; KEY_LEN];
        output
            .fill(&mut key)
            .map_err(|_| CryptoError::InvalidKey)?;
        Ok(key)
    };

    Ok(SessionKeys {
        client_to_server_control: expand(b"client-to-server control")?,
        server_to_client_control: expand(b"server-to-client control")?,
        client_to_server_media: expand(b"client-to-server media")?,
        server_to_client_media: expand(b"server-to-client media")?,
    })
}

pub fn seal(
    key: &[u8; KEY_LEN],
    sequence: u64,
    aad: &[u8],
    plaintext: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let key = aead::UnboundKey::new(&aead::AES_256_GCM, key)
        .map_err(|_| CryptoError::InvalidKey)?;
    let key = aead::LessSafeKey::new(key);
    let mut nonce_bytes = [0; 12];
    nonce_bytes[4..].copy_from_slice(&sequence.to_be_bytes());
    let nonce = aead::Nonce::assume_unique_for_key(nonce_bytes);
    let mut ciphertext = plaintext.to_vec();
    key.seal_in_place_append_tag(nonce, aead::Aad::from(aad), &mut ciphertext)
        .map_err(|_| CryptoError::InvalidCiphertext)?;
    Ok(ciphertext)
}

pub fn open(
    key: &[u8; KEY_LEN],
    sequence: u64,
    aad: &[u8],
    ciphertext: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    if ciphertext.len() < TAG_LEN {
        return Err(CryptoError::InvalidCiphertext);
    }
    let key = aead::UnboundKey::new(&aead::AES_256_GCM, key)
        .map_err(|_| CryptoError::InvalidKey)?;
    let key = aead::LessSafeKey::new(key);
    let mut nonce_bytes = [0; 12];
    nonce_bytes[4..].copy_from_slice(&sequence.to_be_bytes());
    let nonce = aead::Nonce::assume_unique_for_key(nonce_bytes);
    let mut plaintext = ciphertext.to_vec();
    let plaintext_len = key
        .open_in_place(nonce, aead::Aad::from(aad), &mut plaintext)
        .map_err(|_| CryptoError::InvalidCiphertext)?
        .len();
    plaintext.truncate(plaintext_len);
    Ok(plaintext)
}

pub fn seal_control_record(
    key: &[u8; KEY_LEN],
    sequence: u64,
    plaintext: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let mut aad = CONTROL_AAD.to_vec();
    aad.extend_from_slice(&sequence.to_le_bytes());
    let ciphertext = seal(key, sequence, &aad, plaintext)?;
    let length = 8usize + ciphertext.len();
    let mut record = Vec::with_capacity(4 + length);
    record.extend_from_slice(&(length as u32).to_le_bytes());
    record.extend_from_slice(&sequence.to_le_bytes());
    record.extend_from_slice(&ciphertext);
    Ok(record)
}

pub fn seal_media_datagram(
    key: &[u8; KEY_LEN],
    sequence: u64,
    plaintext: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let mut packet = Vec::with_capacity(12 + plaintext.len() + TAG_LEN);
    packet.extend_from_slice(MEDIA_MAGIC);
    packet.extend_from_slice(&sequence.to_le_bytes());
    let ciphertext = seal(key, sequence, &packet, plaintext)?;
    packet.extend_from_slice(&ciphertext);
    Ok(packet)
}

#[derive(Default)]
pub struct ReplayWindow {
    newest: Option<u64>,
    seen: u64,
}

impl ReplayWindow {
    pub fn accept(&mut self, sequence: u64) -> bool {
        let Some(newest) = self.newest else {
            self.newest = Some(sequence);
            self.seen = 1;
            return true;
        };
        if sequence > newest {
            let shift = sequence - newest;
            self.seen = if shift >= u64::BITS as u64 {
                1
            } else {
                (self.seen << shift) | 1
            };
            self.newest = Some(sequence);
            return true;
        }
        let age = newest - sequence;
        if age >= u64::BITS as u64 {
            return false;
        }
        let mask = 1u64 << age;
        if self.seen & mask != 0 {
            return false;
        }
        self.seen |= mask;
        true
    }
}

pub fn open_media_datagram(
    key: &[u8; KEY_LEN],
    replay: &mut ReplayWindow,
    packet: &[u8],
) -> Result<(u64, Vec<u8>), CryptoError> {
    if packet.len() < 12 + TAG_LEN || &packet[..4] != MEDIA_MAGIC {
        return Err(CryptoError::InvalidCiphertext);
    }
    let sequence = u64::from_le_bytes(packet[4..12].try_into().unwrap());
    let plaintext = open(key, sequence, &packet[..12], &packet[12..])?;
    if !replay.accept(sequence) {
        return Err(CryptoError::Authentication);
    }
    Ok((sequence, plaintext))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pairing_authentication_is_nonce_bound() {
        let key = [7; KEY_LEN];
        let client_nonce = [1; 32];
        let server_nonce = [2; 32];
        let client_proof = client_authenticator(&key, &client_nonce);
        assert!(verify_client_authenticator(&key, &client_nonce, &client_proof).is_ok());
        assert_eq!(
            verify_client_authenticator(&key, &[3; 32], &client_proof),
            Err(CryptoError::Authentication)
        );
        assert_eq!(
            verify_client_authenticator(&[8; KEY_LEN], &client_nonce, &client_proof),
            Err(CryptoError::Authentication)
        );

        let server_proof = server_authenticator(&key, &client_nonce, &server_nonce);
        assert!(verify_server_authenticator(
            &key,
            &client_nonce,
            &server_nonce,
            &server_proof
        )
        .is_ok());
        assert_eq!(
            verify_server_authenticator(&key, &client_nonce, &[4; 32], &server_proof),
            Err(CryptoError::Authentication)
        );
    }

    #[test]
    fn session_keys_are_nonce_and_direction_separated() {
        let key = [9; KEY_LEN];
        let client_nonce = [1; 32];
        let server_nonce = [2; 32];
        let keys = derive_session_keys(&key, &client_nonce, &server_nonce).unwrap();
        assert_ne!(keys.client_to_server_control, keys.server_to_client_control);
        assert_ne!(keys.client_to_server_control, keys.client_to_server_media);
        assert_ne!(
            keys,
            derive_session_keys(&key, &client_nonce, &[3; 32]).unwrap()
        );
        assert_eq!(
            keys,
            derive_session_keys(&key, &client_nonce, &server_nonce).unwrap()
        );
    }

    #[test]
    fn aead_authenticates_ciphertext_and_associated_data() {
        let key = [5; KEY_LEN];
        let plaintext = b"kitvr media packet";
        let aad = b"KitVR v2 media";
        let ciphertext = seal(&key, 42, aad, plaintext).unwrap();
        assert_eq!(ciphertext.len(), plaintext.len() + TAG_LEN);
        assert_eq!(open(&key, 42, aad, &ciphertext).unwrap(), plaintext);
        assert_eq!(
            open(&key, 42, b"wrong channel", &ciphertext),
            Err(CryptoError::InvalidCiphertext)
        );
        assert_eq!(
            open(&[7; KEY_LEN], 42, aad, &ciphertext),
            Err(CryptoError::InvalidCiphertext)
        );
        let mut tampered = ciphertext;
        tampered[0] ^= 1;
        assert_eq!(
            open(&key, 42, aad, &tampered),
            Err(CryptoError::InvalidCiphertext)
        );
    }

    #[test]
    fn media_datagrams_reject_replays_but_allow_reordering() {
        let key = [6; KEY_LEN];
        let mut replay = ReplayWindow::default();
        let packet_10 = seal_media_datagram(&key, 10, b"ten").unwrap();
        let packet_12 = seal_media_datagram(&key, 12, b"twelve").unwrap();
        let packet_11 = seal_media_datagram(&key, 11, b"eleven").unwrap();

        assert_eq!(open_media_datagram(&key, &mut replay, &packet_10).unwrap().1, b"ten");
        assert_eq!(open_media_datagram(&key, &mut replay, &packet_12).unwrap().1, b"twelve");
        assert_eq!(open_media_datagram(&key, &mut replay, &packet_11).unwrap().1, b"eleven");
        assert_eq!(
            open_media_datagram(&key, &mut replay, &packet_10),
            Err(CryptoError::Authentication)
        );
        let mut tampered = seal_media_datagram(&key, 13, b"thirteen").unwrap();
        tampered[12] ^= 1;
        assert_eq!(
            open_media_datagram(&key, &mut replay, &tampered),
            Err(CryptoError::InvalidCiphertext)
        );
    }

    #[test]
    fn control_records_carry_a_checked_sequence() {
        let key = [8; KEY_LEN];
        let record = seal_control_record(&key, 4, b"control").unwrap();
        let length = u32::from_le_bytes(record[..4].try_into().unwrap()) as usize;
        let sequence = u64::from_le_bytes(record[4..12].try_into().unwrap());
        assert_eq!(length + 4, record.len());
        assert_eq!(sequence, 4);
        let mut aad = CONTROL_AAD.to_vec();
        aad.extend_from_slice(&sequence.to_le_bytes());
        assert_eq!(open(&key, sequence, &aad, &record[12..]).unwrap(), b"control");
    }
}
