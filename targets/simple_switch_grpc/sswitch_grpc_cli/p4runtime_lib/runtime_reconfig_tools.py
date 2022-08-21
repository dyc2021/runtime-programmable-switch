from typing import List
from typing_extensions import runtime
import networkx as nx
import json

import hashlib
import ete3
import os
import shutil

from .error_utils import P4RuntimeReconfigError
import matplotlib.pyplot as plt

from . import p4prototype

def merge_and_compile(header_file_path: str, control_block_file_path: str, output_path: str) -> str:
    with open(header_file_path, "r") as header_file:
        header_str = header_file.read()
    with open(control_block_file_path, "r") as control_block_file:
        control_block_str = control_block_file.read()
    control_block_name = control_block_str[control_block_str.find("control") + len("control") : control_block_str.find("(")].strip()
    
    merged_str = p4prototype.PROTOTYPE_STR % control_block_name
    hash_func = hashlib.new("sha256")
    hash_func.update((header_str + control_block_str).encode())
    merged_str_hash_code = int(hash_func.hexdigest(), base=16) % (10 ** 8)
    output_folder_name = "header-" + \
                            os.path.basename(header_file_path) + \
                                "-control_block-" + os.path.basename(control_block_file_path) + \
                                    "-" + str(merged_str_hash_code)
    
    final_output_path = os.path.join(output_path, output_folder_name)
    os.makedirs(final_output_path, exist_ok=True)
    shutil.copyfile(header_file_path, os.path.join(final_output_path, "headers.p4"))
    shutil.copyfile(control_block_file_path, os.path.join(final_output_path, "control_block.p4"))
    merged_file_path = os.path.join(final_output_path, "merged.p4")
    with open(merged_file_path, "w") as merged_file:
        merged_file.write(merged_str)
    p4c_compile_command = "p4c --arch v1model --target bmv2 -o {} {}".format(final_output_path, merged_file_path)
    print(p4c_compile_command)
    compile_return_code = os.system(p4c_compile_command)
    if compile_return_code != 0:
        raise P4RuntimeReconfigError("Compiling p4 merged file fails")

    merged_json_file_path = os.path.join(final_output_path, "merged.json")
    if not os.path.isfile(merged_json_file_path):
        raise P4RuntimeReconfigError("Unfound file: {}".foramt(merged_json_file_path))

    return merged_json_file_path

def type_letter_to_type(type_letter: str) -> str:
    if len(type_letter) > 1:
        raise P4RuntimeReconfigError("You should give a letter, not a string")
    
    if type_letter == 't':
        return "tabl"
    elif type_letter == 'c':
        return "cond"
    else:
        raise P4RuntimeReconfigError("Invalid type letter: {}".format(type_letter))

def eliminate_type_letter_in_name(name: str) -> str:
    return name[:4] + name[5:]

