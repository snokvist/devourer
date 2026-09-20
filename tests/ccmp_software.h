#pragma once

#include <cstdint>

#include <openssl/evp.h>

// The software data-plane primitive used by tests/ap_wpa2.cpp.  Keep the
// benchmark on this function: benchmarking EVP_aes_128_ccm() through a
// different context-lifetime policy can give a very different answer on the
// small frames for which per-packet cost matters most.
namespace devourer::test {

inline bool ccmp_software(bool encrypt, const uint8_t* key,
                          const uint8_t* nonce, const uint8_t* aad,
                          int aad_len, const uint8_t* input, int input_len,
                          uint8_t* output, uint8_t* tag) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;

  int len = 0;
  bool ok = true;
  if (encrypt) {
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 13, nullptr) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 8, nullptr) == 1 &&
         EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
         EVP_EncryptUpdate(ctx, nullptr, &len, nullptr, input_len) == 1 &&
         EVP_EncryptUpdate(ctx, nullptr, &len, aad, aad_len) == 1 &&
         EVP_EncryptUpdate(ctx, output, &len, input, input_len) == 1 &&
         EVP_EncryptFinal_ex(ctx, output + len, &len) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 8, tag) == 1;
  } else {
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 13, nullptr) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 8, tag) == 1 &&
         EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
         EVP_DecryptUpdate(ctx, nullptr, &len, nullptr, input_len) == 1 &&
         EVP_DecryptUpdate(ctx, nullptr, &len, aad, aad_len) == 1 &&
         EVP_DecryptUpdate(ctx, output, &len, input, input_len) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

}  // namespace devourer::test
