#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <msgpack.h>

// due msgpack nature.. it holds some data before finalize the object
typedef struct _msgpack_sprintf_context
{
    msgpack_packer *pk; // msgpack_packer object used to write
    msgpack_sbuffer *sb;    // msgpack_packer data is sbuf object .. used as backup
    va_list         ap;

    int flags;  // is an array or a map?
    size_t size;   // size reflects sb->size when the function is called
} msgpack_sprintf_context;

typedef int (*msgpack_sprintf_callback)(msgpack_packer *pack, void *opt);

// based on Martin Kallaman float32 https://gist.github.com/martin-kallman/5049614
// source code from Alex Zhukov https://gist.github.com/zhuker/b4bd1fb306c7b04975b712c37c4c4075
static void hf_to_float32(float *out, const uint16_t in)
{
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;

    t1 = in & 0x7fffu;                       // Non-sign bits
    t2 = in & 0x8000u;                       // Sign bit
    t3 = in & 0x7c00u;                       // Exponent

    t1 <<= 13u;                              // Align mantissa on MSB
    t2 <<= 16u;                              // Shift sign bit into position

    t1 += 0x38000000;                       // Adjust bias

    t1 = (t3 == 0 ? 0 : t1);                // Denormals-as-zero

    t1 |= t2;                               // Re-insert sign bit

    *((uint32_t *) out) = t1;
};


/// <summary>
/// find the next token
/// </summary>
/// <param name="fmt">string</param>
/// <returns>pointer to next token or NULL</returns>
static const char* move_next_token(const char* fmt)
{
    if (fmt != NULL)
    {
        for (; *fmt != 0; ++fmt)
        {
            if (*fmt == ' ')
                continue;
            if (*fmt == ',')
                continue;
            if (*fmt == ':')
                continue;
            break;
        }
        if (*fmt == 0)
            return NULL;
    }

    return fmt;
}
/**
 * get_token parse the input string to find the initial sequence of a token
 * like ' aKey:' '  anotherKey'.
 * @param src string to scan
 * @param end end of token.. it could be : or ' '
 * @return pointer to first valid char
 */
static const char *get_token(const char *src, const char **end)
{
    const char* start = move_next_token(src);

    *end = NULL;

    if (start == NULL)
    {
        return NULL;
    }

    for(src = start; *src != '\0'; ++src)
    {
        if (*src == ' ')
            break;
        if (*src == ':')
            break;
        if (*src == '[')
            break;
        if (*src == '{')
            break;
    }
    
    *end = src;
    
    if (*src == '\0')
        return NULL;

    return start;
}

/// @brief write a default value (true, false, nil) in case fmt contains a specific keyword
/// @param pk msgpack destination object
/// @param fmt current position of fmt
/// @return 0 found keyword, or non zero
static int default_keywords(msgpack_packer *pk, const char *fmt)
{
    int valid = 0;
    switch(*fmt)
    {
    case 'f':
    if (strlen(fmt) >= 5 && memcmp(fmt, "false", 5) == 0)
    {
        msgpack_pack_false(pk);
        valid = 1;
    }
    break;
case 't':
    if (strlen(fmt) >= 4 && memcmp(fmt, "true", 4) == 0)
    {
        msgpack_pack_true(pk);
        valid = 1;
    }
    break;
case 'n':
    if (strlen(fmt) >= 3 && (memcmp(fmt, "null", 4) == 0 || memcmp(fmt, "nil", 3) == 0))
    {
        msgpack_pack_nil(pk);
        valid = 1;
    }
    }

    return !valid;
}
/// @brief copy from src all buffer to dst
/// @param dst 
/// @param src 
static void msgpack_sprintf_bulk(msgpack_packer *dst, msgpack_packer *src)
{
    msgpack_sbuffer *sbuf_dst = (msgpack_sbuffer *)dst->data;
    msgpack_sbuffer *sbuf_src = (msgpack_sbuffer *)src->data;

    msgpack_sbuffer_write(dst->data, sbuf_src->data, sbuf_src->size);
}

/**
 * parse the element after '%' and
 * @param pk
 * @param fmt
 * @param args
 * @return
 */
