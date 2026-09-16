---
title: strings / json_decode
---
# json_decode

### SYNOPSIS

```c
mixed json_decode(string | buffer text);
```

### DESCRIPTION

Parse one complete JSON value from a UTF-8 string or buffer. This is a core
driver efun on native and WebAssembly builds. Buffers are read using their
exact byte length; do not add a NUL terminator.

| JSON value | LPC value |
| --- | --- |
| Object | Mapping with string keys |
| Array | Array |
| String | UTF-8 string, including decoded Unicode escapes and surrogate pairs |
| Integer number | Signed 64-bit integer |
| Number with a decimal point or exponent | Float |
| `true` / `false` | Integer `1` / `0` |
| `null` | Undefined value (`undefinedp(value)` is true) |

Duplicate object keys use the last value. JSON object member order is not
preserved. Floating-point numbers have the precision of an LPC float.
Integer tokens outside the LPC integer range are rejected rather than
rounded into floats. Float overflow and nonzero numbers that underflow to
zero are rejected; representable subnormal floats are accepted.

Leading and trailing JSON whitespace are allowed. Invalid JSON, trailing
non-whitespace, comments, invalid UTF-8, invalid surrogate escapes and
out-of-range numbers raise errors. Raw NUL bytes and `\u0000` escapes in
strings or keys are rejected because LPC strings cannot represent NUL.

Nesting is limited to 128 arrays/objects. The runtime maximum array size,
mapping size and decoded string length also apply. Partially constructed
values are released if parsing fails.

### EXAMPLE

```c
mapping data = json_decode("{\"name\":\"torch\",\"fuel\":42,\"lit\":false}");
// data is ([ "name":"torch", "fuel":42, "lit":0 ])

mixed nothing = json_decode("null");
// undefinedp(nothing) == 1; json_encode(nothing) == "null"
```

LPC has no distinct boolean type, so decoding and re-encoding JSON booleans
produces numeric `1`/`0`. A key containing JSON `null` is present in `keys()`
but its value is undefined; test key membership if you need to distinguish
an explicit null member from an absent member.

Use `efun::json_decode(text)` if the mudlib defines a simulated efun with the
same name. Unlike the testsuite's LPC helper, the native decoder preserves
the distinction between JSON null and ordinary zero and rejects malformed
numbers and unescaped control characters.

### SEE ALSO

[json_encode](json_encode), [restore_variable](../general/restore_variable),
[undefinedp](../general/undefinedp)
