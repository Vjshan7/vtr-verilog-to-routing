#ifndef PACK_H
#define PACK_H

#include <unordered_set>
#include <vector>

class AtomNetId;
class FlatPlacementInfo;
struct t_analysis_opts;
struct t_arch;
struct t_lb_type_rr_node;
struct t_model;
struct t_packer_opts;

/**
 * @brief Try to pack the atom netlist into legal clusters on the given
 *        architecture. Will return true if successful, false otherwise.
 *
 *  @param packer_opts
 *              Options passed by the user to configure the packing algorithm.
 *  @param analysis_opts
 *              Options passed by the user to configure how analysis is
 *              performed in the packer.
 *  @param arch
 *              A pointer to the architecture to create clusters for.
 *  @param user_models
 *              A list of architecture models provided by the architecture file.
 *  @param library_models
 *              A list of architecture models provided by the library.
 *  @param interc_delay
 *  @param lb_type_rr_graphs
 *  @param flat_placement_info
 *              Flat (primitive-level) placement information that may be
 *              provided by the user as a hint for packing. Will be invalid if
 *              there is no flat placement information provided.
 */
bool try_pack(t_packer_opts* packer_opts,
              const t_analysis_opts* analysis_opts,
              const t_arch* arch,
              const t_model* user_models,
              const t_model* library_models,
              float interc_delay,
              std::vector<t_lb_type_rr_node>* lb_type_rr_graphs,
              const FlatPlacementInfo& flat_placement_info);

float get_arch_switch_info(short switch_index, int switch_fanin, float& Tdel_switch, float& R_switch, float& Cout_switch);

std::unordered_set<AtomNetId> alloc_and_load_is_clock();

#endif
