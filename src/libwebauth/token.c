#include "webauthp.h"

#include <sys/types.h>
#include <time.h>
#include <netinet/in.h>
#include <inttypes.h>

#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

/* 
 *  define some macros for offsets (_O) and sizes (_S) in token
 *  {key-hint}{nonce}{hmac}{token-attributes}{padding} 
 */

#define T_HINT_S  4
#define T_NONCE_S 16
#define T_HMAC_S (SHA_DIGEST_LENGTH)

#define T_HINT_O 0
#define T_NONCE_O (T_HINT_O+T_HINT_S)
#define T_HMAC_O (T_NONCE_O+T_NONCE_S)
#define T_ATTR_O (T_HMAC_O+T_HMAC_S)


static int 
binary_encoded_length(const WEBAUTH_ATTR_LIST *list,
                      int *plen)
{
    int len, m;

    /* calculate encrypted data length first */

    /* get length of encoded attrs first */
    len = webauth_attrs_encoded_length(list);

    /* add in nonce and hmac */
    len += T_NONCE_S+T_HMAC_S;

    /* add in padding length */
    m = len % AES_BLOCK_SIZE;
    if (m) {
        *plen = AES_BLOCK_SIZE - m;
    } else {
        *plen = AES_BLOCK_SIZE;
    }

    len += *plen;

    /* add in 4 bytes for creation time */
    len += 4;

    /* now return length including base64 */
    return len;
}

int
webauth_token_encoded_length(const WEBAUTH_ATTR_LIST *list)
{
    int plen;
    int blen;

    assert(list != NULL);
    assert(list->num_attrs);
    blen = binary_encoded_length(list, &plen);
    return webauth_base64_encoded_length(blen);
}

/*
 * encrypts and base64 encodes attrs into a token
 */
