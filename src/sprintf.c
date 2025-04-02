#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <msgpack.h>

#define MSGPACK_SPRINTF_PACK_MAP    1
#define MSGPACK_SPRINTF_PACK_ARRAY  2

// due msgpack nature.. it holds some data before finalize the object
typedef struct _msgpack_sprintf_context
{
    msgpack_packer *pk; // msgpack_packer object used to write
    msgpack_sbuffer *sb;    // msgpack_packer data is sbuf object .. used as backup

    int flags;  // is an array or a map?
    int size;   // size reflects sb->size when the function is called
} msgpack_sprintf_context;

// msgpack can call itself as recursive
static const char *msgpack_sprintf_map(msgpack_sprintf_context *ctx, const char *fmt, va_list args);
static const char *msgpack_sprintf_array(msgpack_sprintf_context *ctx, const char *fmt, va_list args);

/**
 * get_token parse the input string to find the initial sequence of a token
 * like ' aKey:' '  anotherKey'.
 * @param src string to scan
 * @param end end of token.. it could be : or ' '
 * @return pointer to first valid char
 */
static const char *get_token(const char *src, const char **end)
{
    const char *start = NULL;
    *end = NULL;

    for(; *src != '\0'; ++src)
    {
        if (*src == ' ' && start == NULL)   // space.. ignore
            continue;
        if (*src == ':' && start == NULL)   // colon.. no start.. ERROR!
            break;
        
        if (*src == ' ' && start != NULL)
            break;
        if (*src == ':' && start != NULL)
            break;
        
        if (!start)
            start = src;
    }

    if (start != NULL)
    {
        *end = src;
    }

    return start;
}

// encode a string taken from variadic
static void msgpack_sprintf_pack_str(msgpack_packer *pk, va_list args)
{
    const char *in = va_arg(args, const char *);
    size_t size = 0;

    if (in != NULL)
    {
        size = strlen(in);
    }
    msgpack_pack_str_with_body(pk, in, size);
}

/**
 * parse the element after '%' and
 * @param pk
 * @param fmt
 * @param args
 * @return
 */
static const char* msgpack_sprintf_pack_arg(msgpack_packer *pk, const char *fmt, va_list args)
{
    uint32_t u32;
    void *ptr;

    switch(*fmt)
    {
        case 's':
            ptr = va_arg(args, void *);
            if (ptr == NULL)
                msgpack_pack_nil(pk);
            else
                msgpack_pack_str_with_body(pk, ptr, strlen((const char *)ptr));
            break;
        case 'S': // not yet supported;
            break;
        case 'c':
            u32 = va_arg(args, uint32_t);
            msgpack_pack_str_with_body(pk, &u32, 1);
            break;
        case 'n': // null pointer
            va_arg(args, void *); // ignore
            msgpack_pack_nil(pk);
            break;
        case 'b': // boolean
            u32 = va_arg(args, uint32_t);
            if (u32)
                msgpack_pack_true(pk);
            else
                msgpack_pack_false(pk);
            break;
        case 'p': // binary 8/16/32
            ptr = va_arg(args, void *); // fetch pointer
            u32 = va_arg(args, uint32_t);   // fetch size
            msgpack_pack_bin_with_body(pk, ptr, u32);
            break;
        case 'f': // float
            break;
        case 'e': // float64
            break;
        case 'i': //int
            u32 = va_arg(args, uint32_t);
            msgpack_pack_int(pk, (int)u32);
            break;
        case 'u': //uint
            u32 = va_arg(args, uint32_t);
            msgpack_pack_unsigned_int(args, u32);
            break;
        case '!': // callback
            break;
    }
}

