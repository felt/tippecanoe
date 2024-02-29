#include <stdio.h>
#include <algorithm>
#include "geometry.hpp"
#include "serial.hpp"
#include "options.hpp"

// This should all be removed and replaced with --no-simplification-of-shared-nodes

// Does not fix up moveto/lineto
static drawvec reverse_subring(drawvec const &dv) {
	drawvec out;

	for (size_t i = dv.size(); i > 0; i--) {
		out.push_back(dv[i - 1]);
	}

	return out;
}

struct edge {
	unsigned x1 = 0;
	unsigned y1 = 0;
	unsigned x2 = 0;
	unsigned y2 = 0;
	unsigned ring = 0;

	edge(unsigned _x1, unsigned _y1, unsigned _x2, unsigned _y2, unsigned _ring) {
		x1 = _x1;
		y1 = _y1;
		x2 = _x2;
		y2 = _y2;
		ring = _ring;
	}

	bool operator<(const edge &s) const {
		long long cmp = (long long) y1 - s.y1;
		if (cmp == 0) {
			cmp = (long long) x1 - s.x1;
		}
		if (cmp == 0) {
			cmp = (long long) y2 - s.y2;
		}
		if (cmp == 0) {
			cmp = (long long) x2 - s.x2;
		}
		return cmp < 0;
	}
};

struct edgecmp_ring {
	bool operator()(const edge &a, const edge &b) {
		long long cmp = (long long) a.y1 - b.y1;
		if (cmp == 0) {
			cmp = (long long) a.x1 - b.x1;
		}
		if (cmp == 0) {
			cmp = (long long) a.y2 - b.y2;
		}
		if (cmp == 0) {
			cmp = (long long) a.x2 - b.x2;
		}
		if (cmp == 0) {
			cmp = (long long) a.ring - b.ring;
		}
		return cmp < 0;
	}
} edgecmp_ring;

bool edges_same(std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e1, std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e2) {
	if ((e2.second - e2.first) != (e1.second - e1.first)) {
		return false;
	}

	while (e1.first != e1.second) {
		if (e1.first->ring != e2.first->ring) {
			return false;
		}

		++e1.first;
		++e2.first;
	}

	return true;
}

