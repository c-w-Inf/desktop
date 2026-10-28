#include "textbuffer.h"

#include <stdlib.h>
#include <string.h>

bool TextBufferReserve (TextBuffer *buffer, size_t extra) {
    if (buffer->length + extra + 1 <= buffer->capacity) return true;

    size_t capacity = (buffer->capacity == 0) ? 128 : buffer->capacity;
    while (capacity < buffer->length + extra + 1) capacity *= 2;
    char *data = realloc (buffer->data, capacity);
    if (data == NULL) return false;
    buffer->data = data;
    buffer->capacity = capacity;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool TextBufferAppendN (TextBuffer *buffer, const char *text, size_t length) {
    if (!TextBufferReserve (buffer, length)) return false;
    if (length > 0) memcpy (buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool TextBufferAppend (TextBuffer *buffer, const char *text) {
    return TextBufferAppendN (buffer, text, strlen (text));
}

bool TextBufferAppendChar (TextBuffer *buffer, char ch) {
    return TextBufferAppendN (buffer, &ch, 1);
}

char *TextBufferTake (TextBuffer *buffer) {
    if (!TextBufferReserve (buffer, 0)) return NULL;
    char *data = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return data;
}

void TextBufferFree (TextBuffer *buffer) {
    free (buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}
