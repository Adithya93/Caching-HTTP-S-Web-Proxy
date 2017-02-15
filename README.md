# To Run:
`make` followed by `./threadPool`

# General Architecture

Our proxy server is multi-threaded with an in-memory cache. Each thread services requests off a global stack protected by mutexes. See `threadPool.c`.

Logging is performed by a single logging thread that pops log write requests off a queue. See `logging.c`.

Our cache is a map of URIs to structs containing info about the resource. See `cache.c`.

Time parsing is handled in `timeparse.c`.
