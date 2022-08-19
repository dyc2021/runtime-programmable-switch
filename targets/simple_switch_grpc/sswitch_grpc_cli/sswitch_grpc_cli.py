#!/usr/bin/env python3
import os
import sys
from typing import Dict, List
import traceback
import logging
import time

import grpc

import p4runtime_lib.bmv2
import p4runtime_lib.helper
import p4runtime_lib
from p4runtime_lib.switch import RuntimeReconfigCommandParser, ShutdownAllSwitchConnections
from p4runtime_lib.error_utils import P4RuntimeReconfigError, P4RuntimeReconfigWarning
from p4runtime_lib import runtime_reconfig_tools

DETAILED_HELP_MESSAGE = \
"=" * 100 + \
"""
This is a CLI for simple_switch_grpc
You could use it to do runtime reconfiguration for multiple switches in the network

Please follow the steps below to get familiar with it:

::STEP1::
At beginning, this CLI doesn't connect to any switch. Therefore, you should choose a switch to connect at first
For instance,
    simple_switch_grpc_cli> connect <switch_name_given_by_yourself> <switch_address> <device_id>
    <switch_name_given_by_yourself> can be: s1
    <switch_address> can be: 127.0.0.1:50051
    <device_id> can be: 0
    You can connect to a switch named s1 at 127.0.0.1:50051, whose device id is 0
After this,
    You can see the command line prefix becomes: (s1) simple_switch_grpc_cli> 
    This means you are now communicate with s1
    To change to another switch,
        Enter: connect <switch_name_given_by_yourself> 
            to connect to that switch if it has been added previously
        or Enter: connect <switch_name_given_by_yourself> <switch_address> <device_id>
            to connect to a new switch

::STEP2::
Since simple_switch_grpc starts without any p4 configuration, 
you should first use `set_forwarding_pipeline_config` command to init the simple_switch_grpc
For instance,
    (s1) simple_switch_grpc_cli> set_forwarding_pipeline_config <p4info_path> <bmv2_json_path>
    You can initialize s1 with p4info at <p4info_path>, and bmv2 json at <bmv2_json_path>

::STEP3::
Now, you still can't reconfig switch, because the p4objects_new is not initialized in simple_switch_grpc
To load p4objects_new, please use command `init_p4objects_new`
For instance,
    (s1) simple_switch_grpc_cli> init_p4objects_new <bmv2_json_path>
    You can load <bmv2_json_path> to s1's p4objects_new

::STEP4::
You can reconfigurate switch from now on
The reconfiguration commands are the same as what is described in our runtime-programmable-switch repository
Feel free to read the README and runtime_register_reconfig_readme.md
For instance,
    (s1) simple_switch_grpc_cli> insert tabl ingress new_acl
    You can insert a new table acl to s1 (table acl is originally a table in s1's p4objects_new)

::INSTALL_FUNC::
We enable you to install at most 128 self-defined functions at runtime
Command `show_program_graph` assists this process
By invoking this command, you can see a flow diagram of current program installed in this switch
Please use the names in this flow diagram to decide your mount_point (see below)
For instance,
    (s1) simple_switch_grpc_cli> install_func <func_p4_header_file_path> <func_p4_control_block_file_path> <mount_point> <mount_point_number>
    <func_p4_header_file_path> should point to a file containing the headers you need 
    (we assume that your provided headers should be the same as those in this connected switch's program)
    <func_p4_control_block_path> should point to a file containing a single control block
    (NOTE: scalars are also in the headers, so please check your control block doesn't contain any additional scalar)
    (you should obey this assumption when doing runtime reconfiguration; we can't ensure the program will not crash if you try to use different headers)
    <mount_point> should be an edge in program's flow diagram; it should be in the format of <start_node>-><end_node> 
    (for example, table[old_MyIngress.acl]->conditional[old_MyIngress.node_4]); this will install the function between the <start_node> and <end_node>)
    <mount_point_number> is a convenient representation of mount_point which we will use in `uninstall_func` and `migrate_func`
    (mount_point_number should be in range [0, 128), and the same number can't be repeatedly used for installing)

::UNINSTALL_FUNC::
You can uninstall the function previously mounted at a certain mount_point
For instance,
    (s1) simple_switch_grapc_cli> uninstall_func <mount_point_number>

::MIGRATE_FUNC::
We enable you to migrate a certain function in this connected switch to another switch
For instance,
    (s1) simple_switch_grpc_cli> migrate_func <mount_point_number_in_this_switch> <another_switch_name> <mount_point_for_another_switch> <mount_point_number_for_another_switch>
    (please note that this will not uninstall any function in this switch)

::NOTES::
You can use command `list_switches` to see all the switches connected to this CLI
You can use command `q` or `quit` to quit
You can enter `h` or `help` to see a brief help message
To see this detailed help message, please enter `detailed_help`
Please DONT use `set_forwarding_pipeline_config` twice for a single switch, since our reconfiguration changes switch's program
but doesn't update CLI's p4info, which means that your local p4info might be obsolete
""" + \
"=" * 100