def generate_install_func_commands(runtime_json_file_path:str, 
                                   merged_json_file_path: str, 
                                   start_point: str, 
                                   end_point: str, 
                                   mount_point_number: int) -> List[str]:
    runtime_graph = generate_graph(runtime_json_file_path)
    merged_graph = generate_graph(merged_json_file_path)

    install_func_commands = []

    start_point_to_end_point_connection_types = []
    start_point_to_end_point_connection_types_number = len(runtime_graph[start_point][end_point])
    for i in range(start_point_to_end_point_connection_types_number):
        start_point_to_end_point_connection_types.append(runtime_graph[start_point][end_point][i]["type"])

    # reinit p4objects_new
    install_func_commands.append("init_p4objects_new {}".format(merged_json_file_path))

    # add flex branch
    if mount_point_number < 0:
        raise P4RuntimeReconfigError("mount_point_number can't smaller than 0")
    flex_func_mount_point_branch_name = "flx_flex_func_mount_point_number_${}$".format(mount_point_number)
    install_func_commands.append("insert flex ingress {} null null".format(flex_func_mount_point_branch_name))
    
    # add control block's tables and conditionals
    for node in list(merged_graph.nodes()):
        if node == "old_r" or node == "old_s":
            continue
        else:
            node_type = type_letter_to_type(node[4])
            processed_node_name = eliminate_type_letter_in_name(node)
            install_func_commands.append("insert {} ingress {}".format(node_type, processed_node_name))

    # add connections inside control block
    control_block_first_node = None
    control_block_end_nodes = [] # [(end_node, end_edge_type)]
    uv_dict = dict() # { (u, v): occur_times }
    for u, v in merged_graph.edges():
        if u == "old_r":
            control_block_first_node = v
            continue
        
        if (u, v) in uv_dict:
            uv_dict[(u, v)] += 1
        else:
            uv_dict[(u, v)] = 0

        edge_type = merged_graph[u][v][uv_dict[(u, v)]]["type"]

        u_type = type_letter_to_type(u[4])

        processed_u_name = eliminate_type_letter_in_name(u)
        processed_v_name = eliminate_type_letter_in_name(v) if v != "old_s" else "null"
        if v == "old_s":
            end_edge_type = merged_graph[u]["old_s"][uv_dict[(u, v)]]["type"]
            control_block_end_nodes.append((u, end_edge_type))
            continue
        install_func_commands.append("change {} ingress {} {} {}".format(u_type, 
                                                                         processed_u_name, 
                                                                         edge_type, 
                                                                         processed_v_name))

    # connect control block's end nodes to end_point
    # print(control_block_end_nodes)
    for end_node, end_edge_type in control_block_end_nodes:
        end_node_type = type_letter_to_type(end_node[4])
        processed_end_node_name = eliminate_type_letter_in_name(end_node)
        processed_end_point_name = eliminate_type_letter_in_name(end_point) if end_point != "old_s" else "null"
        install_func_commands.append("change {} ingress {} {} {}".format(end_node_type,
                                                                         processed_end_node_name,
                                                                         end_edge_type,
                                                                         processed_end_point_name))

    # connect flex_func_mount_point_branch and end_point
    processed_end_point_name = eliminate_type_letter_in_name(end_point) if end_point != "old_s" else "null"
    install_func_commands.append("change flex ingress {} false_next {}".format(flex_func_mount_point_branch_name,
                                                                               processed_end_point_name))

    # connect flex_func_mount_point_branch and control block's first node
    processed_control_block_first_node_name = eliminate_type_letter_in_name(control_block_first_node)
    install_func_commands.append("change flex ingress {} true_next {}".format(flex_func_mount_point_branch_name,
                                                                              processed_control_block_first_node_name))

    # connect start_point and flex_func_mount_point_branch
    if start_point != "old_r":
        for start_point_to_end_point_connection_type in start_point_to_end_point_connection_types:
            start_point_type = type_letter_to_type(start_point[4])
            processed_start_point_name = eliminate_type_letter_in_name(start_point)
            install_func_commands.append("change {} ingress {} {} {}".format(start_point_type, 
                                                                            processed_start_point_name,
                                                                            start_point_to_end_point_connection_type,
                                                                            flex_func_mount_point_branch_name))
    else:
        install_func_commands.append("change init ingress {}".format(flex_func_mount_point_branch_name))

    # trigger mount point
    install_func_commands.append("trigger on {}".format(mount_point_number))

    return install_func_commands

