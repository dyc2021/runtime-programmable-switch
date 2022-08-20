control AclFunc(inout headers hdr,
                inout metadata meta,
                inout standard_metadata_t standard_metadata) {
    action deny() {
        mark_to_drop(standard_metadata);
    }

    action allow() { }

    table acl {
        key = {
            standard_metadata.ingress_port: exact;
            hdr.ipv4.srcAddr: exact;
            hdr.ipv4.dstAddr: lpm;
            hdr.tcp.srcPort: exact;
            hdr.tcp.dstPort: exact;
        }
        actions = {
            deny;
            allow;
        }
        size = 1024;
        default_action = deny();
    }

    apply {
        if (hdr.tcp.isValid()) {
            acl.apply();
        }
    }
}