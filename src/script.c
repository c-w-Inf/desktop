#include "script.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "entity.h"
#include "menu.h"
#include "object.h"
#include "strmap.h"
#include "user.h"
#include "net.h"
#include "utils.h"

#define SCRIPT_MAX_DEPTH 256
#define SCRIPT_TAB_WIDTH 4
#define SCRIPT_MAX_MESSAGE 256u
#define SCRIPT_MAX_OPERATIONS 1000000u

static void SetErrorV (TextBuffer *error, int line, const char *format, va_list args) {
    if (error->length > 0) return;

    va_list copy;
    va_copy (copy, args);
    int needed = vsnprintf (NULL, 0, format, copy);
    va_end (copy);
    if (needed < 0) return;

    if (line > 0) {
        char prefix[32];
        int length = snprintf (prefix, sizeof (prefix), "第 %d 行: ", line);
        if (length > 0) TextBufferAppendN (error, prefix, (size_t)length);
    }

    char *message = malloc ((size_t)needed + 1);
    if (message == NULL) {
        TextBufferAppend (error, "内存不足");
        return;
    }
    vsnprintf (message, (size_t)needed + 1, format, args);
    TextBufferAppend (error, message);
    free (message);
}

static void SetError (TextBuffer *error, int line, const char *format, ...) {
    va_list args;
    va_start (args, format);
    SetErrorV (error, line, format, args);
    va_end (args);
}

typedef enum {
    TOK_EOF,
    TOK_ERROR,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_FUNC,
    TOK_VAR,
    TOK_UNVAR,
    TOK_IF,
    TOK_ELIF,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_ELSE,
    TOK_RETURN,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_SEMI,
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,
    TOK_PERCENT_ASSIGN,
    TOK_EQ,
    TOK_NE,
    TOK_AND,
    TOK_OR,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_BANG,
    TOK_HASH,
    TOK_AT,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT
} TokenType;

typedef struct {
    TokenType type;
    int line;
    int column;
    char* text;
    double number;
    u64 integer;
    bool isInteger;
} Token;

typedef struct {
    const char* source;
    size_t length;
    size_t pos;
    int line;
    int column;
    Token token;
    TextBuffer error;
    bool failed;
    bool lineStart;
    int bracketDepth;
    int indentCount;
    int indentStack[64];
    TokenType pending[64];
    int pendingLine[64];
    int pendingCount;
    int pendingIndex;
} Lexer;

static char LexerPeek (const Lexer* lexer, size_t offset) {
    size_t pos = lexer->pos + offset;
    return (pos < lexer->length) ? lexer->source[pos] : '\0';
}

static void LexerAdvance (Lexer* lexer) {
    if (lexer->pos >= lexer->length) return;
    char ch = lexer->source[lexer->pos++];
    if (ch == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else if (ch != '\r') {
        lexer->column++;
    }
}

static void LexerFail (Lexer* lexer, int line, const char* format, ...) {
    va_list args;
    va_start (args, format);
    SetErrorV (&lexer->error, line, format, args);
    va_end (args);
}

static bool AppendUtf8 (TextBuffer* buffer, uint32_t codepoint) {
    char bytes[4];
    size_t count;
    if (codepoint == 0) return false;
    if (codepoint <= 0x7F) {
        bytes[0] = (char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7FF) {
        bytes[0] = (char)(0xC0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3F));
        count = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = (char)(0xE0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (codepoint & 0x3F));
        count = 3;
    } else if (codepoint <= 0x10FFFF) {
        bytes[0] = (char)(0xF0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (codepoint & 0x3F));
        count = 4;
    } else {
        return false;
    }
    return TextBufferAppendN (buffer, bytes, count);
}

static int HexValue (char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool LexerReadHex4 (Lexer* lexer, uint32_t* value) {
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        int digit = HexValue (LexerPeek (lexer, i));
        if (digit < 0) return false;
        result = (result << 4) | (uint32_t)digit;
    }
    for (int i = 0; i < 4; i++) LexerAdvance (lexer);
    *value = result;
    return true;
}

static char* LexerReadString (Lexer* lexer, int line) {
    TextBuffer buffer = {0};
    LexerAdvance (lexer);

    while (lexer->pos < lexer->length) {
        char ch = LexerPeek (lexer, 0);
        if (ch == '"') {
            LexerAdvance (lexer);
            if (!TextBufferReserve (&buffer, 0)) {
                TextBufferFree (&buffer);
                return NULL;
            }
            return buffer.data;
        }
        if (ch == '\n' || ch == '\r') {
            LexerFail (lexer, line, "字符串不能跨行");
            break;
        }
        if (ch == '\\') {
            LexerAdvance (lexer);
            char escape = LexerPeek (lexer, 0);
            if (escape == '\0') {
                LexerFail (lexer, line, "字符串转义不完整");
                break;
            }
            LexerAdvance (lexer);
            switch (escape) {
                case '"':
                    if (!TextBufferAppendChar (&buffer, '"')) goto alloc_error;
                    break;
                case '\\':
                    if (!TextBufferAppendChar (&buffer, '\\')) goto alloc_error;
                    break;
                case '/':
                    if (!TextBufferAppendChar (&buffer, '/')) goto alloc_error;
                    break;
                case 'b':
                    if (!TextBufferAppendChar (&buffer, '\b')) goto alloc_error;
                    break;
                case 'f':
                    if (!TextBufferAppendChar (&buffer, '\f')) goto alloc_error;
                    break;
                case 'n':
                    if (!TextBufferAppendChar (&buffer, '\n')) goto alloc_error;
                    break;
                case 'r':
                    if (!TextBufferAppendChar (&buffer, '\r')) goto alloc_error;
                    break;
                case 't':
                    if (!TextBufferAppendChar (&buffer, '\t')) goto alloc_error;
                    break;
                case 'u': {
                    uint32_t codepoint;
                    if (!LexerReadHex4 (lexer, &codepoint)) {
                        LexerFail (lexer, line, "无效的 \\u 转义");
                        goto failed;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (LexerPeek (lexer, 0) != '\\' || LexerPeek (lexer, 1) != 'u') {
                            LexerFail (lexer, line, "缺少低代理项");
                            goto failed;
                        }
                        LexerAdvance (lexer);
                        LexerAdvance (lexer);
                        uint32_t low;
                        if (!LexerReadHex4 (lexer, &low) || low < 0xDC00 || low > 0xDFFF) {
                            LexerFail (lexer, line, "无效的低代理项");
                            goto failed;
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        LexerFail (lexer, line, "无效的代理项");
                        goto failed;
                    }
                    if (!AppendUtf8 (&buffer, codepoint)) goto alloc_error;
                    break;
                }
                default:
                    LexerFail (lexer, line, "未知的字符串转义 \\%c", escape);
                    goto failed;
            }
            continue;
        }
        if ((unsigned char)ch < 0x20) {
            LexerFail (lexer, line, "字符串中包含控制字符");
            break;
        }
        if (!TextBufferAppendChar (&buffer, ch)) goto alloc_error;
        LexerAdvance (lexer);
    }

    LexerFail (lexer, line, "字符串没有结束引号");
failed:
    TextBufferFree (&buffer);
    return NULL;
alloc_error:
    LexerFail (lexer, line, "内存不足");
    TextBufferFree (&buffer);
    return NULL;
}

static bool IsObjectEventName (const char *name) {
    return strcmp (name, "ontouch") == 0 ||
           strcmp (name, "ondrag") == 0 ||
           strcmp (name, "dragged") == 0 ||
           strcmp (name, "touched") == 0;
}

static bool IsReservedName (const char *name, bool includeEvents) {
    static const char *reserved[] = {
        "self", "messagef", "rand", "int", "object", "user", "flip",
        "size", "assets", "type", "caller", "typeof", "menu", "move",
        "pos", "fork", "untouch", "hide", "show", "locked", "color",
        "script", "remove", NULL
    };
    for (int i = 0; reserved[i] != NULL; i++) {
        if (strcmp (name, reserved[i]) == 0) return true;
    }
    return includeEvents && IsObjectEventName (name);
}

static TokenType KeywordType (const char* text) {
    if (strcmp (text, "func") == 0) return TOK_FUNC;
    if (strcmp (text, "var") == 0) return TOK_VAR;
    if (strcmp (text, "unvar") == 0) return TOK_UNVAR;
    if (strcmp (text, "if") == 0) return TOK_IF;
    if (strcmp (text, "elif") == 0) return TOK_ELIF;
    if (strcmp (text, "while") == 0) return TOK_WHILE;
    if (strcmp (text, "for") == 0) return TOK_FOR;
    if (strcmp (text, "in") == 0) return TOK_IN;
    if (strcmp (text, "else") == 0) return TOK_ELSE;
    if (strcmp (text, "return") == 0) return TOK_RETURN;
    if (strcmp (text, "true") == 0) return TOK_TRUE;
    if (strcmp (text, "false") == 0) return TOK_FALSE;
    if (strcmp (text, "null") == 0) return TOK_NULL;
    return TOK_IDENT;
}

static void LexerQueue (Lexer* lexer, TokenType type, int line) {
    if (lexer->pendingCount >= (int)(sizeof (lexer->pending) / sizeof (lexer->pending[0]))) return;
    lexer->pending[lexer->pendingCount] = type;
    lexer->pendingLine[lexer->pendingCount] = line;
    lexer->pendingCount++;
}

static bool LexerTakePending (Lexer* lexer) {
    if (lexer->pendingIndex >= lexer->pendingCount) return false;
    lexer->token.type = lexer->pending[lexer->pendingIndex];
    lexer->token.line = lexer->pendingLine[lexer->pendingIndex];
    lexer->token.column = 1;
    lexer->token.number = 0.0;
    lexer->pendingIndex++;
    if (lexer->pendingIndex == lexer->pendingCount) {
        lexer->pendingIndex = 0;
        lexer->pendingCount = 0;
    }
    return true;
}

static void LexerNext (Lexer* lexer) {
    free (lexer->token.text);
    lexer->token.text = NULL;

    if (LexerTakePending (lexer)) return;

    while (1) {
        if (lexer->lineStart && lexer->bracketDepth == 0) {
            size_t pos = lexer->pos;
            int width = 0;
            while (pos < lexer->length) {
                char c = lexer->source[pos];
                if (c == '\t') {
                    width += SCRIPT_TAB_WIDTH;
                    pos++;
                } else if (c == ' ') {
                    width++;
                    pos++;
                } else {
                    break;
                }
            }
            char next = (pos < lexer->length) ? lexer->source[pos] : '\0';
            bool blank = (next == '\0' || next == '\n' || next == '\r');
            if (blank) {
                while (lexer->pos < pos) LexerAdvance (lexer);
                if (lexer->pos < lexer->length) {
                    if (LexerPeek (lexer, 0) == '\r') LexerAdvance (lexer);
                    if (lexer->pos < lexer->length && LexerPeek (lexer, 0) == '\n') LexerAdvance (lexer);
                }
                if (lexer->pos >= lexer->length) lexer->lineStart = false;
                continue;
            }

            int line = lexer->line;
            int current = lexer->indentStack[lexer->indentCount - 1];
            if (width > current) {
                if (lexer->indentCount >= 64) {
                    LexerFail (lexer, line, "缩进层级过深");
                    lexer->token.type = TOK_ERROR;
                    lexer->token.line = line;
                    return;
                }
                lexer->indentStack[lexer->indentCount++] = width;
                while (lexer->pos < pos) LexerAdvance (lexer);
                lexer->lineStart = false;
                LexerQueue (lexer, TOK_INDENT, line);
                LexerTakePending (lexer);
                return;
            }
            if (width < current) {
                int found = -1;
                for (int i = lexer->indentCount - 1; i >= 0; i--) {
                    if (lexer->indentStack[i] == width) {
                        found = i;
                        break;
                    }
                }
                if (found < 0) {
                    LexerFail (lexer, line, "缩进层级不匹配");
                    lexer->token.type = TOK_ERROR;
                    lexer->token.line = line;
                    return;
                }
                while (lexer->pos < pos) LexerAdvance (lexer);
                lexer->lineStart = false;
                while (lexer->indentCount - 1 > found) {
                    lexer->indentCount--;
                    LexerQueue (lexer, TOK_DEDENT, line);
                }
                LexerTakePending (lexer);
                return;
            }
            while (lexer->pos < pos) LexerAdvance (lexer);
            lexer->lineStart = false;
        }

        while (lexer->pos < lexer->length) {
            char ch = LexerPeek (lexer, 0);
            if (ch == ' ' || ch == '\t') {
                LexerAdvance (lexer);
            } else {
                break;
            }
        }

        if (lexer->pos >= lexer->length) {
            if (lexer->indentCount > 1) {
                lexer->indentCount--;
                lexer->token.type = TOK_DEDENT;
                lexer->token.line = lexer->line;
                lexer->token.column = lexer->column;
                lexer->token.number = 0.0;
                return;
            }
            lexer->token.type = TOK_EOF;
            lexer->token.line = lexer->line;
            lexer->token.column = lexer->column;
            return;
        }

        char ch = LexerPeek (lexer, 0);
        if (ch == '\n' || ch == '\r') {
            int line = lexer->line;
            if (ch == '\r') LexerAdvance (lexer);
            if (lexer->pos < lexer->length && LexerPeek (lexer, 0) == '\n') LexerAdvance (lexer);
            if (lexer->bracketDepth > 0) {
                LexerFail (lexer, line, "JSON 不能跨行书写，必须写在一行内");
                lexer->token.type = TOK_ERROR;
                lexer->token.line = line;
                return;
            }
            lexer->lineStart = true;
            lexer->token.type = TOK_NEWLINE;
            lexer->token.line = line;
            lexer->token.column = 1;
            return;
        }
        break;
    }

    int line = lexer->line;
    int column = lexer->column;
    lexer->token.line = line;
    lexer->token.column = column;
    lexer->token.number = 0.0;
    lexer->token.integer = 0;
    lexer->token.isInteger = false;
    char ch = LexerPeek (lexer, 0);

    if (isalpha ((unsigned char)ch) || ch == '_') {
        size_t start = lexer->pos;
        while (lexer->pos < lexer->length) {
            char c = LexerPeek (lexer, 0);
            if (!isalnum ((unsigned char)c) && c != '_') break;
            LexerAdvance (lexer);
        }
        size_t length = lexer->pos - start;
        lexer->token.text = malloc (length + 1);
        if (lexer->token.text == NULL) {
            LexerFail (lexer, line, "内存不足");
            lexer->token.type = TOK_ERROR;
            return;
        }
        memcpy (lexer->token.text, lexer->source + start, length);
        lexer->token.text[length] = '\0';
        lexer->token.type = KeywordType (lexer->token.text);
        return;
    }

    if (isdigit ((unsigned char)ch) || (ch == '.' && isdigit ((unsigned char)LexerPeek (lexer, 1)))) {
        size_t start = lexer->pos;
        bool isFloat = false;
        while (isdigit ((unsigned char)LexerPeek (lexer, 0))) LexerAdvance (lexer);
        if (LexerPeek (lexer, 0) == '.') {
            isFloat = true;
            LexerAdvance (lexer);
            while (isdigit ((unsigned char)LexerPeek (lexer, 0))) LexerAdvance (lexer);
        }
        if (LexerPeek (lexer, 0) == 'e' || LexerPeek (lexer, 0) == 'E') {
            isFloat = true;
            LexerAdvance (lexer);
            if (LexerPeek (lexer, 0) == '+' || LexerPeek (lexer, 0) == '-') LexerAdvance (lexer);
            if (!isdigit ((unsigned char)LexerPeek (lexer, 0))) {
                LexerFail (lexer, line, "无效的数字指数");
                lexer->token.type = TOK_ERROR;
                return;
            }
            while (isdigit ((unsigned char)LexerPeek (lexer, 0))) LexerAdvance (lexer);
        }
        size_t numberLength = lexer->pos - start;
        char* numberText = malloc (numberLength + 1);
        if (numberText == NULL) {
            LexerFail (lexer, line, "内存不足");
            lexer->token.type = TOK_ERROR;
            return;
        }
        memcpy (numberText, lexer->source + start, numberLength);
        numberText[numberLength] = '\0';
        char* end = NULL;
        if (isFloat) {
            double number = strtod (numberText, &end);
            bool validNumber = (end == numberText + numberLength && isfinite (number));
            free (numberText);
            if (!validNumber) {
                LexerFail (lexer, line, "无效的数字");
                lexer->token.type = TOK_ERROR;
                return;
            }
            lexer->token.number = number;
        } else {
            errno = 0;
            unsigned long long integer = strtoull (numberText, &end, 10);
            bool validNumber = (end == numberText + numberLength && errno != ERANGE);
            free (numberText);
            if (!validNumber) {
                LexerFail (lexer, line, "整数超出 64 位无符号范围");
                lexer->token.type = TOK_ERROR;
                return;
            }
            lexer->token.integer = (u64)integer;
            lexer->token.number = (double)integer;
            lexer->token.isInteger = true;
        }
        lexer->token.type = TOK_NUMBER;
        return;
    }

    if (ch == '"') {
        lexer->token.text = LexerReadString (lexer, line);
        if (lexer->token.text == NULL) {
            lexer->token.type = TOK_ERROR;
            return;
        }
        lexer->token.type = TOK_STRING;
        return;
    }

    LexerAdvance (lexer);
    switch (ch) {
        case '{':
            lexer->token.type = TOK_LBRACE;
            lexer->bracketDepth++;
            break;
        case '}':
            lexer->token.type = TOK_RBRACE;
            if (lexer->bracketDepth > 0) lexer->bracketDepth--;
            break;
        case '(':
            lexer->token.type = TOK_LPAREN;
            break;
        case ')':
            lexer->token.type = TOK_RPAREN;
            break;
        case '[':
            lexer->token.type = TOK_LBRACKET;
            lexer->bracketDepth++;
            break;
        case ']':
            lexer->token.type = TOK_RBRACKET;
            if (lexer->bracketDepth > 0) lexer->bracketDepth--;
            break;
        case ',':
            lexer->token.type = TOK_COMMA;
            break;
        case ':':
            lexer->token.type = TOK_COLON;
            break;
        case ';':
            lexer->token.type = TOK_SEMI;
            break;
        case '+':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_PLUS_ASSIGN;
            } else
                lexer->token.type = TOK_PLUS;
            break;
        case '-':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_MINUS_ASSIGN;
            } else
                lexer->token.type = TOK_MINUS;
            break;
        case '*':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_STAR_ASSIGN;
            } else
                lexer->token.type = TOK_STAR;
            break;
        case '/':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_SLASH_ASSIGN;
            } else
                lexer->token.type = TOK_SLASH;
            break;
        case '%':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_PERCENT_ASSIGN;
            } else
                lexer->token.type = TOK_PERCENT;
            break;
        case '#':
            lexer->token.type = TOK_HASH;
            break;
        case '@':
            lexer->token.type = TOK_AT;
            break;
        case '!':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_NE;
            } else
                lexer->token.type = TOK_BANG;
            break;
        case '&':
            if (LexerPeek (lexer, 0) != '&') {
                LexerFail (lexer, line, "未知字符 '&'");
                lexer->token.type = TOK_ERROR;
                return;
            }
            LexerAdvance (lexer);
            lexer->token.type = TOK_AND;
            break;
        case '|':
            if (LexerPeek (lexer, 0) != '|') {
                LexerFail (lexer, line, "未知字符 '|'");
                lexer->token.type = TOK_ERROR;
                return;
            }
            LexerAdvance (lexer);
            lexer->token.type = TOK_OR;
            break;
        case '=':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_EQ;
            } else
                lexer->token.type = TOK_ASSIGN;
            break;
        case '<':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_LE;
            } else
                lexer->token.type = TOK_LT;
            break;
        case '>':
            if (LexerPeek (lexer, 0) == '=') {
                LexerAdvance (lexer);
                lexer->token.type = TOK_GE;
            } else
                lexer->token.type = TOK_GT;
            break;
        default:
            LexerFail (lexer, line, "未知字符 '%c'", ch);
            lexer->token.type = TOK_ERROR;
            return;
    }
}

