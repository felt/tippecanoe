#ifdef __APPLE__
#define _DARWIN_UNLIMITED_STREAMS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cmath>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <stdint.h>
#include <sqlite3.h>
#include <limits.h>
#include "main.hpp"
#include "mvt.hpp"
#include "mbtiles.hpp"
#include "projection.hpp"
#include "geometry.hpp"
#include "serial.hpp"
#include "errors.hpp"
#include "thread.hpp"

extern "C" {
#include "jsonpull/jsonpull.h"
}

#include "plugin.hpp"
#include "write_json.hpp"
#include "read_json.hpp"

struct writer_arg {
	int write_to;
	std::vector<mvt_layer> *layers;
	unsigned z;
	unsigned x;
	unsigned y;
	int extent;
};

void *run_writer(void *a) {
	writer_arg *wa = (writer_arg *) a;

	FILE *fp = fdopen(wa->write_to, "w");
	if (fp == NULL) {
		perror("fdopen (pipe writer)");
		exit(EXIT_OPEN);
	}

	json_writer state(fp);
	for (size_t i = 0; i < wa->layers->size(); i++) {
		layer_to_geojson((*(wa->layers))[i], wa->z, wa->x, wa->y, false, true, false, true, 0, 0, 0, true, state, 0, std::set<std::string>());
	}

	if (fclose(fp) != 0) {
		if (errno == EPIPE) {
			static bool warned = false;
			if (!warned) {
				fprintf(stderr, "Warning: broken pipe in postfilter\n");
				warned = true;
			}
		} else {
			perror("fclose output to filter");
			exit(EXIT_CLOSE);
		}
	}

	return NULL;
}

// Reads from the postfilter
std::vector<mvt_layer> parse_layers(int fd, int z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, int extent) {
	FILE *f = fdopen(fd, "r");
	if (f == NULL) {
		perror("fdopen filter output");
		exit(EXIT_OPEN);
	}

	std::vector<mvt_layer> out = parse_layers(f, z, x, y, extent, false);

	if (fclose(f) != 0) {
		perror("fclose postfilter output");
		exit(EXIT_CLOSE);
	}

	for (auto const &layer : out) {
		std::string layername = layer.name;

		std::map<std::string, layermap_entry> &layermap = (*layermaps)[tiling_seg];
		if (layermap.count(layername) == 0) {
			layermap_entry lme = layermap_entry(layermap.size());
			lme.minzoom = z;
			lme.maxzoom = z;

			layermap.insert(std::pair<std::string, layermap_entry>(layername, lme));

			if (lme.id >= (*layer_unmaps)[tiling_seg].size()) {
				(*layer_unmaps)[tiling_seg].resize(lme.id + 1);
				(*layer_unmaps)[tiling_seg][lme.id] = layername;
			}
		}

		auto ts = layermap.find(layername);
		if (ts == layermap.end()) {
			fprintf(stderr, "Internal error: layer %s not found\n", layername.c_str());
			exit(EXIT_IMPOSSIBLE);
		}
		if (z < ts->second.minzoom) {
			ts->second.minzoom = z;
		}
		if (z > ts->second.maxzoom) {
			ts->second.maxzoom = z;
		}

		for (auto const &feature : layer.features) {
			if (feature.type == mvt_point) {
				ts->second.points++;
			} else if (feature.type == mvt_linestring) {
				ts->second.lines++;
			} else if (feature.type == mvt_polygon) {
				ts->second.polygons++;
			}

			for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
				const std::string &key = layer.keys[feature.tags[i]];
				const mvt_value &val = layer.values[feature.tags[i + 1]];

				// Nulls can be excluded here because this is the postfilter
				// and it is nearly time to create the vector representation

				if (val.type != mvt_null) {
					add_to_tilestats(ts->second.tilestats, key, mvt_value_to_serial_val(val));
				}
			}
		}
	}

	return out;
}

