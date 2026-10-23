#ifndef HJSON_H
#define HJSON_H

#include <stddef.h>

struct json_object;

/* 解析 HJSON（同时兼容标准 JSON）。失败返回 NULL。 */
struct json_object *HjsonParse (const char *text, size_t length);

#endif
