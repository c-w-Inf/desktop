#ifndef NETUTILS_H
#define NETUTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 不透明的网络流。
 */
typedef struct NetutilStream NetutilStream;
/**
 * @brief 不透明的网络监听器。
 */
typedef struct NetutilServer NetutilServer;

#if defined(__EMSCRIPTEN__)
/**
 * @brief Web 端 WebSocket 目标地址。
 */
typedef struct {
    const char *url; /**< WebSocket URL。 */
} NetutilAddress;
#else
/**
 * @brief 原生 TCP 目标地址。
 */
typedef struct {
    const char *host; /**< 主机名或 IP。 */
    uint16_t port; /**< 端口。 */
} NetutilAddress;
#endif

/**
 * @brief 接受新连接后的回调。
 * @param server 监听器。
 * @param stream 新连接。
 * @param user 注册监听器时传入的用户数据。
 */
typedef void (*NetutilAcceptFn)(NetutilServer *server, NetutilStream *stream,
                                void *user);

/**
 * @brief 网络流事件回调集合。
 */
typedef struct {
    /**
     * @brief 连接成功回调。
     * @param stream 网络流。
     * @param user 用户数据。
     */
    void (*on_connected)(NetutilStream *stream, void *user);
    /**
     * @brief 收到数据回调。
     * @param stream 网络流。
     * @param data 数据指针。
     * @param size 字节数。
     * @param user 用户数据。
     */
    void (*on_read)(NetutilStream *stream, const unsigned char *data,
                    size_t size, void *user);
    /**
     * @brief 连接关闭回调。
     * @param stream 网络流。
     * @param user 用户数据。
     */
    void (*on_closed)(NetutilStream *stream, void *user);
    /**
     * @brief 网络错误回调。
     * @param stream 网络流。
     * @param code 错误码。
     * @param user 用户数据。
     */
    void (*on_error)(NetutilStream *stream, int code, void *user);
} NetutilEvents;

/**
 * @brief 初始化网络后端和事件循环。
 * @return 初始化成功时为 true。
 */
bool NetutilInit(void);
/**
 * @brief 关闭网络后端并释放全局资源。
 */
void NetutilShutdown(void);

/**
 * @brief 创建 TCP 监听器。
 * @param bind 绑定地址。
 * @param port 监听端口。
 * @param accept 新连接回调。
 * @param user 传给回调的用户数据。
 * @param server 输出监听器。
 * @return 创建成功时为 true；Web 后端返回 false。
 */
bool NetutilListen(const char *bind, uint16_t port, NetutilAcceptFn accept,
                   void *user, NetutilServer **server);
/**
 * @brief 关闭监听器。
 * @param server 监听器。
 */
void NetutilServerClose(NetutilServer *server);

/**
 * @brief 异步连接远端。
 * @param address 目标地址。
 * @param events 事件回调。
 * @param user 传给回调的用户数据。
 * @param stream 输出网络流。
 * @return 连接请求成功启动时为 true。
 */
bool NetutilConnect(const NetutilAddress *address, const NetutilEvents *events,
                    void *user, NetutilStream **stream);
/**
 * @brief 替换网络流的事件回调和用户数据。
 * @param stream 网络流。
 * @param events 新事件回调。
 * @param user 新用户数据。
 */
void NetutilSetEvents(NetutilStream *stream, const NetutilEvents *events,
                      void *user);

/**
 * @brief 异步写入数据，数据在返回前会被复制。
 * @param stream 网络流。
 * @param data 数据指针。
 * @param size 字节数。
 * @return 写入请求成功排队时为 true。
 */
bool NetutilWrite(NetutilStream *stream, const void *data, size_t size);
/**
 * @brief 关闭网络流。
 * @param stream 网络流。
 */
void NetutilClose(NetutilStream *stream);

/**
 * @brief 驱动网络事件。
 * @param timeout_ms 最长等待毫秒数；Web 后端会立即返回。
 */
void NetutilPump(int timeout_ms);

#endif
