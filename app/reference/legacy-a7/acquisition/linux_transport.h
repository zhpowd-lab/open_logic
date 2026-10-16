#ifndef AMP_LINUX_TRANSPORT_H
#define AMP_LINUX_TRANSPORT_H
#include "protocol.h"
/* 唯一消费者、私有mmap批读和波形转发复用既有Linux模块，不另建业务缓冲。 */
int amp_transport_start(int (*snapshot)(struct amp_acq_status *), int (*active)(void));
void amp_transport_stop(void);
/* 控制映射使用者停止写命令后调用；一次STOP及对应ACK是释放硬件资源的依据。 */
int amp_transport_stop_remote(void);
#endif
