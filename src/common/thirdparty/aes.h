/*********************************************************************
 * Filename:   aes.h
 * Author:     Brad Conte (brad AT bradconte.com)
 * Copyright:
 * Disclaimer: This code is presented "as is" without any guarantees.
 * Details:    Defines the API for the corresponding AES implementation.
 *********************************************************************/

#pragma once

/*************************** HEADER FILES ***************************/
#include <cstddef>
#include <cstdint>

/****************************** MACROS ******************************/
inline constexpr uint32_t AES_BLOCK_SIZE = 16; // AES operates on 16 bytes at a time
inline constexpr uint32_t AES_KEY_SCHEDULE_SIZE = 60;

/*********************** FUNCTION DECLARATIONS **********************/
///////////////////
// AES
///////////////////
// Key setup must be done before any AES en/de-cryption functions can be used.
void aes_key_setup(const uint8_t key[], // The key, must be 128, 192, or 256 bits
                   uint32_t w[],        // Output key schedule to be used later
                   int keysize);        // Bit length of the key, 128, 192, or 256

void aes_encrypt(const uint8_t in[],   // 16 bytes of plaintext
                 uint8_t out[],        // 16 bytes of ciphertext
                 const uint32_t key[], // From the key setup
                 int keysize);         // Bit length of the key, 128, 192, or 256

void aes_decrypt(const uint8_t in[],   // 16 bytes of ciphertext
                 uint8_t out[],        // 16 bytes of plaintext
                 const uint32_t key[], // From the key setup
                 int keysize);         // Bit length of the key, 128, 192, or 256

///////////////////
// AES - CBC
///////////////////
bool aes_encrypt_cbc(const uint8_t in[],   // Plaintext
                     size_t in_len,        // Must be a multiple of AES_BLOCK_SIZE
                     uint8_t out[],        // Ciphertext, same length as plaintext
                     const uint32_t key[], // From the key setup
                     int keysize,          // Bit length of the key, 128, 192, or 256
                     const uint8_t iv[]);  // IV, must be AES_BLOCK_SIZE bytes long
bool aes_decrypt_cbc(const uint8_t in[],   // Ciphertext
                     size_t in_len,        // Must be a multiple of AES_BLOCK_SIZE
                     uint8_t out[],        // Plaintext, same length as ciphertext
                     const uint32_t key[], // From the key setup
                     int keysize,          // Bit length of the key, 128, 192, or 256
                     const uint8_t iv[]);  // IV, must be AES_BLOCK_SIZE bytes long

#if 0
// Disabled since it's not being used.
// Only output the CBC-MAC of the input.
bool aes_encrypt_cbc_mac(const uint8_t in[],   // plaintext
                         size_t in_len,        // Must be a multiple of AES_BLOCK_SIZE
                         uint8_t out[],        // Output MAC
                         const uint32_t key[], // From the key setup
                         int keysize,          // Bit length of the key, 128, 192, or 256
                         const uint8_t iv[]);  // IV, must be AES_BLOCK_SIZE bytes long

///////////////////
// AES - CTR
///////////////////
void aes_increment_iv(uint8_t iv[],      // Must be a multiple of AES_BLOCK_SIZE
                  int counter_size); // Bytes of the IV used for counting (low end)

void aes_encrypt_ctr(const uint8_t in[],   // Plaintext
                     size_t in_len,        // Any byte length
                     uint8_t out[],        // Ciphertext, same length as plaintext
                     const uint32_t key[], // From the key setup
                     int keysize,          // Bit length of the key, 128, 192, or 256
                     const uint8_t iv[]);  // IV, must be AES_BLOCK_SIZE bytes long

void aes_decrypt_ctr(const uint8_t in[],   // Ciphertext
                     size_t in_len,        // Any byte length
                     uint8_t out[],        // Plaintext, same length as ciphertext
                     const uint32_t key[], // From the key setup
                     int keysize,          // Bit length of the key, 128, 192, or 256
                     const uint8_t iv[]);  // IV, must be AES_BLOCK_SIZE bytes long

///////////////////
// AES - CCM
///////////////////
// Returns True if the input parameters do not violate any constraint.
bool aes_encrypt_ccm(
  const uint8_t plaintext[],          // IN  - Plaintext.
  uint32_t plaintext_len,             // IN  - Plaintext length.
  const uint8_t associated_data[],    // IN  - Associated Data included in authentication, but not encryption.
  unsigned short associated_data_len, // IN  - Associated Data length in bytes.
  const uint8_t nonce[],              // IN  - The Nonce to be used for encryption.
  unsigned short nonce_len,           // IN  - Nonce length in bytes.
  uint8_t ciphertext[],               // OUT - Ciphertext, a concatination of the plaintext and the MAC.
  uint32_t* ciphertext_len,           // OUT - The length of the ciphertext, always plaintext_len + mac_len.
  uint32_t mac_len,                   // IN  - The desired length of the MAC, must be 4, 6, 8, 10, 12, 14, or 16.
  const uint8_t key[],                // IN  - The AES key for encryption.
  int keysize);                       // IN  - The length of the key in bits. Valid values are 128, 192, 256.

// Returns True if the input parameters do not violate any constraint.
// Use mac_auth to ensure decryption/validation was preformed correctly.
// If authentication does not succeed, the plaintext is zeroed out. To overwride
// this, call with mac_auth = NULL. The proper proceedure is to decrypt with
// authentication enabled (mac_auth != NULL) and make a second call to that
// ignores authentication explicitly if the first call failes.
bool aes_decrypt_ccm(
  const uint8_t ciphertext[], // IN  - Ciphertext, the concatination of encrypted plaintext and MAC.
  uint32_t ciphertext_len,    // IN  - Ciphertext length in bytes.
  const uint8_t assoc[],      // IN  - The Associated Data, required for authentication.
  unsigned short assoc_len,   // IN  - Associated Data length in bytes.
  const uint8_t nonce[],      // IN  - The Nonce to use for decryption, same one as for encryption.
  unsigned short nonce_len,   // IN  - Nonce length in bytes.
  uint8_t plaintext[], // OUT - The plaintext that was decrypted. Will need to be large enough to hold ciphertext_len -
                       // mac_len.
  uint32_t* plaintext_len, // OUT - Length in bytes of the output plaintext, always ciphertext_len - mac_len .
  uint32_t mac_len,        // IN  - The length of the MAC that was calculated.
  int* mac_auth,           // OUT - TRUE if authentication succeeded, FALSE if it did not. NULL pointer will ignore the
                           // authentication.
  const uint8_t key[],     // IN  - The AES key for decryption.
  int keysize);            // IN  - The length of the key in BITS. Valid values are 128, 192, 256.
#endif
