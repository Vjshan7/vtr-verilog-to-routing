/**
 * @file
 * @author  Alex Singer
 * @date    September 2024
 * @brief   Implements the full legalizer in the AP flow. The Full Legalizer
 *          takes a partial placement and fully legalizes it. This involves
 *          creating legal clusters and placing them into valid tile sites.
 */

#include "full_legalizer.h"

#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>
#include <queue>
#include <execution>

#include "PreClusterTimingManager.h"
#include "ShowSetup.h"
#include "ap_flow_enums.h"
#include "ap_netlist_fwd.h"
#include "blk_loc_registry.h"
#include "check_netlist.h"
#include "cluster_legalizer.h"
#include "cluster_util.h"
#include "clustered_netlist.h"
#include "device_grid.h"
#include "flat_placement_types.h"
#include "globals.h"
#include "initial_placement.h"
#include "load_flat_place.h"
#include "logic_types.h"
#include "noc_place_utils.h"
#include "pack.h"
#include "partial_placement.h"
#include "physical_types.h"
#include "place.h"
#include "place_and_route.h"
#include "place_constraints.h"
#include "place_macro.h"
#include "prepack.h"
#include "read_place.h"
#include "verify_clustering.h"
#include "verify_placement.h"
#include "vpr_api.h"
#include "vpr_context.h"
#include "vpr_error.h"
#include "vpr_types.h"
#include "vtr_assert.h"
#include "vtr_geometry.h"
#include "vtr_ndmatrix.h"
#include "vtr_random.h"
#include "vtr_strong_id.h"
#include "vtr_time.h"
#include "vtr_vector.h"

#include "physical_types_util.h"

std::unique_ptr<FullLegalizer> make_full_legalizer(e_ap_full_legalizer full_legalizer_type,
                                                   const APNetlist& ap_netlist,
                                                   const AtomNetlist& atom_netlist,
                                                   const Prepacker& prepacker,
                                                   const PreClusterTimingManager& pre_cluster_timing_manager,
                                                   const t_vpr_setup& vpr_setup,
                                                   const t_arch& arch,
                                                   const DeviceGrid& device_grid) {
    switch (full_legalizer_type) {
        case e_ap_full_legalizer::Naive:
            return std::make_unique<NaiveFullLegalizer>(ap_netlist,
                                                        atom_netlist,
                                                        prepacker,
                                                        pre_cluster_timing_manager,
                                                        vpr_setup,
                                                        arch,
                                                        device_grid);
        case e_ap_full_legalizer::APPack:
            return std::make_unique<APPack>(ap_netlist,
                                            atom_netlist,
                                            prepacker,
                                            pre_cluster_timing_manager,
                                            vpr_setup,
                                            arch,
                                            device_grid);
        case e_ap_full_legalizer::Basic_Min_Disturbance:
            VTR_LOG("Basic Minimum Disturbance Full Legalizer selected!\n");
            return std::make_unique<BasicMinDisturbance>(ap_netlist,
                                                        atom_netlist,
                                                        prepacker,
                                                        pre_cluster_timing_manager,
                                                        vpr_setup,
                                                        arch,
                                                        device_grid);
        default:
            VPR_FATAL_ERROR(VPR_ERROR_AP,
                            "Unrecognized full legalizer type");
    }
}

namespace {

/// @brief A unique ID for each root tile on the device.
///
/// This is used for putting the molecules in bins for packing.
// FIXME: Bring this into the device_grid.
//  - Maybe this can be called DeviceRootTileId or something.
struct device_tile_id_tag {};
typedef vtr::StrongId<device_tile_id_tag, size_t> DeviceTileId;

/**
 * @brief Helper class to place cluster in the AP context.
 *
 * A lot of this code was lifted from the Initial Placer within the placement
 * flow.
 * TODO: Should try to do the same thing we did with the ClusterLegalizer to
 *       unify the two flows and make it more stable!
 */
class APClusterPlacer {
  private:
    // Get the macro for the given cluster block.
    t_pl_macro get_macro(ClusterBlockId clb_blk_id) {
        // Basically stolen from initial_placement.cpp:place_one_block
        // TODO: Make this a cleaner interface and share the code.
        int imacro = place_macros_.get_imacro_from_iblk(clb_blk_id);

        // If this block is part of a macro, return it.
        if (imacro != -1) {
            return place_macros_[imacro];
        }
        // If not, create a "fake" macro with a single element.
        t_pl_macro_member macro_member;
        t_pl_offset block_offset(0, 0, 0, 0);
        macro_member.blk_index = clb_blk_id;
        macro_member.offset = block_offset;

        t_pl_macro pl_macro;
        pl_macro.members.push_back(macro_member);
        return pl_macro;
    }

    const PlaceMacros& place_macros_;

  public:
    /**
     * @brief Constructor for the APClusterPlacer
     *
     * Initializes internal and global state necessary to place clusters on the
     * FPGA device.
     */
    APClusterPlacer(const PlaceMacros& place_macros,
                    const char* constraints_file)
        : place_macros_(place_macros) {
        // Initialize the block loc registry.
        auto& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();
        blk_loc_registry.init();

        // Place the fixed blocks and mark them as fixed.
        mark_fixed_blocks(blk_loc_registry);

        // Read the constraint file and place fixed blocks.
        if (strlen(constraints_file) != 0) {
            read_constraints(constraints_file, blk_loc_registry);
        }

        // Update the block loc registry with the fixed / moveable blocks.
        // We can do this here since the fixed blocks will not change beyond
        // this point.
        blk_loc_registry.alloc_and_load_movable_blocks();
    }

    /**
     * @brief Given a cluster and tile it wants to go into, try to place the
     *        cluster at this tile's postion.
     */
    bool place_cluster_reconstruction(ClusterBlockId clb_blk_id,
                       const t_physical_tile_loc& tile_loc,
                       int sub_tile) {
        const DeviceContext& device_ctx = g_vpr_ctx.device();
        const FloorplanningContext& floorplanning_ctx = g_vpr_ctx.floorplanning();
        const ClusteringContext& cluster_ctx = g_vpr_ctx.clustering();
        const auto& block_locs = g_vpr_ctx.placement().block_locs();
        auto& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();
        // If this block has already been placed, just return true.
        // TODO: This should be investigated further. What I think is happening
        //       is that a macro is being placed which contains another cluster.
        //       This must be a carry chain. May need to rewrite the algorithm
        //       below to use macros instead of clusters.
        if (is_block_placed(clb_blk_id, block_locs))
            return true;
        VTR_ASSERT(!is_block_placed(clb_blk_id, block_locs) && "Block already placed. Is this intentional?");
        t_pl_macro pl_macro = get_macro(clb_blk_id);
        t_pl_loc to_loc;
        to_loc.x = tile_loc.x;
        to_loc.y = tile_loc.y;
        to_loc.layer = tile_loc.layer_num;
        // Special case where the tile has no sub-tiles. It just cannot be placed.
        if (device_ctx.grid.get_physical_type(tile_loc)->sub_tiles.size() == 0)
            return false;
        VTR_ASSERT(sub_tile >= 0 && sub_tile < device_ctx.grid.get_physical_type(tile_loc)->capacity);
        // Check if this cluster is constrained and this location is legal.
        if (is_cluster_constrained(clb_blk_id)) {
            const auto& cluster_constraints = floorplanning_ctx.cluster_constraints;
            if (cluster_constraints[clb_blk_id].is_loc_in_part_reg(to_loc))
                return false;
        }
        // If the location is legal, try to exhaustively place it at this tile
        // location. This should try all sub_tiles.
        PartitionRegion pr;
        vtr::Rect<int> rect(tile_loc.x, tile_loc.y, tile_loc.x, tile_loc.y);
        pr.add_to_part_region(Region(rect, to_loc.layer));
        const ClusteredNetlist& clb_nlist = cluster_ctx.clb_nlist;
        t_logical_block_type_ptr block_type = clb_nlist.block_type(clb_blk_id);
        enum e_pad_loc_type pad_loc_type = g_vpr_ctx.device().pad_loc_type;
        
        to_loc.sub_tile = sub_tile;
        return try_place_macro(pl_macro, to_loc, blk_loc_registry);
    }
    
