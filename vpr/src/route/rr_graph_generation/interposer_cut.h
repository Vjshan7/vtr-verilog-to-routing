#pragma once

#include <vector>
#include "rr_graph_fwd.h"
#include "rr_graph_view.h"
#include "device_grid.h"

std::vector<RREdgeId> mark_interposer_cut_edges_for_removal(const RRGraphView& rr_graph, const DeviceGrid& grid);