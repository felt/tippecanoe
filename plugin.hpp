struct key_pool;

// Identifies which worker process is currently running a filter on this
// thread's behalf. Returned by setup_filter and consumed by wait_filter.
struct filter_handle {
	ssize_t worker_idx = -1;
};

std::vector<mvt_layer> filter_layers(const char *filter, std::vector<mvt_layer> &layer, unsigned z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, int extent);
void setup_filter(const char *filter, int *write_to, int *read_from, filter_handle *handle, unsigned z, unsigned x, unsigned y);
void wait_filter(filter_handle handle);
serial_feature parse_feature(json_pull *jp, int z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, bool filters, key_pool &key_pool);

// Pre-fork the pool of single-threaded filter worker processes used by
// setup_filter / wait_filter. Must be called from a single-threaded context
// (i.e. before any reader or tiling threads are spawned).
void filter_workers_init(size_t n);
