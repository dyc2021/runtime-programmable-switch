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

//! @file context.h
//!
//! A Context instance is essentially a switch within a Switch instance. That
//! just gave you a headache, right? :) Let's picture a simple example: a
//! hardware P4-programmable switch, with a parser, a match-action pipeline and
//! a deparser. Now let's make the picture a little bit more complex and let's
//! say that our hardware switch can actually be split into 2 parts, each of
//! which can be programmed with a different P4 program. So our switch
//! essentially has parser1, pipeline1 and deparser1 which can be programmed by
//! prog1.p4, and parser2, pipeline2 and deparser2 which can be programmed by
//! prog2.p4. Maybe prog1.p4 only handles IPv4 packets, while prog2.p4 only
//! handles IPv6 packets. This is what the Context class is trying to capture:
//! different entities within the same switch, which can be programmed with
//! their own P4 objects. Each context even has its own learning engine and can
//! have its own packet replication engine, so they are very much independent.
//!
//! We can remark that the same could be achieved by instantiating several
//! 1-context Switch and doing some tweaking. However, we believe that contexts
//! are slightly more general and slightly more convenient to use. They are also
//! totally optional. When creating your target switch class, you can inherit
//! from Switch instead of SwitchWContexts, and your switch will only have one
//! context.
//!
//! IMPORTANT: Context support has not yet been added to the bmv2
//! compiler. While you can already implemet multi-contexts target switches,
//! they will all have to be programmed with the same P4 logic. We are planning
//! to add support soon.

#ifndef BM_BM_SIM_CONTEXT_H_
#define BM_BM_SIM_CONTEXT_H_

#include <atomic>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "P4Objects.h"
#include "action_profile.h"
#include "match_tables.h"
#include "runtime_interface.h"
#include "lookup_structures.h"
#include "device_id.h"
#include "runtime_reconfig_error_codes.h"

namespace bm {

//! Provides safe access to an extern instance for control plane calls.
class ExternSafeAccess {
 public:
  ExternSafeAccess(boost::shared_lock<boost::shared_mutex> &&lock,
                   ExternType *instance)
      : lock(std::move(lock)), instance(instance) { }

  //! Get a pointer to the extern instance itself.
  ExternType *get() const { return instance; }

 private:
  boost::shared_lock<boost::shared_mutex> lock;
  ExternType *instance;
};

//! Implements a switch within a switch.
//!
//! See context.h description for more information.
class Context final {
  friend class SwitchWContexts;

 public:
  using mbr_hdl_t = RuntimeInterface::mbr_hdl_t;
  using grp_hdl_t = RuntimeInterface::grp_hdl_t;

  using MeterErrorCode = RuntimeInterface::MeterErrorCode;
  using RegisterErrorCode = RuntimeInterface::RegisterErrorCode;

  using ErrorCode = RuntimeInterface::ErrorCode;

 public:
  // needs to be default constructible if I want to put it in a std::vector
  Context();

  //! Add a component to this Context. Each Context maintains a map `T` ->
  //! `shared_ptr<T>`, which maps a type (using `typeid`) to a shared pointer to
  //! an object of the same type. The pointer can be retrieved at a later time
  //! by using get_component().
  template<typename T>
  bool add_component(std::shared_ptr<T> ptr) {
    std::shared_ptr<void> ptr_ = std::static_pointer_cast<void>(ptr);
    const auto &r = components.insert({std::type_index(typeid(T)), ptr_});
    return r.second;
  }

  //! Retrieve the shared pointer to an object of type `T` previously added to
  //! the Context using add_component().
  template<typename T>
  std::shared_ptr<T> get_component() {
    const auto &search = components.find(std::type_index(typeid(T)));
    if (search == components.end()) return nullptr;
    return std::static_pointer_cast<T>(search->second);
  }

  // do these methods need any protection?
  // TODO(antonin): should I return shared_ptrs instead of raw_ptrs?

  //! Get a raw, non-owning pointer to the Pipeline object with P4 name \p
  //! name. Return a nullptr if there is no pipeline with this name.
  Pipeline *get_pipeline(const std::string &name) {
    return p4objects->get_pipeline_rt(name);
  }

  //! Get a raw, non-owning pointer to the Parser object with P4 name \p
  //! name. Return a nullptr if there is no parser with this name.
  Parser *get_parser(const std::string &name) {
    return p4objects->get_parser_rt(name);
  }

  //! Get a raw, non-owning pointer to the Deparser object with P4 name \p
  //! name. Return a nullptr if there is no deparser with this name.
  Deparser *get_deparser(const std::string &name) {
    return p4objects->get_deparser_rt(name);
  }

