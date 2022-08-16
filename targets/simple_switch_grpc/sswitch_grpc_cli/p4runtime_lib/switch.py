# Copyright 2017-present Open Networking Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
from abc import abstractmethod
from datetime import datetime
from queue import Queue

import grpc
from p4.tmp import p4config_pb2
from p4.v1 import p4runtime_pb2, p4runtime_pb2_grpc
from error_utils import P4RuntimeReconfigError

MSG_LOG_MAX_LEN = 1024

# List of all active connections
connections = []

class RuntimeReconfigCommandParser:
    def __init__(self, cmd: str):
        self.cmd = cmd
        self.action = ""
        self.action_target = ""
        self.action_arguments = []
        self._parse_runtime_reconfig_command()

    def _parse_runtime_reconfig_command(self):
        cmd_entries = self.cmd.split()
        self.action = cmd_entries[0]
        valid_actions  = ["insert", "change", "delete", "trigger", "init_p4objects_new"]
        if not self.action in valid_actions:
            raise P4RuntimeReconfigError("Invalid Command: can't recognize action {},"
                                         "your action should be in {}".format(self.action, valid_actions))

        self.action_target = cmd_entries[1] if self.action != "trigger" or self.action != "init_p4objects_new" else ""
        if self.action != "trigger" or self.action != "init_p4objects_new":
            valid_action_targets_for_insert = ["tabl", "cond", "flex", "register_array"]
            valid_action_targets_for_change = ["tabl", "cond", "flex", "register_array_size", "register_array_bitwidth", "init"]
            valid_action_targets_for_delete = ["tabl", "cond", "flex", "register_array"]
            if self.action == "insert" and not self.action_target in valid_action_targets_for_insert:
                raise P4RuntimeReconfigError("Invalid command: can't recognize action target for insert: {},"
                                             "your action target should be in {}".format(self.action_target, valid_action_targets_for_insert))
            elif self.action == "change" and not self.action_target in valid_action_targets_for_change:
                raise P4RuntimeReconfigError("Invalid command: can't recognize action target for change: {},"
                                             "your action target should be in {}".format(self.action_target, valid_action_targets_for_change))
            elif self.action == "delete" and not self.action_target in valid_action_targets_for_delete:
                raise P4RuntimeReconfigError("Invalid command: can't recognize action target for delete: {},"
                                             "your action target should be in {}".format(self.action_target, valid_action_targets_for_delete))

        if self.action == "trigger" or self.action == "init_p4objects_new":
            if len(cmd_entries) != 2:
                raise P4RuntimeReconfigError("Invalid command: `trigger` or `init_p4objects_new` command requires 1 argument")
            self.action_arguments = cmd_entries[1]
        else:
            if self.action == "insert":
                if self.action_target == "tabl" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `insert tabl` should have 2 arguments")
                elif self.action_target == "cond" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `insert cond` should have 2 arguments")
                elif self.action_target == "flex" and len(cmd_entries) != 6:
                    raise P4RuntimeReconfigError("Invalid command: `insert flex` should have 4 arguments")
                elif self.action_target == "register_array" and len(cmd_entries) != 5:
                    raise P4RuntimeReconfigError("Invalid command: `insert register_array` should have 3 arguments")
            
            elif self.action == "change":
                if self.action_target == "tabl" and len(cmd_entries) != 6:
                    raise P4RuntimeReconfigError("Invalid command: `change tabl` should have 4 arguments")
                elif self.action_target == "cond" and len(cmd_entries) != 6:
                    raise P4RuntimeReconfigError("Invalid command: `change cond` should have 4 arguments")
                elif self.action_target == "flex" and len(cmd_entries) != 6:
                    raise P4RuntimeReconfigError("Invalid command: `change flex` should have 4 arguments")
                elif self.action_target == "register_array_size" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `change register_array_size` should have 2 arguments")
                elif self.action_target == "register_array_bitwidth" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `change register_array_bitwidth` should have 2 arguments")

            elif self.action == "delete":
                if self.action_target == "tabl" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `delete tabl` should have 2 arguments")
                elif self.action_target == "cond" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `delete cond` should have 2 arguments")
                elif self.action_target == "flex" and len(cmd_entries) != 4:
                    raise P4RuntimeReconfigError("Invalid command: `delete flex` should have 2 arguments")
                elif self.action_target == "register_array" and len(cmd_entries) != 3:
                    raise P4RuntimeReconfigError("Invalid command: `delete register_array` should have 1 argument")
                                
            self.action_arguments = cmd_entries[2:]

