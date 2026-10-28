#ifndef CONTROL_SIGNAL_H
#define CONTROL_SIGNAL_H

#include <stdbool.h>

/**
 * @brief 检查本地 GUI 控制管道是否已关闭。
 * @return 控制管道已关闭时为 true。
 */
bool ControlClientClosed (void);

#endif
