#include "ryz_script_metadata.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ryz_script_metadata_t parse(const char *source)
{
    ryz_script_metadata_t metadata;
    assert(ryz_script_metadata_parse(source, strlen(source), &metadata) == ESP_OK);
    return metadata;
}

static void expect_empty(const ryz_script_metadata_t *metadata)
{
    assert(metadata->author[0] == '\0' && metadata->version[0] == '\0' &&
           metadata->description[0] == '\0');
    assert(!metadata->author_present && !metadata->author_invalid && !metadata->author_truncated);
    assert(!metadata->version_present && !metadata->version_invalid && !metadata->version_truncated);
    assert(!metadata->description_present && !metadata->description_invalid && !metadata->description_truncated);
}

static void test_grammar(void)
{
    const char source[] =
        "\xef\xbb\xbf\r\n"
        " \t--\t@author:\tRyzoBee Team \t\r\n"
        "-- unrelated leading comment\r\n"
        "-- @unknown: ignored\r\n"
        "\t\r\n"
        "--@version:1.2.3\r\n"
        "-- @description: A small cyberdeck.\r\n"
        "print('app')\n"
        "-- @author: must not replace header\n";
    ryz_script_metadata_t metadata = parse(source);
    assert(strcmp(metadata.author, "RyzoBee Team") == 0 && metadata.author_present);
    assert(strcmp(metadata.version, "1.2.3") == 0 && metadata.version_present);
    assert(strcmp(metadata.description, "A small cyberdeck.") == 0 && metadata.description_present);
    assert(!metadata.author_invalid && !metadata.author_truncated);
    assert(!metadata.version_invalid && !metadata.version_truncated);
    assert(!metadata.description_invalid && !metadata.description_truncated);
    metadata = parse("-- @version:\t \n");
    assert(metadata.version_present && metadata.version[0] == '\0');
    assert(!metadata.version_invalid && !metadata.version_truncated);
    assert(!metadata.author_present && !metadata.description_present);
    metadata = parse("-- @Author: wrong case\n-- @author : wrong colon\n-- @version=wrong format\n");
    expect_empty(&metadata);
}

static void test_header_boundary(void)
{
    const char *ordinary[] = {
        "", "print('ordinary Lua')\n", "local s = [[\n-- @author: in string\n]]\n",
        "print('first code')\n-- @author: too late\n",
        "--[[\n-- @author: in block\n]]\n-- @version: after block\n",
        "--[=[\n-- @author: in block\n]=]\n",
        " \t--[====[\n-- @author: in block\n]====]\n",
        " \xef\xbb\xbf-- @author: BOM not at byte zero\n",
    };
    for (size_t i = 0U; i < sizeof(ordinary) / sizeof(ordinary[0]); ++i) {
        ryz_script_metadata_t metadata = parse(ordinary[i]);
        expect_empty(&metadata);
    }
    ryz_script_metadata_t metadata = parse(
        "-- @author: Before\n--[[\n-- @version: block\n]]\n-- @description: after\n");
    assert(metadata.author_present && strcmp(metadata.author, "Before") == 0);
    assert(!metadata.author_invalid && !metadata.version_present && !metadata.description_present);
    metadata = parse("-- [[ is an ordinary comment\n-- @version: literal\n");
    assert(metadata.version_present && strcmp(metadata.version, "literal") == 0);
    metadata = parse("-- @author: error('never evaluated')\n"
                     "-- @version: not a semver\n"
                     "-- @description: return function() end; \\n <tag>\n");
    assert(strcmp(metadata.author, "error('never evaluated')") == 0);
    assert(strcmp(metadata.version, "not a semver") == 0);
    assert(strcmp(metadata.description, "return function() end; \\n <tag>") == 0);
    assert(!metadata.author_invalid && !metadata.version_invalid && !metadata.description_invalid);
}

