# ProsperoSunshine issue 56 diagnostic extracts

PS5 Pro firmware 10.20, official 0.6.6 debug (6da55c0c), November 14, 2026. This incident was not on test-0.6.6-i56-1.

01: observed capture history from recorder reconnection 13:21; not a complete boot log. 02: final successful teardown. 03: failure and host restart. 04: clean system reboot and recovery. Remaining files: retained host logs.

Filtered diagnostic lines with identifying fields masked; unrelated platform chatter omitted. Kernel recorder timestamps use UTC-3. Host wall clock differs by about 12 seconds at first failure: do not subtract across clocks. Counts include probes, not just streams.
