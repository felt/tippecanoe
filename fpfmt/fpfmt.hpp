// C++ port of Russ Cox's fpfmt shortest-float formatting algorithm,
// from https://github.com/rsc/fpfmt (fpfmt.go).
//
// The original Go code is Copyright 2025 The Go Authors and is covered by
// the BSD-style license in fpfmt/LICENSE.txt.
//
// fpfmt::dtoa() is a drop-in replacement for milo::dtoa_milo(): it produces
// the same output format (JSON-ish shortest round-trippable decimal, with
// "nan", "inf", "-inf" for the non-finite cases), but always chooses the
// genuinely shortest digit string, where Grisu2 sometimes emits one digit
// more than necessary.

#pragma once

#include <stdint.h>

#include <string>

namespace fpfmt {

// shortest computes the shortest decimal d * 10**p that round-trips back to f.
// The caller must have already excluded 0, NaN, and ±Inf. The sign of f is
// ignored; the magnitude is what is formatted.
void shortest(double f, uint64_t *d, int *p);

// digits returns the number of decimal digits in d (d must be nonzero).
int digits(uint64_t d);

// format writes the milo-compatible rendering of (negative ? -1 : 1) * d * 10**p
// into buf, which must have room for at least 32 bytes, and returns the number
// of bytes written. nd must be digits(d).
int format(char *buf, uint64_t d, int p, int nd, bool negative);

// dtoa formats value the way milo::dtoa_milo() did, but using the shortest
// possible digit string.
std::string dtoa(double value);

}  // namespace fpfmt