// Reads from the prefilter
serial_feature parse_feature(json_pull *jp, int z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, bool postfilter, key_pool &key_pool) {
	serial_feature sf;

	while (1) {
		json_object *j = json_read(jp);
		if (j == NULL) {
			if (jp->error != NULL) {
				fprintf(stderr, "Filter output:%d: %s: ", jp->line, jp->error);
				if (jp->root != NULL) {
					json_context(jp->root);
				} else {
					fprintf(stderr, "\n");
				}
				exit(EXIT_JSON);
			}

			json_free(jp->root);
			sf.t = -1;
			return sf;
		}

		json_object *type = json_hash_get(j, "type");
		if (type == NULL || type->type != JSON_STRING) {
			continue;
		}
		if (strcmp(type->value.string.string, "Feature") != 0) {
			continue;
		}

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == NULL) {
			fprintf(stderr, "Filter output:%d: filtered feature with no geometry: ", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_JSON);
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == NULL || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "Filter output:%d: feature without properties hash: ", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_JSON);
		}

		json_object *geometry_type = json_hash_get(geometry, "type");
		if (geometry_type == NULL) {
			fprintf(stderr, "Filter output:%d: null geometry (additional not reported): ", jp->line);
			json_context(j);
			exit(EXIT_JSON);
		}

		if (geometry_type->type != JSON_STRING) {
			fprintf(stderr, "Filter output:%d: geometry type is not a string: ", jp->line);
			json_context(j);
			exit(EXIT_JSON);
		}

		json_object *coordinates = json_hash_get(geometry, "coordinates");
		if (coordinates == NULL || coordinates->type != JSON_ARRAY) {
			fprintf(stderr, "Filter output:%d: feature without coordinates array: ", jp->line);
			json_context(j);
			exit(EXIT_JSON);
		}

		int t;
		for (t = 0; t < GEOM_TYPES; t++) {
			if (strcmp(geometry_type->value.string.string, geometry_names[t]) == 0) {
				break;
			}
		}
		if (t >= GEOM_TYPES) {
			fprintf(stderr, "Filter output:%d: Can't handle geometry type %s: ", jp->line, geometry_type->value.string.string);
			json_context(j);
			exit(EXIT_JSON);
		}

		drawvec dv;
		parse_coordinates(t, coordinates, dv, VT_MOVETO, "Filter output", jp->line, j);
		if (mb_geometry[t] == VT_POLYGON) {
			dv = fix_polygon(dv, false, false);
		}

		// Scale and offset geometry from global to tile
		double scale = 1LL << geometry_scale;
		for (size_t i = 0; i < dv.size(); i++) {
			unsigned sx = 0, sy = 0;
			if (z != 0) {
				sx = x << (32 - z);
				sy = y << (32 - z);
			}
			dv[i].x = std::round(dv[i].x / scale) * scale - sx;
			dv[i].y = std::round(dv[i].y / scale) * scale - sy;
		}

		if (dv.size() > 0) {
			sf.t = mb_geometry[t];
			sf.segment = tiling_seg;
			sf.geometry = dv;
			sf.seq = 0;
			sf.index = 0;
			sf.bbox[0] = sf.bbox[1] = LLONG_MAX;
			sf.bbox[2] = sf.bbox[3] = LLONG_MIN;
			sf.extent = 0;
			sf.has_id = false;

			std::string layername = "unknown";
			json_object *tippecanoe = json_hash_get(j, "tippecanoe");
			if (tippecanoe != NULL) {
				json_object *layer = json_hash_get(tippecanoe, "layer");
				if (layer != NULL && layer->type == JSON_STRING) {
					layername = std::string(layer->value.string.string);
				}

				json_object *index = json_hash_get(tippecanoe, "index");
				if (index != NULL && index->type == JSON_NUMBER) {
					sf.index = index->value.number.number;
				}

				json_object *sequence = json_hash_get(tippecanoe, "sequence");
				if (sequence != NULL && sequence->type == JSON_NUMBER) {
					sf.seq = sequence->value.number.number;
				}

				json_object *extent = json_hash_get(tippecanoe, "extent");
				if (extent != NULL && extent->type == JSON_NUMBER) {
					sf.extent = extent->value.number.number;
				}

				json_object *dropped = json_hash_get(tippecanoe, "dropped");
				if (dropped != NULL && dropped->type == JSON_TRUE) {
					sf.dropped = FEATURE_DROPPED;  // dropped
				} else {
					sf.dropped = FEATURE_KEPT;  // kept
				}
			}

			for (size_t i = 0; i < dv.size(); i++) {
				if (dv[i].op == VT_MOVETO || dv[i].op == VT_LINETO) {
					if (dv[i].x < sf.bbox[0]) {
						sf.bbox[0] = dv[i].x;
					}
					if (dv[i].y < sf.bbox[1]) {
						sf.bbox[1] = dv[i].y;
					}
					if (dv[i].x > sf.bbox[2]) {
						sf.bbox[2] = dv[i].x;
					}
					if (dv[i].y > sf.bbox[3]) {
						sf.bbox[3] = dv[i].y;
					}
				}
			}

			json_object *id = json_hash_get(j, "id");
			if (id != NULL && id->type == JSON_NUMBER) {
				sf.id = id->value.number.number;
				if (id->value.number.large_unsigned > 0) {
					sf.id = id->value.number.large_unsigned;
				}
				sf.has_id = true;
			}

			std::map<std::string, layermap_entry> &layermap = (*layermaps)[tiling_seg];

			if (layermap.count(layername) == 0) {
				layermap_entry lme = layermap_entry(layermap.size());
				lme.minzoom = z;
				lme.maxzoom = z;

				layermap.insert(std::pair<std::string, layermap_entry>(layername, lme));

				if (lme.id >= (*layer_unmaps)[tiling_seg].size()) {
					(*layer_unmaps)[tiling_seg].resize(lme.id + 1);
					(*layer_unmaps)[tiling_seg][lme.id] = layername;
				}
			}

			auto ts = layermap.find(layername);
			if (ts == layermap.end()) {
				fprintf(stderr, "Internal error: layer %s not found\n", layername.c_str());
				exit(EXIT_IMPOSSIBLE);
			}
			sf.layer = ts->second.id;

			if (z < ts->second.minzoom) {
				ts->second.minzoom = z;
			}
			if (z > ts->second.maxzoom) {
				ts->second.maxzoom = z;
			}

			if (!postfilter) {
				if (sf.t == mvt_point) {
					ts->second.points++;
				} else if (sf.t == mvt_linestring) {
					ts->second.lines++;
				} else if (sf.t == mvt_polygon) {
					ts->second.polygons++;
				}
			}

			for (size_t i = 0; i < properties->value.object.length; i++) {
				serial_val v = stringify_value(properties->value.object.values[i], "Filter output", jp->line, j);

				// Nulls can be excluded here because the expression evaluation filter
				// would have already run before prefiltering

				if (v.type != mvt_null) {
					sf.full_keys.push_back(key_pool.pool(std::string(properties->value.object.keys[i]->value.string.string)));
					sf.full_values.push_back(v);

					if (!postfilter) {
						add_to_tilestats(ts->second.tilestats, std::string(properties->value.object.keys[i]->value.string.string), v);
					}
				}
			}

			json_free(j);
			return sf;
		}

		json_free(j);
	}
}

