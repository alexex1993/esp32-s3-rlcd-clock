// Folding arbitrary text into the alphabet the panel can actually draw.
//
// The Russian faces (u8g2_font_*_t_cyrillic) carry exactly two blocks of code
// points: ASCII 0x20..0x7E, and Cyrillic from 0x0400 — up to 0x04F9 in the
// narrowest of the four, which is where the common floor is. Nothing else at
// all. U8g2 draws nothing, and reports nothing, for a code point it has no
// glyph for, so a headline like
//
//     «Я принципиально не подпишу» — почему вузы теряют 1,4 млрд ₽…
//
// would reach the panel as
//
//     Я принципиально не подпишу  почему вузы теряют 1,4 млрд
//
// with the quotes, the dash, the currency and the ellipsis silently gone, and
// nothing anywhere to say so. Russian newswires — and Russian typing — use all
// four constantly. So every string that comes off the network is folded into
// the alphabet the panel can draw, mapping the punctuation rather than
// dropping it.
//
// This lives in its own module rather than in news.cpp because both screens
// that show foreign text need it, and news.cpp compiles to a stub without an
// API key while the pager does not. Anything new that puts text from outside
// on the panel goes through textSanitise() as well.

#include <Arduino.h>

#include "app.h"

// Latin-1 letters folded to their base ASCII letter, indexed from 0x00C0, so a
// "Türkiye" in a wire story keeps its vowel instead of losing it. '?' marks the
// ones that need more than one letter and are handled below.
static const char kLatin1Fold[] =
    "AAAAAA?CEEEEIIIIDNOOOOOxOUUUUY??aaaaaa?ceeeeiiiidnooooo/ouuuuy?y";

uint32_t textUtf8Next(const char **p) {
  const uint8_t *s = (const uint8_t *)*p;
  uint32_t c = *s++;
  int extra;

  if (c < 0x80) {
    extra = 0;
  } else if ((c & 0xE0) == 0xC0) {
    c &= 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    c &= 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    c &= 0x07;
    extra = 3;
  } else {
    *p = (const char *)s;
    return 0xFFFD;
  }

  for (int i = 0; i < extra; i++) {
    if ((*s & 0xC0) != 0x80) {  // also catches the string's own terminator
      *p = (const char *)s;
      return 0xFFFD;
    }
    c = (c << 6) | (uint32_t)(*s++ & 0x3F);
  }
  *p = (const char *)s;
  return c;
}

