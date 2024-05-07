#include <stdlib.h>
#include <stdint.h>
#include <event2/buffer.h>
#include <buffer_crypto.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/decoder.h>
#include <sys_memory.h>
#include <constants.h>
#include <debug.h>
#include <helpers.h>
#include <helpers_crypto.h>

// Take first len bytes in the given buffer, sign them using provided ed25519
// private key and add signature to the end of buffer
int ed25519_buffer_sign(struct evbuffer *buff, size_t len, uint8_t *priv_key) {
    int i, is_err = 0;
    int n_iv;
    struct evbuffer_iovec *iv = NULL;

    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *ctx = NULL;
    EVP_MD_CTX *hashctx = NULL;

    uint8_t hash[EVP_MAX_MD_SIZE];
    int hash_len = EVP_MAX_MD_SIZE;

    uint8_t sig[ED25519_SIGNATURE_LEN];
    size_t sig_len = ED25519_SIGNATURE_LEN;

    if (len == 0)
        len = evbuffer_get_length(buff);

    hashctx = EVP_MD_CTX_new();
    if (!EVP_DigestInit_ex2(hashctx, EVP_sha512(), NULL)) {
        is_err = 1; goto err;
    }

    if (!(pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv_key, ED25519_PRIV_KEY_LEN))) {
        is_err = 1; goto err;
    }

    ctx = EVP_MD_CTX_new();
    if (!EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey)) {
        is_err = 1; goto err;
    }

    n_iv = evbuffer_peek(buff, len, NULL, NULL, 0);
    iv = safe_malloc((sizeof(struct evbuffer_iovec) * n_iv),
        "Failed to allocate memory for evbuffer iovec(s), on buffer sign");

    n_iv = evbuffer_peek(buff, len, NULL, iv, n_iv);

    for (i = 0; i < n_iv; i++) {
        if (!EVP_DigestUpdate(hashctx, iv[i].iov_base, min(iv[i].iov_len, len))) {
            is_err = 1; goto err;
        }
        len -= iv[i].iov_len;
    }

    if (!EVP_DigestFinal_ex(hashctx, hash, &hash_len)) {
        is_err = 1; goto err;
    }

    EVP_DigestSign(ctx, sig, &sig_len, hash, hash_len);
    evbuffer_add(buff, sig, sig_len);

    err:
    free(iv);
    EVP_MD_CTX_free(ctx);
    EVP_MD_CTX_free(hashctx);
    EVP_PKEY_free(pkey);

    if (is_err) {
        debug("An error occured while signing the buffer: %s", 
            ERR_error_string(ERR_get_error(), NULL));
        return 1;
    }

    return 0;
}

// Takes last ED25519_SIGNATURE_LEN bytes as a ed25519 signature and validates
// it against other data using provided ed25519 public key
int ed25519_buffer_validate(struct evbuffer *buff, size_t len, uint8_t *pub_key) {
    int i, j, n_iv, i_sig;
    int is_err = 0, is_valid = 0;
    struct evbuffer_ptr ptr;
    struct evbuffer_iovec *iv;
    size_t content_len = len - ED25519_SIGNATURE_LEN;

    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *ctx = NULL;
    EVP_MD_CTX *hashctx = NULL;

    uint8_t hash[EVP_MAX_MD_SIZE];
    int hash_len = EVP_MAX_MD_SIZE;

    uint8_t sig[ED25519_SIGNATURE_LEN];

    if (len == 0)
        len = evbuffer_get_length(buff);

    if (!(pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub_key, ED25519_PUB_KEY_LEN))) {
        is_err = 1; goto err;
    }

    hashctx = EVP_MD_CTX_new();
    if (!EVP_DigestInit_ex2(hashctx, EVP_sha512(), NULL)) {
        is_err = 1; goto err;
    }

    evbuffer_ptr_set(buff, &ptr, content_len, EVBUFFER_PTR_SET);
    evbuffer_copyout_from(buff, &ptr, sig, ED25519_SIGNATURE_LEN);

    n_iv = evbuffer_peek(buff, content_len, NULL, NULL, 0);
    iv = safe_malloc((sizeof(struct evbuffer_iovec) * n_iv),
        "Failed to allocate memory for evbuffer iovec(s), on buffer verify");
    n_iv = evbuffer_peek(buff, content_len, NULL, iv, n_iv);

    for (i = 0; i < n_iv; i++) {
        if (!EVP_DigestUpdate(hashctx, iv[i].iov_base, min(iv[i].iov_len, content_len))) {
            is_err = 1; goto err;
        }
        content_len -= iv[i].iov_len;
    }

    if (!EVP_DigestFinal_ex(hashctx, hash, &hash_len)) {
        is_err = 1; goto err;
    }

    ctx = EVP_MD_CTX_new();
    if (!EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey)) {
        is_err = 1; goto err;
    };

    is_valid = EVP_DigestVerify(ctx, sig, ED25519_SIGNATURE_LEN, hash, hash_len);

    err:
    free(iv);
    EVP_MD_CTX_free(ctx);
    EVP_MD_CTX_free(hashctx);
    EVP_PKEY_free(pkey);

    if (is_err) {
        debug("An error occured while checking buffer signature: %s", 
            ERR_error_string(ERR_get_error(), NULL));
        return 0;
    }

    return is_valid;
}

