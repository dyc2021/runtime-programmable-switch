import networkx as nx
import json

import sys
import ete3
import os
import shutil

from error_utils import P4RuntimeReconfigError
import matplotlib.pyplot as plt

import p4prototype

def merge_and_compile(header_file_path: str, control_block_file_path: str, output_path: str) -> str:
    with open(header_file_path, "r") as header_file:
        header_str = header_file.read()
    with open(control_block_file_path, "r") as control_block_file:
        control_block_str = control_block_file.read()
    control_block_name = control_block_str[control_block_str.find("control") + len("control"), control_block_str.find("(")].strip()
    
    merged_str = p4prototype.PROTOTYPE_STR % control_block_name
    merged_str_hash_code = abs(hash(control_block_name + header_str + control_block_str)) % (10 ** 8)
    output_folder_name = "header_" + \
                            os.path.basename(header_file_path) + \
                                "_control_block_" + os.path.basename(control_block_file_path) + \
                                    "_" + merged_str_hash_code
    
    final_output_path = os.path.join(output_path, output_folder_name)
    os.makedirs(final_output_path, exist_ok=True)
    shutil.copyfile(header_file_path, os.path.join(final_output_path, "headers.p4"))
    shutil.copyfile(header_file_path, os.path.join(final_output_path, "control_block.p4"))
    merged_file_path = os.path.join(final_output_path, "merged.p4")
    with open(merged_file_path, "w") as merged_file:
        merged_file.write(merged_str)
    compile_return_code = os.system("p4c --arch v1model --target bmv2 -o {} {}".format(final_output_path, merged_file_path))
    if compile_return_code != 0:
        raise P4RuntimeReconfigError("Compiling p4 merged file fails")

    return merged_file_path


# adapted from FlexCore (https://github.com/jiarong0907/FlexCore)

