---
title: strings / json_encode
---
# json_encode

### SYNOPSIS

```c
string json_encode(mixed value);
```

### DESCRIPTION

Encode an LPC value as compact JSON. This is a core driver efun, available
on native and WebAssembly builds without installing an LPC JSON library.

| LPC value | JSON value |
| --- | --- |
| Integer | Number, preserving the full signed 64-bit range |
| Finite float | Number with enough precision to reconstruct the same float |
| String | UTF-8 string with quotes, backslashes and control characters escaped |
| Array | Array |
| Mapping with string keys | Object, with keys sorted for deterministic output |
| Undefined value (`undefinedp(value)`) | `null` |

Ordinary integers `0` and `1` encode as numbers, not JSON booleans. LPC has
no separate boolean type. For example, decoding and re-encoding JSON `true`
produces `1`. Undefined values, including a missing mapping lookup, encode
as `null`; an ordinary zero encodes as `0`.

Objects, functions, buffers, classes and promises are unsupported. Convert
game objects to mappings of their saved properties before encoding them.
Every mapping key, including keys in nested mappings, must be a string.
Unsupported values and non-string keys raise an error instead of silently
replacing or omitting data.

Circular references, non-finite floats, invalid UTF-8, nesting beyond 128
arrays/objects, and output exceeding the runtime `maximum string length`
also raise errors. Reusing the same array or mapping in different branches
is allowed; each occurrence is encoded by value.

### EXAMPLE

```c
mapping room = ([
    "name": "Village Square",
    "exits": ([ "north": "/rooms/tavern" ]),
    "safe": 1
]);
string text = json_encode(room);
mapping restored = json_decode(text);
```

Mudlibs may already define simulated efuns with these names. Use
`efun::json_encode(value)` to explicitly select the driver implementation.
The native implementation is stricter than the testsuite's LPC helper:
it rejects unsupported values, non-string keys and cycles, and does not
round floats to six decimal places.

### MUDLIB INTEGRATION

The codec requires no schema file. A mudlib's existing object-to-mapping
conversion layer can supply the value, provided every nested value satisfies
the table above. Keep blueprint identifiers, record versions and saved fields
in that layer. Decoding the JSON does not clone an object, apply its properties,
write a save file or migrate an existing save format.

Use the preprocessor to require a driver with this efun:

```c
#if !efun_defined(json_encode)
#error This mudlib requires native JSON efuns
#endif
```

Availability depends on the running executable. Rebuild and restart the driver
to add an efun; reloading LPC alone cannot add it. Native launchers, containers
and CI can use separate executables, so update each build's pinned source or
installed binary. The `NO_ADD_ACTION` compile-time option is unrelated to JSON.

### SEE ALSO

[json_decode](json_decode), [save_variable](../general/save_variable),
[undefinedp](../general/undefinedp)
