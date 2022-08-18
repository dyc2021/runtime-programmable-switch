/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

//! @file switch.h
//! This file contains 2 classes: bm::SwitchWContexts and bm::Switch. When
//! implementing your target, you need to subclass one of them. By subclassing
//! bm::SwitchWContexts, you will be able to write a target containing an
//! arbitary number of bm::Context objects. For a detailed description of what a
//! bm::Context is, please read the context.h documentation. However, many
//! targets don't require the notion of bm::Context, which is why we also
//! provide the bm::Switch class. The bm::Switch class inherits from
//! bm::SwitchWContexts. Because it assumes that your switch will only use a
//! single bm::Context, the very notion of context can be removed from the
//! bm::Switch class and its dataplane APIs. However, because we offer unified
//! runtime APIs, you will have to use a context id of `0` when programming the
//! tables, even when your switch class inherits from bm::Switch and not
//! bm::SwitchWContexts.
//! The simple switch target only supports one bm::Context and inherits from
//! bm::Switch.
//!
//! When subclassing on of these two classes, you need to remember to implement
//! the two pure virtual functions:
//! bm::SwitchWContexts::receive_(int port_num, const char *buffer, int len) and
//! bm::SwitchWContexts::start_and_return_(). Your receive() implementation will
//! be called for you every time a new packet is received by the device. In your
//! start_and_return() function, you are supposed to start the different
//! processing threads of your target switch and return immediately. Note that
//! start_and_return() should not be mandatory per se (the target designer could
//! do the initialization any way he wants, even potentially in the
//! constructor). However, we have decided to keep it around for now.
//!
//! Both switch classes support live swapping of P4-JSON configurations. To
//! enable it you need to provide the correct flag to the constructor (see
//! bm::SwitchWContexts::SwitchWContexts()). Swaps are ordered through the
//! runtime interfaces. We ensure that during the actual swap operation
//! (bm::SwitchWContexts::do_swap() method), there is no Packet instance
//! inflight, which we achieve using the process_packet_mutex mutex). The final
//! step of the swap is to call bm::SwitchWContexts::swap_notify_(), which
//! targets can override if they need to perform some operations as part of the
//! swap. Targets are guaranteed that no Packet instances exist as that
//! time. Note that swapping configurations may invalidate pointers that you are
//! still using, and it is your responsibility to refresh them.

#ifndef BM_BM_SIM_SWITCH_H_
#define BM_BM_SIM_SWITCH_H_

#include <condition_variable>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fstream>

#include <boost/thread/shared_mutex.hpp>

#include "action_profile.h"
#include "context.h"
#include "dev_mgr.h"
#include "device_id.h"
#include "learning.h"
#include "lookup_structures.h"
#include "phv_source.h"
#include "queue.h"
#include "runtime_interface.h"
#include "target_parser.h"
#include "logger.h"

namespace bm {

class OptionsParser;
class Packet;

// multiple inheritance in accordance with Google C++ guidelines:
// "Multiple inheritance is allowed only when all superclasses, with the
// possible exception of the first one, are pure interfaces. In order to ensure
// that they remain pure interfaces, they must end with the Interface suffix."
//
//! Base class for a switch implemenattion where multi-context support is
//! required.
class SwitchWContexts : public DevMgr, public RuntimeInterface {
  friend class Switch;

 public:
  //! To enable live swapping of P4-JSON configurations, enable_swap needs to be
  //! set to `true`. See switch.h documentation for more information on
  //! configuration swap.
  explicit SwitchWContexts(size_t nb_cxts = 1u, bool enable_swap = false);

  // TODO(antonin): return reference instead?
  //! Access a Context by context id, throws a std::out_of_range exception if
  //! \p cxt_id is invalid.
  Context *get_context(cxt_id_t cxt_id = 0u) {
    return &contexts.at(cxt_id);
  }

  int receive(port_t port_num, const char *buffer, int len);

  //! Call this function when you are ready to process packets. This function
  //! will call start_and_return_() which you have to override in your switch
  //! implementation. Note that if the switch is started without a P4
  //! configuration, this function will block until a P4 configuration is
  //! available (you can push a configuration through the Thrift RPC service)
  //! before calling start_and_return_().
  void start_and_return();

  //! Returns the Thrift port used for the runtime RPC server.
  int get_runtime_port() const { return thrift_port; }

  //! Returns the device id for this switch instance.
  device_id_t get_device_id() const { return device_id; }

  //! Returns the nanomsg IPC address for this switch.
  std::string get_notifications_addr() const { return notifications_addr; }

  // Returns empty string ("") if debugger disabled
  std::string get_debugger_addr() const;

  // Returns empty string ("") if event logger disabled
  std::string get_event_logger_addr() const;

  //! Enable JSON config swapping for the switch.
  void enable_config_swap();

  //! Disable JSON config swapping for the switch.
  void disable_config_swap();

  //! Specify that the field is required for this target switch, i.e. the field
  //! needs to be defined in the input JSON. This function is purely meant as a
  //! safeguard and you should use it for error checking. For example, the
  //! following can be found in the simple switch target constructor:
  //! @code
  //! add_required_field("standard_metadata", "ingress_port");
  //! add_required_field("standard_metadata", "packet_length");
  //! add_required_field("standard_metadata", "instance_type");
  //! add_required_field("standard_metadata", "egress_spec");
  //! add_required_field("standard_metadata", "egress_port");
  //! @endcode
  void add_required_field(const std::string &header_name,
                          const std::string &field_name);

  //! Checks that the given field exists for context \p cxt_id, i.e. checks that
  //! the field was defined in the input JSON used to configure that context.
  bool field_exists(cxt_id_t cxt_id, const std::string &header_name,
                    const std::string &field_name) const {
    return contexts.at(cxt_id).field_exists(header_name, field_name);
  }

  //! Force arithmetic on field. No effect if field is not defined in the input
  //! JSON. For optimization reasons, only fields on which arithmetic will be
  //! performed receive the ability to perform arithmetic operations. These
  //! special fields are determined by analyzing the P4 program / the JSON
  //! input. For example, if a field is used in a primitive action call,
  //! arithmetic will be automatically enabled for this field in bmv2. Calling
  //! Field::get() on a Field instance for which arithmetic has not been abled
  //! will result in a segfault or an assert. If your target needs to enable
  //! arithmetic on a field for which arithmetic was not automatically enabled
  //! (could happen in some rare cases), you can enable it manually by calling
  //! this method.
  void force_arith_field(const std::string &header_name,
                         const std::string &field_name);

  //! Force arithmetic on all the fields of header \p header_name. No effect if
  //! the header is not defined in the input JSON. Is equivalent to calling
  //! force_arith_field() on all fields in the header. See force_arith_field()
  //! for more information.
  void force_arith_header(const std::string &header_name);