typedef enum {
    EXPR_LITERAL,
    EXPR_VARIABLE,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_ARRAY,
    EXPR_OBJECT,
    EXPR_MEMBER
} ExprType;

typedef struct Expr Expr;
struct Expr {
    ExprType type;
    int line;
    union {
        struct json_object* literal;
        char* name;
        struct {
            TokenType op;
            Expr* operand;
        } unary;
        struct {
            TokenType op;
            Expr* left;
            Expr* right;
        } binary;
        struct {
            char* name;
            Expr* callee;
            Expr* selfExpr;
            Expr** args;
            int count;
            int capacity;
        } call;
        struct {
            Expr** items;
            int count;
            int capacity;
        } array;
        struct {
            char** keys;
            Expr** values;
            int count;
            int capacity;
        } object;
        struct {
            Expr* base;
            Expr* key;
        } member;
    } as;
};

typedef enum { STMT_VAR, STMT_UNVAR, STMT_ASSIGN, STMT_EXPR, STMT_RETURN, STMT_FUNC, STMT_IF, STMT_WHILE, STMT_FOR } StmtType;

typedef struct Stmt Stmt;

typedef struct {
    Stmt** items;
    int count;
    int capacity;
} StmtBlock;

struct Stmt {
    StmtType type;
    int line;
    union {
        struct {
            bool isGlobal;
            char* name;
            Expr* initializer;
        } var;
        struct {
            Expr* target;
            Expr* value;
            TokenType op;
        } assign;
        char* unvarName;
        struct {
            char* name;
            char** params;
            int paramCount;
            int paramCapacity;
            char* body;
        } func;
        Expr* expr;
        Expr* ret;
        struct {
            Expr* condition;
            StmtBlock thenBlock;
            StmtBlock elseBlock;
            bool hasElse;
        } ifStmt;
        struct {
            Expr* condition;
            StmtBlock body;
        } whileStmt;
        struct {
            char* key;
            char* value;
            Expr* iterable;
            StmtBlock body;
        } forStmt;
    } as;
};

typedef struct Env Env;

typedef struct {
    Stmt** statements;
    int statementCount;
    int statementCapacity;
} Program;

static TextBuffer ProgramParse (const char* source, Program* program);

typedef struct {
    Lexer lexer;
    TextBuffer error;
    bool failed;
} Parser;

static Expr* ExprNew (ExprType type, int line) {
    Expr* expr = calloc (1, sizeof (Expr));
    if (expr != NULL) {
        expr->type = type;
        expr->line = line;
    }
    return expr;
}

static void ExprFree (Expr* expr);
static void StmtFree (Stmt* stmt);

static void ParserNext (Parser* parser);

static bool ParserCheck (Parser* parser, TokenType type) { return parser->lexer.token.type == type; }

static bool ParserMatch (Parser* parser, TokenType type) {
    if (!ParserCheck (parser, type)) return false;
    ParserNext (parser);
    return true;
}

static bool ParserConsume (Parser* parser, TokenType type, const char* message) {
    if (ParserCheck (parser, type)) {
        ParserNext (parser);
        return true;
    }
    SetError (&parser->error, parser->lexer.token.line, "%s", message);
    parser->failed = true;
    return false;
}

static void ParserTakeLexerError (Parser* parser) {
    if (parser->error.length == 0) {
        parser->error = parser->lexer.error;
        parser->lexer.error = (TextBuffer){0};
    } else {
        TextBufferFree (&parser->lexer.error);
    }
    parser->failed = true;
}

static void ParserNext (Parser* parser) {
    LexerNext (&parser->lexer);
    if (parser->lexer.token.type == TOK_ERROR) ParserTakeLexerError (parser);
}

static bool ParserEndStatement (Parser* parser, int line) {
    if (parser->failed) return false;
    if (ParserCheck (parser, TOK_NEWLINE)) {
        ParserNext (parser);
        return !parser->failed;
    }
    if (ParserCheck (parser, TOK_DEDENT) || ParserCheck (parser, TOK_EOF)) return true;
    if (ParserCheck (parser, TOK_SEMI))
        SetError (&parser->error, line, "不使用分号，语句以换行结束");
    else
        SetError (&parser->error, line, "语句末尾需要换行");
    parser->failed = true;
    return false;
}

static Stmt* ParseFunctionDefinition (Parser* parser);
static Stmt* ParseIf (Parser* parser);
static Stmt* ParseWhile (Parser* parser);
static Stmt* ParseFor (Parser* parser);
static bool ParseBlock (Parser* parser, StmtBlock* block);
static void StmtBlockFree (StmtBlock* block);

static Expr* ParseExpression (Parser* parser);
static Expr* ParseArray (Parser* parser, int line);
static Expr* ParseObject (Parser* parser, int line);
static Expr* ParseCall (Parser* parser, char* name, int line);
static Expr* ParseCallExpression (Parser* parser, Expr* callee, int line);
static bool ParseCallTail (Parser* parser, Expr* call, int line);
static Expr* ParseSelfValue (Parser* parser);

static bool ExprArrayAdd (Expr* array, Expr* item) {
    if (array->as.array.count == array->as.array.capacity) {
        int capacity = (array->as.array.capacity == 0) ? 4 : array->as.array.capacity * 2;
        Expr** items = realloc (array->as.array.items, (size_t)capacity * sizeof (Expr*));
        if (items == NULL) return false;
        array->as.array.items = items;
        array->as.array.capacity = capacity;
    }
    array->as.array.items[array->as.array.count++] = item;
    return true;
}

static bool ExprObjectAdd (Expr* object, char* key, Expr* value) {
    if (object->as.object.count == object->as.object.capacity) {
        int capacity = (object->as.object.capacity == 0) ? 4 : object->as.object.capacity * 2;
        char** keys = realloc (object->as.object.keys, (size_t)capacity * sizeof (char*));
        if (keys == NULL) return false;
        object->as.object.keys = keys;
        Expr** values = realloc (object->as.object.values, (size_t)capacity * sizeof (Expr*));
        if (values == NULL) return false;
        object->as.object.values = values;
        object->as.object.capacity = capacity;
    }
    object->as.object.keys[object->as.object.count] = key;
    object->as.object.values[object->as.object.count] = value;
    object->as.object.count++;
    return true;
}

static Expr* ParseSelfValue (Parser* parser) {
    int line = parser->lexer.token.line;

    if (ParserCheck (parser, TOK_LPAREN)) {
        ParserNext (parser);
        Expr* expr = ParseExpression (parser);
        if (expr == NULL) return NULL;
        if (!ParserConsume (parser, TOK_RPAREN, "缺少右括号 )")) {
            ExprFree (expr);
            return NULL;
        }
        return expr;
    }

    if (ParserCheck (parser, TOK_IDENT)) {
        char* name = strdup (parser->lexer.token.text);
        if (name == NULL) {
            parser->failed = true;
            return NULL;
        }
        ParserNext (parser);
        if (ParserCheck (parser, TOK_LPAREN))
            return ParseCall (parser, name, line);

        Expr* expr = ExprNew (EXPR_VARIABLE, line);
        if (expr == NULL) {
            free (name);
            parser->failed = true;
            return NULL;
        }
        expr->as.name = name;
        return expr;
    }

    if (ParserCheck (parser, TOK_NUMBER)) {
        Expr* expr = ExprNew (EXPR_LITERAL, line);
        if (expr == NULL) {
            parser->failed = true;
            return NULL;
        }
        Token token = parser->lexer.token;
        expr->as.literal =
            token.isInteger ? json_object_new_uint64 (token.integer) : json_object_new_double (token.number);
        if (expr->as.literal == NULL) {
            ExprFree (expr);
            parser->failed = true;
            return NULL;
        }
        ParserNext (parser);
        return expr;
    }

    SetError (&parser->error, line, "@ 后需要变量、数字、函数调用或括号表达式");
    parser->failed = true;
    return NULL;
}

static Expr* ParsePrimary (Parser* parser) {
    int line = parser->lexer.token.line;
    Token token = parser->lexer.token;

    switch (token.type) {
        case TOK_NUMBER: {
            Expr* expr = ExprNew (EXPR_LITERAL, line);
            if (expr == NULL) return NULL;
            expr->as.literal =
                token.isInteger ? json_object_new_uint64 (token.integer) : json_object_new_double (token.number);
            if (expr->as.literal == NULL) {
                ExprFree (expr);
                return NULL;
            }
            ParserNext (parser);
            return expr;
        }
        case TOK_STRING: {
            Expr* expr = ExprNew (EXPR_LITERAL, line);
            if (expr == NULL) return NULL;
            expr->as.literal = json_object_new_string (token.text);
            if (expr->as.literal == NULL) {
                ExprFree (expr);
                return NULL;
            }
            ParserNext (parser);
            return expr;
        }
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_NULL: {
            Expr* expr = ExprNew (EXPR_LITERAL, line);
            if (expr == NULL) return NULL;
            if (token.type != TOK_NULL) {
                expr->as.literal = json_object_new_boolean (token.type == TOK_TRUE);
                if (expr->as.literal == NULL) {
                    ExprFree (expr);
                    return NULL;
                }
            }
            ParserNext (parser);
            return expr;
        }
        case TOK_IDENT: {
            char* name = strdup (token.text);
            if (name == NULL) return NULL;
            ParserNext (parser);

            if (ParserCheck (parser, TOK_LPAREN)) return ParseCall (parser, name, line);
            if (ParserCheck (parser, TOK_AT)) {
                SetError (&parser->error, parser->lexer.token.line,
                          "@ 只能跟在函数调用的右括号之后");
                parser->failed = true;
                free (name);
                return NULL;
            }

            Expr* expr = ExprNew (EXPR_VARIABLE, line);
            if (expr == NULL) {
                free (name);
                return NULL;
            }
            expr->as.name = name;
            return expr;
        }
        case TOK_LPAREN: {
            ParserNext (parser);
            Expr* expr = ParseExpression (parser);
            if (expr == NULL) return NULL;
            if (!ParserConsume (parser, TOK_RPAREN, "缺少右括号 )")) {
                ExprFree (expr);
                return NULL;
            }
            return expr;
        }
        case TOK_LBRACKET:
            return ParseArray (parser, line);
        case TOK_LBRACE:
            return ParseObject (parser, line);
        default:
            SetError (&parser->error, line, "需要值或表达式");
            parser->failed = true;
            return NULL;
    }
}

static Expr* ParseArray (Parser* parser, int line) {
    ParserNext (parser);
    Expr* array = ExprNew (EXPR_ARRAY, line);
    if (array == NULL) return NULL;

    if (ParserCheck (parser, TOK_RBRACKET)) {
        ParserNext (parser);
        return array;
    }

    while (!parser->failed) {
        Expr* item = ParseExpression (parser);
        if (item == NULL) break;
        if (!ExprArrayAdd (array, item)) {
            SetError (&parser->error, line, "内存不足");
            parser->failed = true;
            ExprFree (item);
            break;
        }
        if (ParserMatch (parser, TOK_COMMA)) {
            if (ParserCheck (parser, TOK_RBRACKET)) break;
            continue;
        }
        break;
    }

    if (!parser->failed && !ParserConsume (parser, TOK_RBRACKET, "缺少右方括号 ]")) {
        parser->failed = true;
    }
    if (parser->failed) {
        ExprFree (array);
        return NULL;
    }
    return array;
}

static Expr* ParseObject (Parser* parser, int line) {
    ParserNext (parser);
    Expr* object = ExprNew (EXPR_OBJECT, line);
    if (object == NULL) return NULL;

    if (ParserCheck (parser, TOK_RBRACE)) {
        ParserNext (parser);
        return object;
    }

    while (!parser->failed) {
        if (!ParserCheck (parser, TOK_STRING)) {
            SetError (&parser->error, parser->lexer.token.line, "JSON 对象的键必须是字符串");
            parser->failed = true;
            break;
        }
        char* key = strdup (parser->lexer.token.text);
        if (key == NULL) {
            parser->failed = true;
            break;
        }
        ParserNext (parser);
        if (!ParserConsume (parser, TOK_COLON, "缺少冒号 :")) {
            free (key);
            break;
        }
        Expr* value = ParseExpression (parser);
        if (value == NULL) {
            free (key);
            break;
        }
        if (!ExprObjectAdd (object, key, value)) {
            SetError (&parser->error, line, "内存不足");
            parser->failed = true;
            free (key);
            ExprFree (value);
            break;
        }
        if (ParserMatch (parser, TOK_COMMA)) {
            if (ParserCheck (parser, TOK_RBRACE)) break;
            continue;
        }
        break;
    }

    if (!parser->failed && !ParserConsume (parser, TOK_RBRACE, "缺少右花括号 }")) {
        parser->failed = true;
    }
    if (parser->failed) {
        ExprFree (object);
        return NULL;
    }
    return object;
}

static bool ParseCallTail (Parser* parser, Expr* call, int line) {
    ParserNext (parser);

    if (!ParserCheck (parser, TOK_RPAREN)) {
        while (!parser->failed) {
            Expr* arg = ParseExpression (parser);
            if (arg == NULL) break;
            if (call->as.call.count == call->as.call.capacity) {
                int capacity = (call->as.call.capacity == 0) ? 4 : call->as.call.capacity * 2;
                Expr** args = realloc (call->as.call.args, (size_t)capacity * sizeof (Expr*));
                if (args == NULL) {
                    SetError (&parser->error, line, "内存不足");
                    parser->failed = true;
                    ExprFree (arg);
                    break;
                }
                call->as.call.args = args;
                call->as.call.capacity = capacity;
            }
            call->as.call.args[call->as.call.count++] = arg;
            if (ParserMatch (parser, TOK_COMMA)) {
                if (ParserCheck (parser, TOK_RPAREN)) break;
                continue;
            }
            break;
        }
    }

    if (!parser->failed && !ParserConsume (parser, TOK_RPAREN, "缺少右括号 )"))
        parser->failed = true;
    if (!parser->failed && ParserCheck (parser, TOK_AT)) {
        if (call->as.call.name == NULL) {
            SetError (&parser->error, parser->lexer.token.line,
                      "@ 只能用在函数名调用之后");
            parser->failed = true;
        } else {
            ParserNext (parser);
            call->as.call.selfExpr = ParseSelfValue (parser);
            if (call->as.call.selfExpr == NULL) parser->failed = true;
        }
    }
    return !parser->failed;
}

static Expr* ParseCall (Parser* parser, char* name, int line) {
    Expr* call = ExprNew (EXPR_CALL, line);
    if (call == NULL) {
        free (name);
        return NULL;
    }
    call->as.call.name = name;
    if (!ParseCallTail (parser, call, line)) {
        ExprFree (call);
        return NULL;
    }
    return call;
}

static Expr* ParseCallExpression (Parser* parser, Expr* callee, int line) {
    Expr* call = ExprNew (EXPR_CALL, line);
    if (call == NULL) {
        ExprFree (callee);
        parser->failed = true;
        return NULL;
    }
    call->as.call.callee = callee;
    if (!ParseCallTail (parser, call, line)) {
        ExprFree (call);
        return NULL;
    }
    return call;
}

static Expr* ParsePostfix (Parser* parser) {
    Expr* expr = ParsePrimary (parser);
    while (expr != NULL && (ParserCheck (parser, TOK_HASH) || ParserCheck (parser, TOK_LPAREN))) {
        int line = parser->lexer.token.line;
        if (ParserCheck (parser, TOK_LPAREN)) {
            expr = ParseCallExpression (parser, expr, line);
            continue;
        }
        ParserNext (parser);

        Expr* key = NULL;
        if (ParserCheck (parser, TOK_STRING)) {
            key = ExprNew (EXPR_LITERAL, line);
            if (key == NULL) { ExprFree (expr); parser->failed = true; return NULL; }
            key->as.literal = json_object_new_string (parser->lexer.token.text);
            if (key->as.literal == NULL) {
                ExprFree (key);
                ExprFree (expr);
                parser->failed = true;
                return NULL;
            }
            ParserNext (parser);
        } else if (ParserCheck (parser, TOK_IDENT)) {
            key = ExprNew (EXPR_VARIABLE, line);
            if (key == NULL) { ExprFree (expr); parser->failed = true; return NULL; }
            key->as.name = strdup (parser->lexer.token.text);
            if (key->as.name == NULL) {
                ExprFree (key);
                ExprFree (expr);
                parser->failed = true;
                return NULL;
            }
            ParserNext (parser);
        } else if (ParserCheck (parser, TOK_NUMBER)) {
            Token token = parser->lexer.token;
            if (!token.isInteger) {
                SetError (&parser->error, line, "数组下标必须是整数");
                parser->failed = true;
                ExprFree (expr);
                return NULL;
            }
            key = ExprNew (EXPR_LITERAL, line);
            if (key == NULL) { ExprFree (expr); parser->failed = true; return NULL; }
            key->as.literal = json_object_new_uint64 (token.integer);
            if (key->as.literal == NULL) {
                ExprFree (key);
                ExprFree (expr);
                parser->failed = true;
                return NULL;
            }
            ParserNext (parser);
        } else {
            SetError (&parser->error, parser->lexer.token.line,
                "# 后需要字符串、标识符或整数下标");
            parser->failed = true;
            ExprFree (expr);
            return NULL;
        }

        Expr* member = ExprNew (EXPR_MEMBER, line);
        if (member == NULL) {
            ExprFree (key);
            ExprFree (expr);
            SetError (&parser->error, line, "内存不足");
            parser->failed = true;
            return NULL;
        }
        member->as.member.base = expr;
        member->as.member.key = key;
        expr = member;
    }
    return expr;
}

static Expr* MakeBinary (Parser* parser, TokenType op, Expr* left, Expr* right) {
    Expr* expr = ExprNew (EXPR_BINARY, left->line);
    if (expr == NULL) {
        ExprFree (left);
        ExprFree (right);
        parser->failed = true;
        return NULL;
    }
    expr->as.binary.op = op;
    expr->as.binary.left = left;
    expr->as.binary.right = right;
    return expr;
}

static Expr* ParseUnary (Parser* parser) {
    if (ParserCheck (parser, TOK_PLUS) || ParserCheck (parser, TOK_MINUS) || ParserCheck (parser, TOK_BANG)) {
        TokenType op = parser->lexer.token.type;
        int line = parser->lexer.token.line;
        ParserNext (parser);
        Expr* operand = ParseUnary (parser);
        if (operand == NULL) return NULL;
        Expr* expr = ExprNew (EXPR_UNARY, line);
        if (expr == NULL) {
            ExprFree (operand);
            parser->failed = true;
            return NULL;
        }
        expr->as.unary.op = op;
        expr->as.unary.operand = operand;
        return expr;
    }
    return ParsePostfix (parser);
}

