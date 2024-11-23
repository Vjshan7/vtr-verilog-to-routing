/**
 * @file
 * @author  Vaughn Betz (first revision - VPack),
 *          Alexander Marquardt (second revision - T-VPack),
 *          Jason Luu (third revision - AAPack),
 *          Alex Singer (fourth revision - APPack)
 * @date    June 8, 2011
 * @brief   Main clustering algorithm
 *
 * The clusterer uses several key data structures:
 *
 *      t_pb_type (and related types):
 *          Represent the architecture as described in the architecture file.
 *
 *      t_pb_graph_node (and related types):
 *          Represents a flattened version of the architecture with t_pb_types
 *          expanded (according to num_pb) into unique t_pb_graph_node instances,
 *          and the routing connectivity converted to a graph of t_pb_graph_pin (nodes)
 *          and t_pb_graph_edge.
 *
 *      t_pb:
 *          Represents a clustered instance of a t_pb_graph_node containing netlist primitives
 *
 *  t_pb_type and t_pb_graph_node (and related types) describe the targeted FPGA architecture, while t_pb represents
 *  the actual clustering of the user netlist.
 *
 *  For example:
 *      Consider an architecture where CLBs contain 4 BLEs, and each BLE is a LUT + FF pair.
 *      We wish to map a netlist of 400 LUTs and 400 FFs.
 *
 *      A BLE corresponds to one t_pb_type (which has num_pb = 4).
 *
 *      Each of the 4 BLE positions corresponds to a t_pb_graph_node (each of which references the BLE t_pb_type).
 *
 *      The output of clustering is 400 t_pb of type BLE which represent the clustered user netlist.
 *      Each of the 400 t_pb will reference one of the 4 BLE-type t_pb_graph_nodes.
 */

#include "greedy_clusterer.h"
#include <map>
#include <vector>
#include "atom_netlist.h"
#include "attraction_groups.h"
#include "cluster_legalizer.h"
#include "cluster_util.h"
#include "constraints_report.h"
#include "greedy_seed_selector.h"
#include "physical_types.h"
#include "prepack.h"
#include "vtr_vector.h"

GreedyClusterer::GreedyClusterer(const t_packer_opts& packer_opts,
                                 const t_analysis_opts& analysis_opts,
                                 const AtomNetlist& atom_netlist,
                                 const t_arch* arch,
                                 const t_pack_high_fanout_thresholds& high_fanout_thresholds,
                                 const std::unordered_set<AtomNetId>& is_clock,
                                 const std::unordered_set<AtomNetId>& is_global)
        : packer_opts_(packer_opts),
          analysis_opts_(analysis_opts),
          atom_netlist_(atom_netlist),
          arch_(arch),
          high_fanout_thresholds_(high_fanout_thresholds),
          is_clock_(is_clock),
          is_global_(is_global),
          primitive_candidate_block_types_(identify_primitive_candidate_block_types()) {}