// ----------------------------------------------------------------------------
// Filter worker processes.
//
// Forking and waiting for the prefilter / postfilter shell processes used to
// happen directly in the tiling threads. fork() from a multithreaded process
// is expensive (the parent's full virtual address space has to be copy-on-write
// duplicated) and unsafe (other threads may hold libc / malloc locks at the
// moment of fork, which can deadlock the child before exec). Instead, we now
// pre-fork a small pool of single-threaded worker processes once, before any
// tiling threads have been started. When a tiling thread wants to run a
// filter, it asks one of these workers (over a unix-domain socketpair) to
// fork+exec the shell child on its behalf and to send back the input/output
// pipe file descriptors via SCM_RIGHTS. The tiling thread then reads/writes
// those pipes exactly as before, but the fork() and the subsequent waitpid()
// for the shell child happen inside the single-threaded worker, where they
// are cheap and safe.
// ----------------------------------------------------------------------------

namespace {

struct filter_worker {
	int sock = -1;	// parent end of socketpair to this worker
	pid_t pid = -1;
};

struct filter_pool {
	std::vector<filter_worker> workers;
	std::vector<size_t> free_list;
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	bool initialized = false;
};

filter_pool g_pool;

// Read/write helpers that loop over short reads/writes and EINTR.
ssize_t read_fully(int fd, void *buf, size_t n) {
	char *p = (char *) buf;
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);
		if (r == 0) {
			return (ssize_t) got;
		}
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		got += (size_t) r;
	}
	return (ssize_t) got;
}

