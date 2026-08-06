#include "mlt.hpp"

#include <mlt/decoder.hpp>
#include <mlt/geometry.hpp>
#include <mlt/layer.hpp>
#include <mlt/properties.hpp>
#include <mlt/tile.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

bool read_varint(const char *&p, const char *end, unsigned long long &out) {
	out = 0;
	int shift = 0;

	while (p < end) {
		unsigned char c = (unsigned char) *p++;
		if (shift > 63) {
			return false;
		}
		out |= ((unsigned long long) (c & 0x7F)) << shift;
		if ((c & 0x80) == 0) {
			return true;
		}
		shift += 7;
	}

	return false;
}

long long round_coord(float v) {
	return (long long) std::llround(v);
}

// Append a sequence of coordinates as a moveto followed by linetos. Rings
// come out of the MLT decoder explicitly closed, but MVT rings are closed
// implicitly by the closepath operation, so the repeated final point is
// dropped.
void add_line(mvt_feature &feature, const mlt::CoordVec &coords, bool ring) {
	size_t n = coords.size();

	if (ring && n > 1 && coords.front() == coords.back()) {
		n--;
	}

	for (size_t i = 0; i < n; i++) {
		feature.geometry.emplace_back(i == 0 ? mvt_moveto : mvt_lineto,
					      round_coord(coords[i].x), round_coord(coords[i].y));
	}

	if (ring && n > 0) {
		feature.geometry.emplace_back(mvt_closepath, 0, 0);
	}
}

void convert_geometry(const mlt::geometry::Geometry &geom, mvt_feature &feature) {
	using GeometryType = mlt::metadata::tileset::GeometryType;
	namespace geometry = mlt::geometry;

	switch (geom.type) {
	case GeometryType::POINT: {
		const auto &point = static_cast<const geometry::Point &>(geom);
		feature.type = mvt_point;
		feature.geometry.emplace_back(mvt_moveto,
					      round_coord(point.getCoordinate().x),
					      round_coord(point.getCoordinate().y));
		break;
	}

	case GeometryType::MULTIPOINT: {
		const auto &multipoint = static_cast<const geometry::MultiPoint &>(geom);
		feature.type = mvt_point;
		for (const auto &coord : multipoint.getCoordinates()) {
			feature.geometry.emplace_back(mvt_moveto, round_coord(coord.x), round_coord(coord.y));
		}
		break;
	}

	case GeometryType::LINESTRING: {
		const auto &linestring = static_cast<const geometry::LineString &>(geom);
		feature.type = mvt_linestring;
		add_line(feature, linestring.getCoordinates(), false);
		break;
	}

	case GeometryType::MULTILINESTRING: {
		const auto &multilinestring = static_cast<const geometry::MultiLineString &>(geom);
		feature.type = mvt_linestring;
		for (const auto &linestring : multilinestring.getLineStrings()) {
			add_line(feature, linestring, false);
		}
		break;
	}

	case GeometryType::POLYGON: {
		const auto &polygon = static_cast<const geometry::Polygon &>(geom);
		feature.type = mvt_polygon;
		for (const auto &ring : polygon.getRings()) {
			add_line(feature, ring, true);
		}
		break;
	}

	case GeometryType::MULTIPOLYGON: {
		const auto &multipolygon = static_cast<const geometry::MultiPolygon &>(geom);
		feature.type = mvt_polygon;
		for (const auto &polygon : multipolygon.getPolygons()) {
			for (const auto &ring : polygon) {
				add_line(feature, ring, true);
			}
		}
		break;
	}
	}
}

// Converts one MLT property into the equivalent MVT value, returning false
// for properties that MVT has no way to represent, which are left off the
// feature instead.
struct property_converter {
	mvt_value &out;
	const std::shared_ptr<std::string> &pool;

	bool operator()(std::nullptr_t) const {
		return false;
	}

	bool operator()(bool v) const {
		out.type = mvt_bool;
		out.numeric_value.bool_value = v;
		return true;
	}

