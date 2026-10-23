#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;

char *StrDup (const char *text);

/* 文件名扩展名判断；extension 包含点，大小写不敏感。 */
bool PathHasExtension (const char *path, const char *extension);
/* 是否为本程序支持的图片文件。 */
bool PathIsImage (const char *path);
/* 是否为目录（跟随符号链接）。 */
bool PathIsDirectory (const char *path);
/* 读取整个文件；返回 malloc 分配的字节，失败返回 NULL。 */
unsigned char *ReadFileBytes (const char *path, int *size);

u64 RandomNext (void);
u64 RandomBounded (u64 limit);

#ifndef strdup
#define strdup StrDup
#endif

typedef struct IdGroup {
    u64 *ids;
    int count;
    int capacity;
} IdGroup;

IdGroup IdGroupCreate (void);
void IdGroupUnload (IdGroup *group);
void IdGroupClear (IdGroup *group);

bool IdGroupAdd (IdGroup *group, u64 id);
bool IdGroupRemoveAt (IdGroup *group, int index);
bool IdGroupMove (IdGroup *group, int from, int to);
bool IdGroupCopy (IdGroup *group, const u64 *ids, int count);

typedef struct TextBuffer {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

bool TextBufferReserve (TextBuffer *buffer, size_t extra);
bool TextBufferAppendN (TextBuffer *buffer, const char *text, size_t length);
bool TextBufferAppend (TextBuffer *buffer, const char *text);
bool TextBufferAppendChar (TextBuffer *buffer, char ch);
char *TextBufferTake (TextBuffer *buffer);
void TextBufferFree (TextBuffer *buffer);

#endif
