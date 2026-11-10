#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
/* openssl/evp.h Commands and Data Types Used:
    Used for the md5 calculation
    -lcrypto included in the compile line
    EVP_MD_CTX_new(): Allocate context
    EVP_MD_CTX_free(): Free allocated space for context
    EVP_MAX_MD_SIZE
*/

static void digest_to_hex(const unsigned char *digest, unsigned int digest_len, char *out_hex)
{
    for (unsigned int i = 0; i < digest_len; i++)
    {
        // %02x keeps the leading zero, so 0x0a becoms 0a and not a
        sprintf(out_hex + (i * 2), "%02x", digest[i]);
    }

    out_hex[digest_len * 2] = "\0";
}

void compute_md5_hex(const unsigned char *data, size_t *len, unsigned char *out_hex)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new(); // Allocate the context space
    if (ctx == NULL)
    {
        return -1;
    }

    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 || EVP_DigestUpdate(ctx, data, len) != 1 || EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1)
    {
        EVP_MD_CTX_free(ctx); // Free on error
        return -1;
    }

    EVP_MD_CTX_free(ctx);

    digest_to_hex(digest, digest_len, out_hex);
    return 0;
}