// Takes RSA 2048bit key in DER format encrypts content of plain buffer and puts it into
// enc buffer in following format, used for sending it over network
//
//  >> DATA LEN (4 bytes)
//  >> DATA
//  >> DATA KEY (AES 256 bytes)
//  >> DATA IV  (16 bytes)
//
// returns 1 on success and 0 on failure
int rsa_buffer_encrypt(struct evbuffer *plain, uint8_t *der_pub_key, struct evbuffer *enc) {
    int i, temp_len, is_err = 0;
    uint32_t encrypted_len;

    int ek_len = 1;
    uint8_t *ek = NULL;
    uint8_t *iv = NULL;

    EVP_PKEY *pkey = NULL;
    EVP_CIPHER_CTX *cipctx = NULL;

    int n_vec_plain;
    struct evbuffer_iovec vec_enc;
    struct evbuffer_iovec *vec_plain = NULL;

    // Decode DER key
    if (!(pkey = rsa_2048bit_pub_key_decode(der_pub_key))) {
        is_err = 1; goto err;
    }

    encrypted_len = (evbuffer_get_length(plain) / EVP_CIPHER_block_size(EVP_aes_256_cbc()) + 1) * EVP_CIPHER_block_size(EVP_aes_256_cbc());
    encrypted_len = htonl(encrypted_len);
    evbuffer_add(enc, &encrypted_len, sizeof(encrypted_len));

    // Allocate space for symetric key and IV
    ek = safe_malloc(EVP_PKEY_get_size(pkey), "Failed to allocate PKEY buffer");
    iv = safe_malloc(EVP_CIPHER_get_iv_length(EVP_aes_256_cbc()), "Failed to allocate IV buffer");

    // Init seal operation
    if (
        !(cipctx = EVP_CIPHER_CTX_new()) ||
        !EVP_SealInit(cipctx, EVP_aes_256_cbc(), &ek, &ek_len, iv, &pkey, 1)
    ) {
        is_err = 1; goto err;
    }

    // Get buffer chunks
    n_vec_plain = evbuffer_peek(plain, evbuffer_get_length(plain), NULL, NULL, 0);
    vec_plain = safe_malloc(sizeof(struct evbuffer_iovec) * n_vec_plain, "Failed to allocate iovec");
    n_vec_plain = evbuffer_peek(plain, evbuffer_get_length(plain), NULL, vec_plain, n_vec_plain);

    // For each of the buffer chunks
    for (i = 0; i < n_vec_plain; i++) {
        size_t len = vec_plain[i].iov_len;

        // Add encrypted chunk to the end of enc buffer
        evbuffer_reserve_space(enc, len + EVP_CIPHER_block_size(EVP_aes_256_cbc()), &vec_enc, 1);
        temp_len = vec_enc.iov_len;
        if (!EVP_SealUpdate(cipctx, vec_enc.iov_base, &temp_len, vec_plain[i].iov_base, len)) {
            is_err = 1; goto err;
        }
        debug("FB: %x", ((uint8_t *)(vec_enc.iov_base))[0]);
        vec_enc.iov_len = temp_len;
        evbuffer_commit_space(enc, &vec_enc, 1);
    }

    // Write final block to the buffer
    evbuffer_reserve_space(enc, EVP_CIPHER_block_size(EVP_aes_256_cbc()), &vec_enc, 1);
    temp_len = vec_enc.iov_len;
    if (!EVP_SealFinal(cipctx, vec_enc.iov_base, &temp_len)) {
        is_err = 1; goto err;
    }
    vec_enc.iov_len = temp_len;
    evbuffer_commit_space(enc, &vec_enc, 1);

    evbuffer_add(enc, ek, ek_len);
    debug("STORING EK: %02x%02x", ek[0], ek[1]);
    evbuffer_add(enc, iv, EVP_CIPHER_get_iv_length(EVP_aes_256_cbc()));
    debug("STORING iv: %02x%02x", iv[0], iv[1]);

    // Free everything
    err:
    if (is_err) {
        debug("Error encrypt");
    }
    free(iv);
    free(ek);
    free(vec_plain);
    EVP_PKEY_free(pkey);
    EVP_CIPHER_CTX_free(cipctx);
    return !is_err;
}

