package com.crystalkitsune.kitvr;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public final class SecureChannel {
    private static final byte[] CLIENT_LABEL =
            "KitVR v2 client authentication".getBytes(StandardCharsets.UTF_8);
    private static final byte[] SERVER_LABEL =
            "KitVR v2 server authentication".getBytes(StandardCharsets.UTF_8);

    private final byte[] clientControl;
    private final byte[] serverControl;
    private final byte[] clientMedia;
    private final byte[] serverMedia;
    private final boolean client;

    public SecureChannel(byte[] pairingKey, byte[] clientNonce, byte[] serverNonce,
                         boolean isClient) throws Exception {
        if (pairingKey.length != 32 || clientNonce.length != 32 || serverNonce.length != 32) {
            throw new IllegalArgumentException("Invalid KitVR v2 key material");
        }
        byte[] salt = new byte[64];
        System.arraycopy(clientNonce, 0, salt, 0, 32);
        System.arraycopy(serverNonce, 0, salt, 32, 32);
        byte[] prk = hmac(salt, pairingKey);
        clientControl = expand(prk, "KitVR v2 client-to-server control");
        serverControl = expand(prk, "KitVR v2 server-to-client control");
        clientMedia = expand(prk, "KitVR v2 client-to-server media");
        serverMedia = expand(prk, "KitVR v2 server-to-client media");
        client = isClient;
        Arrays.fill(prk, (byte) 0);
    }

    public static byte[] clientAuthenticator(byte[] key, byte[] clientNonce) throws Exception {
        return hmac(key, concat(CLIENT_LABEL, clientNonce));
    }

    public static boolean verifyServerAuthenticator(byte[] key, byte[] clientNonce,
                                                     byte[] serverNonce, byte[] proof)
            throws Exception {
        return MessageDigest.isEqual(
                hmac(key, concat(SERVER_LABEL, clientNonce, serverNonce)), proof);
    }

    public byte[] sealControl(long sequence, byte[] plaintext) throws Exception {
        return seal(controlKey(true), sequence,
                concat("KitVR v2 control".getBytes(StandardCharsets.UTF_8),
                        longBytes(sequence)), plaintext);
    }

    public byte[] openControl(long sequence, byte[] ciphertext) throws Exception {
        return open(controlKey(false), sequence,
                concat("KitVR v2 control".getBytes(StandardCharsets.UTF_8),
                        longBytes(sequence)), ciphertext);
    }

    public byte[] sealMedia(long sequence, byte[] header, byte[] plaintext) throws Exception {
        return seal(mediaKey(true), sequence, header, plaintext);
    }

    public byte[] openMedia(long sequence, byte[] header, byte[] ciphertext) throws Exception {
        return open(mediaKey(false), sequence, header, ciphertext);
    }

    private byte[] controlKey(boolean sending) {
        boolean clientToServer = client == sending;
        return clientToServer ? clientControl : serverControl;
    }

    private byte[] mediaKey(boolean sending) {
        boolean clientToServer = client == sending;
        return clientToServer ? clientMedia : serverMedia;
    }

    private static byte[] seal(byte[] key, long sequence, byte[] aad, byte[] plaintext)
            throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"),
                new GCMParameterSpec(128, nonce(sequence)));
        cipher.updateAAD(aad);
        return cipher.doFinal(plaintext);
    }

    private static byte[] open(byte[] key, long sequence, byte[] aad, byte[] ciphertext)
            throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"),
                new GCMParameterSpec(128, nonce(sequence)));
        cipher.updateAAD(aad);
        return cipher.doFinal(ciphertext);
    }

    private static byte[] nonce(long sequence) {
        return ByteBuffer.allocate(12).order(ByteOrder.BIG_ENDIAN)
                .putInt(0).putLong(sequence).array();
    }

    private static byte[] hmac(byte[] key, byte[] input) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(input);
    }

    private static byte[] expand(byte[] prk, String info) throws Exception {
        byte[] input = concat(info.getBytes(StandardCharsets.UTF_8), new byte[]{1});
        return hmac(prk, input);
    }

    private static byte[] concat(byte[]... parts) {
        int length = 0;
        for (byte[] part : parts) length += part.length;
        byte[] result = new byte[length];
        int offset = 0;
        for (byte[] part : parts) {
            System.arraycopy(part, 0, result, offset, part.length);
            offset += part.length;
        }
        return result;
    }

    private static byte[] longBytes(long value) {
        return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array();
    }
}
