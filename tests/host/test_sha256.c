#include <northstar/sha256.h>

#include <stdio.h>
#include <string.h>

static int check_vector(const void *data, size_t length, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    struct ns_sha256 context;
    uint8_t digest[NS_SHA256_DIGEST_BYTES];
    char actual[NS_SHA256_DIGEST_BYTES * 2u + 1u];

    ns_sha256_init(&context);
    ns_sha256_update(&context, data, length / 2u);
    ns_sha256_update(&context, (const uint8_t *)data + length / 2u,
                     length - length / 2u);
    ns_sha256_final(&context, digest);
    for (size_t index = 0; index < sizeof(digest); ++index) {
        actual[index * 2u] = hex[digest[index] >> 4];
        actual[index * 2u + 1u] = hex[digest[index] & 15u];
    }
    actual[sizeof(digest) * 2u] = '\0';
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "SHA-256 mismatch: %s != %s\n", actual, expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const char message[] = "abc";
    if (check_vector("", 0,
                     "e3b0c44298fc1c149afbf4c8996fb924"
                     "27ae41e4649b934ca495991b7852b855") != 0 ||
        check_vector(message, sizeof(message) - 1u,
                     "ba7816bf8f01cfea414140de5dae2223"
                     "b00361a396177a9cb410ff61f20015ad") != 0)
        return 1;
    puts("ok - SHA-256 known vectors and incremental updates");
    return 0;
}
