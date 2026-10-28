#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#include <uv.h>

/**
 * @brief GUI 侧控制管道服务端。
 */
typedef struct {
    uv_loop_t *loop;
    uv_pipe_t listener;
    uv_pipe_t peer;
    bool listenerOpen;
    bool peerOpen;
    char path[256];
} ControlServer;

/**
 * @brief 服务端侧控制管道客户端，在独立线程中运行。
 */
typedef struct {
    uv_thread_t thread;
    uv_mutex_t commandMutex;
    volatile bool commandMutexReady;
    volatile bool started;
    volatile bool stop;
    volatile bool closed;
    volatile bool failed;
    bool exportReady;
    char exportPath[1024];
    char path[256];
} ControlClient;

/**
 * @brief 生成指定端口的控制管道路径。
 * @param text 输出缓冲区。
 * @param size 缓冲区字节数。
 * @param port 服务端端口。
 * @return 路径成功写入且未截断时为 true。
 */
bool ControlPath (char *text, size_t size, int port);
/**
 * @brief 启动控制管道监听。
 * @param server 控制管道服务端。
 * @param loop 使用的 libuv 事件循环。
 * @param path 管道路径。
 * @return 启动成功时为 true。
 */
bool ControlServerStart (ControlServer *server, uv_loop_t *loop,
                         const char *path);
/**
 * @brief 停止控制管道并关闭连接。
 * @param server 控制管道服务端。
 */
void ControlServerStop (ControlServer *server);

/**
 * @brief 启动后台线程连接控制管道。
 * @param client 控制管道客户端。
 * @param path 管道路径。
 * @return 线程和互斥量初始化成功时为 true。
 */
bool ControlClientStart (ControlClient *client, const char *path);
/**
 * @brief 检查活动控制管道是否已关闭。
 * @return 已连接后远端关闭时为 true。
 */
bool ControlClientClosed (void);
/**
 * @brief 取走待处理的导出路径。
 * @param path 输出缓冲区。
 * @param size 缓冲区字节数。
 * @return 存在待处理导出且路径已写入时为 true。
 */
bool ControlClientTakeExport (char *path, size_t size);
/**
 * @brief 停止控制管道客户端线程并释放资源。
 * @param client 控制管道客户端。
 */
void ControlClientStop (ControlClient *client);

/**
 * @brief 向活动 GUI 发送导出命令。
 * @param path 服务端要写入的存档路径。
 * @return 命令已提交发送时为 true。
 */
bool ControlServerSendExport (const char *path);

#endif
