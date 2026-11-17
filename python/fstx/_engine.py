"""The one lock around the fstx engine.

The engine has a single worker pool that runs one job at a time (a second job fails with "worker pool is
already running a job"). Every call that can use the pool takes ``LOCK``: a write, opening a read stream,
pulling a batch from it, a filter. The lock is held with the GIL released inside the engine, so other Python
threads keep running; they only take turns at the engine. It is re-entrant: an abandoned batch reader that
is collected while its thread already holds the lock must be able to release its stream.
"""
import threading

LOCK = threading.RLock()
