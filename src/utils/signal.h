#ifndef UTILS_SIGNAL_H
#define UTILS_SIGNAL_H

#include <stdbool.h>

/**
 * @brief 安装 Ctrl+C 和终止信号处理器。
 */
void UtilsInstallStopHandlers (void);
/**
 * @brief 检查是否收到停止信号。
 * @return 收到停止信号时为 true。
 */
bool UtilsStopRequested (void);

#endif