class ProgramGraphManager:
    def __init__(self):
        self.init_graph: nx.MultiDiGraph = None
        self.cur_graph: nx.MultiDiGraph = None
        self.alias = {
            'ingress_metadata.ifindex' : 'standard_metadata.ingress_port',
            'l2_metadata.lkp_pkt_type' : 'l2_metadata.lkp_pkt_type',
            'l2_metadata.lkp_mac_sa' : 'ethernet.srcAddr',
            'l2_metadata.lkp_mac_da' : 'ethernet.dstAddr',
            'l3_metadata.lkp_ip_ttl' : 'ipv4.ttl',
            'ipv4_metadata.lkp_ipv4_sa' : 'ipv4.srcAddr',
            'ipv4_metadata.lkp_ipv4_da' : 'ipv4.dstAddr',
            'l3_metadata.lkp_ip_ttl' : 'ipv6.hopLimit',
            'ipv6_metadata.lkp_ipv6_sa' : 'ipv6.srcAddr',
            'ipv6_metadata.lkp_ipv6_da' : 'ipv6.dstAddr',
        }

    def _aliasList_check(self, f1, f2):
        return False

    def _compare_fieldValue(self, f1, f2):
        if f1[0] == 'scalars' and f2[0] == 'scalars':
            length = min(len(f1[1]), len(f2[1]))
            prefixLen = length - 3
            if f1[1][0:prefixLen] == f2[1][0:prefixLen]: # TODO: temporary solution for diff numbering of targets
                return True
            else:
                return self._aliasList_check(f1, f2)
        else:
            if f1 == f2:
                return True
            return self._aliasList_check(f1, f2)

    def _compare_exprValue(self, v1, v2):
        if not ('op' in v1 and 'op' in v2):
            print ("op not in exprValue: ", v1, v2)
            sys.exit(-2)
        if v1['op'] != v2['op']:
            return False
        if not ('left' in v1 and 'left' in v2 and 'right' in v1 and 'right' in v2):
            print ("Something wrong about understanding op expression, ", v1, v2)
            sys.exit(-2)
        if not self._compare_leftRight(v1['left'], v2['left']) \
            or not self._compare_leftRight(v1['right'], v2['right']):
            return False
        return True

    def _compare_leftRight(self, lr1, lr2):
        if lr1 == None and lr2 == None:
            return True
        if lr1 == None or lr2 == None:
            return False
        if lr1['type'] != lr2['type']:
            return False
        if lr1['type'] == 'expression':
            return self._compare_exprValue(lr1['value'], lr2['value'])
        if lr1['type'] == 'field':
            return self._compare_fieldValue(lr1['value'], lr2['value'])
        if lr1['type'] == 'hexstr':
            return lr1['value'] == lr2['value']
        print ("Something wrong about understanding leftRight, type: "+lr1['type'])
        sys.exit(-3)

    def _compare_expression(self, e1, e2):
        return self._compare_leftRight(e1, e2)

    def _compare_conditional(self, n1, n2):
        return self._compare_expression(n1['expression'], n2['expression'])

    def _alias_check(self, s1, s2):
        if s1 == s2:
            return True
        if s1 in self.alias and self.alias[s1] == s2 \
            or s2 in self.alias and self.alias[s2] == s1:
            return True
        return False

    def _compare_table(self, n1, n2):
        # compare keys
        if len(n1['key']) != len(n2['key']):
            return False
        for i in range(0, len(n1['key'])):
            if n1['key'][i]['match_type'] != n2['key'][i]['match_type'] \
                or not self._alias_check(n1['key'][i]['name'], n2['key'][i]['name']):
                return False

        # compare actions
        if len(n1['actions']) != len(n2['actions']):
            return False
        for i in range(0, len(n1['actions'])):
            if n1['actions'][i] != n2['actions'][i] \
                or not self._alias_check(n1['actions'][i], n2['actions'][i]):
                return False

        # If one table has __HIT__ and __MISS__, but the other does not, they are viewed as
        # different tables, even if everything else is the same.
        if ('__HIT__' in n1['next_tables'] and '__HIT__' not in n2['next_tables']) or \
           ('__HIT__' in n2['next_tables'] and '__HIT__' not in n1['next_tables']):
           return False

        return True

    def update_json_file_and_cur_graph(self, new_config_json_path):
        """
        Please note that this function will add "myId" field to json file; 
        To ensure the coherence between updates, please apply this function to json file before using that json file to init switch
        """        
        with open(new_config_json_path, "r") as f:
            new_p4json = json.load(f)

        pipelines = new_p4json["pipelines"]
        for p in pipelines:
            if p["name"] == "ingress":
                ingress = p
            if p["name"] == "egress":
                egress = p

        graph = nx.MultiDiGraph()
        # the root of the graph
        graph.add_nodes_from([("old_"+"r", {'myId':"old_"+"r", 'name':'r'})])
        # the sink of the graph
        graph.add_nodes_from([("old_"+"s", {'myId':"old_"+"s", 'name':'s'})])

        name2id = {None: "old_"+"s"}
        actionId2Name = {}

        # process the nodes
        for a in new_p4json["actions"]:
            actionId2Name[a["id"]] = a["name"]

        for t in ingress["tables"]:
            if self.init_graph is None or \
                    ("myId" in t and t["myId"] in self.init_graph.nodes and self._compare_table(t, self.init_graph.nodes[t["myId"]])):
                t["myId"] = "old_"+"t%s" % t["name"]
            else:
                t["myId"] = "new_"+"t%s" % t["name"]

            graph.add_nodes_from([(t["myId"], t)])
            name2id[t["name"]] = t["myId"]

        for c in ingress["conditionals"]:
            if self.init_graph is None or \
                    ("myId" in c and c["myId"] in self.init_graph.nodes and self._compare_conditional(c, self.init_graph.nodes[c["myId"]])):
                c["myId"] = "old_"+"c%s" % c["name"]
            else:
                c["myId"] = "new_"+"c%s" % c["name"] if "TE" not in c["name"] else "flx_"+"c%s" % c["name"]

            graph.add_nodes_from([(c["myId"], c)])
            name2id[c["name"]] = c["myId"]

        if "action_calls" in ingress:
            for a in ingress["action_calls"]:
                # we assume that if actions' names are equal, they can be seemed as one action
                if self.init_graph is None or \
                        (a["name"] in self.init_graph.nodes):
                    aid = "old_"+"c%s" % a["name"]
                else:
                    aid = "new_"+"c%s" % a["name"]

                graph.add_nodes_from([(aid, {"myId":aid, "name":actionId2Name[a["action_id"]]})])
                name2id[a["name"]] = aid

        # process the edges
        graph.add_edges_from([("old_"+"r", name2id[ingress["init_table"]], {'type':'b_next'})]) # base_default_next
        for t in ingress["tables"]:
            # normally it has only one next table
            curId = name2id[t["name"]]
            nextId = name2id[t["base_default_next"]]
            graph.add_edges_from([(curId, nextId, {'type':'b_next'})])

            for nt in t["next_tables"]:
                nextName = t["next_tables"][nt]
                nextId = name2id[nextName]
                graph.add_edges_from([(curId, nextId, {"type": nt})])

        for c in ingress["conditionals"]:
            graph.add_edges_from([(name2id[c["name"]], name2id[c["true_next"]], {'type':'t_next'}),
                                (name2id[c["name"]], name2id[c["false_next"]], {'type':'f_next'})])

        if "action_calls" in ingress:
            for a in ingress["action_calls"]:
                graph.add_edges_from([(name2id[c["name"]], name2id[c["next_node"]], {'type':'b_next'})])

        if self.init_graph is None:
            self.init_graph = graph
        self.cur_graph = graph

        # update original json file
        with open(new_config_json_path, "w") as f:
            json.dump(fp=f, obj=new_p4json)