def generate_uninstall_func_commands(runtime_json_file_path: str, mount_point_number: int) -> List[str]:
    runtime_graph = generate_graph(runtime_json_file_path)

    flex_func_mount_point_branch_name = "flx_flex_func_mount_point_number_${}$".format(mount_point_number)

    uninstall_func_commands = []

    # trigger off
    uninstall_func_commands.append("trigger off {}".format(mount_point_number))

    # find nodes to be deleted
    if len(runtime_graph[flex_func_mount_point_branch_name]) != 2:
        raise P4RuntimeReconfigError("flex_func_mount_point_branch doesn't have two edges")
    true_next_node = None
    false_next_node = None
    for neightbor in runtime_graph[flex_func_mount_point_branch_name]:
        edge_type = runtime_graph[flex_func_mount_point_branch_name][neightbor][0]["type"]
        if edge_type == "true_next":
            true_next_node = neightbor
        elif edge_type == "false_next":
            false_next_node = neightbor

    processed_true_next_node_name = eliminate_type_letter_in_name(true_next_node)
    processed_false_next_node_name = eliminate_type_letter_in_name(false_next_node) if false_next_node != "old_s" else "null"
    
    nodes_to_be_deleted = set()
    for path in list(nx.all_simple_paths(runtime_graph, source=true_next_node, target=false_next_node)):
        nodes_to_be_deleted.update(path)
    nodes_to_be_deleted.remove(false_next_node)

    # delete nodes
    for node in nodes_to_be_deleted:
        node_type = type_letter_to_type(node[4])
        processed_node_name = eliminate_type_letter_in_name(node)
        uninstall_func_commands.append("delete {} ingress {}".format(node_type, processed_node_name))
    
    # let the parents of flex_func_mount_point_branch point to the false_next_node
    parents_of_flex = list(runtime_graph.in_edges(flex_func_mount_point_branch_name, data=True))
    for i in range(len(parents_of_flex)):
        if parents_of_flex[i][0] != "old_r":
            parent_node_of_flex = parents_of_flex[i][0]
            ori_edge_type = parents_of_flex[i][2]["type"]
            parent_node_type = type_letter_to_type(parent_node_of_flex[4])
            processed_parent_node_name = eliminate_type_letter_in_name(parent_node_of_flex)
            uninstall_func_commands.append("change {} ingress {} {} {}".format(parent_node_type,
                                                                               processed_parent_node_name,
                                                                               ori_edge_type,
                                                                               processed_false_next_node_name))
        else:
            if len(parent_node_of_flex) > 1:
                raise P4RuntimeReconfigError("old_r is flex branch's parent, but flex branch has more than one parent")
            uninstall_func_commands.append("change init ingress {}".format(processed_false_next_node_name))
    
    # delete flex_func_mount_point_branch
    uninstall_func_commands.append("delete flex ingress {}".format(flex_func_mount_point_branch_name))

    return uninstall_func_commands

