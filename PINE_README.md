
## SAVESTATES:


loadstate algo: ~~done~~
(target frame = the frame we want to rollback to)
Scan forward from target to current, collecting every page that was dirtied anywhere in that range.
For each unique dirty page, search backward from target toward the oldest slot to find the most recent
saved state at or before the target frame - that slot has the correct data for that page at the target frame.
If no save slot has it, use the base snapshot (a full RAM image kept current by absorbing evicted slots).
Then copy all those pages into game RAM.
Here's an ai ascii visual of this process:
```
  base       |<- pre-window ->|<------------- rollback window ------------->|
  snapshot   [slot] ...[slot]  [TARGET] [slot] [slot] ... [slot] [CURRENT]
                  |                |                                    |
                  |                +------ scan forward for dirty pages-+-->    <- takes the "first" (oldest dirty page) it finds, newer ones are ignored
                  |                |
                  +<--- search backward for each page's source ---------+       <- stops on first hit ("most recent at or before target frame")
                  |
           first hit = source   (no hit = use base snapshot)
```


## GAME SIM

Need to time the core game sim loop. If it's as slow as i remember, need to really profile it properly
probably through jit instrumentation & the existing jit profiler, but have it emit profiling events that i can symbolicate offline somehow (chrome tracing format?)
Tried implementing a JIT Tracer. Current bookmark is symbols aren't symbolicating and the chrome tracing events were formatted a little weirdly.

it's fast! like 1ms or less!

2 things to optimize:
- skip the NW4R EffectSystem::Calc during resim frames
- skip havok secondary physics (ph2ndaryWorld, it's all the cloth sim) by toggling its enable flag during resim
