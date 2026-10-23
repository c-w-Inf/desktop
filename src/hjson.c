#include "hjson.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "utils.h"

#define HJSON_MAX_DEPTH 256

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
    bool failed;
} Hjson;

static char HjsonPeek (const Hjson *h, size_t offset) {
    size_t pos = h->pos + offset;
    return pos < h->length ? h->text[pos] : '\0';
}

static bool HjsonSpace (char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static bool HjsonSkip (Hjson *h) {
    while (h->pos < h->length) {
        char ch = h->text[h->pos];
        if (HjsonSpace (ch)) {
            h->pos++;
            continue;
        }
        if (ch == '#' ||
            (ch == '/' && HjsonPeek (h, 1) == '/')) {
            while (h->pos < h->length &&
                   h->text[h->pos] != '\n' && h->text[h->pos] != '\r')
                h->pos++;
            continue;
        }
        if (ch == '/' && HjsonPeek (h, 1) == '*') {
            h->pos += 2;
            while (h->pos + 1 < h->length &&
                   !(h->text[h->pos] == '*' && h->text[h->pos + 1] == '/'))
                h->pos++;
            if (h->pos + 1 >= h->length) {
                h->failed = true;
                return false;
            }
            h->pos += 2;
            continue;
        }
        break;
    }
    return true;
}

static struct json_object *HjsonQuoted (Hjson *h) {
    size_t start = h->pos;
    h->pos++;

    while (h->pos < h->length) {
        char ch = h->text[h->pos];
        if (ch == '\\') {
            h->pos++;
            if (h->pos >= h->length) {
                h->failed = true;
                return NULL;
            }
            h->pos++;
        } else if (ch == '"') {
            h->pos++;
            size_t length = h->pos - start;
            if (length > INT_MAX) {
                h->failed = true;
                return NULL;
            }
            struct json_tokener *tokener = json_tokener_new_ex (HJSON_MAX_DEPTH);
            if (tokener == NULL) {
                h->failed = true;
                return NULL;
            }
            struct json_object *value = json_tokener_parse_ex (
                tokener, h->text + start, (int)length);
            bool ok = value != NULL &&
                      json_tokener_get_error (tokener) == json_tokener_success &&
                      json_object_is_type (value, json_type_string) &&
                      json_tokener_get_parse_end (tokener) == length;
            json_tokener_free (tokener);
            if (!ok) json_object_put (value);
            return ok ? value : NULL;
        } else if (ch == '\n' || ch == '\r') {
            h->failed = true;
            return NULL;
        } else {
            h->pos++;
        }
    }
    h->failed = true;
    return NULL;
}

static struct json_object *HjsonMultiline (Hjson *h) {
    h->pos += 3;
    size_t start = h->pos;
    size_t close = SIZE_MAX;
    for (size_t i = start; i + 2 < h->length; i++) {
        if (h->text[i] == '\'' && h->text[i + 1] == '\'' &&
            h->text[i + 2] == '\'') {
            close = i;
            break;
        }
    }
    if (close == SIZE_MAX) {
        h->failed = true;
        return NULL;
    }
    h->pos = close + 3;

    size_t end = close;
    size_t lineStart = close;
    while (lineStart > start && h->text[lineStart - 1] != '\n' &&
           h->text[lineStart - 1] != '\r')
        lineStart--;
    size_t indentLength = close - lineStart;
    bool indentOnly = true;
    for (size_t i = 0; i < indentLength; i++) {
        if (h->text[lineStart + i] != ' ' && h->text[lineStart + i] != '\t') {
            indentOnly = false;
            break;
        }
    }
    if (indentOnly && lineStart > start) {
        end = lineStart;
        if (end > start && h->text[end - 1] == '\n') end--;
        if (end > start && h->text[end - 1] == '\r') end--;
    } else {
        indentLength = 0;
    }

    size_t begin = start;
    if (begin < end && h->text[begin] == '\r') begin++;
    if (begin < end && h->text[begin] == '\n') begin++;

    TextBuffer result = {0};
    size_t line = begin;
    bool ok = true;
    while (ok && line < end) {
        size_t lineEnd = line;
        while (lineEnd < end && h->text[lineEnd] != '\n' &&
               h->text[lineEnd] != '\r')
            lineEnd++;

        size_t copy = line;
        if (indentLength > 0 && lineEnd - line >= indentLength &&
            memcmp (h->text + line, h->text + lineStart, indentLength) == 0)
            copy += indentLength;
        ok = TextBufferAppendN (&result, h->text + copy, lineEnd - copy);

        if (lineEnd < end) {
            if (h->text[lineEnd] == '\r') lineEnd++;
            if (lineEnd < end && h->text[lineEnd] == '\n') lineEnd++;
            if (ok) ok = TextBufferAppendChar (&result, '\n');
        }
        line = lineEnd;
    }

    if (!ok) {
        TextBufferFree (&result);
        h->failed = true;
        return NULL;
    }
    struct json_object *value = json_object_new_string_len (
        result.data != NULL ? result.data : "", (int)result.length);
    TextBufferFree (&result);
    if (value == NULL) h->failed = true;
    return value;
}

static bool HjsonCommentStart (const Hjson *h, size_t start) {
    char ch = h->text[h->pos];
    bool boundary = h->pos == start || HjsonSpace (h->text[h->pos - 1]);
    if (!boundary) return false;
    if (ch == '#') return true;
    if (ch == '/' && h->pos + 1 < h->length) {
        char next = h->text[h->pos + 1];
        return next == '/' || next == '*';
    }
    return false;
}

static struct json_object *HjsonScalar (Hjson *h) {
    size_t start = h->pos;
    while (h->pos < h->length) {
        char ch = h->text[h->pos];
        if (ch == '\n' || ch == '\r' || ch == ',' ||
            ch == ']' || ch == '}')
            break;
        if (HjsonCommentStart (h, start)) break;
        h->pos++;
    }

    size_t end = h->pos;
    while (end > start && HjsonSpace (h->text[end - 1])) end--;
    size_t length = end - start;
    if (length == 4 && memcmp (h->text + start, "true", 4) == 0) {
        struct json_object *value = json_object_new_boolean (1);
        if (value == NULL) h->failed = true;
        return value;
    }
    if (length == 5 && memcmp (h->text + start, "false", 5) == 0) {
        struct json_object *value = json_object_new_boolean (0);
        if (value == NULL) h->failed = true;
        return value;
    }
    if (length == 4 && memcmp (h->text + start, "null", 4) == 0)
        return NULL;

    char *token = malloc (length + 1);
    if (token == NULL) {
        h->failed = true;
        return NULL;
    }
    memcpy (token, h->text + start, length);
    token[length] = '\0';

    bool number = length > 0;
    bool integer = true;
    for (size_t i = 0; i < length; i++) {
        char ch = token[i];
        if (ch == '.' || ch == 'e' || ch == 'E') integer = false;
        if (i == 0 && (ch == '-' || ch == '+')) continue;
        if (!isdigit ((unsigned char)ch) && ch != '.' && ch != 'e' &&
            ch != 'E' && ch != '+' && ch != '-') {
            number = false;
            break;
        }
    }
    if (number) {
        char *endPtr = NULL;
        if (integer) {
            errno = 0;
            long long value = strtoll (token, &endPtr, 10);
            bool ok = endPtr == token + length && errno != ERANGE;
            if (!ok && token[0] != '-') {
                errno = 0;
                unsigned long long unsignedValue = strtoull (token, &endPtr, 10);
                ok = endPtr == token + length && errno != ERANGE;
                if (ok) {
                    free (token);
                    struct json_object *value =
                        json_object_new_uint64 ((uint64_t)unsignedValue);
                    if (value == NULL) h->failed = true;
                    return value;
                }
            }
            if (ok) {
                free (token);
                struct json_object *result =
                    value >= 0 ? json_object_new_uint64 ((uint64_t)value)
                               : json_object_new_int64 (value);
                if (result == NULL) h->failed = true;
                return result;
            }
        } else {
            errno = 0;
            double value = strtod (token, &endPtr);
            bool ok = endPtr == token + length && errno != ERANGE &&
                      isfinite (value);
            if (ok) {
                free (token);
                struct json_object *result = json_object_new_double (value);
                if (result == NULL) h->failed = true;
                return result;
            }
        }
    }

    struct json_object *value = json_object_new_string_len (token, (int)length);
    free (token);
    if (value == NULL) h->failed = true;
    return value;
}

static char *HjsonKey (Hjson *h) {
    if (!HjsonSkip (h) || h->failed || h->pos >= h->length) {
        h->failed = true;
        return NULL;
    }

    if (HjsonPeek (h, 0) == '"') {
        struct json_object *keyValue = HjsonQuoted (h);
        if (h->failed || keyValue == NULL) {
            h->failed = true;
            return NULL;
        }
        const char *text = json_object_get_string (keyValue);
        char *key = text != NULL ? StrDup (text) : NULL;
        json_object_put (keyValue);
        if (key == NULL) h->failed = true;
        return key;
    }

    size_t start = h->pos;
    while (h->pos < h->length && h->text[h->pos] != ':' &&
           h->text[h->pos] != '\n' && h->text[h->pos] != '\r')
        h->pos++;
    if (h->pos >= h->length || h->text[h->pos] != ':') {
        h->failed = true;
        return NULL;
    }
    size_t end = h->pos;
    while (end > start && HjsonSpace (h->text[end - 1])) end--;
    if (end == start) {
        h->failed = true;
        return NULL;
    }
    size_t length = end - start;
    char *key = malloc (length + 1);
    if (key == NULL) {
        h->failed = true;
        return NULL;
    }
    memcpy (key, h->text + start, length);
    key[length] = '\0';
    return key;
}

static struct json_object *HjsonValue (Hjson *h, int depth);

static struct json_object *HjsonObject (Hjson *h, bool braced, int depth) {
    if (depth > HJSON_MAX_DEPTH) {
        h->failed = true;
        return NULL;
    }

    struct json_object *object = json_object_new_object ();
    if (object == NULL) {
        h->failed = true;
        return NULL;
    }

    if (braced) {
        if (!HjsonSkip (h) || h->failed || HjsonPeek (h, 0) != '{') {
            h->failed = true;
            json_object_put (object);
            return NULL;
        }
        h->pos++;
    }

    while (true) {
        if (!HjsonSkip (h) || h->failed) {
            h->failed = true;
            json_object_put (object);
            return NULL;
        }
        if (braced && HjsonPeek (h, 0) == '}') {
            h->pos++;
            return object;
        }
        if (h->pos >= h->length) {
            if (!braced) return object;
            h->failed = true;
            json_object_put (object);
            return NULL;
        }

        char *key = HjsonKey (h);
        if (h->failed || key == NULL) {
            h->failed = true;
            json_object_put (object);
            return NULL;
        }

        if (!HjsonSkip (h) || h->failed || HjsonPeek (h, 0) != ':') {
            h->failed = true;
            free (key);
            json_object_put (object);
            return NULL;
        }
        h->pos++;

        struct json_object *value = HjsonValue (h, depth + 1);
        if (h->failed) {
            free (key);
            json_object_put (object);
            return NULL;
        }
        if (json_object_object_add (object, key, value) != 0) {
            h->failed = true;
            free (key);
            json_object_put (value);
            json_object_put (object);
            return NULL;
        }
        free (key);

        if (!HjsonSkip (h) || h->failed) {
            h->failed = true;
            json_object_put (object);
            return NULL;
        }
        if (HjsonPeek (h, 0) == ',') {
            h->pos++;
            continue;
        }
        if (braced && HjsonPeek (h, 0) == '}') {
            h->pos++;
            return object;
        }
        if (!braced && h->pos >= h->length) return object;
        if (h->pos >= h->length) {
            h->failed = true;
            json_object_put (object);
            return NULL;
        }
    }
}

static struct json_object *HjsonArray (Hjson *h, int depth) {
    if (depth > HJSON_MAX_DEPTH) {
        h->failed = true;
        return NULL;
    }

    struct json_object *array = json_object_new_array ();
    if (array == NULL) {
        h->failed = true;
        return NULL;
    }
    if (!HjsonSkip (h) || h->failed || HjsonPeek (h, 0) != '[') {
        h->failed = true;
        json_object_put (array);
        return NULL;
    }
    h->pos++;

    while (true) {
        if (!HjsonSkip (h) || h->failed) {
            h->failed = true;
            json_object_put (array);
            return NULL;
        }
        if (HjsonPeek (h, 0) == ']') {
            h->pos++;
            return array;
        }
        if (h->pos >= h->length) {
            h->failed = true;
            json_object_put (array);
            return NULL;
        }

        struct json_object *value = HjsonValue (h, depth + 1);
        if (h->failed) {
            json_object_put (array);
            return NULL;
        }
        if (json_object_array_add (array, value) != 0) {
            h->failed = true;
            json_object_put (value);
            json_object_put (array);
            return NULL;
        }

        if (!HjsonSkip (h) || h->failed) {
            h->failed = true;
            json_object_put (array);
            return NULL;
        }
        if (HjsonPeek (h, 0) == ',') {
            h->pos++;
            continue;
        }
        if (HjsonPeek (h, 0) == ']') {
            h->pos++;
            return array;
        }
        if (h->pos >= h->length) {
            h->failed = true;
            json_object_put (array);
            return NULL;
        }
    }
}

static struct json_object *HjsonValue (Hjson *h, int depth) {
    if (!HjsonSkip (h) || h->failed || h->pos >= h->length) {
        h->failed = true;
        return NULL;
    }
    char ch = HjsonPeek (h, 0);
    if (ch == ',' || ch == ']' || ch == '}') {
        h->failed = true;
        return NULL;
    }
    if (ch == '{') return HjsonObject (h, true, depth);
    if (ch == '[') return HjsonArray (h, depth);
    if (ch == '"') return HjsonQuoted (h);
    if (ch == '\'' && HjsonPeek (h, 1) == '\'' && HjsonPeek (h, 2) == '\'')
        return HjsonMultiline (h);
    return HjsonScalar (h);
}

struct json_object *HjsonParse (const char *text, size_t length) {
    if (text == NULL || length > INT_MAX) return NULL;

    Hjson h = {text, length, 0, false};
    if (length >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        h.pos = 3;

    if (!HjsonSkip (&h) || h.failed) return NULL;

    struct json_object *value = NULL;
    if (HjsonPeek (&h, 0) == '{')
        value = HjsonObject (&h, true, 0);
    else if (HjsonPeek (&h, 0) == '[')
        value = HjsonArray (&h, 0);
    else
        value = HjsonObject (&h, false, 0);

    if (h.failed || value == NULL || !HjsonSkip (&h) || h.failed ||
        h.pos != h.length) {
        json_object_put (value);
        return NULL;
    }
    return value;
}
