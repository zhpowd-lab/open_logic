#include <assert.h>
#include <stdio.h>
#include "service_window.h"
int main(void)
{
    const rt_u64 base = 1ULL << 40;
    assert(amp_service_window(938, 170, base, base + 576, 192));
    assert(!amp_service_window(938, 170, base, base + 577, 192));
    assert(!amp_service_window(938, 170, base, base + 768, 192));
    assert(!amp_service_window(938, 170, base, base + 938, 192));
    assert(!amp_service_window(938, 170, base, base - 1, 192));
    assert(!amp_service_window(938, 938, base, base, 192));
    assert(!amp_service_window(0, 0, base, base, 192));
    assert(amp_service_window(1875, 200, base, base + 900, 192));
    puts("Background window: 8 boundary/overrun/clock scenarios passed");
    return 0;
}
