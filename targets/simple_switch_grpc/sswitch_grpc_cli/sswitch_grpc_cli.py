#!/usr/bin/env python3
import argparse
import os
import sys
from time import sleep
from typing import List
import traceback
import logging

import grpc

import p4runtime_lib.bmv2
import p4runtime_lib.helper
from p4runtime_lib.switch import RuntimeReconfigCommandParser, ShutdownAllSwitchConnections
from .p4runtime_lib.error_utils import P4RuntimeReconfigError

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

::NOTES::
You can use command `list_switches` to see all the switches connected to this CLI
You can use command `q` or `quit` to quit
You can enter `h` or `help` to see a brief help message
To see this detailed help message, please enter `detailed_help`
Please DONT use `set_forwarding_pipeline_config` twice for a single switch, since our reconfiguration changes switch's program
but doesn't update CLI's p4info. Using an obsolete p4info is dangerous for `set_forwarding_pipeline_config`
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
List connected switches: list_switches
Quit: q or quit
"""

OUTSIDE_HELP_MESSAGE = \
"""
You can start this CLI without any command line argument
Or, by adding tag `--script <script_path>`, you can directly run a script
"""

def printGrpcError(e):
    print("gRPC Error:", e.details(), end=' ')
    status_code = e.code()
    print("(%s)" % status_code.name, end=' ')
    traceback = sys.exc_info()[2]
    print("[%s:%d]" % (traceback.tb_frame.f_code.co_filename, traceback.tb_lineno))

def exec_commands(commands: List[str]):
    print("Start running script")

    cur_connection = None # BMV2SwitchConnect object

    for i, command in enumerate(commands):
        print("Execute command [{}]: {}".format(i, command))

        parsed_user_input = command.split()
        if parsed_user_input[0] == "connect" and len(parsed_user_input) == 4:
            if cur_connection is not None:
                print("If you run a script, you can't use `connect` command twice")
                return 1
            else:
                try:
                    connection = p4runtime_lib.bmv2.Bmv2SwitchConnection(name=parsed_user_input[1],
                                                                        address=parsed_user_input[2],
                                                                        device_id=int(parsed_user_input[3]),
                                                                        proto_dump_file="logs/{}_p4runtime_requests.txt".format(parsed_user_input[1]))
                    connection.MasterArbitrationUpdate()
                    cur_connection = connection
                except grpc.RpcError as e:
                    printGrpcError(e)
                    break
                except Exception as e:
                    logging.error(traceback.format_exc())
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    break
        elif parsed_user_input[0] == "set_forwarding_pipeline_config" and len(parsed_user_input) == 3:
            if cur_connection is None:
                print("This CLI doesn't connect to any switch now, please use `connect` command before running `set_forwarding_pipeline_config`")
                return 1
            else:
                if not os.path.exists(parsed_user_input[1]):
                    print("p4info file not found: {}".format(parsed_user_input[1]))
                    break
                if not os.path.exists(parsed_user_input[2]):
                    print("bmv2 JSON file not found: {}".format(parsed_user_input[2]))
                    break

                try:
                    p4info_helper = p4runtime_lib.helper.P4InfoHelper(parsed_user_input[1])
                    cur_connection.SetForwardingPipelineConfig(p4info=p4info_helper.p4info,
                                                               bmv2_json_file_path=parsed_user_input[2])
                except grpc.RpcError as e:
                    printGrpcError(e)
                    break
                except Exception as e:
                    logging.error(traceback.format_exc())
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    break
        else:
            if cur_connection is None:
                print("This CLI doesn't connect to any switch now, please use `connect` command")
                break
            else:
                try:
                    parsed_runtime_reconfig_cmd = RuntimeReconfigCommandParser(command)
                except P4RuntimeReconfigError:
                    print("Invalid command: {}".format(command))
                    break

                try:
                    cur_connection.RuntimeReconfig(parsed_cmd=parsed_runtime_reconfig_cmd)
                except grpc.RpcError as e:
                    printGrpcError(e)
                    break
                except P4RuntimeReconfigError as e:
                    print(e)
                    break
                except Exception as e:
                    logging.error(traceback.format_exc())
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    break

        print("Finish running command [{}]: {}".format(i, command))
        if i == len(commands) - 1:
            print("All commands are executed")


    print("Shutdown connection")
    ShutdownAllSwitchConnections()
    print("Program exits")
    return 0

def exec_command_loop():
    print(DETAILED_HELP_MESSAGE)
    connections = {} # { switch_name: BMV2SwitchConnect object }
    cur_connection = None # BMV2SwitchConnect object
    user_input = input("simple_switch_grpc_cli> ")

    while user_input != "q" or user_input != "quit":
        parsed_user_input = user_input.split()
        if len(parsed_user_input) == 1 and (parsed_user_input[0] == "h" or parsed_user_input[0] == "help"):
            print(HELP_MESSAGE)
        elif len(parsed_user_input) == 1 and parsed_user_input[0] == "detailed_help":
            print(DETAILED_HELP_MESSAGE)
        elif len(parsed_user_input) == 1 and parsed_user_input[0] == "list_switches":
            if len(connections) != 0:
                for i, connection in enumerate(connections):
                    print("connection [{}]: Name: {}, Address: {}, device_id: {}, log_file: {}".format(i, 
                                                                                                       connection.name, 
                                                                                                       connection.address, 
                                                                                                       connection.device_id, 
                                                                                                       connection.proto_dump_file))
            else:
                print("No connection")
        elif parsed_user_input[0] == "connect" and (len(parsed_user_input) == 2 or len(parsed_user_input) == 4):
            if len(parsed_user_input) == 2:
                if parsed_user_input[1] in connections:
                    cur_connection = connections[parsed_user_input[1]]
                else:
                    print("Can't find connection whose name is {}".format(parsed_user_input[1]))
            else:
                print("Connecting ...")
                try:
                    connection = p4runtime_lib.bmv2.Bmv2SwitchConnection(name=parsed_user_input[1],
                                                                        address=parsed_user_input[2],
                                                                        device_id=int(parsed_user_input[3]),
                                                                        proto_dump_file="logs/{}_p4runtime_requests.txt".format(parsed_user_input[1]))
                    connection.MasterArbitrationUpdate()
                    connections[parsed_user_input[1]] = connection
                    cur_connection = connection
                except grpc.RpcError as e:
                    printGrpcError(e)
                    break
                except Exception as e:
                    logging.error(traceback.format_exc())
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    break
                print("Connect successfully")
        elif parsed_user_input[0] == "set_forwarding_pipeline_config" and len(parsed_user_input) == 3:
            if cur_connection is None:
                print("This CLI doesn't connect to any switch now, please use `connect` command before running `set_forwarding_pipeline_config`")
            else:
                if not os.path.exists(parsed_user_input[1]):
                    print("p4info file not found: {}".format(parsed_user_input[1]))
                    try:
                        user_input = input("{}simple_switch_grpc_cli> ".format("" if cur_connection is None else "({}) ".format(cur_connection.name)))
                    except Exception as e:
                        logging.error(traceback.format_exc())
                        break
                    except KeyboardInterrupt:
                        print("Keyboard interrupt")
                        break
                    continue
                if not os.path.exists(parsed_user_input[2]):
                    print("bmv2 JSON file not found: {}".format(parsed_user_input[2]))
                    try:
                        user_input = input("{}simple_switch_grpc_cli> ".format("" if cur_connection is None else "({}) ".format(cur_connection.name)))
                    except Exception as e:
                        logging.error(traceback.format_exc())
                        break
                    except KeyboardInterrupt:
                        print("Keyboard interrupt")
                        break
                    continue
                print("Installing p4 program on {} ...".format(cur_connection.name))
                try:
                    p4info_helper = p4runtime_lib.helper.P4InfoHelper(parsed_user_input[1])
                    cur_connection.SetForwardingPipelineConfig(p4info=p4info_helper.p4info,
                                                               bmv2_json_file_path=parsed_user_input[2])
                except grpc.RpcError as e:
                    printGrpcError(e)
                    break
                except Exception as e:
                    logging.error(traceback.format_exc())
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    break
                print("Install successfully")
        else:
            if cur_connection is None:
                print("This CLI doesn't connect to any switch now, please use `connect` command")
            else:
                try:
                    parsed_runtime_reconfig_cmd = RuntimeReconfigCommandParser(user_input)
                except P4RuntimeReconfigError:
                    print("Invalid command, please enter again")
                    try:
                        user_input = input("{}simple_switch_grpc_cli> ".format("" if cur_connection is None else "({}) ".format(cur_connection.name)))
                    except Exception as e:
                        logging.error(traceback.format_exc())
                        break
                    except KeyboardInterrupt:
                        print("Keyboard interrupt")
                        break
                    continue

                print("Runtime reconfigurating ...")
                try:
                    cur_connection.RuntimeReconfig(parsed_cmd=parsed_runtime_reconfig_cmd)
                except grpc.RpcError as e:
                    printGrpcError(e)
                    break
                except P4RuntimeReconfigError as e:
                    print(e)
                    break
                except Exception as e:
                    logging.error(traceback.format_exc())
                    break
                except KeyboardInterrupt:
                    print("Keyboard interrupt")
                    break
                print("Runtime reconfiguration ends")

        try:
            user_input = input("{}simple_switch_grpc_cli> ".format("" if cur_connection is None else "({}) ".format(cur_connection.name)))
        except Exception as e:
            logging.error(traceback.format_exc())
            break
        except KeyboardInterrupt:
            print("Keyboard interrupt")
            break

    print("Shutdown all connections")
    ShutdownAllSwitchConnections()
    print("CLI quit")
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and len(sys.argv) != 3 or (len(sys.argv) == 3 and sys.argv[1] != "--script"):
        print(OUTSIDE_HELP_MESSAGE)
        exit(1)
    
    if sys.argv[1] == "--script":
        if not os.path.exists(sys.argv[2]):
            print("Can't find the script: {}".format(sys.argv[2]))
            exit(1)
        else:
            with open(sys.argv[2], "r") as f:
                exec_commands(f.readlines())
                exit(0)

    exec_command_loop()
