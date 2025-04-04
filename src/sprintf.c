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
    va_list         ap;

    int flags;  // is an array or a map?
    size_t size;   // size reflects sb->size when the function is called
} msgpack_sprintf_context;

typedef void (*msgpack_sprintf_callback)(msgpack_packer *pack, void *opt);

// msgpack can call itself as recursive
static const char *msgpack_sprintf_map(msgpack_sprintf_context *ctx, const char *fmt);
static const char *msgpack_sprintf_array(msgpack_sprintf_context *ctx, const char *fmt);

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

    do
    {
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
                break;
            case 'i': //int
                if (half)
                    i32 = (int32_t)(va_arg(*ap, int16_t));
                else
                    i32 = va_arg(*ap, uint32_t);
                msgpack_pack_int(pk, (int)i32);
                break;
            case 'u': //uint
                if (half)
                    u32 = (uint32_t)(va_arg(*ap, uint16_t));
                else
                    u32 = va_arg(*ap, uint32_t);
                msgpack_pack_unsigned_int(pk, u32);
                break;
            case '!': // callback
                callback = va_arg(*ap, msgpack_sprintf_callback);
                ptr = va_arg(*ap, void *);
                msgpack_sbuffer_init(&sbuf);
                msgpack_packer_init(&new_pk, &sbuf, msgpack_sbuffer_write);
                
                callback(&new_pk, ptr);

                msgpack_sprintf_bulk(pk, &new_pk);
                msgpack_sbuffer_free(&sbuf);
                break;
        }
        ++fmt;
    } while(half != 0);

    return fmt;
}

/* create an array in msgpack */
static const char *msgpack_sprintf_array(msgpack_sprintf_context *ctx, const char *fmt)
{
    int32_t array_size = -1;
    msgpack_packer *pk = ctx->pk;

    msgpack_sprintf_context tmp;
    tmp.flags = 0;
    tmp.pk = pk;
    tmp.ap = ctx->ap;
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
                fmt = msgpack_sprintf_pack_arg(tmp.pk, fmt, &tmp.ap);
                ++array_size;
                break;
            case '{': // consume a map
                fmt = msgpack_sprintf_map(&tmp, fmt++);
                ++array_size;
                break;
            case '[': // consume an array
                fmt = msgpack_sprintf_array(&tmp, fmt++);
                ++array_size;
                break;
            default:
                break;
        }
    }

    ctx->ap = tmp.ap;
    if (array_size >= 0)  // we have a valid size.. so we can adjust the buffer
    {
        size_t data_off = tmp.size + 5;
        size_t data_len = ctx->sb->size - tmp.size + 5;

        ctx->sb->size = tmp.size;   // reset the pointer

        msgpack_pack_map(ctx->pk, array_size);    // write the right size
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
static const char *msgpack_sprintf_map(msgpack_sprintf_context *ctx, const char *fmt)
{
    msgpack_packer *pk = ctx->pk;

    msgpack_sprintf_context tmp;
    tmp.flags = 0;
    tmp.pk = pk;
    tmp.size = ctx->sb->size;
    tmp.sb = ctx->sb;
    tmp.ap = ctx->ap;

    msgpack_pack_map(ctx->pk, 0x65537); // force msgpack to write a map32

    int32_t map_size = -1;

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
                    fmt = msgpack_sprintf_pack_arg(tmp.pk, fmt, &tmp.ap);
                    done = true;
                    ++map_size;
                    break;
                case '{':
                    fmt = msgpack_sprintf_map(&tmp, fmt++);
                    ++map_size;
                    done = true;
                    break;
                case '[':
                    fmt = msgpack_sprintf_array(&tmp, fmt++);
                    ++map_size;
                    done = true;
                    break;
                default:
                    break;
            }
            ++fmt;
        } while(done);
    }

    ctx->ap = tmp.ap;

    if (map_size >= 0)  // we have a valid size.. so we can adjust the buffer
    {
        size_t data_off = tmp.size + 5;
        size_t data_len = ctx->sb->size - tmp.size + 5;

        ctx->sb->size = tmp.size;   // reset the pointer

        msgpack_pack_map(ctx->pk, map_size);    // write the right size
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
                ctx.flags = MSGPACK_SPRINTF_PACK_ARRAY;
                fmt = msgpack_sprintf_array(&ctx, fmt++);
                break;
            case '{':
                ctx.flags = MSGPACK_SPRINTF_PACK_MAP;
                fmt = msgpack_sprintf_map(&ctx, fmt++);
                break;
            case ' ':
                break;
            default:    // unknown formatter
                break;
        }
    }
    return 0;
}