/* 包含实际控制端，单独验证共享NC读拷贝，不打开装置或取得消费权。 */
#define main amp_test_client_main
#include "../acquisition/test_client.c"
#undef main
#include <assert.h>

int main(void)
{
    _Alignas(8) unsigned char source[288], dest[288];
    unsigned cases = 0;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i * 73U + 19U);
    for (unsigned start = 0; start < 8; start++) {
        for (unsigned target = 0; target < 8; target++) {
            for (unsigned bytes = 0; bytes <= 257; bytes++) {
                memset(dest, 0xc7, sizeof(dest));
                from(source, dest + 8 + target, start, bytes);
                assert(!memcmp(dest + 8 + target, source + start, bytes));
                for (unsigned j = 0; j < 8 + target; j++) assert(dest[j] == 0xc7);
                for (unsigned j = 8 + target + bytes; j < sizeof(dest); j++) assert(dest[j] == 0xc7);
                cases++;
            }
        }
    }
    printf("{\"client_shared_copy_cases\":%u,\"result\":\"passed\"}\n", cases);
    return 0;
}
