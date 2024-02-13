#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include "text.hpp"
#include "milo/dtoa_milo.h"
#include "milo/milo.h"
#include "errors.hpp"

/**
 * Returns an empty string if `s` is valid utf8;
 * otherwise returns an error message.
 */
std::string check_utf8(std::string s) {
	for (size_t i = 0; i < s.size(); i++) {
		size_t fail = 0;

		if ((s[i] & 0x80) == 0x80) {
			if ((s[i] & 0xE0) == 0xC0) {
				if (i + 1 >= s.size() || (s[i + 1] & 0xC0) != 0x80) {
					fail = 2;
				} else {
					i += 1;
				}
			} else if ((s[i] & 0xF0) == 0xE0) {
				if (i + 2 >= s.size() || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) {
					fail = 3;
				} else {
					i += 2;
				}
			} else if ((s[i] & 0xF8) == 0xF0) {
				if (i + 3 >= s.size() || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80) {
					fail = 4;
				} else {
					i += 3;
				}
			} else {
				fail = 1;
			}
		}

		if (fail != 0) {
			std::string out = "\"" + s + "\" is not valid UTF-8 (";
			for (size_t j = 0; j < fail && i + j < s.size(); j++) {
				if (j != 0) {
					out += " ";
				}
				char tmp[6];
				snprintf(tmp, sizeof(tmp), "0x%02X", s[i + j] & 0xFF);
				out += std::string(tmp);
			}
			out += ")";
			return out;
		}
	}

	return "";
}

const char *utf8_next(const char *s, long *c) {
	if (s == NULL) {
		*c = -1;
		return NULL;
	}

	if (*s == '\0') {
		*c = -1;
		return NULL;
	}

	if ((s[0] & 0x80) == 0x80) {
		if ((s[0] & 0xE0) == 0xC0) {
			if ((s[1] & 0xC0) != 0x80) {
				*c = 0xFFFD;
				s++;
			} else {
				*c = ((long) (s[0] & 0x1F) << 6) | ((long) (s[1] & 0x7F));
				s += 2;
			}
		} else if ((s[0] & 0xF0) == 0xE0) {
			if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
				*c = 0xFFFD;
				s++;
			} else {
				*c = ((long) (s[0] & 0x0F) << 12) | ((long) (s[1] & 0x7F) << 6) | ((long) (s[2] & 0x7F));
				s += 3;
			}
		} else if ((s[0] & 0xF8) == 0xF0) {
			if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
				*c = 0xFFFD;
				s++;
			} else {
				*c = ((long) (s[0] & 0x0F) << 18) | ((long) (s[1] & 0x7F) << 12) | ((long) (s[2] & 0x7F) << 6) | ((long) (s[3] & 0x7F));
				s += 4;
			}
		} else {
			*c = 0xFFFD;
			s++;
		}
	} else {
		*c = s[0];
		s++;
	}

	return s;
}

std::string truncate16(std::string const &s, size_t runes) {
	const char *cp = s.c_str();
	const char *start = cp;
	const char *lastgood = cp;
	size_t len = 0;
	long c;

	while ((cp = utf8_next(cp, &c)) != NULL) {
		if (c <= 0xFFFF) {
			len++;
		} else {
			len += 2;
		}

		if (len <= runes) {
			lastgood = cp;
		} else {
			break;
		}
	}

	return std::string(s, 0, lastgood - start);
}

int integer_zoom(std::string where, std::string text) {
	double d = atof(text.c_str());
	if (!std::isfinite(d) || d != floor(d) || d < 0 || d > 32) {
		fprintf(stderr, "%s: Expected integer zoom level in \"tippecanoe\" GeoJSON extension, not %s\n", where.c_str(), text.c_str());
		exit(EXIT_JSON);
	}
	return d;
}

std::string format_commandline(int argc, char **argv) {
	std::string out;

	for (int i = 0; i < argc; i++) {
		bool need_quote = false;
		for (char *cp = argv[i]; *cp != '\0'; cp++) {
			if (!isalpha(*cp) && !isdigit(*cp) &&
			    *cp != '/' && *cp != '-' && *cp != '_' && *cp != '@' && *cp != ':' &&
			    *cp != '.' && *cp != '%' && *cp != ',') {
				need_quote = true;
				break;
			}
		}

		if (need_quote) {
			out.push_back('\'');
			for (char *cp = argv[i]; *cp != '\0'; cp++) {
				if (*cp == '\'') {
					out.append("'\"'\"'");
				} else {
					out.push_back(*cp);
				}
			}
			out.push_back('\'');
		} else {
			out.append(argv[i]);
		}

		if (i + 1 < argc) {
			out.push_back(' ');
		}
	}

	return out;
}

// for jsonpull to call from C
char *dtoa_milo(double val) {
	std::string s = milo::dtoa_milo(val);
	char *dup = strdup(s.c_str());
	if (dup == NULL) {
		perror("strdup");
		exit(EXIT_MEMORY);
	}
	return dup;
}