static const char* msgpack_sprintf_pack_arg(msgpack_packer *pk, const char *fmt, va_list *ap)
{
    uint16_t u16;
    float f32; double f64;
    int32_t i32;
    uint32_t u32;
    void *ptr;
    msgpack_sprintf_callback callback;
    msgpack_packer new_pk;
    msgpack_sbuffer sbuf;
    uint8_t half = 0;
    int loop_done;

    do
    {
        fmt++;
        switch(*fmt)
        {
            case 'h': half = 1; // half prefix
                break;
            case 's':
                ptr = va_arg(*ap, void *);
                if (ptr == NULL)
                    msgpack_pack_nil(pk);
                else
                    msgpack_pack_str_with_body(pk, ptr, strlen((const char *)ptr));
                break;
            case 'S': // not yet supported;
                break;
            case 'c':
                u32 = va_arg(*ap, uint32_t);
                msgpack_pack_str_with_body(pk, &u32, 1);
                break;
            case 'n': // null pointer
                va_arg(*ap, void *); // ignore
                msgpack_pack_nil(pk);
                break;
            case 'd': // C23 boolean
                u32 = va_arg(*ap, uint32_t);
                if (u32)
                    msgpack_pack_true(pk);
                else
                    msgpack_pack_false(pk);
                break;
            case 'p': // binary 8/16/32
                ptr = va_arg(*ap, void *); // fetch pointer
                u32 = va_arg(*ap, uint32_t);   // fetch size
                msgpack_pack_bin_with_body(pk, ptr, u32);
                break;
            case 'f': // float
                if (half)
                {
                    u16 = va_arg(*ap, uint16_t);    // BFLOAT16 .. easily mapped to float32
                    f32 = 0.0;
                    u32 = (uint32_t)(u16 << 16);
#if (MSGPACK_ENDIAN_LITTLE_BYTE == 1)
                    u32 = _msgpack_be32(u32);
#endif
                    memcpy(&f32, &u32, sizeof(u32));
                }
                else
                {
                    f32 = va_arg(*ap, float);
                    msgpack_pack_float(pk, f32);
                }
                break;
            case 'e': // float64
                if (half)
                {
                    u16 = va_arg(*ap, uint16_t);
                    f32 = 0.0;
                    hf_to_float32(&f32, u16);
                    msgpack_pack_float(pk, f32);
                }
                else
                {
                    f64 = va_arg(*ap, double);
                    msgpack_pack_double(pk, f64);
                }
                half = 0;
                break;
            case 'i': //int
                if (half)
                    i32 = (int32_t)(va_arg(*ap, int16_t));
                else
                    i32 = va_arg(*ap, int);
                msgpack_pack_int(pk, (int)i32);
                half = 0;
                break;
            case 'u': //uint
                if (half)
                    u32 = (uint32_t)(va_arg(*ap, uint16_t));
                else
                    u32 = va_arg(*ap, uint32_t);
                msgpack_pack_unsigned_int(pk, u32);
                half = 0;
                break;
            case '!': // callback.. in this case it should be a map.. so just a nested object without being recursive
                callback = va_arg(*ap, msgpack_sprintf_callback);
                ptr = va_arg(*ap, void*);
                msgpack_sbuffer_init(&sbuf);
                msgpack_packer_init(&new_pk, &sbuf, msgpack_sbuffer_write);

                callback(&new_pk, ptr);

                msgpack_sprintf_bulk(pk, &new_pk);
                msgpack_sbuffer_destroy(&sbuf);
                half = 0;
                break;
        }
    } while(half != 0);

    return fmt;
}