int
webauth_token_create(const WEBAUTH_ATTR_LIST *list,
                     unsigned char *output,
                     int max_output_len,
                     const WEBAUTH_KEY_RING *ring)
{
    unsigned char *ebuff;
    int elen, blen, plen, alen, n, i;
    uint32_t currt; /* sizeof MUST equal T_HINT_S */
    unsigned char temp_nonce[T_NONCE_S] = 
        {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

    /* ivec is always 0 since we use nonce as ivec */
    unsigned char aes_ivec[AES_BLOCK_SIZE] = 
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    AES_KEY aes_key;
    WEBAUTH_KEY *key;

    assert(list!= NULL);
    assert(list->num_attrs);
    assert(output != NULL);
    assert(max_output_len);
    assert(ring != NULL);

    /* find the best key */
    key = webauth_key_ring_best_encryption_key(ring);
    if (key == NULL) {
        return WA_ERR_BAD_KEY;
    }

    if (AES_set_encrypt_key(key->data, key->length*8, &aes_key)) {
        return WA_ERR_BAD_KEY;
    }

    /* {key-hint}{nonce}{hmac}{token-attributes}{padding} */

    elen = binary_encoded_length(list, &plen);
    blen = webauth_base64_encoded_length(elen);

    if (blen > max_output_len) {
        return WA_ERR_NO_ROOM;
    }

    ebuff = malloc(elen);
    if (ebuff == NULL) {
        return WA_ERR_NO_MEM;
    }

    n = 0;

    currt = htonl((uint32_t)time(NULL));

    /* FIXME: hint might need to optionally be passed in
       if we need to propagate it */

    /* copy in current time */
    memcpy(ebuff, &currt, T_HINT_S);
    n += T_HINT_S;

    /* copy in nonce */
    memcpy(ebuff+n, temp_nonce, T_NONCE_S);
    n += T_NONCE_S;

    /* leave room for hmac of data */
    n += T_HMAC_S;

    /* encode attributes */
    alen = webauth_attrs_encode(list, ebuff+T_ATTR_O, elen-n-plen);
    if (alen < 0) {
        free(ebuff);
        return alen;
    }

    n += alen;

    /* add padding for AES */
    for (i=0; i < plen; i++) {
        *(ebuff+n+i) = plen;
    }
    n += plen;

    /* calculate hmac over data+padding, using nonce as key
       FIXME: change hmac key to something better. might want
       to always carry around a an AES_KEY and an HMAC key, */

    /* HMAC doesn't return an errors */
    HMAC(EVP_sha1(), 
         (void*)temp_nonce, T_NONCE_S,  /* key, len */
         ebuff+T_ATTR_O, alen+plen,     /* data, len */
         ebuff+T_HMAC_O, NULL);         /* hmac, len (out) */

    /* now AES encrypt everything but the time at the front */
    /* AES_cbc_encrypt doesn't return anything */


    AES_cbc_encrypt(ebuff+T_NONCE_O,
                    ebuff+T_NONCE_O, /* encrypt in-place */
                    elen-T_HINT_S,
                    &aes_key,
                    aes_ivec, AES_ENCRYPT);

    /* now base64 token */
    blen = webauth_base64_encode(ebuff, elen, output, max_output_len);

    /* free buffer */
    free(ebuff);

    return blen;
}

/*
 * base64 decodes and decrypts attrs into a token
 * input buffer is modified, and the resulting
 * attrs point into it for their values.
 *
 * returns number of attrs in the resulting token
 * or an error
 *
 * FIXME: need to deal with key versions. We'll probably
 * want to change WEBUTH_AES_KEY to WEBAUTH_KEY_RING,
 * and let webauth_toke_parse pick the best key from the key ring
 */

int
webauth_token_parse(unsigned char *input,
                    int input_len,
                    WEBAUTH_ATTR_LIST **list,
                    const WEBAUTH_KEY_RING *ring)
{
    /* ivec is always 0 since we use nonce as ivec */
    unsigned char aes_ivec[AES_BLOCK_SIZE] = 
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    /* hmac we compute from data */
    unsigned char computed_hmac[T_HMAC_S];
    int plen, i, elen;
    AES_KEY aes_key;
    WEBAUTH_KEY *key;

    assert (key != NULL);
    assert(list);
    assert(ring);

    if (ring->num_entries == 0) {
        return WA_ERR_BAD_KEY;
    }

    /* FIXME: need to use key hint and potentially iterate through
       all keys in the ring to decrypt, for now, we use the first */

    key = ring->entries[0].key;

    if (AES_set_decrypt_key(key->data, key->length*8, &aes_key)) {
        return WA_ERR_BAD_KEY;
    }

    /** base64 decode (in place) first */
    elen=webauth_base64_decode(input, input_len, input, input_len);
    if (elen < 0) {
        return elen;
    }


    /* {key-hint}{nonce}{hmac}{token-attributes}{padding} */

    /* decrypt using our key */
    /* now AES decrypt everything but the time at the front */
    /* AES_cbc_encrypt doesn't return anything useful */
    AES_cbc_encrypt(input+T_NONCE_O,
                    input+T_NONCE_O,  /* decrypt in-place */
                    elen-T_HINT_S,
                    &aes_key,
                    aes_ivec, AES_DECRYPT);

    /* we now need to compute HMAC to see if decryption succeeded */

    /* calculate hmac over data+padding, using nonce as key */
    /* FIXME: change hmac key to something better */
    /* hamc doesn't return anything */
    HMAC(EVP_sha1(),
         (void*)input+T_NONCE_O, T_NONCE_S,  /* key, len */
         input+T_ATTR_O, elen-T_ATTR_O, /* data, len */
         computed_hmac, NULL);               /* hmac, len (out) */

    /* compare computed against decrypted */
    if (memcmp(input+T_HMAC_O, computed_hmac, T_HMAC_S) != 0) {
        return WA_ERR_BAD_HMAC;
    }

    /* check padding length validity */
    plen = *(input+elen-1);
    if (plen >= AES_BLOCK_SIZE || plen > elen) {
        return WA_ERR_CORRUPT;
    }
    /* check padding data validity */
    for (i=0; i < plen; i++) {
        if (*(input+elen-1-i) != plen) {
            return WA_ERR_CORRUPT;
        }
    }

    return webauth_attrs_decode(input+T_ATTR_O, elen-T_ATTR_O-plen, list);
}

/*
**  Local variables:
**  mode: c
**  c-basic-offset: 4
**  indent-tabs-mode: nil
**  end:
*/