    /**
     * @brief Given a cluster and tile it wants to go into, try to place the
     *        cluster at this tile's postion.
     */
    bool place_cluster(ClusterBlockId clb_blk_id,
                       const t_physical_tile_loc& tile_loc,
                       int sub_tile) {
        const DeviceContext& device_ctx = g_vpr_ctx.device();
        const FloorplanningContext& floorplanning_ctx = g_vpr_ctx.floorplanning();
        const ClusteringContext& cluster_ctx = g_vpr_ctx.clustering();
        const auto& block_locs = g_vpr_ctx.placement().block_locs();
        auto& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();
        // If this block has already been placed, just return true.
        // TODO: This should be investigated further. What I think is happening
        //       is that a macro is being placed which contains another cluster.
        //       This must be a carry chain. May need to rewrite the algorithm
        //       below to use macros instead of clusters.
        if (is_block_placed(clb_blk_id, block_locs))
            return true;
        VTR_ASSERT(!is_block_placed(clb_blk_id, block_locs) && "Block already placed. Is this intentional?");
        t_pl_macro pl_macro = get_macro(clb_blk_id);
        t_pl_loc to_loc;
        to_loc.x = tile_loc.x;
        to_loc.y = tile_loc.y;
        to_loc.layer = tile_loc.layer_num;
        // Special case where the tile has no sub-tiles. It just cannot be placed.
        if (device_ctx.grid.get_physical_type(tile_loc)->sub_tiles.size() == 0)
            return false;
        VTR_ASSERT(sub_tile >= 0 && sub_tile < device_ctx.grid.get_physical_type(tile_loc)->capacity);
        // Check if this cluster is constrained and this location is legal.
        if (is_cluster_constrained(clb_blk_id)) {
            const auto& cluster_constraints = floorplanning_ctx.cluster_constraints;
            if (cluster_constraints[clb_blk_id].is_loc_in_part_reg(to_loc))
                return false;
        }
        // If the location is legal, try to exhaustively place it at this tile
        // location. This should try all sub_tiles.
        PartitionRegion pr;
        vtr::Rect<int> rect(tile_loc.x, tile_loc.y, tile_loc.x, tile_loc.y);
        pr.add_to_part_region(Region(rect, to_loc.layer));
        const ClusteredNetlist& clb_nlist = cluster_ctx.clb_nlist;
        t_logical_block_type_ptr block_type = clb_nlist.block_type(clb_blk_id);
        enum e_pad_loc_type pad_loc_type = g_vpr_ctx.device().pad_loc_type;
        // FIXME: This currently ignores the sub_tile. Was running into issues
        //        with trying to force clusters to specific sub_tiles.
        return try_place_macro_exhaustively(pl_macro, pr, block_type,
                                            pad_loc_type, blk_loc_registry);
    }

    // This is not the best way of doing things, but its the simplest. Given a
    // cluster, just find somewhere for it to go.
    // TODO: Make this like the initial placement code where we first try
    //       centroid, then random, then exhaustive.
    bool exhaustively_place_cluster(ClusterBlockId clb_blk_id) {
        const auto& block_locs = g_vpr_ctx.placement().block_locs();
        auto& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();
        // If this block has already been placed, just return true.
        // TODO: See similar comment above.
        if (is_block_placed(clb_blk_id, block_locs))
            return true;
        VTR_ASSERT(!is_block_placed(clb_blk_id, block_locs) && "Block already placed. Is this intentional?");
        t_pl_macro pl_macro = get_macro(clb_blk_id);
        const PartitionRegion& pr = is_cluster_constrained(clb_blk_id) ? g_vpr_ctx.floorplanning().cluster_constraints[clb_blk_id] : get_device_partition_region();
        t_logical_block_type_ptr block_type = g_vpr_ctx.clustering().clb_nlist.block_type(clb_blk_id);
        // FIXME: We really should get this from the place context, not the device context.
        //      - Stealing it for now to get this to work.
        enum e_pad_loc_type pad_loc_type = g_vpr_ctx.device().pad_loc_type;
        return try_place_macro_exhaustively(pl_macro, pr, block_type, pad_loc_type, blk_loc_registry);
    }
};

} // namespace

/**
 * @brief Create a new cluster for the given seed molecule using the cluster
 *        legalizer.
 *
 *  @param seed_molecule                    The molecule to use as a starting
 *                                          point for the cluster.
 *  @param cluster_legalizer                A cluster legalizer object to build
 *                                          the cluster.
 *  @param primitive_candidate_block_types  A list of candidate block types for
 *                                          the given molecule.
 */
static LegalizationClusterId create_new_cluster(PackMoleculeId seed_molecule_id,
                                                const Prepacker& prepacker,
                                                ClusterLegalizer& cluster_legalizer,
                                                const vtr::vector<LogicalModelId, std::vector<t_logical_block_type_ptr>>& primitive_candidate_block_types) {
    const AtomContext& atom_ctx = g_vpr_ctx.atom();
    // This was stolen from pack/cluster_util.cpp:start_new_cluster
    // It tries to find a block type and mode for the given molecule.
    // TODO: This should take into account the tile this molecule wants to be
    //       placed into.
    // TODO: The original implementation sorted based on balance. Perhaps this
    //       should do the same.
    VTR_ASSERT(seed_molecule_id.is_valid());
    const t_pack_molecule& seed_molecule = prepacker.get_molecule(seed_molecule_id);
    AtomBlockId root_atom = seed_molecule.atom_block_ids[seed_molecule.root];
    LogicalModelId root_model_id = atom_ctx.netlist().block_model(root_atom);

    VTR_ASSERT(root_model_id.is_valid());
    VTR_ASSERT(!primitive_candidate_block_types[root_model_id].empty());
    const std::vector<t_logical_block_type_ptr>& candidate_types = primitive_candidate_block_types[root_model_id];

    for (t_logical_block_type_ptr type : candidate_types) {
        int num_modes = type->pb_graph_head->pb_type->num_modes;
        for (int mode = 0; mode < num_modes; mode++) {
            e_block_pack_status pack_status = e_block_pack_status::BLK_STATUS_UNDEFINED;
            LegalizationClusterId new_cluster_id;
            std::tie(pack_status, new_cluster_id) = cluster_legalizer.start_new_cluster(seed_molecule_id, type, mode);
            if (pack_status == e_block_pack_status::BLK_PASSED)
                return new_cluster_id;
        }
    }
    // This should never happen.
    VPR_FATAL_ERROR(VPR_ERROR_AP,
                    "Unable to create a cluster for the given seed molecule");
    return LegalizationClusterId();
}

// /*
// Initializes the grids to hold the LegalizationCluster's created
// */
// void BasicMinDisturbance::initialize_cluster_grids() {
//     VTR_LOG("You are in initialize_cluster_grids()\n");

//     const DeviceGrid& device_grid = g_vpr_ctx.device().grid;

//     size_t grid_width = device_grid.width();
//     size_t grid_height = device_grid.height();
//     size_t num_layers = device_grid.get_num_layers();

//     cluster_grids = ClusterGridReconstruction(num_layers, grid_width, grid_height);
//     tile_type.resize({num_layers, grid_width, grid_height});

//     for (size_t layer_num = 0; layer_num < num_layers; layer_num++) {
//         for (size_t x = 0; x < grid_width; x++) {
//             for (size_t y = 0; y < grid_height; y++) {
//                 t_physical_tile_loc tile_loc = {(int)x, (int)y, (int)layer_num};
//                 auto type = device_grid.get_physical_type(tile_loc);
//                 int num_subtiles = type->capacity;

//                 cluster_grids.initialize_tile(layer_num, x, y, num_subtiles);
//                 tile_type[layer_num][x][y] = type;
//             }
//         }
//     }

//     VTR_LOG("Cluster grids initialized with dimensions: layers=%zu, width=%zu, height=%zu\n",
//             num_layers, grid_width, grid_height);
// }

