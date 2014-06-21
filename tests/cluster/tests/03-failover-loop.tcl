# Failover stress test.
# In this test a different node is killed in a loop for N
# iterations. The test checks that certain properties
# are preseved across iterations.

source "../tests/includes/init-tests.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster is up" {
    assert_cluster_state ok
}

set iterations 20
set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

while {[incr iterations -1]} {
    set tokill [randomInt 10]
    set other [expr {($tokill+1)%10}] ; # Some other instance.
    set key [randstring 20 20 alpha]
    set val [randstring 20 20 alpha]
    set role [RI $tokill role]

    set current_epoch [CI $other cluster_current_epoch]

    puts "--- Iteration $iterations ---"

    if {$role eq {master}} {
        test "Wait for slave of #$tokill to sync" {
            wait_for_condition 1000 50 {
                [string match {*state=online*} [RI $tokill slave0]]
            } else {
                fail "Slave of node #$tokill is not ok"
            }
        }
    }

    test "Killing node #$tokill" {
        kill_instance redis $tokill
    }

    if {$role eq {master}} {
        test "Wait failover" {
            wait_for_condition 1000 50 {
                [CI $other cluster_current_epoch] > $current_epoch
            } else {
                fail "No failover detected"
            }
        }
    }

    test "Cluster should eventually be up again" {
        assert_cluster_state ok
    }

    test "Cluster is writable" {
        catch {$cluster set $key $val} err
        assert {$err eq {OK}}
    }

    test "Restarting node #$tokill" {
        restart_instance redis $tokill
    }
    
    test "Instance #$tokill is now a slave" {
        wait_for_condition 1000 50 {
            [RI $tokill role] eq {slave}
        } else {
            fail "Restarted instance is not a slave"
        }
    }

    test "We can read back the value we set before" {
        catch {$cluster get $key} err
        assert {$err eq $val}
    }
}
