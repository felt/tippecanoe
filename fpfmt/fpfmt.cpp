// C++ port of Russ Cox's fpfmt shortest-float formatting algorithm,
// from https://github.com/rsc/fpfmt (fpfmt.go).
//
// The original Go code is Copyright 2025 The Go Authors and is covered by
// the BSD-style license in fpfmt/LICENSE.txt.
//
// The port is deliberately literal: the functions, their names, and the
// comments follow fpfmt.go closely, so the two can be diffed. The only
// substantive additions are the Go-compatible shift helpers (Go defines a
// shift of 64 or more as 0, where C++ leaves it undefined) and format(),
// which renders the (d, p) pair the way milo::Prettify() does.

#include "fpfmt.hpp"

#include <string.h>

#include <cmath>

#include "pow10tab.hpp"

namespace fpfmt {

static_assert((-1 >> 1) == -1, "fpfmt needs an arithmetic right shift on signed ints");

// go_shl and go_shr shift the way Go does: a shift count of 64 or more
// yields 0 rather than undefined behavior. The Short() code below relies on
// this for subnormals, where the neighbor offset shifts entirely away.
static inline uint64_t go_shl(uint64_t x, int s) {
	return s >= 64 ? 0 : x << s;
}

static inline uint64_t go_shr(uint64_t x, int s) {
	return s >= 64 ? 0 : x >> s;
}

// len64 returns the minimum number of bits required to represent x;
// it is 0 for x == 0. (Go's bits.Len64.)
static inline int len64(uint64_t x) {
	return x == 0 ? 0 : 64 - __builtin_clzll(x);
}

// mul64 returns the 128-bit product of x and y as hi, lo. (Go's bits.Mul64.)
static inline void mul64(uint64_t x, uint64_t y, uint64_t *hi, uint64_t *lo) {
	unsigned __int128 p = (unsigned __int128) x * (unsigned __int128) y;
	*hi = (uint64_t) (p >> 64);
	*lo = (uint64_t) p;
}

// mul64hi returns just the high half of the 128-bit product of x and y.
static inline uint64_t mul64hi(uint64_t x, uint64_t y) {
	return (uint64_t) (((unsigned __int128) x * (unsigned __int128) y) >> 64);
}

// ror64 rotates x right by k bits. (Go's bits.RotateLeft64(x, -k).)
static inline uint64_t ror64(uint64_t x, int k) {
	return (x >> k) | (x << (64 - k));
}

// unpack64 returns m, e such that f = m * 2**e.
// The caller is expected to have handled 0, NaN, and ±Inf already.
// The sign of f is ignored.
static inline void unpack64(double f, uint64_t *mp, int *ep) {
	const int shift = 64 - 53;
	const int minExp = -(1074 + shift);

	uint64_t b;
	memcpy(&b, &f, sizeof(b));

	uint64_t m = (1ULL << 63) | ((b & ((1ULL << 52) - 1)) << shift);
	int e = (int) ((b >> 52) & ((1ULL << shift) - 1));
	if (e == 0) {
		m &= ~(1ULL << 63);
		e = minExp;
		int s = 64 - len64(m);
		*mp = m << s;
		*ep = e - s;
		return;
	}
	*mp = m;
	*ep = (e - 1) + minExp;
}

// An unrounded represents an unrounded value: the top 62 bits are the value
// scaled by 4, the low bit is a sticky bit recording a nonzero remainder.
typedef uint64_t unrounded;

static inline uint64_t un_floor(unrounded u) {
	return (u + 0) >> 2;
}

static inline uint64_t un_round(unrounded u) {
	return (u + 1 + ((u >> 2) & 1)) >> 2;
}

static inline uint64_t un_ceil(unrounded u) {
	return (u + 3) >> 2;
}

static inline unrounded un_nudge(unrounded u, int delta) {
	return u + (unrounded) (int64_t) delta;
}

static inline unrounded un_div(unrounded u, uint64_t d) {
	uint64_t x = u;
	return (x / d) | (u & 1) | (uint64_t) (x % d != 0);
}

// log10_pow2(x) returns ⌊log₁₀ 2**x⌋ = ⌊x * log₁₀ 2⌋.
static inline int log10_pow2(int x) {
	// log₁₀ 2 ≈ 0.30102999566 ≈ 78913 / 2^18
	return (int) (((int64_t) x * 78913) >> 18);
}

// log2_pow10(x) returns ⌊log₂ 10**x⌋ = ⌊x * log₂ 10⌋.
static inline int log2_pow10(int x) {
	// log₂ 10 ≈ 3.32192809489 ≈ 108853 / 2^15
	return (int) (((int64_t) x * 108853) >> 15);
}

// skewed computes the skewed footprint of m * 2**e,
// which is ⌊log₁₀ 3/4 * 2**e⌋ = ⌊e*(log₁₀ 2)-(log₁₀ 4/3)⌋.
static inline int skewed(int e) {
	return (int) (((int64_t) e * 631305 - 261663) >> 21);
}

// uint64pow10[x] is 10**x.
static const uint64_t uint64pow10[20] = {
	1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL,
	100000ULL, 1000000ULL, 10000000ULL, 100000000ULL, 1000000000ULL,
	10000000000ULL, 100000000000ULL, 1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
	1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL, 1000000000000000000ULL,
	10000000000000000000ULL};

// A scaler holds derived scaling constants for a given e, p pair.
struct scaler {
	pm_hi_lo pm;
	int s;
};

// prescale returns the scaling constants for e, p.
// lp must be log2_pow10(p).
static inline scaler prescale(int e, int p, int lp) {
	scaler c;
	c.pm = pow10_tab[p - pow10Min];
	c.s = -(e + lp + 3);
	return c;
}

// uscale returns unround(x * 2**e * 10**p).
// The caller should pass c = prescale(e, p, log2_pow10(p))
// and should have left-justified x so its high bit is set.
static inline unrounded uscale(uint64_t x, scaler c) {
	uint64_t hi, mid;
	mul64(x, c.pm.hi, &hi, &mid);
	uint64_t sticky = 1;
	if ((hi & ((1ULL << (c.s & 63)) - 1)) == 0) {
		uint64_t mid2 = mul64hi(x, c.pm.lo);
		sticky = (uint64_t) (mid - mid2 > 1);
		hi -= (uint64_t) (mid < mid2);
	}
	return go_shr(hi, c.s) | sticky;
}

// trim_zeros removes trailing zeros from x * 10**p.
// If x ends in k zeros, trim_zeros returns x/10**k, p+k.
// It assumes that x ends in at most 16 zeros.
static inline void trim_zeros(uint64_t *xp, int *pp) {
	const uint64_t maxUint64 = ~(uint64_t) 0;
	const uint64_t inv5p8 = 0xc767074b22e90e21ULL;	// inverse of 5**8
	const uint64_t inv5p4 = 0xd288ce703afb7e91ULL;	// inverse of 5**4
	const uint64_t inv5p2 = 0x8f5c28f5c28f5c29ULL;	// inverse of 5**2
	const uint64_t inv5 = 0xcccccccccccccccdULL;	// inverse of 5

	uint64_t x = *xp;
	int p = *pp;

	// Cut 1 zero, or else return.
	uint64_t d = ror64(x * inv5, 1);
	if (d <= maxUint64 / 10) {
		x = d;
		p += 1;
	} else {
		*xp = x;
		*pp = p;
		return;
	}

	// Cut 8 zeros, then 4, then 2, then 1.
	d = ror64(x * inv5p8, 8);
	if (d <= maxUint64 / 100000000) {
		x = d;
		p += 8;
	}
	d = ror64(x * inv5p4, 4);
	if (d <= maxUint64 / 10000) {
		x = d;
		p += 4;
	}
	d = ror64(x * inv5p2, 2);
	if (d <= maxUint64 / 100) {
		x = d;
		p += 2;
	}
	d = ror64(x * inv5, 1);
	if (d <= maxUint64 / 10) {
		x = d;
		p += 1;
	}

	*xp = x;
	*pp = p;
}

// shortest computes the shortest formatting of f,
// using as few digits as possible that will still round trip
// back to the original float64.
void shortest(double f, uint64_t *dp, int *pp) {
	const int minExp = -1085;

	uint64_t m;
	int e;
	unpack64(f, &m, &e);

	uint64_t mn;
	int p;
	int z = 11;  // extra zero bits at bottom of m; 11 for 53-bit m
	if (m == (1ULL << 63) && e > minExp) {
		p = -skewed(e + z);
		mn = m - go_shl(1, z - 2);  // mn = m - 1/4 * 2**(e+z)
	} else {
		if (e < minExp) {
			z = 11 + (minExp - e);
		}
		p = -log10_pow2(e + z);
		mn = m - go_shl(1, z - 1);  // mn = m - 1/2 * 2**(e+z)
	}
	uint64_t mx = m + go_shl(1, z - 1);  // mx = m + 1/2 * 2**(e+z)
	int odd = (int) (go_shr(m, z) & 1);

	scaler pre = prescale(e, p, log2_pow10(p));
	uint64_t dmin = un_ceil(un_nudge(uscale(mn, pre), +odd));
	uint64_t dmax = un_floor(un_nudge(uscale(mx, pre), -odd));

	uint64_t d = dmax / 10;
	if (d * 10 >= dmin) {
		int q = -(p - 1);
		trim_zeros(&d, &q);
		*dp = d;
		*pp = q;
		return;
	}
	d = dmin;
	if (d < dmax) {
		d = un_round(uscale(m, pre));
	}
	*dp = d;
	*pp = -p;
}

// digits returns the number of decimal digits in d.
int digits(uint64_t d) {
	int nd = log10_pow2(len64(d));
	return nd + (int) (d >= uint64pow10[nd]);
}

// i2a is the formatting of 00..99 concatenated,
// a lookup table for formatting [0, 99].
static const char i2a[201] =
	"00010203040506070809"
	"10111213141516171819"
	"20212223242526272829"
	"30313233343536373839"
	"40414243444546474849"
	"50515253545556575859"
	"60616263646566676869"
	"70717273747576777879"
	"80818283848586878889"
	"90919293949596979899";

// format_base10 formats the decimal representation of u into the nd bytes
// at a. The caller is responsible for ensuring that nd is big enough to hold
// u. If nd is too big, leading zeros will be filled in as needed.
static inline void format_base10(char *a, int nd, uint64_t u) {
	while (nd >= 8) {
		// Format last 8 digits (4 pairs).
		uint32_t x3210 = (uint32_t) (u % 100000000);
		u /= 100000000;
		uint32_t x32 = x3210 / 10000, x10 = x3210 % 10000;
		uint32_t x1 = (x10 / 100) * 2, x0 = (x10 % 100) * 2;
		uint32_t x3 = (x32 / 100) * 2, x2 = (x32 % 100) * 2;
		a[nd - 1] = i2a[x0 + 1];
		a[nd - 2] = i2a[x0];
		a[nd - 3] = i2a[x1 + 1];
		a[nd - 4] = i2a[x1];
		a[nd - 5] = i2a[x2 + 1];
		a[nd - 6] = i2a[x2];
		a[nd - 7] = i2a[x3 + 1];
		a[nd - 8] = i2a[x3];
		nd -= 8;
	}

	uint32_t x = (uint32_t) u;
	if (nd >= 4) {
		// Format last 4 digits (2 pairs).
		uint32_t x10 = x % 10000;
		x /= 10000;
		uint32_t x1 = (x10 / 100) * 2, x0 = (x10 % 100) * 2;
		a[nd - 1] = i2a[x0 + 1];
		a[nd - 2] = i2a[x0];
		a[nd - 3] = i2a[x1 + 1];
		a[nd - 4] = i2a[x1];
		nd -= 4;
	}
	if (nd >= 2) {
		// Format last 2 digits.
		uint32_t x0 = (x % 100) * 2;
		x /= 100;
		a[nd - 1] = i2a[x0 + 1];
		a[nd - 2] = i2a[x0];
		nd -= 2;
	}
	if (nd > 0) {
		// Format final digit.
		a[0] = (char) ('0' + x);
	}
}

// write_exponent appends the exponent k the way milo::WriteExponent() does:
// a sign, then one, two, or three digits with no leading zero padding.
static inline int write_exponent(char *s, int k) {
	int n = 0;
	if (k < 0) {
		s[n++] = '-';
		k = -k;
	} else {
		s[n++] = '+';
	}

	if (k >= 100) {
		s[n++] = (char) ('0' + k / 100);
		k %= 100;
		s[n++] = i2a[k * 2];
		s[n++] = i2a[k * 2 + 1];
	} else if (k >= 10) {
		s[n++] = i2a[k * 2];
		s[n++] = i2a[k * 2 + 1];
	} else {
		s[n++] = (char) ('0' + k);
	}
	return n;
}

// format renders d * 10**p, negated if negative, choosing between plain and
// exponential notation exactly as milo::Prettify() does.
int format(char *buf, uint64_t d, int p, int nd, bool negative) {
	char *s = buf;
	if (negative) {
		*s++ = '-';
	}

	const int kk = nd + p;	// 10^(kk-1) <= v < 10^kk

	if (nd <= kk && kk <= 21) {
		// 1234e7 -> 12340000000
		format_base10(s, nd, d);
		memset(s + nd, '0', (size_t) (kk - nd));
		return (int) (s - buf) + kk;
	}

	if (0 < kk && kk <= 21) {
		// 1234e-2 -> 12.34
		// Lay the digits down one byte high, then slide the integer part
		// back down over the gap, which leaves room for the point.
		format_base10(s + 1, nd, d);
		memmove(s, s + 1, (size_t) kk);
		s[kk] = '.';
		return (int) (s - buf) + nd + 1;
	}

	if (-6 < kk && kk <= 0) {
		// 1234e-6 -> 0.001234
		s[0] = '0';
		s[1] = '.';
		memset(s + 2, '0', (size_t) -kk);
		format_base10(s + 2 - kk, nd, d);
		return (int) (s - buf) + 2 - kk + nd;
	}

	int n;
	if (nd == 1) {
		// 1e30
		s[0] = (char) ('0' + d);
		n = 1;
	} else {
		// 1234e30 -> 1.234e33
		format_base10(s + 1, nd, d);
		s[0] = s[1];
		s[1] = '.';
		n = nd + 1;
	}
	s[n++] = 'e';
	n += write_exponent(s + n, kk - 1);
	return (int) (s - buf) + n;
}

std::string dtoa(double value) {
	if (std::isnan(value)) {
		return "nan";
	}
	if (std::isinf(value)) {
		if (value < 0) {
			return "-inf";
		} else {
			return "inf";
		}
	}
	if (value == 0) {
		return "0";
	}

	uint64_t d;
	int p;
	shortest(value, &d, &p);

	char buf[32];
	int n = format(buf, d, p, digits(d), value < 0);
	return std::string(buf, (size_t) n);
}

}  // namespace fpfmt
