
## SAVESTATES:


- optimize! Low stress - low hanging fruit optimization, now that it's proven that they are fast enough.
Loading:
 crunch down all the deltas to remove "duplicates" - i.e. if frame 0 and frame 1 both touched
     page X, only the oldest delta (frame 0) needs to be applied to restore the target frame;
     applying frame 1's delta would just write the same data back into page X and waste time.
 overlap the applydeltas work with gappagerestore work
RestoreNonDeltaState can also probably be overlapped with the above memcpy work?

loadstate algo:
for all pages that were written to within [target frame, current frame] rollback window (iterating from current -> target)
  find the "source data" slot that gives us the correct "version" of that page on "target frame"
    How to do that: find the oldest save slot BEFORE the slot this page was written, we can use that slot's page as the "source"
                    if there is no older slot, we use the base snapshot
this returns a mapping (page index, savestate slot that contains the "correct data" for target frame)
We then apply that mapping - copying the savestate slot with "correct data" for the given page index into game ram



## GAME SIM

Need to time the core game sim loop. If it's as slow as i remember, need to really profile it properly
probably through jit instrumentation & the existing jit profiler, but have it emit profiling events that i can symbolicate offline somehow (chrome tracing format?)
Tried implementing a JIT Tracer. Current bookmark is symbols aren't symbolicating and the chrome tracing events were formatted a little weirdly.

it's fast! like 1ms or less!

2 things to optimize:
- skip the NW4R EffectSystem::Calc during resim frames
- skip havok secondary physics (ph2ndaryWorld, it's all the cloth sim) by toggling its enable flag during resim
