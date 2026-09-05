/*
 * MD4C: Markdown parser for C
 * (http://github.com/mity/md4c)
 *
 * Copyright (c) 2016-2020 Martin Mitas
 */

#ifndef MD4C_H
#define MD4C_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Parser flags */
#define MD_FLAG_COLLAPSEWHITESPACE          0x0001
#define MD_FLAG_PERMISSIVEATXHEADERS        0x0002
#define MD_FLAG_PERMISSIVEURLAUTOLINKS      0x0004
#define MD_FLAG_PERMISSIVEEMAILAUTOLINKS    0x0008
#define MD_FLAG_NOINDENTEDCODEBLOCKS        0x0010
#define MD_FLAG_NOHTMLBLOCKS                0x0020
#define MD_FLAG_NOHTMLSPANS                 0x0040
#define MD_FLAG_TABLES                      0x0100
#define MD_FLAG_STRIKETHROUGH               0x0200
#define MD_FLAG_PERMISSIVEWWWAUTOLINKS      0x0400
#define MD_FLAG_TASKLISTS                   0x0800
#define MD_FLAG_LATEXMATHSPANS              0x1000
#define MD_FLAG_WIKILINKS                   0x2000
#define MD_FLAG_UNDERLINE                   0x4000

#define MD_FLAG_PERMISSIVEAUTOLINKS         (MD_FLAG_PERMISSIVEEMAILAUTOLINKS | \
                                             MD_FLAG_PERMISSIVEURLAUTOLINKS | \
                                             MD_FLAG_PERMISSIVEWWWAUTOLINKS)

#define MD_FLAG_NOHTML                      (MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS)

#define MD_FLAG_DIALECT_GITHUB              (MD_FLAG_PERMISSIVEAUTOLINKS | \
                                             MD_FLAG_TABLES | \
                                             MD_FLAG_STRIKETHROUGH | \
                                             MD_FLAG_TASKLISTS)

typedef struct MD_PARSER {
    unsigned abi_version;
    unsigned flags;
    int (*enter_block)(int /*type*/, void* /*detail*/, void* /*userdata*/);
    int (*leave_block)(int /*type*/, void* /*detail*/, void* /*userdata*/);
    int (*enter_span)(int /*type*/, void* /*detail*/, void* /*userdata*/);
    int (*leave_span)(int /*type*/, void* /*detail*/, void* /*userdata*/);
    int (*text)(int /*type*/, const char* /*text*/, unsigned /*size*/, void* /*userdata*/);
    void (*debug_log)(const char* /*msg*/, void* /*userdata*/);
    void* syntax;
} MD_PARSER;

int md_parse(const char* text, unsigned size, const MD_PARSER* parser, void* userdata);

#ifdef __cplusplus
}
#endif

#endif  /* MD4C_H */
