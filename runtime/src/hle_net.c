/* ws2_32 (offline): byte-order helpers and address text conversions work; everything that would open a socket or
 * resolve a name answers "network down", so the game's online code takes its offline path.
 * oleaut32 BSTRs live here too (SysAllocString / SysFreeString).
 * What it does not do: networking of any kind (online play is out of scope for now).
 */
#include <string.h>
#include <time.h>

#include "hle.h"

#define WSAENETDOWN 10050u
static uint32_t wsa_error = WSAENETDOWN;

uint32_t x_htonl(CPU *c, uint32_t a) { (void)c; return __builtin_bswap32(ARG(0)); }
uint32_t x_ntohl(CPU *c, uint32_t a) { (void)c; return __builtin_bswap32(ARG(0)); }
uint32_t x_htons(CPU *c, uint32_t a) { (void)c; return __builtin_bswap16((uint16_t)ARG(0)); }
uint32_t x_ntohs(CPU *c, uint32_t a) { (void)c; return __builtin_bswap16((uint16_t)ARG(0)); }

uint32_t x_inet_addr(CPU *c, uint32_t a) {
    (void)c;
    unsigned b[4];
    if (sscanf(STR(ARG(0)), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) return 0xFFFFFFFFu;
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}
uint32_t x_inet_ntoa(CPU *c, uint32_t a) {
    (void)c;
    static uint32_t buf;
    if (!buf) buf = gmalloc(20);
    uint32_t v = ARG(0);
    snprintf((char *)HOST(buf), 20, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    return buf;
}
uint32_t x_WSAStartup(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    if (p) { memset(HOST(p), 0, 400); W16(p, 0x0202); W16(p + 2, 0x0202); strcpy((char *)HOST(p + 4), "RaymanOrigins-PC offline"); }
    return 0;
}
uint32_t x_WSACleanup(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_WSAGetLastError(CPU *c, uint32_t a) { (void)c; (void)a; return wsa_error; }
uint32_t x_WSASetLastError(CPU *c, uint32_t a) { (void)c; wsa_error = ARG(0); return 0; }
uint32_t x_socket(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = WSAENETDOWN; return 0xFFFFFFFFu; }
uint32_t x_gethostbyname(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = 11001u; return 0; }   /* WSAHOST_NOT_FOUND */
uint32_t x_gethostbyaddr(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = 11001u; return 0; }
uint32_t x_getservbyname(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_getservbyport(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_ioctlsocket(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = 10038u; return 0xFFFFFFFFu; }   /* WSAENOTSOCK */
uint32_t x_recv(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = 10038u; return 0xFFFFFFFFu; }
uint32_t x_sendto(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = 10038u; return 0xFFFFFFFFu; }

/* BSTR: a length prefix (bytes) before the UTF-16 text; the pointer is to the text */
uint32_t x_SysAllocString(CPU *c, uint32_t a) {
    (void)c;
    if (!ARG(0)) return 0;
    uint32_t n = (uint32_t)wlen(ARG(0));
    uint32_t p = gmalloc(4 + 2 * n + 2);
    W32(p, 2 * n);
    memcpy(HOST(p + 4), HOST(ARG(0)), 2 * n + 2);
    return p + 4;
}
uint32_t x_SysFreeString(CPU *c, uint32_t a) { (void)c; if (ARG(0)) gfree(ARG(0) - 4); return 0; }

/* ---- added for Rayman Origins (Quazal online layer): events work, every socket call fails "network down" ---- */
#define NETFAIL(name) uint32_t x_##name(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = WSAENETDOWN; return 0xFFFFFFFFu; }
uint32_t x_WSACreateEvent(CPU *c, uint32_t a) { (void)c; (void)a; static uint32_t n; return 0x7E100000u + 4u * ++n; }
uint32_t x_WSAResetEvent(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t x_WSACloseEvent(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t x_WSAWaitForMultipleEvents(CPU *c, uint32_t a) {        /* nothing ever arrives: wait the timeout out */
    (void)c;
    uint32_t ms = ARG(3);
    if (ms && ms != 0xFFFFFFFFu) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
    else if (ms == 0xFFFFFFFFu) { struct timespec t = {0, 100000000L}; nanosleep(&t, NULL); }
    return 0x102u;                                                 /* WSA_WAIT_TIMEOUT */
}
uint32_t x_gethostname(CPU *c, uint32_t a) { (void)c; if (ARG(1) > 7) strcpy((char *)HOST(ARG(0)), "rayman"); return 0; }
uint32_t x_getaddrinfo(CPU *c, uint32_t a) { (void)c; (void)a; return 11001u; }
uint32_t x_freeaddrinfo(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x___WSAFDIsSet(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_WSAGetOverlappedResult(CPU *c, uint32_t a) { (void)c; (void)a; wsa_error = WSAENETDOWN; return 0; }
NETFAIL(WSAIoctl) NETFAIL(recvfrom) NETFAIL(WSARecvFrom) NETFAIL(WSASendTo) NETFAIL(WSASocketA) NETFAIL(getsockname)
NETFAIL(closesocket) NETFAIL(select) NETFAIL(connect) NETFAIL(shutdown) NETFAIL(getpeername) NETFAIL(getsockopt)
NETFAIL(bind) NETFAIL(listen) NETFAIL(send) NETFAIL(setsockopt) NETFAIL(accept)
