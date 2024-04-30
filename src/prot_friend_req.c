#include <stdlib.h>
#include <onion.h>
#include <db_options.h>
#include <db_contact.h>
#include <sys_memory.h>
#include <prot_main.h>
#include <sqlite3.h>
#include <prot_ack.h>
#include <prot_friend_req.h>
#include <buffer_crypto.h>
#include <event2/buffer.h>
#include <debug.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/encoder.h>

// Called when ACK message is received (or cleaned up)
static void ack_received_cb(int ack_success, void *arg) {
    struct prot_friend_req *msg = arg;

    if (ack_success) {
        db_contact_save(msg->db, msg->friend);
    }

    prot_friend_req_free(msg);
}

// Called if friend request is send successfully
static void tran_done(struct prot_main *pmain, struct prot_tran_handler *phand) {
    struct prot_ack_ed25519 *ack;
    struct prot_friend_req *msg = phand->msg;

    // Will be cleaned up by ack messages
    phand->cleanup_cb = NULL;

    ack = prot_ack_ed25519_new(PROT_ACK_ONION, msg->friend->onion_pub_key, NULL, ack_received_cb, msg);
    prot_main_push_recv(pmain, &(ack->hrecv));
}

// Free friend request object
static void tran_cleanup(struct prot_tran_handler *phand) {
    struct prot_friend_req *msg = phand->msg;
    prot_friend_req_free(msg);
}

// Called to build friend request message and put it into buffer
static void tran_setup(struct prot_main *pmain, struct prot_tran_handler *phand) {
    size_t len;
    uint8_t *key_ptr;
    uint8_t nick_len;
    char nick[CLIENT_NICK_MAX_LEN + 1];
    struct prot_friend_req *msg = phand->msg;

    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *keyctx;

    OSSL_ENCODER_CTX *encctx;

    char onion_address[ONION_ADDRESS_LEN];
    char mb_onion_address[ONION_ADDRESS_LEN];
    uint8_t mb_id[MAILBOX_ID_LEN];
    uint8_t onion_priv_key[ONION_PRIV_KEY_LEN];

    // Generate ED25519 keypair
    if (
        !(keyctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL)) ||
        !EVP_PKEY_keygen_init(keyctx) ||
        !EVP_PKEY_generate(keyctx, &pkey)
    )
        sys_openssl_crash("Failed to generate ED25519 in friend request");

    len = CLIENT_SIG_KEY_PUB_LEN;
    if (!EVP_PKEY_get_raw_public_key(pkey, msg->friend->local_sig_key_pub, &len))
        sys_openssl_crash("Failed to extract public ED25519 key in friend request");

    len = CLIENT_SIG_KEY_PRIV_LEN;
    if (!EVP_PKEY_get_raw_private_key(pkey, msg->friend->local_sig_key_priv, &len))
        sys_openssl_crash("Failed to extract private ED25519 key in friend request");;

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(keyctx);

    // Generate RSA 2048bit keypair
    if (!(pkey = EVP_RSA_gen(2048)))
        sys_openssl_crash("Failed to create RSA keypair in friend request");

    if (!(encctx = OSSL_ENCODER_CTX_new_for_pkey(pkey, EVP_PKEY_PUBLIC_KEY, "DER", NULL, NULL)))
        sys_openssl_crash("Failed to create DER encoder for public key in friend req");

    len = CLIENT_ENC_KEY_PUB_LEN;
    key_ptr = msg->friend->local_enc_key_pub;
    if (!OSSL_ENCODER_to_data(encctx, &key_ptr, &len))
        sys_openssl_crash("Failed to encode RSA public key to DER");
    OSSL_ENCODER_CTX_free(encctx);

    if (!(encctx = OSSL_ENCODER_CTX_new_for_pkey(pkey, EVP_PKEY_KEYPAIR, "DER", NULL, NULL)))
        sys_openssl_crash("Failed to create DER encoder for private key in friend req");

    len = CLIENT_ENC_KEY_PRIV_LEN;
    key_ptr = msg->friend->local_enc_key_priv;
    if (!OSSL_ENCODER_to_data(encctx, &key_ptr, &len))
        sys_openssl_crash("Failed to encode RSA private key to DER");

    OSSL_ENCODER_CTX_free(encctx);
    EVP_PKEY_free(pkey);

    // Fetch data from the database
    db_options_get_text(msg->db, "onion_address", onion_address, ONION_ADDRESS_LEN);
    db_options_get_bin(msg->db, "onion_private_key", onion_priv_key, ONION_PRIV_KEY_LEN);
    db_options_get_bin(msg->db, "mailbox_id", mb_id, MAILBOX_ID_LEN);
    db_options_get_bin(msg->db, "mailbox_onion_address", mb_onion_address, ONION_ADDRESS_LEN);
    nick_len = db_options_get_text(msg->db, "nickname", nick, CLIENT_NICK_MAX_LEN);

    // Stuff all data into buffer
    evbuffer_add(phand->buffer, prot_header(PROT_FRIEND_REQUEST), PROT_HEADER_LEN);
    evbuffer_add(phand->buffer, pmain->transaction_id, TRANSACTION_ID_LEN);
    evbuffer_add(phand->buffer, onion_address, ONION_ADDRESS_LEN);
    evbuffer_add(phand->buffer, msg->friend->local_sig_key_pub, CLIENT_SIG_KEY_PUB_LEN);
    evbuffer_add(phand->buffer, msg->friend->local_enc_key_pub, CLIENT_ENC_KEY_PUB_LEN);

    evbuffer_add(phand->buffer, mb_onion_address, ONION_ADDRESS_LEN);
    evbuffer_add(phand->buffer, mb_id, MAILBOX_ID_LEN);

    evbuffer_add(phand->buffer, &nick_len, sizeof(nick_len));
    evbuffer_add(phand->buffer, nick, nick_len);
    
    // Sign the buffer
    db_contact_onion_extract_key(msg->friend);
    ed25519_buffer_sign(phand->buffer, 0, onion_priv_key);
}