// Takes buffer encrypted by rsa_buffer_encrypt function and decrypts it into plain buffer
// using provided RSA 2048bit key in DER format, returns 1 on success and 0 on failure
int rsa_buffer_decrypt(struct evbuffer *enc_buff, uint8_t *der_priv_key, struct evbuffer *plain_buff) {
    int i, is_err;
    size_t len;
    int len_int;

    int ekl;            // Encrypted symetric key
    uint8_t *ek = NULL;
    int ivl;            // IV number for AES algorithm
    uint8_t *iv = NULL;

    uint32_t encrypted_len;  // Length of cipher text
    struct evbuffer_ptr pos; // Evbuffer pointer

    int n_vec_enc;                   // Number of ciphertext chunks
    struct evbuffer_iovec *vec_enc;  // Ciphertext chunks
    struct evbuffer_iovec vec_plain; // Plaintext chunk used when decrypting

    EVP_PKEY *pkey_priv = NULL;
    EVP_CIPHER_CTX *cipctx = NULL; 

    // Decode DER encoded private key
    if (!(pkey_priv = rsa_2048bit_priv_key_decode(der_priv_key))) {
        is_err = 1; goto err;
        debug("Decode failed");
    }

    // Calculate symetric key and IV length
    ekl = EVP_PKEY_get_size(pkey_priv);
    ivl = EVP_CIPHER_get_iv_length(EVP_aes_256_cbc());
    // Allocate memory for symetric key and IV
    ek = safe_malloc(ekl, "Failed to allocate EK when decrypting buffer");
    iv = safe_malloc(ivl, "Failed to allocate IV when decrypting buffer");

    // If buffer is too small to specify ciphertext length
    if (evbuffer_get_length(enc_buff) < sizeof(encrypted_len)) {
        is_err = 1; goto err;
    }

    // Get length of cipher text
    evbuffer_copyout(enc_buff, &encrypted_len, sizeof(encrypted_len));
    encrypted_len = ntohl(encrypted_len);

    // If buffer is too small to contain all specified data
    if (evbuffer_get_length(enc_buff) < sizeof(encrypted_len) + encrypted_len + ekl + ivl) {
        is_err = 1; goto err;
    }

    // Pull out key and iv from the buffer
    evbuffer_ptr_set(enc_buff, &pos, sizeof(encrypted_len) + encrypted_len, EVBUFFER_PTR_SET);
    evbuffer_copyout_from(enc_buff, &pos, ek, ekl);
    evbuffer_ptr_set(enc_buff, &pos, sizeof(encrypted_len) + encrypted_len + ekl, EVBUFFER_PTR_SET);
    evbuffer_copyout_from(enc_buff, &pos, iv, ivl);

    // Get iovec array for encrypted data
    evbuffer_ptr_set(enc_buff, &pos, sizeof(encrypted_len), EVBUFFER_PTR_SET);
    n_vec_enc = evbuffer_peek(enc_buff, encrypted_len, &pos, NULL, 0);
    vec_enc = safe_malloc(sizeof(struct evbuffer_iovec) * n_vec_enc, "FAIL");
    n_vec_enc = evbuffer_peek(enc_buff, encrypted_len, &pos, vec_enc, n_vec_enc);

    // Init data decryption with given keys
    if (
        !(cipctx = EVP_CIPHER_CTX_new()) ||
        !EVP_OpenInit(cipctx, EVP_aes_256_cbc(), ek, ekl, iv, pkey_priv)
    ) {
        debug("Failed to init Open: %s", ERR_error_string(ERR_get_error(), NULL));
        is_err = 1; goto err;
    }

    // For each chunk of ciphertext
    for (i = 0; i < n_vec_enc; i++) {
        // Get data len
        len = (vec_enc[i].iov_len < encrypted_len) ? vec_enc[i].iov_len : encrypted_len;
        // Reserve space for the plain text
        evbuffer_reserve_space(plain_buff, len, &vec_plain, 1);
        // Decrypt chunk and commit plain text
        len_int = vec_plain.iov_len;
        if (EVP_OpenUpdate(cipctx, vec_plain.iov_base, &len_int, vec_enc[i].iov_base, len) == 0) {
            debug("Decryption step failed %s", ERR_error_string(ERR_get_error(), NULL));
            is_err = 1; goto err;
        }
        vec_plain.iov_len = len_int;
        evbuffer_commit_space(plain_buff, &vec_plain, 1);
    }

    // Decrypt final chunk
    evbuffer_reserve_space(plain_buff, EVP_CIPHER_block_size(EVP_aes_256_cbc()), &vec_plain, 1);
    len_int = vec_plain.iov_len;
    if (!EVP_OpenFinal(cipctx, vec_plain.iov_base, &len_int))
        debug("Open final failed");
    vec_plain.iov_len = len_int;
    evbuffer_commit_space(plain_buff, &vec_plain, 1);

    err:
    // Free all allocated memory
    free(ek);
    free(iv);
    EVP_PKEY_free(pkey_priv);
    EVP_CIPHER_CTX_free(cipctx);

    return !is_err;
}