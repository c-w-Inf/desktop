#ifndef CUTILS_H
#define CUTILS_H

#include <stdbool.h>
#include <stddef.h>
#include "raylib.h"

/**
 * @brief 界面默认字体。
 */
extern Font CutilsFont;
/**
 * @brief 等宽字体。
 */
extern Font CutilsMonoFont;

/**
 * @brief 加载界面字体。
 */
void CutilsLoadFonts (void);
/**
 * @brief 释放界面字体。
 */
void CutilsUnloadFonts (void);
/**
 * @brief 初始化鼠标输入队列。
 */
void CutilsInitInput (void);
/**
 * @brief 查询鼠标按键是否按下。
 * @param button 按键编号。
 * @return 本帧按下时为 true。
 */
bool CutilsMousePressed (int button);
/**
 * @brief 查询鼠标按键是否完成点击。
 * @param button 按键编号。
 * @return 本帧点击时为 true。
 */
bool CutilsMouseClicked (int button);
/**
 * @brief 查询鼠标按键是否释放。
 * @param button 按键编号。
 * @return 本帧释放时为 true。
 */
bool CutilsMouseReleased (int button);
/**
 * @brief 开始新帧鼠标输入。
 */
void CutilsMouseReset (void);
/**
 * @brief 开始新帧键盘输入。
 */
void CutilsKeysReset (void);/**
 * @brief 开始裁剪区域。
 * @param x 逻辑横坐标。
 * @param y 逻辑纵坐标。
 * @param width 逻辑宽度。
 * @param height 逻辑高度。
 */
void CutilsBeginScissor (int x, int y, int width, int height);
/**
 * @brief 结束裁剪区域。
 */
void CutilsEndScissor (void);

/**
 * @brief 读取一个字符。
 * @return Unicode 码点，没有时为 0。
 */
int CutilsCharPressed (void);
/**
 * @brief 查询按键是否在本帧按下。
 * @param key 键码。
 * @return 按下时为 true。
 */
bool CutilsKeyPressed (int key);
/**
 * @brief 查询按键是否处于按下状态。
 * @param key 键码。
 * @return 按下时为 true。
 */
bool CutilsKeyDown (int key);/**
 * @brief 获取本帧滚轮增量。
 * @return 滚轮增量。
 */
float CutilsWheelMove (void);


/**
 * @brief 文件对话框返回状态。
 */
typedef enum {
    CUTILS_DIALOG_CANCELLED,
    CUTILS_DIALOG_OK,
    CUTILS_DIALOG_TOO_LONG,
    CUTILS_DIALOG_FAILED,
    CUTILS_DIALOG_PENDING
} CutilsDialogResult;

/**
 * @brief Web 文件选择完成后的回调。
 * @param path MEMFS 中的临时文件路径。
 */
typedef void (*CutilsFileChosenFn)(const char *path);

/**
 * @brief 注册 Web 文件选择完成回调。
 * @param handler 回调函数，可为 NULL。
 */
void CutilsSetFileHandler(CutilsFileChosenFn handler);

/**
 * @brief 驱动平台事件循环。
 */
void CutilsPump(void);
/**
 * @brief 打开新的远程客户端窗口。
 * @param host 服务器主机名或 IP。
 * @param port 服务器端口。
 * @param username 用户名。
 * @return 成功启动时为 true。
 */
bool CutilsOpenRemote(const char *host, int port, const char *username);

/**
 * @brief 打开图片选择对话框。
 * @param path 输出选中路径。
 * @param size 输出缓冲区字节数。
 * @return 对话框状态；Web 端立即返回 CUTILS_DIALOG_PENDING。
 */
CutilsDialogResult CutilsOpenImage (char *path, size_t size);
/**
 * @brief 打开存档或图片导入对话框。
 * @param path 输出选中路径。
 * @param size 输出缓冲区字节数。
 * @return 对话框状态；Web 端立即返回 CUTILS_DIALOG_PENDING。
 */
CutilsDialogResult CutilsOpenImport (char *path, size_t size);
/**
 * @brief 打开文件夹选择对话框。
 * @param path 输出选中路径。
 * @param size 输出缓冲区字节数。
 * @return 对话框状态；Web 端返回 CUTILS_DIALOG_FAILED。
 */
CutilsDialogResult CutilsOpenFolder (char *path, size_t size);
/**
 * @brief 打开 .dsk 保存对话框并补齐扩展名。
 * @param path 输出文件路径。
 * @param size 输出缓冲区字节数。
 * @return 对话框状态；Web 端返回 CUTILS_DIALOG_FAILED。
 */
CutilsDialogResult CutilsSaveDsk (char *path, size_t size);

/**
 * @brief 判断路径是否为目录。
 * @param path 待检查路径。
 * @return 是目录时为 true。
 */
bool PathIsDirectory (const char *path);

#endif