HELP_MESSAGE = \
"""
In command line, enter `h` or `help` to see this message
For detailed demonstration, enter `detailed_help`

Commands:
Connect to a new switch: connect <switch_name_given_by_yourself> <switch_address> <device_id>
Change to a switch: connect <switch_name_given_by_yourself>
Init switch: set_forwarding_pipeline_config <p4info_path> <bmv2_json_path>
Init p4objects_new: init_p4objects_new <bmv2_json_path>
Runtime reconfiguration: see the README and runtime_register_reconfig_readme.md in our repository
Show program's flow diagram: show_program_graph
Install function: install_func <func_p4_header_file_path> <func_p4_control_block_file_path> <mount_point> <mount_point_number>
Uninstall function: uninstall_func <mount_point_number>
Migrate function: migrate_func <mount_point_number_in_this_switch> <another_switch_name> <mount_point_for_another_switch> <mount_point_number_for_another_switch>
List connected switches: list_switches
Quit: q or quit
"""

OUTSIDE_HELP_MESSAGE = \
"""
You can start this CLI without any command line argument
Or, by adding tag `--script <script_path>`, you can directly run a script
"""

OUTPUT_FOLDER = "sswitch_graph_cli_output"

DISPLAY_GRAPH_IN_COMMAND_LINE = True

def printGrpcError(e):
    print("gRPC Error:", e.details(), end=' ')
    status_code = e.code()
    print("(%s)" % status_code.name, end=' ')
    traceback = sys.exc_info()[2]
    print("[%s:%d]" % (traceback.tb_frame.f_code.co_filename, traceback.tb_lineno))

class SSwitchGRPCConnection:
    def __init__(self, name: str, address: str, device_id: int, proto_dump_file: str) -> None:
        self.bmv2_connection = p4runtime_lib.bmv2.Bmv2SwitchConnection(name=name, address=address, device_id=device_id, proto_dump_file=proto_dump_file)
        self.program_graph_manager = runtime_reconfig_tools.ProgramGraphManager()
        self.latest_config_json_path: str = None

