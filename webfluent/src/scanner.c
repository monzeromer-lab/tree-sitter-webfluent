// What the grammar cannot read by itself: where the end of a line means
// something.
//
// A chunk of raw CSS in a style value runs to the end of its line, a `;` or
// the closing brace, whichever comes first outside parentheses and quotes;
// `$token` and `{expr}` splices inside it are the grammar's own tokens, so a
// chunk stops before `$` and `{`. A line that ends with a comma continues
// on the next, as in the compiler's lexer (`src/lexer/v2.rs`).
//
// In the indented layout a block is the lines indented under the line that
// opens it: where the grammar allows a block, a line indented deeper than
// the line before it is an `_indent`, and a line indented less than the
// innermost indented block closes one block per level it leaves, a
// `_dedent` each. Both are zero-width, at the end of the line before, and
// are only offered where the grammar could take them. Blank lines and
// comment lines do not count. The stack of open indents, the indentation
// of the current line and the count of braces the writer has opened are
// the scanner's state; a line is measured against the line before, not the
// stack, so a braced block's lines — which push nothing — open no block of
// their own, and inside the writer's own braces the layout is free, as it
// is for the compiler.
//
// Because the state only survives with a token, the braces pass through
// the scanner, and `_layout`, an empty extra, records a line at the same
// level. Blanks the scanner skipped without finding a token are the
// parser's again when it declines. A comment is the grammar's own token: a
// line that starts with `/` decides nothing, and the line after it is
// measured once the comment has been read.

#include "tree_sitter/parser.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// One scanner, two grammars: `tree-sitter-webfluentx/src/scanner.c`
// includes this file with `WF_GRAMMAR` set to its own name, so the entry
// points carry the name the generated parser calls.
#ifndef WF_GRAMMAR
#define WF_GRAMMAR webfluent
#endif
#define WF_CAT2(a, b) a##b
#define WF_CAT(a, b) WF_CAT2(a, b)
#define WF_SCANNER(fn) WF_CAT(WF_CAT(tree_sitter_, WF_GRAMMAR), WF_CAT(_external_scanner_, fn))

enum TokenType {
  STYLE_TEXT,
  INDENT,
  DEDENT,
  LAYOUT,
  LBRACE,
  RBRACE,
  LPAREN,
  RPAREN,
  LBRACKET,
  RBRACKET,
  TRANSLATE,
  RAW_STRING,
};

#define MAX_DEPTH 64

typedef struct {
  uint16_t line_indent;
  // Braces, parentheses and brackets the writer wrote and has not closed.
  uint16_t explicit;
  uint16_t depth;
  uint16_t indents[MAX_DEPTH];
} Scanner;

void *WF_SCANNER(create)(void) { return calloc(1, sizeof(Scanner)); }

void WF_SCANNER(destroy)(void *payload) { free(payload); }

#define HEAD (2 * sizeof(uint16_t))

unsigned WF_SCANNER(serialize)(void *payload, char *buffer) {
  Scanner *s = payload;
  unsigned n = HEAD + s->depth * sizeof(uint16_t);
  if (n > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) n = TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
  memcpy(buffer, &s->line_indent, sizeof(uint16_t));
  memcpy(buffer + sizeof(uint16_t), &s->explicit, sizeof(uint16_t));
  memcpy(buffer + HEAD, s->indents, n - HEAD);
  return n;
}

void WF_SCANNER(deserialize)(void *payload, const char *buffer,
                                                        unsigned length) {
  Scanner *s = payload;
  s->line_indent = 0;
  s->explicit = 0;
  s->depth = 0;
  if (length < HEAD) return;
  memcpy(&s->line_indent, buffer, sizeof(uint16_t));
  memcpy(&s->explicit, buffer + sizeof(uint16_t), sizeof(uint16_t));
  s->depth = (length - HEAD) / sizeof(uint16_t);
  if (s->depth > MAX_DEPTH) s->depth = MAX_DEPTH;
  memcpy(s->indents, buffer + HEAD, s->depth * sizeof(uint16_t));
}

static bool is_space(int32_t c) { return c == ' ' || c == '\t' || c == '\r'; }