  //! Get a raw, non-owning pointer to the FieldList object with id
  //! \p field_list_id
  FieldList *get_field_list(const p4object_id_t field_list_id) {
    return p4objects->get_field_list(field_list_id);
  }

  //! Obtain a pointer to an extern instance, wrapped inside an ExternSafeAccess
  //! object. The wrapper holds a shared mutex, to make sure that accessing the
  //! extern is safe (in the case where a P4 swap occurs).
  ExternSafeAccess get_extern_instance(const std::string &name);

  // Added for testing, other "object types" can be added if needed
  p4object_id_t get_table_id(const std::string &name) {
    return p4objects->get_match_action_table(name)->get_id();
  }

  p4object_id_t get_action_id(const std::string &table_name,
                              const std::string &action_name) {
    return p4objects->get_action(table_name, action_name)->get_id();
  }

  // this should only be used for tests
  P4Objects* get_p4objects_rt() {
    return p4objects_rt.get();
  }

  // this should only be used for tests
  P4Objects* get_p4objects_new() {
    return p4objects_new.get();
  }

 private:
  // ---------- runtime interfaces ----------

  MatchErrorCode
  mt_get_num_entries(const std::string &table_name, size_t *num_entries) const;

  MatchErrorCode
  mt_clear_entries(const std::string &table_name, bool reset_default_entry);

  MatchErrorCode
  mt_add_entry(const std::string &table_name,
               const std::vector<MatchKeyParam> &match_key,
               const std::string &action_name,
               ActionData action_data,
               entry_handle_t *handle,
               int priority = -1  /*only used for ternary*/);

  RuntimeReconfigErrorCode
  mt_runtime_reconfig(const std::string &json_file,
                      const std::string &plan_file,
                      LookupStructureFactory *lookup_factory,
                      const std::set<P4Objects::header_field_pair> &required_fields,
                      const P4Objects::ForceArith &arith_objects);

  // This function aims to:
  // 1. To be called by mt_runtime_reconfig
  // 2. To be used in tests for the convenience of getting commands directly
  RuntimeReconfigErrorCode
  mt_runtime_reconfig_with_stream(std::istream* json_file_stream,
                                  std::istream* plan_file_stream,
                                  LookupStructureFactory *lookup_factory,
                                  const std::set<P4Objects::header_field_pair> &required_fields,
                                  const P4Objects::ForceArith &arith_objects);

  MatchErrorCode
  mt_set_default_action(const std::string &table_name,
                        const std::string &action_name,
                        ActionData action_data);

  MatchErrorCode
  mt_reset_default_entry(const std::string &table_name);

  MatchErrorCode
  mt_delete_entry(const std::string &table_name,
                  entry_handle_t handle);

  MatchErrorCode
  mt_modify_entry(const std::string &table_name,
                  entry_handle_t handle,
                  const std::string &action_name,
                  ActionData action_data);

  MatchErrorCode
  mt_set_entry_ttl(const std::string &table_name,
                   entry_handle_t handle,
                   unsigned int ttl_ms);

  // action profiles

  MatchErrorCode
  mt_act_prof_add_member(const std::string &act_prof_name,
                         const std::string &action_name,
                         ActionData action_data, mbr_hdl_t *mbr);

  MatchErrorCode
  mt_act_prof_delete_member(const std::string &act_prof_name, mbr_hdl_t mbr);

  MatchErrorCode
  mt_act_prof_modify_member(const std::string &act_prof_name, mbr_hdl_t mbr,
                            const std::string &action_name,
                            ActionData action_data);

  MatchErrorCode
  mt_act_prof_create_group(const std::string &act_prof_name, grp_hdl_t *grp);

  MatchErrorCode
  mt_act_prof_delete_group(const std::string &act_prof_name, grp_hdl_t grp);

  MatchErrorCode
  mt_act_prof_add_member_to_group(const std::string &act_prof_name,
                                  mbr_hdl_t mbr, grp_hdl_t grp);

  MatchErrorCode
  mt_act_prof_remove_member_from_group(const std::string &act_prof_name,
                                       mbr_hdl_t mbr, grp_hdl_t grp);

  std::vector<ActionProfile::Member>
  mt_act_prof_get_members(const std::string &act_prof_name) const;

  MatchErrorCode
  mt_act_prof_get_member(const std::string &act_prof_name, grp_hdl_t grp,
                         ActionProfile::Member *member) const;

