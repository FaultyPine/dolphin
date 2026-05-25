
## MODDING

Need to get the modding workflow back in

## SAVESTATES:

BOOKMARK: optimize! Low stress - low hanging fruit optimization, now that it's proven that they are fast enough.
Both: proper fan-out memcpy and NT wide stores
Loading:
 crunch down all the deltas to remove "duplicates" - i.e. if frame 0 and frame 1 both touched
     page X, only the oldest delta (frame 0) needs to be applied to restore the target frame;
     applying frame 1's delta would just write the same data back into page X and waste time.
 overlap the applydeltas work with gappagerestore work
RestoreNonDeltaState can also probably be overlapped with the above memcpy work?
Saving:
 use threadpool for the async eviction job, instead of launching a new thread every time


## GAME SIM

Need to time the core game sim loop. If it's as slow as i remember, need to really profile it properly
probably through jit instrumentation & the existing jit profiler, but have it emit profiling events that i can symbolicate offline somehow (chrome tracing format?)

## ROLLBACK

Need to port my old rollback logic in here, or reimplement it myself
