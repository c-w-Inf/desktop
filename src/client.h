#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include "ui.h"
#include "view.h"

typedef void (*ClientPumpFn)(void);
typedef bool (*ClientOpenRemoteFn)(const char *program, const char *host,
                                   int port, const char *username);

/* 网络已完成首帧全量同步后运行客户端 UI。 */
int ClientRun(Ui *ui, View *view, bool localMode, int localPort,
              const char *localAddress, ClientPumpFn pump,
              ClientOpenRemoteFn openRemote, const char *program);

#endif