  //! Use a custom GroupSelectionIface implementation for dataplane member
  //! selection for action profile with name \p action_profile_name. Returns
  //! false in case of failure (if the action profile name is not valid).
  bool set_group_selector(
      cxt_id_t cxt_id,
      const std::string &act_prof_name,
      std::shared_ptr<ActionProfile::GroupSelectionIface> selector);

  //! Get the number of contexts included in this switch
  size_t get_nb_cxts() { return nb_cxts; }

  int init_objects(const std::string &json_path, device_id_t device_id = 0,
                   std::shared_ptr<TransportIface> notif_transport = nullptr);

  int init_objects_empty(device_id_t dev_id,
                         std::shared_ptr<TransportIface> transport);

  //! Initialize the switch using command line options. This function is meant
  //! to be called right after your switch instance has been constructed. For
  //! example, in the case of the standard simple switch target:
  //! @code
  //! simple_switch = new SimpleSwitch();
  //! int status = simple_switch->init_from_command_line_options(argc, argv);
  //! if (status != 0) std::exit(status);
  //! @endcode
  //! If your target has custom CLI options, you can provide a pointer \p tp to
  //! a secondary parser which implements the TargetParserIface interface. The
  //! bm::TargetParserIface::parse method will be called with the unrecognized
  //! options. Target specific options need to appear after bmv2 general
  //! options on the command line, and be separated from them by `--`. For
  //! example:
  //! @code
  //! <my_target_exe> prog.json -i 0@eth0 -- --my-option v
  //! @endcode
  //! If you wish to use your own TransportIface implementation for
  //! notifications instead of the default nanomsg one, you can provide
  //! one. Similarly if you want to provide your own DevMgrIface implementation
  //! for packet I/O, you can do so. Note that even when using your own
  //! DevMgrIface implementation, you can still use the `--interface` (or `-i`)
  //! command-line option; we will call port_add on your implementation
  //! appropriately.
  int init_from_command_line_options(
      int argc, char *argv[],
      TargetParserIface *tp = nullptr,
      std::shared_ptr<TransportIface> my_transport = nullptr,
      std::unique_ptr<DevMgrIface> my_dev_mgr = nullptr);

  //! Initialize the switch using an bm::OptionsParser instance. This is similar
  //! to init_from_command_line_options() but the target is responsible for
  //! parsing the command-line options itself. In other words, the target needs
  //! to instantiate a bm::OptionsParser object, invoke
  //! bm::OptionsParser::parse() on it and pass the object to this method as \p
  //! parser. This is useful if the target needs to access some of the
  //! command-line options before initializing the switch. For example, the
  //! target may want to use a custom bm::DevMgrIface implementation and may
  //! need some information from the command-line to instantiate it.
  int init_from_options_parser(
      const OptionsParser &parser,
      std::shared_ptr<TransportIface> my_transport = nullptr,
      std::unique_ptr<DevMgrIface> my_dev_mgr = nullptr);

  //! Retrieve the shared pointer to an object of type `T` previously added to
  //! the switch using add_component().
  template<typename T>
  std::shared_ptr<T> get_component() {
    const auto &search = components.find(std::type_index(typeid(T)));
    if (search == components.end()) return nullptr;
    return std::static_pointer_cast<T>(search->second);
  }

  //! Retrieve the shared pointer to an object of type `T` previously added to
  //! one of the switch contexts using add_cxt_component().
  template<typename T>
  std::shared_ptr<T> get_cxt_component(cxt_id_t cxt_id) {
    return contexts.at(cxt_id).get_component<T>();
  }

  //! Returns true if a configuration swap was requested by the control
  //! plane. See switch.h documentation for more information.
  int swap_requested();

  //! Performs a configuration swap if one was requested by the control
  //! plane. Returns `0` if a swap had indeed been requested, `1` otherwise. If
  //! a swap was requested, the method will prevent new Packet instances from
  //! being created and will block until all existing instances have been
  //! destroyed. It will then perform the swap. Care should be taken when using
  //! this function, as it invalidates some pointers that your target may still
  //! be using. See switch.h documentation for more information.
  int do_swap();

  //! Utility function which prevents new Packet instances from being
  //! created and blocks until all existing Packet instances have been
  //! destroyed in all contexts
  void block_until_no_more_packets();

  //! Construct and return a Packet instance for the given \p cxt_id.
  std::unique_ptr<Packet> new_packet_ptr(cxt_id_t cxt_id, port_t ingress_port,
                                         packet_id_t id, int ingress_length,
                                         // cpplint false positive
                                         // NOLINTNEXTLINE(whitespace/operators)
                                         PacketBuffer &&buffer);

  //! @copydoc new_packet_ptr
  Packet new_packet(cxt_id_t cxt_id, port_t ingress_port, packet_id_t id,
                    // cpplint false positive
                    // NOLINTNEXTLINE(whitespace/operators)
                    int ingress_length, PacketBuffer &&buffer);

  //! Obtain a pointer to the LearnEngine for a given Context
  LearnEngineIface *get_learn_engine(cxt_id_t cxt_id) {
    return contexts.at(cxt_id).get_learn_engine();
  }

  AgeingMonitorIface *get_ageing_monitor(cxt_id_t cxt_id) {
    return contexts.at(cxt_id).get_ageing_monitor();
  }

  //! Return string-to-string map of the target-specific options included in the
  //! input config JSON for a given context.
  ConfigOptionMap get_config_options(cxt_id_t cxt_id) const {
    return contexts.at(cxt_id).get_config_options();
  }

  //! Return a copy of the error codes map (a bi-directional map between an
  //! error code's integral value and its name / description) for a given
  //! context.
  ErrorCodeMap get_error_codes(cxt_id_t cxt_id) const {
    return contexts.at(cxt_id).get_error_codes();
  }

  // meant for testing
  int transport_send_probe(uint64_t x) const;

  // ---------- RuntimeInterface ----------

  MatchErrorCode
  mt_get_num_entries(cxt_id_t cxt_id,
                     const std::string &table_name,
                     size_t *num_entries) const override {
    return contexts.at(cxt_id).mt_get_num_entries(table_name, num_entries);
  }

  MatchErrorCode
  mt_clear_entries(cxt_id_t cxt_id,
                   const std::string &table_name,
                   bool reset_default_entry) override {
    return contexts.at(cxt_id).mt_clear_entries(table_name,
                                                reset_default_entry);
  }

  MatchErrorCode
  mt_add_entry(cxt_id_t cxt_id,
               const std::string &table_name,
               const std::vector<MatchKeyParam> &match_key,
               const std::string &action_name,
               ActionData action_data,
               entry_handle_t *handle,
               int priority = -1  /*only used for ternary*/) override {
    return contexts.at(cxt_id).mt_add_entry(
        table_name, match_key, action_name,
        std::move(action_data), handle, priority);
  }

  MatchErrorCode
  mt_set_default_action(cxt_id_t cxt_id,
                        const std::string &table_name,
                        const std::string &action_name,
                        ActionData action_data) override {
    return contexts.at(cxt_id).mt_set_default_action(
        table_name, action_name, std::move(action_data));
  }

