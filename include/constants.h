#ifndef _INCLUDE_CONSTANTS_H_
#define _INCLUDE_CONSTANTS_H_

/**
 * Application constants
 */

// Onion service will expose this port
#define DEEP_MESSENGER_PORT 20425
// Globaly used protocol version
#define DEEP_MESSENGER_PROTOCOL_VER 1
// Onion service will expose this port when running mailbox
#define DEEP_MESSENGER_MAILBOX_PORT 20426

/**
 * Cryptography
 */

// ED25519 keys
#define ED25519_PUB_KEY_LEN   32
#define ED25519_PRIV_KEY_LEN  32
#define ED25519_SIGNATURE_LEN 64

/**
 * Mailbox service specific constants
 */

// Length of string used to identify mailbox account
#define MAILBOX_ID_LEN         16
// Length of mailbox access key (used to register on mailbox)
#define MAILBOX_ACCESS_KEY_LEN 25

// A key supplied to mailbox on registration (ED25519)
#define MAILBOX_ACCOUNT_KEY_PUB_LEN  32
// Used for signing when communicating with mailbox (ED25519)
#define MAILBOX_ACCOUNT_KEY_PRIV_LEN 64

/**
 * Client and 
 */

// Transaction id generated for each transaction (connection)
#define TRANSACTION_ID_LEN 16

// Global message id
#define MESSAGE_ID_LEN 16

// Max length of user nickname
#define CLIENT_NICK_MAX_LEN 255

// Key pair for signing (ED25519), exchanged during friend request
#define CLIENT_SIG_KEY_PUB_LEN  32
#define CLIENT_SIG_KEY_PRIV_LEN 64

// Key pair for encryption (RSA), exchanged during friend request
#define CLIENT_ENC_KEY_PUB_LEN  526
#define CLIENT_ENC_KEY_PRIV_LEN 2348

#endif