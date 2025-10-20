#include <vector>

#include "device_grid.h"
#include "rr_graph_fwd.h"
#include "rr_graph_view.h"
#include "rr_node_types.h"
#include "vtr_assert.h"

#include "interposer_cut.h"


static bool should_cut(int src_start_loc, int sink_start_loc, int cut_loc) {
    int src_delta = src_start_loc - cut_loc;
    int sink_delta = sink_start_loc - cut_loc;
    
    // Same sign means that both sink and source are on the same side of this cut
    if ((src_delta < 0 && sink_delta < 0) || (src_delta >= 0 && sink_delta >= 0)) {
        return false;
    } else {
        return true;
    }
}

static short node_xstart(const RRGraphView& rr_graph, RRNodeId node) {
    // Return early for OPIN and IPIN types (Some BIDIR pins would trigger the assertion below)
    if (rr_graph.node_type(node) == e_rr_type::OPIN || rr_graph.node_type(node) == e_rr_type::IPIN) {
        VTR_ASSERT(rr_graph.node_xlow(node) == rr_graph.node_xhigh(node));
        return rr_graph.node_xlow(node);
    }

    switch (rr_graph.node_direction(node)) {
        case Direction::DEC:
            return rr_graph.node_xhigh(node);
            break;

        case Direction::INC:
            return rr_graph.node_xlow(node);
            break;

        case Direction::NONE:
            VTR_ASSERT(rr_graph.node_xlow(node) == rr_graph.node_xhigh(node));
            return (rr_graph.node_xlow(node));
            break;

        case Direction::BIDIR:
            VTR_ASSERT_MSG(false, "Bidir node has no starting point");
            break;

        default:
            VTR_ASSERT(false);
            break;
    }
}

static short node_ystart(const RRGraphView& rr_graph, RRNodeId node) {
    // Return early for OPIN and IPIN types (Some BIDIR pins would trigger the assertion below)
    if (rr_graph.node_type(node) == e_rr_type::OPIN || rr_graph.node_type(node) == e_rr_type::IPIN) {
        return rr_graph.node_ylow(node);
    }

    switch(rr_graph.node_direction(node)) {
        case Direction::DEC:
            return rr_graph.node_yhigh(node);
            break;

        case Direction::INC:
            return rr_graph.node_ylow(node);
            break;

        case Direction::NONE:
            VTR_ASSERT(rr_graph.node_ylow(node) == rr_graph.node_yhigh(node));
            return (rr_graph.node_ylow(node));
            break;

        case Direction::BIDIR:
            VTR_ASSERT_MSG(false, "Bidir node has no starting point");
            break;

        default:
            VTR_ASSERT(false);
            break;
    }
}

std::vector<RREdgeId> mark_interposer_cut_edges_for_removal(const RRGraphView& rr_graph, const DeviceGrid& grid) {
    std::vector<RREdgeId> edges_to_be_removed;

    // Loop over all RREdgeIds and mark ones that cross a cutline to be removed
    
    for (RREdgeId edge_id : rr_graph.all_edges()) {
        RRNodeId src_node = rr_graph.edge_src_node(edge_id);
        RRNodeId sink_node = rr_graph.edge_sink_node(edge_id);

        // TODO: ignoring chanz nodes for now
        if (rr_graph.node_type(src_node) == e_rr_type::CHANZ || rr_graph.node_type(sink_node) == e_rr_type::CHANZ) {
            continue;
        }

        VTR_ASSERT(rr_graph.node_layer(src_node) == rr_graph.node_layer(sink_node));

        int layer = rr_graph.node_layer(src_node);

        for (int cut_loc_y : grid.get_horizontal_cuts()[layer]) {
            int src_start_loc_y = node_ystart(rr_graph, src_node);
            int sink_start_loc_y = node_ystart(rr_graph, sink_node);

            if(should_cut(src_start_loc_y, sink_start_loc_y, cut_loc_y)) {
                edges_to_be_removed.push_back(edge_id);
            }
        }

        for (int cut_loc_x : grid.get_vertical_cuts()[layer]) {
            int src_start_loc_x = node_xstart(rr_graph, src_node);
            int sink_start_loc_x = node_xstart(rr_graph, sink_node);
            
            if(should_cut(src_start_loc_x, sink_start_loc_x, cut_loc_x)) {
                edges_to_be_removed.push_back(edge_id);
            }
        }
    }

    return edges_to_be_removed;
}