std::map<t_logical_block_type_ptr, size_t>
GreedyClusterer::do_clustering(ClusterLegalizer& cluster_legalizer,
                               Prepacker& prepacker,
                               bool allow_unrelated_clustering,
                               bool balance_block_type_utilization,
                               AttractionInfo& attraction_groups) {

    /* Does the actual work of clustering multiple netlist blocks *
     * into clusters.                                                  */

    /* Algorithm employed
     * 1.  Find type that can legally hold block and create cluster with pb info
     * 2.  Populate started cluster
     * 3.  Repeat 1 until no more blocks need to be clustered
     *
     */

    /* This routine returns a map that details the number of used block type instances.
     * The bool floorplan_regions_overfull also acts as a return value - it is set to
     * true when one or more floorplan regions have more blocks assigned to them than
     * they can fit.
     */

    /****************************************************************
     * Initialization
     *****************************************************************/
    t_clustering_data clustering_data;
    t_cluster_progress_stats cluster_stats;

    //int num_molecules, num_molecules_processed, mols_since_last_print, blocks_since_last_analysis,
    int num_blocks_hill_added;

    const int verbosity = packer_opts_.pack_verbosity;

    int unclustered_list_head_size;
    std::unordered_map<AtomNetId, int> net_output_feeds_driving_block_input;

    cluster_stats.num_molecules_processed = 0;
    cluster_stats.mols_since_last_print = 0;

    std::map<t_logical_block_type_ptr, size_t> num_used_type_instances;

    enum e_block_pack_status block_pack_status;

    t_pack_molecule *next_molecule, *prev_molecule;

    auto& device_ctx = g_vpr_ctx.mutable_device();

    std::shared_ptr<PreClusterDelayCalculator> clustering_delay_calc;
    std::shared_ptr<SetupTimingInfo> timing_info;

    // this data structure tracks the number of Logic Elements (LEs) used. It is
    // populated only for architectures which has LEs. The architecture is assumed
    // to have LEs only iff it has a logic block that contains LUT primitives and is
    // the first pb_block to have more than one instance from the top of the hierarchy
    // (All parent pb_block have one instance only and one mode only). Index 0 holds
    // the number of LEs that are used for both logic (LUTs/adders) and registers.
    // Index 1 holds the number of LEs that are used for logic (LUTs/adders) only.
    // Index 2 holds the number of LEs that are used for registers only.
    std::vector<int> le_count(3, 0);

    int total_clb_num = 0;

    /* TODO: This is memory inefficient, fix if causes problems */
    /* Store stats on nets used by packed block, useful for determining transitively connected blocks
     * (eg. [A1, A2, ..]->[B1, B2, ..]->C implies cluster [A1, A2, ...] and C have a weak link) */
    vtr::vector<LegalizationClusterId, std::vector<AtomNetId>> clb_inter_blk_nets(atom_netlist_.blocks().size());

    const t_molecule_stats max_molecule_stats = prepacker.calc_max_molecule_stats(atom_netlist_);

    cluster_stats.num_molecules = prepacker.get_num_molecules();

    if (packer_opts_.hill_climbing_flag) {
        size_t max_cluster_size = cluster_legalizer.get_max_cluster_size();
        clustering_data.hill_climbing_inputs_avail = new int[max_cluster_size + 1];
        for (size_t i = 0; i < max_cluster_size + 1; i++)
            clustering_data.hill_climbing_inputs_avail[i] = 0;
    } else {
        clustering_data.hill_climbing_inputs_avail = nullptr; /* if used, die hard */
    }

#if 0
	check_for_duplicate_inputs ();
#endif

    alloc_and_init_clustering(max_molecule_stats,
                              prepacker,
                              clustering_data, net_output_feeds_driving_block_input,
                              unclustered_list_head_size, cluster_stats.num_molecules);

    // find the cluster type that has lut primitives
    auto logic_block_type = identify_logic_block_type(primitive_candidate_block_types_);
    // find a LE pb_type within the found logic_block_type
    auto le_pb_type = identify_le_block_type(logic_block_type);

    cluster_stats.blocks_since_last_analysis = 0;
    num_blocks_hill_added = 0;

    //Default criticalities set to zero (e.g. if not timing driven)
    vtr::vector<AtomBlockId, float> atom_criticality(atom_netlist_.blocks().size(), 0.);

    if (packer_opts_.timing_driven) {
        calc_init_packing_timing(packer_opts_, analysis_opts_, prepacker,
                                 clustering_delay_calc, timing_info, atom_criticality);
    }

    // Create the greedy seed selector.
    GreedySeedSelector seed_selector(atom_netlist_,
                                     prepacker,
                                     packer_opts_.cluster_seed_type,
                                     max_molecule_stats,
                                     atom_criticality);

    // Pick the first seed molecule.
    t_pack_molecule* istart = seed_selector.get_next_seed(prepacker,
                                                          cluster_legalizer);

    print_pack_status_header();

    /****************************************************************
     * Clustering
     *****************************************************************/

    while (istart != nullptr) {
        bool is_cluster_legal = false;
        // The basic algorithm:
        // 1) Try to put all the molecules in that you can without doing the
        //    full intra-lb route. Then do full legalization at the end.
        // 2) If the legalization at the end fails, try again, but this time
        //    do full legalization for each molecule added to the cluster.
        const ClusterLegalizationStrategy legalization_strategies[] = {ClusterLegalizationStrategy::SKIP_INTRA_LB_ROUTE,
                                                                       ClusterLegalizationStrategy::FULL};
        for (const ClusterLegalizationStrategy strategy : legalization_strategies) {
            // If the cluster is legal, no need to try a stronger cluster legalizer
            // mode.
            if (is_cluster_legal)
                break;
            // Set the legalization strategy of the cluster legalizer.
            cluster_legalizer.set_legalization_strategy(strategy);

            LegalizationClusterId legalization_cluster_id;

            VTR_LOGV(verbosity > 2, "Complex block %d:\n", total_clb_num);

            start_new_cluster(cluster_legalizer,
                              legalization_cluster_id,
                              istart,
                              num_used_type_instances,
                              packer_opts_.target_device_utilization,
                              arch_, packer_opts_.device_layout,
                              primitive_candidate_block_types_,
                              verbosity,
                              balance_block_type_utilization);

            //initial molecule in cluster has been processed
            cluster_stats.num_molecules_processed++;
            cluster_stats.mols_since_last_print++;
            print_pack_status(total_clb_num,
                              cluster_stats.num_molecules,
                              cluster_stats.num_molecules_processed,
                              cluster_stats.mols_since_last_print,
                              device_ctx.grid.width(),
                              device_ctx.grid.height(),
                              attraction_groups,
                              cluster_legalizer);

            VTR_LOGV(verbosity > 2,
                     "Complex block %d: '%s' (%s) ", total_clb_num,
                     cluster_legalizer.get_cluster_pb(legalization_cluster_id)->name,
                     cluster_legalizer.get_cluster_type(legalization_cluster_id)->name.c_str());
            VTR_LOGV(verbosity > 2, ".");
            //Progress dot for seed-block
            fflush(stdout);

            int high_fanout_threshold = high_fanout_thresholds_.get_threshold(cluster_legalizer.get_cluster_type(legalization_cluster_id)->name);
            update_cluster_stats(istart,
                                 cluster_legalizer,
                                 is_clock_,  //Set of clock nets
                                 is_global_, //Set of global nets (currently all clocks)
                                 packer_opts_.global_clocks,
                                 packer_opts_.alpha, packer_opts_.beta,
                                 packer_opts_.timing_driven, packer_opts_.connection_driven,
                                 high_fanout_threshold,
                                 *timing_info,
                                 attraction_groups,
                                 net_output_feeds_driving_block_input);
            total_clb_num++;

            if (packer_opts_.timing_driven) {
                cluster_stats.blocks_since_last_analysis++;
                /*it doesn't make sense to do a timing analysis here since there*
                 *is only one atom block clustered it would not change anything      */
            }
            cluster_stats.num_unrelated_clustering_attempts = 0;
            next_molecule = get_molecule_for_cluster(cluster_legalizer.get_cluster_pb(legalization_cluster_id),
                                                     attraction_groups,
                                                     allow_unrelated_clustering,
                                                     packer_opts_.prioritize_transitive_connectivity,
                                                     packer_opts_.transitive_fanout_threshold,
                                                     packer_opts_.feasible_block_array_size,
                                                     &cluster_stats.num_unrelated_clustering_attempts,
                                                     prepacker,
                                                     cluster_legalizer,
                                                     clb_inter_blk_nets,
                                                     legalization_cluster_id,
                                                     verbosity,
                                                     clustering_data.unclustered_list_head,
                                                     unclustered_list_head_size,
                                                     primitive_candidate_block_types_);
            prev_molecule = istart;

            /*
             * When attraction groups are created, the purpose is to pack more densely by adding more molecules
             * from the cluster's attraction group to the cluster. In a normal flow, (when attraction groups are
             * not on), the cluster keeps being packed until the get_molecule routines return either a repeated
             * molecule or a nullptr. When attraction groups are on, we want to keep exploring molecules for the
             * cluster until a nullptr is returned. So, the number of repeated molecules allowed is increased to a
             * large value.
             */
            int max_num_repeated_molecules = 0;
            if (attraction_groups.num_attraction_groups() > 0) {
                max_num_repeated_molecules = attraction_groups_max_repeated_molecules_;
            } else {
                max_num_repeated_molecules = 1;
            }
            int num_repeated_molecules = 0;

            while (next_molecule != nullptr && num_repeated_molecules < max_num_repeated_molecules) {
                prev_molecule = next_molecule;

                try_fill_cluster(cluster_legalizer,
                                 prepacker,
                                 packer_opts_,
                                 prev_molecule,
                                 next_molecule,
                                 num_repeated_molecules,
                                 cluster_stats,
                                 total_clb_num,
                                 legalization_cluster_id,
                                 attraction_groups,
                                 clb_inter_blk_nets,
                                 allow_unrelated_clustering,
                                 high_fanout_threshold,
                                 is_clock_,
                                 is_global_,
                                 timing_info,
                                 block_pack_status,
                                 clustering_data.unclustered_list_head,
                                 unclustered_list_head_size,
                                 net_output_feeds_driving_block_input,
                                 primitive_candidate_block_types_);
            }

            if (strategy == ClusterLegalizationStrategy::FULL) {
                // If the legalizer fully legalized for every molecule added,
                // the cluster should be legal.
                is_cluster_legal = true;
            } else {
                // If the legalizer did not check everything for every molecule,
                // need to check that the full cluster is legal (need to perform
                // intra-lb routing).
                is_cluster_legal = cluster_legalizer.check_cluster_legality(legalization_cluster_id);
            }

            if (is_cluster_legal) {
                // Pick new seed.
                istart = seed_selector.get_next_seed(prepacker,
                                                     cluster_legalizer);
                // Update cluster stats.
                if (packer_opts_.timing_driven && num_blocks_hill_added > 0)
                    cluster_stats.blocks_since_last_analysis += num_blocks_hill_added;

                store_cluster_info_and_free(packer_opts_, legalization_cluster_id, logic_block_type, le_pb_type, le_count, cluster_legalizer, clb_inter_blk_nets);
                // Since the cluster will no longer be added to beyond this point,
                // clean the cluster of any data not strictly necessary for
                // creating the clustered netlist.
                cluster_legalizer.clean_cluster(legalization_cluster_id);
            } else {
                // If the cluster is not legal, requeue used mols.
                num_used_type_instances[cluster_legalizer.get_cluster_type(legalization_cluster_id)]--;
                total_clb_num--;
                // Destroy the illegal cluster.
                cluster_legalizer.destroy_cluster(legalization_cluster_id);
                cluster_legalizer.compress();
            }
        }
    }

    // if this architecture has LE physical block, report its usage
    if (le_pb_type) {
        print_le_count(le_count, le_pb_type);
    }

    // Ensure that we have kept track of the number of clusters correctly.
    // TODO: The total_clb_num variable could probably just be replaced by
    //       clusters().size().
    VTR_ASSERT(cluster_legalizer.clusters().size() == (size_t)total_clb_num);

    // Free the clustering data.
    // FIXME: This struct should use standard data structures so it does not
    //        have to be freed like this.
    free_clustering_data(packer_opts_, clustering_data);

    return num_used_type_instances;
}