ssize_t write_fully(int fd, const void *buf, size_t n) {
	const char *p = (const char *) buf;
	size_t sent = 0;
	while (sent < n) {
		ssize_t r = write(fd, p + sent, n - sent);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		sent += (size_t) r;
	}
	return (ssize_t) sent;
}

// Send a one-byte payload along with two file descriptors via SCM_RIGHTS.
// Returns the number of payload bytes sent (1) on success, -1 on error.
ssize_t send_two_fds(int sock, int fd1, int fd2) {
	char dummy = 0;
	struct iovec iov;
	iov.iov_base = &dummy;
	iov.iov_len = 1;

	char ctrl[CMSG_SPACE(sizeof(int) * 2)];
	memset(ctrl, 0, sizeof(ctrl));

	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof(ctrl);

	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int) * 2);
	int *fdptr = (int *) CMSG_DATA(cm);
	fdptr[0] = fd1;
	fdptr[1] = fd2;

	while (true) {
		ssize_t r = sendmsg(sock, &msg, 0);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		return r;
	}
}

// Receive a one-byte payload along with two file descriptors via SCM_RIGHTS.
// Returns 1 on success (and writes the fds into *fd1, *fd2), 0 on EOF, -1 on error.
ssize_t recv_two_fds(int sock, int *fd1, int *fd2) {
	char dummy = 0;
	struct iovec iov;
	iov.iov_base = &dummy;
	iov.iov_len = 1;

	char ctrl[CMSG_SPACE(sizeof(int) * 2)];
	memset(ctrl, 0, sizeof(ctrl));

	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof(ctrl);

	ssize_t r;
	while (true) {
		r = recvmsg(sock, &msg, 0);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		break;
	}
	if (r <= 0) {
		return r;
	}

	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	if (cm == NULL || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS || cm->cmsg_len != CMSG_LEN(sizeof(int) * 2)) {
		errno = EPROTO;
		return -1;
	}
	int *fdptr = (int *) CMSG_DATA(cm);
	*fd1 = fdptr[0];
	*fd2 = fdptr[1];
	return r;
}

