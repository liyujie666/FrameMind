/*
 * MD4C: Markdown parser for C
 * (http://github.com/mity/md4c)
 *
 * Copyright (c) 2016-2020 Martin Mitas
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#ifndef MD4C_HTML_H
#define MD4C_HTML_H

#include "md4c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render Markdown into HTML.
 *
 * Note only contents of <body> tag is generated. Caller must generate
 * HTML header/footer manually before/after calling md_html().
 *
 * Params input and input_size specify the Markdown input.
 * Callback process_output() gets called with chunks of HTML output.
 * (Typical implementation may just output the bytes to a file or append to
 * some buffer).
 * Param userdata is just propagated back to process_output() callback.
 * Param parser_flags are flags from md4c.h propagated to md_parse().
 * Param render_flags is bitmask of MD_HTML_FLAG_xxxx.
 *
 * Returns -1 on error (if md_parse() fails.)
 * Returns 0 on success.
 */
#define MD_HTML_FLAG_DEBUG              0x0001  /* For development use. */
#define MD_HTML_FLAG_SKIP_UTF8_BOM      0x0002  /* Default is to include UTF-8 byte order mark. */
#define MD_HTML_FLAG_XHTML              0x0004  /* Generate XHTML instead of HTML. */

int md_html(const char* input, unsigned input_size,
            void (*process_output)(const char*, unsigned, void*),
            void* userdata, unsigned parser_flags, unsigned renderer_flags);


#ifdef __cplusplus
}
#endif

#endif  /* MD4C_HTML_H */
