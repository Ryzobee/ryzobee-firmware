#include "ryz_script_metadata.h"

#include <stdint.h>
#include <string.h>

static bool horizontal_space(unsigned char byte)
{
    return byte == ' ' || byte == '\t';
}

/* Strict scalar decoding; never examines a byte outside the caller's slice. */
static size_t decode_utf8(const unsigned char *bytes, size_t length,
                          uint32_t *out_codepoint)
{
    if (length == 0U) {
        return 0U;
    }
    unsigned char first = bytes[0];
    if (first < 0x80U) {
        *out_codepoint = first;
        return 1U;
    }

    size_t width;
    uint32_t codepoint;
    if (first >= 0xc2U && first <= 0xdfU) {
        width = 2U;
        codepoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
        width = 3U;
        codepoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        width = 4U;
        codepoint = first & 0x07U;
    } else {
        return 0U;
    }
    if (length < width) {
        return 0U;
    }
    for (size_t i = 1U; i < width; ++i) {
        if ((bytes[i] & 0xc0U) != 0x80U) {
            return 0U;
        }
        codepoint = (codepoint << 6U) | (bytes[i] & 0x3fU);
    }
    if ((width == 2U && codepoint < 0x80U) ||
        (width == 3U && codepoint < 0x800U) ||
        (width == 4U && codepoint < 0x10000U) ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
        codepoint > 0x10ffffU) {
        return 0U;
    }
    *out_codepoint = codepoint;
    return width;
}

static bool prohibited_display_codepoint(uint32_t codepoint)
{
    return codepoint < 0x20U ||
           (codepoint >= 0x7fU && codepoint <= 0x9fU) ||
           codepoint == 0x00adU || codepoint == 0x061cU ||
           codepoint == 0x180eU ||
           (codepoint >= 0x200bU && codepoint <= 0x200fU) ||
           (codepoint >= 0x2028U && codepoint <= 0x202eU) ||
           (codepoint >= 0x2060U && codepoint <= 0x206fU) ||
           codepoint == 0xfeffU;
}

static void read_value(const unsigned char *value, size_t length,
                       char *destination, size_t capacity, bool *present,
                       bool *truncated, bool *invalid)
{
    if (*present) {
        *invalid = true;
        return;
    }
    *present = true;
    while (length != 0U && horizontal_space(value[0])) {
        ++value;
        --length;
    }
    while (length != 0U && horizontal_space(value[length - 1U])) {
        --length;
    }

    size_t prefix = 0U;
    bool exceeds_capacity = false;
    for (size_t position = 0U; position < length;) {
        uint32_t codepoint;
        size_t width = decode_utf8(value + position, length - position,
                                   &codepoint);
        if (width == 0U || prohibited_display_codepoint(codepoint)) {
            /* Nothing is copied until the entire value has been validated. */
            *invalid = true;
            return;
        }
        if (!exceeds_capacity && width <= capacity - prefix) {
            prefix += width;
        } else {
            exceeds_capacity = true;
        }
        position += width;
    }
    if (prefix != 0U) {
        memcpy(destination, value, prefix);
    }
    destination[prefix] = '\0';
    *truncated = exceeds_capacity;
}

static bool begins_with(const unsigned char *text, size_t length,
                         const char *prefix, size_t prefix_length)
{
    return length >= prefix_length && memcmp(text, prefix, prefix_length) == 0;
}

static void read_comment(const unsigned char *comment, size_t length,
                         ryz_script_metadata_t *out)
{
    while (length != 0U && horizontal_space(comment[0])) {
        ++comment;
        --length;
    }
    if (begins_with(comment, length, "@author:", 8U)) {
        read_value(comment + 8U, length - 8U, out->author,
                   RYZ_SCRIPT_METADATA_AUTHOR_MAX, &out->author_present,
                   &out->author_truncated, &out->author_invalid);
    } else if (begins_with(comment, length, "@version:", 9U)) {
        read_value(comment + 9U, length - 9U, out->version,
                   RYZ_SCRIPT_METADATA_VERSION_MAX, &out->version_present,
                   &out->version_truncated, &out->version_invalid);
    } else if (begins_with(comment, length, "@description:", 13U)) {
        read_value(comment + 13U, length - 13U, out->description,
                   RYZ_SCRIPT_METADATA_DESCRIPTION_MAX,
                   &out->description_present, &out->description_truncated,
                   &out->description_invalid);
    }
}

static bool begins_long_comment(const unsigned char *comment, size_t length)
{
    if (length == 0U || comment[0] != '[') {
        return false;
    }
    size_t position = 1U;
    while (position < length && comment[position] == '=') {
        ++position;
    }
    return position < length && comment[position] == '[';
}

esp_err_t ryz_script_metadata_parse(const char *source, size_t length,
                                    ryz_script_metadata_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (source == NULL && length != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > RYZ_SCRIPT_METADATA_SOURCE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (length == 0U) {
        return ESP_OK;
    }

    const unsigned char *bytes = (const unsigned char *)source;
    size_t position = 0U;
    if (length >= 3U && bytes[0] == 0xefU && bytes[1] == 0xbbU &&
        bytes[2] == 0xbfU) {
        position = 3U;
    }
    while (position < length) {
        size_t start = position;
        while (position < length && bytes[position] != '\n') {
            ++position;
        }
        size_t end = position;
        if (position < length) {
            ++position;
            if (end > start && bytes[end - 1U] == '\r') {
                --end;
            }
        }
        while (start < end && horizontal_space(bytes[start])) {
            ++start;
        }
        if (start == end) {
            continue;
        }
        if (end - start < 2U || bytes[start] != '-' ||
            bytes[start + 1U] != '-') {
            break;
        }
        start += 2U;
        if (begins_long_comment(bytes + start, end - start)) {
            break;
        }
        read_comment(bytes + start, end - start, out);
    }
    return ESP_OK;
}
