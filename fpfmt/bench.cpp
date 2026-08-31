// Head-to-head comparison of fpfmt::dtoa() against the Grisu2 implementation
// (milo::dtoa_milo) that it replaced.
//
//	make fpfmt-bench && ./fpfmt-bench          # timings
//	./fpfmt-bench -check [n]                   # correctness sweep
//
// -check verifies, over structured edge cases plus n random doubles, that
// every fpfmt::dtoa() result parses back to the value it came from, and
// reports how its digit strings compare with Grisu2's.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "fpfmt/fpfmt.hpp"
#include "milo/dtoa_milo.h"

using clk = std::chrono::steady_clock;

static double from_bits(uint64_t b) {
	double d;
	memcpy(&d, &b, sizeof(d));
	return d;
}

static uint64_t bits_of(double d) {
	uint64_t b;
	memcpy(&b, &d, sizeof(b));
	return b;
}

// significant_digits counts the significant decimal digits in a formatted
// number: leading zeros and zeros trailing the last nonzero digit don't count.
static int significant_digits(const std::string &str) {
	std::string s = str;
	size_t e = s.find('e');
	if (e != std::string::npos) {
		s = s.substr(0, e);
	}
	int n = 0, trailing = 0;
	bool seen = false;
	for (char c : s) {
		if (c >= '1' && c <= '9') {
			n += trailing + 1;
			trailing = 0;
			seen = true;
		} else if (c == '0' && seen) {
			trailing++;
		}
	}
	return n == 0 ? 1 : n;
}

struct dataset {
	const char *name;
	std::vector<double> v;
};

static std::vector<dataset> make_datasets(size_t n, std::mt19937_64 &rng) {
	std::vector<dataset> sets;

	{
		// Uniform random bit patterns: the worst case, nearly all 17 digits.
		dataset s{"random bit patterns", {}};
		while (s.v.size() < n) {
			double d = from_bits(rng());
			if (std::isfinite(d) && d != 0) {
				s.v.push_back(d);
			}
		}
		sets.push_back(std::move(s));
	}
	{
		// What tippecanoe mostly formats: longitudes and latitudes.
		dataset s{"geo coordinates", {}};
		std::uniform_real_distribution<double> u(-180, 180);
		while (s.v.size() < n) {
			s.v.push_back(u(rng));
		}
		sets.push_back(std::move(s));
	}
	{
		// Attribute values with only a few significant digits.
		dataset s{"short decimals", {}};
		std::uniform_int_distribution<int> m(1, 999999);
		std::uniform_int_distribution<int> e(-4, 4);
		while (s.v.size() < n) {
			s.v.push_back(m(rng) * pow(10.0, e(rng)));
		}
		sets.push_back(std::move(s));
	}
	{
		// IDs, counts, zoom levels.
		dataset s{"small integers", {}};
		std::uniform_int_distribution<int> u(0, 1000000);
		while (s.v.size() < n) {
			s.v.push_back((double) u(rng));
		}
		sets.push_back(std::move(s));
	}

	return sets;
}

static volatile size_t sink;

template <typename F>
static double best_ns(const std::vector<double> &v, int reps, F f) {
	double best = 1e300;
	for (int r = 0; r < reps; r++) {
		auto t0 = clk::now();
		size_t acc = 0;
		for (double d : v) {
			acc += f(d);
		}
		auto t1 = clk::now();
		sink += acc;
		double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double) v.size();
		best = std::min(best, ns);
	}
	return best;
}