class SSwitchGRPCCLI:
    def __init__(self) -> None:
        self.connections: Dict[str, SSwitchGRPCConnection] = dict() # { switch_name: SSwitchGRPCConnection }
        self.cur_connection: SSwitchGRPCConnection = None
        self.program_graph_manager = runtime_reconfig_tools.ProgramGraphManager()
        os.makedirs(OUTPUT_FOLDER, exist_ok=True)

    def _read_user_input(self) -> str:
        return input("{}simple_switch_grpc_cli> ".format("" if self.cur_connection is None else "({}) ".format(self.cur_connection.name)))

    def _install_func(self, func_p4_header_file_path: str, func_p4_control_block_file_path: str, mount_point: str, mount_point_number: int):
        merged_json_file_path = runtime_reconfig_tools.merge_and_compile(header_file_path=func_p4_header_file_path,
                                                                        control_block_file_path=func_p4_control_block_file_path,
                                                                        output_path=OUTPUT_FOLDER)
        # inject "flex_name" field
        runtime_reconfig_tools.update_merged_json_file(merged_json_file_path=merged_json_file_path)
        if len(mount_point.split("->")) != 2:
            raise P4RuntimeReconfigError("Parsing mount point fails, mount point is: {}".format(mount_point))
        start_point, end_point = mount_point.split("->")
        start_point = runtime_reconfig_tools.human_readable_name_to_flex_name(start_point)
        end_point = runtime_reconfig_tools.human_readable_name_to_flex_name(end_point)
        install_func_commands = runtime_reconfig_tools.generate_install_func_commands(merged_json_file_path=merged_json_file_path,
                                                                                     start_point=start_point,
                                                                                     end_point=end_point,
                                                                                     mount_point_number=mount_point_number)
        print("install func commands: ")
        for command in install_func_commands:
            print("\t" + command)
        
        for i, command in enumerate(install_func_commands):
            print("Execute command [{}]: {}".format(i, command))
            self._exec_one_command(command=command)
            print("Finish running command [{}]: {}".format(i, command))

    def _uninstall_func(self, mount_point_number: int):
        uninstall_func_commands = runtime_reconfig_tools.generate_uninstall_func_commands(runtime_json_file_path=self.cur_connection.latest_config_json_path,
                                                                                          mount_point_number=mount_point_number)
        print("uninstall func commands: ")
        for command in uninstall_func_commands:
            print("\t" + command)
        
        for i, command in enumerate(uninstall_func_commands):
            print("Execute command [{}]: {}".format(i, command))
            self._exec_one_command(command=command)
            print("Finish running command [{}]: {}".format(i, command))

    def _exec_one_command(self, command: str):
        parsed_command = command.split()
        if len(parsed_command) == 1 and (parsed_command[0] == "h" or parsed_command[0] == "help"):
            print(HELP_MESSAGE)
        elif len(parsed_command) == 1 and parsed_command[0] == "detailed_help":
            print(DETAILED_HELP_MESSAGE)
        elif len(parsed_command) == 1 and parsed_command[0] == "list_switches":
            if len(self.connections) != 0:
                for connection_name, connection in self.connections.items():
                    print("connection [{}]: address: {}, device_id: {}, log_file: {}".format(connection_name, 
                                                                                            connection.bmv2_connection.name, 
                                                                                            connection.bmv2_connection.address, 
                                                                                            connection.bmv2_connection.device_id, 
                                                                                            connection.bmv2_connection.proto_dump_file))
            else:
                raise P4RuntimeReconfigWarning("CLI doesn't connect to any switch")
        elif len(parsed_command) == 1 and parsed_command[0] == "show_program_graph":
            if self.cur_connection is None:
                raise P4RuntimeReconfigWarning("CLI doesn't connect to any switch")
            elif self.cur_connection.program_graph_manager.cur_graph is None:
                raise P4RuntimeReconfigWarning("Connection {} doesn't have any program graph".format(self.cur_connection.bmv2_connection.name))
            else:
                if DISPLAY_GRAPH_IN_COMMAND_LINE:
                    runtime_reconfig_tools.display_graph_in_command_line(self.cur_connection.program_graph_manager.cur_graph)
                else:
                    runtime_reconfig_tools.display_graph_using_matplotlib(self.cur_connection.program_graph_manager.cur_graph)
        elif parsed_command[0] == "connect" and (len(parsed_command) == 2 or len(parsed_command) == 4):
            if len(parsed_command) == 2:
                if parsed_command[1] in self.connections:
                    self.cur_connection = self.connections[parsed_command[1]]
                else:
                    raise P4RuntimeReconfigWarning("Can't find connection whose name is {}".format(parsed_command[1]))
            else:
                print("Connecting ...")
                connection = p4runtime_lib.bmv2.Bmv2SwitchConnection(name=parsed_command[1],
                                                                    address=parsed_command[2],
                                                                    device_id=int(parsed_command[3]),
                                                                    proto_dump_file="{}/{}_p4runtime_requests.txt".format(OUTPUT_FOLDER, parsed_command[1]))
                connection.MasterArbitrationUpdate()
                self.connections[parsed_command[1]] = connection
                self.cur_connection = connection
                print("Connect successfully")
        elif parsed_command[0] == "set_forwarding_pipeline_config" and len(parsed_command) == 3:
            if self.cur_connection is None:
                raise P4RuntimeReconfigWarning("This CLI doesn't connect to any switch now, please use `connect` command before running `set_forwarding_pipeline_config`")
            else:
                if not os.path.exists(parsed_command[1]):
                    raise P4RuntimeReconfigError("p4info file not found: {}".format(parsed_command[1]))
                if not os.path.exists(parsed_command[2]):
                    raise P4RuntimeReconfigError("bmv2 JSON file not found: {}".format(parsed_command[2]))
                # inject "flex_name" field
                runtime_reconfig_tools.update_init_json_file(parsed_command[2])
                print("Installing p4 program on {} ...".format(self.cur_connection.name))
                p4info_helper = p4runtime_lib.helper.P4InfoHelper(parsed_command[1])
                self.cur_connection.bmv2_connection.SetForwardingPipelineConfig(p4info=p4info_helper.p4info,
                                                                                bmv2_json_file_path=parsed_command[2])
                self.cur_connection.latest_config_json_path = parsed_command[2]
                self.cur_connection.program_graph_manager.update_graph(new_config_json_file_path=parsed_command[2])
                print("Install successfully")
        elif parsed_command[0] == "install_func" and len(parsed_command) == 5:
            if self.cur_connection is None:
                raise P4RuntimeReconfigWarning("This CLI doesn't connect to any switch now")
            else:
                if self.cur_connection.latest_config_json_path is None:
                    raise P4RuntimeReconfigWarning("The switch hasn't been initiated please use `set_forwarding_pipeline_config` command")
                if not os.path.exists(parsed_command[1]):
                    raise P4RuntimeReconfigError("p4 header file not found: {}".format(parsed_command[1]))
                if not os.path.exists(parsed_command[2]):
                    raise P4RuntimeReconfigError("Control block file not found: {}".format(parsed_command[2]))
                if int(parsed_command[4]) < 0 or int(parsed_command[4]) >= 128:
                    raise P4RuntimeReconfigError("Mount point number should be in range [0, 128)")
            print("Installing function ...")
            self._install_func(func_p4_header_file_path=parsed_command[1],
                               func_p4_control_block_file_path=parsed_command[2],
                               mount_point=parsed_command[3],
                               mount_point_number=int(parsed_command[4]))
            print("Install successfully")
        elif parsed_command[0] == "uninstall_func" and len(parsed_command) == 2:
            if self.cur_connection is None:
                raise P4RuntimeReconfigWarning("This CLI doesn't connect to any switch now")
            else:
                if self.cur_connection.latest_config_json_path is None:
                    raise P4RuntimeReconfigWarning("The switch hasn't been initiated please use `set_forwarding_pipeline_config` command")
                if int(parsed_command[1]) < 0 or int(parsed_command[1]) >= 128:
                    raise P4RuntimeReconfigError("Mount point number should be in range [0, 128)")
            print("Uninstalling function ...")
            self._uninstall_func(mount_point_number=int(parsed_command[1]))
            print("Uninstall successfully")
        elif parsed_command[0] == "migrate_func" and len(parsed_command) == 5:
            raise P4RuntimeReconfigWarning("We now don't support migrate_func")
        else:
            if self.cur_connection is None:
                raise P4RuntimeReconfigWarning("This CLI doesn't connect to any switch now, please use `connect` command")
            else:
                if self.cur_connection.latest_config_json_path is None:
                    raise P4RuntimeReconfigWarning("The switch hasn't been initiated please use `set_forwarding_pipeline_config` command")
                try:
                    parsed_command = RuntimeReconfigCommandParser(command)
                except P4RuntimeReconfigError:
                    raise P4RuntimeReconfigWarning("Invalid command, please enter again")

                print("Runtime reconfigurating ...")
                response = self.cur_connection.bmv2_connection.RuntimeReconfig(parsed_cmd=parsed_command)
                # we expect the returned json is a string
                returned_json = response.p4objects_json_entry.p4objects_json
                if not isinstance(returned_json, str):
                    raise P4RuntimeReconfigError("Returned json is not a string")
                returned_json_file_path = os.path.join(OUTPUT_FOLDER, "returned_json_{}.json".format(time.time()))
                with open(returned_json_file_path, "w") as returned_json_file:
                    returned_json_file.write(returned_json)
                self.cur_connection.latest_config_json_path = returned_json_file_path
                self.cur_connection.program_graph_manager.update_graph(new_config_json_file_path=returned_json_file_path)
                print("Runtime reconfiguration ends")

    def exec_script(self, commands: List[str]):
        get_error = False
        print("Start running script")
        for i, command in enumerate(commands):
            print("Execute command [{}]: {}".format(i, command))
            try:
                self._exec_one_command(command=command)
            except P4RuntimeReconfigWarning as w:
                print(w)
                get_error = True
                break
            except P4RuntimeReconfigError as e:
                print(e)
                get_error = True
                break
            except grpc.RpcError as e:
                printGrpcError(e)
                get_error = True
                break
            except Exception as e:
                logging.error(traceback.format_exc())
                get_error = True
                break
            except KeyboardInterrupt:
                print("Keyboard interrupt")
                get_error = True
                break
            print("Finish running command [{}]: {}".format(i, command))
            if i == len(commands) - 1:
                print("All commands are executed")
        print("Shutdown all connections")
        ShutdownAllSwitchConnections()
        print("Program exits")
        return 0 if not get_error else 1 

    def exec_command_loop(self):
        get_error = False
        print(DETAILED_HELP_MESSAGE)
        user_input = self._read_user_input()
        while user_input != "q" and user_input != "quit":
            try:
                self._exec_one_command(command=user_input)
            except P4RuntimeReconfigWarning as w:
                print(w)
                # if we get a warning, let user give a input again
                try:
                    user_input = self._read_user_input()
                except Exception as e:
                    logging.error(traceback.format_exc())
                    get_error = True
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    get_error = True
                    break
                continue
            except P4RuntimeReconfigError as e:
                print(e)
                get_error = True
                break
            except grpc.RpcError as e:
                printGrpcError(e)
                get_error = True
                break
            except Exception as e:
                logging.error(traceback.format_exc())
                get_error = True
                break
            except KeyboardInterrupt:
                print("Keyboard interrupt")
                get_error = True
                break

            try:
                user_input = self._read_user_input()
            except Exception as e:
                logging.error(traceback.format_exc())
                get_error = True
                break
            except KeyboardInterrupt:
                print("Keyboard interrupt")
                get_error = True
                break
        print("Shutdown all connections")
        ShutdownAllSwitchConnections()
        print("CLI quit")
        return 0 if not get_error else 1


if __name__ == '__main__':
    if len(sys.argv) > 1 and len(sys.argv) != 3 or (len(sys.argv) == 3 and sys.argv[1] != "--script"):
        print(OUTSIDE_HELP_MESSAGE)
        exit(1)
    
    if len(sys.argv) == 3 and sys.argv[1] == "--script":
        if not os.path.exists(sys.argv[2]):
            print("Can't find the script: {}".format(sys.argv[2]))
            exit(1)
        else:
            with open(sys.argv[2], "r") as f:
                sswitch_grapc_cli = SSwitchGRPCCLI()
                sswitch_grapc_cli.exec_script(f.readlines())
                exit(0)

    sswitch_grapc_cli = SSwitchGRPCCLI()
    sswitch_grapc_cli.exec_command_loop()