static Expr* ParseMultiplicative (Parser* parser) {
    Expr* expr = ParseUnary (parser);
    while (expr != NULL &&
           (ParserCheck (parser, TOK_STAR) || ParserCheck (parser, TOK_SLASH) || ParserCheck (parser, TOK_PERCENT))) {
        TokenType op = parser->lexer.token.type;
        ParserNext (parser);
        Expr* right = ParseUnary (parser);
        if (right == NULL) {
            ExprFree (expr);
            return NULL;
        }
        expr = MakeBinary (parser, op, expr, right);
    }
    return expr;
}

static Expr* ParseAdditive (Parser* parser) {
    Expr* expr = ParseMultiplicative (parser);
    while (expr != NULL && (ParserCheck (parser, TOK_PLUS) || ParserCheck (parser, TOK_MINUS))) {
        TokenType op = parser->lexer.token.type;
        ParserNext (parser);
        Expr* right = ParseMultiplicative (parser);
        if (right == NULL) {
            ExprFree (expr);
            return NULL;
        }
        expr = MakeBinary (parser, op, expr, right);
    }
    return expr;
}

static Expr* ParseRelational (Parser* parser) {
    Expr* expr = ParseAdditive (parser);
    while (expr != NULL && (ParserCheck (parser, TOK_LT) || ParserCheck (parser, TOK_LE) ||
                            ParserCheck (parser, TOK_GT) || ParserCheck (parser, TOK_GE))) {
        TokenType op = parser->lexer.token.type;
        ParserNext (parser);
        Expr* right = ParseAdditive (parser);
        if (right == NULL) {
            ExprFree (expr);
            return NULL;
        }
        expr = MakeBinary (parser, op, expr, right);
    }
    return expr;
}

static Expr* ParseEquality (Parser* parser) {
    Expr* expr = ParseRelational (parser);
    while (expr != NULL && (ParserCheck (parser, TOK_EQ) || ParserCheck (parser, TOK_NE))) {
        TokenType op = parser->lexer.token.type;
        ParserNext (parser);
        Expr* right = ParseRelational (parser);
        if (right == NULL) {
            ExprFree (expr);
            return NULL;
        }
        expr = MakeBinary (parser, op, expr, right);
    }
    return expr;
}

static Expr* ParseLogicalAnd (Parser* parser) {
    Expr* expr = ParseEquality (parser);
    while (expr != NULL && ParserCheck (parser, TOK_AND)) {
        ParserNext (parser);
        Expr* right = ParseEquality (parser);
        if (right == NULL) {
            ExprFree (expr);
            return NULL;
        }
        expr = MakeBinary (parser, TOK_AND, expr, right);
    }
    return expr;
}

static Expr* ParseLogicalOr (Parser* parser) {
    Expr* expr = ParseLogicalAnd (parser);
    while (expr != NULL && ParserCheck (parser, TOK_OR)) {
        ParserNext (parser);
        Expr* right = ParseLogicalAnd (parser);
        if (right == NULL) {
            ExprFree (expr);
            return NULL;
        }
        expr = MakeBinary (parser, TOK_OR, expr, right);
    }
    return expr;
}

static Expr* ParseExpression (Parser* parser) { return ParseLogicalOr (parser); }

static Stmt* StmtNew (StmtType type, int line) {
    Stmt* stmt = calloc (1, sizeof (Stmt));
    if (stmt != NULL) {
        stmt->type = type;
        stmt->line = line;
    }
    return stmt;
}

static bool ProgramAddStatement (Program* program, Stmt* stmt) {
    if (program->statementCount == program->statementCapacity) {
        int capacity = (program->statementCapacity == 0) ? 8 : program->statementCapacity * 2;
        Stmt** statements = realloc (program->statements, (size_t)capacity * sizeof (Stmt*));
        if (statements == NULL) return false;
        program->statements = statements;
        program->statementCapacity = capacity;
    }
    program->statements[program->statementCount++] = stmt;
    return true;
}

static Stmt* ParseVar (Parser* parser, bool isGlobal, int line) {
    ParserNext (parser);
    if (!ParserCheck (parser, TOK_IDENT)) {
        SetError (&parser->error, parser->lexer.token.line, "var 后面需要变量名");
        parser->failed = true;
        return NULL;
    }
    char* name = strdup (parser->lexer.token.text);
    if (name == NULL) {
        parser->failed = true;
        return NULL;
    }
    if (strcmp (name, "self") == 0 || strcmp (name, "type") == 0 ||
        strcmp (name, "caller") == 0) {
        SetError (&parser->error, line, "%s 是只读特殊变量，不能重新定义", name);
        free (name);
        parser->failed = true;
        return NULL;
    }
    if (strcmp (name, "remove") == 0) {
        SetError (&parser->error, line, "remove 是特殊变量，不能重新定义");
        free (name);
        parser->failed = true;
        return NULL;
    }
    ParserNext (parser);

    Expr* initializer = NULL;
    if (ParserMatch (parser, TOK_ASSIGN)) {
        initializer = ParseExpression (parser);
        if (initializer == NULL) {
            free (name);
            return NULL;
        }
    }
    if (!ParserEndStatement (parser, line)) {
        free (name);
        ExprFree (initializer);
        return NULL;
    }

    Stmt* stmt = StmtNew (STMT_VAR, line);
    if (stmt == NULL) {
        free (name);
        ExprFree (initializer);
        parser->failed = true;
        return NULL;
    }
    stmt->as.var.isGlobal = isGlobal;
    stmt->as.var.name = name;
    stmt->as.var.initializer = initializer;
    return stmt;
}

static bool IsAssignmentToken (TokenType type) {
    return type == TOK_ASSIGN || type == TOK_PLUS_ASSIGN ||
           type == TOK_MINUS_ASSIGN || type == TOK_STAR_ASSIGN ||
           type == TOK_SLASH_ASSIGN || type == TOK_PERCENT_ASSIGN;
}

static Stmt* ParseStatement (Parser* parser, bool topLevel) {
    int line = parser->lexer.token.line;

    if (ParserCheck (parser, TOK_FUNC)) {
        return ParseFunctionDefinition (parser);
    }
    if (ParserCheck (parser, TOK_IF)) return ParseIf (parser);
    if (ParserCheck (parser, TOK_WHILE)) return ParseWhile (parser);
    if (ParserCheck (parser, TOK_FOR)) return ParseFor (parser);
    if (ParserCheck (parser, TOK_ELSE) || ParserCheck (parser, TOK_ELIF)) {
        SetError (&parser->error, line, ParserCheck (parser, TOK_ELIF)
                                          ? "elif 没有对应的 if"
                                          : "else 没有对应的 if");
        parser->failed = true;
        return NULL;
    }

    if (ParserCheck (parser, TOK_UNVAR)) {
        ParserNext (parser);
        if (!ParserCheck (parser, TOK_IDENT)) {
            SetError (&parser->error, parser->lexer.token.line, "unvar 后面需要变量名");
            parser->failed = true;
            return NULL;
        }
        char* name = strdup (parser->lexer.token.text);
        if (name == NULL) {
            parser->failed = true;
            return NULL;
        }
        if (strcmp (name, "self") == 0 || strcmp (name, "type") == 0 ||
        strcmp (name, "caller") == 0) {
            SetError (&parser->error, line, "%s 是只读特殊变量，不能 unvar", name);
            free (name);
            parser->failed = true;
            return NULL;
        }
        if (strcmp (name, "remove") == 0) {
            SetError (&parser->error, line, "remove 是特殊变量，不能 unvar");
            free (name);
            parser->failed = true;
            return NULL;
        }
        ParserNext (parser);
        if (!ParserEndStatement (parser, line)) {
            free (name);
            return NULL;
        }
        Stmt* stmt = StmtNew (STMT_UNVAR, line);
        if (stmt == NULL) {
            free (name);
            parser->failed = true;
            return NULL;
        }
        stmt->as.unvarName = name;
        return stmt;
    }

    if (ParserCheck (parser, TOK_VAR)) return ParseVar (parser, topLevel, line);

    if (ParserCheck (parser, TOK_RETURN)) {
        ParserNext (parser);
        Expr* value = NULL;
        if (!ParserCheck (parser, TOK_NEWLINE) && !ParserCheck (parser, TOK_DEDENT) && !ParserCheck (parser, TOK_EOF) &&
            !ParserCheck (parser, TOK_SEMI)) {
            value = ParseExpression (parser);
            if (value == NULL) return NULL;
        }
        if (!ParserEndStatement (parser, line)) {
            ExprFree (value);
            return NULL;
        }
        Stmt* stmt = StmtNew (STMT_RETURN, line);
        if (stmt == NULL) {
            ExprFree (value);
            parser->failed = true;
            return NULL;
        }
        stmt->as.ret = value;
        return stmt;
    }

    Expr* expr = ParseExpression (parser);
    if (expr == NULL) return NULL;

    if (IsAssignmentToken (parser->lexer.token.type)) {
        if (expr->type != EXPR_VARIABLE && expr->type != EXPR_MEMBER) {
            SetError (&parser->error, line, "赋值号左边必须是变量或成员");
            parser->failed = true;
            ExprFree (expr);
            return NULL;
        }
        if (expr->type == EXPR_VARIABLE &&
            (strcmp (expr->as.name, "self") == 0 || strcmp (expr->as.name, "type") == 0 ||
            strcmp (expr->as.name, "caller") == 0)) {
            SetError (&parser->error, line, "%s 是只读特殊变量，不能赋值",
                      expr->as.name);
            parser->failed = true;
            ExprFree (expr);
            return NULL;
        }
        Expr* target = expr;
        TokenType op = parser->lexer.token.type;
        ParserNext (parser);
        Expr* value = ParseExpression (parser);
        if (value == NULL) {
            ExprFree (target);
            return NULL;
        }
        if (!ParserEndStatement (parser, line)) {
            ExprFree (target);
            ExprFree (value);
            return NULL;
        }
        Stmt* stmt = StmtNew (STMT_ASSIGN, line);
        if (stmt == NULL) {
            ExprFree (target);
            ExprFree (value);
            parser->failed = true;
            return NULL;
        }
        stmt->as.assign.target = target;
        stmt->as.assign.value = value;
        stmt->as.assign.op = op;
        return stmt;
    }

    if (!ParserEndStatement (parser, line)) {
        ExprFree (expr);
        return NULL;
    }
    Stmt* stmt = StmtNew (STMT_EXPR, line);
    if (stmt == NULL) {
        ExprFree (expr);
        parser->failed = true;
        return NULL;
    }
    stmt->as.expr = expr;
    return stmt;
}

static bool StmtBlockAdd (StmtBlock* block, Stmt* stmt) {
    if (block->count == block->capacity) {
        int capacity = (block->capacity == 0) ? 8 : block->capacity * 2;
        Stmt** items = realloc (block->items, (size_t)capacity * sizeof (Stmt*));
        if (items == NULL) return false;
        block->items = items;
        block->capacity = capacity;
    }
    block->items[block->count++] = stmt;
    return true;
}

static void StmtBlockFree (StmtBlock* block) {
    for (int i = 0; i < block->count; i++) StmtFree (block->items[i]);
    free (block->items);
    memset (block, 0, sizeof (*block));
}

static bool ParseBlock (Parser* parser, StmtBlock* block) {
    if (!ParserConsume (parser, TOK_INDENT, "代码块需要缩进")) return false;
    while (!parser->failed) {
        while (ParserMatch (parser, TOK_NEWLINE)) {
        }
        if (parser->failed) break;
        if (ParserCheck (parser, TOK_DEDENT)) {
            ParserNext (parser);
            return true;
        }
        if (ParserCheck (parser, TOK_EOF)) {
            SetError (&parser->error, parser->lexer.token.line, "代码块缩进没有结束");
            parser->failed = true;
            break;
        }
        if (ParserCheck (parser, TOK_INDENT)) {
            SetError (&parser->error, parser->lexer.token.line, "意外的额外缩进");
            parser->failed = true;
            break;
        }
        Stmt* stmt = ParseStatement (parser, false);
        if (stmt == NULL) break;
        if (!StmtBlockAdd (block, stmt)) {
            SetError (&parser->error, stmt->line, "内存不足");
            StmtFree (stmt);
            parser->failed = true;
            break;
        }
    }
    return false;
}

static Stmt* ParseIf (Parser* parser) {
    int line = parser->lexer.token.line;
    const char *keyword = ParserCheck (parser, TOK_ELIF) ? "elif" : "if";
    ParserNext (parser);
    Expr* condition = ParseExpression (parser);
    if (condition == NULL) return NULL;
    char newlineError[64];
    snprintf (newlineError, sizeof (newlineError), "%s 条件后需要换行",
              keyword);
    if (!ParserConsume (parser, TOK_NEWLINE, newlineError)) {
        ExprFree (condition);
        return NULL;
    }

    StmtBlock thenBlock = {0};
    if (!ParseBlock (parser, &thenBlock)) {
        ExprFree (condition);
        StmtBlockFree (&thenBlock);
        return NULL;
    }

    StmtBlock elseBlock = {0};
    bool hasElse = false;
    if (ParserCheck (parser, TOK_ELIF)) {
        Stmt *elif = ParseIf (parser);
        if (elif == NULL) {
            ExprFree (condition);
            StmtBlockFree (&thenBlock);
            return NULL;
        }
        if (!StmtBlockAdd (&elseBlock, elif)) {
            StmtFree (elif);
            ExprFree (condition);
            StmtBlockFree (&thenBlock);
            SetError (&parser->error, line, "内存不足");
            parser->failed = true;
            return NULL;
        }
        hasElse = true;
    } else if (ParserCheck (parser, TOK_ELSE)) {
        hasElse = true;
        ParserNext (parser);
        if (!ParserConsume (parser, TOK_NEWLINE, "else 后需要换行")) {
            ExprFree (condition);
            StmtBlockFree (&thenBlock);
            return NULL;
        }
        if (!ParseBlock (parser, &elseBlock)) {
            ExprFree (condition);
            StmtBlockFree (&thenBlock);
            StmtBlockFree (&elseBlock);
            return NULL;
        }
    }

    Stmt* stmt = StmtNew (STMT_IF, line);
    if (stmt == NULL) {
        ExprFree (condition);
        StmtBlockFree (&thenBlock);
        StmtBlockFree (&elseBlock);
        parser->failed = true;
        return NULL;
    }
    stmt->as.ifStmt.condition = condition;
    stmt->as.ifStmt.thenBlock = thenBlock;
    stmt->as.ifStmt.elseBlock = elseBlock;
    stmt->as.ifStmt.hasElse = hasElse;
    return stmt;
}

static Stmt* ParseWhile (Parser* parser) {
    int line = parser->lexer.token.line;
    ParserNext (parser);
    Expr* condition = ParseExpression (parser);
    if (condition == NULL) return NULL;
    if (!ParserConsume (parser, TOK_NEWLINE, "while 条件后需要换行")) {
        ExprFree (condition);
        return NULL;
    }
    StmtBlock body = {0};
    if (!ParseBlock (parser, &body)) {
        ExprFree (condition);
        StmtBlockFree (&body);
        return NULL;
    }
    Stmt* stmt = StmtNew (STMT_WHILE, line);
    if (stmt == NULL) {
        ExprFree (condition);
        StmtBlockFree (&body);
        parser->failed = true;
        return NULL;
    }
    stmt->as.whileStmt.condition = condition;
    stmt->as.whileStmt.body = body;
    return stmt;
}

static Stmt* ParseFor (Parser* parser) {
    int line = parser->lexer.token.line;
    ParserNext (parser);

    if (!ParserCheck (parser, TOK_IDENT)) {
        SetError (&parser->error, parser->lexer.token.line, "for 后面需要变量名");
        parser->failed = true;
        return NULL;
    }
    char *key = strdup (parser->lexer.token.text);
    if (key == NULL) {
        parser->failed = true;
        return NULL;
    }
    if (strcmp (key, "self") == 0 || strcmp (key, "type") == 0 || strcmp (key, "caller") == 0) {
        SetError (&parser->error, line, "%s 是只读特殊变量，不能作为循环变量", key);
        free (key);
        parser->failed = true;
        return NULL;
    }
    if (strcmp (key, "remove") == 0) {
        SetError (&parser->error, line, "remove 是特殊变量，不能作为循环变量");
        free (key);
        parser->failed = true;
        return NULL;
    }
    ParserNext (parser);

    char *value = NULL;
    if (ParserMatch (parser, TOK_COMMA)) {
        if (!ParserCheck (parser, TOK_IDENT)) {
            SetError (&parser->error, parser->lexer.token.line, "for 后面需要第二个变量名");
            free (key);
            parser->failed = true;
            return NULL;
        }
        value = strdup (parser->lexer.token.text);
        if (value == NULL) {
            free (key);
            parser->failed = true;
            return NULL;
        }
        if (strcmp (value, "self") == 0 || strcmp (value, "type") == 0 || strcmp (value, "caller") == 0) {
            SetError (&parser->error, line, "%s 是只读特殊变量，不能作为循环变量", value);
            free (value);
            free (key);
            parser->failed = true;
            return NULL;
        }
        if (strcmp (value, "remove") == 0) {
            SetError (&parser->error, line, "remove 是特殊变量，不能作为循环变量");
            free (value);
            free (key);
            parser->failed = true;
            return NULL;
        }
        if (strcmp (key, value) == 0) {
            SetError (&parser->error, line, "循环变量 %s 重复", key);
            free (value);
            free (key);
            parser->failed = true;
            return NULL;
        }
        ParserNext (parser);
    }

    if (!ParserConsume (parser, TOK_IN, "for 缺少 in")) {
        free (value);
        free (key);
        return NULL;
    }

    Expr *iterable = ParseExpression (parser);
    if (iterable == NULL) {
        free (value);
        free (key);
        return NULL;
    }
    if (!ParserConsume (parser, TOK_NEWLINE, "for 语句后需要换行")) {
        ExprFree (iterable);
        free (value);
        free (key);
        return NULL;
    }

    StmtBlock body = {0};
    if (!ParseBlock (parser, &body)) {
        ExprFree (iterable);
        free (value);
        free (key);
        StmtBlockFree (&body);
        return NULL;
    }

    Stmt *stmt = StmtNew (STMT_FOR, line);
    if (stmt == NULL) {
        ExprFree (iterable);
        free (value);
        free (key);
        StmtBlockFree (&body);
        parser->failed = true;
        return NULL;
    }
    stmt->as.forStmt.key = key;
    stmt->as.forStmt.value = value;
    stmt->as.forStmt.iterable = iterable;
    stmt->as.forStmt.body = body;
    return stmt;
}

static void FreeParamList (char **params, int count) {
    for (int i = 0; i < count; i++) free (params[i]);
    free (params);
}

static size_t StripIndentWidth (const char *source, size_t start, size_t end,
                                int width) {
    size_t pos = start;
    int current = 0;
    while (pos < end && current < width) {
        if (source[pos] == '\t') current += SCRIPT_TAB_WIDTH;
        else if (source[pos] == ' ') current++;
        else break;
        pos++;
    }
    return pos;
}