void textPutCp(char *out, size_t cap, size_t *len, uint32_t cp) {
  char tmp[3];
  int n;

  if (cp < 0x80) {
    tmp[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    tmp[0] = (char)(0xC0 | (cp >> 6));
    tmp[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else {
    tmp[0] = (char)(0xE0 | (cp >> 12));
    tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    tmp[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  }

  if (*len + (size_t)n + 1 > cap) return;
  memcpy(out + *len, tmp, (size_t)n);
  *len += (size_t)n;
  out[*len] = '\0';
}

static void putStr(char *out, size_t cap, size_t *len, const char *s) {
  while (*s != '\0' && *len + 1 < cap) out[(*len)++] = *s++;
  out[*len] = '\0';
}

// Rewrites one code point into something the panel has a glyph for, appending
// nothing at all when there is no honest stand-in.
static void emitCp(char *out, size_t cap, size_t *len, uint32_t cp) {
  if (cp >= 0x0400 && cp <= 0x04F9) {  // Cyrillic, straight through
    textPutCp(out, cap, len, cp);
    return;
  }

  // Whitespace of every flavour, collapsed to one plain space below. The
  // no-break space matters most: Russian copy sets one between a preposition
  // and its noun, and dropping it would glue the two words together. The
  // newline matters for the pager, where a message is typed by hand and may
  // well have several.
  if (cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x00A0 ||
      (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F) {
    cp = ' ';
  }

  if (cp >= 0x20 && cp < 0x7F) {
    // A run of spaces is one space, and a leading one is nothing.
    if (cp == ' ' && (*len == 0 || out[*len - 1] == ' ')) return;
    textPutCp(out, cap, len, cp);
    return;
  }

  switch (cp) {
    // The guillemets are Russian quotation marks, so they turn up in any
    // headline that names anything.
    case 0x00AB: case 0x00BB: case 0x201C: case 0x201D: case 0x201E:
    case 0x2033:
      putStr(out, cap, len, "\"");
      return;
    case 0x2018: case 0x2019: case 0x201A: case 0x2032:
      putStr(out, cap, len, "'");
      return;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
    case 0x2015: case 0x2212: case 0x2022: case 0x00B7:
      putStr(out, cap, len, "-");
      return;
    case 0x2026:
      putStr(out, cap, len, "...");
      return;
    // The ruble sign is a Р with a bar through it, so the bare letter is both
    // the closest glyph the fonts have and the way it is read aloud.
    case 0x20BD:
      textPutCp(out, cap, len, 0x0420);
      return;
    case 0x20AC: putStr(out, cap, len, "EUR");  return;
    case 0x00A3: putStr(out, cap, len, "GBP");  return;
    case 0x00A5: putStr(out, cap, len, "JPY");  return;
    case 0x2116: putStr(out, cap, len, "N");    return;
    case 0x00D7: putStr(out, cap, len, "x");    return;
    case 0x00C6: putStr(out, cap, len, "AE");   return;
    case 0x00E6: putStr(out, cap, len, "ae");   return;
    case 0x00DF: putStr(out, cap, len, "ss");   return;
    case 0x00DE: putStr(out, cap, len, "Th");   return;
    case 0x00FE: putStr(out, cap, len, "th");   return;
    default: break;
  }

  if (cp >= 0x00C0 && cp <= 0x00FF) {
    const char c = kLatin1Fold[cp - 0x00C0];
    if (c != '?') textPutCp(out, cap, len, (uint32_t)(uint8_t)c);
    return;
  }

  // Everything else is dropped — emoji above all. There is no glyph for them
  // and no sensible substitute.
}

void textSanitise(const char *in, char *out, size_t cap) {
  size_t len = 0;
  out[0] = '\0';
  // The 4 of headroom is the widest thing emitCp can append for one input
  // code point plus its terminator, so the loop never has to unwind a partial
  // write. It is <= rather than <: a name that fills the buffer exactly
  // should arrive whole, not one letter short.
  while (*in != '\0' && len + 4 <= cap) emitCp(out, cap, &len, textUtf8Next(&in));
  while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

// --- JSON strings ---------------------------------------------------------
//
// Both network screens walk their answer byte by byte rather than holding it
// whole, so neither can hand a JSON library a buffer — but both then need the
// same escape handling, `\uXXXX` and surrogate pairs included. The byte source
// is a callback so that this can sit here, above the two transports: news.cpp
// pulls from an HTTPClient stream, telegram.cpp from an mbedTLS session.

bool jsonReadString(TextByteSource src, void *ctx, char *out, size_t cap) {
  size_t len = 0;
  out[0] = '\0';

  for (;;) {
    const int c = src(ctx);
    if (c < 0) return false;
    if (c == '"') return true;

    if (c != '\\') {
      // Raw UTF-8 continuation bytes are copied through as they are; a
      // sequence chopped by cap is left dangling and textUtf8Next() drops it.
      if (len + 2 <= cap) {
        out[len++] = (char)c;
        out[len] = '\0';
      }
      continue;
    }

    const int esc = src(ctx);
    if (esc < 0) return false;

    uint32_t cp;
    switch (esc) {
      case 'n': case 'r': case 't': case 'b': case 'f':
        cp = ' ';
        break;
      case 'u': {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
          const int h = src(ctx);
          if (h < 0) return false;
          if (h >= '0' && h <= '9')      v = (v << 4) | (uint32_t)(h - '0');
          else if (h >= 'a' && h <= 'f') v = (v << 4) | (uint32_t)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') v = (v << 4) | (uint32_t)(h - 'A' + 10);
          else                           v = v << 4;
        }
        if (v >= 0xD800 && v <= 0xDBFF) {
          // The high half of a surrogate pair: an emoji. Swallow the low half
          // so the scan stays aligned, and emit nothing — there is no glyph
          // for anything above the BMP.
          for (int i = 0; i < 6 && src(ctx) >= 0; i++) {
          }
          cp = 0;
        } else if (v >= 0xDC00 && v <= 0xDFFF) {
          cp = 0;  // a stray low half
        } else {
          cp = v;
        }
        break;
      }
      default:  // \" \\ \/ and anything unrecognised
        cp = (uint32_t)esc;
        break;
    }

    if (cp != 0) textPutCp(out, cap, &len, cp);
  }
}
