proc test_psync2 {mdl sdls sdll} {
start_server {tags {"psync2"}} {
start_server {} {
start_server {} {
start_server {} {
start_server {} {
    set master_id 0                 ; # Current master
    set start_time [clock seconds]  ; # Test start time
    set counter_value 0             ; # Current value of the Redis counter "x"

    # Config
    set debug_msg 0                 ; # Enable additional debug messages

    set no_exit 0                   ; # Do not exit at end of the test

    set duration 20                 ; # Total test seconds

    set genload 1                   ; # Load master with writes at every cycle

    set genload_time 5000           ; # Writes duration time in ms

    set disconnect 1                ; # Break replication link between random
                                      # master and slave instances while the
                                      # master is loaded with writes.

    set disconnect_period 1000      ; # Disconnect repl link every N ms.

    for {set j 0} {$j < 5} {incr j} {
        set R($j) [srv [expr 0-$j] client]
        set R_host($j) [srv [expr 0-$j] host]
        set R_port($j) [srv [expr 0-$j] port]
        if {$debug_msg} {puts "Log file: [srv [expr 0-$j] stdout]"}
    }

    test "PSYNC2: ### SETTING diskless master: $mdl; diskless slave (sync, load): $sdls, $sdll ###" {
    }

    set cycle 1
    while {([clock seconds]-$start_time) < $duration} {
        test "PSYNC2: --- CYCLE $cycle ---" {
            incr cycle
        }

        # Create a random replication layout.
        # Start with switching master (this simulates a failover).

        # 1) Select the new master.
        set master_id [randomInt 5]
        set used [list $master_id]
        test "PSYNC2: \[NEW LAYOUT\] Set #$master_id as master" {
            $R($master_id) slaveof no one
            $R($master_id) config set repl-diskless-sync $mdl
            $R($master_id) config set repl-diskless-sync-delay 1
            if {$counter_value == 0} {
                $R($master_id) set x $counter_value
            }
        }

        # 2) Attach all the slaves to a random instance
        while {[llength $used] != 5} {
            while 1 {
                set slave_id [randomInt 5]
                if {[lsearch -exact $used $slave_id] == -1} break
            }
            set rand [randomInt [llength $used]]
            set mid [lindex $used $rand]
            set master_host $R_host($mid)
            set master_port $R_port($mid)

            test "PSYNC2: Set #$slave_id to replicate from #$mid" {
                $R($slave_id) config set repl-diskless-load $sdll
                $R($slave_id) config set repl-diskless-sync $sdls
                $R($slave_id) config set repl-diskless-sync-delay 1
                $R($slave_id) slaveof $master_host $master_port
            }
            lappend used $slave_id
        }

        # 3) Increment the counter and wait for all the instances
        # to converge.
        test "PSYNC2: cluster is consistent after failover" {
            $R($master_id) incr x; incr counter_value
            for {set j 0} {$j < 5} {incr j} {
                wait_for_condition 50 1000 {
                    [$R($j) get x] == $counter_value
                } else {
                    fail "Instance #$j x variable is inconsistent"
                }
            }
        }

        # 4) Generate load while breaking the connection of random
        # slave-master pairs.
        test "PSYNC2: generate load while killing replication links" {
            set t [clock milliseconds]
            set next_break [expr {$t+$disconnect_period}]
            while {[clock milliseconds]-$t < $genload_time} {
                if {$genload} {
                    $R($master_id) incr x; incr counter_value
                }
                if {[clock milliseconds] == $next_break} {
                    set next_break \
                        [expr {[clock milliseconds]+$disconnect_period}]
                    set slave_id [randomInt 5]
                    if {$disconnect} {
                        $R($slave_id) client kill type master
                        if {$debug_msg} {
                            puts "+++ Breaking link for slave #$slave_id"
                        }
                    }
                }
            }
        }

        # 5) Increment the counter and wait for all the instances
        set x [$R($master_id) get x]
        test "PSYNC2: cluster is consistent after load (x = $x)" {
            for {set j 0} {$j < 5} {incr j} {
                wait_for_condition 50 1000 {
                    [$R($j) get x] == $counter_value
                } else {
                    fail "Instance #$j x variable is inconsistent"
                }
            }
        }

        # Put down the old master so that it cannot generate more
        # replication stream, this way in the next master switch, the time at
        # which we move slaves away is not important, each will have full
        # history (otherwise PINGs will make certain slaves have more history),
        # and sometimes a full resync will be needed.
        $R($master_id) slaveof 127.0.0.1 0 ;# We use port zero to make it fail.

        if {$debug_msg} {
            for {set j 0} {$j < 5} {incr j} {
                puts "$j: sync_full: [status $R($j) sync_full]"
                puts "$j: id1      : [status $R($j) master_replid]:[status $R($j) master_repl_offset]"
                puts "$j: id2      : [status $R($j) master_replid2]:[status $R($j) second_repl_offset]"
                puts "$j: backlog  : firstbyte=[status $R($j) repl_backlog_first_byte_offset] len=[status $R($j) repl_backlog_histlen]"
                puts "---"
            }
        }

        test "PSYNC2: total sum of full synchronizations is exactly 4" {
            set sum 0
            for {set j 0} {$j < 5} {incr j} {
                incr sum [status $R($j) sync_full]
            }
            assert {$sum == 4}
        }
    }

    test "PSYNC2: Bring the master back again for next test" {
        $R($master_id) slaveof no one
        set master_host $R_host($master_id)
        set master_port $R_port($master_id)
        for {set j 0} {$j < 5} {incr j} {
            if {$j == $master_id} continue
            $R($j) slaveof $master_host $master_port
        }

        # Wait for slaves to sync
        wait_for_condition 50 1000 {
            [status $R($master_id) connected_slaves] == 4
        } else {
            fail "Slave not reconnecting"
        }
    }

    test "PSYNC2: Partial resync after restart using RDB aux fields" {
        # Pick a random slave
        set slave_id [expr {($master_id+1)%5}]
        set sync_count [status $R($master_id) sync_full]
        catch {
            $R($slave_id) config rewrite
            $R($slave_id) debug restart
        }
        wait_for_condition 50 1000 {
            [status $R($master_id) connected_slaves] == 4
        } else {
            fail "Slave not reconnecting"
        }
        set new_sync_count [status $R($master_id) sync_full]
        assert {$sync_count == $new_sync_count}
    }

    test "PSYNC2: Slave RDB restart with EVALSHA in backlog issue #4483" {
        # Pick a random slave
        set slave_id [expr {($master_id+1)%5}]
        set sync_count [status $R($master_id) sync_full]

        # Make sure to replicate the first EVAL while the salve is online
        # so that it's part of the scripts the master believes it's safe
        # to propagate as EVALSHA.
        $R($master_id) EVAL {return redis.call("incr","__mycounter")} 0
        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0

        # Wait for the two to sync
        wait_for_condition 50 1000 {
            [$R($master_id) debug digest] == [$R($slave_id) debug digest]
        } else {
            fail "Slave not reconnecting"
        }

        # Prevent the slave from receiving master updates, and at
        # the same time send a new script several times to the
        # master, so that we'll end with EVALSHA into the backlog.
        $R($slave_id) slaveof 127.0.0.1 0

        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0
        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0
        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0

        catch {
            $R($slave_id) config rewrite
            $R($slave_id) debug restart
        }

        # Reconfigure the slave correctly again, when it's back online.
        set retry 50
        while {$retry} {
            if {[catch {
                $R($slave_id) slaveof $master_host $master_port
            }]} {
                after 1000
            } else {
                break
            }
            incr retry -1
        }

        # The master should be back at 4 slaves eventually
        wait_for_condition 50 1000 {
            [status $R($master_id) connected_slaves] == 4
        } else {
            fail "Slave not reconnecting"
        }
        set new_sync_count [status $R($master_id) sync_full]
        assert {$sync_count == $new_sync_count}

        # However if the slave started with the full state of the
        # scripting engine, we should now have the same digest.
        wait_for_condition 50 1000 {
            [$R($master_id) debug digest] == [$R($slave_id) debug digest]
        } else {
            fail "Debug digest mismatch between master and slave in post-restart handshake"
        }
    }

    if {$no_exit} {
        while 1 { puts -nonewline .; flush stdout; after 1000}
    }

}}}}}
}

foreach mdl {yes no} {
    foreach sdls {yes no} {
        foreach sdll {disabled swapdb} {
            test_psync2 $mdl $sdls $sdll
        }
    }
}
