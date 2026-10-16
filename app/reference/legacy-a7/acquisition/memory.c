#include <stddef.h>
#include <stdint.h>
typedef uint32_t alias_u32 __attribute__((__may_alias__));
/* 裸机无libc；公共采集代码只需要这些确定长度的本地复制，不分配内存。 */
void *memcpy(void *out, const void *in, size_t bytes)
{
    unsigned char *dst = out;
    const unsigned char *src = in;
    /* 私有帧和栈常按4字节对齐；整字复制避免每帧数百次字节循环。
     * 不对未对齐指针作宽访问，也不越过指定长度。 */
    if (!(((uintptr_t)dst | (uintptr_t)src) & 3U)) {
        while (bytes >= 4U) {
            *(alias_u32 *)dst = *(const alias_u32 *)src;
            dst += 4; src += 4; bytes -= 4;
        }
    }
    while (bytes--)
        *dst++ = *src++;
    return out;
}
void *memset(void *out, int value, size_t bytes)
{
    unsigned char *dst = out;
    if (!((uintptr_t)dst & 3U)) {
        uint32_t word = (unsigned char)value * 0x01010101U;
        while (bytes >= 4U) {
            *(alias_u32 *)dst = word;
            dst += 4; bytes -= 4;
        }
    }
    while (bytes--)
        *dst++ = (unsigned char)value;
    return out;
}
void __aeabi_memclr4(void *out, size_t bytes) { memset(out, 0, bytes); }
void __aeabi_memclr8(void *out, size_t bytes) { memset(out, 0, bytes); }
void __aeabi_memcpy(void *out, const void *in, size_t bytes) { memcpy(out, in, bytes); }
void __aeabi_memcpy4(void *out, const void *in, size_t bytes) { memcpy(out, in, bytes); }
void __aeabi_memcpy8(void *out, const void *in, size_t bytes) { memcpy(out, in, bytes); }