def generate_migrate_func_commands(s0_name: str,
                                   s0_migrate_json_file_path: str,
                                   s0_func_mount_point_number: int,
                                   s1_name: str,
                                   s1_runtime_json_file_path: str,
                                   s1_start_point: str,
                                   s1_end_point: str,
                                   s1_func_mount_point_number: int) -> List[str]:
    s0_migrate_json_graph = generate_graph(s0_migrate_json_file_path)
    s1_runtime_graph = generate_graph(s1_runtime_json_file_path)
    s0_flex_func_mount_point_branch_name = "flx_flex_func_mount_point_number_${}$".format(s0_func_mount_point_number)
    s1_flex_func_mount_point_branch_name = "flx_flex_func_mount_point_number_${}$".format(s1_func_mount_point_number)

    migrate_func_commands = []

    # connect to s1
    migrate_func_commands.append("connect {}".format(s1_name))

    # reinit s1's p4objects_new using the s0_migrate_json_file (generated from s0's runtime json)
    migrate_func_commands.append("init_p4objects_new {}".format(s0_migrate_json_file_path))

    # get the connection types between s1's start point and end point
    s1_start_point_to_end_point_connection_types = []
    s1_start_point_to_end_point_connection_types_number = len(s1_runtime_graph[s1_start_point][s1_end_point])
    for i in range(s1_start_point_to_end_point_connection_types_number):
        s1_start_point_to_end_point_connection_types.append(s1_runtime_graph[s1_start_point][s1_end_point][i]["type"])

    # inject s1_flex_func_mount_point_branch
    migrate_func_commands.append("insert flex ingress {} null null".format(s1_flex_func_mount_point_branch_name))

    # find the tables/conditionals to be added (these components are in s0_migrate_json_graph)
    if len(s0_migrate_json_graph[s0_flex_func_mount_point_branch_name]) != 2:
        raise P4RuntimeReconfigError("s0_flex_func_mount_point_branch doesn't have two edges")
    true_next_node = None
    false_next_node = None
    for neightbor in s0_migrate_json_graph[s0_flex_func_mount_point_branch_name]:
        edge_type = s0_migrate_json_graph[s0_flex_func_mount_point_branch_name][neightbor][0]["type"]
        if edge_type == "true_next":
            true_next_node = neightbor
        elif edge_type == "false_next":
            false_next_node = neightbor

    processed_true_next_node_name = eliminate_type_letter_in_name(true_next_node)
    processed_false_next_node_name = eliminate_type_letter_in_name(false_next_node) if false_next_node != "old_s" else "null"
    
    nodes_to_be_added = set()
    for path in list(nx.all_simple_paths(s0_migrate_json_graph, source=true_next_node, target=false_next_node)):
        nodes_to_be_added.update(path)
    nodes_to_be_added.remove(false_next_node)

    # add nodes
    for node in nodes_to_be_added:
        node_type = type_letter_to_type(node[4])
        processed_node_name = eliminate_type_letter_in_name(node)
        migrate_func_commands.append("insert {} ingress {}".format(node_type, processed_node_name))

    # a subgraph containing the nodes to be added
    s0_migrate_json_subgraph: nx.MultiDiGraph = s0_migrate_json_graph.subgraph(nodes_to_be_added)

    # add edges between these nodes
    control_block_end_nodes = [] # [ (end_node, end_edge_type) ]
    uv_dict = dict() # { (u, v): occur_times }
    for u, v in s0_migrate_json_subgraph.edges():
        if (u, v) in uv_dict:
            uv_dict[(u, v)] += 1
        else:
            uv_dict[(u, v)] = 0

        edge_type = s0_migrate_json_subgraph[u][v][uv_dict[(u, v)]]["type"]

        u_type = type_letter_to_type(u[4])

        processed_u_name = eliminate_type_letter_in_name(u)
        processed_v_name = eliminate_type_letter_in_name(v) if v != "old_s" else "null"
        if v == "old_s":
            end_edge_type = s0_migrate_json_subgraph[u]["old_s"][uv_dict[(u, v)]]["type"]
            control_block_end_nodes.append((u, end_edge_type))
            continue
        migrate_func_commands.append("change {} ingress {} {} {}".format(u_type, 
                                                                         processed_u_name, 
                                                                         edge_type, 
                                                                         processed_v_name))
    
    # connect control block's end nodes to s1_end_point
    for end_node, end_edge_type in control_block_end_nodes:
        end_node_type = type_letter_to_type(end_node[4])
        processed_end_node_name = eliminate_type_letter_in_name(end_node)
        processed_end_point_name = eliminate_type_letter_in_name(s1_end_point) if s1_end_point != "old_s" else "null"
        migrate_func_commands.append("change {} ingress {} {} {}".format(end_node_type,
                                                                         processed_end_node_name,
                                                                         end_edge_type,
                                                                         processed_end_point_name))

    # connect s1_flex_func_mount_point_branch and s1_end_point
    processed_end_point_name = eliminate_type_letter_in_name(s1_end_point) if s1_end_point != "old_s" else "null"
    migrate_func_commands.append("change flex ingress {} false_next {}".format(s1_flex_func_mount_point_branch_name,
                                                                               processed_end_point_name))

    # connect s1_flex_func_mount_point_branch and control block's first node
    processed_control_block_first_node_name = processed_true_next_node_name
    migrate_func_commands.append("change flex ingress {} true_next {}".format(s1_flex_func_mount_point_branch_name,
                                                                              processed_control_block_first_node_name))

    # connect s1_start_point and s1_flex_func_mount_point_branch
    if s1_start_point != "old_r":
        for start_point_to_end_point_connection_type in s1_start_point_to_end_point_connection_types:
            start_point_type = type_letter_to_type(s1_start_point[4])
            processed_start_point_name = eliminate_type_letter_in_name(s1_start_point)
            migrate_func_commands.append("change {} ingress {} {} {}".format(start_point_type, 
                                                                            processed_start_point_name,
                                                                            start_point_to_end_point_connection_type,
                                                                            s1_flex_func_mount_point_branch_name))
    else:
        migrate_func_commands.append("change init ingress {}".format(s1_flex_func_mount_point_branch_name))

    # trigger mount point
    migrate_func_commands.append("trigger on {}".format(s1_func_mount_point_number))

    # connect to original switch
    migrate_func_commands.append("connect {}".format(s0_name))

    return migrate_func_commands