	bool operator()(std::int32_t v) const {
		out.type = mvt_int;
		out.numeric_value.int_value = v;
		return true;
	}

	bool operator()(std::int64_t v) const {
		out.type = mvt_int;
		out.numeric_value.int_value = v;
		return true;
	}

	bool operator()(std::uint32_t v) const {
		out.type = mvt_uint;
		out.numeric_value.uint_value = v;
		return true;
	}

	bool operator()(std::uint64_t v) const {
		out.type = mvt_uint;
		out.numeric_value.uint_value = v;
		return true;
	}

	bool operator()(float v) const {
		out.type = mvt_float;
		out.numeric_value.float_value = v;
		return true;
	}

	bool operator()(double v) const {
		out.type = mvt_double;
		out.numeric_value.double_value = v;
		return true;
	}

	bool operator()(std::string_view v) const {
		out.s = pool;
		out.set_string_value(v);
		return true;
	}

	template <typename T>
	bool operator()(const std::optional<T> &v) const {
		if (v.has_value()) {
			return (*this)(*v);
		}
		return false;
	}
};

void convert_layer(const mlt::Layer &in, mvt_layer &out, const std::shared_ptr<std::string> &pool) {
	out.version = 2;
	out.name = in.getName();
	out.extent = in.getExtent();

	// The property columns are held in an unordered map, so sort the names
	// to keep the attributes of the decoded tile in a stable order.
	std::vector<std::pair<const std::string *, const mlt::PresentProperties *>> columns;
	columns.reserve(in.getProperties().size());
	for (const auto &property : in.getProperties()) {
		columns.emplace_back(&property.first, &property.second);
	}
	std::sort(columns.begin(), columns.end(), [](auto const &a, auto const &b) {
		return *a.first < *b.first;
	});

	out.features.reserve(in.getFeatures().size());

	for (const auto &in_feature : in.getFeatures()) {
		mvt_feature feature;

		if (in_feature.getID()) {
			feature.id = *in_feature.getID();
			feature.has_id = true;
		}

		convert_geometry(in_feature.getGeometry(), feature);

		for (const auto &column : columns) {
			auto property = column.second->getProperty(in_feature.getIndex());
			if (!property) {
				continue;
			}

			mvt_value value;
			if (std::visit(property_converter{value, pool}, *property)) {
				out.tag(feature, *column.first, value);
			}
		}

		out.features.push_back(std::move(feature));
	}
}

}  // namespace

bool is_mlt(const std::string &message) {
	// An MLT tile is a sequence of layers, each of which is a varint byte
	// count followed by a varint tag whose only defined value is 1.
	//
	// An MVT tile is a protobuf whose only field is the repeated layer
	// field 3, so it begins with the byte 0x1a followed by the byte count
	// of a layer, which can never be as short as the single byte that
	// would be needed to masquerade as an MLT layer tag.

	const char *p = message.data();
	const char *end = p + message.size();

	unsigned long long layer_length;
	if (!read_varint(p, end, layer_length)) {
		return false;
	}
	if (layer_length < 2 || layer_length > (unsigned long long) (end - p)) {
		return false;
	}

	unsigned long long layer_tag;
	if (!read_varint(p, end, layer_tag)) {
		return false;
	}

	return layer_tag == 1;
}

bool decode_mlt(const std::string &message, mvt_tile &out) {
	try {
		mlt::Decoder decoder(true);
		mlt::MapLibreTile tile = decoder.decode(mlt::DataView(message.data(), message.size()));

		std::shared_ptr<std::string> pool = std::make_shared<std::string>();

		out.layers.clear();
		out.layers.reserve(tile.getLayers().size());

		for (const auto &layer : tile.getLayers()) {
			mvt_layer out_layer;
			convert_layer(layer, out_layer, pool);
			out.layers.push_back(std::move(out_layer));
		}

		return true;
	} catch (std::exception const &e) {
		fprintf(stderr, "MLT decoding error: %s\n", e.what());
		return false;
	}
}
