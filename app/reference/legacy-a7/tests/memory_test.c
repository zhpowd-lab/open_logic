#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

void *amp_memcpy(void *out, const void *in, size_t bytes);
void *amp_memset(void *out, int value, size_t bytes);

int main(void)
{
    _Alignas(4) unsigned char source[272], target[272];
    unsigned from, to, length, i, fill, cases = 0;
    for (i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i * 37U + 11U);
    for (from = 0; from < 4; from++) for (to = 0; to < 4; to++) {
        for (length = 0; length <= 256; length++) {
            for (i = 0; i < sizeof(target); i++) target[i] = 0xa5;
            assert(amp_memcpy(target + 4 + to, source + from, length) == target + 4 + to);
            for (i = 0; i < sizeof(target); i++) {
                unsigned char expected = i >= 4 + to && i < 4 + to + length ?
                    source[i - 4 - to + from] : 0xa5;
                assert(target[i] == expected);
            }
            cases++;
        }
    }
    for (to = 0; to < 4; to++) for (fill = 0; fill < 4; fill++) {
        const int values[] = {0, 0x5a, -1, 0x1ab};
        for (length = 0; length <= 256; length++) {
            for (i = 0; i < sizeof(target); i++) target[i] = 0xa5;
            assert(amp_memset(target + 4 + to, values[fill], length) == target + 4 + to);
            for (i = 0; i < sizeof(target); i++) {
                unsigned char expected = i >= 4 + to && i < 4 + to + length ?
                    (unsigned char)values[fill] : 0xa5;
                assert(target[i] == expected);
            }
            cases++;
        }
    }
    printf("Private memory alignment/tail/canary comparisons passed: %u\n", cases);
    return 0;
}