// Free friend request object
static void recv_cleanup(struct prot_recv_handler *phand) {
    struct prot_friend_req *msg = phand->msg;
    prot_friend_req_free(msg);
}

static void ack_sent_cb(int ack_success, void *arg) {
    struct prot_friend_req *msg = arg;

    if (ack_success) {
        db_contact_save(msg->db, msg->friend);
    }

    prot_friend_req_free(msg);
}

// Called to handle incomming friend request
static void recv_handle(struct prot_main *pmain, struct prot_recv_handler *phand) {
    struct evbuffer *input;
    struct prot_friend_req *msg = phand->msg;
    struct prot_ack_ed25519 *ack;

    uint8_t onion_priv_key[ONION_PRIV_KEY_LEN];

    uint8_t *onion_addr_ptr;
    uint8_t received_onion_key[ONION_PUB_KEY_LEN];
    char received_onion_address[ONION_ADDRESS_LEN];

    uint8_t nick_len;
    uint8_t *message_static;

    // Length of all fields before nickname field
    int message_len = PROT_HEADER_LEN + TRANSACTION_ID_LEN + ONION_ADDRESS_LEN + 
        CLIENT_SIG_KEY_PUB_LEN + CLIENT_ENC_KEY_PUB_LEN + ONION_ADDRESS_LEN + MAILBOX_ID_LEN + 1;

    input = bufferevent_get_input(pmain->bev);

    if (evbuffer_get_length(input) < message_len)
        return;

    message_static = evbuffer_pullup(input, message_len);
    nick_len = message_static[message_len - 1]; // Last byte of static fields

    // If nick is too large
    if (nick_len > CLIENT_NICK_MAX_LEN) {
        prot_main_set_error(pmain, PROT_ERR_INVALID_MSG); 
        return;
    }

    // Add nickname and signature length
    message_len += nick_len + ED25519_SIGNATURE_LEN;

    // Wait for all data
    if (evbuffer_get_length(input) < message_len)
        return;

    onion_addr_ptr = message_static + PROT_HEADER_LEN + TRANSACTION_ID_LEN;

    if (!onion_address_valid(onion_addr_ptr)) {
        prot_main_set_error(pmain, PROT_ERR_INVALID_MSG); 
        return;
    }

    onion_extract_key(onion_addr_ptr, received_onion_key);
    if (!ed25519_buffer_validate(input, message_len, received_onion_key)) {
        prot_main_set_error(pmain, PROT_ERR_INVALID_MSG);
        return;
    }

    evbuffer_drain(input, PROT_HEADER_LEN + TRANSACTION_ID_LEN);
    evbuffer_remove(input, received_onion_address, ONION_ADDRESS_LEN);

    // Check for this onion on the database
    msg->friend = db_contact_get_by_onion(msg->db, received_onion_address, NULL);
    if (!msg->friend) {
        msg->friend = db_contact_new();
        memcpy(msg->friend->onion_address, received_onion_address, ONION_ADDRESS_LEN);
    }

    evbuffer_remove(input, msg->friend->remote_sig_key_pub, CLIENT_SIG_KEY_PUB_LEN);
    evbuffer_remove(input, msg->friend->remote_enc_key_pub, CLIENT_ENC_KEY_PUB_LEN);

    evbuffer_remove(input, msg->friend->mailbox_onion, ONION_ADDRESS_LEN);
    evbuffer_remove(input, msg->friend->mailbox_id, MAILBOX_ID_LEN);
    msg->friend->has_mailbox = !!(msg->friend->mailbox_id[0]);

    // Get nickname
    evbuffer_drain(input, 1); // Drain nickname length (we already have it)
    msg->friend->nickname_len = nick_len;
    evbuffer_remove(input, msg->friend->nickname, nick_len);
    // Drain signature
    evbuffer_drain(input, ED25519_SIGNATURE_LEN);

    // Get local user's onion priv key from database
    db_options_get_bin(msg->db, "onion_private_key", onion_priv_key, ONION_PRIV_KEY_LEN);

    // Time to send ACK, cleanup is disabled since ACK will do it
    phand->cleanup_cb = NULL;
    ack = prot_ack_ed25519_new(PROT_ACK_ONION, NULL, onion_priv_key, ack_sent_cb, msg);
    prot_main_push_tran(pmain, &(ack->htran));
    pmain->current_recv_done = 1;
}