static int check(long n) {
	uint64_t tested = 0, roundtrip_fail = 0, differ = 0, fpfmt_shorter = 0, milo_shorter = 0, same_length = 0;
	std::string first_fail;

	auto one = [&](double d) {
		tested++;
		std::string a = fpfmt::dtoa(d);
		std::string b = milo::dtoa_milo(d);
		if (strtod(a.c_str(), NULL) != d) {
			roundtrip_fail++;
			if (first_fail.empty()) {
				char buf[128];
				snprintf(buf, sizeof(buf), "bits=%016llx -> %s", (unsigned long long) bits_of(d), a.c_str());
				first_fail = buf;
			}
		}
		if (a != b) {
			differ++;
			int na = significant_digits(a), nb = significant_digits(b);
			if (na < nb) {
				fpfmt_shorter++;
			} else if (na > nb) {
				milo_shorter++;
			} else {
				same_length++;
			}
		}
	};

	// Non-finite and zero cases have to agree exactly.
	static const double special[] = {0.0, -0.0, INFINITY, -INFINITY, NAN};
	for (double d : special) {
		std::string a = fpfmt::dtoa(d), b = milo::dtoa_milo(d);
		if (a != b) {
			printf("MISMATCH on special value: fpfmt=%s milo=%s\n", a.c_str(), b.c_str());
			return 1;
		}
	}

	static const double edge[] = {
		1.0, -1.0, 0.1, 0.2, 0.3, 0.5, 1.5, 2.0, 3.0,
		1e-7, 1e-6, 1e-5, 1e-1, 1e20, 1e21, 1e22, 1e23,
		5e-324, 1e-323, 2.2250738585072014e-308, 2.2250738585072011e-308,
		1.7976931348623157e308, 123456789.0, 3.14159265358979,
		9007199254740992.0, 9007199254740993.0, 1e100, 1e-100};
	for (double d : edge) {
		one(d);
	}

	for (int i = -320; i <= 308; i++) {
		double d = pow(10.0, i);
		if (std::isfinite(d) && d != 0) {
			one(d);
		}
	}
	for (int i = 1; i < 100000; i++) {
		one((double) i);
		one(1.0 / i);
		one(-(double) i);
	}

	// Every float32 bit pattern, subsampled, promoted to double.
	for (uint64_t u = 0; u < (1ull << 32); u += 521) {
		float f;
		uint32_t b = (uint32_t) u;
		memcpy(&f, &b, sizeof(f));
		if (std::isfinite(f) && f != 0) {
			one((double) f);
		}
	}

	std::mt19937_64 rng(12345);
	for (long i = 0; i < n; i++) {
		double d = from_bits(rng());
		if (std::isfinite(d) && d != 0) {
			one(d);
		}
	}
	std::uniform_real_distribution<double> lat(-90, 90), lon(-180, 180);
	for (long i = 0; i < n / 4; i++) {
		one(lat(rng));
		one(lon(rng));
	}

	printf("values tested                    %llu\n", (unsigned long long) tested);
	printf("fpfmt failed to round trip       %llu\n", (unsigned long long) roundtrip_fail);
	printf("output differs from Grisu2       %llu\n", (unsigned long long) differ);
	printf("  fpfmt used fewer digits        %llu\n", (unsigned long long) fpfmt_shorter);
	printf("  Grisu2 used fewer digits       %llu\n", (unsigned long long) milo_shorter);
	printf("  same digit count, last differs %llu\n", (unsigned long long) same_length);
	if (!first_fail.empty()) {
		printf("first round trip failure: %s\n", first_fail.c_str());
	}
	return (roundtrip_fail != 0 || milo_shorter != 0) ? 1 : 0;
}

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "-check") == 0) {
		return check(argc > 2 ? atol(argv[2]) : 20000000L);
	}

	size_t n = (argc > 1) ? (size_t) atol(argv[1]) : 2000000;
	int reps = (argc > 2) ? atoi(argv[2]) : 5;
	std::mt19937_64 rng(2024);
	std::vector<dataset> sets = make_datasets(n, rng);

	printf("%zu values per dataset, best of %d runs\n\n", n, reps * 2);
	printf("full std::string formatting\n");
	printf("%-22s %14s %14s %9s\n", "dataset", "Grisu2 ns", "fpfmt ns", "speedup");
	for (auto &s : sets) {
		// Interleave the two so they share cache and clock conditions.
		double tm = best_ns(s.v, reps, [](double d) { return milo::dtoa_milo(d).size(); });
		double tf = best_ns(s.v, reps, [](double d) { return fpfmt::dtoa(d).size(); });
		tm = std::min(tm, best_ns(s.v, reps, [](double d) { return milo::dtoa_milo(d).size(); }));
		tf = std::min(tf, best_ns(s.v, reps, [](double d) { return fpfmt::dtoa(d).size(); }));
		printf("%-22s %14.2f %14.2f %8.2fx\n", s.name, tm, tf, tm / tf);
	}

	printf("\ndigit generation only, without the std::string\n");
	printf("%-22s %14s %14s %9s\n", "dataset", "Grisu2 ns", "fpfmt ns", "speedup");
	for (auto &s : sets) {
		double tm = best_ns(s.v, reps, [](double d) {
			static std::string b;
			b.clear();
			if (d == 0) {
				return (size_t) 1;
			}
			if (d < 0) {
				d = -d;
			}
			int length, K;
			milo::Grisu2(d, b, &length, &K);
			return (size_t) (length + K);
		});
		double tf = best_ns(s.v, reps, [](double d) {
			if (d == 0) {
				return (size_t) 1;
			}
			uint64_t dd;
			int p;
			fpfmt::shortest(d, &dd, &p);
			return (size_t) (fpfmt::digits(dd) + p);
		});
		printf("%-22s %14.2f %14.2f %8.2fx\n", s.name, tm, tf, tm / tf);
	}
	return 0;
}
