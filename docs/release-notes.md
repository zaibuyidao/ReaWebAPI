# ReaWebAPI

- Schedule video capture independently on the main thread across platforms, following distinct host frames in auto mode and reduce Windows video transport delays with high-resolution waits.
- Capture unseen selected host frames before advancing video, skip repeated conversion, and avoid clearing the last image after brief scheduler pauses.
- Preserve project video frame order through bounded capture, transport and demo rendering queues to absorb brief delivery delays and clear stale queued frames on video invalidation.
- Reuse video transport buffers and reduce BGRA-to-RGBA conversion, scaling and copy overhead while preserving pixel output and letterboxing.
- Support automatic and fixed fractional video update rates from 1 to 75 FPS.