// A letter, a digit or `-`: what a property's name is made of.
static bool is_word_char(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

// `#"…"#`, `##"…"##`, …: the text up to a quote followed by as many `#` as
// opened it — no escapes, no splices.
static bool scan_raw_string(TSLexer *lexer, bool one_line) {
  unsigned hashes = 0;
  while (lexer->lookahead == '#') {
    lexer->advance(lexer, false);
    hashes++;
  }
  if (hashes == 0 || lexer->lookahead != '"') return false;
  lexer->advance(lexer, false);
  for (;;) {
    if (lexer->lookahead == 0 || (one_line && lexer->lookahead == '\n')) return false;
    if (lexer->lookahead != '"') {
      lexer->advance(lexer, false);
      continue;
    }
    lexer->advance(lexer, false);
    unsigned closing = 0;
    while (closing < hashes && lexer->lookahead == '#') {
      lexer->advance(lexer, false);
      closing++;
    }
    if (closing == hashes) {
      // In error recovery the `#` may have been the first character of an
      // ordinary string (`"#"`), and this quote the opening one of the next
      // (`"#top"`, `"#fff"`): a closing that runs on into a word or a
      // string is not taken there.
      int32_t c = lexer->lookahead;
      if (one_line && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '"' || c == '#' || c == '{' || c >= 0x80)) {
        return false;
      }
      break;
    }
  }
  lexer->mark_end(lexer);
  lexer->result_symbol = RAW_STRING;
  return true;
}

// At a word after two spaces in a style value: whether it is the name of
// the next declaration (`radius-md:`). The word, and the spaces after it,
// are read either way; when it is not a name they are the value's text, and
// `last` is what was read last.
static bool declaration_ahead(TSLexer *lexer, int32_t *last) {
  int32_t c = lexer->lookahead;
  if (!is_word_char(c) || (c >= '0' && c <= '9')) return false;
  while (is_word_char(lexer->lookahead)) {
    *last = lexer->lookahead;
    lexer->advance(lexer, false);
  }
  while (lexer->lookahead == ' ') {
    *last = ' ';
    lexer->advance(lexer, false);
  }
  return lexer->lookahead == ':';
}

// The text read so far, up to where its end was marked.
static bool style_text_so_far(TSLexer *lexer) {
  lexer->result_symbol = STYLE_TEXT;
  return true;
}

static bool scan_style_text(TSLexer *lexer) {
  // Leading blanks belong to nobody; a line break before any text means
  // the value is over.
  unsigned blanks = 0;
  while (is_space(lexer->lookahead)) {
    lexer->advance(lexer, true);
    blanks++;
  }
  if (lexer->lookahead == '\n' || lexer->lookahead == 0) return false;
  if (lexer->lookahead == '{' || lexer->lookahead == '}' || lexer->lookahead == '$' ||
      lexer->lookahead == ';') {
    return false;
  }

  int parens = 0;
  bool consumed = false;
  int32_t last = 0;
  // After a splice, two spaces and the next declaration end the value as
  // they do anywhere in it (see below).
  if (blanks >= 2) {
    if (lexer->lookahead == '&' || lexer->lookahead == '@') return false;
    if (declaration_ahead(lexer, &last)) return false;
    if (last != 0) {
      consumed = true;
      lexer->mark_end(lexer);
    }
  }
  // A comment after a splice (`--accent: {tone}  // follows state`): the
  // value is over, and the comment is the grammar's.
  if (!consumed && lexer->lookahead == '/') {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '/') return false;
    lexer->mark_end(lexer);
    consumed = true;
    last = '/';
  }
  while (lexer->lookahead != 0) {
    int32_t c = lexer->lookahead;
    if (c == '\n') {
      // A trailing comma carries the value onto the next line.
      if (last == ',' || parens > 0) {
        lexer->advance(lexer, false);
        consumed = true;
        continue;
      }
      break;
    }
    if (c == '{' || c == '$') break;
    if (c == '}') break;
    if (c == ';' && parens == 0) break;
    // A comment after a space ends the value; `url(http://…)` keeps its
    // slashes.
    if (c == '/' && parens == 0 && last == ' ') {
      lexer->mark_end(lexer);
      lexer->advance(lexer, false);
      if (lexer->lookahead == '/') return consumed && style_text_so_far(lexer);
      consumed = true;
      last = '/';
      continue;
    }
    if (c == '"' || c == '\'') {
      // A quoted string, whole.
      int32_t quote = c;
      lexer->advance(lexer, false);
      while (lexer->lookahead != 0 && lexer->lookahead != quote && lexer->lookahead != '\n') {
        lexer->advance(lexer, false);
      }
      if (lexer->lookahead == quote) lexer->advance(lexer, false);
      consumed = true;
      last = quote;
      lexer->mark_end(lexer);
      continue;
    }
    // Two spaces and the next declaration — `name:`, `&…` or `@…` — end
    // the value: `color-primary: #0F766E  radius-md: 14px` on one line is
    // two declarations, as it is for the compiler.
    if (c == ' ' && parens == 0) {
      int spaces = 0;
      while (lexer->lookahead == ' ') {
        lexer->advance(lexer, false);
        spaces++;
      }
      consumed = true;
      if (last != ',') last = ' ';
      if (spaces >= 2) {
        int32_t n = lexer->lookahead;
        if (n == '&' || n == '@') return style_text_so_far(lexer);
        if (declaration_ahead(lexer, &last)) return style_text_so_far(lexer);
      }
      lexer->mark_end(lexer);
      continue;
    }
    if (c == '(') parens++;
    if (c == ')' && parens > 0) parens--;
    lexer->advance(lexer, false);
    consumed = true;
    if (!is_space(c)) last = c;
    else if (last != ',') last = ' ';
    lexer->mark_end(lexer);
  }
  if (!consumed) return false;
  lexer->result_symbol = STYLE_TEXT;
  return true;
}