def ShutdownAllSwitchConnections():
    for c in connections:
        c.shutdown()

class SwitchConnection(object):
    def __init__(self, name=None, address='127.0.0.1:50051', device_id=0,
                 proto_dump_file=None):
        self.name = name
        self.address = address
        self.device_id = device_id
        self.p4info = None
        self.channel = grpc.insecure_channel(self.address)
        self.proto_dump_file = proto_dump_file
        self.already_init_p4objects_new = False
        if proto_dump_file is not None:
            interceptor = GrpcRequestLogger(proto_dump_file)
            self.channel = grpc.intercept_channel(self.channel, interceptor)
        self.client_stub = p4runtime_pb2_grpc.P4RuntimeStub(self.channel)
        self.requests_stream = IterableQueue()
        self.stream_msg_resp = self.client_stub.StreamChannel(iter(self.requests_stream))
        self.proto_dump_file = proto_dump_file
        connections.append(self)

    @abstractmethod
    def buildDeviceConfig(self, **kwargs):
        return p4config_pb2.P4DeviceConfig()

    def shutdown(self):
        self.requests_stream.close()
        self.stream_msg_resp.cancel()

    def MasterArbitrationUpdate(self, dry_run=False, **kwargs):
        request = p4runtime_pb2.StreamMessageRequest()
        request.arbitration.device_id = self.device_id
        request.arbitration.election_id.high = 0
        request.arbitration.election_id.low = 1

        if dry_run:
            print("P4Runtime MasterArbitrationUpdate: ", request)
        else:
            self.requests_stream.put(request)
            for item in self.stream_msg_resp:
                return item # just one

    def SetForwardingPipelineConfig(self, p4info, dry_run=False, **kwargs):
        device_config = self.buildDeviceConfig(**kwargs)
        request = p4runtime_pb2.SetForwardingPipelineConfigRequest()
        request.election_id.low = 1
        request.device_id = self.device_id
        config = request.config

        config.p4info.CopyFrom(p4info)
        config.p4_device_config = device_config.SerializeToString()

        request.action = p4runtime_pb2.SetForwardingPipelineConfigRequest.VERIFY_AND_COMMIT
        if dry_run:
            print("P4Runtime SetForwardingPipelineConfig:", request)
        else:
            self.client_stub.SetForwardingPipelineConfig(request)

    def WriteTableEntry(self, table_entry, dry_run=False):
        request = p4runtime_pb2.WriteRequest()
        request.device_id = self.device_id
        request.election_id.low = 1
        update = request.updates.add()
        if table_entry.is_default_action:
            update.type = p4runtime_pb2.Update.MODIFY
        else:
            update.type = p4runtime_pb2.Update.INSERT
        update.entity.table_entry.CopyFrom(table_entry)
        if dry_run:
            print("P4Runtime Write:", request)
        else:
            self.client_stub.Write(request)

    def ReadTableEntries(self, table_id=None, dry_run=False):
        request = p4runtime_pb2.ReadRequest()
        request.device_id = self.device_id
        entity = request.entities.add()
        table_entry = entity.table_entry
        if table_id is not None:
            table_entry.table_id = table_id
        else:
            table_entry.table_id = 0
        if dry_run:
            print("P4Runtime Read:", request)
        else:
            for response in self.client_stub.Read(request):
                yield response

    def ReadCounters(self, counter_id=None, index=None, dry_run=False):
        request = p4runtime_pb2.ReadRequest()
        request.device_id = self.device_id
        entity = request.entities.add()
        counter_entry = entity.counter_entry
        if counter_id is not None:
            counter_entry.counter_id = counter_id
        else:
            counter_entry.counter_id = 0
        if index is not None:
            counter_entry.index.index = index
        if dry_run:
            print("P4Runtime Read:", request)
        else:
            for response in self.client_stub.Read(request):
                yield response


    def WritePREEntry(self, pre_entry, dry_run=False):
        request = p4runtime_pb2.WriteRequest()
        request.device_id = self.device_id
        request.election_id.low = 1
        update = request.updates.add()
        update.type = p4runtime_pb2.Update.INSERT
        update.entity.packet_replication_engine_entry.CopyFrom(pre_entry)
        if dry_run:
            print("P4Runtime Write:", request)
        else:
            self.client_stub.Write(request)

    def RuntimeReconfig(self, parsed_cmd: RuntimeReconfigCommandParser, dry_run=False):
        # We assume that parser has already verified cmd's action, target and the number of arguments

        request = p4runtime_pb2.WriteRequest()
        request.device_id = self.device_id
        request.election_id.low = 1
        update = request.updates.add()
        update.type = p4runtime_pb2.Update.RUNTIME_RECONFIG

        runtime_reconfig_entry = update.entity.runtime_reconfig_entry
        runtime_reconfig_type = runtime_reconfig_entry.runtime_reconfig_type
        runtime_reconfig_content = runtime_reconfig_entry.runtime_reconfig_content

        if parsed_cmd.action != "init_p4objects_new" and not self.already_init_p4objects_new:
            raise P4RuntimeReconfigError("p4objects_new has not been initialized for this switch, you should init it before doing runtime reconfig")

        if parsed_cmd.action == "init_p4objects_new":
            runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.INIT_P4OBJECTS_NEW
            p4objects_new_json_path = parsed_cmd.action_arguments[0]
            with open(p4objects_new_json_path, "r") as f:
                init_p4objects_new_entry = p4runtime_pb2.InitP4ObjectsNewEntry()
                init_p4objects_new_entry.p4objects_new_json = f.read().encode('utf-8')
                runtime_reconfig_content.init_p4objects_new_entry = \
                                                    init_p4objects_new_entry.SerializeToString()

        elif parsed_cmd.action == "insert":
            if parsed_cmd.action_target == "tabl":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.INSERT_TABLE
                runtime_reconfig_content.insert_table_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.insert_table_entry.table_name = parsed_cmd.action_arguments[1]
            elif parsed_cmd.action_target == "cond":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.INSERT_CONDITIONAL
                runtime_reconfig_content.insert_conditional_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.insert_conditonal_entry.branch_name = parsed_cmd.action_arguments[1]
            elif parsed_cmd.action_target == "flex":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.INSERT_FLEX
                runtime_reconfig_content.insert_flex_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.insert_flex_entry.node_name = parsed_cmd.action_arguments[1]
                runtime_reconfig_content.insert_flex_entry.true_next_node = parsed_cmd.action_arguments[2]
                runtime_reconfig_content.insert_flex_entry.false_next_node = parsed_cmd.action_arguments[3]
            elif parsed_cmd.action_target == "register_array":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.INSERT_REGISTER_ARRAY
                runtime_reconfig_content.insert_register_array_entry.register_array_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.insert_register_array_entry.register_array_size = parsed_cmd.action_arguments[1]
                runtime_reconfig_content.insert_register_array_entry.register_array_bitwidth = parsed_cmd.action_arguments[2]
        
        elif parsed_cmd.action == "change":
            if parsed_cmd.action_target == "tabl":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.CHANGE_TABLE
                runtime_reconfig_content.change_table_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.change_table_entry.table_name = parsed_cmd.action_arguments[1]
                runtime_reconfig_content.change_table_entry.edge_name = parsed_cmd.action_arguments[2]
                runtime_reconfig_content.change_table_entry.table_name_next = parsed_cmd.action_arguments[3]
            elif parsed_cmd.action_target == "cond":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.CHANGE_CONDITIONAL
                runtime_reconfig_content.change_conditional_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.change_conditional_entry.branch_name = parsed_cmd.action_arguments[1]
                runtime_reconfig_content.change_conditional_entry.true_or_false_next = True if parsed_cmd.action_arguments[2] == "true_next" else False
                runtime_reconfig_content.change_conditional_entry.node_name = parsed_cmd.action_arguments[3]
            elif parsed_cmd.action_target == "flex":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.CHANGE_FLEX
                runtime_reconfig_content.change_flex_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.change_flex_entry.flx_name = parsed_cmd.action_arguments[1]
                runtime_reconfig_content.change_flex_entry.true_or_false_next = True if parsed_cmd.action_arguments[2] == "true_next" else False
                runtime_reconfig_content.change_flex_entry.node_next = parsed_cmd.action_arguments[3]
            elif "register_array" in parsed_cmd.action_target:
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.CHANGE_REGISTER_ARRAY
                runtime_reconfig_content.change_register_array_entry.register_array_name = parsed_cmd.action_arguments[0]
                if parsed_cmd.action_target == "register_array_size":
                    runtime_reconfig_content.change_register_array_entry.register_array_change_type = p4runtime_pb2.ChangeRegisterArrayEntry.CHANGE_SIZE
                elif parsed_cmd.action_target == "register_array_bitwidth":
                    runtime_reconfig_content.change_register_array_entry.register_array_change_type = p4runtime_pb2.ChangeRegisterArrayEntry.CHANGE_BITWIDTH
                runtime_reconfig_content.change_register_array_entry.new_value = int(parsed_cmd.action_arguments[1])
            elif parsed_cmd.action_target == "init":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.CHANGE_INIT
                runtime_reconfig_content.change_init_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.change_init_entry.table_name_next = parsed_cmd.action_arguments[1]
        
        elif parsed_cmd.action == "delete":
            if parsed_cmd.action_target == "tabl":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.DELETE_TABLE
                runtime_reconfig_content.delete_table_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.delete_table_entry.table_name = parsed_cmd.action_arguments[1]
            elif parsed_cmd.action_target == "cond":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.DELETE_CONDITIONAL
                runtime_reconfig_content.delete_conditional_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.delete_conditional_entry.branch_name = parsed_cmd.action_arguments[1]
            elif parsed_cmd.action_target == "flex":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.DELETE_FLEX
                runtime_reconfig_content.delete_flex_entry.pipeline_name = parsed_cmd.action_arguments[0]
                runtime_reconfig_content.delete_flex_entry.flx_name = parsed_cmd.action_arguments[1]
            elif parsed_cmd.action_target == "register_array":
                runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.DELETE_REGISTER_ARRAY
                runtime_reconfig_content.delete_register_array_entry.register_array_name = parsed_cmd.action_arguments[0]
        
        elif parsed_cmd.action == "trigger":
            runtime_reconfig_type = p4runtime_pb2.RuntimeReconfigEntry.TRIGGER
            runtime_reconfig_content.trigger_entry.on_or_off = True if parsed_cmd.action_arguments[0] == "on" else False

        else:
            raise P4RuntimeReconfigError("Can't parse the command, this shouldn't happen, please check RuntimeReconfigCommandParser")

        if dry_run:
            print("P4Runtime Reconfig Request: ", request)
        else:
            print("Send P4Runtime Reconfig Request:", request)
            self.client_stub.Write(request)
            if parsed_cmd.action == "init_p4objects_new":
                self.already_init_p4objects_new = True

class GrpcRequestLogger(grpc.UnaryUnaryClientInterceptor,
                        grpc.UnaryStreamClientInterceptor):
    """Implementation of a gRPC interceptor that logs request to a file"""

    def __init__(self, log_file):
        self.log_file = log_file
        with open(self.log_file, 'w') as f:
            # Clear content if it exists.
            f.write("")

    def log_message(self, method_name, body):
        with open(self.log_file, 'a') as f:
            ts = datetime.utcnow().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
            msg = str(body)
            f.write("\n[%s] %s\n---\n" % (ts, method_name))
            if len(msg) < MSG_LOG_MAX_LEN:
                f.write(str(body))
            else:
                f.write("Message too long (%d bytes)! Skipping log...\n" % len(msg))
            f.write('---\n')

    def intercept_unary_unary(self, continuation, client_call_details, request):
        self.log_message(client_call_details.method, request)
        return continuation(client_call_details, request)

    def intercept_unary_stream(self, continuation, client_call_details, request):
        self.log_message(client_call_details.method, request)
        return continuation(client_call_details, request)

class IterableQueue(Queue):
    _sentinel = object()

    def __iter__(self):
        return iter(self.get, self._sentinel)

    def close(self):
        self.put(self._sentinel)
