# msgpack_sprintf
A sprintf function to create msgpack objects, using the printf syntax with some changes to be compatible with JSON syntax.

This repository contains some simple code, to allows c/c++ developers to create msgpack objects (map and array) using same syntax of printf/sprintf/scanf function.

This implementation is stack based, except msgpack callback implementation, which can allocate memory through sbuffer primitives.

Actually, the string is not validated as input, so in case there is an error, the output object could be corrupted (actually I'm writing in the buffer the maximum number of objects possible, then I overwrite the value with the final one).

## msgpack-c
This source code is based on [msgpack-c](https://github.com/msgpack/msgpack-c/tree/c_master) source code. Until this project is under development, the original source code is duplicated into this repo to allow test/development. The modified files are:
- Files.cmake: Added src/sprintf.c
- include/msgpack.h - Added the msgpack_sprintf function declaration

```c
int msgpack_sprintf(msgpack_packer *pack, const char *fmt, ...);
```

Parameters:
- pack: A msgpack_packer struct, initialized through msgpack_packer_new
- fmt: A pointer to a null-terminated string that contains the text of msgpack to write in pack object. See fmt syntax section for more Information.

To simplify the creation of objects, and to improve code readability, keys in map can be hardcoded with/without double quotes, and immediate values as integer, strings and some symbols are accepted.

## fmt syntax
Symbols and place-holder adopted internally for fmt syntax.

| specifier    | Size | msgpack-object | Description | Status |
|--------------|------|----------------|-------------|--------|
| { }          | 1..n | map            | Create a map that supports from 0 to 2^32-1 objects | See Note 1 |
| [ ]          | 1..n | array          | Create an array of objects that supports from 0 to 2^32'-1 elements | OK |
| %c           | 1    | fixstr [1]     | Store a value as fixstr of 1 byte | TBT |
| %s           | 0..n | str            | store a value as string | Note 1 |
| %S           |      |                | store a value as string, but the input is an unicode string | Note 2 |
| %n           | 1    | nil            | Store a null pointer, regardless the value placed in variadic arguments | Note 3 |
| %d           | 1    | false or true  | Store a boolean value, the variadic must be a value, 0 for false, any other value is true | TBT |
| %p           | 0..n | bin           | Store a buffer as bin8, bin16 or bin32, but it requires two arguments in variadic, a pointer and a size | Note 3 |
| %hf          | 1+4  | bfloat16       | The input is a bfloat16 number, which will be converted into float and stored as float in msgpack | Note 4 |
|              |      |                | The bfloat16 is mapped into a float number without rouding |
| %f           | 1+4  | float32        | Store a float value | TBT |
| %he          | 1+4  | float32        | The input is a float16 number, which will be converted into float and stored as float in msgpack | TBI |
| %e           | 1+8  | float64        | Store a double value | TBT |
| %i           | 1..9 | int8..int64    | Store an integer value, from 8 bit to 64bit | TBT |
| %u           | 1..9 | UINT8..UINT64  | Store an unsigned integer value, from 8 to 64 bit. | TBT |
| %!           | 1..n |                | A place holder used to fill an object through a callback function | TBT |
| null         | 1    | nil            | write nil as value | TBI |
| key          | 1..n | fixstr...      | write a key as string | Note 5 |

Status definition:
- TESTED
- TBT: To be tested
- TBI: To be implemented

- Note 1: A map or an array has a variable number of elements, in msgpack both uses different opcodes (a simple opcode for objects with a size between 0 and 16.. a second opcode if size is less than 65537 and the third opcode for 2^32-1 elements.   When a new object (array or map) is written, we need to specify to msgpack how many objects we should write. To bypass this limitation, initially it's written the 65537 objects (forcing msgpack to use the 32-bit opcode), and when the serialization of the sequence is completed, the buffer is overwritten with the right opcode (moving back all data). It's not an efficient way.. but for now it's ok.
- Note 2: Strings are encoded using UTF8 format. Unicode strings must be converted using the appropriate function for your O.S. For now they are not handled (the symbol is reserved, but not handled).
- Note 3: nil (or null) can be specified as value, but if a string or a pointer, or a callback is null, the value will be null, so please check the resulting value
- Note 4: The Half-Float and the BFLOAT are handled without using FPU instructions, their value is retrieved as uint16_t from the variadic arguments.
- Note 5: All identifier can starts with a number or a letter, the only symbols currently used as token are space, comma, square and curly brackets. In case of a value, nil, null, true or false are also accepted as immediate value (they don't require a variadic argument).

## Examples
```c
/* Create an object of one element with a string as value */
msgpack_sprintf(out, "{key: %s}", "value");

/* Create an array of integers */
msgpack_sprintf(out, "[%i, %i, %i]", 1, 2, 3);

/* Create an object with a binary as value */
const char *str = "This is a buffer!";
msgpack_sprintf(out, "{key: %s, buffer: %p}", "value",
   str, // %p uses two arguments, the first one as pointer to the buffer
   strlen(str) // the second one, as size of bin buffer
  );

/* Create an object with a custom value */
int custom_element(msgpack_packer *pack, void *opt)
{
  msgpack_sprintf(pack, "[%i]", 1);
  return 0;
}

int recursive_element(msgpack_packer *pack, void *opt)
{
  msgpack_sprintf(pack, "%i", 1);
  int *r = (int *) opt;
  *r--;
  return *r != 0;
}

msgpack_sprintf(out, "{key: %!}", custom_element, NULL);

/** Deprecated format
msgpack_sprintf(out, "{key: [%!]}", recursive_element, (void *) 4); // call recursive_element until the function returns 0
 **/
```

## Array Recursive Expansion
To allow format for `%!` and to keep simple logic (until the internal parsing will be improved) the recursion must be used only to generate array or map, so I discourage the use of inline array expansion because msgpack_sprintf at top level evaluate an array or a map.


## Extensions
msgpack supports extensions in data serialization, but actually it's not possible.

## Examples:
```c
#include <msgpack.h>
#include <stdio.h>

int main(void)
{
  msgpack_sbuffer sbuf;
  msgpack_sbuffer_init(&sbuf);

  msgpack_packer pk;
  msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

  msgpack_sprintf(&pk, "{keyA: %s keyB: %i keyC: [%i %i] }",
    "keyA value",
    1,
    2, 3);

  /* deserialize the buffer into msgpack_object instance. */
    /* deserialized object is valid during the msgpack_zone instance alive. */
    msgpack_zone mempool;
    msgpack_zone_init(&mempool, 2048);

    msgpack_object deserialized;
    msgpack_unpack(sbuf.data, sbuf.size, NULL, &mempool, &deserialized);

    /* print the deserialized object. */
    msgpack_object_print(stdout, deserialized);
    puts("");

    msgpack_zone_destroy(&mempool);
    msgpack_sbuffer_destroy(&sbuf);
    return 0;
}
```

Excepted JSON value:
```json
{
  "keyA": "keyA value",
  "keyB": 1,
  "keyC": [
    2, 3
  ]
}
```