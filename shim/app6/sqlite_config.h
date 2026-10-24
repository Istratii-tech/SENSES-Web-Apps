// Included after the build's forced includes, for external/sqlite only:
// AndroidConfig.h defines HAVE_MALLOC_H empty, and sqlite3.c tests it with
// #if (sqlite's own convention is 0/1).
#undef HAVE_MALLOC_H
#define HAVE_MALLOC_H 1
#define HAVE_MALLOC_USABLE_SIZE 1
