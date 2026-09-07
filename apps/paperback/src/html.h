#pragma once
// Hacker News comment HTML (<p>, <i>, <a>, <pre><code>, entities) to reader
// text: paragraphs separated by blank lines, _italics_ kept as underscores so
// the layout engine renders them, code blocks indented so they stay verse.
#include <stddef.h>
#include <stdint.h>

// Appends to out (NUL-terminated, cap bytes). Each output paragraph is prefixed
// with `indent` spaces. Returns the new length.
size_t htmlToText(const char* in, size_t n, char* out, size_t cap, size_t len, int indent);
