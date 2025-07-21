#pragma once
/**
 * @file draw_rr_edges.h
 * 
 * draw_rr_edges.cpp contains all functions that draw lines between RR nodes.
 */

#ifndef NO_GRAPHICS

#include <cstdio>
#include <cfloat>
#include <cstring>
#include <cmath>

#include "draw_types.h"
#include "rr_graph_fwd.h"

#include "ezgl/graphics.hpp"

/**
 * @brief Draws the edge between two vertical channel nodes
 */
void draw_chany_to_chany_edge(RRNodeId from_node, RRNodeId to_node, short switch_type, ezgl::renderer* g);

/**
 * @brief Draws the edge between two horizontal channel nodes
 */
void draw_chanx_to_chanx_edge(RRNodeId from_node, RRNodeId to_node, short switch_type, ezgl::renderer* g);

/**
 * @brief Draws the edge between a horizontal channel node and a vertical channel node
 * @param chanx_node The horizontal channel node
 * @param chany_node The vertical channel node
 * @param edge_dir The direction of the edge, FROM_X_TO_Y or FROM_Y_TO_X
 * @param switch_type The type of switch used for the connection
 * @param g The ezgl renderer 
 */
void draw_chanx_to_chany_edge(RRNodeId chanx_node, RRNodeId chany_node, enum e_chan_edge_dir edge_dir, short switch_type, ezgl::renderer* g);

/**
 * @brief Draws the edge between an intra-cluster pin and an inter-cluster pin when flat routing is enabled.
 * @param intra_cluster_node The intra-cluster pin node
 * @param inter_cluster_node The inter-cluster pin node
 * @param pin_edge_dir The direction of the edge, FROM_INTER_CLUSTER_TO_INTRA_CLUSTER or FROM_INTRA_CLUSTER_TO_INTER_CLUSTER
 * @param pin_side The side of the inter-cluster pin (e.g. TOP, RIGHT, BOTTOM, LEFT)
 * @param g The ezgl renderer
 */
void draw_intra_cluster_pin_to_pin(RRNodeId intra_cluster_node, RRNodeId inter_cluster_node, e_pin_edge_dir pin_edge_dir, e_side pin_side, ezgl::renderer* g);

/**
 * @brief Draws the edge between two intra-cluster pins when flat routing is enabled.
 * @param inode The current node to draw to
 * @param prev_node The previous node to draw from
 * @param g The ezgl renderer
 */
void draw_intra_cluster_edge(RRNodeId inode, RRNodeId prev_node, ezgl::renderer* g);

/**
 * @brief This routine directly draws an edge from an inter-cluster output pin to an inter-cluster input pin.
 */
void draw_pin_to_pin(RRNodeId opin, RRNodeId ipin, ezgl::renderer* g);

//TODO: These two functions currently do not draw correctly after rearranging the block locations. They need an update.
void draw_pin_to_sink(RRNodeId ipin_node, RRNodeId sink_node, ezgl::renderer* g);

void draw_source_to_pin(RRNodeId source_node, RRNodeId opin_node, ezgl::renderer* g);

/**
 * @brief Determines the side of clb, the inter-cluster pin is located on based on the channel node.
 * @param pin_node The inter-cluster pin node
 * @param chan_node The channel node (CHANX or CHANY) that the pin is connected to
 * @return The side of the clb that the pin is located on (e.g. TOP, RIGHT, BOTTOM, LEFT)
 */
e_side get_pin_side(RRNodeId pin_node, RRNodeId chan_node);

/**
 * @brief Draws an edge from a inter-cluster pin node to a channel node (CHANX or CHANY).
 */
void draw_pin_to_chan_edge(RRNodeId pin_node, RRNodeId chan_node, ezgl::renderer* g);

#endif /* NO_GRAPHICS */