# adapted from FlexCore (https://github.com/jiarong0907/FlexCore)
def generate_graph(json_file_path) -> nx.MultiDiGraph:
    with open(json_file_path, "r") as f:
        new_p4json = json.load(f)

    pipelines = new_p4json["pipelines"]
    for p in pipelines:
        if p["name"] == "ingress":
            ingress = p
        if p["name"] == "egress":
            egress = p

    graph = nx.MultiDiGraph()
    # the root of the graph
    graph.add_nodes_from([("old_"+"r", {'flex_name':"old_"+"r", 'name':'r'})])
    # the sink of the graph
    graph.add_nodes_from([("old_"+"s", {'flex_name':"old_"+"s", 'name':'s'})])

    name2id = {None: "old_"+"s"}

    # process the nodes
    for t in ingress["tables"]:
        if "flex_name" not in t:
            raise P4RuntimeReconfigError("Can't find flex name for table {} in the json file".format(t["name"]))
        graph.add_nodes_from([(t["flex_name"], t)])
        name2id[t["name"]] = t["flex_name"]

    for c in ingress["conditionals"]:
        if "flex_name" not in c:
            raise P4RuntimeReconfigError("Can't find flex name for conditional {} in the json file".format(c["name"]))
        graph.add_nodes_from([(c["flex_name"], c)])
        name2id[c["name"]] = c["flex_name"]

    if "action_calls" in ingress:
        raise P4RuntimeReconfigError("We don't support action_calls")

    # process the edges
    graph.add_edges_from([("old_"+"r", name2id[ingress["init_table"]], {'type':'base_default_next'})])
    for t in ingress["tables"]:
        # normally it has only one next table
        if t["name"] in name2id:
            curId = name2id[t["name"]]
            if t["base_default_next"] in name2id:
                nextId = name2id[t["base_default_next"]]
                graph.add_edges_from([(curId, nextId, {'type':'base_default_next'})])

                for nt in t["next_tables"]:
                    nextName = t["next_tables"][nt]
                    if nextName in name2id:
                        nextId = name2id[nextName]
                        graph.add_edges_from([(curId, nextId, {"type": nt})])
                    else:
                        print("Unfound name: {}; maybe you have deleted this node, please check".format(nextName))
            else:
                print("Unfound name: {}; maybe you have deleted this node, please check".format(t["base_default_next"]))
        else:
            print("Unfound name: {}; maybe you have deleted this node, please check".format(t["name"]))

    for c in ingress["conditionals"]:
        if c["name"] in name2id:
            if c["true_next"] in name2id:
                if c["false_next"] in name2id:
                    graph.add_edges_from([(name2id[c["name"]], name2id[c["true_next"]], {'type':'true_next'}),
                                        (name2id[c["name"]], name2id[c["false_next"]], {'type':'false_next'})])
                else:
                    print("Unfound name: {}; maybe you have deleted this node, please check".format(c["false_next"]))
            else:
                print("Unfound name: {}; maybe you have deleted this node, please check".format(c["true_next"]))
        else:
            print("Unfound name: {}; maybe you have deleted this node, please check".format(c["name"]))

    return graph

class ProgramGraphManager:
    def __init__(self):
        self.init_graph: nx.MultiDiGraph = None
        self.cur_graph: nx.MultiDiGraph = None

    def update_graph(self, new_config_json_file_path: str):
        graph = generate_graph(new_config_json_file_path)
        if self.init_graph is None:
            self.init_graph = graph
        self.cur_graph = graph

def update_merged_json_file(merged_json_file_path: str):
    with open(merged_json_file_path, "r") as f:
            merged_json = json.load(f)

    pipelines = merged_json["pipelines"]
    for p in pipelines:
        if p["name"] == "ingress":
            ingress = p
        if p["name"] == "egress":
            egress = p

    for t in ingress["tables"]:
        t["flex_name"] = "new_"+"t%s" % t["name"]

    for c in ingress["conditionals"]:
        c["flex_name"] = "new_"+"c%s" % c["name"]

    if "action_calls" in ingress:
        raise P4RuntimeReconfigError("We don't support action_calls")

    # update original json file
    with open(merged_json_file_path, "w") as f:
        json.dump(fp=f, obj=merged_json, indent=4, sort_keys=True)

def update_init_forwarding_pipeline_json_file(init_forwarding_pipeline_json_file_path: str):
    with open(init_forwarding_pipeline_json_file_path, "r") as f:
            init_forwarding_pipeline_json = json.load(f)

    pipelines = init_forwarding_pipeline_json["pipelines"]
    for p in pipelines:
        if p["name"] == "ingress":
            ingress = p
        if p["name"] == "egress":
            egress = p

    for t in ingress["tables"]:
        t["flex_name"] = "old_"+"t%s" % t["name"]

    for c in ingress["conditionals"]:
        c["flex_name"] = "old_"+"c%s" % c["name"]

    if "action_calls" in ingress:
        raise P4RuntimeReconfigError("We don't support action_calls")

    # update original json file
    with open(init_forwarding_pipeline_json_file_path, "w") as f:
        json.dump(fp=f, obj=init_forwarding_pipeline_json, indent=4, sort_keys=True)

