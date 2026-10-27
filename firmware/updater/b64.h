/* Standard base64 (RFC 4648, with padding) — decode only: the firmware never
 * needs to emit a signature. */
#ifndef MOGGIE_B64_H
#define MOGGIE_B64_H

#include <stddef.h>

/* Decodes `inlen` characters into `out`.  Returns the number of bytes written,
 * or -1 on invalid input (bad character, bad padding, output overflow). */
long b64_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap);

#endif /* MOGGIE_B64_H */