void BasicMinDisturbance::place_clusters(const ClusteredNetlist& clb_nlist,
                                         const PlaceMacros& place_macros,
                                         std::unordered_map<LegalizationClusterId, ClusterBlockId> legalization_id_to_cluster_id) {
    vtr::ScopedStartFinishTimer actual_place_clusters("Actual Place Clusters");
    VTR_LOG("=== BasicMinDisturbance::place_clusters ===\n");
    std::vector<ClusterBlockId> unplaced_clusters;
                                            
    APClusterPlacer ap_cluster_placer(place_macros, vpr_setup_.PlacerOpts.constraints_file.c_str());
    for (const auto [loc, legalization_cluster_id]: loc_to_cluster_id_placed) {
        const ClusterBlockId clb_index = legalization_id_to_cluster_id[legalization_cluster_id];
        t_physical_tile_loc tile_loc = {loc.x,loc.y,loc.layer};
        bool placed = ap_cluster_placer.place_cluster_reconstruction(clb_index, tile_loc, loc.sub_tile);
        if (!placed) {
            // Add to list of unplaced clusters.
            unplaced_clusters.push_back(clb_index);
        }
    }

    // if (!unplaced_clusters.empty()) {
    //     VPR_FATAL_ERROR(VPR_ERROR_AP, "BasicMinDisturbance unplaced cluster policy is not implemented yet. Number of unplaced clusters is %zu\n.", unplaced_clusters.size());
    // }
    VTR_LOG("Number of unplaced clusters to determined locations is %zu out of %zu clusters.\n", unplaced_clusters.size(), clb_nlist.blocks().size());

    // Any clusters that were not placed previously are exhaustively placed.
    for (ClusterBlockId clb_blk_id : unplaced_clusters) {
        bool success = ap_cluster_placer.exhaustively_place_cluster(clb_blk_id);
        if (!success) {
            VPR_FATAL_ERROR(VPR_ERROR_AP,
                            "Unable to find valid place for cluster in AP placement!");
        }
    }
}




// Idea is to get the logical block type of a molecule and implementation is inspired by the create_new_cluster function
t_logical_block_type_ptr get_molecule_logical_block_type(
    PackMoleculeId mol_id,
    const Prepacker& prepacker,
    const vtr::vector<LogicalModelId, std::vector<t_logical_block_type_ptr>>& primitive_candidate_block_types) {
    
    const AtomContext& atom_ctx = g_vpr_ctx.atom();
    const t_pack_molecule& molecule = prepacker.get_molecule(mol_id);

    AtomBlockId root_atom = molecule.atom_block_ids[molecule.root];

    if (!root_atom.is_valid()) {
        VTR_LOG_WARN("Molecule ID %zu does not have a valid root atom!\n", size_t(mol_id));
        return nullptr;
    }

    // ✅ Use LogicalModelId (not t_model*)
    LogicalModelId root_model_id = atom_ctx.netlist().block_model(root_atom);
    if (!root_model_id.is_valid()) {
        VTR_LOG_WARN("Molecule ID %zu has an invalid root model ID!\n", size_t(mol_id));
        return nullptr;
    }

    // ✅ Access by index, not .find()
    const auto& candidate_types = primitive_candidate_block_types[root_model_id];
    if (!candidate_types.empty()) {
        return candidate_types.front();
    }

    VTR_LOG_WARN("Molecule ID %zu has no valid logical block type!\n", size_t(mol_id));
    return nullptr;
}


bool is_root_tile(const DeviceGrid& grid, const t_physical_tile_loc& tile_loc) {
    return grid.get_width_offset(tile_loc) == 0 && 
           grid.get_height_offset(tile_loc) == 0;
}

void BasicMinDisturbance::place_remaining_clusters(ClusterLegalizer& cluster_legalizer,
                       const DeviceGrid& device_grid,
                       std::unordered_map<t_physical_tile_loc, std::vector<LegalizationClusterId>>& cluster_id_to_loc_unplaced) {

    // Process all unplaced clusters
    auto unplaced_copy = cluster_id_to_loc_unplaced; // Copy for safe iteration
    for (const auto& [orig_loc, clusters] : unplaced_copy) {
        for (auto cluster_id : clusters) {
            bool placed = false;
            const int max_search_radius = std::max(device_grid.width(), device_grid.height());
            int search_radius = 0;

            // Get cluster type once
            const auto cluster_type = cluster_legalizer.get_cluster_type(cluster_id);

            while (!placed && search_radius <= max_search_radius) {
                // Check all positions at current Manhattan distance
                for (int dx = -search_radius; dx <= search_radius; ++dx) {
                    for (int dy = -search_radius; dy <= search_radius; ++dy) {
                        // Manhattan distance check
                        if (std::abs(dx) + std::abs(dy) != search_radius) continue;

                        const int x = orig_loc.x + dx;
                        const int y = orig_loc.y + dy;
                        const int layer = orig_loc.layer_num;

                        // Skip invalid coordinates
                        if (x < 0 || y < 0 || x >= device_grid.width() || y >= device_grid.height()) continue;

                        // Get tile information
                        const t_physical_tile_loc tile_loc{x, y, layer};
                        const auto tile_type = device_grid.get_physical_type(tile_loc);
                        
                        // Skip incompatible tiles
                        if (!is_tile_compatible(tile_type, cluster_type)) continue;

                        // Check all subtiles
                        const int capacity = tile_type->capacity;
                        for (int sub_tile = 0; sub_tile < capacity; ++sub_tile) {
                            if (!is_root_tile(device_grid, tile_loc)) {
                                break;
                            }
                            t_pl_loc candidate_loc{x, y, sub_tile, layer};
                            
                            // Skip occupied locations
                            if (loc_to_cluster_id_placed.count(candidate_loc)) continue;

                            // Update data structures
                            loc_to_cluster_id_placed[candidate_loc] = cluster_id;
                            auto& cluster_vec = cluster_id_to_loc_unplaced[orig_loc];
                            cluster_vec.erase(std::remove(cluster_vec.begin(), cluster_vec.end(), cluster_id), cluster_vec.end());
                            if (cluster_vec.empty()) {
                                cluster_id_to_loc_unplaced.erase(orig_loc);
                            }
                            
                            placed = true;
                            break;
                        }
                        if (placed) break;
                    }
                    if (placed) break;
                }
                
                // Expand search area if not placed
                if (!placed) {
                    VTR_LOGV_DEBUG(3, "No placement found for cluster %zu at radius %d\n",
                                  size_t(cluster_id), search_radius);
                    search_radius++;
                }
            }

            if (!placed) {
                VTR_LOGV_DEBUG(VPR_ERROR_AP,
                    "Failed to place cluster %zu after exhaustive search (radius %d) around (%d,%d) layer %d\n",
                    size_t(cluster_id), max_search_radius, orig_loc.x, orig_loc.y, orig_loc.layer_num);
            }
        }
    }
}

bool has_empty_primitive(t_pb* pb) {
    if (!pb) return false;
    const t_pb_type* type = pb->pb_graph_node->pb_type;

    if (type->num_modes == 0) {
        return (pb->name == nullptr); // empty primitive
    }

    if (pb->child_pbs == nullptr) return true;

    for (int i = 0; i < type->modes[pb->mode].num_pb_type_children; ++i) {
        for (int j = 0; j < type->modes[pb->mode].pb_type_children[i].num_pb; ++j) {
            if (has_empty_primitive(&pb->child_pbs[i][j])) return true;
        }
    }

    return false;
}