// Main loop of a worker process. Reads filter requests from its socket,
// forks/execs the shell command, hands the input/output pipe fds back to
// the parent via SCM_RIGHTS, waits for the shell child to exit, then
// reports completion to the parent.
[[noreturn]] void worker_main(int sock) {
	while (true) {
		uint32_t flen;
		ssize_t r = read_fully(sock, &flen, sizeof(flen));
		if (r == 0) {
			// Parent closed the socket; we are done.
			_exit(0);
		}
		if (r < 0) {
			perror("filter worker: read header");
			_exit(1);
		}

		std::string filter(flen, '\0');
		if (flen > 0 && read_fully(sock, &filter[0], flen) != (ssize_t) flen) {
			perror("filter worker: read filter string");
			_exit(1);
		}

		uint32_t coords[3];
		if (read_fully(sock, coords, sizeof(coords)) != (ssize_t) sizeof(coords)) {
			perror("filter worker: read coords");
			_exit(1);
		}
		unsigned z = coords[0];
		unsigned x = coords[1];
		unsigned y = coords[2];

		int pipe_orig[2];
		int pipe_filtered[2];
		if (pipe(pipe_orig) < 0) {
			perror("filter worker: pipe (original features)");
			_exit(1);
		}
		if (pipe(pipe_filtered) < 0) {
			perror("filter worker: pipe (filtered features)");
			_exit(1);
		}

		std::string z_str = std::to_string(z);
		std::string x_str = std::to_string(x);
		std::string y_str = std::to_string(y);

		pid_t pid = fork();
		if (pid < 0) {
			perror("filter worker: fork");
			close(pipe_orig[0]);
			close(pipe_orig[1]);
			close(pipe_filtered[0]);
			close(pipe_filtered[1]);
			_exit(1);
		} else if (pid == 0) {
			// shell child
			if (dup2(pipe_orig[0], 0) < 0) {
				perror("dup child stdin");
				_exit(EXIT_OPEN);
			}
			if (dup2(pipe_filtered[1], 1) < 0) {
				perror("dup child stdout");
				_exit(EXIT_OPEN);
			}
			close(pipe_orig[0]);
			close(pipe_orig[1]);
			close(pipe_filtered[0]);
			close(pipe_filtered[1]);
			close(sock);

			execlp("sh", "sh", "-c", filter.c_str(), "sh", z_str.c_str(), x_str.c_str(), y_str.c_str(), NULL);
			perror("filter worker: exec");
			_exit(EXIT_PTHREAD);
		}

		// Worker (between parent tippecanoe and shell child).
		close(pipe_orig[0]);
		close(pipe_filtered[1]);

		// Hand the parent's-side fds back to the parent. After this
		// the parent owns pipe_orig[1] and pipe_filtered[0] in its
		// own fd table; we close our copies so EOFs propagate
		// correctly when the parent closes its ends.
		ssize_t s = send_two_fds(sock, pipe_orig[1], pipe_filtered[0]);
		close(pipe_orig[1]);
		close(pipe_filtered[0]);
		if (s < 0) {
			// Parent likely went away. Reap the shell child and exit.
			int stat_loc;
			while (waitpid(pid, &stat_loc, 0) < 0 && errno == EINTR) {
			}
			_exit(0);
		}

		// Block here until the shell child finishes. This is exactly
		// the wait that used to block a tiling thread; now it only
		// blocks this single-threaded worker, which has no other
		// work to do anyway.
		int stat_loc;
		while (waitpid(pid, &stat_loc, 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("filter worker: waitpid");
			break;
		}

		// Tell the parent we are done. A single byte is enough; the
		// existing code did not inspect the shell's exit status.
		char done = 0;
		if (write_fully(sock, &done, 1) < 0) {
			// Parent gone; just exit quietly.
			_exit(0);
		}
	}
}

size_t acquire_worker() {
	if (pthread_mutex_lock(&g_pool.mtx) != 0) {
		perror("pthread_mutex_lock (filter pool)");
		exit(EXIT_PTHREAD);
	}
	if (!g_pool.initialized) {
		fprintf(stderr, "Internal error: filter worker pool used before init\n");
		exit(EXIT_IMPOSSIBLE);
	}
	while (g_pool.free_list.empty()) {
		if (pthread_cond_wait(&g_pool.cond, &g_pool.mtx) != 0) {
			perror("pthread_cond_wait (filter pool)");
			exit(EXIT_PTHREAD);
		}
	}
	size_t idx = g_pool.free_list.back();
	g_pool.free_list.pop_back();
	if (pthread_mutex_unlock(&g_pool.mtx) != 0) {
		perror("pthread_mutex_unlock (filter pool)");
		exit(EXIT_PTHREAD);
	}
	return idx;
}

void release_worker(size_t idx) {
	if (pthread_mutex_lock(&g_pool.mtx) != 0) {
		perror("pthread_mutex_lock (filter pool)");
		exit(EXIT_PTHREAD);
	}
	g_pool.free_list.push_back(idx);
	if (pthread_cond_signal(&g_pool.cond) != 0) {
		perror("pthread_cond_signal (filter pool)");
		exit(EXIT_PTHREAD);
	}
	if (pthread_mutex_unlock(&g_pool.mtx) != 0) {
		perror("pthread_mutex_unlock (filter pool)");
		exit(EXIT_PTHREAD);
	}
}

}  // namespace

