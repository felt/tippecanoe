// C-callable shortest-float formatting, for jsonpull.
// The implementation lives in text.cpp and calls fpfmt::dtoa().

#ifdef __cplusplus
extern "C" {
#endif

// dtoa_shortest returns a newly strdup()ed shortest decimal form of val.
// The caller owns the result.
char *dtoa_shortest(double val);

#ifdef __cplusplus
}
#endif
