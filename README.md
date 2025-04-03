# msgpack_sprintf
A sprintf function to create msgpack objects

This repository contains some simple code, to allows c/c++ developers to create msgpack objects (map and array) using same syntax of printf/sprintf/scanf function.ù

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
MsgPack supports object serialization like JSON.

Symbols and place-holder adopted internally for fmt syntax.
Note: Not specifiers are supported, due binary serialization in msgpack

| specifier    | Size | msgpack-object | Description |
|--------------|------|----------------|-------------|
| { }          | 1..n | fixmap         | Create a map that supports from 0 to 2^32-1 objects |
|              |      | map16          | |
|              |      | map32          | |
| [ ]          | 1..n | fixarray       | Create an array of objects that supports from 0 to 2^32'-1 elements |
|              |      | array16        | |
|              |      | array32        | |
| %s           | 0..n | fixstr         | store a value as string |
| %S           |      |                | store a value as string, but the input is an unicode string |
|              |      | str8           | |
|              |      | str16          | |
|              |      | str32          | |
| %c           | 1    | fixstr [1]     | Store a value as fixstr of 1 byte |
| %n           | 1    | nil            | Store a null pointer, regardless the value placed in variadic arguments |
| %b           | 1    | false or true  | Store a boolean value, the variadic must be a value, 0 for false, any other value is true |
| %p           | 0..n | bin8           | Store a buffer as bin8, bin16 or bin32, but it requires two arguments in variadic, a pointer and a size |
|              |      | bin16          | |
|              |      | bin32          | |
| %f           | 1+4  | float32        | Store a float value |
| %e           | 1+8  | float64        | Store a double value |
| %i           | 1..9 | int8..int64    | Store an integer value, from 8 bit to 64bit |
| %u           | 1..9 | UINT8..UINT64  | Store an unsigned integer value, from 8 to 64 bit. |
| %!           | 1..n |                | A place holder used to fill an object through a callback function |
| null         | 1    | nil            | write nil as value |
| key          | 1..n | fixstr...      | write a key as string |

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