static void test_duplicates(void)
{
    ryz_script_metadata_t metadata = parse(
        "-- @author: First\n-- @author: Second\n"
        "-- @version:\n-- @version: Later\n"
        "-- @description: Kept\n-- @description:\n");
    assert(metadata.author_present && metadata.author_invalid && strcmp(metadata.author, "First") == 0);
    assert(metadata.version_present && metadata.version_invalid && metadata.version[0] == '\0');
    assert(metadata.description_present && metadata.description_invalid && strcmp(metadata.description, "Kept") == 0);
    assert(!metadata.author_truncated && !metadata.version_truncated && !metadata.description_truncated);
    metadata = parse("-- @author: bad\tvalue\n-- @author: Repaired?\n-- @version: 1\n");
    assert(metadata.author_present && metadata.author_invalid && metadata.author[0] == '\0');
    assert(!metadata.author_truncated);
    assert(metadata.version_present && !metadata.version_invalid && strcmp(metadata.version, "1") == 0);
    char duplicate_long[160];
    const char prefix[] = "-- @author: ";
    const char duplicate[] = "\n-- @author: must not repair\n";
    const size_t first_length = RYZ_SCRIPT_METADATA_AUTHOR_MAX + 1U;
    memcpy(duplicate_long, prefix, sizeof(prefix) - 1U);
    memset(duplicate_long + sizeof(prefix) - 1U, 'A', first_length);
    memcpy(duplicate_long + sizeof(prefix) - 1U + first_length,
           duplicate, sizeof(duplicate));
    metadata = parse(duplicate_long);
    assert(metadata.author_present && metadata.author_invalid && metadata.author_truncated);
    assert(strlen(metadata.author) == RYZ_SCRIPT_METADATA_AUTHOR_MAX);
}

static ryz_script_metadata_t parse_author_bytes(const unsigned char *bytes, size_t length)
{
    const char prefix[] = "-- @author: ";
    const size_t prefix_length = sizeof(prefix) - 1U;
    char *source = malloc(prefix_length + length);
    assert(source != NULL);
    memcpy(source, prefix, prefix_length);
    memcpy(source + prefix_length, bytes, length);
    ryz_script_metadata_t metadata;
    assert(ryz_script_metadata_parse(source, prefix_length + length, &metadata) == ESP_OK);
    free(source);
    return metadata;
}

static void expect_bad_author(const unsigned char *bytes, size_t length)
{
    ryz_script_metadata_t metadata = parse_author_bytes(bytes, length);
    assert(metadata.author_present && metadata.author_invalid && !metadata.author_truncated);
    assert(metadata.author[0] == '\0');
    assert(!metadata.version_present && !metadata.description_present);
}