/* create a map in msgpack */
static const char *msgpack_sprintf_obj(msgpack_sprintf_context *ctx, const char *fmt)
{
    void* ptr;
    msgpack_sprintf_callback callback;
    msgpack_packer new_pk;
    msgpack_sbuffer sbuf;

    const char* next_token;

    msgpack_packer *pk = ctx->pk;

    msgpack_sprintf_context tmp;
    tmp.flags = 0;
    tmp.pk = pk;
    tmp.size = ctx->sb->size;
    tmp.sb = ctx->sb;
    tmp.ap = ctx->ap;

    if (ctx->flags == MSGPACK_OBJECT_MAP)
        msgpack_pack_map(ctx->pk, 65537); // force msgpack to write a map32
    else
        msgpack_pack_array(ctx->pk, 65537);

    int32_t object_size = 0;

    char sequence_terminator = (ctx->flags == MSGPACK_OBJECT_ARRAY) ? ']' : '}';

    for(; fmt != NULL && *fmt != '\0' && *fmt != sequence_terminator; ++fmt)
    {
        if (ctx->flags == MSGPACK_OBJECT_MAP)
        {
            // fetch the key.. then fetch a value
            const char* token_start = NULL, * token_end = NULL;
            token_start = get_token(fmt, &token_end);

            if (token_start == NULL || token_end == NULL)
                break;

            size_t token_length = token_end - token_start;

            msgpack_pack_str_with_body(ctx->pk, token_start, token_length); // write the key
            fmt = token_end;
        }

        int r = 0;
        int done = 0;
        do
        {
            fmt = move_next_token(fmt);
            if (!fmt)
                break;

            switch (*fmt) {
                // ignore character
                case '%':
                    if (fmt[1] == '!' && ctx->flags == MSGPACK_OBJECT_ARRAY)
                    {
                        callback = va_arg(tmp.ap, msgpack_sprintf_callback);
                        ptr = va_arg(tmp.ap, void*);
                        msgpack_sbuffer_init(&sbuf);
                        msgpack_packer_init(&new_pk, &sbuf, msgpack_sbuffer_write);

                        do
                        {
                            r = callback(&new_pk, ptr);
                            ++object_size;
                        } while (r != 0);

                        msgpack_sprintf_bulk(pk, &new_pk);
                        msgpack_sbuffer_destroy(&sbuf);
                        done = 1;
                        fmt++;
                    }
                    else
                    {
                        fmt = msgpack_sprintf_pack_arg(tmp.pk, fmt, &tmp.ap);
                        done = 1;
                        ++object_size;
                    }
                    break;
                case '{':
                    tmp.flags = MSGPACK_OBJECT_MAP;
                    next_token = move_next_token(fmt);
                    fmt = msgpack_sprintf_obj(&tmp, ++next_token);
                    ++object_size;
                    done = 1;
                    break;
                case '[':
                    tmp.flags = MSGPACK_OBJECT_ARRAY;
                    next_token = move_next_token(fmt);
                    fmt = msgpack_sprintf_obj(&tmp, ++next_token);
                    ++object_size;
                    done = 1;
                    break;
                default:
                    if (default_keywords(tmp.pk, fmt) == 0)
                    {
                        fmt = move_next_token(fmt);
                        ++object_size;
                        done = 1;
                    }
                    break;
            }
        } while(done == 0);
    }

    ctx->ap = tmp.ap;

    if (object_size >= 0)  // we have a valid size.. so we can adjust the buffer
    {
        size_t data_off = tmp.size + 5;
        size_t data_len = ctx->sb->size - tmp.size - 5;

        ctx->sb->size = tmp.size;   // reset the pointer

        if (ctx->flags == MSGPACK_OBJECT_MAP)
            msgpack_pack_map(ctx->pk, object_size);    // write the right size
        else
            msgpack_pack_array(ctx->pk, object_size);

        memcpy(ctx->sb->data + ctx->sb->size, ctx->sb->data + data_off, data_len);  // transfer the bytes
        ctx->sb->size += data_len;
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
    msgpack_sprintf_context ctx;

    ctx.pk = pk;
    ctx.size = 0;
    ctx.flags = 0;
    ctx.sb = (msgpack_sbuffer *) pk->data;  // to improve map/array serialization, I need to rewrite data structure
    va_start(ctx.ap, fmt);

    for(; *fmt != '\0'; ++fmt)
    {
        switch(*fmt)
        {
            case '[':
                ctx.flags = MSGPACK_OBJECT_ARRAY;
                fmt = msgpack_sprintf_obj(&ctx, ++fmt);
                break;
            case '{':
                ctx.flags = MSGPACK_OBJECT_MAP;
                fmt = msgpack_sprintf_obj(&ctx, ++fmt);
                break;
            case ' ':
                break;
            default:    // unknown formatter
                break;
        }
    }
    return 0;
}