static Stmt* ParseFunctionDefinition (Parser* parser) {
    int line = parser->lexer.token.line;
    ParserNext (parser);

    if (!ParserCheck (parser, TOK_IDENT)) {
        SetError (&parser->error, parser->lexer.token.line, "func 后面需要函数名");
        parser->failed = true;
        return NULL;
    }
    char* name = strdup (parser->lexer.token.text);
    if (name == NULL) {
        parser->failed = true;
        return NULL;
    }
    if (IsReservedName (name, false)) {
        SetError (&parser->error, line, "%s 是保留名称", name);
        free (name);
        parser->failed = true;
        return NULL;
    }
    ParserNext (parser);

    if (!ParserConsume (parser, TOK_LPAREN, "函数名后缺少左括号 (")) {
        free (name);
        return NULL;
    }

    char **params = NULL;
    int paramCount = 0;
    int paramCapacity = 0;
    bool paramsOk = true;
    if (!ParserCheck (parser, TOK_RPAREN)) {
        while (!parser->failed) {
            if (!ParserCheck (parser, TOK_IDENT)) {
                SetError (&parser->error, parser->lexer.token.line, "参数名必须是标识符");
                paramsOk = false;
                break;
            }
            const char *param = parser->lexer.token.text;
            if (IsReservedName (param, true)) {
                SetError (&parser->error, parser->lexer.token.line, "%s 是保留名称", param);
                paramsOk = false;
                break;
            }
            for (int i = 0; i < paramCount; i++) {
                if (strcmp (params[i], param) == 0) {
                    SetError (&parser->error, parser->lexer.token.line,
                              "参数 %s 重复", param);
                    paramsOk = false;
                    break;
                }
            }
            if (!paramsOk) break;
            if (paramCount == paramCapacity) {
                int capacity = (paramCapacity == 0) ? 4 : paramCapacity * 2;
                char **items = realloc (params, (size_t)capacity * sizeof (char *));
                if (items == NULL) {
                    SetError (&parser->error, parser->lexer.token.line, "内存不足");
                    paramsOk = false;
                    break;
                }
                params = items;
                paramCapacity = capacity;
            }
            params[paramCount] = strdup (param);
            if (params[paramCount] == NULL) {
                SetError (&parser->error, parser->lexer.token.line, "内存不足");
                paramsOk = false;
                break;
            }
            paramCount++;
            ParserNext (parser);
            if (ParserMatch (parser, TOK_COMMA)) continue;
            break;
        }
    }
    if (paramsOk && !ParserConsume (parser, TOK_RPAREN, "缺少右括号 )")) paramsOk = false;
    if (!paramsOk) {
        parser->failed = true;
        for (int i = 0; i < paramCount; i++) free (params[i]);
        free (params);
        free (name);
        return NULL;
    }
    if (!ParserCheck (parser, TOK_NEWLINE) &&
        !ParserCheck (parser, TOK_EOF)) {
        SetError (&parser->error, parser->lexer.token.line, "函数声明后只能换行");
        parser->failed = true;
        for (int i = 0; i < paramCount; i++) free (params[i]);
        free (params);
        free (name);
        return NULL;
    }

    free (parser->lexer.token.text);
    parser->lexer.token.text = NULL;

    int headerIndent = parser->lexer.indentStack[parser->lexer.indentCount - 1];
    TextBuffer body = {0};
    bool started = false;
    int bodyIndent = 0;

    while (parser->lexer.pos < parser->lexer.length) {
        size_t lineStart = parser->lexer.pos;
        size_t p = lineStart;
        int width = 0;
        while (p < parser->lexer.length) {
            char c = parser->lexer.source[p];
            if (c == '\t') {
                width += SCRIPT_TAB_WIDTH;
                p++;
            } else if (c == ' ') {
                width++;
                p++;
            } else
                break;
        }
        bool blank = (p >= parser->lexer.length ||
                      parser->lexer.source[p] == '\n' ||
                      parser->lexer.source[p] == '\r');
        if (blank) {
            if (started && !TextBufferAppendChar (&body, '\n')) {
                SetError (&parser->error, line, "内存不足");
                parser->failed = true;
                break;
            }
            while (parser->lexer.pos < p) LexerAdvance (&parser->lexer);
            if (parser->lexer.pos < parser->lexer.length && LexerPeek (&parser->lexer, 0) == '\r')
                LexerAdvance (&parser->lexer);
            if (parser->lexer.pos < parser->lexer.length && LexerPeek (&parser->lexer, 0) == '\n')
                LexerAdvance (&parser->lexer);
            continue;
        }

        if (width <= headerIndent) break;
        if (!started) {
            started = true;
            bodyIndent = width;
        }
        if (width < bodyIndent) break;

        size_t contentStart = StripIndentWidth (parser->lexer.source, lineStart,
                                                p, bodyIndent);
        size_t lineEnd = p;
        while (lineEnd < parser->lexer.length &&
               parser->lexer.source[lineEnd] != '\n' &&
               parser->lexer.source[lineEnd] != '\r')
            lineEnd++;
        if (lineEnd > contentStart &&
            !TextBufferAppendN (&body, parser->lexer.source + contentStart,
                                lineEnd - contentStart)) {
            SetError (&parser->error, line, "内存不足");
            parser->failed = true;
            break;
        }
        if (!TextBufferAppendChar (&body, '\n')) {
            SetError (&parser->error, line, "内存不足");
            parser->failed = true;
            break;
        }
        while (parser->lexer.pos < lineEnd) LexerAdvance (&parser->lexer);
        if (parser->lexer.pos < parser->lexer.length && LexerPeek (&parser->lexer, 0) == '\r')
            LexerAdvance (&parser->lexer);
        if (parser->lexer.pos < parser->lexer.length && LexerPeek (&parser->lexer, 0) == '\n')
            LexerAdvance (&parser->lexer);
    }

    if (parser->failed) {
        TextBufferFree (&body);
        FreeParamList (params, paramCount);
        free (name);
        return NULL;
    }
    if (!TextBufferReserve (&body, 0)) {
        SetError (&parser->error, line, "内存不足");
        parser->failed = true;
        FreeParamList (params, paramCount);
        free (name);
        return NULL;
    }
    if (!started) body.data[0] = '\0';

    parser->lexer.lineStart = true;
    ParserNext (parser);
    if (parser->failed) {
        TextBufferFree (&body);
        FreeParamList (params, paramCount);
        free (name);
        return NULL;
    }

    Stmt* stmt = StmtNew (STMT_FUNC, line);
    if (stmt == NULL) {
        TextBufferFree (&body);
        FreeParamList (params, paramCount);
        free (name);
        parser->failed = true;
        return NULL;
    }
    stmt->as.func.name = name;
    stmt->as.func.params = params;
    stmt->as.func.paramCount = paramCount;
    stmt->as.func.paramCapacity = paramCapacity;
    stmt->as.func.body = body.data;
    return stmt;
}

static void ExprFree (Expr* expr) {
    if (expr == NULL) return;
    switch (expr->type) {
        case EXPR_LITERAL:
            json_object_put (expr->as.literal);
            break;
        case EXPR_VARIABLE:
            free (expr->as.name);
            break;
        case EXPR_UNARY:
            ExprFree (expr->as.unary.operand);
            break;
        case EXPR_BINARY:
            ExprFree (expr->as.binary.left);
            ExprFree (expr->as.binary.right);
            break;
        case EXPR_CALL:
            free (expr->as.call.name);
            ExprFree (expr->as.call.callee);
            ExprFree (expr->as.call.selfExpr);
            for (int i = 0; i < expr->as.call.count; i++) ExprFree (expr->as.call.args[i]);
            free (expr->as.call.args);
            break;
        case EXPR_ARRAY:
            for (int i = 0; i < expr->as.array.count; i++) ExprFree (expr->as.array.items[i]);
            free (expr->as.array.items);
            break;
        case EXPR_OBJECT:
            for (int i = 0; i < expr->as.object.count; i++) {
                free (expr->as.object.keys[i]);
                ExprFree (expr->as.object.values[i]);
            }
            free (expr->as.object.keys);
            free (expr->as.object.values);
            break;
        case EXPR_MEMBER:
            ExprFree (expr->as.member.base);
            ExprFree (expr->as.member.key);
            break;
    }
    free (expr);
}

static void StmtFree (Stmt* stmt) {
    if (stmt == NULL) return;
    switch (stmt->type) {
        case STMT_VAR:
            free (stmt->as.var.name);
            ExprFree (stmt->as.var.initializer);
            break;
        case STMT_UNVAR:
            free (stmt->as.unvarName);
            break;
        case STMT_ASSIGN:
            ExprFree (stmt->as.assign.target);
            ExprFree (stmt->as.assign.value);
            break;
        case STMT_EXPR:
            ExprFree (stmt->as.expr);
            break;
        case STMT_RETURN:
            ExprFree (stmt->as.ret);
            break;
        case STMT_FUNC:
            free (stmt->as.func.name);
            FreeParamList (stmt->as.func.params, stmt->as.func.paramCount);
            free (stmt->as.func.body);
            break;
        case STMT_IF:
            ExprFree (stmt->as.ifStmt.condition);
            StmtBlockFree (&stmt->as.ifStmt.thenBlock);
            StmtBlockFree (&stmt->as.ifStmt.elseBlock);
            break;
        case STMT_WHILE:
            ExprFree (stmt->as.whileStmt.condition);
            StmtBlockFree (&stmt->as.whileStmt.body);
            break;
        case STMT_FOR:
            free (stmt->as.forStmt.key);
            free (stmt->as.forStmt.value);
            ExprFree (stmt->as.forStmt.iterable);
            StmtBlockFree (&stmt->as.forStmt.body);
            break;
    }
    free (stmt);
}

struct Env {
    StrMap *vars;
    Env* parent;
    bool remove;
};

static void EnvInit (Env* env, Env* parent) {
    memset (env, 0, sizeof (*env));
    env->parent = parent;
}

static void EnvFreeVars (StrMap *vars) {
    int count = StrMapCount (vars);
    for (int i = 0; i < count; i++) {
        void *value = NULL;
        if (StrMapAt (vars, i, NULL, &value) && value != NULL)
            json_object_put (value);
    }
    StrMapDestroy (vars);
}

static void EnvClearVars (Env* env) {
    env->remove = false;
    if (env->vars == NULL) return;
    EnvFreeVars (env->vars);
    env->vars = NULL;
}

static void EnvFree (Env* env) {
    EnvClearVars (env);
    memset (env, 0, sizeof (*env));
}

static void EnvDataDestroy (void* data) {
    Env* env = data;
    EnvFree (env);
    free (env);
}

static void **EnvFindLocal (Env *env, const char *name) {
    if (env->vars == NULL) return NULL;
    return StrMapSlot (env->vars, name);
}

static bool EnvGet (Env *env, const char *name, struct json_object **value) {
    for (Env *current = env; current != NULL; current = current->parent) {
        if (current->vars == NULL) continue;
        void *stored = NULL;
        if (StrMapGet (current->vars, name, &stored)) {
            *value = stored;
            return true;
        }
    }
    return false;
}

static void **EnvFindVisible (Env *env, const char *name, Env **owner) {
    for (Env *current = env; current != NULL; current = current->parent) {
        if (current->vars == NULL) continue;
        void **slot = StrMapSlot (current->vars, name);
        if (slot != NULL) {
            if (owner != NULL) *owner = current;
            return slot;
        }
    }
    return NULL;
}

static bool EnvRemoveVisible (Env *env, const char *name) {
    Env *owner = NULL;
    void **slot = EnvFindVisible (env, name, &owner);
    if (slot == NULL) return false;
    json_object_put (*slot);
    return StrMapRemove (owner->vars, name);
}

static void **EnvSlot (Env *env, const char *name) {
    return EnvFindVisible (env, name, NULL);
}

static bool EnvDeclareLocal (Env *env, const char *name, struct json_object *value,
                             TextBuffer *error, int line) {
    if (EnvFindLocal (env, name) != NULL) {
        SetError (error, line, "变量 %s 已定义", name);
        return false;
    }
    if (env->vars == NULL) {
        env->vars = StrMapCreate ();
        if (env->vars == NULL) {
            SetError (error, line, "内存不足");
            return false;
        }
    }
    if (!StrMapAdd (env->vars, name, value)) {
        SetError (error, line, "内存不足");
        return false;
    }
    return true;
}

static Env* EnvSelf (ScriptEnv* global, u64 self, bool create) {
    if (self == 0) return global;

    Env* env = EntityGetData (self);
    if (env != NULL || !create) return env;
    if (EntityFind (self) == NULL) return NULL;

    env = calloc (1, sizeof (Env));
    if (env == NULL) return NULL;
    EnvInit (env, global);
    if (!EntitySetData (self, env, EnvDataDestroy)) {
        free (env);
        return NULL;
    }
    return env;
}

ScriptEnv* ScriptEnvCreate (void) {
    Env* env = calloc (1, sizeof (Env));
    if (env != NULL) EnvInit (env, NULL);
    return env;
}

void ScriptEnvDestroy (ScriptEnv* env) {
    EnvFree (env);
    free (env);
}

#define SCRIPT_JSON_MAX_DEPTH 256

static bool JsonValueHasCycle (struct json_object *value,
                               struct json_object **stack, int depth) {
    if (value == NULL) return false;
    if (depth >= SCRIPT_JSON_MAX_DEPTH) return true;

    if (json_object_is_type (value, json_type_array) ||
        json_object_is_type (value, json_type_object)) {
        for (int i = 0; i < depth; i++) {
            if (stack[i] == value) return true;
        }
        stack[depth] = value;
    }

    if (json_object_is_type (value, json_type_array)) {
        size_t count = json_object_array_length (value);
        for (size_t i = 0; i < count; i++) {
            if (JsonValueHasCycle (json_object_array_get_idx (value, i),
                                   stack, depth + 1))
                return true;
        }
    } else if (json_object_is_type (value, json_type_object)) {
        json_object_object_foreach (value, key, child) {
            (void)key;
            if (JsonValueHasCycle (child, stack, depth + 1)) return true;
        }
    }
    return false;
}

static char *ScriptEnvLocalToJson (ScriptEnv* env) {
    struct json_object *object = json_object_new_object ();
    if (object == NULL) return NULL;

    int count = env->vars != NULL ? StrMapCount (env->vars) : 0;
    struct json_object *stack[SCRIPT_JSON_MAX_DEPTH] = {0};
    for (int i = 0; i < count; i++) {
        const char *name = NULL;
        void *value = NULL;
        if (!StrMapAt (env->vars, i, &name, &value)) {
            json_object_put (object);
            return NULL;
        }
        if (strcmp (name, "remove") == 0) continue;
        if (JsonValueHasCycle (value, stack, 0)) {
            json_object_put (object);
            return NULL;
        }
        json_object_object_add (object, name,
                                value != NULL ? json_object_get (value) : NULL);
    }

    const char *text = json_object_to_json_string_ext (object, JSON_C_TO_STRING_PLAIN);
    char *result = text != NULL ? strdup (text) : NULL;
    json_object_put (object);
    return result;
}

char *ScriptEnvJson (ScriptEnv *globalEnv, u64 id) {
    Env *env = id == 0 ? globalEnv : EntityGetData (id);
    return env != NULL ? ScriptEnvLocalToJson (env) : NULL;
}

bool ScriptEnvMergeJson (ScriptEnv *env, const char *json, TextBuffer *conflicts) {
    size_t length = strlen (json);
    struct json_tokener *tokener = json_tokener_new_ex (SCRIPT_JSON_MAX_DEPTH);
    if (tokener == NULL || length > INT_MAX) {
        if (tokener != NULL) json_tokener_free (tokener);
        return false;
    }
    struct json_object *object = json_tokener_parse_ex (tokener, json, (int)length);
    bool parsed = object != NULL &&
                  json_tokener_get_error (tokener) == json_tokener_success;
    json_tokener_free (tokener);
    if (!parsed || !json_object_is_type (object, json_type_object)) {
        json_object_put (object);
        return false;
    }

    if (env->vars == NULL) env->vars = StrMapCreate ();
    if (env->vars == NULL) {
        json_object_put (object);
        return false;
    }

    bool ok = true;
    json_object_object_foreach (object, key, value) {
        if (strcmp (key, "remove") == 0) continue;
        if (EnvFindLocal (env, key) != NULL) {
            if (conflicts != NULL) {
                if (!TextBufferAppend (conflicts, key) ||
                    !TextBufferAppendChar (conflicts, '\n')) {
                    ok = false;
                    break;
                }
            }
            continue;
        }
        if (!StrMapAdd (env->vars, key,
                        value != NULL ? json_object_get (value) : NULL)) {
            ok = false;
            break;
        }
    }
    json_object_put (object);
    return ok;
}

bool ScriptEnvLoadJson (ScriptEnv *globalEnv, u64 id, const char *json) {
    Env *env = id == 0 ? globalEnv : EnvSelf (globalEnv, id, true);
    if (env == NULL) return false;

    size_t length = strlen (json);
    struct json_tokener *tokener = json_tokener_new_ex (SCRIPT_JSON_MAX_DEPTH);
    if (tokener == NULL || length > INT_MAX) {
        if (tokener != NULL) json_tokener_free (tokener);
        return false;
    }
    struct json_object *object = json_tokener_parse_ex (tokener, json, (int)length);
    bool parsed = object != NULL &&
                  json_tokener_get_error (tokener) == json_tokener_success;
    json_tokener_free (tokener);
    if (!parsed || !json_object_is_type (object, json_type_object)) {
        json_object_put (object);
        return false;
    }

    StrMap *vars = StrMapCreate ();
    if (vars == NULL) {
        json_object_put (object);
        return false;
    }

    bool ok = true;
    json_object_object_foreach (object, key, value) {
        if (strcmp (key, "remove") == 0) continue;
        if (!StrMapAdd (vars, key, value != NULL ? json_object_get (value) : NULL)) {
            ok = false;
            break;
        }
    }
    json_object_put (object);
    if (!ok) {
        EnvFreeVars (vars);
        return false;
    }

    EnvClearVars (env);
    env->vars = vars;
    return true;
}

static void ProgramFree (Program* program) {
    for (int i = 0; i < program->statementCount; i++) StmtFree (program->statements[i]);
    free (program->statements);
    memset (program, 0, sizeof (*program));
}

typedef struct {
    ScriptEnv* global;
    u64 self;
    u64 caller;
    TextBuffer error;
    bool failed;
    int depth;
    u64 operations;
    bool inFunction;
    bool localTopLevel;
    Env* selfEnv;
} Runtime;

typedef struct {
    bool hasReturn;
    struct json_object* value;
} ExecResult;

static void RuntimeFail (Runtime* runtime, int line, const char* format, ...) {
    if (runtime->failed) return;
    va_list args;
    va_start (args, format);
    SetErrorV (&runtime->error, line, format, args);
    va_end (args);
    runtime->failed = true;
}

static bool RuntimeTick (Runtime* runtime, int line) {
    runtime->operations++;
    if (runtime->operations >= SCRIPT_MAX_OPERATIONS) {
        RuntimeFail (runtime, line, "脚本执行次数超过限制");
        return false;
    }
    return true;
}

static bool MessageAppend (Runtime* runtime, int line, TextBuffer* message,
                           const char* text, size_t length) {
    if (message->length + length > SCRIPT_MAX_MESSAGE) {
        RuntimeFail (runtime, line, "消息超过 256 字节限制");
        return false;
    }
    if (!TextBufferAppendN (message, text, length)) {
        RuntimeFail (runtime, line, "内存不足");
        return false;
    }
    return true;
}

