import sys

file_path = "src/components/wolfssl/src/quic.c"
with open(file_path, "r") as f:
    content = f.read()

replacement = """int wolfSSL_quic_aead_encrypt(uint8_t* dest, WOLFSSL_EVP_CIPHER_CTX* ctx,
                              const uint8_t* plain, size_t plainlen,
                              const uint8_t* iv, const uint8_t* aad,
                              size_t aadlen)
{
    int len;
    
    // Allocate temporary buffer for plain text to avoid in-place encryption issues with ESP32 DMA
    uint8_t* temp_plain = (uint8_t*)malloc(plainlen);
    if (!temp_plain) return WOLFSSL_FAILURE;
    memcpy(temp_plain, plain, plainlen);

    printf("QUIC ENCRYPT: plainlen=%d, first bytes: %02x %02x %02x %02x\\n", 
           (int)plainlen, plain[0], plain[1], plain[2], plain[3]);

    if (wolfSSL_EVP_CipherInit(ctx, NULL, NULL, iv, 1) != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CipherUpdate(
                ctx, NULL, &len, aad, (int)aadlen) != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CipherUpdate(
                ctx, dest, &len, temp_plain, (int)plainlen) != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CipherFinal(ctx, dest + len, &len) != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CIPHER_CTX_ctrl(
                ctx, WOLFSSL_EVP_CTRL_AEAD_GET_TAG, ctx->authTagSz, dest + plainlen)
           != WOLFSSL_SUCCESS) {
        free(temp_plain);
        return WOLFSSL_FAILURE;
    }
    
    free(temp_plain);
    return WOLFSSL_SUCCESS;
}"""

import re
pattern = r"int wolfSSL_quic_aead_encrypt\(uint8_t\* dest, WOLFSSL_EVP_CIPHER_CTX\* ctx,\s*const uint8_t\* plain, size_t plainlen,\s*const uint8_t\* iv, const uint8_t\* aad,\s*size_t aadlen\)\s*\{\s*int len;\s*// Allocate.*?return WOLFSSL_SUCCESS;\s*\}"

content = re.sub(pattern, replacement, content, flags=re.DOTALL)

with open(file_path, "w") as f:
    f.write(content)
print("Patched!")