void BasicMinDisturbance::neighbor_cluster_pass(
    ClusterLegalizer& cluster_legalizer,
    const DeviceGrid& device_grid,
    const vtr::vector<LogicalModelId, std::vector<t_logical_block_type_ptr>>& primitive_candidate_block_types,
    std::vector<std::pair<PackMoleculeId, t_physical_tile_loc>>& unclustered_blocks,
    std::unordered_map<t_physical_tile_loc, std::vector<PackMoleculeId>>& unclustered_block_locs,
    std::unordered_map<t_physical_tile_loc, std::vector<LegalizationClusterId>>& cluster_id_to_loc_unplaced,
    ClusterLegalizationStrategy strategy,
    int search_radius) {

    std::unordered_set<PackMoleculeId> clustered_molecules;

    const auto unclustered_blocks_copy = unclustered_blocks;
    for (const auto& [mol_id, seed_tile_loc] : unclustered_blocks_copy) {
        if (clustered_molecules.count(mol_id)) continue;

        LegalizationClusterId cluster_id = create_new_cluster(mol_id, prepacker_, cluster_legalizer, primitive_candidate_block_types);
        //cluster_id_to_loc_desired[cluster_id] = seed_tile_loc;
        clustered_molecules.insert(mol_id);
        //cluster_id_to_loc_unplaced[seed_tile_loc].push_back(cluster_id);

        auto try_cluster_tile = [&](const t_physical_tile_loc& tile_loc) {
            auto it_tile = unclustered_block_locs.find(tile_loc);
            if (it_tile == unclustered_block_locs.end()) return;

            auto& mol_list = it_tile->second;
            for (auto it = mol_list.begin(); it != mol_list.end();) {
                PackMoleculeId neighbor_mol = *it;

                // FIXME: ensure we skip already clustered molecules
                if (cluster_legalizer.is_mol_clustered(neighbor_mol)) {
                    it = mol_list.erase(it); // remove it, it shouldn't be retried
                    continue;
                }
                
                if (clustered_molecules.count(neighbor_mol)) {
                    it = mol_list.erase(it);
                    continue;
                }

                if (cluster_legalizer.is_molecule_compatible(neighbor_mol, cluster_id) &&
                    cluster_legalizer.add_mol_to_cluster(neighbor_mol, cluster_id) == e_block_pack_status::BLK_PASSED) {
                    clustered_molecules.insert(neighbor_mol);
                    it = mol_list.erase(it);
                } else {
                    ++it;
                }
            }

            if (mol_list.empty()) {
                unclustered_block_locs.erase(tile_loc);
            }
        };

        // Try clustering molecules at seed tile
        try_cluster_tile(seed_tile_loc);

        // Try neighbor tiles in BFS-like increasing Manhattan distance
        for (int r = 1; r <= search_radius; ++r) {
            for (int dx = -r; dx <= r; ++dx) {
                for (int dy = -r; dy <= r; ++dy) {
                    if (std::abs(dx) + std::abs(dy) != r) continue;

                    int nx = seed_tile_loc.x + dx;
                    int ny = seed_tile_loc.y + dy;
                    int layer = seed_tile_loc.layer_num;

                    t_physical_tile_loc neighbor_tile{nx, ny, layer};
                    // Skip early if there's no molecule at this tile
                    if (!unclustered_block_locs.count(neighbor_tile)) continue;
                    try_cluster_tile(neighbor_tile);

                    if (!has_empty_primitive(cluster_legalizer.get_cluster_pb(cluster_id)))
                        goto skip_remaining_neighbors;
                }
            }
        }
        skip_remaining_neighbors:;
        
        if (strategy == ClusterLegalizationStrategy::FULL) {
            cluster_id_to_loc_unplaced[seed_tile_loc].push_back(cluster_id);
            cluster_id_to_loc_desired[cluster_id] = seed_tile_loc;
            cluster_legalizer.clean_cluster(cluster_id);
            continue;
        }
        
        if (cluster_legalizer.check_cluster_legality(cluster_id)) {
            cluster_id_to_loc_unplaced[seed_tile_loc].push_back(cluster_id);
            cluster_id_to_loc_desired[cluster_id] = seed_tile_loc;
            cluster_legalizer.clean_cluster(cluster_id);
        } else {
            for (auto mol_id: cluster_legalizer.get_cluster_molecules(cluster_id)) {
                unclustered_blocks.push_back({mol_id, seed_tile_loc});
                unclustered_block_locs[seed_tile_loc].push_back(mol_id);
                clustered_molecules.erase(clustered_molecules.find(mol_id));
            }
            //VTR_LOG("\tCluster %zu has %zu molecules\n", cluster_id, cluster_legalizer.get_cluster_molecules(cluster_id).size());
            //VTR_LOG("\tUnclustered block count: %zu\n", unclustered_blocks.size());
            cluster_legalizer.destroy_cluster(cluster_id);
        }
    }

    // Final cleanup of clustered molecules from unclustered_blocks
    unclustered_blocks.erase(
        std::remove_if(unclustered_blocks.begin(), unclustered_blocks.end(),
                       [&](const auto& p) { return clustered_molecules.count(p.first); }),
        unclustered_blocks.end());



}


