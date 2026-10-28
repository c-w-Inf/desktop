#ifndef UTILS_STR_H
#define UTILS_STR_H

/**
 * @brief 复制字符串。
 * @param text 源字符串。
 * @return 新字符串，调用方负责 free；失败时为 NULL。
 */
char *StrDup (const char *text);

#endif
