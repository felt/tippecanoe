#ifndef SORT_HPP
#define SORT_HPP

void fqsort(std::vector<FILE *> &inputs, size_t width, int (*cmp)(const void *, const void *), FILE *out, size_t mem);

#endif