void BasicMinDisturbance::pack_recontruction_pass(ClusterLegalizer& cluster_legalizer,
                                                  const PartialPlacement& p_placement) 
{
    vtr::ScopedStartFinishTimer pack_reconstruction_timer("Pack Reconstruction");
    
    VTR_LOG("===> Before sorting molecules: \t(time: %f sec, max_rss: %f mib, delta_max_rss: %f mib)\n", pack_reconstruction_timer.elapsed_sec(), pack_reconstruction_timer.max_rss_mib(), pack_reconstruction_timer.delta_max_rss_mib());

    const DeviceGrid& device_grid = g_vpr_ctx.device().grid;
    VTR_LOG("Device (width, height): (%zu,%zu)\n", device_grid.width(), device_grid.height());

    //std::unordered_map<t_pl_loc, LegalizationClusterId> loc_to_cluster_id_placed;
    std::vector<std::pair<PackMoleculeId, t_physical_tile_loc>> unclustered_blocks;

    vtr::vector<LogicalModelId, std::vector<t_logical_block_type_ptr>>
        primitive_candidate_block_types = identify_primitive_candidate_block_types();

    std::unordered_map<APBlockId, std::tuple<t_physical_tile_loc, int, t_logical_block_type_ptr>> unclustered_block_info;

    std::unordered_map<t_physical_tile_loc, std::vector<LegalizationClusterId>> cluster_id_to_loc_unplaced;

    std::unordered_map<t_physical_tile_loc, std::vector<PackMoleculeId>> unclustered_block_locs;

    // Cache molecule stats first
    std::unordered_map<PackMoleculeId, int> molecule_ext_inps_cache;
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId mol_id = ap_netlist_.block_molecule(ap_blk_id);
        if (!molecule_ext_inps_cache.count(mol_id)) {
            molecule_ext_inps_cache[mol_id] = 
                prepacker_.calc_molecule_stats(mol_id, atom_netlist_, arch_.models).num_used_ext_inputs;
        }
    }

    // Create compact sorting structure
    struct BlockSortInfo {
        APBlockId blk_id;
        int ext_inps;
    };
    std::vector<BlockSortInfo> sorted_blocks;
    sorted_blocks.reserve(ap_netlist_.blocks().size());

    // Populate with cached values
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId mol_id = ap_netlist_.block_molecule(ap_blk_id);
        sorted_blocks.push_back({
            ap_blk_id,
            molecule_ext_inps_cache.at(mol_id)
        });
    }

    // Parallel sort using TBB (or std::sort if not available)
    std::sort(std::execution::par_unseq, sorted_blocks.begin(), sorted_blocks.end(),
        [](const BlockSortInfo& a, const BlockSortInfo& b) {
            return a.ext_inps > b.ext_inps;  // Descending order
        });

    float first_pass_start_time = pack_reconstruction_timer.elapsed_sec();

    VTR_LOG("===> Before Reconstruction Pass: \t(time: %f sec, max_rss: %f mib, delta_max_rss: %f mib)\n", pack_reconstruction_timer.elapsed_sec(), pack_reconstruction_timer.max_rss_mib(), pack_reconstruction_timer.delta_max_rss_mib());
    
    // Grouping the molecules per tile for reconstruction pass (to be able to clean cluster right after)
    std::unordered_map<t_physical_tile_loc, std::vector<APBlockId>> tile_blocks;
    for (const BlockSortInfo& block_info : sorted_blocks) {
        APBlockId ap_blk_id = block_info.blk_id;
        const t_physical_tile_loc tile_loc = p_placement.get_containing_tile_loc(ap_blk_id);
        tile_blocks[tile_loc].push_back(ap_blk_id);
    }

    size_t cluster_created_mid_first_pass = 0;
    for (const auto& [key, value] : tile_blocks) {
        t_physical_tile_loc tile_loc = key;
        std::vector<APBlockId> tile_blocks = value;
        const auto tile_type = device_grid.get_physical_type(tile_loc);
        //std::vector<LegalizationClusterId> cluster_ids_to_check;
        std::unordered_map<LegalizationClusterId, t_pl_loc> cluster_ids_to_check;
        for (APBlockId ap_blk_id: tile_blocks) {
            PackMoleculeId mol_id = ap_netlist_.block_molecule(ap_blk_id);
            const auto& mol = prepacker_.get_molecule(mol_id);
            const auto block_type = get_molecule_logical_block_type(mol_id, prepacker_, primitive_candidate_block_types);
            if (!block_type) {
                VPR_FATAL_ERROR(VPR_ERROR_AP, "Could not determine block type for molecule ID %zu\n", size_t(mol_id));
            }

            bool placed = false;

            // Try all subtiles in a single loop
            for (int sub_tile = 0; sub_tile < tile_type->capacity; ++sub_tile) {
                if (!is_root_tile(device_grid, tile_loc)) {
                    break;
                }

                const t_pl_loc loc{tile_loc.x, tile_loc.y, sub_tile, tile_loc.layer_num};
                auto cluster_it = loc_to_cluster_id_placed.find(loc);

                if (cluster_it != loc_to_cluster_id_placed.end()) {
                    // Try adding to existing cluster
                    LegalizationClusterId cluster_id = cluster_it->second;
                    // If you still want to double-check
                    if (!has_empty_primitive(cluster_legalizer.get_cluster_pb(cluster_id))) {
                        //VTR_LOG("Catched a non-empty cluster (id: %zu)!\n", cluster_id); // moderate cost, fairly accurate
                        continue;
                    }
                    if (cluster_legalizer.is_molecule_compatible(mol_id, cluster_id) &&
                        cluster_legalizer.add_mol_to_cluster(mol_id, cluster_id) == e_block_pack_status::BLK_PASSED) {
                        placed = true;
                        break;
                    }
                } else if (is_tile_compatible(tile_type, block_type)) {
                    // Create new cluster
                    LegalizationClusterId new_id = create_new_cluster(mol_id, prepacker_, cluster_legalizer, primitive_candidate_block_types);
                    cluster_ids_to_check[new_id] = loc;
                    loc_to_cluster_id_placed[loc] = new_id;
                    cluster_id_to_loc_desired[new_id] = tile_loc;
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                unclustered_blocks.push_back({mol_id, tile_loc});
                if (unclustered_block_locs.find(tile_loc) != unclustered_block_locs.end()) {
                    unclustered_block_locs[tile_loc].push_back(mol_id);
                } else {
                    unclustered_block_locs[tile_loc] = {mol_id};
                }
            }
        }

        // get the illegal clusters' molecules
        std::vector<PackMoleculeId> illegal_cluster_mols;
        for (const auto& [cluster_id, loc] : cluster_ids_to_check) {
            if (!cluster_legalizer.check_cluster_legality(cluster_id)) {
                for (auto mol_id: cluster_legalizer.get_cluster_molecules(cluster_id)) {
                    //unclustered_blocks.push_back({mol_id, tile_loc});
                    //unclustered_block_locs[tile_loc].push_back(mol_id);
                    illegal_cluster_mols.push_back(mol_id);
                }
                //VTR_LOG("\tCluster %zu has %zu molecules\n", cluster_id, cluster_legalizer.get_cluster_molecules(cluster_id).size());
                //VTR_LOG("\tUnclustered block count: %zu\n", unclustered_blocks.size());
                // clean from placemen data structures
                loc_to_cluster_id_placed.erase(loc);
                cluster_legalizer.destroy_cluster(cluster_id);
            } else {
                cluster_legalizer.clean_cluster(cluster_id);
            }
        }

        // set the legalization strategy to full
        cluster_legalizer.set_legalization_strategy(ClusterLegalizationStrategy::FULL);
        for (PackMoleculeId mol_id: illegal_cluster_mols) {
            const auto& mol = prepacker_.get_molecule(mol_id);
            const auto block_type = get_molecule_logical_block_type(mol_id, prepacker_, primitive_candidate_block_types);
            if (!block_type) {
                VPR_FATAL_ERROR(VPR_ERROR_AP, "Could not determine block type for molecule ID %zu\n", size_t(mol_id));
            }

            bool placed = false;

            // Try all subtiles in a single loop
            for (int sub_tile = 0; sub_tile < tile_type->capacity; ++sub_tile) {
                if (!is_root_tile(device_grid, tile_loc)) {
                    break;
                }

                const t_pl_loc loc{tile_loc.x, tile_loc.y, sub_tile, tile_loc.layer_num};
                auto cluster_it = loc_to_cluster_id_placed.find(loc);

                if (cluster_it != loc_to_cluster_id_placed.end()) {
                    // Try adding to existing cluster
                    LegalizationClusterId cluster_id = cluster_it->second;
                    // If you still want to double-check
                    if (!has_empty_primitive(cluster_legalizer.get_cluster_pb(cluster_id))) {
                        //VTR_LOG("Catched a non-empty cluster (id: %zu)!\n", cluster_id); // moderate cost, fairly accurate
                        continue;
                    }
                    if (cluster_legalizer.is_molecule_compatible(mol_id, cluster_id) &&
                        cluster_legalizer.add_mol_to_cluster(mol_id, cluster_id) == e_block_pack_status::BLK_PASSED) {
                        placed = true;
                        break;
                    }
                } else if (is_tile_compatible(tile_type, block_type)) {
                    // Create new cluster
                    LegalizationClusterId new_id = create_new_cluster(mol_id, prepacker_, cluster_legalizer, primitive_candidate_block_types);
                    cluster_created_mid_first_pass++;
                    cluster_ids_to_check[new_id] = loc;
                    loc_to_cluster_id_placed[loc] = new_id;
                    cluster_id_to_loc_desired[new_id] = tile_loc;
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                unclustered_blocks.push_back({mol_id, tile_loc});
                unclustered_block_locs[tile_loc].push_back(mol_id);
            }
        }
        // set the legalization strategy to fast check again for next round
        cluster_legalizer.set_legalization_strategy(ClusterLegalizationStrategy::SKIP_INTRA_LB_ROUTE);
    }

    float first_pass_end_time = pack_reconstruction_timer.elapsed_sec();
    VTR_LOG("First (Reconstruction) pass in pack reconstruction took %f (sec).\n", first_pass_end_time-first_pass_start_time);

    VTR_LOG("Number of molecules that coud not clusterd after first iteration is %zu out of %zu. They want to go %zu unique tile locations.\n", unclustered_blocks.size(), ap_netlist_.blocks().size(), unclustered_block_locs.size());
    VTR_LOG("=== Number of clusters created with full strategy fall back is: %zu\n", cluster_created_mid_first_pass);

    int NEIGHBOR_SEARCH_RADIUS = 4;

    VTR_LOG("Adaptive neighbor search radius set to %d\n", NEIGHBOR_SEARCH_RADIUS);

    VTR_LOG("===> Before First Neighbour Pass: \t(time: %f sec, max_rss: %f mib, delta_max_rss: %f mib)\n", pack_reconstruction_timer.elapsed_sec(), pack_reconstruction_timer.max_rss_mib(), pack_reconstruction_timer.delta_max_rss_mib());

    neighbor_cluster_pass(cluster_legalizer, 
                        device_grid,
                        primitive_candidate_block_types,
                        unclustered_blocks,
                        unclustered_block_locs,
                        cluster_id_to_loc_unplaced,
                        ClusterLegalizationStrategy::SKIP_INTRA_LB_ROUTE,
                        NEIGHBOR_SEARCH_RADIUS);
    
    float first_neighbour_pass_end_time = pack_reconstruction_timer.elapsed_sec();
    VTR_LOG("First neighbour pass in pack reconstruction took %f (sec).\n", first_neighbour_pass_end_time-first_pass_end_time);

    VTR_LOG("After neighbor clustering (with search depth %d): %zu unclustered blocks remaining\n", NEIGHBOR_SEARCH_RADIUS, unclustered_blocks.size());

    
    

    // set to full legalization strategy for neighbour pass
    cluster_legalizer.set_legalization_strategy(ClusterLegalizationStrategy::FULL);

    NEIGHBOR_SEARCH_RADIUS = 4;
    VTR_LOG("===> Before Second Neighbour Pass: \t(time: %f sec, max_rss: %f mib, delta_max_rss: %f mib)\n", pack_reconstruction_timer.elapsed_sec(), pack_reconstruction_timer.max_rss_mib(), pack_reconstruction_timer.delta_max_rss_mib());
    neighbor_cluster_pass(cluster_legalizer, 
                        device_grid,
                        primitive_candidate_block_types,
                        unclustered_blocks,
                        unclustered_block_locs,
                        cluster_id_to_loc_unplaced,
                        ClusterLegalizationStrategy::FULL,
                        NEIGHBOR_SEARCH_RADIUS);

    float second_neighbour_pass_end_time = pack_reconstruction_timer.elapsed_sec();
    VTR_LOG("Second neighbour pass in pack reconstruction took %f (sec).\n", second_neighbour_pass_end_time-first_neighbour_pass_end_time);

    VTR_LOG("After neighbor clustering (with search depth %d): %zu unclustered blocks remaining\n", NEIGHBOR_SEARCH_RADIUS, unclustered_blocks.size());

    size_t total_unplaced_clusters = 0;
    for (const auto& [tile_loc, cluster_ids] : cluster_id_to_loc_unplaced) {
        total_unplaced_clusters += cluster_ids.size();
    }

    size_t num_unplaced_tiles = cluster_id_to_loc_unplaced.size();

    VTR_LOG("Unplaced clusters: %zu clusters at %zu unique tile locations.\n",total_unplaced_clusters, num_unplaced_tiles);

    if (unclustered_blocks.empty()) {
        VTR_LOG("All molecules successfully clustered.\n");
    } else {
        VTR_LOG("%zu molecules remain unclustered after neighbor pass.\n", unclustered_blocks.size());
    }

    // maybe cluster_legalizer.compress ?
    
    // In pack_reconstruction_pass():
    VTR_LOG("===> Before Place Remainig Clusters in Packing: \t(time: %f sec, max_rss: %f mib, delta_max_rss: %f mib)\n", pack_reconstruction_timer.elapsed_sec(), pack_reconstruction_timer.max_rss_mib(), pack_reconstruction_timer.delta_max_rss_mib());
    place_remaining_clusters(cluster_legalizer, device_grid, cluster_id_to_loc_unplaced);

    float pseudo_place_end_time = pack_reconstruction_timer.elapsed_sec();
    VTR_LOG("Pseudo placement of remaining clusters in pack reconstruction took %f (sec).\n", pseudo_place_end_time-second_neighbour_pass_end_time);

    VTR_LOG( "%zu clusters remain unassigned placement\n", cluster_id_to_loc_unplaced.size());
    // Then handle remaining unclustered blocks
    if (!cluster_id_to_loc_unplaced.empty()) {
        VPR_FATAL_ERROR(VPR_ERROR_AP, "%zu clusters remain unplaced\n", cluster_id_to_loc_unplaced.size());
    }

    // VPR_FATAL_ERROR(VPR_ERROR_AP, "Stopped BMD for runtime and quality analysis.\n");
}


void BasicMinDisturbance::legalize(const PartialPlacement& p_placement) {
    // Create a scoped timer for the full legalizer
    vtr::ScopedStartFinishTimer full_legalizer_timer("AP Full Legalizer");

    FlatPlacementInfo flat_placement_info(atom_netlist_);
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId mol_id = ap_netlist_.block_molecule(ap_blk_id);
        const t_pack_molecule& mol = prepacker_.get_molecule(mol_id);
        for (AtomBlockId atom_blk_id : mol.atom_block_ids) {
            if (!atom_blk_id.is_valid())
                continue;
            flat_placement_info.blk_x_pos[atom_blk_id] = p_placement.block_x_locs[ap_blk_id];
            flat_placement_info.blk_y_pos[atom_blk_id] = p_placement.block_y_locs[ap_blk_id];
            flat_placement_info.blk_layer[atom_blk_id] = p_placement.block_layer_nums[ap_blk_id];
            flat_placement_info.blk_sub_tile[atom_blk_id] = p_placement.block_sub_tiles[ap_blk_id];
        }
    }
    
    VTR_LOG("Entered the legalize function of BasicMinDisturbance.\n");

    /*
    Data structure to keep track of the clusters created at locations.
    

    grids[layer][x][y] -> vector<int sub_tile, LegalizationCluster created_cluster>

    Lets say we have a molecule that want to go x, y, layer, sub_tile. If there is a cluster created already, 
    there will be a element in grids[layer][x][y] vector with first element being the given sub_tile. 
    If there is a cluster already, we will try to add teh current molecule there. Otherwise, we will try to
    create a new one.

    By trying, we mean that the physical block at that location is compatible with logical block we have and
    there is enough space.
    
    */

    std::vector<std::string> target_ext_pin_util = {"1.0"};
    
    t_pack_high_fanout_thresholds high_fanout_thresholds(vpr_setup_.PackerOpts.high_fanout_threshold);
    ClusterLegalizer cluster_legalizer(atom_netlist_,
                                       prepacker_,
                                       vpr_setup_.PackerRRGraph,
                                       target_ext_pin_util,
                                       high_fanout_thresholds,
                                       //ClusterLegalizationStrategy::FULL, //Change this to skip one
                                       ClusterLegalizationStrategy::SKIP_INTRA_LB_ROUTE,
                                       vpr_setup_.PackerOpts.enable_pin_feasibility_filter,
                                       arch_.models,
                                       vpr_setup_.PackerOpts.pack_verbosity);

    

    // molecule ids that cannot be placed for any reason
    pack_recontruction_pass(cluster_legalizer, p_placement); 
    
    // save the LegalizationClusterId's of atoms for placing
    std::unordered_map<AtomBlockId, LegalizationClusterId> atom_to_legalization_map;
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId blk_mol_id = ap_netlist_.block_molecule(ap_blk_id);
        const t_pack_molecule& blk_mol = prepacker_.get_molecule(blk_mol_id);
        for (AtomBlockId atom_blk_id : blk_mol.atom_block_ids) {
            if (!atom_blk_id.is_valid())
                continue;
            // Ensure that this block is not in any other AP block. That would
            // be weird.
            VTR_ASSERT(!atom_to_legalization_map[atom_blk_id].is_valid());
            LegalizationClusterId cluser_id = cluster_legalizer.get_atom_cluster(atom_blk_id);
            VTR_ASSERT(cluser_id.is_valid());
            atom_to_legalization_map[atom_blk_id] = cluser_id;    
        }
    }

    VTR_LOG("=== Passed: atom_to_legalization_map;\n");
    cluster_legalizer.compress();

    // Check and output the clustering.
    std::unordered_set<AtomNetId> is_clock = alloc_and_load_is_clock();
    check_and_output_clustering(cluster_legalizer, vpr_setup_.PackerOpts, is_clock, &arch_);

    VTR_LOG("=== Passed: check_and_output_clustering(cluster_legalizer, vpr_setup_.PackerOpts, is_clock, &arch_);\n");

    

    // Reset the cluster legalizer. This is required to load the packing.
    cluster_legalizer.reset();
    VTR_LOG("=== Passed: cluster_legalizer.reset();s\n");
    // Regenerate the clustered netlist from the file generated previously.
    // FIXME: This writing and loading from a file is wasteful. Should generate
    //        the clusters directly from the cluster legalizer.
    vpr_load_packing(vpr_setup_, arch_);
    const ClusteredNetlist& clb_nlist = g_vpr_ctx.clustering().clb_nlist;

    // Verify the packing and print some info
    check_netlist(vpr_setup_.PackerOpts.pack_verbosity);
    writeClusteredNetlistStats(vpr_setup_.FileNameOpts.write_block_usage);
    print_pb_type_count(clb_nlist);
    
    // Verify that the clustering created by the full legalizer is valid.
    unsigned num_clustering_errors = verify_clustering(g_vpr_ctx);
    if (num_clustering_errors == 0) {
        VTR_LOG("Completed clustering consistency check successfully.\n");
    } else {
        VPR_ERROR(VPR_ERROR_AP,
                  "Completed placement consistency check, %u errors found.\n"
                  "Aborting program.\n",
                  num_clustering_errors);
    }
    
   // Setup the global variables for placement.
    g_vpr_ctx.mutable_placement().init_placement_context(vpr_setup_.PlacerOpts, arch_.directs);
    g_vpr_ctx.mutable_floorplanning().update_floorplanning_context_pre_place(*g_vpr_ctx.placement().place_macros);

    // The placement will be stored in the global block loc registry.
    BlkLocRegistry& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();

    // Create the noc cost handler used in the initial placer.
    std::optional<NocCostHandler> noc_cost_handler;
    if (vpr_setup_.NocOpts.noc)
        noc_cost_handler.emplace(blk_loc_registry.block_locs());

    // Create the RNG container for the initial placer.
    vtr::RngContainer rng(vpr_setup_.PlacerOpts.seed);

    // Run the initial placer on the clusters created by the packer, using the
    // flat placement information from the global placer to guide where to place
    // the clusters.
    VTR_LOG("=== Calling initial_placement after packing.\n");
    initial_placement(vpr_setup_.PlacerOpts,
                      vpr_setup_.PlacerOpts.constraints_file.c_str(),
                      vpr_setup_.NocOpts,
                      blk_loc_registry,
                      *g_vpr_ctx.placement().place_macros,
                      noc_cost_handler,
                      flat_placement_info,
                      rng);

    // Log some information on how good the reconstruction was.
    log_flat_placement_reconstruction_info(flat_placement_info,
                                           blk_loc_registry.block_locs(),
                                           g_vpr_ctx.clustering().atoms_lookup,
                                           g_vpr_ctx.atom().lookup(),
                                           atom_netlist_,
                                           g_vpr_ctx.clustering().clb_nlist);

    // Verify that the placement is valid for the VTR flow.
    unsigned num_errors = verify_placement(blk_loc_registry,
                                           *g_vpr_ctx.placement().place_macros,
                                           g_vpr_ctx.clustering().clb_nlist,
                                           g_vpr_ctx.device().grid,
                                           g_vpr_ctx.floorplanning().cluster_constraints);
    if (num_errors != 0) {
        VPR_ERROR(VPR_ERROR_AP,
                  "\nCompleted placement consistency check, %d errors found.\n"
                  "Aborting program.\n",
                  num_errors);
    }

    // Synchronize the pins in the clusters after placement.
    post_place_sync();
}