  MatchErrorCode
  mt_reset_default_entry(cxt_id_t cxt_id,
                         const std::string &table_name) override {
    return contexts.at(cxt_id).mt_reset_default_entry(table_name);
  }

  MatchErrorCode
  mt_delete_entry(cxt_id_t cxt_id,
                  const std::string &table_name,
                  entry_handle_t handle) override {
    return contexts.at(cxt_id).mt_delete_entry(table_name, handle);
  }

  MatchErrorCode
  mt_modify_entry(cxt_id_t cxt_id,
                  const std::string &table_name,
                  entry_handle_t handle,
                  const std::string &action_name,
                  ActionData action_data) override {
    return contexts.at(cxt_id).mt_modify_entry(
        table_name, handle, action_name, std::move(action_data));
  }

  MatchErrorCode
  mt_set_entry_ttl(cxt_id_t cxt_id,
                   const std::string &table_name,
                   entry_handle_t handle,
                   unsigned int ttl_ms) override {
    return contexts.at(cxt_id).mt_set_entry_ttl(table_name, handle, ttl_ms);
  }

  // action profiles

  MatchErrorCode
  mt_act_prof_add_member(cxt_id_t cxt_id,
                         const std::string &act_prof_name,
                         const std::string &action_name,
                         ActionData action_data,
                         mbr_hdl_t *mbr) override {
    return contexts.at(cxt_id).mt_act_prof_add_member(
        act_prof_name, action_name, std::move(action_data), mbr);
  }

  MatchErrorCode
  mt_act_prof_delete_member(cxt_id_t cxt_id,
                            const std::string &act_prof_name,
                            mbr_hdl_t mbr) override {
    return contexts.at(cxt_id).mt_act_prof_delete_member(act_prof_name, mbr);
  }

  MatchErrorCode
  mt_act_prof_modify_member(cxt_id_t cxt_id,
                            const std::string &act_prof_name,
                            mbr_hdl_t mbr,
                            const std::string &action_name,
                            ActionData action_data) override {
    return contexts.at(cxt_id).mt_act_prof_modify_member(
        act_prof_name, mbr, action_name, std::move(action_data));
  }

  MatchErrorCode
  mt_act_prof_create_group(cxt_id_t cxt_id,
                           const std::string &act_prof_name,
                           grp_hdl_t *grp) override {
    return contexts.at(cxt_id).mt_act_prof_create_group(act_prof_name, grp);
  }

  MatchErrorCode
  mt_act_prof_delete_group(cxt_id_t cxt_id,
                           const std::string &act_prof_name,
                           grp_hdl_t grp) override {
    return contexts.at(cxt_id).mt_act_prof_delete_group(act_prof_name, grp);
  }

  MatchErrorCode
  mt_act_prof_add_member_to_group(cxt_id_t cxt_id,
                                  const std::string &act_prof_name,
                                  mbr_hdl_t mbr, grp_hdl_t grp) override {
    return contexts.at(cxt_id).mt_act_prof_add_member_to_group(
        act_prof_name, mbr, grp);
  }

  MatchErrorCode
  mt_act_prof_remove_member_from_group(cxt_id_t cxt_id,
                                       const std::string &act_prof_name,
                                       mbr_hdl_t mbr, grp_hdl_t grp) override {
    return contexts.at(cxt_id).mt_act_prof_remove_member_from_group(
        act_prof_name, mbr, grp);
  }

  std::vector<ActionProfile::Member>
  mt_act_prof_get_members(cxt_id_t cxt_id,
                          const std::string &act_prof_name) const override {
    return contexts.at(cxt_id).mt_act_prof_get_members(act_prof_name);
  }

  MatchErrorCode
  mt_act_prof_get_member(cxt_id_t cxt_id, const std::string &act_prof_name,
                         mbr_hdl_t mbr,
                         ActionProfile::Member *member) const override {
    return contexts.at(cxt_id).mt_act_prof_get_member(
        act_prof_name, mbr, member);
  }

  std::vector<ActionProfile::Group>
  mt_act_prof_get_groups(cxt_id_t cxt_id,
                         const std::string &act_prof_name) const override {
    return contexts.at(cxt_id).mt_act_prof_get_groups(act_prof_name);
  }

  MatchErrorCode
  mt_act_prof_get_group(cxt_id_t cxt_id, const std::string &act_prof_name,
                        grp_hdl_t grp,
                        ActionProfile::Group *group) const override {
    return contexts.at(cxt_id).mt_act_prof_get_group(act_prof_name, grp, group);
  }

  // indirect tables

  MatchErrorCode
  mt_indirect_add_entry(cxt_id_t cxt_id,
                        const std::string &table_name,
                        const std::vector<MatchKeyParam> &match_key,
                        mbr_hdl_t mbr,
                        entry_handle_t *handle,
                        int priority = 1) override {
    return contexts.at(cxt_id).mt_indirect_add_entry(
        table_name, match_key, mbr, handle, priority);
  }

  MatchErrorCode
  mt_indirect_modify_entry(cxt_id_t cxt_id,
                           const std::string &table_name,
                           entry_handle_t handle,
                           mbr_hdl_t mbr) override {
    return contexts.at(cxt_id).mt_indirect_modify_entry(
        table_name, handle, mbr);
  }

  MatchErrorCode
  mt_indirect_delete_entry(cxt_id_t cxt_id,
                           const std::string &table_name,
                           entry_handle_t handle) override {
    return contexts.at(cxt_id).mt_indirect_delete_entry(table_name, handle);
  }

  MatchErrorCode
  mt_indirect_set_entry_ttl(cxt_id_t cxt_id,
                            const std::string &table_name,
                            entry_handle_t handle,
                            unsigned int ttl_ms) override {
    return contexts.at(cxt_id).mt_indirect_set_entry_ttl(
        table_name, handle, ttl_ms);
  }

  MatchErrorCode
  mt_indirect_set_default_member(cxt_id_t cxt_id,
                                 const std::string &table_name,
                                 mbr_hdl_t mbr) override {
    return contexts.at(cxt_id).mt_indirect_set_default_member(table_name, mbr);
  }

  MatchErrorCode
  mt_indirect_reset_default_entry(cxt_id_t cxt_id,
                                  const std::string &table_name) override {
    return contexts.at(cxt_id).mt_indirect_reset_default_entry(table_name);
  }

  MatchErrorCode
  mt_indirect_ws_add_entry(cxt_id_t cxt_id,
                           const std::string &table_name,
                           const std::vector<MatchKeyParam> &match_key,
                           grp_hdl_t grp,
                           entry_handle_t *handle,
                           int priority = 1) override {
    return contexts.at(cxt_id).mt_indirect_ws_add_entry(
        table_name, match_key, grp, handle, priority);
  }