  std::vector<ActionProfile::Group>
  mt_act_prof_get_groups(const std::string &act_prof_name) const;

  MatchErrorCode
  mt_act_prof_get_group(const std::string &act_prof_name, grp_hdl_t grp,
                        ActionProfile::Group *group) const;

  // indirect tables

  MatchErrorCode
  mt_indirect_add_entry(const std::string &table_name,
                        const std::vector<MatchKeyParam> &match_key,
                        mbr_hdl_t mbr,
                        entry_handle_t *handle,
                        int priority = 1);

  MatchErrorCode
  mt_indirect_modify_entry(const std::string &table_name,
                           entry_handle_t handle,
                           mbr_hdl_t mbr);

  MatchErrorCode
  mt_indirect_delete_entry(const std::string &table_name,
                           entry_handle_t handle);

  MatchErrorCode
  mt_indirect_set_entry_ttl(const std::string &table_name,
                            entry_handle_t handle,
                            unsigned int ttl_ms);

  MatchErrorCode
  mt_indirect_set_default_member(const std::string &table_name,
                                 mbr_hdl_t mbr);

  MatchErrorCode
  mt_indirect_reset_default_entry(const std::string &table_name);

  MatchErrorCode
  mt_indirect_ws_add_entry(const std::string &table_name,
                           const std::vector<MatchKeyParam> &match_key,
                           grp_hdl_t grp,
                           entry_handle_t *handle,
                           int priority = 1);

  MatchErrorCode
  mt_indirect_ws_modify_entry(const std::string &table_name,
                              entry_handle_t handle,
                              grp_hdl_t grp);

  MatchErrorCode
  mt_indirect_ws_set_default_group(const std::string &table_name,
                                   grp_hdl_t grp);

  MatchTableType
  mt_get_type(const std::string &table_name) const;

  template <typename T>
  std::vector<typename T::Entry>
  mt_get_entries(const std::string &table_name) const;

  template <typename T>
  MatchErrorCode
  mt_get_entry(const std::string &table_name, entry_handle_t handle,
               typename T::Entry *entry) const;

  template <typename T>
  MatchErrorCode
  mt_get_default_entry(const std::string &table_name,
                       typename T::Entry *default_entry) const;

  template <typename T>
  MatchErrorCode
  mt_get_entry_from_key(const std::string &table_name,
                        const std::vector<MatchKeyParam> &match_key,
                        typename T::Entry *entry,
                        int priority = 1) const;

  MatchErrorCode
  mt_read_counters(const std::string &table_name,
                   entry_handle_t handle,
                   MatchTableAbstract::counter_value_t *bytes,
                   MatchTableAbstract::counter_value_t *packets);

  MatchErrorCode
  mt_reset_counters(const std::string &table_name);

  MatchErrorCode
  mt_write_counters(const std::string &table_name,
                    entry_handle_t handle,
                    MatchTableAbstract::counter_value_t bytes,
                    MatchTableAbstract::counter_value_t packets);

  MatchErrorCode
  mt_set_meter_rates(const std::string &table_name, entry_handle_t handle,
                     const std::vector<Meter::rate_config_t> &configs);

  MatchErrorCode
  mt_get_meter_rates(const std::string &table_name, entry_handle_t handle,
                     std::vector<Meter::rate_config_t> *configs);

  MatchErrorCode
  mt_reset_meter_rates(const std::string &table_name, entry_handle_t handle);

  Counter::CounterErrorCode
  read_counters(const std::string &counter_name,
                size_t index,
                MatchTableAbstract::counter_value_t *bytes,
                MatchTableAbstract::counter_value_t *packets);

  Counter::CounterErrorCode
  reset_counters(const std::string &counter_name);

  Counter::CounterErrorCode
  write_counters(const std::string &counter_name,
                 size_t index,
                 MatchTableAbstract::counter_value_t bytes,
                 MatchTableAbstract::counter_value_t packets);

  MeterErrorCode
  meter_array_set_rates(
      const std::string &meter_name,
      const std::vector<Meter::rate_config_t> &configs);

  MeterErrorCode
  meter_set_rates(const std::string &meter_name, size_t idx,
                  const std::vector<Meter::rate_config_t> &configs);

  MeterErrorCode
  meter_get_rates(const std::string &meter_name, size_t idx,
                  std::vector<Meter::rate_config_t> *configs);

  MeterErrorCode
  meter_reset_rates(const std::string &meter_name, size_t idx);

  RegisterErrorCode
  register_read(const std::string &register_name,
                const size_t idx, Data *value);

