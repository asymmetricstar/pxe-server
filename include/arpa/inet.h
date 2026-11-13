/*
 * Minimal arpa/inet.h for Windows (Winsock2)
 */
#ifndef ARPA_INET_H
#define ARPA_INET_H

#include <winsock2.h>
#include <ws2tcpip.h>

/* inet_addr, inet_ntoa, inet_pton, inet_ntop are provided by Winsock2 */
/* Use them directly - no wrapper needed */

#endif