static bool ValueIsNumeric (const struct json_object* value) {
    return json_object_is_type (value, json_type_int) || json_object_is_type (value, json_type_double);
}

static double ValueNumeric (const struct json_object* value) {
    return json_object_is_type (value, json_type_int) ? (double)json_object_get_uint64 (value)
                                                      : json_object_get_double (value);
}

static bool ValueTruthy (const struct json_object* value) {
    if (value == NULL) return false;
    if (json_object_is_type (value, json_type_boolean)) return json_object_get_boolean (value) != 0;
    if (json_object_is_type (value, json_type_int)) return json_object_get_uint64 (value) != 0;
    if (json_object_is_type (value, json_type_double)) return json_object_get_double (value) != 0.0;
    if (json_object_is_type (value, json_type_string)) {
        const char* text = json_object_get_string ((struct json_object*)value);
        return text != NULL && text[0] != '\0';
    }
    if (json_object_is_type (value, json_type_array)) return json_object_array_length (value) > 0;
    if (json_object_is_type (value, json_type_object)) return json_object_object_length (value) > 0;
    return false;
}

static struct json_object *EvalExpression (Runtime *runtime, Env *env, Expr *expr);
static bool ExecStatement (Runtime* runtime, Env* env, Stmt* stmt, ExecResult* exec);

static struct json_object *RandCall (Runtime *runtime, int line,
                                    struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "rand 需要 1 个参数");
        return NULL;
    }
    struct json_object *value = args[0];
    u64 limit = 0;
    bool valid = false;
    if (json_object_is_type (value, json_type_int)) {
        u64 integer = json_object_get_uint64 (value);
        valid = (integer >= 1 && integer <= INT_MAX);
        if (valid) limit = integer;
    }
    if (!valid) {
        RuntimeFail (runtime, line, "rand 参数必须是 1 到 INT_MAX 的整数");
        return NULL;
    }
    struct json_object *result = json_object_new_uint64 (RandomBounded (limit));
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static struct json_object *IntCall (Runtime *runtime, int line,
                                    struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "int 需要 1 个参数");
        return NULL;
    }
    struct json_object *value = args[0];
    if (json_object_is_type (value, json_type_int))
        return json_object_get (value);
    if (json_object_is_type (value, json_type_double)) {
        double number = json_object_get_double (value);
        if (!isfinite (number) || number < 0.0 || number >= 18446744073709551616.0) {
            RuntimeFail (runtime, line, "int 参数必须是 0 到 u64 范围内的有限数字");
            return NULL;
        }
        struct json_object *result = json_object_new_uint64 ((u64)trunc (number));
        if (result == NULL) RuntimeFail (runtime, line, "内存不足");
        return result;
    }
    RuntimeFail (runtime, line, "int 参数必须是数字");
    return NULL;
}

static struct json_object *ObjectCall (Runtime *runtime, int line,
                                       struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "object 需要 1 个参数");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_string)) {
        RuntimeFail (runtime, line, "object 参数必须是字符串");
        return NULL;
    }

    const char *name = json_object_get_string (args[0]);
    Object *object = ObjectFind (name);
    if (object == NULL) {
        RuntimeFail (runtime, line, "物件 %s 不存在", name);
        return NULL;
    }
    struct json_object *result = json_object_new_uint64 (object->id);
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static struct json_object *UserCall (Runtime *runtime, int line,
                                     struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "user 需要 1 个参数");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_string)) {
        RuntimeFail (runtime, line, "user 参数必须是字符串");
        return NULL;
    }

    const char *name = json_object_get_string (args[0]);
    u64 id = 0;
    IdGroup ids = UserIds ();
    for (int i = 0; i < ids.count; i++) {
        User *user = (User *)EntityFind (ids.ids[i]);
        if (user != NULL && strcmp (user->name, name) == 0) {
            id = user->id;
            break;
        }
    }
    IdGroupUnload (&ids);

    if (id == 0) {
        RuntimeFail (runtime, line, "用户 %s 不存在", name);
        return NULL;
    }
    struct json_object *result = json_object_new_uint64 (id);
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static const char *SelfTypeName (u64 self) {
    if (self == 0) return "global";
    switch (EntityKindOf (self)) {
    case ENTITY_KIND_OBJECT: return "object";
    case ENTITY_KIND_USER: return "user";
    case ENTITY_KIND_ASSET: return "asset";
    default: return NULL;
    }
}

static struct json_object *FlipCall (Runtime *runtime, int line, u64 self,
                                     struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "flip 需要 1 个参数");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_int)) {
        RuntimeFail (runtime, line, "flip 参数必须是整数");
        return NULL;
    }

    u64 target = self != 0 ? self : runtime->self;
    Object *object = EntityKindOf (target) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (target) : NULL;
    if (object == NULL) {
        RuntimeFail (runtime, line, "self 不是物件");
        return NULL;
    }

    u64 index = json_object_get_uint64 (args[0]);
    if (index >= (u64)object->assets.count) {
        RuntimeFail (runtime, line, "flip 下标 %" PRIu64 " 越界", index);
        return NULL;
    }

    object->assetIndex = (int)index;
    NetServerBroadcastObjectState (object);
    return NULL;
}

static bool CoordinateValue (struct json_object *value, double *coordinate) {
    if (!json_object_is_type (value, json_type_int) &&
        !json_object_is_type (value, json_type_double))
        return false;
    *coordinate = json_object_get_double (value);
    return isfinite (*coordinate);
}

static struct json_object *MoveCall (Runtime *runtime, int line, u64 self,
                                     struct json_object **args, int argCount) {
    double x, y;
    if (argCount == 1 && args != NULL &&
        json_object_is_type (args[0], json_type_object)) {
        struct json_object *xValue = NULL;
        struct json_object *yValue = NULL;
        if (!json_object_object_get_ex (args[0], "x", &xValue) ||
            !json_object_object_get_ex (args[0], "y", &yValue)) {
            RuntimeFail (runtime, line, "move 参数必须包含 x 和 y");
            return NULL;
        }
        if (!CoordinateValue (xValue, &x) || !CoordinateValue (yValue, &y)) {
            RuntimeFail (runtime, line, "move 的 x 和 y 必须是有限数字");
            return NULL;
        }
    } else if (argCount == 2 && args != NULL) {
        if (!CoordinateValue (args[0], &x) || !CoordinateValue (args[1], &y)) {
            RuntimeFail (runtime, line, "move 参数必须是有限数字");
            return NULL;
        }
    } else {
        RuntimeFail (runtime, line,
                     "move 需要 1 个对象参数或 2 个数字参数");
        return NULL;
    }

    u64 target = self != 0 ? self : runtime->self;
    Object *object = EntityKindOf (target) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (target) : NULL;
    if (object == NULL) {
        RuntimeFail (runtime, line, "self 不是物件");
        return NULL;
    }
    ObjectMove (object, (Vector2){(float)x, (float)y});
    NetServerBroadcastObjectState (object);
    return NULL;
}

static struct json_object *PosCall (Runtime *runtime, int line, u64 self,
                                    struct json_object **args, int argCount) {
    (void)args;
    if (argCount != 0) {
        RuntimeFail (runtime, line, "pos 不需要参数");
        return NULL;
    }

    u64 target = self != 0 ? self : runtime->self;
    Object *object = EntityKindOf (target) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (target) : NULL;
    if (object == NULL) {
        RuntimeFail (runtime, line, "self 不是物件");
        return NULL;
    }

    struct json_object *result = json_object_new_object ();
    struct json_object *x = json_object_new_double (object->pos.x);
    struct json_object *y = json_object_new_double (object->pos.y);
    if (result == NULL || x == NULL || y == NULL) {
        json_object_put (result);
        json_object_put (x);
        json_object_put (y);
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    if (json_object_object_add (result, "x", x) != 0) {
        json_object_put (x);
        json_object_put (y);
        json_object_put (result);
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    if (json_object_object_add (result, "y", y) != 0) {
        json_object_put (y);
        json_object_put (result);
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    return result;
}

static u64 CurrentUserTarget (Runtime *runtime, u64 self) {
    if (self != 0 && EntityKindOf (self) == ENTITY_KIND_USER) return self;
    if (runtime->self != 0 && EntityKindOf (runtime->self) == ENTITY_KIND_USER)
        return runtime->self;
    if (runtime->caller != 0 && EntityKindOf (runtime->caller) == ENTITY_KIND_USER)
        return runtime->caller;
    return 0;
}

static struct json_object *ColorCall (Runtime *runtime, int line, u64 self,
                                    struct json_object **args, int argCount) {
    if (argCount != 3) {
        RuntimeFail (runtime, line, "color 需要 3 个参数");
        return NULL;
    }
    if (args == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    unsigned char rgb[3];
    for (int i = 0; i < 3; i++) {
        if (!json_object_is_type (args[i], json_type_int)) {
            RuntimeFail (runtime, line, "color 参数必须是 0..255 的整数");
            return NULL;
        }
        int64_t value = json_object_get_int64 (args[i]);
        if (value < 0 || value > 255) {
            RuntimeFail (runtime, line, "color 参数必须是 0..255 的整数");
            return NULL;
        }
        rgb[i] = (unsigned char)value;
    }

    u64 target = CurrentUserTarget (runtime, self);
    if (target == 0) {
        RuntimeFail (runtime, line, "color 只能在用户身份下调用");
        return NULL;
    }

    uint32_t color = ((uint32_t)rgb[0] << 24) |
                     ((uint32_t)rgb[1] << 16) |
                     ((uint32_t)rgb[2] << 8) | 0xFFu;
    NetServerSetUserColor (target, color);
    return NULL;
}

static struct json_object *ScriptPermissionCall (Runtime *runtime, int line,
                                              u64 self,
                                              struct json_object **args,
                                              int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "script 需要 1 个参数");
        return NULL;
    }
    if (args == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_boolean)) {
        RuntimeFail (runtime, line, "script 参数必须是 bool");
        return NULL;
    }
    u64 target = CurrentUserTarget (runtime, self);
    if (target == 0) {
        RuntimeFail (runtime, line, "script 只能在用户身份下调用");
        return NULL;
    }
    NetServerSetUserCanScript (target, json_object_get_boolean (args[0]));
    return NULL;
}

static struct json_object *LockedCall (Runtime *runtime, int line,
                                      struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "locked 需要 1 个参数");
        return NULL;
    }
    if (args == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_int)) {
        RuntimeFail (runtime, line, "locked 参数必须是物件 ID");
        return NULL;
    }
    u64 id = json_object_get_uint64 (args[0]);
    Object *object = EntityKindOf (id) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (id) : NULL;
    if (object == NULL) {
        RuntimeFail (runtime, line, "物件 %" PRIu64 " 不存在", id);
        return NULL;
    }
    struct json_object *result = json_object_new_boolean (object->lockOwnerId != 0);
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static struct json_object *VisibilityCall (Runtime *runtime, int line, u64 self,
                                          bool hidden, const char *name,
                                          struct json_object **args, int argCount) {
    if (argCount > 1) {
        RuntimeFail (runtime, line, "%s 需要 0 或 1 个参数", name);
        return NULL;
    }
    u64 target = self != 0 ? self : runtime->self;
    Object *object = EntityKindOf (target) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (target) : NULL;
    if (object == NULL) {
        RuntimeFail (runtime, line, "self 不是物件");
        return NULL;
    }

    if (argCount == 0) {
        ObjectSetHidden (object, hidden);
    } else {
        if (args == NULL) {
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
        if (!json_object_is_type (args[0], json_type_int)) {
            RuntimeFail (runtime, line, "%s 参数必须是用户 ID", name);
            return NULL;
        }
        u64 userId = json_object_get_uint64 (args[0]);
        if (userId == 0 || EntityKindOf (userId) != ENTITY_KIND_USER) {
            RuntimeFail (runtime, line, "用户 %" PRIu64 " 不存在", userId);
            return NULL;
        }
        if (!ObjectSetHiddenForUser (object, userId, hidden)) {
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
    }
    NetServerBroadcastObjectState (object);
    return NULL;
}

static bool CopyEnvironmentLocal (Env *target, Env *source,
                                  Runtime *runtime, int line) {
    if (source->vars == NULL) return true;
    int count = StrMapCount (source->vars);
    if (count == 0) return true;
    if (target->vars == NULL) {
        target->vars = StrMapCreate ();
        if (target->vars == NULL) {
            RuntimeFail (runtime, line, "内存不足");
            return false;
        }
    }

    for (int i = 0; i < count; i++) {
        const char *name = NULL;
        void *value = NULL;
        if (!StrMapAt (source->vars, i, &name, &value)) {
            RuntimeFail (runtime, line, "内存不足");
            return false;
        }
        if (strcmp (name, "remove") == 0) continue;

        struct json_object *copy = NULL;
        if (value != NULL) {
            struct json_object *stack[SCRIPT_JSON_MAX_DEPTH] = {0};
            if (JsonValueHasCycle (value, stack, 0)) {
                RuntimeFail (runtime, line, "环境包含循环引用");
                return false;
            }
            if (json_object_deep_copy (value, &copy, NULL) != 0) {
                RuntimeFail (runtime, line, "内存不足");
                return false;
            }
        }
        if (!StrMapAdd (target->vars, name, copy)) {
            json_object_put (copy);
            RuntimeFail (runtime, line, "内存不足");
            return false;
        }
    }
    return true;
}

static bool MakeForkName (const char *requested, char *name, size_t size) {
    int written = snprintf (name, size, "%s", requested);
    if (written < 0 || (size_t)written >= size) return false;
    if (ObjectFind (name) == NULL) return true;

    for (int suffix = 1; suffix < 1000000; suffix++) {
        written = snprintf (name, size, "%s#%d", requested, suffix);
        if (written < 0 || (size_t)written >= size) return false;
        if (ObjectFind (name) == NULL) return true;
    }
    return false;
}

static struct json_object *ForkCall (Runtime *runtime, int line, u64 self,
                                     struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "fork 需要 1 个参数");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_string)) {
        RuntimeFail (runtime, line, "fork 参数必须是字符串");
        return NULL;
    }

    const char *requested = json_object_get_string (args[0]);
    if (requested[0] == '\0') {
        RuntimeFail (runtime, line, "fork 名称不能为空");
        return NULL;
    }
    char name[256];
    if (!MakeForkName (requested, name, sizeof (name))) {
        RuntimeFail (runtime, line, "无法生成唯一物件名称");
        return NULL;
    }

    u64 sourceId = self != 0 ? self : runtime->self;
    Object *source = EntityKindOf (sourceId) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (sourceId) : NULL;
    if (source == NULL) {
        RuntimeFail (runtime, line, "self 不是物件");
        return NULL;
    }

    u64 newId = NetServerAllocateEntityId ();
    Object *target = ObjectAdd (name, source->pos, source->layer,
                                source->timestamp, newId);
    if (target == NULL) {
        RuntimeFail (runtime, line, "新建物件失败");
        return NULL;
    }
    if (!IdGroupCopy (ObjectAssets (target), source->assets.ids,
                      source->assets.count)) {
        ObjectDelete (target);
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    target->assetIndex = source->assetIndex;

    Env *sourceEnv = EnvSelf (runtime->global, sourceId, false);
    if (sourceEnv != NULL) {
        Env *targetEnv = EnvSelf (runtime->global, newId, true);
        if (targetEnv == NULL ||
            !CopyEnvironmentLocal (targetEnv, sourceEnv, runtime, line)) {
            ObjectDelete (target);
            if (runtime->error.length == 0)
                RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
    }

    int menuCount = MenuCount (sourceId);
    for (int i = 0; i < menuCount; i++) {
        const char *menuName = MenuName (sourceId, i);
        const char *menuFunc = MenuFunc (sourceId, i);
        if (menuName == NULL || !MenuAdd (newId, menuName, menuFunc)) {
            ObjectDelete (target);
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
    }

    ObjectTouch (target);
    struct json_object *result = json_object_new_uint64 (newId);
    if (result == NULL) {
        ObjectDelete (target);
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    NetServerBroadcastObjectState (target);
    NetServerBroadcastMenu (newId);
    return result;
}

static struct json_object *UntouchCall (Runtime *runtime, int line, u64 self,
                                       struct json_object **args, int argCount) {
    (void)args;
    if (argCount != 0) {
        RuntimeFail (runtime, line, "untouch 不需要参数");
        return NULL;
    }
    u64 target = self != 0 ? self : runtime->self;
    Object *object = EntityKindOf (target) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (target) : NULL;
    if (object == NULL) {
        RuntimeFail (runtime, line, "self 不是物件");
        return NULL;
    }
    NetServerReleaseObjectLock (object->id);
    return NULL;
}

static struct json_object *SizeCall (Runtime *runtime, int line,
                                     struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "size 需要 1 个参数");
        return NULL;
    }

    u64 size;
    if (json_object_is_type (args[0], json_type_array))
        size = (u64)json_object_array_length (args[0]);
    else if (json_object_is_type (args[0], json_type_object))
        size = (u64)json_object_object_length (args[0]);
    else {
        RuntimeFail (runtime, line, "size 参数必须是对象或数组");
        return NULL;
    }

    struct json_object *result = json_object_new_uint64 (size);
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static struct json_object *AssetsCall (Runtime *runtime, int line, u64 self,
                                       struct json_object **args, int argCount) {
    (void)args;
    if (argCount != 0) {
        RuntimeFail (runtime, line, "assets 不需要参数");
        return NULL;
    }

    u64 target = self != 0 ? self : runtime->self;
    Object *object = EntityKindOf (target) == ENTITY_KIND_OBJECT ?
                       (Object *)EntityFind (target) : NULL;
    if (object == NULL) return NULL;

    struct json_object *array = json_object_new_array ();
    if (array == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }

    for (int i = 0; i < object->assets.count; i++) {
        struct json_object *id = json_object_new_uint64 (object->assets.ids[i]);
        if (id == NULL || json_object_array_add (array, id) != 0) {
            json_object_put (id);
            json_object_put (array);
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
    }
    return array;
}

static struct json_object *TypeofCall (Runtime *runtime, int line,
                                       struct json_object **args, int argCount) {
    if (argCount != 1) {
        RuntimeFail (runtime, line, "typeof 需要 1 个参数");
        return NULL;
    }

    const char *name;
    struct json_object *value = args[0];
    if (value == NULL)
        name = "null";
    else if (json_object_is_type (value, json_type_boolean))
        name = "boolean";
    else if (json_object_is_type (value, json_type_int))
        name = "integer";
    else if (json_object_is_type (value, json_type_double))
        name = "double";
    else if (json_object_is_type (value, json_type_string))
        name = "string";
    else if (json_object_is_type (value, json_type_array))
        name = "array";
    else if (json_object_is_type (value, json_type_object))
        name = "object";
    else
        name = "null";

    struct json_object *result = json_object_new_string (name);
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static struct json_object *MenuCall (Runtime *runtime, int line, u64 self,
                                     struct json_object **args, int argCount) {
    if (argCount != 0 && argCount != 2) {
        RuntimeFail (runtime, line, "menu 需要 0 或 2 个参数");
        return NULL;
    }
    if (argCount == 2) {
        if (!json_object_is_type (args[0], json_type_string)) {
            RuntimeFail (runtime, line, "menu 的第一个参数必须是字符串");
            return NULL;
        }
        if (!json_object_is_type (args[1], json_type_string)) {
            RuntimeFail (runtime, line, "menu 的第二个参数必须是字符串");
            return NULL;
        }
    }

    u64 target = self != 0 ? self : runtime->self;
    if (target != 0) {
        EntityKind kind = EntityKindOf (target);
        if (kind != ENTITY_KIND_USER && kind != ENTITY_KIND_OBJECT) {
            RuntimeFail (runtime, line, "self 不是用户或物件");
            return NULL;
        }
    }

    if (argCount == 0) {
        MenuClear (target);
        NetServerBroadcastMenu (target);
        return NULL;
    }

    if (!MenuAdd (target, json_object_get_string (args[0]),
                  json_object_get_string (args[1]))) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    NetServerBroadcastMenu (target);
    return NULL;
}

static struct json_object *MessagefCall (Runtime *runtime, int line, u64 target,
                                         struct json_object **args, int argCount) {
    if (argCount < 1) {
        RuntimeFail (runtime, line, "messagef 至少需要一个格式字符串");
        return NULL;
    }
    if (!json_object_is_type (args[0], json_type_string)) {
        RuntimeFail (runtime, line, "messagef 的第一个参数必须是字符串");
        return NULL;
    }

    const char *format = json_object_get_string (args[0]);
    TextBuffer message = {0};
    bool ok = true;
    int nextArg = 1;
    for (size_t i = 0; ok && format[i] != '\0'; i++) {
        if (format[i] != '%') {
            ok = MessageAppend (runtime, line, &message, &format[i], 1);
            continue;
        }
        i++;
        if (format[i] == '%') {
            ok = MessageAppend (runtime, line, &message, "%", 1);
        } else if (format[i] == 'x') {
            if (nextArg >= argCount) {
                RuntimeFail (runtime, line, "messagef 的 %%x 缺少对应参数");
                ok = false;
                break;
            }
            const char *text = json_object_to_json_string_ext (
                args[nextArg++], JSON_C_TO_STRING_PLAIN);
            if (text == NULL) {
                RuntimeFail (runtime, line, "无法序列化 messagef 参数");
                ok = false;
                break;
            }
            ok = MessageAppend (runtime, line, &message, text, strlen (text));
        } else if (format[i] == '\0') {
            RuntimeFail (runtime, line, "messagef 格式字符串以 %% 结尾");
            ok = false;
        } else {
            RuntimeFail (runtime, line, "messagef 不支持格式 %%%c，只支持 %%x", format[i]);
            ok = false;
        }
    }
    if (!ok) {
        TextBufferFree (&message);
        return NULL;
    }

    if (target != 0 && EntityKindOf (target) != ENTITY_KIND_USER) {
        TextBufferFree (&message);
        RuntimeFail (runtime, line, "用户 %" PRIu64 " 不存在", target);
        return NULL;
    }
    if (target == 0)
        NetServerBroadcastMessage (message.data != NULL ? message.data : "");
    else
        NetServerSendMessage (target, message.data != NULL ? message.data : "");
    TextBufferFree (&message);
    return NULL;
}

static struct json_object *EvalCallUser (Runtime *runtime, int line,
                                         u64 scopeSelf,
                                         struct json_object *value,
                                         struct json_object **args, int argCount) {
    struct json_object *returned = NULL;
    struct json_object *function = NULL;
    char **paramNames = NULL;
    int paramCount = 0;
    char *code = NULL;
    Program body;
    memset (&body, 0, sizeof (body));
    Env *targetEnv = NULL;
    Env local;
    bool localInit = false;
    bool switched = false;
    ExecResult exec = {0};
    u64 oldSelf;
    Env *oldSelfEnv;
    bool oldLocalTop;
    bool oldInFunction;

    if (value == NULL || !json_object_is_type (value, json_type_string)) {
        RuntimeFail (runtime, line, "不是函数");
        goto cleanup;
    }
    function = json_object_get (value);

    const char *bodyText = NULL;
    const char *functionText = json_object_get_string (function);
    const char *paramEnd = strchr (functionText, '\n');
    if (paramEnd == NULL) {
        RuntimeFail (runtime, line, "函数格式错误");
        goto cleanup;
    }
    size_t pos = 0;
    const char *paramText = functionText;
    size_t metaLength = (size_t)(paramEnd - paramText);
    if (metaLength >= 4096) {
        RuntimeFail (runtime, line, "函数参数列表过长");
        goto cleanup;
    }

    int paramCapacity = 0;
    pos = 0;
    while (pos < metaLength) {
        while (pos < metaLength && paramText[pos] == ' ') pos++;
        if (pos >= metaLength) break;
        size_t startPos = pos;
        if (!(isalpha ((unsigned char)paramText[pos]) || paramText[pos] == '_')) {
            RuntimeFail (runtime, line, "函数参数格式错误");
            goto cleanup;
        }
        pos++;
        while (pos < metaLength &&
               (isalnum ((unsigned char)paramText[pos]) || paramText[pos] == '_')) pos++;
        if (pos < metaLength && paramText[pos] != ' ') {
            RuntimeFail (runtime, line, "函数参数格式错误");
            goto cleanup;
        }
        size_t nameLength = pos - startPos;
        if (paramCount == paramCapacity) {
            int next = (paramCapacity == 0) ? 4 : paramCapacity * 2;
            char **items = realloc (paramNames, (size_t)next * sizeof (char *));
            if (items == NULL) {
                RuntimeFail (runtime, line, "内存不足");
                goto cleanup;
            }
            paramNames = items;
            paramCapacity = next;
        }
        paramNames[paramCount] = malloc (nameLength + 1);
        if (paramNames[paramCount] == NULL) {
            RuntimeFail (runtime, line, "内存不足");
            goto cleanup;
        }
        memcpy (paramNames[paramCount], paramText + startPos, nameLength);
        paramNames[paramCount][nameLength] = '\0';
        paramCount++;
    }

    if (argCount != paramCount) {
        RuntimeFail (runtime, line, "函数需要 %d 个参数，实际 %d 个",
                     paramCount, argCount);
        goto cleanup;
    }
    bodyText = paramEnd + 1;

    code = strdup (bodyText);
    if (code == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        goto cleanup;
    }
    TextBuffer parseError = ProgramParse (code, &body);
    free (code);
    code = NULL;
    if (parseError.length > 0) {
        RuntimeFail (runtime, line, "函数解析失败: %s", parseError.data);
        TextBufferFree (&parseError);
        goto cleanup;
    }

    targetEnv = EnvSelf (runtime->global, scopeSelf, true);
    if (targetEnv == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        goto cleanup;
    }

    oldSelf = runtime->self;
    oldSelfEnv = runtime->selfEnv;
    oldLocalTop = runtime->localTopLevel;
    oldInFunction = runtime->inFunction;
    runtime->self = scopeSelf;
    runtime->selfEnv = targetEnv;
    runtime->localTopLevel = true;
    runtime->inFunction = true;
    runtime->depth++;
    switched = true;

    EnvInit (&local, targetEnv);
    localInit = true;
    for (int i = 0; i < paramCount; i++) {
        if (!EnvDeclareLocal (&local, paramNames[i], args[i], &runtime->error, line)) {
            runtime->failed = true;
            goto cleanup;
        }
        args[i] = NULL;
    }

    for (int i = 0; i < body.statementCount && !runtime->failed && !exec.hasReturn; i++)
        ExecStatement (runtime, &local, body.statements[i], &exec);
    if (runtime->failed) goto cleanup;

    returned = exec.hasReturn ? exec.value : NULL;
    exec.value = NULL;

cleanup:
    if (switched) {
        runtime->depth--;
        runtime->self = oldSelf;
        runtime->selfEnv = oldSelfEnv;
        runtime->localTopLevel = oldLocalTop;
        runtime->inFunction = oldInFunction;
    }
    if (localInit) EnvFree (&local);
    FreeParamList (paramNames, paramCount);
    free (code);
    ProgramFree (&body);
    if (runtime->failed) {
        json_object_put (exec.value);
        json_object_put (returned);
        returned = NULL;
    }
    json_object_put (function);
    return returned;
}

static void FreeArgs (struct json_object **args, int count) {
    if (args == NULL) return;
    for (int i = 0; i < count; i++) json_object_put (args[i]);
    free (args);
}

static struct json_object *CallFunction (Runtime *runtime, Env *env, int line,
                                        const char *name, u64 self, bool hasSelf,
                                        struct json_object **args, int argCount) {
    struct json_object *result = NULL;
    if (strcmp (name, "messagef") == 0) {
        result = MessagefCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "object") == 0) {
        result = ObjectCall (runtime, line, args, argCount);
    } else if (strcmp (name, "user") == 0) {
        result = UserCall (runtime, line, args, argCount);
    } else if (strcmp (name, "flip") == 0) {
        result = FlipCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "move") == 0) {
        result = MoveCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "pos") == 0) {
        result = PosCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "locked") == 0) {
        result = LockedCall (runtime, line, args, argCount);
    } else if (strcmp (name, "color") == 0) {
        result = ColorCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "script") == 0) {
        result = ScriptPermissionCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "fork") == 0) {
        result = ForkCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "untouch") == 0) {
        result = UntouchCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "hide") == 0) {
        result = VisibilityCall (runtime, line, self, true, "hide",
                                 args, argCount);
    } else if (strcmp (name, "show") == 0) {
        result = VisibilityCall (runtime, line, self, false, "show",
                                 args, argCount);
    } else if (strcmp (name, "size") == 0) {
        result = SizeCall (runtime, line, args, argCount);
    } else if (strcmp (name, "assets") == 0) {
        result = AssetsCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "typeof") == 0) {
        result = TypeofCall (runtime, line, args, argCount);
    } else if (strcmp (name, "menu") == 0) {
        result = MenuCall (runtime, line, self, args, argCount);
    } else if (strcmp (name, "rand") == 0) {
        result = RandCall (runtime, line, args, argCount);
    } else if (strcmp (name, "int") == 0) {
        result = IntCall (runtime, line, args, argCount);
    } else if (runtime->depth >= SCRIPT_MAX_DEPTH) {
        RuntimeFail (runtime, line, "函数调用层级过深（可能存在无限递归）");
    } else {
        u64 scopeSelf = 0;
        void **variable = NULL;
        if (hasSelf) {
            Env *targetEnv = EnvSelf (runtime->global, self, false);
            if (targetEnv != NULL) variable = EnvFindLocal (targetEnv, name);
            if (variable == NULL && self != 0)
                variable = EnvFindLocal (runtime->global, name);
            if (variable != NULL) scopeSelf = self;
        } else {
            Env *owner = NULL;
            variable = EnvFindVisible (env, name, &owner);
            if (variable != NULL)
                scopeSelf = (owner == runtime->global) ? 0 : runtime->self;
        }
        if (variable == NULL) {
            u64 reported = (self != 0) ? self : runtime->self;
            RuntimeFail (runtime, line, "物件 %" PRIu64 " 没有变量或函数 %s", reported, name);
        } else {
            result = EvalCallUser (runtime, line, scopeSelf, *variable, args,
                                   argCount);
        }
    }
    FreeArgs (args, argCount);
    return result;
}

