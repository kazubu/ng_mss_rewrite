#!/bin/sh
#
# Common helper functions for ng_mss_rewrite tests
#
# This library provides polling functions to replace sleep statements
# for more reliable and faster test execution.
#

# Poll for a node to exist
# Usage: wait_for_node <node_name> [timeout_seconds]
# Returns: 0 on success, 1 on timeout
wait_for_node() {
	local node_name=$1
	local timeout=${2:-5}
	local elapsed=0
	local interval=0.1

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		if ngctl show "$node_name" >/dev/null 2>&1; then
			return 0
		fi
		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for multiple nodes to exist
# Usage: wait_for_nodes <timeout_seconds> <node1> <node2> ...
# Returns: 0 if all nodes exist, 1 on timeout
wait_for_nodes() {
	local timeout=$1
	shift
	local elapsed=0
	local interval=0.1
	local all_exist

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		all_exist=1
		for node in "$@"; do
			if ! ngctl show "$node" >/dev/null 2>&1; then
				all_exist=0
				break
			fi
		done

		if [ $all_exist -eq 1 ]; then
			return 0
		fi

		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for a statistics counter to change
# Usage: wait_for_counter_change <node_name> <counter_name> <initial_value> [timeout_seconds]
# Returns: 0 on success, 1 on timeout
wait_for_counter_change() {
	local node_name=$1
	local counter_name=$2
	local initial_value=$3
	local timeout=${4:-2}
	local elapsed=0
	local interval=0.05
	local current_value

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		current_value=$(ngctl msg "$node_name" getstats 2>&1 | grep -o "${counter_name}=[0-9]*" | cut -d= -f2)

		if [ -n "$current_value" ] && [ "$current_value" != "$initial_value" ]; then
			return 0
		fi

		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for packets_sent counter to increment
# Usage: wait_for_packet_sent <node_name> <sent_before> [timeout_seconds]
# Returns: 0 on success, 1 on timeout
wait_for_packet_sent() {
	local node_name=$1
	local sent_before=$2
	local timeout=${3:-1}
	local elapsed=0
	local interval=0.05
	local sent_after

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		sent_after=$(ngctl msg "$node_name" getstats 2>&1 | grep -o 'packets_sent=[0-9]*' | cut -d= -f2)
		[ -z "$sent_after" ] && sent_after=0

		if [ $sent_after -gt $sent_before ]; then
			return 0
		fi

		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for packets_processed counter to reach a specific value
# Usage: wait_for_packets_processed <node_name> <expected_count> [timeout_seconds]
# Returns: 0 on success, 1 on timeout
wait_for_packets_processed() {
	local node_name=$1
	local expected_count=$2
	local timeout=${3:-2}
	local elapsed=0
	local interval=0.05
	local processed

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		processed=$(ngctl msg "$node_name" getstats 2>&1 | grep -o 'packets_processed=[0-9]*' | cut -d= -f2)
		[ -z "$processed" ] && processed=0

		if [ $processed -ge $expected_count ]; then
			return 0
		fi

		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for a process to start (PID becomes valid)
# Usage: wait_for_process <pid> [timeout_seconds]
# Returns: 0 if process is running, 1 on timeout or death
wait_for_process() {
	local pid=$1
	local timeout=${2:-5}
	local elapsed=0
	local interval=0.1

	# Wait a bit for process to actually start
	sleep 0.2

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		if kill -0 $pid 2>/dev/null; then
			return 0
		fi
		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for module to be loaded
# Usage: wait_for_module <module_name> [timeout_seconds]
# Returns: 0 on success, 1 on timeout
wait_for_module() {
	local module_name=$1
	local timeout=${2:-5}
	local elapsed=0
	local interval=0.1

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		if kldstat | grep -q "$module_name"; then
			return 0
		fi
		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}

# Poll for module to be unloaded
# Usage: wait_for_module_unload <module_name> [timeout_seconds]
# Returns: 0 on success, 1 on timeout
wait_for_module_unload() {
	local module_name=$1
	local timeout=${2:-5}
	local elapsed=0
	local interval=0.1

	while [ $(echo "$elapsed < $timeout" | bc) -eq 1 ]; do
		if ! kldstat | grep -q "$module_name"; then
			return 0
		fi
		sleep $interval
		elapsed=$(echo "$elapsed + $interval" | bc)
	done

	return 1
}