static void test_unicode(void)
{
    const char valid[] = "RyzoBee \xc3\xa9 \xe4\xb8\xad \xf0\x9f\x90\x9d";
    ryz_script_metadata_t metadata = parse_author_bytes((const unsigned char *)valid, sizeof(valid) - 1U);
    assert(metadata.author_present && !metadata.author_invalid && !metadata.author_truncated);
    assert(strcmp(metadata.author, valid) == 0);
    static const struct { unsigned char bytes[5]; size_t length; } bad[] = {
        {{0x80}, 1U}, {{0xc0, 0xaf}, 2U}, {{0xc1, 0xbf}, 2U},
        {{0xc2}, 1U}, {{0xe2, 0x82}, 2U}, {{0xf0, 0x9f, 0x90}, 3U},
        {{0xc2, 'A'}, 2U}, {{0xe2, 'A', 0xac}, 3U},
        {{0xe0, 0x80, 0xaf}, 3U}, {{0xed, 0xa0, 0x80}, 3U},
        {{0xf0, 0x80, 0x80, 0xaf}, 4U}, {{0xf4, 0x90, 0x80, 0x80}, 4U},
        {{0xf5, 0x80, 0x80, 0x80}, 4U}, {{0xff}, 1U},
        {{'A', 0, 'B'}, 3U}, {{'A', 1, 'B'}, 3U}, {{'A', '\t', 'B'}, 3U},
        {{'A', '\r', 'B'}, 3U}, {{'A', '\r'}, 2U},
        {{0x7f}, 1U}, {{0xc2, 0x80}, 2U}, {{0xc2, 0x9f}, 2U},
        {{0xe2, 0x80, 0xa8}, 3U}, {{0xe2, 0x80, 0xa9}, 3U},
        {{0xc2, 0xad}, 2U}, {{0xd8, 0x9c}, 2U}, {{0xe1, 0xa0, 0x8e}, 3U},
        {{0xe2, 0x80, 0x8b}, 3U}, {{0xe2, 0x80, 0x8f}, 3U},
        {{0xe2, 0x80, 0xaa}, 3U}, {{0xe2, 0x80, 0xae}, 3U},
        {{0xe2, 0x81, 0xa0}, 3U}, {{0xe2, 0x81, 0xa6}, 3U},
        {{0xe2, 0x81, 0xa9}, 3U}, {{0xe2, 0x81, 0xaf}, 3U},
        {{0xef, 0xbb, 0xbf}, 3U},
    };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
        expect_bad_author(bad[i].bytes, bad[i].length);

    unsigned char long_value[RYZ_SCRIPT_METADATA_AUTHOR_MAX + 5U];
    memset(long_value, 'A', sizeof(long_value));
    metadata = parse_author_bytes(long_value, RYZ_SCRIPT_METADATA_AUTHOR_MAX);
    assert(strlen(metadata.author) == RYZ_SCRIPT_METADATA_AUTHOR_MAX && !metadata.author_truncated);
    metadata = parse_author_bytes(long_value, RYZ_SCRIPT_METADATA_AUTHOR_MAX + 1U);
    assert(strlen(metadata.author) == RYZ_SCRIPT_METADATA_AUTHOR_MAX && metadata.author_truncated);
    assert(!metadata.author_invalid);
    /* 79 ASCII bytes plus a three-byte code point cannot split at byte 80. */
    long_value[RYZ_SCRIPT_METADATA_AUTHOR_MAX - 1U] = 0xe2;
    long_value[RYZ_SCRIPT_METADATA_AUTHOR_MAX] = 0x82;
    long_value[RYZ_SCRIPT_METADATA_AUTHOR_MAX + 1U] = 0xac;
    metadata = parse_author_bytes(long_value, RYZ_SCRIPT_METADATA_AUTHOR_MAX + 2U);
    assert(strlen(metadata.author) == RYZ_SCRIPT_METADATA_AUTHOR_MAX - 1U);
    assert(metadata.author_truncated && !metadata.author_invalid);
    /* A later short ASCII code point cannot be copied around an excluded one. */
    long_value[RYZ_SCRIPT_METADATA_AUTHOR_MAX + 2U] = 'Z';
    metadata = parse_author_bytes(long_value, RYZ_SCRIPT_METADATA_AUTHOR_MAX + 3U);
    assert(strlen(metadata.author) == RYZ_SCRIPT_METADATA_AUTHOR_MAX - 1U);
    assert(metadata.author_truncated && !metadata.author_invalid);
    /* Validation must continue past the visible/truncated prefix. */
    memset(long_value, 'A', sizeof(long_value));
    long_value[sizeof(long_value) - 1U] = 0xff;
    expect_bad_author(long_value, sizeof(long_value));
    long_value[sizeof(long_value) - 1U] = 1U;
    expect_bad_author(long_value, sizeof(long_value));

    char source[512];
    size_t length = 0U;
    const char version_prefix[] = "-- @version: ";
    memcpy(source, version_prefix, sizeof(version_prefix) - 1U);
    length += sizeof(version_prefix) - 1U;
    memset(source + length, 'V', RYZ_SCRIPT_METADATA_VERSION_MAX - 1U);
    length += RYZ_SCRIPT_METADATA_VERSION_MAX - 1U;
    const char description_prefix[] = "\xc3\xa9\n-- @description: ";
    memcpy(source + length, description_prefix, sizeof(description_prefix) - 1U);
    length += sizeof(description_prefix) - 1U;
    memset(source + length, 'D', RYZ_SCRIPT_METADATA_DESCRIPTION_MAX - 1U);
    length += RYZ_SCRIPT_METADATA_DESCRIPTION_MAX - 1U;
    memcpy(source + length, "\xf0\x9f\x90\x9d", 4U);
    length += 4U;
    assert(ryz_script_metadata_parse(source, length, &metadata) == ESP_OK);
    assert(metadata.version_present && metadata.version_truncated && !metadata.version_invalid);
    assert(strlen(metadata.version) == RYZ_SCRIPT_METADATA_VERSION_MAX - 1U);
    assert(metadata.description_present && metadata.description_truncated && !metadata.description_invalid);
    assert(strlen(metadata.description) == RYZ_SCRIPT_METADATA_DESCRIPTION_MAX - 1U);
}