/* create an array in msgpack */
static const char *msgpack_sprintf_array(msgpack_sprintf_context *ctx, const char *fmt, va_list args)
{
    ssize_t array_size = -1;
    msgpack_packer *pk = ctx->pk;

    msgpack_sprintf_context tmp;
    tmp.flags = 0;
    tmp.pk = pk;
    tmp.size = ctx->sb->size;
    tmp.sb = ctx->sb;

    msgpack_pack_array(ctx->pk, 0x65537); // force msgpack to write a array32

    for(; *fmt != '\0' || *fmt != ']'; ++fmt)
    {
        switch(*fmt)
        {
            // ignore character
            case ' ':
            case ',':
                break;
            case '%': ++fmt;
              fmt = msgpack_sprintf_pack_arg(ctx->pk, fmt, args);
                break;
            case '{': // consume a map
                fmt = msgpack_sprintf_map(ctx, fmt++, args);
                break;
            case '[': // consume an array
                fmt = msgpack_sprintf_array(ctx, fmt++, args);
                break;
            default:
                break;
        }
    }

    if (array_size >= 0)  // we have a valid size.. so we can adjust the buffer
    {
        size_t data_off = tmp.size + 5;
        size_t data_len = ctx->sb->size - tmp.size + 5;

        ctx->sb->size = tmp.size;   // reset the pointer

        msgpack_pack_map(ctx->sb, array_size);    // write the right size
        memcpy(ctx->sb->data + ctx->sb->size, ctx->sb->data + data_off, data_len);  // transfer the bytes
    }
    else
    {   // an error has been detected parsing elements.. the full sequence will be destroyed
        size_t clear_off = ctx->sb->size;
        ctx->sb->size = tmp.size;

        memset(ctx->sb->data + ctx->sb->size, 0, clear_off - tmp.size);
    }

    return fmt;
}

/* create a map in msgpack */
static const char *msgpack_sprintf_map(msgpack_sprintf_context *ctx, const char *fmt, va_list args)
{
    msgpack_packer *pk = ctx->pk;

    msgpack_sprintf_context tmp;
    tmp.flags = 0;
    tmp.pk = pk;
    tmp.size = ctx->sb->size;
    tmp.sb = ctx->sb;

    msgpack_pack_map(ctx->pk, 0x65537); // force msgpack to write a map32

    ssize_t map_size = -1;

    for(; *fmt != '\0' || *fmt != '}'; ++fmt)
    {
        // fetch the key.. then fetch a value
        const char *token_start = NULL, *token_end = NULL;
        token_start = get_token(fmt, &token_end);

        if (token_start == NULL || token_end == NULL)
            break;

        size_t token_length = token_end - token_start;

        msgpack_pack_str_with_body(ctx->pk, token_start, token_length); // write the key
        fmt = token_end;

        bool done = false;
        do
        {
            switch (*fmt) {
                // ignore character
                case ':':
                case ' ':
                case ',':
                    break;
                case '%':
                    ++fmt;
                    fmt = msgpack_sprintf_pack_arg(&tmp, fmt, args);
                    done = true;
                    break;
                default:
                    break;
            }
            ++fmt;
        } while(done);
    }

    if (map_size >= 0)  // we have a valid size.. so we can adjust the buffer
    {
        size_t data_off = tmp.size + 5;
        size_t data_len = ctx->sb->size - tmp.size + 5;

        ctx->sb->size = tmp.size;   // reset the pointer

        msgpack_pack_map(ctx->sb, map_size);    // write the right size
        memcpy(ctx->sb->data + ctx->sb->size, ctx->sb->data + data_off, data_len);  // transfer the bytes
    }
    else
    {   // an error has been detected parsing elements.. the full sequence will be destroyed
        size_t clear_off = ctx->sb->size;
        ctx->sb->size = tmp.size;

        memset(ctx->sb->data + ctx->sb->size, 0, clear_off - tmp.size);
    }
    return fmt;
}

int msgpack_sprintf(msgpack_packer* pk, const char *fmt, ...)
{
    va_list args;
    msgpack_sprintf_context ctx;

    ctx.pk = pk;
    ctx.size = 0;
    ctx.flags = 0;
    ctx.sb = (msgpack_sbuffer *) pk->data;  // to improve map/array serialization, I need to rewrite data structure

    for(va_start(args, fmt); *fmt != '\0'; ++fmt)
    {
        switch(*fmt)
        {
            case '[':
                ctx.flags = MSGPACK_SPRINTF_PACK_ARRAY;
                fmt = msgpack_sprintf_array(pk, fmt++, args);
                break;
            case '{':
                ctx.flags = MSGPACK_SPRINTF_PACK_MAP;
                fmt = msgpack_sprintf_map(pk, fmt++, args);
                break;
            case ' ':
                break;
            default:    // unknown formatter
                break;
        }
    }
    return 0;
}