static struct json_object *EvalCall (Runtime *runtime, Env *env, Expr *call) {
    u64 self = 0;
    bool hasSelf = call->as.call.selfExpr != NULL;
    if (hasSelf) {
        struct json_object *selfValue = EvalExpression (runtime, env, call->as.call.selfExpr);
        if (runtime->failed) return NULL;
        if (json_object_is_type (selfValue, json_type_int))
            self = json_object_get_uint64 (selfValue);
        else
            RuntimeFail (runtime, call->line, "self 必须是 64 位非负整数");
        json_object_put (selfValue);
        if (runtime->failed) return NULL;
    }

    struct json_object *callee = NULL;
    if (call->as.call.name == NULL) {
        callee = EvalExpression (runtime, env, call->as.call.callee);
        if (runtime->failed) return NULL;
    }

    int argCount = call->as.call.count;
    struct json_object **args = NULL;
    if (argCount > 0) {
        args = calloc ((size_t)argCount, sizeof (struct json_object *));
        if (args == NULL) {
            json_object_put (callee);
            RuntimeFail (runtime, call->line, "内存不足");
            return NULL;
        }
        for (int i = 0; i < argCount; i++) {
            args[i] = EvalExpression (runtime, env, call->as.call.args[i]);
            if (runtime->failed) {
                FreeArgs (args, argCount);
                json_object_put (callee);
                return NULL;
            }
        }
    }

    if (call->as.call.name == NULL) {
        u64 scopeSelf = hasSelf ? self : runtime->self;
        struct json_object *result = NULL;
        if (runtime->depth >= SCRIPT_MAX_DEPTH) {
            RuntimeFail (runtime, call->line, "函数调用层级过深（可能存在无限递归）");
        } else {
            result = EvalCallUser (runtime, call->line, scopeSelf, callee,
                                   args, argCount);
        }
        FreeArgs (args, argCount);
        json_object_put (callee);
        return result;
    }

    return CallFunction (runtime, env, call->line, call->as.call.name, self,
                         hasSelf, args, argCount);
}


static struct json_object *ApplyBinary (Runtime *runtime, Expr *expr,
                                        struct json_object *left,
                                        struct json_object *right);

static bool MapAllNumeric (struct json_object *map) {
    json_object_object_foreach (map, key, value) {
        (void)key;
        if (!ValueIsNumeric (value)) return false;
    }
    return true;
}

static bool MapSameKeys (struct json_object *left, struct json_object *right) {
    if (json_object_object_length (left) != json_object_object_length (right))
        return false;
    json_object_object_foreach (left, key, value) {
        (void)value;
        struct json_object *other = NULL;
        if (!json_object_object_get_ex (right, key, &other)) return false;
    }
    return true;
}

