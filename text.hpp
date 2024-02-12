#ifndef TEXT_HPP
#define TEXT_HPP

#include <string>

std::string check_utf8(std::string text);
const char *utf8_next(const char *s, long *c);
std::string truncate16(std::string const &s, size_t runes);
int integer_zoom(std::string where, std::string text);
std::string format_commandline(int argc, char **argv);
std::vector<std::string> read_unidecode(const char *fname);
std::string unidecode_smash(std::vector<std::string> const &unidecode_data, const char *s);
unsigned long long fnv1a(std::string const &s);
unsigned long long fnv1a(const char *s, char additional);
unsigned long long fnv1a(size_t size, void *p);
unsigned long long bit_reverse(unsigned long long v);

#endif