bool find_common_edges(std::vector<serial_feature> &features, int z, int line_detail, double simplification, int maxzoom, double merge_fraction) {
	size_t merge_count = ceil((1 - merge_fraction) * features.size());

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				drawvec &g = features[i].geometry;
				drawvec out;

				for (size_t k = 0; k < g.size(); k++) {
					if (g[k].op == VT_LINETO && k > 0 && g[k - 1] == g[k]) {
						;
					} else {
						out.push_back(g[k]);
					}
				}

				features[i].geometry = out;
			}
		}
	}

	// Construct a mapping from all polygon edges to the set of rings
	// that each edge appears in. (The ring number is across all polygons;
	// we don't need to look it back up, just to tell where it changes.)

	std::vector<edge> edges;
	size_t ring = 0;
	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				for (size_t k = 0; k + 1 < features[i].geometry.size(); k++) {
					if (features[i].geometry[k].op == VT_MOVETO) {
						ring++;
					}

					if (features[i].geometry[k + 1].op == VT_LINETO) {
						drawvec dv;
						if (features[i].geometry[k] < features[i].geometry[k + 1]) {
							dv.push_back(features[i].geometry[k]);
							dv.push_back(features[i].geometry[k + 1]);
						} else {
							dv.push_back(features[i].geometry[k + 1]);
							dv.push_back(features[i].geometry[k]);
						}

						edges.push_back(edge(dv[0].x, dv[0].y, dv[1].x, dv[1].y, ring));
					}
				}
			}
		}
	}

	std::stable_sort(edges.begin(), edges.end(), edgecmp_ring);
	std::set<draw> necessaries;

	// Now mark all the points where the set of rings using the edge on one side
	// is not the same as the set of rings using the edge on the other side.

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				drawvec &g = features[i].geometry;

				for (size_t k = 0; k < g.size(); k++) {
					g[k].necessary = 0;
				}

				for (size_t a = 0; a < g.size(); a++) {
					if (g[a].op == VT_MOVETO) {
						size_t b;

						for (b = a + 1; b < g.size(); b++) {
							if (g[b].op != VT_LINETO) {
								break;
							}
						}

						// -1 because of duplication at the end
						size_t s = b - a - 1;

						if (s > 0) {
							drawvec left;
							if (g[a + (s - 1) % s] < g[a]) {
								left.push_back(g[a + (s - 1) % s]);
								left.push_back(g[a]);
							} else {
								left.push_back(g[a]);
								left.push_back(g[a + (s - 1) % s]);
							}
							if (left[1] < left[0]) {
								fprintf(stderr, "left misordered\n");
							}
							std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e1 = std::equal_range(edges.begin(), edges.end(), edge(left[0].x, left[0].y, left[1].x, left[1].y, 0));

							for (size_t k = 0; k < s; k++) {
								drawvec right;

								if (g[a + k] < g[a + k + 1]) {
									right.push_back(g[a + k]);
									right.push_back(g[a + k + 1]);
								} else {
									right.push_back(g[a + k + 1]);
									right.push_back(g[a + k]);
								}

								std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e2 = std::equal_range(edges.begin(), edges.end(), edge(right[0].x, right[0].y, right[1].x, right[1].y, 0));

								if (right[1] < right[0]) {
									fprintf(stderr, "left misordered\n");
								}

								if (e1.first == e1.second || e2.first == e2.second) {
									fprintf(stderr, "Internal error: polygon edge lookup failed for %lld,%lld to %lld,%lld or %lld,%lld to %lld,%lld\n", left[0].x, left[0].y, left[1].x, left[1].y, right[0].x, right[0].y, right[1].x, right[1].y);
									exit(EXIT_IMPOSSIBLE);
								}

								if (!edges_same(e1, e2)) {
									g[a + k].necessary = 1;
									necessaries.insert(g[a + k]);
								}

								e1 = e2;
							}
						}

						a = b - 1;
					}
				}
			}
		}
	}

	edges.clear();
	std::map<drawvec, size_t> arcs;
	std::multimap<ssize_t, size_t> merge_candidates;  // from arc to serial_feature

	// Roll rings that include a necessary point around so they start at one

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				drawvec &g = features[i].geometry;

				for (size_t k = 0; k < g.size(); k++) {
					if (necessaries.count(g[k]) != 0) {
						g[k].necessary = 1;
					}
				}

				for (size_t k = 0; k < g.size(); k++) {
					if (g[k].op == VT_MOVETO) {
						ssize_t necessary = -1;
						ssize_t lowest = k;
						size_t l;
						for (l = k + 1; l < g.size(); l++) {
							if (g[l].op != VT_LINETO) {
								break;
							}

							if (g[l].necessary) {
								necessary = l;
							}
							if (g[l] < g[lowest]) {
								lowest = l;
							}
						}

						if (necessary < 0) {
							necessary = lowest;
							// Add a necessary marker if there was none in the ring,
							// so the arc code below can find it.
							g[lowest].necessary = 1;
						}

						{
							drawvec tmp;

							// l - 1 because the endpoint is duplicated
							for (size_t m = necessary; m < l - 1; m++) {
								tmp.push_back(g[m]);
							}
							for (ssize_t m = k; m < necessary; m++) {
								tmp.push_back(g[m]);
							}

							// replace the endpoint
							tmp.push_back(g[necessary]);

							if (tmp.size() != l - k) {
								fprintf(stderr, "internal error shifting ring\n");
								exit(EXIT_IMPOSSIBLE);
							}

							for (size_t m = 0; m < tmp.size(); m++) {
								if (m == 0) {
									tmp[m].op = VT_MOVETO;
								} else {
									tmp[m].op = VT_LINETO;
								}

								g[k + m] = tmp[m];
							}
						}

						// Now peel off each set of segments from one necessary point to the next
						// into an "arc" as in TopoJSON

						for (size_t m = k; m < l; m++) {
							if (!g[m].necessary) {
								fprintf(stderr, "internal error in arc building\n");
								exit(EXIT_IMPOSSIBLE);
							}

							drawvec arc;
							size_t n;
							for (n = m; n < l; n++) {
								arc.push_back(g[n]);
								if (n > m && g[n].necessary) {
									break;
								}
							}

							auto f = arcs.find(arc);
							if (f == arcs.end()) {
								drawvec arc2 = reverse_subring(arc);

								auto f2 = arcs.find(arc2);
								if (f2 == arcs.end()) {
									// Add new arc
									size_t added = arcs.size() + 1;
									arcs.insert(std::pair<drawvec, size_t>(arc, added));
									features[i].arc_polygon.push_back(added);
									merge_candidates.insert(std::pair<ssize_t, size_t>(added, i));
								} else {
									features[i].arc_polygon.push_back(-(ssize_t) f2->second);
									merge_candidates.insert(std::pair<ssize_t, size_t>(-(ssize_t) f2->second, i));
								}
							} else {
								features[i].arc_polygon.push_back(f->second);
								merge_candidates.insert(std::pair<ssize_t, size_t>(f->second, i));
							}

							m = n - 1;
						}

						features[i].arc_polygon.push_back(0);

						k = l - 1;
					}
				}
			}
		}
	}

	// Simplify each arc

	std::vector<drawvec> simplified_arcs;

	for (auto ai = arcs.begin(); ai != arcs.end(); ++ai) {
		if (simplified_arcs.size() < ai->second + 1) {
			simplified_arcs.resize(ai->second + 1);
		}

		drawvec dv = ai->first;
		for (size_t i = 0; i < dv.size(); i++) {
			if (i == 0) {
				dv[i].op = VT_MOVETO;
			} else {
				dv[i].op = VT_LINETO;
			}
		}
		if (!(prevent[P_SIMPLIFY] || (z == maxzoom && prevent[P_SIMPLIFY_LOW]) || (z < maxzoom && additional[A_GRID_LOW_ZOOMS]))) {
			// tx and ty are 0 here because we aren't trying to do anything with the shared_nodes_map
			simplified_arcs[ai->second] = simplify_lines(dv, z, 0, 0, line_detail, !(prevent[P_CLIPPING] || prevent[P_DUPLICATION]), simplification, 4, drawvec(), NULL, 0);
		} else {
			simplified_arcs[ai->second] = dv;
		}
	}

	// If necessary, merge some adjacent polygons into some other polygons

	struct merge_order {
		ssize_t edge = 0;
		unsigned long long gap = 0;
		size_t p1 = 0;
		size_t p2 = 0;

		bool operator<(const merge_order &m) const {
			return gap < m.gap;
		}
	};
	std::vector<merge_order> order;

	for (ssize_t i = 0; i < (ssize_t) simplified_arcs.size(); i++) {
		auto r1 = merge_candidates.equal_range(i);
		for (auto r1i = r1.first; r1i != r1.second; ++r1i) {
			auto r2 = merge_candidates.equal_range(-i);
			for (auto r2i = r2.first; r2i != r2.second; ++r2i) {
				if (r1i->second != r2i->second) {
					merge_order mo;
					mo.edge = i;
					if (features[r1i->second].index > features[r2i->second].index) {
						mo.gap = features[r1i->second].index - features[r2i->second].index;
					} else {
						mo.gap = features[r2i->second].index - features[r1i->second].index;
					}
					mo.p1 = r1i->second;
					mo.p2 = r2i->second;
					order.push_back(mo);
				}
			}
		}
	}
	std::stable_sort(order.begin(), order.end());

	size_t merged = 0;
	for (size_t o = 0; o < order.size(); o++) {
		if (merged >= merge_count) {
			break;
		}

		size_t i = order[o].p1;
		while (features[i].renamed >= 0) {
			i = features[i].renamed;
		}
		size_t i2 = order[o].p2;
		while (features[i2].renamed >= 0) {
			i2 = features[i2].renamed;
		}

		for (size_t j = 0; j < features[i].arc_polygon.size() && merged < merge_count; j++) {
			if (features[i].arc_polygon[j] == order[o].edge) {
				{
					// XXX snap links
					if (features[order[o].p2].arc_polygon.size() > 0) {
						// This has to merge the ring that contains the anti-arc to this arc
						// into the current ring, and then add whatever other rings were in
						// that feature on to the end.
						//
						// This can't be good for keeping parent-child relationships among
						// the rings in order, but Wagyu should sort that out later

						std::vector<ssize_t> additions;
						std::vector<ssize_t> &here = features[i].arc_polygon;
						std::vector<ssize_t> &other = features[i2].arc_polygon;

#if 0
						printf("seeking %zd\n", features[i].arc_polygon[j]);
						printf("before: ");
						for (size_t k = 0; k < here.size(); k++) {
							printf("%zd ", here[k]);
						}
						printf("\n");
						printf("other: ");
						for (size_t k = 0; k < other.size(); k++) {
							printf("%zd ", other[k]);
						}
						printf("\n");
#endif

						for (size_t k = 0; k < other.size(); k++) {
							size_t l;
							for (l = k; l < other.size(); l++) {
								if (other[l] == 0) {
									break;
								}
							}
							if (l >= other.size()) {
								l--;
							}

#if 0
							for (size_t m = k; m <= l; m++) {
								printf("%zd ", other[m]);
							}
							printf("\n");
#endif

							size_t m;
							for (m = k; m <= l; m++) {
								if (other[m] == -features[i].arc_polygon[j]) {
									break;
								}
							}

							if (m <= l) {
								// Found the shared arc

								here.erase(here.begin() + j);

								size_t off = 0;
								for (size_t n = m + 1; n < l; n++) {
									here.insert(here.begin() + j + off, other[n]);
									off++;
								}
								for (size_t n = k; n < m; n++) {
									here.insert(here.begin() + j + off, other[n]);
									off++;
								}
							} else {
								// Looking at some other ring

								for (size_t n = k; n <= l; n++) {
									additions.push_back(other[n]);
								}
							}

							k = l;
						}

						features[i2].arc_polygon.clear();
						features[i2].renamed = i;
						merged++;

						for (size_t k = 0; k < additions.size(); k++) {
							features[i].arc_polygon.push_back(additions[k]);
						}

#if 0
						printf("after: ");
						for (size_t k = 0; k < here.size(); k++) {
							printf("%zd ", here[k]);
						}
						printf("\n");
#endif

#if 0
						for (size_t k = 0; k + 1 < here.size(); k++) {
							if (here[k] != 0 && here[k + 1] != 0) {
								if (simplified_arcs[here[k + 1]][0] != simplified_arcs[here[k]][simplified_arcs[here[k]].size() - 1]) {
									printf("error from %zd to %zd\n", here[k], here[k + 1]);
								}
							}
						}
#endif
					}
				}
			}
		}
	}

	// Turn the arc representations of the polygons back into standard polygon geometries

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			features[i].geometry.clear();
			bool at_start = true;
			draw first(-1, 0, 0);

			for (size_t j = 0; j < features[i].arc_polygon.size(); j++) {
				ssize_t p = features[i].arc_polygon[j];

				if (p == 0) {
					if (first.op >= 0) {
						features[i].geometry.push_back(first);
						first = draw(-1, 0, 0);
					}
					at_start = true;
				} else if (p > 0) {
					for (size_t k = 0; k + 1 < simplified_arcs[p].size(); k++) {
						if (at_start) {
							features[i].geometry.push_back(draw(VT_MOVETO, simplified_arcs[p][k].x, simplified_arcs[p][k].y));
							first = draw(VT_LINETO, simplified_arcs[p][k].x, simplified_arcs[p][k].y);
						} else {
							features[i].geometry.push_back(draw(VT_LINETO, simplified_arcs[p][k].x, simplified_arcs[p][k].y));
						}
						at_start = 0;
					}
				} else { /* p < 0 */
					for (ssize_t k = simplified_arcs[-p].size() - 1; k > 0; k--) {
						if (at_start) {
							features[i].geometry.push_back(draw(VT_MOVETO, simplified_arcs[-p][k].x, simplified_arcs[-p][k].y));
							first = draw(VT_LINETO, simplified_arcs[-p][k].x, simplified_arcs[-p][k].y);
						} else {
							features[i].geometry.push_back(draw(VT_LINETO, simplified_arcs[-p][k].x, simplified_arcs[-p][k].y));
						}
						at_start = 0;
					}
				}
			}
		}
	}

	if (merged >= merge_count) {
		return true;
	} else {
		return false;
	}
}