  MatchErrorCode
  mt_indirect_ws_modify_entry(cxt_id_t cxt_id,
                              const std::string &table_name,
                              entry_handle_t handle,
                              grp_hdl_t grp) override {
    return contexts.at(cxt_id).mt_indirect_ws_modify_entry(
        table_name, handle, grp);
  }

  MatchErrorCode
  mt_indirect_ws_set_default_group(cxt_id_t cxt_id,
                                   const std::string &table_name,
                                   grp_hdl_t grp) override {
    return contexts.at(cxt_id).mt_indirect_ws_set_default_group(
        table_name, grp);
  }

  MatchTableType
  mt_get_type(cxt_id_t cxt_id, const std::string &table_name) const override {
    return contexts.at(cxt_id).mt_get_type(table_name);
  }

  std::vector<MatchTable::Entry>
  mt_get_entries(cxt_id_t cxt_id,
                 const std::string &table_name) const override {
    return contexts.at(cxt_id).mt_get_entries<MatchTable>(table_name);
  }

  std::vector<MatchTableIndirect::Entry>
  mt_indirect_get_entries(cxt_id_t cxt_id,
                          const std::string &table_name) const override {
    return contexts.at(cxt_id).mt_get_entries<MatchTableIndirect>(table_name);
  }

  std::vector<MatchTableIndirectWS::Entry>
  mt_indirect_ws_get_entries(cxt_id_t cxt_id,
                             const std::string &table_name) const override {
    return contexts.at(cxt_id).mt_get_entries<MatchTableIndirectWS>(table_name);
  }

  MatchErrorCode
  mt_get_entry(cxt_id_t cxt_id, const std::string &table_name,
               entry_handle_t handle, MatchTable::Entry *entry) const override {
    return contexts.at(cxt_id).mt_get_entry<MatchTable>(
        table_name, handle, entry);
  }

  MatchErrorCode
  mt_indirect_get_entry(cxt_id_t cxt_id, const std::string &table_name,
                        entry_handle_t handle,
                        MatchTableIndirect::Entry *entry) const override {
    return contexts.at(cxt_id).mt_get_entry<MatchTableIndirect>(
        table_name, handle, entry);
  }

  MatchErrorCode
  mt_indirect_ws_get_entry(cxt_id_t cxt_id, const std::string &table_name,
                           entry_handle_t handle,
                           MatchTableIndirectWS::Entry *entry) const override {
    return contexts.at(cxt_id).mt_get_entry<MatchTableIndirectWS>(
        table_name, handle, entry);
  }

  MatchErrorCode
  mt_get_default_entry(cxt_id_t cxt_id, const std::string &table_name,
                       MatchTable::Entry *entry) const override {
    return contexts.at(cxt_id).mt_get_default_entry<MatchTable>(
        table_name, entry);
  }

  MatchErrorCode
  mt_indirect_get_default_entry(
      cxt_id_t cxt_id, const std::string &table_name,
      MatchTableIndirect::Entry *entry) const override {
    return contexts.at(cxt_id).mt_get_default_entry<MatchTableIndirect>(
        table_name, entry);
  }

  MatchErrorCode
  mt_indirect_ws_get_default_entry(
      cxt_id_t cxt_id, const std::string &table_name,
      MatchTableIndirectWS::Entry *entry) const override {
    return contexts.at(cxt_id).mt_get_default_entry<MatchTableIndirectWS>(
        table_name, entry);
  }

  MatchErrorCode
  mt_get_entry_from_key(cxt_id_t cxt_id, const std::string &table_name,
                        const std::vector<MatchKeyParam> &match_key,
                        MatchTable::Entry *entry,
                        int priority = 1) const override {
    return contexts.at(cxt_id).mt_get_entry_from_key<MatchTable>(
        table_name, match_key, entry, priority);
  }

  MatchErrorCode
  mt_indirect_get_entry_from_key(cxt_id_t cxt_id, const std::string &table_name,
                                 const std::vector<MatchKeyParam> &match_key,
                                 MatchTableIndirect::Entry *entry,
                                 int priority = 1) const override {
    return contexts.at(cxt_id).mt_get_entry_from_key<MatchTableIndirect>(
        table_name, match_key, entry, priority);
  }

  MatchErrorCode
  mt_indirect_ws_get_entry_from_key(cxt_id_t cxt_id,
                                    const std::string &table_name,
                                    const std::vector<MatchKeyParam> &match_key,
                                    MatchTableIndirectWS::Entry *entry,
                                    int priority = 1) const override {
    return contexts.at(cxt_id).mt_get_entry_from_key<MatchTableIndirectWS>(
        table_name, match_key, entry, priority);
  }

  MatchErrorCode
  mt_read_counters(cxt_id_t cxt_id,
                   const std::string &table_name,
                   entry_handle_t handle,
                   MatchTableAbstract::counter_value_t *bytes,
                   MatchTableAbstract::counter_value_t *packets) override {
    return contexts.at(cxt_id).mt_read_counters(
        table_name, handle, bytes, packets);
  }

  MatchErrorCode
  mt_reset_counters(cxt_id_t cxt_id,
                    const std::string &table_name) override {
    return contexts.at(cxt_id).mt_reset_counters(table_name);
  }

  MatchErrorCode
  mt_write_counters(cxt_id_t cxt_id,
                    const std::string &table_name,
                    entry_handle_t handle,
                    MatchTableAbstract::counter_value_t bytes,
                    MatchTableAbstract::counter_value_t packets) override {
    return contexts.at(cxt_id).mt_write_counters(
        table_name, handle, bytes, packets);
  }

  MatchErrorCode
  mt_set_meter_rates(
      cxt_id_t cxt_id, const std::string &table_name, entry_handle_t handle,
      const std::vector<Meter::rate_config_t> &configs) override {
    return contexts.at(cxt_id).mt_set_meter_rates(table_name, handle, configs);
  }

  MatchErrorCode
  mt_get_meter_rates(
      cxt_id_t cxt_id, const std::string &table_name, entry_handle_t handle,
      std::vector<Meter::rate_config_t> *configs) override {
    return contexts.at(cxt_id).mt_get_meter_rates(table_name, handle, configs);
  }

  MatchErrorCode
  mt_reset_meter_rates(
      cxt_id_t cxt_id, const std::string &table_name,
      entry_handle_t handle) override {
    return contexts.at(cxt_id).mt_reset_meter_rates(table_name, handle);
  }

  Counter::CounterErrorCode
  read_counters(cxt_id_t cxt_id,
                const std::string &counter_name,
                size_t index,
                MatchTableAbstract::counter_value_t *bytes,
                MatchTableAbstract::counter_value_t *packets) override {
    return contexts.at(cxt_id).read_counters(
        counter_name, index, bytes, packets);
  }

  Counter::CounterErrorCode
  reset_counters(cxt_id_t cxt_id,
                 const std::string &counter_name) override {
    return contexts.at(cxt_id).reset_counters(counter_name);
  }