static struct json_object *MapAddSub (Runtime *runtime, int line, TokenType op,
                                      struct json_object *left,
                                      struct json_object *right) {
    if (!MapAllNumeric (left) || !MapAllNumeric (right)) {
        RuntimeFail (runtime, line, "map 的所有字段必须是数字");
        return NULL;
    }
    if (!MapSameKeys (left, right)) {
        RuntimeFail (runtime, line, "map 相加或相减要求字段完全相同");
        return NULL;
    }

    struct json_object *result = json_object_new_object ();
    if (result == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    json_object_object_foreach (left, key, leftValue) {
        struct json_object *rightValue = NULL;
        json_object_object_get_ex (right, key, &rightValue);
        Expr fieldExpr;
        memset (&fieldExpr, 0, sizeof (fieldExpr));
        fieldExpr.type = EXPR_BINARY;
        fieldExpr.line = line;
        fieldExpr.as.binary.op = op;
        struct json_object *field = ApplyBinary (runtime, &fieldExpr, leftValue,
                                                 rightValue);
        if (field == NULL || runtime->failed ||
            json_object_object_add (result, key, field) != 0) {
            json_object_put (field);
            json_object_put (result);
            if (!runtime->failed) RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
    }
    return result;
}

static struct json_object *MapScalar (Runtime *runtime, int line, TokenType op,
                                      struct json_object *map,
                                      struct json_object *scalar) {
    if (!MapAllNumeric (map)) {
        RuntimeFail (runtime, line, "map 的所有字段必须是数字");
        return NULL;
    }
    if (!ValueIsNumeric (scalar)) {
        RuntimeFail (runtime, line, "map 乘法、除法或取模需要一个数字");
        return NULL;
    }
    if ((op == TOK_SLASH || op == TOK_PERCENT) &&
        ValueNumeric (scalar) == 0.0) {
        RuntimeFail (runtime, line,
                     op == TOK_SLASH ? "除数为 0" : "取余的除数为 0");
        return NULL;
    }

    struct json_object *result = json_object_new_object ();
    if (result == NULL) {
        RuntimeFail (runtime, line, "内存不足");
        return NULL;
    }
    json_object_object_foreach (map, key, value) {
        Expr fieldExpr;
        memset (&fieldExpr, 0, sizeof (fieldExpr));
        fieldExpr.type = EXPR_BINARY;
        fieldExpr.line = line;
        fieldExpr.as.binary.op = op;
        struct json_object *field = ApplyBinary (runtime, &fieldExpr, value,
                                                 scalar);
        if (field == NULL || runtime->failed ||
            json_object_object_add (result, key, field) != 0) {
            json_object_put (field);
            json_object_put (result);
            if (!runtime->failed) RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
    }
    return result;
}

static struct json_object *ApplyBinary (
    Runtime* runtime, Expr* expr, struct json_object* left, struct json_object* right) {
    TokenType op = expr->as.binary.op;
    int line = expr->line;
    struct json_object *result = NULL;

    if (op == TOK_EQ || op == TOK_NE) {
        bool equal = json_object_equal (left, right);
        result = json_object_new_boolean (op == TOK_EQ ? equal : !equal);
        if (result == NULL) RuntimeFail (runtime, line, "内存不足");
        return result;
    }
    bool leftInteger = json_object_is_type (left, json_type_int);
    bool rightInteger = json_object_is_type (right, json_type_int);

    if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
        if (!ValueIsNumeric (left) || !ValueIsNumeric (right)) {
            RuntimeFail (runtime, line, "比较运算需要两个数字");
            return NULL;
        }
        bool value;
        if (leftInteger && rightInteger) {
            u64 a = json_object_get_uint64 (left);
            u64 b = json_object_get_uint64 (right);
            value = (op == TOK_LT) ? (a < b) : (op == TOK_LE) ? (a <= b) : (op == TOK_GT) ? (a > b) : (a >= b);
        } else {
            double a = ValueNumeric (left);
            double b = ValueNumeric (right);
            value = (op == TOK_LT) ? (a < b) : (op == TOK_LE) ? (a <= b) : (op == TOK_GT) ? (a > b) : (a >= b);
        }
        result = json_object_new_boolean (value);
        if (result == NULL) RuntimeFail (runtime, line, "内存不足");
        return result;
    }
    bool leftMap = json_object_is_type (left, json_type_object);
    bool rightMap = json_object_is_type (right, json_type_object);
    if (leftMap || rightMap) {
        if (op == TOK_PLUS || op == TOK_MINUS) {
            if (!leftMap || !rightMap) {
                RuntimeFail (runtime, line, "map 相加或相减需要两个 map");
                return NULL;
            }
            return MapAddSub (runtime, line, op, left, right);
        }
        if (op == TOK_STAR) {
            if (leftMap && ValueIsNumeric (right))
                return MapScalar (runtime, line, op, left, right);
            if (ValueIsNumeric (left) && rightMap)
                return MapScalar (runtime, line, op, right, left);
            RuntimeFail (runtime, line, "map 乘法需要一个数字");
            return NULL;
        }
        if (op == TOK_SLASH || op == TOK_PERCENT) {
            if (!leftMap || !ValueIsNumeric (right)) {
                RuntimeFail (runtime, line, "map 除法或取模需要一个数字");
                return NULL;
            }
            return MapScalar (runtime, line, op, left, right);
        }
    }

    if (!ValueIsNumeric (left) || !ValueIsNumeric (right)) {
        RuntimeFail (runtime, line, "算术运算需要两个数字");
        return NULL;
    }

    if (leftInteger && rightInteger) {
        u64 a = json_object_get_uint64 (left);
        u64 b = json_object_get_uint64 (right);
        u64 integer = 0;
        switch (op) {
            case TOK_PLUS:
                if (a > UINT64_MAX - b) {
                    RuntimeFail (runtime, line, "整数溢出");
                    return NULL;
                }
                integer = a + b;
                break;
            case TOK_MINUS:
                if (a < b) {
                    result = json_object_new_double (-(double)(b - a));
                    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
                    return result;
                }
                integer = a - b;
                break;
            case TOK_STAR:
                if (a != 0 && b > UINT64_MAX / a) {
                    RuntimeFail (runtime, line, "整数溢出");
                    return NULL;
                }
                integer = a * b;
                break;
            case TOK_SLASH:
                if (b == 0) {
                    RuntimeFail (runtime, line, "除数为 0");
                    return NULL;
                }
                integer = a / b;
                break;
            case TOK_PERCENT:
                if (b == 0) {
                    RuntimeFail (runtime, line, "取余的除数为 0");
                    return NULL;
                }
                integer = a % b;
                break;
            default:
                RuntimeFail (runtime, line, "未知运算符");
                return NULL;
        }
        result = json_object_new_uint64 (integer);
        if (result == NULL) RuntimeFail (runtime, line, "内存不足");
        return result;
    }

    double a = ValueNumeric (left);
    double b = ValueNumeric (right);
    double value = 0.0;
    switch (op) {
        case TOK_PLUS:
            value = a + b;
            break;
        case TOK_MINUS:
            value = a - b;
            break;
        case TOK_STAR:
            value = a * b;
            break;
        case TOK_SLASH:
            if (b == 0.0) {
                RuntimeFail (runtime, line, "除数为 0");
                return NULL;
            }
            value = a / b;
            break;
        case TOK_PERCENT:
            if (b == 0.0) {
                RuntimeFail (runtime, line, "取余的除数为 0");
                return NULL;
            }
            value = fmod (a, b);
            break;
        default:
            RuntimeFail (runtime, line, "未知运算符");
            return NULL;
    }
    result = json_object_new_double (value);
    if (result == NULL) RuntimeFail (runtime, line, "内存不足");
    return result;
}

static bool ResolveMemberKey (Runtime *runtime, Env *env, Expr *member,
                              struct json_object **keyValue, bool *isIndex,
                              const char **key, u64 *index) {
    struct json_object *value = EvalExpression (runtime, env, member->as.member.key);
    if (runtime->failed) return false;

    if (json_object_is_type (value, json_type_string)) {
        *isIndex = false;
        *key = json_object_get_string (value);
    } else if (json_object_is_type (value, json_type_int)) {
        *isIndex = true;
        *index = json_object_get_uint64 (value);
    } else {
        RuntimeFail (runtime, member->line, "成员下标必须是字符串或整数");
        json_object_put (value);
        return false;
    }
    *keyValue = value;
    return true;
}

static TokenType AssignmentOperation (TokenType op) {
    switch (op) {
        case TOK_PLUS_ASSIGN: return TOK_PLUS;
        case TOK_MINUS_ASSIGN: return TOK_MINUS;
        case TOK_STAR_ASSIGN: return TOK_STAR;
        case TOK_SLASH_ASSIGN: return TOK_SLASH;
        case TOK_PERCENT_ASSIGN: return TOK_PERCENT;
        default: return TOK_ASSIGN;
    }
}

static struct json_object *ApplyAssignmentOperation (Runtime *runtime, int line,
                                                     TokenType op,
                                                     struct json_object *left,
                                                     struct json_object *right) {
    if (op == TOK_PLUS_ASSIGN && json_object_is_type (left, json_type_array)) {
        struct json_object *array = json_object_new_array ();
        if (array == NULL) {
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }

        size_t count = json_object_array_length (left);
        for (size_t i = 0; i < count; i++) {
            struct json_object *item = json_object_get (json_object_array_get_idx (left, i));
            if (json_object_array_add (array, item) != 0) {
                json_object_put (item);
                json_object_put (array);
                RuntimeFail (runtime, line, "内存不足");
                return NULL;
            }
        }

        struct json_object *item = json_object_get (right);
        if (json_object_array_add (array, item) != 0) {
            json_object_put (item);
            json_object_put (array);
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
        return array;
    }

    if (op == TOK_MINUS_ASSIGN && json_object_is_type (left, json_type_array)) {
        if (!json_object_is_type (right, json_type_int)) {
            RuntimeFail (runtime, line, "数组删除数量必须是整数");
            return NULL;
        }
        u64 remove = json_object_get_uint64 (right);
        size_t count = json_object_array_length (left);
        if (remove > count) {
            RuntimeFail (runtime, line, "数组删除数量 %" PRIu64 " 超过数组长度", remove);
            return NULL;
        }

        struct json_object *array = json_object_new_array ();
        if (array == NULL) {
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
        size_t keep = count - (size_t)remove;
        for (size_t i = 0; i < keep; i++) {
            struct json_object *item = json_object_get (json_object_array_get_idx (left, i));
            if (json_object_array_add (array, item) != 0) {
                json_object_put (item);
                json_object_put (array);
                RuntimeFail (runtime, line, "内存不足");
                return NULL;
            }
        }
        return array;
    }

    if (op == TOK_MINUS_ASSIGN && json_object_is_type (left, json_type_object)) {
        if (json_object_is_type (right, json_type_object)) {
            Expr expression;
            memset (&expression, 0, sizeof (expression));
            expression.type = EXPR_BINARY;
            expression.line = line;
            expression.as.binary.op = TOK_MINUS;
            return ApplyBinary (runtime, &expression, left, right);
        }
        if (!json_object_is_type (right, json_type_string)) {
            RuntimeFail (runtime, line, "对象删除键必须是字符串");
            return NULL;
        }
        const char *remove = json_object_get_string (right);
        struct json_object *object = json_object_new_object ();
        if (object == NULL) {
            RuntimeFail (runtime, line, "内存不足");
            return NULL;
        }
        json_object_object_foreach (left, key, value) {
            if (strcmp (key, remove) == 0) continue;
            if (json_object_object_add (object, key, json_object_get (value)) != 0) {
                json_object_put (object);
                RuntimeFail (runtime, line, "内存不足");
                return NULL;
            }
        }
        return object;
    }

    Expr expression;
    memset (&expression, 0, sizeof (expression));
    expression.type = EXPR_BINARY;
    expression.line = line;
    expression.as.binary.op = AssignmentOperation (op);
    return ApplyBinary (runtime, &expression, left, right);
}

static struct json_object *EvalExpression (Runtime *runtime, Env *env, Expr *expr) {
    if (runtime->failed) return NULL;

    switch (expr->type) {
        case EXPR_LITERAL:
            return json_object_get (expr->as.literal);

        case EXPR_VARIABLE: {
            if (strcmp (expr->as.name, "self") == 0) {
                struct json_object *value = json_object_new_uint64 (runtime->self);
                if (value == NULL) RuntimeFail (runtime, expr->line, "内存不足");
                return value;
            }
            if (strcmp (expr->as.name, "type") == 0) {
                const char *name = SelfTypeName (runtime->self);
                if (name == NULL) return NULL;
                struct json_object *value = json_object_new_string (name);
                if (value == NULL) RuntimeFail (runtime, expr->line, "内存不足");
                return value;
            }
            if (strcmp (expr->as.name, "caller") == 0) {
                struct json_object *value = json_object_new_uint64 (runtime->caller);
                if (value == NULL) RuntimeFail (runtime, expr->line, "内存不足");
                return value;
            }
            if (strcmp (expr->as.name, "remove") == 0) {
                struct json_object *value = json_object_new_boolean (
                    runtime->selfEnv != NULL && runtime->selfEnv->remove);
                if (value == NULL) RuntimeFail (runtime, expr->line, "内存不足");
                return value;
            }
            struct json_object *value = NULL;
            if (!EnvGet (env, expr->as.name, &value)) {
                RuntimeFail (runtime, expr->line, "变量 %s 未定义", expr->as.name);
                return NULL;
            }
            return json_object_get (value);
        }

        case EXPR_UNARY: {
            struct json_object *operand = EvalExpression (runtime, env, expr->as.unary.operand);
            if (runtime->failed) return NULL;
            struct json_object *result = NULL;
            if (expr->as.unary.op == TOK_BANG) {
                result = json_object_new_boolean (!ValueTruthy (operand));
            } else if (!ValueIsNumeric (operand)) {
                RuntimeFail (runtime, expr->line, "一元运算需要数字");
            } else {
                double number = ValueNumeric (operand);
                if (expr->as.unary.op == TOK_MINUS) number = -number;
                result = json_object_new_double (number);
            }
            json_object_put (operand);
            if (result == NULL && !runtime->failed) RuntimeFail (runtime, expr->line, "内存不足");
            return result;
        }

        case EXPR_BINARY: {
            struct json_object *left = EvalExpression (runtime, env, expr->as.binary.left);
            if (runtime->failed) return NULL;

            TokenType op = expr->as.binary.op;
            if (op == TOK_AND || op == TOK_OR) {
                bool truth = ValueTruthy (left);
                json_object_put (left);
                if ((op == TOK_AND && !truth) || (op == TOK_OR && truth)) {
                    struct json_object *result = json_object_new_boolean (truth);
                    if (result == NULL) RuntimeFail (runtime, expr->line, "内存不足");
                    return result;
                }

                struct json_object *right = EvalExpression (
                    runtime, env, expr->as.binary.right);
                if (runtime->failed) return NULL;
                truth = ValueTruthy (right);
                json_object_put (right);
                struct json_object *result = json_object_new_boolean (truth);
                if (result == NULL) RuntimeFail (runtime, expr->line, "内存不足");
                return result;
            }

            struct json_object *right = EvalExpression (runtime, env, expr->as.binary.right);
            if (runtime->failed) {
                json_object_put (left);
                return NULL;
            }
            struct json_object *result = ApplyBinary (runtime, expr, left, right);
            json_object_put (left);
            json_object_put (right);
            return result;
        }

        case EXPR_CALL:
            return EvalCall (runtime, env, expr);

        case EXPR_ARRAY: {
            struct json_object *array = json_object_new_array ();
            if (array == NULL) {
                RuntimeFail (runtime, expr->line, "内存不足");
                return NULL;
            }
            for (int i = 0; i < expr->as.array.count; i++) {
                struct json_object *item = EvalExpression (runtime, env, expr->as.array.items[i]);
                if (runtime->failed) {
                    json_object_put (array);
                    return NULL;
                }
                if (json_object_array_add (array, item) != 0) {
                    json_object_put (item);
                    json_object_put (array);
                    RuntimeFail (runtime, expr->line, "内存不足");
                    return NULL;
                }
            }
            return array;
        }

        case EXPR_OBJECT: {
            struct json_object *object = json_object_new_object ();
            if (object == NULL) {
                RuntimeFail (runtime, expr->line, "内存不足");
                return NULL;
            }
            for (int i = 0; i < expr->as.object.count; i++) {
                struct json_object *item = EvalExpression (runtime, env, expr->as.object.values[i]);
                if (runtime->failed) {
                    json_object_put (object);
                    return NULL;
                }
                if (json_object_object_add (object, expr->as.object.keys[i], item) != 0) {
                    json_object_put (item);
                    json_object_put (object);
                    RuntimeFail (runtime, expr->line, "内存不足");
                    return NULL;
                }
            }
            return object;
        }

        case EXPR_MEMBER: {
            struct json_object *base = EvalExpression (runtime, env, expr->as.member.base);
            if (runtime->failed) return NULL;

            struct json_object *keyValue = NULL;
            bool isIndex = false;
            const char *key = NULL;
            u64 index = 0;
            if (!ResolveMemberKey (runtime, env, expr, &keyValue, &isIndex, &key, &index)) {
                json_object_put (base);
                return NULL;
            }

            struct json_object *selected = NULL;
            if (isIndex) {
                if (!json_object_is_type (base, json_type_array)) {
                    RuntimeFail (runtime, expr->line, "数字 # 访问需要数组");
                } else if (index > SIZE_MAX || (size_t)index >= json_object_array_length (base)) {
                    RuntimeFail (runtime, expr->line, "数组下标 %" PRIu64 " 越界", index);
                } else {
                    selected = json_object_array_get_idx (base, (size_t)index);
                }
            } else {
                if (!json_object_is_type (base, json_type_object)) {
                    RuntimeFail (runtime, expr->line, "字符串 # 访问需要对象");
                } else if (!json_object_object_get_ex (base, key, &selected)) {
                    RuntimeFail (runtime, expr->line, "对象没有字段 %s", key);
                }
            }

            struct json_object *result = NULL;
            if (!runtime->failed) result = json_object_get (selected);
            json_object_put (keyValue);
            json_object_put (base);
            return result;
        }
    }
    RuntimeFail (runtime, expr->line, "未知表达式");
    return NULL;
}

static struct json_object *ReadMemberValue (Runtime *runtime, Expr *member,
                                           struct json_object *base,
                                           bool isIndex, const char *key, u64 index) {
    if (isIndex) {
        if (!json_object_is_type (base, json_type_array)) {
            RuntimeFail (runtime, member->line, "数字 # 访问需要数组");
            return NULL;
        }
        if (index > SIZE_MAX || (size_t)index >= json_object_array_length (base)) {
            RuntimeFail (runtime, member->line, "数组下标 %" PRIu64 " 越界", index);
            return NULL;
        }
        return json_object_get (json_object_array_get_idx (base, (size_t)index));
    }

    if (!json_object_is_type (base, json_type_object)) {
        RuntimeFail (runtime, member->line, "字符串 # 访问需要对象");
        return NULL;
    }
    struct json_object *selected = NULL;
    if (!json_object_object_get_ex (base, key, &selected)) {
        RuntimeFail (runtime, member->line, "对象没有字段 %s", key);
        return NULL;
    }
    return json_object_get (selected);
}

static bool SetMemberValue (Runtime *runtime, Expr *member, struct json_object *base,
                            bool isIndex, const char *key, u64 index,
                            struct json_object *value) {
    if (isIndex) {
        if (!json_object_is_type (base, json_type_array)) {
            RuntimeFail (runtime, member->line, "数字 # 赋值需要数组");
            return false;
        }
        if (index > SIZE_MAX || (size_t)index >= json_object_array_length (base)) {
            RuntimeFail (runtime, member->line, "数组下标 %" PRIu64 " 越界", index);
            return false;
        }
        if (json_object_array_put_idx (base, (size_t)index,
                                       json_object_get (value)) != 0) {
            RuntimeFail (runtime, member->line, "内存不足");
            return false;
        }
        return true;
    }

    if (!json_object_is_type (base, json_type_object)) {
        RuntimeFail (runtime, member->line, "字符串 # 赋值需要对象");
        return false;
    }
    if (json_object_object_add (base, key, json_object_get (value)) != 0) {
        RuntimeFail (runtime, member->line, "内存不足");
        return false;
    }
    return true;
}

static bool AssignMember (Runtime *runtime, Env *env, Expr *member,
                          struct json_object *value) {
    struct json_object *base = EvalExpression (runtime, env, member->as.member.base);
    if (runtime->failed) return false;

    struct json_object *keyValue = NULL;
    bool isIndex = false;
    const char *key = NULL;
    u64 index = 0;
    if (!ResolveMemberKey (runtime, env, member, &keyValue, &isIndex, &key, &index)) {
        json_object_put (base);
        return false;
    }

    bool ok = SetMemberValue (runtime, member, base, isIndex, key, index, value);
    json_object_put (keyValue);
    json_object_put (base);
    return ok;
}

static bool AssignMemberCompound (Runtime *runtime, Env *env, Expr *member,
                                  Expr *valueExpr, TokenType op, int line) {
    struct json_object *base = EvalExpression (runtime, env, member->as.member.base);
    if (runtime->failed) return false;

    struct json_object *keyValue = NULL;
    bool isIndex = false;
    const char *key = NULL;
    u64 index = 0;
    if (!ResolveMemberKey (runtime, env, member, &keyValue, &isIndex, &key, &index)) {
        json_object_put (base);
        return false;
    }

    struct json_object *current = ReadMemberValue (runtime, member, base, isIndex, key, index);
    if (runtime->failed) {
        json_object_put (keyValue);
        json_object_put (base);
        return false;
    }

    struct json_object *right = EvalExpression (runtime, env, valueExpr);
    struct json_object *result = NULL;
    if (!runtime->failed)
        result = ApplyAssignmentOperation (runtime, line, op, current, right);

    bool ok = false;
    if (!runtime->failed)
        ok = SetMemberValue (runtime, member, base, isIndex, key, index, result);

    json_object_put (result);
    json_object_put (right);
    json_object_put (current);
    json_object_put (keyValue);
    json_object_put (base);
    return ok;
}

static bool ExecBlock (Runtime* runtime, Env* parent, const StmtBlock* block, ExecResult* exec) {
    Env env;
    EnvInit (&env, parent);
    bool ok = true;
    for (int i = 0; i < block->count && !runtime->failed && !exec->hasReturn; i++) {
        if (!ExecStatement (runtime, &env, block->items[i], exec)) {
            ok = false;
            break;
        }
    }
    EnvFree (&env);
    return ok && !runtime->failed;
}

static bool ExecForArray (Runtime *runtime, Env *env, Stmt *stmt, ExecResult *exec) {
    struct json_object *iterable = EvalExpression (runtime, env, stmt->as.forStmt.iterable);
    if (runtime->failed) return false;
    if (json_object_is_type (iterable, json_type_object)) {
        RuntimeFail (runtime, stmt->line, "遍历对象需要两个变量");
        json_object_put (iterable);
        return false;
    }
    if (!json_object_is_type (iterable, json_type_array)) {
        RuntimeFail (runtime, stmt->line, "for 只能遍历数组或对象");
        json_object_put (iterable);
        return false;
    }
    if (stmt->as.forStmt.value != NULL) {
        RuntimeFail (runtime, stmt->line, "遍历数组只能使用一个变量");
        json_object_put (iterable);
        return false;
    }

    size_t count = json_object_array_length (iterable);
    struct json_object **items = NULL;
    if (count > 0) {
        items = calloc (count, sizeof (struct json_object *));
        if (items == NULL) {
            RuntimeFail (runtime, stmt->line, "内存不足");
            json_object_put (iterable);
            return false;
        }
        for (size_t i = 0; i < count; i++)
            items[i] = json_object_get (json_object_array_get_idx (iterable, i));
    }

    bool ok = true;
    for (size_t i = 0; i < count && !runtime->failed && !exec->hasReturn; i++) {
        if (i > 0 && !RuntimeTick (runtime, stmt->line)) {
            ok = false;
            break;
        }

        Env loop;
        EnvInit (&loop, env);
        struct json_object *element = items[i];
        items[i] = NULL;
        if (!EnvDeclareLocal (&loop, stmt->as.forStmt.key, element,
                              &runtime->error, stmt->line)) {
            runtime->failed = true;
            json_object_put (element);
            EnvFree (&loop);
            ok = false;
            break;
        }
        if (!ExecBlock (runtime, &loop, &stmt->as.forStmt.body, exec)) {
            EnvFree (&loop);
            ok = false;
            break;
        }
        EnvFree (&loop);
    }

    for (size_t i = 0; i < count; i++) json_object_put (items[i]);
    free (items);
    json_object_put (iterable);
    return ok && !runtime->failed;
}

static bool ExecForObject (Runtime *runtime, Env *env, Stmt *stmt, ExecResult *exec) {
    struct json_object *iterable = EvalExpression (runtime, env, stmt->as.forStmt.iterable);
    if (runtime->failed) return false;
    if (json_object_is_type (iterable, json_type_array)) {
        RuntimeFail (runtime, stmt->line, "遍历数组只能使用一个变量");
        json_object_put (iterable);
        return false;
    }
    if (!json_object_is_type (iterable, json_type_object)) {
        RuntimeFail (runtime, stmt->line, "for 只能遍历数组或对象");
        json_object_put (iterable);
        return false;
    }
    if (stmt->as.forStmt.value == NULL) {
        RuntimeFail (runtime, stmt->line, "遍历对象需要两个变量");
        json_object_put (iterable);
        return false;
    }

    int count = json_object_object_length (iterable);
    char **keys = NULL;
    struct json_object **values = NULL;
    if (count > 0) {
        keys = calloc ((size_t)count, sizeof (char *));
        values = calloc ((size_t)count, sizeof (struct json_object *));
        if (keys == NULL || values == NULL) {
            RuntimeFail (runtime, stmt->line, "内存不足");
            free (keys);
            free (values);
            json_object_put (iterable);
            return false;
        }
    }

    int index = 0;
    if (count > 0) {
        json_object_object_foreach (iterable, key, value) {
            keys[index] = strdup (key);
            if (keys[index] == NULL) {
                RuntimeFail (runtime, stmt->line, "内存不足");
                break;
            }
            values[index] = json_object_get (value);
            index++;
        }
    }

    bool ok = !runtime->failed;
    for (int i = 0; i < index && !runtime->failed && !exec->hasReturn; i++) {
        if (i > 0 && !RuntimeTick (runtime, stmt->line)) {
            ok = false;
            break;
        }

        Env loop;
        EnvInit (&loop, env);
        struct json_object *keyValue = json_object_new_string (keys[i]);
        if (keyValue == NULL ||
            !EnvDeclareLocal (&loop, stmt->as.forStmt.key, keyValue,
                              &runtime->error, stmt->line)) {
            json_object_put (keyValue);
            runtime->failed = true;
            EnvFree (&loop);
            ok = false;
            break;
        }
        struct json_object *value = values[i];
        values[i] = NULL;
        if (!EnvDeclareLocal (&loop, stmt->as.forStmt.value, value,
                              &runtime->error, stmt->line)) {
            json_object_put (value);
            runtime->failed = true;
            EnvFree (&loop);
            ok = false;
            break;
        }
        if (!ExecBlock (runtime, &loop, &stmt->as.forStmt.body, exec)) {
            EnvFree (&loop);
            ok = false;
            break;
        }
        EnvFree (&loop);
    }

    for (int i = 0; i < index; i++) {
        free (keys[i]);
        json_object_put (values[i]);
    }
    free (keys);
    free (values);
    json_object_put (iterable);
    return ok && !runtime->failed;
}

static bool FunctionHasOneParameter (const char *text) {
    const char *end = strchr (text, '\n');
    if (end == NULL) return false;

    int count = 0;
    const char *p = text;
    while (p < end) {
        while (p < end && *p == ' ') p++;
        if (p >= end) break;
        if (!(isalpha ((unsigned char)*p) || *p == '_')) return false;
        p++;
        while (p < end && (isalnum ((unsigned char)*p) || *p == '_')) p++;
        if (p < end && *p != ' ') return false;
        count++;
    }
    return count == 1;
}

static bool EventValueValid (struct json_object *value) {
    if (value == NULL) return true;
    return json_object_is_type (value, json_type_string) &&
           FunctionHasOneParameter (json_object_get_string (value));
}

static bool ValidateEventValue (Runtime *runtime, int line, const char *name,
                                struct json_object *value) {
    if (!IsObjectEventName (name) || EventValueValid (value)) return true;
    RuntimeFail (runtime, line, "%s 只能赋值为 null 或一元函数", name);
    return false;
}

static bool ExecStatement (Runtime* runtime, Env* env, Stmt* stmt, ExecResult* exec) {
    if (runtime->failed) return false;
    if (stmt->type != STMT_WHILE && !RuntimeTick (runtime, stmt->line)) return false;

    switch (stmt->type) {
        case STMT_IF: {
            struct json_object *condition = EvalExpression (runtime, env, stmt->as.ifStmt.condition);
            if (runtime->failed) return false;
            bool truthy = ValueTruthy (condition);
            json_object_put (condition);
            if (truthy) return ExecBlock (runtime, env, &stmt->as.ifStmt.thenBlock, exec);
            if (stmt->as.ifStmt.hasElse) return ExecBlock (runtime, env, &stmt->as.ifStmt.elseBlock, exec);
            return true;
        }

        case STMT_FOR:
            if (stmt->as.forStmt.value == NULL)
                return ExecForArray (runtime, env, stmt, exec);
            return ExecForObject (runtime, env, stmt, exec);

        case STMT_WHILE: {
            while (!runtime->failed && !exec->hasReturn) {
                if (!RuntimeTick (runtime, stmt->line)) return false;
                struct json_object *condition = EvalExpression (runtime, env, stmt->as.whileStmt.condition);
                if (runtime->failed) return false;
                bool truthy = ValueTruthy (condition);
                json_object_put (condition);
                if (!truthy) break;
                if (!ExecBlock (runtime, env, &stmt->as.whileStmt.body, exec)) return false;
            }
            return !runtime->failed;
        }

        case STMT_FUNC: {
            if (runtime->localTopLevel) {
                RuntimeFail (runtime, stmt->line, "func 只能在顶层脚本中定义");
                return false;
            }
            Env* target = runtime->selfEnv;
            if (EnvFindLocal (target, stmt->as.func.name) != NULL) {
                RuntimeFail (runtime, stmt->line, "变量 %s 已定义，不能重复 func", stmt->as.func.name);
                return false;
            }
            if (IsObjectEventName (stmt->as.func.name) &&
                stmt->as.func.paramCount != 1) {
                RuntimeFail (runtime, stmt->line, "%s 事件函数需要 1 个参数",
                             stmt->as.func.name);
                return false;
            }
            TextBuffer functionText = {0};
            bool textOk = true;
            for (int i = 0; i < stmt->as.func.paramCount && textOk; i++) {
                if (i > 0) textOk = TextBufferAppendChar (&functionText, ' ');
                if (textOk) textOk = TextBufferAppend (&functionText, stmt->as.func.params[i]);
            }
            if (textOk) textOk = TextBufferAppendChar (&functionText, '\n');
            if (textOk) textOk = TextBufferAppend (&functionText, stmt->as.func.body);
            if (!textOk) {
                TextBufferFree (&functionText);
                RuntimeFail (runtime, stmt->line, "内存不足");
                return false;
            }
            char *functionData = TextBufferTake (&functionText);
            if (functionData == NULL) {
                TextBufferFree (&functionText);
                RuntimeFail (runtime, stmt->line, "内存不足");
                return false;
            }
            struct json_object* value = json_object_new_string (functionData);
            free (functionData);
            if (value == NULL) {
                RuntimeFail (runtime, stmt->line, "内存不足");
                return false;
            }
            if (!EnvDeclareLocal (
                    target, stmt->as.func.name, value, &runtime->error, stmt->line)) {
                json_object_put (value);
                runtime->failed = true;
                return false;
            }
            return true;
        }

        case STMT_UNVAR:
            if (!EnvRemoveVisible (env, stmt->as.unvarName)) {
                RuntimeFail (runtime, stmt->line, "变量或函数 %s 未定义，无法 unvar", stmt->as.unvarName);
                return false;
            }
            return true;

        case STMT_VAR: {
            bool isGlobal = stmt->as.var.isGlobal && !runtime->localTopLevel;
            Env* target = isGlobal ? runtime->selfEnv : env;
            if (isGlobal && EnvFindLocal (target, stmt->as.var.name) != NULL) return true;

            struct json_object *value = NULL;
            if (stmt->as.var.initializer != NULL) {
                value = EvalExpression (runtime, env, stmt->as.var.initializer);
                if (runtime->failed) return false;
            }
            if (!ValidateEventValue (runtime, stmt->line, stmt->as.var.name, value)) {
                json_object_put (value);
                return false;
            }
            if (!EnvDeclareLocal (
                    target, stmt->as.var.name, value, &runtime->error, stmt->line)) {
                json_object_put (value);
                runtime->failed = true;
                return false;
            }
            return true;
        }

        case STMT_ASSIGN: {
            Expr *target = stmt->as.assign.target;
            TokenType op = stmt->as.assign.op;

            if (target->type == EXPR_VARIABLE) {
                struct json_object *value = EvalExpression (runtime, env, stmt->as.assign.value);
                if (runtime->failed) return false;

                if (strcmp (target->as.name, "remove") == 0) {
                    if (op != TOK_ASSIGN) {
                        RuntimeFail (runtime, stmt->line, "remove 只能使用 = 赋值");
                        json_object_put (value);
                        return false;
                    }
                    if (EntityKindOf (runtime->self) != ENTITY_KIND_OBJECT) {
                        RuntimeFail (runtime, stmt->line, "remove 只能用于物件脚本");
                        json_object_put (value);
                        return false;
                    }
                    if (!json_object_is_type (value, json_type_boolean)) {
                        RuntimeFail (runtime, stmt->line, "remove 只能为 bool 类型");
                        json_object_put (value);
                        return false;
                    }
                    runtime->selfEnv->remove = json_object_get_boolean (value);
                    json_object_put (value);
                    return true;
                }

                if (IsObjectEventName (target->as.name)) {
                    if (op != TOK_ASSIGN) {
                        RuntimeFail (runtime, stmt->line, "%s 只能使用 = 赋值",
                                     target->as.name);
                        json_object_put (value);
                        return false;
                    }
                    if (EntityKindOf (runtime->self) != ENTITY_KIND_OBJECT) {
                        RuntimeFail (runtime, stmt->line,
                                     "%s 只能用于物件脚本", target->as.name);
                        json_object_put (value);
                        return false;
                    }
                    if (!ValidateEventValue (runtime, stmt->line, target->as.name,
                                             value)) {
                        json_object_put (value);
                        return false;
                    }
                    void **eventSlot = EnvFindLocal (runtime->selfEnv,
                                                     target->as.name);
                    if (eventSlot == NULL) {
                        if (!EnvDeclareLocal (runtime->selfEnv, target->as.name,
                                              value, &runtime->error,
                                              stmt->line)) {
                            json_object_put (value);
                            runtime->failed = true;
                            return false;
                        }
                        return true;
                    }
                    json_object_put (*eventSlot);
                    *eventSlot = value;
                    return true;
                }

                void **slot = EnvSlot (env, target->as.name);
                if (slot == NULL) {
                    RuntimeFail (runtime, stmt->line, "变量 %s 未定义，无法赋值",
                                 target->as.name);
                    json_object_put (value);
                    return false;
                }

                if (op == TOK_ASSIGN) {
                    json_object_put (*slot);
                    *slot = value;
                    return true;
                }

                struct json_object *current = json_object_get (*slot);
                struct json_object *result = ApplyAssignmentOperation (
                    runtime, stmt->line, op, current, value);
                if (!runtime->failed) {
                    json_object_put (*slot);
                    *slot = result;
                    result = NULL;
                }
                json_object_put (result);
                json_object_put (current);
                json_object_put (value);
                return !runtime->failed;
            }

            if (target->type == EXPR_MEMBER) {
                if (op == TOK_ASSIGN) {
                    struct json_object *value = EvalExpression (runtime, env,
                                                                stmt->as.assign.value);
                    if (runtime->failed) return false;
                    bool ok = AssignMember (runtime, env, target, value);
                    json_object_put (value);
                    return ok;
                }
                return AssignMemberCompound (runtime, env, target,
                                             stmt->as.assign.value, op, stmt->line);
            }

            RuntimeFail (runtime, stmt->line, "无效的赋值目标");
            return false;
        }

        case STMT_EXPR: {
            struct json_object *value = EvalExpression (runtime, env, stmt->as.expr);
            if (runtime->failed) return false;
            json_object_put (value);
            return true;
        }

        case STMT_RETURN: {
            if (!runtime->inFunction) {
                RuntimeFail (runtime, stmt->line, "return 只能出现在函数内部");
                return false;
            }
            struct json_object *value = NULL;
            if (stmt->as.ret != NULL) {
                value = EvalExpression (runtime, env, stmt->as.ret);
                if (runtime->failed) return false;
            }
            exec->hasReturn = true;
            exec->value = value;
            return true;
        }
    }
    return false;
}

static void ScriptCleanupRemovedObjects (void) {
    IdGroup ids = ObjectIds ();
    for (int i = 0; i < ids.count; i++) {
        Object *object = EntityFind (ids.ids[i]);
        if (object == NULL) continue;
        ScriptEnv *objectEnv = EntityGetData (object->id);
        if (objectEnv != NULL && objectEnv->remove)
            NetServerDeleteObject (object->id);
    }
    IdGroupUnload (&ids);
}

TextBuffer ScriptRun (u64 self, const char* source, ScriptEnv* env, char** resultJson) {
    *resultJson = NULL;

    Program program;
    memset (&program, 0, sizeof (program));
    TextBuffer error = ProgramParse (source, &program);
    if (error.length > 0) return error;

    Env* selfEnv = EnvSelf (env, self, true);
    if (selfEnv == NULL) {
        ProgramFree (&program);
        if (self != 0 && EntityFind (self) == NULL)
            SetError (&error, 0, "self %" PRIu64 " 实体不存在", self);
        else
            SetError (&error, 0, "内存不足");
        return error;
    }

    Runtime runtime;
    memset (&runtime, 0, sizeof (runtime));
    runtime.global = env;
    runtime.self = self;
    runtime.selfEnv = selfEnv;
    runtime.inFunction = true;

    ExecResult exec = {0};
    for (int i = 0; i < program.statementCount && !runtime.failed && !exec.hasReturn; i++)
        ExecStatement (&runtime, selfEnv, program.statements[i], &exec);
    ProgramFree (&program);

    if (runtime.failed) {
        error = runtime.error;
        runtime.error = (TextBuffer){0};
        json_object_put (exec.value);
        ScriptCleanupRemovedObjects ();
        return error;
    }

    struct json_object* returnValue = exec.hasReturn ? exec.value : NULL;
    if (returnValue != NULL) {
        const char* text = json_object_to_json_string_ext (returnValue, JSON_C_TO_STRING_PLAIN);
        char* json = strdup (text);
        if (json == NULL) {
            SetError (&error, 0, "无法序列化脚本返回值");
            json_object_put (returnValue);
            ScriptCleanupRemovedObjects ();
            return error;
        }
        *resultJson = json;
        json_object_put (returnValue);
    }

    TextBufferFree (&runtime.error);
    ScriptCleanupRemovedObjects ();
    return (TextBuffer){0};
}

TextBuffer ScriptRunFunction (u64 self, const char* functionText, ScriptEnv* env,
                              u64 caller, char** resultJson) {
    *resultJson = NULL;
    TextBuffer error = {0};

    Env* selfEnv = EnvSelf (env, self, true);
    if (selfEnv == NULL) {
        if (self != 0 && EntityFind (self) == NULL)
            SetError (&error, 0, "self %" PRIu64 " 实体不存在", self);
        else
            SetError (&error, 0, "内存不足");
        return error;
    }

    Runtime runtime;
    memset (&runtime, 0, sizeof (runtime));
    runtime.global = env;
    runtime.self = self;
    runtime.caller = caller;
    runtime.selfEnv = selfEnv;
    runtime.inFunction = true;

    struct json_object* function = json_object_new_string (functionText);
    if (function == NULL) {
        SetError (&error, 0, "内存不足");
        return error;
    }

    struct json_object* returned = EvalCallUser (&runtime, 0, self, function, NULL, 0);
    json_object_put (function);
    if (runtime.failed) {
        error = runtime.error;
        runtime.error = (TextBuffer){0};
        json_object_put (returned);
        ScriptCleanupRemovedObjects ();
        return error;
    }

    if (returned != NULL) {
        const char* text = json_object_to_json_string_ext (returned, JSON_C_TO_STRING_PLAIN);
        char* json = strdup (text);
        if (json == NULL) {
            SetError (&error, 0, "无法序列化函数返回值");
            json_object_put (returned);
            ScriptCleanupRemovedObjects ();
            return error;
        }
        *resultJson = json;
        json_object_put (returned);
    }

    TextBufferFree (&runtime.error);
    ScriptCleanupRemovedObjects ();
    return (TextBuffer){0};
}


TextBuffer ScriptRunObjectEvent (u64 self, const char *event, u64 userId,
                                 ScriptEnv *env) {
    TextBuffer error = {0};
    if (self == 0 || event == NULL || env == NULL) return error;

    Env *selfEnv = EnvSelf (env, self, false);
    if (selfEnv == NULL) return error;
    void **slot = EnvFindLocal (selfEnv, event);
    if (slot == NULL || *slot == NULL) return error;
    if (!json_object_is_type (*slot, json_type_string)) {
        SetError (&error, 0, "事件不是函数");
        return error;
    }

    struct json_object *argument = json_object_new_uint64 (userId);
    if (argument == NULL) {
        SetError (&error, 0, "内存不足");
        return error;
    }

    Runtime runtime;
    memset (&runtime, 0, sizeof (runtime));
    runtime.global = env;
    runtime.self = self;
    runtime.caller = userId;
    runtime.selfEnv = selfEnv;
    runtime.inFunction = true;

    struct json_object *arguments[1] = {argument};
    struct json_object *returned = EvalCallUser (
        &runtime, 0, self, *slot, arguments, 1);
    json_object_put (arguments[0]);
    json_object_put (returned);
    if (runtime.failed) {
        error = runtime.error;
        runtime.error = (TextBuffer){0};
    }
    TextBufferFree (&runtime.error);
    ScriptCleanupRemovedObjects ();
    return error;
}
static TextBuffer ProgramParse (const char* source, Program* program) {
    memset (program, 0, sizeof (*program));

    Parser parser;
    memset (&parser, 0, sizeof (parser));
    parser.lexer.source = source;
    parser.lexer.length = strlen (source);
    parser.lexer.pos = 0;
    parser.lexer.line = 1;
    parser.lexer.column = 1;
    parser.lexer.lineStart = true;
    parser.lexer.bracketDepth = 0;
    parser.lexer.indentCount = 1;
    parser.lexer.indentStack[0] = 0;
    LexerNext (&parser.lexer);
    if (parser.lexer.token.type == TOK_ERROR) ParserTakeLexerError (&parser);

    while (!parser.failed && !ParserCheck (&parser, TOK_EOF)) {
        while (ParserMatch (&parser, TOK_NEWLINE)) {
        }
        if (ParserCheck (&parser, TOK_EOF)) break;
        if (ParserCheck (&parser, TOK_INDENT) || ParserCheck (&parser, TOK_DEDENT)) {
            SetError (&parser.error, parser.lexer.token.line, "顶层出现意外缩进");
            parser.failed = true;
            break;
        }
        Stmt* stmt = ParseStatement (&parser, true);
        if (stmt == NULL) break;
        if (!ProgramAddStatement (program, stmt)) {
            SetError (&parser.error, stmt->line, "内存不足");
            StmtFree (stmt);
            parser.failed = true;
        }
    }
    free (parser.lexer.token.text);
    TextBufferFree (&parser.lexer.error);
    if (parser.failed) {
        if (parser.error.length == 0) SetError (&parser.error, 0, "脚本解析失败");
        ProgramFree (program);
        return parser.error;
    }
    TextBufferFree (&parser.error);
    return (TextBuffer){0};
}
