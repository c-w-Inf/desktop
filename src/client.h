#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include "raylib.h"
#include "object.h"
#include "ui.h"
#include "utils.h"
#include "view.h"

typedef void (*ClientPumpFn)(void);
typedef bool (*ClientOpenRemoteFn)(const char *program, const char *host,
                                   int port, const char *username);

typedef struct ClientFrameState {
    Ui *ui;
    View *view;
    int localPort;
    const char *localAddress;
    ClientPumpFn pump;
    ClientOpenRemoteFn openRemote;
    const char *program;
    Object *chosen;
    bool dragged;
    bool dragMoved;
    Vector2 lastMouse;
    Vector2 dragPreviewPos;
    double lastCursorSend;
    bool closeAfterConnect;
    u64 sceneVersion;
} ClientFrameState;

/* 初始化逐帧客户端状态。 */
void ClientFrameInit(ClientFrameState *state, Ui *ui, View *view,
                     bool localMode, int localPort, const char *localAddress,
                     ClientPumpFn pump, ClientOpenRemoteFn openRemote,
                     const char *program);

/* 运行一帧；返回 false 表示客户端循环应退出。 */
bool ClientFrame(ClientFrameState *state);

/* 清理逐帧状态持有的临时数据。 */
void ClientFrameUnload(ClientFrameState *state);

/* 原生阻塞入口，内部反复调用 ClientFrame。 */
int ClientRun(Ui *ui, View *view, bool localMode, int localPort,
              const char *localAddress, ClientPumpFn pump,
              ClientOpenRemoteFn openRemote, const char *program);

#endif