  Counter::CounterErrorCode
  write_counters(cxt_id_t cxt_id,
                 const std::string &counter_name,
                 size_t index,
                 MatchTableAbstract::counter_value_t bytes,
                 MatchTableAbstract::counter_value_t packets) override {
    return contexts.at(cxt_id).write_counters(
        counter_name, index, bytes, packets);
  }

  MeterErrorCode
  meter_array_set_rates(
      cxt_id_t cxt_id, const std::string &meter_name,
      const std::vector<Meter::rate_config_t> &configs) override {
    return contexts.at(cxt_id).meter_array_set_rates(meter_name, configs);
  }

  MeterErrorCode
  meter_set_rates(cxt_id_t cxt_id,
                  const std::string &meter_name, size_t idx,
                  const std::vector<Meter::rate_config_t> &configs) override {
    return contexts.at(cxt_id).meter_set_rates(meter_name, idx, configs);
  }

  MeterErrorCode
  meter_get_rates(cxt_id_t cxt_id,
                  const std::string &meter_name, size_t idx,
                  std::vector<Meter::rate_config_t> *configs) override {
    return contexts.at(cxt_id).meter_get_rates(meter_name, idx, configs);
  }

  MeterErrorCode
  meter_reset_rates(cxt_id_t cxt_id,
                    const std::string &meter_name, size_t idx) override {
    return contexts.at(cxt_id).meter_reset_rates(meter_name, idx);
  }

  RegisterErrorCode
  register_read(cxt_id_t cxt_id,
                const std::string &register_name,
                const size_t idx, Data *value) override {
    return contexts.at(cxt_id).register_read(register_name, idx, value);
  }

  std::vector<Data>
  register_read_all(cxt_id_t cxt_id,
                    const std::string &register_name) override {
    return contexts.at(cxt_id).register_read_all(register_name);
  }

  RegisterErrorCode
  register_write(cxt_id_t cxt_id,
                 const std::string &register_name,
                 const size_t idx, Data value) override {
    return contexts.at(cxt_id).register_write(
        register_name, idx, std::move(value));
  }

  RegisterErrorCode
  register_write_range(cxt_id_t cxt_id,
                       const std::string &register_name,
                       const size_t start, const size_t end,
                       Data value) override {
    return contexts.at(cxt_id).register_write_range(
        register_name, start, end, std::move(value));
  }

  RegisterErrorCode
  register_reset(cxt_id_t cxt_id, const std::string &register_name) override {
    return contexts.at(cxt_id).register_reset(register_name);
  }

  ParseVSet::ErrorCode
  parse_vset_add(cxt_id_t cxt_id, const std::string &parse_vset_name,
                 const ByteContainer &value) override {
    return contexts.at(cxt_id).parse_vset_add(parse_vset_name, value);
  }

  ParseVSet::ErrorCode
  parse_vset_remove(cxt_id_t cxt_id, const std::string &parse_vset_name,
                    const ByteContainer &value) override {
    return contexts.at(cxt_id).parse_vset_remove(parse_vset_name, value);
  }

  ParseVSet::ErrorCode
  parse_vset_get(cxt_id_t cxt_id, const std::string &parse_vset_name,
                 std::vector<ByteContainer> *values) override {
    return contexts.at(cxt_id).parse_vset_get(parse_vset_name, values);
  }

  ParseVSet::ErrorCode
  parse_vset_clear(cxt_id_t cxt_id,
                   const std::string &parse_vset_name) override {
    return contexts.at(cxt_id).parse_vset_clear(parse_vset_name);
  }

  RuntimeInterface::ErrorCode
  reset_state() override;

  RuntimeInterface::ErrorCode
  serialize(std::ostream *out) override;

  RuntimeInterface::ErrorCode
  load_new_config(const std::string &new_config) override;

  RuntimeInterface::ErrorCode
  swap_configs() override;

  std::string get_config() const override;
  std::string get_config_md5() const override;

  P4Objects::IdLookupErrorCode p4objects_id_from_name(
      cxt_id_t cxt_id, P4Objects::ResourceType type, const std::string &name,
      p4object_id_t *id) const;

  // conscious choice not to use templates here (or could not use virtual)
  CustomCrcErrorCode
  set_crc16_custom_parameters(
      cxt_id_t cxt_id, const std::string &calc_name,
      const CustomCrcMgr<uint16_t>::crc_config_t &crc16_config) override;

  CustomCrcErrorCode
  set_crc32_custom_parameters(
      cxt_id_t cxt_id, const std::string &calc_name,
      const CustomCrcMgr<uint32_t>::crc_config_t &crc32_config) override;