// to work with data from https://github.com/kmike/text-unidecode
std::vector<std::string> read_unidecode(const char *fname) {
	std::string data;

	FILE *f = fopen(fname, "rb");
	if (f == NULL) {
		perror(fname);
		exit(EXIT_OPEN);
	}

	std::string buf;
	buf.resize(2000);

	while (true) {
		size_t nread = fread((void *) buf.c_str(), sizeof(char), buf.size(), f);
		if (nread == 0) {
			break;
		}
		data.append(buf.c_str(), nread);
	}

	fclose(f);

	std::vector<std::string> out;
	out.emplace_back();  // because the data file is 1-indexed
	out.emplace_back();  // ascii 001

	for (size_t i = 0; i < data.size(); i++) {
		if (data[i] == '\0') {
			out.emplace_back();
		} else {
			if (data[i] >= '\0' && data[i] <= '~') {
				data[i] = tolower(data[i]);
			}
			out.back().push_back(data[i]);
		}
	}

	return out;
}

std::string unidecode_smash(std::vector<std::string> const &unidecode_data, const char *s) {
	if (unidecode_data.size() == 0) {
		return s;
	}

	std::string out;
	out.reserve(strlen(s));

	long c;
	while (true) {
		const char *os = s;
		s = utf8_next(s, &c);
		if (s == NULL) {
			break;
		}

		if (c >= 0 && c < (long) unidecode_data.size()) {
			out.append(unidecode_data[c]);
		} else {
			// pass through anything that is out of unidecode range literally
			for (; os != s; os++) {
				out.push_back(*os);
			}
		}
	}

	return out;
}

unsigned long long fnv1a(std::string const &s) {
	// Store tiles by a hash of their contents (fnv1a 64-bit)
	// http://www.isthe.com/chongo/tech/comp/fnv/
	const unsigned long long fnv_offset_basis = 14695981039346656037u;
	const unsigned long long fnv_prime = 1099511628211u;
	unsigned long long h = fnv_offset_basis;
	for (size_t i = 0; i < s.size(); i++) {
		h ^= (unsigned char) s[i];
		h *= fnv_prime;
	}
	return h;
}

// The "additional" is to make it easier to hash a serial_val attribute value and type together
unsigned long long fnv1a(const char *s, char additional) {
	// http://www.isthe.com/chongo/tech/comp/fnv/
	const unsigned long long fnv_offset_basis = 14695981039346656037u;
	const unsigned long long fnv_prime = 1099511628211u;
	unsigned long long h = fnv_offset_basis;
	for (size_t i = 0; s[i] != '\0'; i++) {
		h ^= (unsigned char) s[i];
		h *= fnv_prime;
	}
	h ^= (unsigned char) additional;
	h *= fnv_prime;
	return h;
}

unsigned long long fnv1a(size_t size, void *p) {
	// http://www.isthe.com/chongo/tech/comp/fnv/
	unsigned char *s = (unsigned char *) p;
	const unsigned long long fnv_offset_basis = 14695981039346656037u;
	const unsigned long long fnv_prime = 1099511628211u;
	unsigned long long h = fnv_offset_basis;
	for (size_t i = 0; i < size; i++) {
		h ^= (unsigned char) s[i];
		h *= fnv_prime;
	}
	return h;
}

// This function reverses the order of the bits in a 64-bit word.
// Instead of shifting individual bits in a loop, it shifts them
// in blocks, starting with swapping the halfwords, and working downward
// until it is swapping individual pairs of adjacent bits.
//
// The purpose is to permute the order in which features are visited:
// instead of working in an orderly fashion from the top left to the
// bottom right of the tile, instead jump around to minimize adjacency,
// like a hash function, but taking advantage of the knowledge that we
// are operating on a fixed-size input that can be directly inverted.
// https://en.wikipedia.org/wiki/Bit-reversal_permutation
//
// This allows calculating an appropriate set of features to appear
// at a fractional zoom level: at what is effectively z4.25, for example,
// we can bring in a quarter of the features that will be added in the
// transition from z4 to z5, and have them be spatially distributed
// across the tile rather than clumped together.

unsigned long long bit_reverse(unsigned long long v) {
	v = ((v & 0x00000000FFFFFFFF) << 32) | ((v & 0xFFFFFFFF00000000) >> 32);
	v = ((v & 0x0000FFFF0000FFFF) << 16) | ((v & 0xFFFF0000FFFF0000) >> 16);
	v = ((v & 0x00FF00FF00FF00FF) << 8) | ((v & 0xFF00FF00FF00FF00) >> 8);
	v = ((v & 0x0F0F0F0F0F0F0F0F) << 4) | ((v & 0xF0F0F0F0F0F0F0F0) >> 4);
	v = ((v & 0x3333333333333333) << 2) | ((v & 0xCCCCCCCCCCCCCCCC) >> 2);
	v = ((v & 0x5555555555555555) << 1) | ((v & 0xAAAAAAAAAAAAAAAA) >> 1);
	return v;
}