// Allocate new friend request handler
struct prot_friend_req * prot_friend_req_new(sqlite3 *db, const char *onion_address) {
    struct prot_friend_req *msg;

    msg = safe_malloc(sizeof(struct prot_friend_req), 
        "Failed to allocate new friend request message");
    memset(msg, 0, sizeof(struct prot_friend_req));

    msg->db = db;

    if (onion_address) {
        msg->friend = db_contact_get_by_onion(db, onion_address, NULL);

        if (!msg->friend) {
            msg->friend = db_contact_new();
            memcpy(msg->friend->onion_address, onion_address, ONION_ADDRESS_LEN);
        }
    }

    msg->hrecv.msg = msg;
    msg->hrecv.msg_code = PROT_FRIEND_REQUEST;
    msg->hrecv.require_transaction = 1;
    msg->hrecv.handle_cb = recv_handle;
    msg->hrecv.cleanup_cb = recv_cleanup;

    msg->htran.msg = msg;
    msg->htran.msg_code = PROT_FRIEND_REQUEST;
    msg->htran.done_cb = tran_done;
    msg->htran.setup_cb = tran_setup;
    msg->htran.cleanup_cb = tran_cleanup;
    msg->htran.buffer = evbuffer_new();

    return msg;
}

// Free memory for given friend request
void prot_friend_req_free(struct prot_friend_req *msg) {
    if (msg && msg->friend)
        db_contact_free(msg->friend);
    if (msg && msg->htran.buffer)
        evbuffer_free(msg->htran.buffer);

    free(msg);
}