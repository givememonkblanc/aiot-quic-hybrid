import sys

file_path = "src/components/wolfssl/src/quic.c"
with open(file_path, "r") as f:
    content = f.read()

replacement = """int wolfSSL_quic_aead_decrypt(uint8_t* dest, WOLFSSL_EVP_CIPHER_CTX* ctx,
                              const uint8_t* enc, size_t enclen,
                              const uint8_t* iv, const uint8_t* aad,
                              size_t aadlen)
{
    int len;
    const uint8_t* tag;

    if (enclen > INT_MAX || ctx->authTagSz > (int)enclen) {
        return WOLFSSL_FAILURE;
    }

    enclen -= (size_t)ctx->authTagSz;
    tag = enc + enclen;
    
    // Allocate temporary buffer for cipher text to avoid in-place decryption issues with ESP32 DMA
    uint8_t* temp_enc = (uint8_t*)malloc(enclen);
    if (!temp_enc) return WOLFSSL_FAILURE;
    memcpy(temp_enc, enc, enclen);

    if (wolfSSL_EVP_CipherInit(ctx, NULL, NULL, iv, 0) != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CIPHER_CTX_ctrl(
                ctx, WOLFSSL_EVP_CTRL_AEAD_SET_TAG, ctx->authTagSz, (uint8_t*)tag)
            != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CipherUpdate(ctx, NULL, &len, aad, (int)aadlen)
            != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CipherUpdate(ctx, dest, &len, temp_enc, (int)enclen)
            != WOLFSSL_SUCCESS
        || wolfSSL_EVP_CipherFinal(ctx, dest, &len) != WOLFSSL_SUCCESS) {
        free(temp_enc);
        return WOLFSSL_FAILURE;
    }

    free(temp_enc);
    return WOLFSSL_SUCCESS;
}"""

import re
pattern = r"int wolfSSL_quic_aead_decrypt\(uint8_t\* dest, WOLFSSL_EVP_CIPHER_CTX\* ctx,\s*const uint8_t\* enc, size_t enclen,\s*const uint8_t\* iv, const uint8_t\* aad,\s*size_t aadlen\)\s*\{\s*int len;\s*const uint8_t\* tag;\s*/\*.*?return WOLFSSL_SUCCESS;\s*\}"

content = re.sub(pattern, replacement, content, flags=re.DOTALL)

with open(file_path, "w") as f:
    f.write(content)
print("Patched!")
