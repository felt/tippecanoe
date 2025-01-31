#ifndef ATTRIBUTE_HPP
#define ATTRIBUTE_HPP

#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include "mvt.hpp"
#include "milo/dtoa_milo.h"

enum attribute_op {
	op_sum,
	op_product,
	op_mean,
	op_concat,
	op_comma,
	op_max,
	op_min,
	op_count,
};

struct serial_val;
struct key_pool;

void set_attribute_accum(std::unordered_map<std::string, attribute_op> &attribute_accum, std::string name, std::string type);
void set_attribute_accum(std::unordered_map<std::string, attribute_op> &attribute_accum, const char *arg, char **argv);

void preserve_attribute(attribute_op const &op, std::string const &key, serial_val const &val, std::vector<std::shared_ptr<std::string>> &full_keys, std::vector<serial_val> &full_values, key_pool &key_pool);
void preserve_attribute(attribute_op const &op, std::string const &key, mvt_value const &val, std::vector<std::shared_ptr<std::string>> &full_keys, std::vector<mvt_value> &full_values, key_pool &key_pool);

extern std::map<std::string, attribute_op> numeric_operations;

#endif