def generate_migrate_json_file(runtime_json_file_path: str) -> str:
    with open(runtime_json_file_path, "r") as f:
        runtime_json = json.load(f)
    
    pipelines = runtime_json["pipelines"]
    for p in pipelines:
        if p["name"] == "ingress":
            ingress = p
        if p["name"] == "egress":
            egress = p

    for t in ingress["tables"]:
        if "flex_name" in t:
            t["flex_name"] = "new_"+"t%s" % t["name"]
        else:
            raise P4RuntimeReconfigError("Can't find flex name for table {} in the json file".format(t["name"]))
    
    for c in ingress["conditionals"]:
        if "flex_name" in c:
            if c["flex_name"][:4] != "flx_":
                c["flex_name"] = "new_"+"c%s" % c["name"]
        else:
            raise P4RuntimeReconfigError("Can't find flex name for conditional {} in the json file".format(c["name"]))

    if "action_calls" in ingress:
        raise P4RuntimeReconfigError("We don't support action_calls")

    parent_folder_path_of_runtime_json_file = os.path.dirname(runtime_json_file_path)
    runtime_json_file_name = os.path.basename(runtime_json_file_path).split(".")[0]
    migrate_json_file_path = os.path.join(parent_folder_path_of_runtime_json_file, runtime_json_file_name + "_migrate.json")

    # generate a new json file
    with open(migrate_json_file_path, "w") as f:
        json.dump(fp=f, obj=runtime_json, indent=4, sort_keys=True)

    return migrate_json_file_path

def flex_name_to_human_readable_name(flex_name: str) -> str:
    # we assume flex_name is in the form of "old_<type>xxx" or "new_<type>xxx", <type> = 't' or 'c'
    # Or, specially, "old_r", "old_s" or "flx_xxx"
    if flex_name == "old_r":
        return " [root] "
    elif flex_name == "old_s":
        return " [sink] "
    elif flex_name[:4] == "flx_":
        return flex_name
    elif flex_name[4] == 't':
        return "table" + "[" + eliminate_type_letter_in_name(flex_name) + "]"
    elif flex_name[4] == 'c':
        return "conditional" + "[" + eliminate_type_letter_in_name(flex_name) + "]"
    else:
        raise P4RuntimeReconfigError("Invalid graph name: graph name contains type label [{}] which can't be interpreted".format(flex_name[4]))

def human_readable_name_to_flex_name(human_readable_name: str) -> str:
    # we assume human readable names are in the form of "<type>[old_xxx]" or "<type>[new_xxx]", <type> = "table" or "conditional"
    # Or, specially, "[root]", "[sink]" or "flx_xxx"
    if human_readable_name == "[root]":
        return "old_r"
    elif human_readable_name == "[sink]":
        return "old_s"
    elif human_readable_name[:4] == "flx_":
        return human_readable_name

    type_name, _, node_name_tmp =  human_readable_name.partition('[')
    node_name = node_name_tmp[:-1]

    if type_name == "table":
        return node_name[:4] + 't' + node_name[4:]
    elif type_name == "conditional":
        return node_name[:4] + 'c' + node_name[4:]
    else:
        raise P4RuntimeReconfigError("Invalid human readable name: human readable name contains type label [{}] which can't be interpreted".format(type_name))

def display_graph_in_command_line(graph: nx.MultiDiGraph):
    # refer to https://stackoverflow.com/questions/51273890/how-to-convert-from-networkx-graph-to-ete3-tree-object
    # when the graph is large, this function might be very slow
    # TODO: find an alternative
    # root = " [root] "
    # subtrees = {flex_name_to_human_readable_name(node):ete3.Tree(name=flex_name_to_human_readable_name(node)) for node in graph.nodes()}
    # [*map(lambda edge:subtrees[flex_name_to_human_readable_name(edge[0])].add_child(subtrees[flex_name_to_human_readable_name(edge[1])]), graph.edges())]
    # tree = subtrees[root]
    # print(tree.get_ascii())
    raise P4RuntimeReconfigError("We don't support displaying graph in command line")

def display_graph_using_matplotlib(graph: nx.MultiDiGraph):
    pos = nx.spring_layout(graph)
    nx.draw(graph, pos, node_size=1500, with_labels=True)
    plt.draw()
    plt.show()
    # plt.savefig("1.pdf")