static void test_bounds_and_ownership(void)
{
    ryz_script_metadata_t metadata;
    memset(&metadata, 0xa5, sizeof(metadata));
    assert(ryz_script_metadata_parse(NULL, 0U, &metadata) == ESP_OK);
    expect_empty(&metadata);
    memset(&metadata, 0xa5, sizeof(metadata));
    assert(ryz_script_metadata_parse(NULL, 1U, &metadata) == ESP_ERR_INVALID_ARG);
    expect_empty(&metadata);
    assert(ryz_script_metadata_parse("x", 1U, NULL) == ESP_ERR_INVALID_ARG);
    const char exact[] = "-- @author: Copied";
    char *owned = malloc(sizeof(exact) - 1U);
    assert(owned != NULL);
    memcpy(owned, exact, sizeof(exact) - 1U);
    assert(ryz_script_metadata_parse(owned, sizeof(exact) - 1U, &metadata) == ESP_OK);
    memset(owned, '?', sizeof(exact) - 1U);
    free(owned);
    assert(strcmp(metadata.author, "Copied") == 0 && metadata.author_present);

    const char bounded[] = "-- @author: Visible\n-- @version: hidden";
    assert(ryz_script_metadata_parse(bounded, sizeof("-- @author: Visible\n") - 1U, &metadata) == ESP_OK);
    assert(strcmp(metadata.author, "Visible") == 0 && !metadata.version_present);
    char *maximum = malloc(RYZ_SCRIPT_METADATA_SOURCE_MAX + 1U);
    assert(maximum != NULL);
    memset(maximum, ' ', RYZ_SCRIPT_METADATA_SOURCE_MAX + 1U);
    const char first[] = "-- @author: First\n";
    const char last[] = "-- @version: edge";
    memcpy(maximum, first, sizeof(first) - 1U);
    memcpy(maximum + RYZ_SCRIPT_METADATA_SOURCE_MAX - (sizeof(last) - 1U), last, sizeof(last) - 1U);
    assert(ryz_script_metadata_parse(maximum, RYZ_SCRIPT_METADATA_SOURCE_MAX, &metadata) == ESP_OK);
    assert(strcmp(metadata.author, "First") == 0 && strcmp(metadata.version, "edge") == 0);
    assert(ryz_script_metadata_parse(maximum, RYZ_SCRIPT_METADATA_SOURCE_MAX + 1U, &metadata) ==
           ESP_ERR_INVALID_SIZE);
    expect_empty(&metadata);
    free(maximum);
    assert(ryz_script_metadata_parse("-- @author: stale", 0U, &metadata) == ESP_OK);
    expect_empty(&metadata);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "file") == 0) {
        FILE *file = fopen(argv[2], "rb");
        assert(file != NULL);
        char source[RYZ_SCRIPT_METADATA_SOURCE_MAX + 1U];
        size_t length = fread(source, 1, sizeof(source), file);
        assert(!ferror(file) && length <= RYZ_SCRIPT_METADATA_SOURCE_MAX);
        assert(fclose(file) == 0);
        ryz_script_metadata_t metadata;
        assert(ryz_script_metadata_parse(source, length, &metadata) == ESP_OK);
        assert(metadata.author_present && metadata.author[0] && !metadata.author_invalid && !metadata.author_truncated);
        assert(metadata.version_present && metadata.version[0] && !metadata.version_invalid && !metadata.version_truncated);
        assert(metadata.description_present && metadata.description[0] && !metadata.description_invalid && !metadata.description_truncated);
        printf("SCRIPT_METADATA_PASS file\n");
        return 0;
    }
    assert(argc == 2);
    if (strcmp(argv[1], "grammar") == 0) test_grammar();
    else if (strcmp(argv[1], "boundary") == 0) test_header_boundary();
    else if (strcmp(argv[1], "duplicates") == 0) test_duplicates();
    else if (strcmp(argv[1], "unicode") == 0) test_unicode();
    else if (strcmp(argv[1], "bounds") == 0) test_bounds_and_ownership();
    else { fprintf(stderr, "Unknown metadata case: %s\n", argv[1]); return 2; }
    printf("SCRIPT_METADATA_PASS %s\n", argv[1]);
    return 0;
}