void NaiveFullLegalizer::create_clusters(const PartialPlacement& p_placement) {
    // PACKING:
    // Initialize the cluster legalizer (Packing)
    // FIXME: The legalization strategy is currently set to full. Should handle
    //        this better to make it faster.
    t_pack_high_fanout_thresholds high_fanout_thresholds(vpr_setup_.PackerOpts.high_fanout_threshold);
    ClusterLegalizer cluster_legalizer(atom_netlist_,
                                       prepacker_,
                                       vpr_setup_.PackerRRGraph,
                                       vpr_setup_.PackerOpts.target_external_pin_util,
                                       high_fanout_thresholds,
                                       ClusterLegalizationStrategy::FULL,
                                       vpr_setup_.PackerOpts.enable_pin_feasibility_filter,
                                       arch_.models,
                                       vpr_setup_.PackerOpts.pack_verbosity);
    // Create clusters for each tile.
    //  Start by giving each root tile a unique ID.
    size_t grid_width = device_grid_.width();
    size_t grid_height = device_grid_.height();
    vtr::NdMatrix<DeviceTileId, 2> tile_grid({grid_width, grid_height});
    size_t num_device_tiles = 0;
    for (size_t x = 0; x < grid_width; x++) {
        for (size_t y = 0; y < grid_height; y++) {
            // Ignoring 3D placement for now.
            t_physical_tile_loc tile_loc(x, y, 0);
            // Ignore non-root locations
            size_t width_offset = device_grid_.get_width_offset(tile_loc);
            size_t height_offset = device_grid_.get_height_offset(tile_loc);
            if (width_offset != 0 || height_offset != 0) {
                tile_grid[x][y] = tile_grid[x - width_offset][y - height_offset];
                continue;
            }
            tile_grid[x][y] = DeviceTileId(num_device_tiles);
            num_device_tiles++;
        }
    }
    //  Next, collect the AP blocks which will go into each root tile
    VTR_ASSERT_SAFE(p_placement.verify_locs(ap_netlist_, grid_width, grid_height));
    vtr::vector<DeviceTileId, std::vector<APBlockId>> blocks_in_tiles(num_device_tiles);
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        // FIXME: Add these conversions to the PartialPlacement class.
        t_physical_tile_loc tile_loc = p_placement.get_containing_tile_loc(ap_blk_id);
        VTR_ASSERT(p_placement.block_layer_nums[ap_blk_id] == 0);
        DeviceTileId tile_id = tile_grid[tile_loc.x][tile_loc.y];
        blocks_in_tiles[tile_id].push_back(ap_blk_id);
    }
    //  Create the legalized clusters per tile.
    vtr::vector<LogicalModelId, std::vector<t_logical_block_type_ptr>>
        primitive_candidate_block_types = identify_primitive_candidate_block_types();
    for (size_t tile_id_idx = 0; tile_id_idx < num_device_tiles; tile_id_idx++) {
        DeviceTileId tile_id = DeviceTileId(tile_id_idx);
        // Create the molecule list
        std::list<PackMoleculeId> mol_list;
        for (APBlockId ap_blk_id : blocks_in_tiles[tile_id]) {
            mol_list.push_back(ap_netlist_.block_molecule(ap_blk_id));
        }
        // Clustering algorithm: Create clusters one at a time.
        while (!mol_list.empty()) {
            // Arbitrarily choose the first molecule as a seed molecule.
            PackMoleculeId seed_mol_id = mol_list.front();
            mol_list.pop_front();
            // Use the seed molecule to create a cluster for this tile.
            LegalizationClusterId new_cluster_id = create_new_cluster(seed_mol_id, prepacker_, cluster_legalizer, primitive_candidate_block_types);
            // Insert all molecules that you can into the cluster.
            // NOTE: If the mol_list was somehow sorted, we can just stop at
            //       first failure!
            auto it = mol_list.begin();
            while (it != mol_list.end()) {
                PackMoleculeId mol_id = *it;
                if (!cluster_legalizer.is_molecule_compatible(mol_id, new_cluster_id)) {
                    ++it;
                    continue;
                }
                // Try to insert it. If successful, remove from list.
                e_block_pack_status pack_status = cluster_legalizer.add_mol_to_cluster(mol_id, new_cluster_id);
                if (pack_status == e_block_pack_status::BLK_PASSED) {
                    it = mol_list.erase(it);
                } else {
                    ++it;
                }
            }
            // Once all molecules have been inserted, clean the cluster.
            cluster_legalizer.clean_cluster(new_cluster_id);
        }
    }

    // Check and output the clustering.
    std::unordered_set<AtomNetId> is_clock = alloc_and_load_is_clock();
    check_and_output_clustering(cluster_legalizer, vpr_setup_.PackerOpts, is_clock, &arch_);
    // Reset the cluster legalizer. This is required to load the packing.
    cluster_legalizer.reset();
    // Regenerate the clustered netlist from the file generated previously.
    // FIXME: This writing and loading from a file is wasteful. Should generate
    //        the clusters directly from the cluster legalizer.
    vpr_load_packing(vpr_setup_, arch_);
    const ClusteredNetlist& clb_nlist = g_vpr_ctx.clustering().clb_nlist;

    // Verify the packing and print some info
    check_netlist(vpr_setup_.PackerOpts.pack_verbosity);
    writeClusteredNetlistStats(vpr_setup_.FileNameOpts.write_block_usage);
    print_pb_type_count(clb_nlist);

    //VPR_FATAL_ERROR(VPR_ERROR_AP, "Stopped to compare cluster creation with the BMD.\n");
}

