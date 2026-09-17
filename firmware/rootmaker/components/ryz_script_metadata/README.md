# Script metadata

This pure C module reads optional descriptive fields from a Lua script's
leading ordinary line comments. It performs no file access, Lua execution,
allocation, logging or shared-state mutation. Its only Interface is
`ryz_script_metadata_parse(source, length, out)`.

## One supported format

```lua
-- @author: RyzoBee
-- @version: 1.2.0
-- @description: A small device application.

local app = {}
```

For a controlled app, keep its existing `-- ryz-app/1` marker at byte zero
and place these tags on the following comment lines. Likewise, do not move an
existing runtime marker or add a BOM ahead of it: metadata acceptance does
not change the runtime's marker detection or Lua loader behavior.

The tags `@author:`, `@version:` and `@description:` are case-sensitive and
must appear immediately after the comment marker's optional spaces/tabs.
There may be spaces/tabs before `--` and after `:`. Values' leading/trailing
spaces and tabs are trimmed; interior spaces are preserved. Empty values
are permitted. There is no multiline value, escaping, Lua string evaluation,
semantic-version validation, alternate tag spelling or metadata schema alias.

LF and CRLF lines and an initial UTF-8 BOM are supported; a bare CR at EOF is
not a line terminator and is rejected when it occurs in a field. Blank lines may occur
between comments. Unknown ordinary comments are ignored. Parsing stops before
the first source line, including a Lua long-comment opener (`--[[`, `--[=[`,
and so on). It never enters a block comment or resumes after source code.
Consequently a comment-looking line inside a Lua string or block comment is
not metadata. Lua's long-comment opener has no whitespace after `--`;
`-- [[` is an ordinary line comment.

## Result and display safety

The output contains `author[81]`, `version[33]`, and `description[257]`, plus
`present`, `truncated`, and `invalid` flags for each field. Capacities exclude
the terminating NUL and count UTF-8 bytes, not characters or display pixels.

| Input condition | Result |
| --- | --- |
| Tag absent | Empty value; all flags false |
| First tag with empty value | Empty value; present true |
| First well-formed value within capacity | Complete value; present true |
| First well-formed value beyond capacity | Longest complete UTF-8 prefix; present and truncated true |
| First malformed/control-containing value | Empty value; present and invalid true; truncated false |
| Duplicate recognized tag | First result retained; invalid additionally set true |

The first occurrence wins even when it is empty, invalid or truncated. There
is no implicit repair by a later duplicate. No last-writer or concatenation
ambiguity is exposed to callers. A duplicate's value is never displayed.

UTF-8 validation rejects overlong encodings, lone continuation bytes,
incomplete sequences, surrogate values and code points above U+10FFFF. The
entire first value is validated, including bytes beyond display capacity.
It rejects C0/C1/DEL controls, U+2028/U+2029 separators and this explicit set
of bidi/zero-width format controls: U+00AD, U+061C, U+180E, U+200B–U+200F,
U+202A–U+202E, U+2060–U+206F, U+FEFF. A BOM is only syntax at byte zero of
the complete source, never valid field content. Trimming only removes outer
ASCII space/tab; an interior tab is rejected as a control character.

This is not Unicode normalization, a font-coverage guarantee, an authenticity
check, a script validator or an HTML/markup sanitizer. Consumers must render
metadata as untrusted plain text. `invalid` concerns metadata alone and must
not silently change a script's execution policy. No timestamps, permissions,
startup decisions or author identity are inferred.

## Bounds and errors

Source is an immutable, length-delimited buffer of 0–16384 bytes; a terminating
NUL is neither needed nor scanned past. `source == NULL` is permitted only for
length zero. The caller provides non-overlapping output storage. Every call
with a non-NULL output clears it before argument/range checks. NULL output or
nonempty NULL input returns `ESP_ERR_INVALID_ARG`; oversized input returns
`ESP_ERR_INVALID_SIZE`. Content problems are returned as field flags with
`ESP_OK`, not as source execution errors.

The parser uses O(length) time, constant auxiliary stack, no heap and no
platform operations. It retains no input pointers and is reentrant. Its
result owns the copied values and remains valid after the source is released.

No Store, Workbench, UI, boot policy or device configuration is changed by
this module. Integration and real-device rendering are separate work.
