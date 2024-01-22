#ifndef ATTRIBUTE_HPP
#define ATTRIBUTE_HPP

enum attribute_op {
	op_sum,
	op_product,
	op_mean,
	op_concat,
	op_comma,
	op_max,
	op_min,
};

void set_attribute_accum(std::map<std::string, attribute_op> &attribute_accum, std::string name, std::string type);
void set_attribute_accum(std::map<std::string, attribute_op> &attribute_accum, const char *arg, char **argv);

#endif