void NaiveFullLegalizer::place_clusters(const ClusteredNetlist& clb_nlist,
                                        const PlaceMacros& place_macros,
                                        const PartialPlacement& p_placement) {
    // PLACING:
    // Create a lookup from the AtomBlockId to the APBlockId
    vtr::vector<AtomBlockId, APBlockId> atom_to_ap_block(atom_netlist_.blocks().size());
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId blk_mol_id = ap_netlist_.block_molecule(ap_blk_id);
        const t_pack_molecule& blk_mol = prepacker_.get_molecule(blk_mol_id);
        for (AtomBlockId atom_blk_id : blk_mol.atom_block_ids) {
            // See issue #2791, some of the atom_block_ids may be invalid. They
            // can safely be ignored.
            if (!atom_blk_id.is_valid())
                continue;
            // Ensure that this block is not in any other AP block. That would
            // be weird.
            VTR_ASSERT(!atom_to_ap_block[atom_blk_id].is_valid());
            atom_to_ap_block[atom_blk_id] = ap_blk_id;
        }
    }
    // Move the clusters to where they want to be first.
    // TODO: The fixed clusters should probably be moved first for legality
    //       reasons.
    APClusterPlacer ap_cluster_placer(place_macros, vpr_setup_.PlacerOpts.constraints_file.c_str());
    std::vector<ClusterBlockId> unplaced_clusters;
    for (ClusterBlockId cluster_blk_id : clb_nlist.blocks()) {
        // Assume that the cluster will always want to be placed wherever the
        // first atom in the cluster wants to be placed.
        // FIXME: This assumption does not always hold! Will need to unify the
        //        cluster legalizer and the clustered netlist!
        const std::unordered_set<AtomBlockId>& atoms_in_cluster = g_vpr_ctx.clustering().atoms_lookup[cluster_blk_id];
        VTR_ASSERT(atoms_in_cluster.size() > 0);
        AtomBlockId first_atom_blk = *atoms_in_cluster.begin();
        APBlockId first_ap_blk = atom_to_ap_block[first_atom_blk];
        size_t blk_sub_tile = p_placement.block_sub_tiles[first_ap_blk];
        t_physical_tile_loc tile_loc = p_placement.get_containing_tile_loc(first_ap_blk);
        bool placed = ap_cluster_placer.place_cluster(cluster_blk_id, tile_loc, blk_sub_tile);
        if (placed)
            continue;

        // Add to list of unplaced clusters.
        unplaced_clusters.push_back(cluster_blk_id);
    }

    // Any clusters that were not placed previously are exhaustively placed.
    for (ClusterBlockId clb_blk_id : unplaced_clusters) {
        bool success = ap_cluster_placer.exhaustively_place_cluster(clb_blk_id);
        if (!success) {
            VPR_FATAL_ERROR(VPR_ERROR_AP,
                            "Unable to find valid place for cluster in AP placement!");
        }
    }

    // Print some statistics about what happened here. This will be useful to
    // improve other algorithms.
    VTR_LOG("Number of clusters which needed to be moved: %zu\n", unplaced_clusters.size());

    // TODO: Print a breakdown per block type. We may find that specific block
    //       types are always conflicting.

    // FIXME: Allocate and load moveable blocks?
    //      - This may be needed to perform SA. Not needed right now.
}

