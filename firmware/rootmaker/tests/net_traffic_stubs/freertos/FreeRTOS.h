#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void net_traffic_test_enter(portMUX_TYPE *mux);
void net_traffic_test_exit(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) net_traffic_test_enter(mux)
#define portEXIT_CRITICAL(mux) net_traffic_test_exit(mux)
