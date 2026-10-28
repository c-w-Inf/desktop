#ifndef HJSON_H
#define HJSON_H

#include <stddef.h>

struct json_object;

/**
 * @brief 解析 HJSON，同时兼容标准 JSON。
 * @param text 输入文本。
 * @param length 文本字节数。
 * @return 解析出的 json-c 对象，失败时为 NULL；调用方负责 json_object_put。
 */
struct json_object *HjsonParse (const char *text, size_t length);

#endif