void NaiveFullLegalizer::legalize(const PartialPlacement& p_placement) {
    // Create a scoped timer for the full legalizer
    vtr::ScopedStartFinishTimer full_legalizer_timer("AP Full Legalizer");

    FlatPlacementInfo flat_placement_info(atom_netlist_);
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId mol_id = ap_netlist_.block_molecule(ap_blk_id);
        const t_pack_molecule& mol = prepacker_.get_molecule(mol_id);
        for (AtomBlockId atom_blk_id : mol.atom_block_ids) {
            if (!atom_blk_id.is_valid())
                continue;
            flat_placement_info.blk_x_pos[atom_blk_id] = p_placement.block_x_locs[ap_blk_id];
            flat_placement_info.blk_y_pos[atom_blk_id] = p_placement.block_y_locs[ap_blk_id];
            flat_placement_info.blk_layer[atom_blk_id] = p_placement.block_layer_nums[ap_blk_id];
            flat_placement_info.blk_sub_tile[atom_blk_id] = p_placement.block_sub_tiles[ap_blk_id];
        }
    }

    // Pack the atoms into clusters based on the partial placement.
    create_clusters(p_placement);
    // Verify that the clustering created by the full legalizer is valid.
    unsigned num_clustering_errors = verify_clustering(g_vpr_ctx);
    if (num_clustering_errors == 0) {
        VTR_LOG("Completed clustering consistency check successfully.\n");
    } else {
        VPR_ERROR(VPR_ERROR_AP,
                  "Completed placement consistency check, %u errors found.\n"
                  "Aborting program.\n",
                  num_clustering_errors);
    }
    // Get the clustering from the global context.
    // TODO: Eventually should be returned from the create_clusters method.
    const ClusteredNetlist& clb_nlist = g_vpr_ctx.clustering().clb_nlist;

    // Initialize the placement context.
    g_vpr_ctx.mutable_placement().init_placement_context(vpr_setup_.PlacerOpts,
                                                         arch_.directs);

    const PlaceMacros& place_macros = *g_vpr_ctx.placement().place_macros;

    // Update the floorplanning context with the macro information.
    g_vpr_ctx.mutable_floorplanning().update_floorplanning_context_pre_place(place_macros);

    // Place the clusters based on where the atoms want to be placed.
    place_clusters(clb_nlist, place_macros, p_placement);

    // Log some information on how good the reconstruction was.
    BlkLocRegistry& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();
    log_flat_placement_reconstruction_info(flat_placement_info,
                                           blk_loc_registry.block_locs(),
                                           g_vpr_ctx.clustering().atoms_lookup,
                                           g_vpr_ctx.atom().lookup(),
                                           atom_netlist_,
                                           g_vpr_ctx.clustering().clb_nlist);

    // Verify that the placement created by the full legalizer is valid.
    unsigned num_placement_errors = verify_placement(g_vpr_ctx);
    if (num_placement_errors == 0) {
        VTR_LOG("Completed placement consistency check successfully.\n");
    } else {
        VPR_ERROR(VPR_ERROR_AP,
                  "Completed placement consistency check, %u errors found.\n"
                  "Aborting program.\n",
                  num_placement_errors);
    }

    // TODO: This was taken from vpr_api. Not sure why it is needed. Should be
    //       made part of the placement and verify placement should check for
    //       it.
    post_place_sync();
}

void APPack::legalize(const PartialPlacement& p_placement) {
    // Create a scoped timer for the full legalizer
    vtr::ScopedStartFinishTimer full_legalizer_timer("AP Full Legalizer");

    // Convert the Partial Placement (APNetlist) to a flat placement (AtomNetlist).
    FlatPlacementInfo flat_placement_info(atom_netlist_);
    for (APBlockId ap_blk_id : ap_netlist_.blocks()) {
        PackMoleculeId mol_id = ap_netlist_.block_molecule(ap_blk_id);
        const t_pack_molecule& mol = prepacker_.get_molecule(mol_id);
        for (AtomBlockId atom_blk_id : mol.atom_block_ids) {
            if (!atom_blk_id.is_valid())
                continue;
            flat_placement_info.blk_x_pos[atom_blk_id] = p_placement.block_x_locs[ap_blk_id];
            flat_placement_info.blk_y_pos[atom_blk_id] = p_placement.block_y_locs[ap_blk_id];
            flat_placement_info.blk_layer[atom_blk_id] = p_placement.block_layer_nums[ap_blk_id];
            flat_placement_info.blk_sub_tile[atom_blk_id] = p_placement.block_sub_tiles[ap_blk_id];
        }
    }

    // Run the Packer stage with the flat placement as a hint.
    try_pack(vpr_setup_.PackerOpts,
             vpr_setup_.AnalysisOpts,
             vpr_setup_.APOpts,
             arch_,
             vpr_setup_.PackerRRGraph,
             prepacker_,
             pre_cluster_timing_manager_,
             flat_placement_info);

    // The Packer stores the clusters into a .net file. Load the packing file.
    // FIXME: This should be removed. Reading from a file is strange.
    vpr_load_packing(vpr_setup_, arch_);

    // Setup the global variables for placement.
    g_vpr_ctx.mutable_placement().init_placement_context(vpr_setup_.PlacerOpts, arch_.directs);
    g_vpr_ctx.mutable_floorplanning().update_floorplanning_context_pre_place(*g_vpr_ctx.placement().place_macros);

    // The placement will be stored in the global block loc registry.
    BlkLocRegistry& blk_loc_registry = g_vpr_ctx.mutable_placement().mutable_blk_loc_registry();

    // Create the noc cost handler used in the initial placer.
    std::optional<NocCostHandler> noc_cost_handler;
    if (vpr_setup_.NocOpts.noc)
        noc_cost_handler.emplace(blk_loc_registry.block_locs());

    // Create the RNG container for the initial placer.
    vtr::RngContainer rng(vpr_setup_.PlacerOpts.seed);

    // Run the initial placer on the clusters created by the packer, using the
    // flat placement information from the global placer to guide where to place
    // the clusters.
    initial_placement(vpr_setup_.PlacerOpts,
                      vpr_setup_.PlacerOpts.constraints_file.c_str(),
                      vpr_setup_.NocOpts,
                      blk_loc_registry,
                      *g_vpr_ctx.placement().place_macros,
                      noc_cost_handler,
                      flat_placement_info,
                      rng);

    // Log some information on how good the reconstruction was.
    log_flat_placement_reconstruction_info(flat_placement_info,
                                           blk_loc_registry.block_locs(),
                                           g_vpr_ctx.clustering().atoms_lookup,
                                           g_vpr_ctx.atom().lookup(),
                                           atom_netlist_,
                                           g_vpr_ctx.clustering().clb_nlist);

    // Verify that the placement is valid for the VTR flow.
    unsigned num_errors = verify_placement(blk_loc_registry,
                                           *g_vpr_ctx.placement().place_macros,
                                           g_vpr_ctx.clustering().clb_nlist,
                                           g_vpr_ctx.device().grid,
                                           g_vpr_ctx.floorplanning().cluster_constraints);
    if (num_errors != 0) {
        VPR_ERROR(VPR_ERROR_AP,
                  "\nCompleted placement consistency check, %d errors found.\n"
                  "Aborting program.\n",
                  num_errors);
    }

    // Synchronize the pins in the clusters after placement.
    post_place_sync();
}