  int
  mt_runtime_reconfig(cxt_id_t cxt_id,
                      const std::string &json_file,
                      const std::string &plan_file) {
    std::ifstream json_file_stream(json_file, std::ios::in);
    if (!json_file_stream) {
      BMLOG_ERROR("JSON input file {} can't be opened", json_file);
      return static_cast<int>(RuntimeReconfigErrorCode::OPEN_JSON_FILE_FAIL);
    }

    std::ifstream plan_file_stream(plan_file);
    if (!plan_file_stream) {
      BMLOG_ERROR("Open plan file {} failed", plan_file);
      return static_cast<int>(RuntimeReconfigErrorCode::OPEN_PLAN_FILE_FAIL);
    }

    int reconfig_return_code = mt_runtime_reconfig_with_stream(0, &json_file_stream, &plan_file_stream);

    if (reconfig_return_code != static_cast<int>(RuntimeReconfigErrorCode::SUCCESS)) {
      return reconfig_return_code;
    }

    std::ofstream ofs(json_file+".new", std::ios::out);
    if (!ofs) {
      BMLOG_ERROR("Error: cannot open output file: {}", json_file + ".new");
      return static_cast<int>(RuntimeReconfigErrorCode::OPEN_OUTPUT_FILE_FAIL);
    }

    contexts.at(cxt_id).print_runtime_cfg(ofs);

    std::cout << "table reconfig successfully" << std::endl;
    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  // This function aims to:
  // 1. To be called by mt_runtime_reconfig
  // 2. To be used in tests for the convenience of getting commands directly
  int
  mt_runtime_reconfig_with_stream(cxt_id_t cxt_id,
                                  std::istream* json_file_stream,
                                  std::istream* plan_file_stream,
                                  const std::string& output_json_file = "") {
    RuntimeReconfigErrorCode reconfig_return_code = contexts.at(cxt_id).mt_runtime_reconfig_with_stream(json_file_stream, 
                                                                                                        plan_file_stream,
                                                                                                        get_lookup_factory(),
                                                                                                        required_fields,
                                                                                                        arith_objects);

    if (reconfig_return_code != RuntimeReconfigErrorCode::SUCCESS) {
      return static_cast<int>(reconfig_return_code);
    }

    if (output_json_file.empty()) {
      std::cout << "table reconfig successfully" << std::endl;
      return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
    }

    std::ofstream ofs(output_json_file+".new", std::ios::out);
    if (!ofs) {
      BMLOG_ERROR("Error: cannot open output file: {}", output_json_file + ".new");
      return static_cast<int>(RuntimeReconfigErrorCode::OPEN_OUTPUT_FILE_FAIL);
    }

    contexts.at(cxt_id).print_runtime_cfg(ofs);

    std::cout << "table reconfig successfully" << std::endl;
    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }
  
  private:
  // These three functions (convert_id_to_name, dup_check, hash_function_check) 
  // are originally from context.cpp
  // They are moved here for the convenience of gRPC runtime reconfiguartion
  
  // helper function for FlexCore
  // It will return 0, if success
  // Otherwise, return 1 if the id is unfound, or return 2 if the prefix is wrong
  int 
  convert_id_to_name(std::unordered_map<std::string, std::string> &id2newNodeName, 
      std::string *out, std::string *in, int size) {
    for (int i = 0; i < size; i++) {
      if (in[i] == "null") {
        out[i] = "";
        continue;
      }
      const std::string prefix = in[i].substr(0, 3);
      const std::string actual_name = in[i].substr(4);
      if (prefix == "new"
          || prefix == "flx") {
        if (id2newNodeName.find(in[i]) == id2newNodeName.end()) {
          BMLOG_ERROR("Error: cannot find the id {} from id2newNodeName", in[i]);
          return 1;
        }
        out[i] = id2newNodeName[in[i]];
      } else if (prefix == "old") {
        out[i] = actual_name;
      } else {
        BMLOG_ERROR("Error: prefix {} has no match", prefix);
        return 2;
      }
    }

    return 0;
  }

  // helper function for FlexCore
  int
  dup_check(const std::unordered_map<std::string, std::string> &id2newNodeName, 
      const std::string &name) {
    if (id2newNodeName.find(name) != id2newNodeName.end()) {
      BMLOG_ERROR("Error: Duplicated id {} from id2newNodeName", name);
      return 1;
    }

    return 0;
  }

  int 
  hash_function_check(const std::string& name) {
    if (!CalculationsMap::get_instance()->get_copy(name)) {
      BMLOG_ERROR("Error: can't find the hash function by name: {}", name);
      return 1;
    }

    return 0; 
  }
  
  public:

  int 
  mt_runtime_reconfig_init_p4objects_new(cxt_id_t cxt_id, 
                                         const char* p4objects_new_json) {
    std::istringstream json_ss(p4objects_new_json);
    if (!json_ss) {
      BMLOG_ERROR("JSON stream can't be opened when initiating p4objects_new");
      return static_cast<int>(RuntimeReconfigErrorCode::OPEN_JSON_STREAM_FAIL);
    }

    auto& p4objects_new = contexts.at(cxt_id).p4objects_new;
    p4objects_new = std::make_shared<P4Objects>(std::cout, true);
    int status = p4objects_new->init_objects(&json_ss, 
                                             get_lookup_factory(), 
                                             contexts.at(cxt_id).device_id, 
                                             cxt_id, 
                                             contexts.at(cxt_id).notifications_transport,
                                             required_fields,
                                             arith_objects);

    if (status) return static_cast<int>(RuntimeReconfigErrorCode::P4OBJECTS_INIT_FAIL);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_insert_table(cxt_id_t cxt_id, 
                                   const char* pipeline_name, 
                                   const char* table_name) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = table_name;

    auto& context = contexts.at(cxt_id);

    const std::string prefix = items[0].substr(0, 3);
    const std::string actual_name = items[0].substr(4);
    if (prefix != "new") {
      BMLOG_ERROR("Error: inserted table should only have prefix 'new_', but you enter {}", items[0]);
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    if (dup_check(context.id2newNodeName, items[0])) {
      return static_cast<int>(RuntimeReconfigErrorCode::DUP_CHECK_ERROR);
    }

    context.id2newNodeName[items[0]] = 
      context.p4objects_rt->insert_match_table_rt(context.p4objects_new, pipeline, actual_name, true);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int mt_runtime_reconfig_change_table(cxt_id_t cxt_id,
                                       const char* pipeline_name,
                                       const char* table_name,
                                       const char* edge_name,
                                       const char* table_name_next) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = table_name;
    items[1] = table_name_next;
    items[2] = edge_name;

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 2);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    context.p4objects_rt->change_table_next_node_rt(pipeline, vals[0], items[2], vals[1]);
  
    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_delete_table(cxt_id_t cxt_id,
                                   const char* pipeline_name,
                                   const char* table_name) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = table_name;

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 1);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }

    context.p4objects_rt->delete_match_table_rt(pipeline, vals[0]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_insert_conditional(cxt_id_t cxt_id,
                                         const char* pipeline_name,
                                         const char* branch_name) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = branch_name;

    auto& context = contexts.at(cxt_id);

    const std::string prefix = items[0].substr(0, 3);
    const std::string actual_name = items[0].substr(4);
    if (prefix != "new") {
      BMLOG_ERROR("Error: inserted cond should only have prefix 'new_', but you enter {}", items[0]);
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    if (dup_check(context.id2newNodeName, items[0])) {
      return static_cast<int>(RuntimeReconfigErrorCode::DUP_CHECK_ERROR);
    }
    context.id2newNodeName[items[0]] = context.p4objects_rt->insert_conditional_rt(context.p4objects_new, pipeline, actual_name, true);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_change_conditional(cxt_id_t cxt_id, 
                                         const char* pipeline_name,
                                         const char* branch_name,
                                         bool true_or_false_next,
                                         const char* node_name) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = branch_name;
    items[1] = node_name;
    items[2] = true_or_false_next ? "true_next" : "false_next";

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 2);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    context.p4objects_rt->change_conditional_next_node_rt(pipeline, vals[0], items[2], vals[1]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_delete_conditional(cxt_id_t cxt_id,
                                         const char* pipeline_name,
                                         const char* branch_name) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = branch_name;

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 1);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }

    context.p4objects_rt->delete_conditional_rt(pipeline, vals[0]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_insert_flex(cxt_id_t cxt_id,
                                  const char* pipeline_name,
                                  const char* node_name,
                                  const char* true_next_node,
                                  const char* false_next_node) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = node_name;
    items[1] = true_next_node;
    items[2] = false_next_node;

    auto& context = contexts.at(cxt_id);

    const std::string prefix = items[0].substr(0, 3);
    const std::string actual_name = items[0].substr(4);
    if (prefix != "flx") {
      BMLOG_ERROR("Error: inserted flex should only have prefix 'flx_', but you enter {}", items[0]);
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items+1, 2);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    if (dup_check(context.id2newNodeName, items[0])) {
        return static_cast<int>(RuntimeReconfigErrorCode::DUP_CHECK_ERROR);
    }

   int func_mount_point_number_value = std::numeric_limits<int>::min();
    size_t first_occurance_of_sign = actual_name.find('$');
    size_t last_occurance_of_sign = actual_name.find_last_of('$');
    if (first_occurance_of_sign != std::string::npos && last_occurance_of_sign != std::string::npos) {
      if (actual_name.substr(0, first_occurance_of_sign) == "flex_func_mount_point_number_") {
      
        int func_mount_point_number = std::stoi(actual_name.substr(first_occurance_of_sign + 1, 
                                                                    last_occurance_of_sign - 
                                                                    first_occurance_of_sign - 1));
        if (func_mount_point_number < 0) {
          BMLOG_ERROR("FlexCore Error: invalid func_mount_point_number {}", func_mount_point_number);
          return static_cast<int>(RuntimeReconfigErrorCode::INVALID_COMMAND_ERROR);
        } else {
          func_mount_point_number_value = func_mount_point_number;
        }
      }
    }
  
    context.id2newNodeName[items[0]] = func_mount_point_number_value == std::numeric_limits<int>::min() ?
                                                context.p4objects_rt->insert_flex_rt(pipeline, vals[0], vals[1], -1) :
                                                context.p4objects_rt->insert_flex_rt(pipeline, vals[0], vals[1], func_mount_point_number_value);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_change_flex(cxt_id_t cxt_id,
                                  const char* pipeline_name,
                                  const char* flx_name,
                                  bool true_or_false_next,
                                  const char* node_next) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = flx_name;
    items[1] = node_next;
    items[2] = true_or_false_next ? "true_next" : "false_next";

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 2);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    context.p4objects_rt->change_conditional_next_node_rt(pipeline, vals[0], items[2], vals[1]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_delete_flex(cxt_id_t cxt_id,
                                  const char* pipeline_name,
                                  const char* flx_name) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = flx_name;

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 1);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }

