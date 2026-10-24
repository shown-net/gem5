# Ordinary-load research timing

`ordinary_load_service.hh` owns clock-local admission and address arbitration.
`ordinary_load_timing.cc` implements LSQUnit's eligibility predicate and its
request/result events. The native LSQ retains ownership of requests, packets,
retry state, squash and drain. Pending events keep their requests alive and
participate in drain; they do not create duplicate cache requests or early
dependent wakeups.

The candidate covers ordinary, non-split, cacheable 8-byte integer reads.
Atomic, floating/vector, HTM, strict-order, local, LL/SC and locked accesses
continue on the native path. Store-to-load forwarding bypasses the cache
service path and is not calibrated by this mechanism.

`ordinaryLoadService=false` and `ordinaryLoadMinLatency=0` retain baseline
execution timing. Service admission, arbitration and propagation are separate
ablations. The historical minimum-latency intervention is mutually exclusive
with service. All parameters use BaseO3CPU and the normal project cfg/variant
overlay. Arbitration is an XS-inspired research approximation, not a recovered
SPR bank topology. Experiments and acceptance remain in cpu_microarchitecture.