def graph_name_to_human_readable_name(graph_name: str) -> str:
    # we assume graph names are in the form of "old_<type>xxx" or "new_<type>xxx", <type> = 't' or 'c' or 'a'
    # Or, specially, "old_r" and "old_s"
    if graph_name == "old_r":
        return " [root] "
    elif graph_name == "old_s":
        return " [sink] "
    elif graph_name[4] == 't':
        return "table" + "[" + graph_name[:4] + graph_name[5:] + "]"
    elif graph_name[4] == 'c':
        return "conditional" + "[" + graph_name[:4] + graph_name[5:] + "]"
    elif graph_name[4] == 'a':
        return "action_call" + "[" + graph_name[:4] + graph_name[5:] + "]"
    else:
        raise P4RuntimeReconfigError("Invalid graph name: graph name contains type label [{}] which can't be interpreted".format(graph_name[4]))

def human_readable_name_to_graph_name(human_readable_name: str) -> str:
    # we assume human readable names are in the form of "<type>[old_xxx]" or "<type>[new_xxx]", <type> = "table" or "conditional" or "action_call"
    # Or, specially, "[root]" and "[sink]"
    if human_readable_name == "[root]":
        return "old_r"
    elif human_readable_name == "[sink]":
        return "old_s"

    type_name, _, node_name_tmp =  human_readable_name.partition('[')
    node_name = node_name_tmp[:-1]

    if type_name == "table":
        return node_name[:4] + 't' + node_name[4:]
    elif type_name == "conditional":
        return node_name[:4] + 'c' + node_name[4:]
    elif type_name == "action_call":
        return node_name[:4] + 'a' + node_name[4:]
    else:
        raise P4RuntimeReconfigError("Invalid human readable name: human readable name contains type label [{}] which can't be interpreted".format(type_name))

def display_graph_in_command_line(graph: nx.MultiDiGraph):
    # refer to https://stackoverflow.com/questions/51273890/how-to-convert-from-networkx-graph-to-ete3-tree-object
    root = " [root] "
    subtrees = {graph_name_to_human_readable_name(node):ete3.Tree(name=graph_name_to_human_readable_name(node)) for node in graph.nodes()}
    [*map(lambda edge:subtrees[graph_name_to_human_readable_name(edge[0])].add_child(subtrees[graph_name_to_human_readable_name(edge[1])]), graph.edges())]
    tree = subtrees[root]
    print(tree.get_ascii())

def display_graph_using_matplotlib(graph: nx.MultiDiGraph):
    pos = nx.spring_layout(graph)
    nx.draw(graph, pos, node_size=1500, with_labels=True)
    plt.draw()
    plt.show()
    # plt.savefig("1.pdf")