bool WF_SCANNER(scan)(void *payload, TSLexer *lexer,
                                                 const bool *valid_symbols) {
  Scanner *s = payload;
  // Every symbol at once is error recovery: no layout is decided then. A
  // raw string is still read, so one in broken code is not taken apart —
  // but only one that closes on its own line and whose closing runs into
  // no word or string, since here the `#` may sit just inside an ordinary
  // string (`to: "#"`) and the quote after it be the next one's (`"#top"`).
  if (valid_symbols[STYLE_TEXT] && valid_symbols[INDENT] && valid_symbols[DEDENT] &&
      valid_symbols[LBRACE] && valid_symbols[RBRACE]) {
    while (is_space(lexer->lookahead) || lexer->lookahead == '\n') lexer->advance(lexer, true);
    if (valid_symbols[RAW_STRING] && lexer->lookahead == '#') return scan_raw_string(lexer, true);
    return false;
  }
  // A value first: it stops at the end of its line without crossing it,
  // where the layout is then read.
  if (valid_symbols[STYLE_TEXT] && scan_style_text(lexer)) return true;

  // A layout token, if any, is zero-width at the end of the line before.
  lexer->mark_end(lexer);
  bool end_of_line = false;
  uint32_t indent = 0;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == '\n') {
      end_of_line = true;
      indent = 0;
    } else if (c == ' ' || c == '\r') {
      indent++;
    } else if (c == '\t') {
      indent += 4;
    } else if (c == 0) {
      end_of_line = true;
      indent = 0;
      break;
    } else {
      break;
    }
    lexer->advance(lexer, true);
  }
  if (end_of_line && s->explicit == 0 && lexer->lookahead != '/') {
    if (valid_symbols[INDENT] && indent > s->line_indent && s->depth < MAX_DEPTH) {
      s->indents[s->depth++] = (uint16_t)indent;
      s->line_indent = (uint16_t)indent;
      lexer->result_symbol = INDENT;
      return true;
    }
    if (valid_symbols[DEDENT] && s->depth > 0 && indent < s->indents[s->depth - 1]) {
      s->depth--;
      lexer->result_symbol = DEDENT;
      return true;
    }
    if (valid_symbols[LAYOUT] && indent != s->line_indent) {
      s->line_indent = (uint16_t)indent;
      lexer->result_symbol = LAYOUT;
      return true;
    }
  }
  // A brace, parenthesis or bracket the writer wrote: counted, so the
  // layout inside it is free — an argument list may go on over several
  // lines before the indented body it opens.
  static const struct { int32_t open_c, close_c; enum TokenType open, close; } pairs[] = {
      {'{', '}', LBRACE, RBRACE}, {'(', ')', LPAREN, RPAREN}, {'[', ']', LBRACKET, RBRACKET}};
  for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
    if (valid_symbols[pairs[i].open] && lexer->lookahead == pairs[i].open_c) {
      lexer->advance(lexer, false);
      lexer->mark_end(lexer);
      if (s->explicit < UINT16_MAX) s->explicit++;
      lexer->result_symbol = pairs[i].open;
      return true;
    }
  }
  for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
    if (valid_symbols[pairs[i].close] && lexer->lookahead == pairs[i].close_c) {
      lexer->advance(lexer, false);
      lexer->mark_end(lexer);
      if (s->explicit > 0) s->explicit--;
      lexer->result_symbol = pairs[i].close;
      return true;
    }
  }
  if (valid_symbols[RAW_STRING] && lexer->lookahead == '#') return scan_raw_string(lexer, false);
  // `t(` of a translated rule message: one token, its parenthesis counted.
  if (valid_symbols[TRANSLATE] && lexer->lookahead == 't') {
    lexer->advance(lexer, false);
    if (lexer->lookahead != '(') return false;
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    if (s->explicit < UINT16_MAX) s->explicit++;
    lexer->result_symbol = TRANSLATE;
    return true;
  }
  return false;
}
