#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_SCRIPT_METADATA_SOURCE_MAX 16384U
#define RYZ_SCRIPT_METADATA_AUTHOR_MAX 80U
#define RYZ_SCRIPT_METADATA_VERSION_MAX 32U
#define RYZ_SCRIPT_METADATA_DESCRIPTION_MAX 256U

typedef struct {
    char author[RYZ_SCRIPT_METADATA_AUTHOR_MAX + 1U];
    char version[RYZ_SCRIPT_METADATA_VERSION_MAX + 1U];
    char description[RYZ_SCRIPT_METADATA_DESCRIPTION_MAX + 1U];
    bool author_present;
    bool author_truncated;
    bool author_invalid;
    bool version_present;
    bool version_truncated;
    bool version_invalid;
    bool description_present;
    bool description_truncated;
    bool description_invalid;
} ryz_script_metadata_t;

/**
 * Parse optional leading Lua line comments, without Lua execution, I/O, heap
 * allocation or retained pointers. Source/output must not overlap. source may
 * be NULL only when length=0; source need not have a trailing NUL.
 *
 * Grammar: [space/tab]-- [space/tab]@author: VALUE (also @version: and
 * @description:). Tags are case-sensitive; no space is allowed before ':'.
 * Zero or more spaces/tabs after '--' and ':' are accepted; value's outer
 * spaces/tabs are trimmed. One UTF-8 BOM is accepted only at source offset 0.
 * LF/CRLF and blank lines are accepted. The first non-line-comment source
 * line, including a Lua long-comment opener --[=[, ends the metadata header.
 * Unknown ordinary line comments are ignored; no Lua strings, escapes or
 * block-comment bodies are interpreted. No continuation or alternate format.
 *
 * Each field's first recognized tag wins, including an empty/invalid first
 * value. present records the tag's existence. A duplicate sets invalid but
 * does not replace the first value. Values must be strict UTF-8; C0/C1/DEL,
 * U+2028/U+2029 and format controls U+00AD, U+061C, U+180E, U+200B..U+200F,
 * U+202A..U+202E, U+2060..U+206F, U+FEFF are rejected. A malformed/control-
 * containing first value is cleared entirely
 * and marked invalid (not truncated). Empty values are present and valid.
 * Valid long values keep the longest complete UTF-8 prefix within MAX bytes
 * and set truncated; the ENTIRE original value is validated before display.
 * Values are untrusted descriptive text, not executable or authenticated data.
 *
 * Returns ESP_OK for absent, malformed or duplicate metadata: field flags
 * describe content problems, never the Lua source's executability. NULL out
 * or nonempty NULL source returns INVALID_ARG; length>SOURCE_MAX returns
 * INVALID_SIZE. Every call with non-NULL out clears it before range checks.
 * Time O(length), constant auxiliary stack, thread-safe/reentrant.
 */
esp_err_t ryz_script_metadata_parse(const char *source, size_t length,
                                    ryz_script_metadata_t *out);

#ifdef __cplusplus
}
#endif