  std::vector<Data>
  register_read_all(const std::string &register_name);

  RegisterErrorCode
  register_write(const std::string &register_name,
                 const size_t idx, Data value);

  RegisterErrorCode
  register_write_range(const std::string &register_name,
                       const size_t start, const size_t end, Data value);

  RegisterErrorCode
  register_reset(const std::string &register_name);

  ParseVSet::ErrorCode
  parse_vset_add(const std::string &parse_vset_name,
                 const ByteContainer &value);

  ParseVSet::ErrorCode
  parse_vset_remove(const std::string &parse_vset_name,
                    const ByteContainer &value);

  ParseVSet::ErrorCode
  parse_vset_get(const std::string &parse_vset_name,
                 std::vector<ByteContainer> *values);

  ParseVSet::ErrorCode
  parse_vset_clear(const std::string &parse_vset_name);

  P4Objects::IdLookupErrorCode p4objects_id_from_name(
      P4Objects::ResourceType type, const std::string &name,
      p4object_id_t *id) const;

  template <typename T>
  CustomCrcErrorCode
  set_crc_custom_parameters(
      const std::string &calc_name,
      const typename CustomCrcMgr<T>::crc_config_t &crc_config);

  bool set_group_selector(
      const std::string &act_prof_name,
      std::shared_ptr<ActionProfile::GroupSelectionIface> selector);

  // ---------- End runtime interfaces ----------

  using ReadLock = std::unique_lock<std::mutex>;
  using WriteLock = std::unique_lock<std::mutex>;

  Context(const Context &other) = delete;
  Context &operator=(const Context &other) = delete;

 private:
  enum SwapStatus {
    NEW_CONFIG_LOADED = 0,
    SWAP_REQUESTED = 1,
    SWAP_COMPLETED = 2,
    SWAP_CANCELLED = 3
  };

  MatchErrorCode get_mt_indirect(const std::string &table_name,
                                 MatchTableIndirect **table) const;
  MatchErrorCode get_mt_indirect_ws(const std::string &table_name,
                                    MatchTableIndirectWS **table) const;

  bool field_exists(const std::string &header_name,
                    const std::string &field_name) const {
    return p4objects->field_exists(header_name, field_name);
  }

  PHVFactory &get_phv_factory();

  LearnEngineIface *get_learn_engine();

  AgeingMonitorIface *get_ageing_monitor();

  void set_notifications_transport(std::shared_ptr<TransportIface> transport);

  void set_device_id(device_id_t device_id);

  void set_cxt_id(cxt_id_t cxt_id);

  void set_force_arith(bool force_arith);

  using header_field_pair = P4Objects::header_field_pair;
  using ForceArith = P4Objects::ForceArith;
  int init_objects(std::istream *is,
                   LookupStructureFactory * lookup_factory,
                   const std::set<header_field_pair> &required_fields =
                     std::set<header_field_pair>(),
                   const ForceArith &arith_objects = ForceArith());

  ErrorCode load_new_config(
      std::istream *is,
      LookupStructureFactory * lookup_factory,
      const std::set<header_field_pair> &required_fields =
        std::set<header_field_pair>(),
      const ForceArith &arith_objects = ForceArith());

  ErrorCode swap_configs();

  ErrorCode reset_state();

  ErrorCode serialize(std::ostream *out);
  ErrorCode deserialize(std::istream *in);

  int do_swap();

  int swap_requested() { return swap_ordered; }

  //! Return string-to-string map of the target-specific options included in the
  //! input config JSON for this context.
  ConfigOptionMap get_config_options() const;

  //! Return a copy of the error codes map (a bi-directional map between an
  //! error code's integral value and its name / description).
  ErrorCodeMap get_error_codes() const;

  void send_swap_status_notification(SwapStatus status);

  void print_runtime_cfg(std::ostream& os) {
    p4objects_rt->print_cfg(os);
  }

 private:  // data members
  cxt_id_t cxt_id{};

  device_id_t device_id{0};

  std::shared_ptr<P4Objects> p4objects{nullptr};
  std::shared_ptr<P4Objects> p4objects_rt{nullptr};
  std::shared_ptr<P4Objects> p4objects_new{nullptr};

  std::unordered_map<std::type_index, std::shared_ptr<void> > components{};

  std::shared_ptr<TransportIface> notifications_transport{nullptr};

  mutable boost::shared_mutex request_mutex{};

  std::atomic<bool> swap_ordered{false};

  bool force_arith{false};
};

}  // namespace bm

#endif  // BM_BM_SIM_CONTEXT_H_
