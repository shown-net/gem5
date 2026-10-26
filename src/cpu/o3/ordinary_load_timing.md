# Ordinary-load research timing

`ordinary_load_service.hh` owns clock-local admission and address arbitration.
`ordinary_load_timing.cc` implements LSQUnit's eligibility predicate and its
request/result events. The native LSQ retains ownership of requests, packets,
retry state, squash and drain. Pending events keep their requests alive and
participate in drain; they do not create duplicate cache requests or early
dependent wakeups.

One event per LSQUnit arbitrates its translated ready requests in instruction
age order. Each queued pointer is the existing LSQRequest, retained by its
service lifetime flag; acceptance or squash releases that flag. Capacity and
cache retries remain queued for the next clock edge. This removes per-request
equal-priority EventQueue LIFO ordering from LSU resource arbitration. Younger
requests may use slots that an older read-window-conflicted request
cannot use; they do not displace an older request that can use the same slot.

The candidate covers ordinary, non-split, cacheable 8-byte integer reads.
Atomic, floating/vector, HTM, strict-order, local, LL/SC and locked accesses
continue on the native path. Store-to-load forwarding bypasses the cache
service path and is not calibrated by this mechanism.

`ordinaryLoadService=false` and `ordinaryLoadMinLatency=0` retain baseline
execution timing. Service admission, arbitration and propagation are separate
ablations. Minimum result latency can be combined with request admission and
arbitration when clocked propagation is disabled; it remains mutually exclusive
with clocked propagation. All parameters use BaseO3CPU and the normal project cfg/variant
overlay. Arbitration counts each real load against concurrent line-offset
read windows, including repeated reads of identical bytes. The default two
read slots are a candidate inferred from SPR address-layout responses;
`loadServiceBankPorts=1` is a resource-dose intervention. An unaligned qword
consumes both overlapping eight-byte read windows. Their granularity is a
candidate inferred from layout responses, not a physical SPR bank topology.
The former set-key read reuse was removed after
same-address repeated loads remained limited to about two per cycle.
The blanket two-read limit for unaligned qwords was rejected by an independent
rotated unaligned layout that sustains about three loads per cycle. Disabling
admission removes its global three-read limit while preserving arbitration.
Experiments and acceptance remain in cpu_microarchitecture.
