#ifndef ATTRIBUTE_HPP
#define ATTRIBUTE_HPP

#include <vector>
#include <unordered_map>

enum attribute_op {
	op_sum,
	op_product,
	op_mean,
	op_concat,
	op_comma,
	op_max,
	op_min,
};

struct accum_state {
	double sum = 0;
	double count = 0;
};

struct serial_val;

void set_attribute_accum(std::unordered_map<std::string, attribute_op> &attribute_accum, std::string name, std::string type);
void set_attribute_accum(std::unordered_map<std::string, attribute_op> &attribute_accum, const char *arg, char **argv);
void preserve_attribute(attribute_op const &op, const std::string &key, serial_val const &val, std::vector<std::string> &full_keys, std::vector<serial_val> &full_values, std::unordered_map<std::string, accum_state> &attribute_accum_state);

#endif
