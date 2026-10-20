# Provisioning network checks

Run `bash projects/mybot/tests/provision_network/run.sh`. For ASAN/UBSAN use
`ASAN_OPTIONS=detect_leaks=0 PROVISION_NETWORK_TEST_SANITIZERS=1 bash projects/mybot/tests/provision_network/run.sh`.

The harness compiles the production CAS mailbox, Wi-Fi manager, HTTP portal and
pinned AVDK JSON parser. Vendor APIs, sockets and the single Wi-Fi storage record
are fault-injected. Twelve groups cover 256 real cancel/commit thread races, request
priority/coalescing, saved-only recovery, old credentials until candidate IPv4 and
commit, late-cancel rejection, save failure reopening cancellation, automatic and
explicit portals, scan/connection/HTTP cancellation, factory precedence and driver
start/stop failures with retained cleanup ownership. Storage mocks reject every
write outside `mybot.wifi.v1`, protecting device authentication and volume scope.
Write faults include errors after complete replacement, partial/hidden old nodes,
false success, verified restoration of the old record or absence, and rollback
failure that closes the portal while keeping late cancellation disabled.

Host checks do not validate radio, RTOS scheduling or flash power-loss behavior;
those require device testing.
