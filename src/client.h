#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>


/**
 * @brief 初始化逐帧客户端状态和菜单。
 * @param localMode 是否为本地服务端模式。
 * @param localPort 本地服务端端口。
 * @param localAddress 本地服务端监听地址。
 */
void ClientInit (bool localMode, int localPort,
                 const char *localAddress);

/**
 * @brief 运行一帧输入、网络和绘制逻辑。
 * @return 继续运行时为 true，应退出时为 false。
 */
bool ClientFrame (void);

#endif
