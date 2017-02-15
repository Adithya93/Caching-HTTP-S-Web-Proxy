# To Run:
`make` followed by `./threadPool`

# General Architecture

Our proxy server is multi-threaded with an in-memory cache. Each thread services requests off a global stack protected by mutexes.

Logging is performed by a single logging thread that pops log write requests off a queue.
