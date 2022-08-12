#include <bm/bm_sim/_assert.h>
#include <bm/bm_sim/logger.h>
#include <bm/bm_sim/switch.h>

#include <PI/pi.h>
#include <PI/target/pi_runtime_reconfig_imp.h>

#include "common.h"

namespace {

pi_status_t convert_error_code(int error_code) {
    return static_cast<pi_status_t>(PI_STATUS_TARGET_ERROR + error_code);
}

}

extern "C" {

pi_status_t _pi_runtime_reconfig_init_p4objects_new(pi_session_handle_t session_handle,
                                                   pi_dev_tgt_t dev_tgt, 
                                                   const char* p4objects_new_json) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_init_p4objects_new(0, p4objects_new_json);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_insert_table(pi_session_handle_t session_handle,
                                             pi_dev_tgt_t dev_tgt,
                                             const char* pipeline_name,
                                             const char* table_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_insert_table(0, pipeline_name, table_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_change_table(pi_session_handle_t session_handle,
                                             pi_dev_tgt_t dev_tgt,
                                             const char* pipeline_name,
                                             const char* table_name,
                                             const char* edge_name,
                                             const char* table_name_next) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_change_table(0, pipeline_name, table_name,
                                                                        edge_name, table_name_next);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_delete_table(pi_session_handle_t session_handle,
                                             pi_dev_tgt_t dev_tgt,
                                             const char* pipeline_name,
                                             const char* table_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_delete_table(0, pipeline_name, table_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_insert_conditional(pi_session_handle_t session_handle,
                                                   pi_dev_tgt_t dev_tgt,
                                                   const char* pipeline_name,
                                                   const char* branch_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_insert_conditional(0, pipeline_name, branch_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_change_conditional(pi_session_handle_t session_handle,
                                                   pi_dev_tgt_t dev_tgt,
                                                   const char* pipeline_name,
                                                   const char* branch_name,
                                                   bool true_or_false_next,
                                                   const char* node_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_change_conditional(0, pipeline_name, branch_name,
                                                                              true_or_false_next, node_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_delete_conditional(pi_session_handle_t session_handle,
                                                   pi_dev_tgt_t dev_tgt,
                                                   const char* pipeline_name,
                                                   const char* branch_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_delete_conditional(0, pipeline_name, branch_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_insert_flex(pi_session_handle_t session_handle,
                                            pi_dev_tgt_t dev_tgt,
                                            const char* pipeline_name,
                                            const char* node_name,
                                            const char* true_next_node,
                                            const char* false_next_node) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_insert_flex(0, pipeline_name, node_name,
                                                                       true_next_node, false_next_node);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_change_flex(pi_session_handle_t session_handle,
                                            pi_dev_tgt_t dev_tgt,
                                            const char* pipeline_name,
                                            const char* flx_name,
                                            bool true_or_false_next,
                                            const char* node_next) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_change_flex(0, pipeline_name, flx_name,
                                                                       true_or_false_next, node_next);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_delete_flex(pi_session_handle_t session_handle,
                                            pi_dev_tgt_t dev_tgt,
                                            const char* pipeline_name,
                                            const char* flx_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_delete_flex(0, pipeline_name, flx_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_insert_register_array(pi_session_handle_t session_handle,
                                                      pi_dev_tgt_t dev_tgt,
                                                      const char* register_array_name,
                                                      const uint32_t register_array_size,
                                                      const uint32_t register_array_bitwidth) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_insert_register_array(0, register_array_name, 
                                                                                register_array_size, register_array_bitwidth);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_change_register_array(pi_session_handle_t session_handle,
                                                      pi_dev_tgt_t dev_tgt,
                                                      const char* register_array_name,
                                                      const uint32_t change_type,
                                                      const uint32_t new_value) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_change_register_array(0, register_array_name, 
                                                                                change_type, new_value);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_delete_register_array(pi_session_handle_t session_handle,
                                                      pi_dev_tgt_t dev_tgt,
                                                      const char* register_array_name) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_delete_register_array(0, register_array_name);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_trigger(pi_session_handle_t session_handle,
                                        pi_dev_tgt_t dev_tgt,
                                        bool on_or_off) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_trigger(0, on_or_off);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

pi_status_t _pi_runtime_reconfig_change_init(pi_session_handle_t session_handle,
                                            pi_dev_tgt_t dev_tgt,
                                            const char* pipeline_name,
                                            const char* table_name_next) {
    _BM_UNUSED(session_handle);

    const auto *p4info = pibmv2::get_device_info(dev_tgt.dev_id);
    assert(p4info != nullptr);

    auto error_code = pibmv2::switch_->mt_runtime_reconfig_change_init(0, pipeline_name,
                                                                       table_name_next);
    if (error_code != 0)
        return convert_error_code(error_code);
    
    return PI_STATUS_SUCCESS;
}

}