    context.p4objects_rt->delete_flex_rt(pipeline, vals[0]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_insert_register_array(cxt_id_t cxt_id,
                                            const char* register_array_name,
                                            const uint32_t register_array_size,
                                            const uint32_t register_array_bitwidth) {
    std::string items[3], vals[3];
    items[0] = register_array_name;
    vals[0] = std::to_string(register_array_size);
    vals[1] = std::to_string(register_array_bitwidth);

    auto& context = contexts.at(cxt_id);

    const std::string prefix = items[0].substr(0, 3);
    const std::string actual_name = items[0].substr(4);
    if (prefix != "new") {
      BMLOG_ERROR("Error: inserted register_array should only have prefix 'new_', but you enter {}", items[0]);
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    if (dup_check(context.id2newNodeName, items[0])) {
      return static_cast<int>(RuntimeReconfigErrorCode::DUP_CHECK_ERROR);
    }
    context.id2newNodeName[items[0]] = context.p4objects_rt->insert_register_array_rt(actual_name, vals[0], vals[1]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_change_register_array(cxt_id_t cxt_id,
                                            const char* register_array_name,
                                            const uint32_t change_type,
                                            const uint32_t new_value) {
    std::string items[3], vals[3];
    items[0] = register_array_name;
    items[1] = std::to_string(new_value);

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 1);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }

    if (change_type == 0) {
      context.p4objects_rt->change_register_array_size_rt(vals[0], items[1]);
    } else if (change_type == 1) {
      context.p4objects_rt->change_register_array_bitwidth_rt(vals[0], items[1]);
    } else {
      BMLOG_ERROR("Error: invalid change_type when changing register_array {}", register_array_name);
      return static_cast<int>(RuntimeReconfigErrorCode::INVALID_COMMAND_ERROR);
    }
    
    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_delete_register_array(cxt_id_t cxt_id,
                                            const char* register_array_name) {
    std::string items[3], vals[3];
    items[0] = register_array_name;

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 1);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }

    context.p4objects_rt->delete_register_array_rt(vals[0]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_trigger(cxt_id_t cxt_id,
                              bool on_or_off,
                              int trigger_number = -1) {
    auto& context = contexts.at(cxt_id);
    if (on_or_off) {
      context.p4objects_rt->flex_trigger_rt(true, trigger_number);
    } else {
      context.p4objects_rt->flex_trigger_rt(false, trigger_number);
    }

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  int
  mt_runtime_reconfig_change_init(cxt_id_t cxt_id,
                                  const char* pipeline_name,
                                  const char* table_name_next) {
    std::string pipeline(pipeline_name);

    std::string items[3], vals[3];
    items[0] = table_name_next;

    auto& context = contexts.at(cxt_id);

    int convert_id_return_code = convert_id_to_name(context.id2newNodeName, vals, items, 1);
    if (convert_id_return_code == 1) {
      return static_cast<int>(RuntimeReconfigErrorCode::UNFOUND_ID_ERROR);
    } else if (convert_id_return_code == 2) {
      return static_cast<int>(RuntimeReconfigErrorCode::PREFIX_ERROR);
    }
    context.p4objects_rt->change_init_node_rt(pipeline, vals[0]);

    return static_cast<int>(RuntimeReconfigErrorCode::SUCCESS);
  }

  // ---------- End RuntimeInterface ----------

 protected:
  using header_field_pair = Context::header_field_pair;
  using ForceArith = Context::ForceArith;

  const std::set<header_field_pair> &get_required_fields() const {
    return required_fields;
  }

  //! Add a component to this switch. Each switch maintains a map `T` ->
  //! `shared_ptr<T>`, which maps a type (using `typeid`) to a shared pointer to
  //! an object of the same type. The pointer can be retrieved at a later time
  //! by using get_component(). This method should be used for components which
  //! are global to the switch and not specific to a Context of the switch,
  //! otherwise you can use add_cxt_component(). The pointer can be retrieved at
  //! a later time by using get_component().
  template<typename T>
  bool add_component(std::shared_ptr<T> ptr) {
    std::shared_ptr<void> ptr_ = std::static_pointer_cast<void>(ptr);
    const auto &r = components.insert({std::type_index(typeid(T)), ptr_});
    return r.second;
  }

  //! Add a component to a context of the switch. Essentially calls
  //! Context::add_component() for the correct context. This method should be
  //! used for components which are specific to a Context (e.g. you can have one
  //! packet replication engine instance per context) and not global to the
  //! switch, otherwise you can use add_component(). The pointer can be
  //! retrieved at a later time by using get_cxt_component().
  template<typename T>
  bool add_cxt_component(cxt_id_t cxt_id, std::shared_ptr<T> ptr) {
    return contexts.at(cxt_id).add_component<T>(ptr);
  }

  void
  set_lookup_factory(
      const std::shared_ptr<LookupStructureFactory> &new_factory) {
    lookup_factory = new_factory;
  }

  int deserialize(std::istream *in);
  int deserialize_from_file(const std::string &state_dump_path);

 private:
  int init_objects(std::istream *is, device_id_t dev_id,
                   std::shared_ptr<TransportIface> transport);

  void reset_target_state();

  void swap_notify();

  //! Override in your switch implementation; it will be called every time a
  //! packet is received.
  virtual int receive_(port_t port_num, const char *buffer, int len) = 0;

  //! Override in your switch implementation; do all your initialization in this
  //! function (e.g. start processing threads) and call start_and_return() when
  //! you are ready to process packets. See start_and_return() for more
  //! information.
  virtual void start_and_return_() = 0;

  //! You can override this method in your target. It will be called whenever
  //! reset_state() is invoked by the control plane. For example, the
  //! simple_switch target uses this to reset PRE state.
  virtual void reset_target_state_() { }

  //! You can override this method in your target. It will be called at the end
  //! of a config swap operation. At that time, you will be guaranteed that no
  //! Packet instances exist, as long as your target uses the correct methods to
  //! instantiate these objects (bm::SwitchWContexts::new_packet_ptr() and
  //! bm::SwitchWContexts::new_packet()).
  virtual void swap_notify_() { }

 private:
  size_t nb_cxts{};
  // TODO(antonin)
  // Context is not-movable, but is default-constructible, so I can put it in a
  // std::vector
  std::vector<Context> contexts{};

  LookupStructureFactory *get_lookup_factory() const {
    return lookup_factory ? lookup_factory.get() : &default_lookup_factory;
  }

  // internal version of get_config_md5(), which does not acquire config_lock
  std::string get_config_md5_() const;

  // Create an instance of the default lookup factory
  static LookupStructureFactory default_lookup_factory;
  // All Switches will refer to that instance unless explicitly
  // given a factory
  std::shared_ptr<LookupStructureFactory> lookup_factory{nullptr};

  bool enable_swap{false};

  std::unique_ptr<PHVSourceIface> phv_source{nullptr};

  std::unordered_map<std::type_index, std::shared_ptr<void> > components{};

  std::set<header_field_pair> required_fields{};
  ForceArith arith_objects{};

  int thrift_port{};

  device_id_t device_id{};

  // same transport used for all notifications, irrespective of the thread, made
  // possible by multi-threading support in nanomsg
  std::string notifications_addr{};
  std::shared_ptr<TransportIface> notifications_transport{nullptr};

  mutable boost::shared_mutex process_packet_mutex{};

  std::string current_config{"{}"};  // empty JSON config
  bool config_loaded{false};
  mutable std::condition_variable config_loaded_cv{};
  mutable std::mutex config_mutex{};

  std::string event_logger_addr{};
};


//! Convenience subclass of SwitchWContexts for targets with a single
//! Context. This is the base class for the standard simple switch target
//! implementation.
class Switch : public SwitchWContexts {
 public:
  //! See SwitchWContexts::SwitchWContexts()
  explicit Switch(bool enable_swap = false);

  // to avoid C++ name hiding
  using SwitchWContexts::field_exists;
  //! Checks that the given field was defined in the input JSON used to
  //! configure the switch
  bool field_exists(const std::string &header_name,
                    const std::string &field_name) const {
    return field_exists(0, header_name, field_name);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::new_packet_ptr;
  //! Convenience wrapper around SwitchWContexts::new_packet_ptr() for a single
  //! context switch.
  std::unique_ptr<Packet> new_packet_ptr(port_t ingress_port,
                                         packet_id_t id, int ingress_length,
                                         // cpplint false positive
                                         // NOLINTNEXTLINE(whitespace/operators)
                                         PacketBuffer &&buffer);

  // to avoid C++ name hiding
  using SwitchWContexts::new_packet;
  //! Convenience wrapper around SwitchWContexts::new_packet() for a single
  //! context switch.
  Packet new_packet(port_t ingress_port, packet_id_t id, int ingress_length,
                    // cpplint false positive
                    // NOLINTNEXTLINE(whitespace/operators)
                    PacketBuffer &&buffer);

  //! Return a raw, non-owning pointer to Pipeline \p name. This pointer will be
  //! invalidated if a configuration swap is performed by the target. See
  //! switch.h documentation for details. Return a nullptr if there is no
  //! pipeline with this name.
  Pipeline *get_pipeline(const std::string &name) {
    return get_context(0)->get_pipeline(name);
  }

  //! Return a raw, non-owning pointer to Parser \p name. This pointer will be
  //! invalidated if a configuration swap is performed by the target. See
  //! switch.h documentation for details. Return a nullptr if there is no parser
  //! with this name.
  Parser *get_parser(const std::string &name) {
    return get_context(0)->get_parser(name);
  }

  //! Return a raw, non-owning pointer to Deparser \p name. This pointer will be
  //! invalidated if a configuration swap is performed by the target. See
  //! switch.h documentation for details. Return a nullptr if there is no
  //! deparser with this name.
  Deparser *get_deparser(const std::string &name) {
    return get_context(0)->get_deparser(name);
  }

  //! Return a raw, non-owning pointer to the FieldList with id \p
  //! field_list_id. This pointer will be invalidated if a configuration swap is
  //! performed by the target. See switch.h documentation for details.
  FieldList *get_field_list(const p4object_id_t field_list_id) {
    return get_context(0)->get_field_list(field_list_id);
  }

  // Added for testing, other "object types" can be added if needed
  p4object_id_t get_table_id(const std::string &name) {
    return get_context(0)->get_table_id(name);
  }

  p4object_id_t get_action_id(const std::string &table_name,
                              const std::string &action_name) {
    return get_context(0)->get_action_id(table_name, action_name);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_learn_engine;
  //! Obtain a pointer to the LearnEngine for this Switch instance
  LearnEngineIface *get_learn_engine() {
    return get_learn_engine(0);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_ageing_monitor;
  AgeingMonitorIface *get_ageing_monitor() {
    return get_ageing_monitor(0);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_config_options;
  ConfigOptionMap get_config_options() const {
    return get_config_options(0);
  }

  // to avoid C++ name hiding
  using SwitchWContexts::get_error_codes;
  //! Return a copy of the error codes map (a bi-directional map between an
  //! error code's integral value and its name / description) for the switch.
  ErrorCodeMap get_error_codes() const {
    return get_error_codes(0);
  }

  //! Add a component to this Switch. Each Switch maintains a map `T` ->
  //! `shared_ptr<T>`, which maps a type (using `typeid`) to a shared pointer to
  //! an object of the same type. The pointer can be retrieved at a later time
  //! by using get_component().
  template<typename T>
  bool add_component(std::shared_ptr<T> ptr) {
    return add_cxt_component<T>(0, std::move(ptr));
  }

  //! Retrieve the shared pointer to an object of type `T` previously added to
  //! the Switch using add_component().
  template<typename T>
  std::shared_ptr<T> get_component() {
    return get_cxt_component<T>(0);
  }
};

}  // namespace bm

#endif  // BM_BM_SIM_SWITCH_H_
