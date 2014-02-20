test "Sentinels aren't monitoring any master" {
    foreach_sentinel_id id {
        assert {[S $id sentinel masters] eq {}}
    }
}

set redis_slaves 4
test "Create a master-slaves cluster of [expr $redis_slaves+1] instances" {
    create_redis_master_slave_cluster [expr {$redis_slaves+1}]
}
set master_id 0

test "Sentinels can start monitoring a master" {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR mymaster \
              [get_instance_attrib redis $master_id host] \
              [get_instance_attrib redis $master_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
    }
}

test "Sentinels are able to auto-discover other sentinels" {
    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "At least some sentinel can't detect some other sentinel"
        }
    }
}

test "Sentinels are able to auto-discover slaves" {
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-slaves] == $redis_slaves
        } else {
            fail "At least some sentinel can't detect some slave"
        }
    }
}

test "Can change master parameters via SENTINEL SET" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster down-after-milliseconds 2000
    }
    foreach_sentinel_id id {
        assert {[dict get [S $id sentinel master mymaster] down-after-milliseconds] == 2000}
    }
}

test "Basic failover works if the master is down" {
    set old_port [RI $master_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    R $master_id debug sleep 5
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not received failover info"
        }
    }
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}