// Spawn the filter worker pool. Must be called from a single-threaded
// context (i.e. before any tiling or reader threads have been created)
// so that the workers themselves are forked from a clean, single-threaded
// process state. Calling this more than once is a no-op.
void filter_workers_init(size_t n) {
	if (g_pool.initialized) {
		return;
	}
	if (n < 1) {
		n = 1;
	}

	g_pool.workers.resize(n);
	g_pool.free_list.reserve(n);

	for (size_t i = 0; i < n; i++) {
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
			perror("socketpair (filter worker)");
			exit(EXIT_PTHREAD);
		}

		pid_t pid = fork();
		if (pid < 0) {
			perror("fork (filter worker)");
			exit(EXIT_PTHREAD);
		} else if (pid == 0) {
			// Worker process.
			close(sv[0]);
			// Close the parent ends of any sibling workers we
			// inherited from earlier iterations of this loop.
			for (size_t j = 0; j < i; j++) {
				close(g_pool.workers[j].sock);
			}
			worker_main(sv[1]);
			// worker_main is [[noreturn]].
		} else {
			close(sv[1]);
			if (fcntl(sv[0], F_SETFD, FD_CLOEXEC) != 0) {
				perror("cloexec (filter worker socket)");
				exit(EXIT_CLOSE);
			}
			g_pool.workers[i].sock = sv[0];
			g_pool.workers[i].pid = pid;
			g_pool.free_list.push_back(i);
		}
	}

	g_pool.initialized = true;
}

void setup_filter(const char *filter, int *write_to, int *read_from, filter_handle *handle, unsigned z, unsigned x, unsigned y) {
	// Send the request to a worker process which will fork+exec the shell
	// child and send back the input/output pipe fds via SCM_RIGHTS. The
	// fork happens entirely inside the single-threaded worker, so the
	// parent's tiling threads never have to fork.

	size_t idx = acquire_worker();
	int sock = g_pool.workers[idx].sock;

	uint32_t flen = (uint32_t) strlen(filter);
	if (write_fully(sock, &flen, sizeof(flen)) < 0 ||
	    (flen > 0 && write_fully(sock, filter, flen) < 0)) {
		perror("write filter request");
		exit(EXIT_PTHREAD);
	}
	uint32_t coords[3] = {(uint32_t) z, (uint32_t) x, (uint32_t) y};
	if (write_fully(sock, coords, sizeof(coords)) < 0) {
		perror("write filter coords");
		exit(EXIT_PTHREAD);
	}

	int wfd = -1, rfd = -1;
	ssize_t r = recv_two_fds(sock, &wfd, &rfd);
	if (r <= 0) {
		fprintf(stderr, "Failed to receive filter pipe fds from worker\n");
		exit(EXIT_PTHREAD);
	}

	if (fcntl(wfd, F_SETFD, FD_CLOEXEC) != 0) {
		perror("cloexec output to filter");
		exit(EXIT_CLOSE);
	}
	if (fcntl(rfd, F_SETFD, FD_CLOEXEC) != 0) {
		perror("cloexec input from filter");
		exit(EXIT_CLOSE);
	}

	*write_to = wfd;
	*read_from = rfd;
	handle->worker_idx = (ssize_t) idx;
}

void wait_filter(filter_handle handle) {
	if (handle.worker_idx < 0) {
		return;
	}
	int sock = g_pool.workers[handle.worker_idx].sock;

	char done;
	while (true) {
		ssize_t r = read(sock, &done, 1);
		if (r == 1) {
			break;
		}
		if (r == 0) {
			fprintf(stderr, "filter worker exited unexpectedly\n");
			exit(EXIT_PTHREAD);
		}
		if (errno == EINTR) {
			continue;
		}
		perror("read filter completion");
		exit(EXIT_PTHREAD);
	}

	release_worker((size_t) handle.worker_idx);
}

std::vector<mvt_layer> filter_layers(const char *filter, std::vector<mvt_layer> &layers, unsigned z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, int extent) {
	int write_to, read_from;
	filter_handle handle;
	setup_filter(filter, &write_to, &read_from, &handle, z, x, y);

	writer_arg wa;
	wa.write_to = write_to;
	wa.layers = &layers;
	wa.z = z;
	wa.x = x;
	wa.y = y;
	wa.extent = extent;

	pthread_t writer;
	// this does need to be a real thread, so we can pipe both to and from it
	if (pthread_create(&writer, NULL, run_writer, &wa) != 0) {
		perror("pthread_create (filter writer)");
		exit(EXIT_PTHREAD);
	}

	std::vector<mvt_layer> nlayers = parse_layers(read_from, z, x, y, layermaps, tiling_seg, layer_unmaps, extent);

	wait_filter(handle);

	void *ret;
	if (pthread_join(writer, &ret) != 0) {
		perror("pthread_join filter writer");
		exit(EXIT_PTHREAD);
	}

	return nlayers;
}
