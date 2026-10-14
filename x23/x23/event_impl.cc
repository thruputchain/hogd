
#include "x23/config.h"

#if USE_EPOLL
#include "event_impl_epoll.cc"
#else // !USE_EPOLL
#include "event_impl_poll.cc"
#endif // !USE_EPOLL
