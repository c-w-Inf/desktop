#ifndef UTILS_TEXT_BUFFER_H
#define UTILS_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 以 NUL 结尾的可增长文本缓冲区。
 */
typedef struct TextBuffer {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

/**
 * @brief 确保缓冲区可额外容纳 extra 字节及结尾 NUL。
 * @param buffer 文本缓冲区。
 * @param extra 额外字节数。
 * @return 容量足够时为 true。
 */
bool TextBufferReserve (TextBuffer *buffer, size_t extra);
/**
 * @brief 追加指定长度的文本。
 * @param buffer 文本缓冲区。
 * @param text 源文本。
 * @param length 字节数。
 * @return 追加成功时为 true。
 */
bool TextBufferAppendN (TextBuffer *buffer, const char *text, size_t length);
/**
 * @brief 追加 NUL 结尾字符串。
 * @param buffer 文本缓冲区。
 * @param text 源文本。
 * @return 追加成功时为 true。
 */
bool TextBufferAppend (TextBuffer *buffer, const char *text);
/**
 * @brief 追加一个字符。
 * @param buffer 文本缓冲区。
 * @param ch 字符。
 * @return 追加成功时为 true。
 */
bool TextBufferAppendChar (TextBuffer *buffer, char ch);
/**
 * @brief 取走缓冲区内容并重置缓冲区。
 * @param buffer 文本缓冲区。
 * @return NUL 结尾文本，调用方负责 free；失败时为 NULL。
 */
char *TextBufferTake (TextBuffer *buffer);
/**
 * @brief 释放缓冲区存储并清零。
 * @param buffer 文本缓冲区。
 */
void TextBufferFree (TextBuffer *buffer);

#endif
