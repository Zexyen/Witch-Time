/* Included by nnnet.c (round "close4", helper A): nn::nifm leftovers, nn::socket / nnsocket* (bsd + resolver) on
 * Winsock, nn::ssl / nnssl*, nn::nsd::ResolveEx, nn::ldn, nn::ec::ShopServiceAccessor.
 *
 * Sources: [sdk] = binaries\13.0.4\sdk.elf (addresses quoted), [suyu] = reference\suyu\src\core (file named).
 * Console state modelled: no network connection (the nifm network request is never accepted), wireless
 * communication not available (a PC has no local-wireless radio), one local user.
 *
 * ------------------------------------------------------------------ nn::nifm
 * [sdk 0x3218f0] detail::GetGeneralServicePointer and [sdk 0x321990] GetNetworkConnectionPointer return 0xca6e
 *     (module 110, description 101) while the library is not initialised (byte .bss 0xcc9910 != 1).
 * [sdk 0x322900] GetCurrentPrimaryIpAddress(in_addr *): null pointer -> abort; IGeneralService command at vtable
 *     +0x70 (GetCurrentIpAddress); 4 bytes copied out on success.
 * [sdk 0x322ae0] GetCurrentIpConfigInfo(ip, mask, gateway, dns1, dns2): any null -> abort; command at +0x88;
 *     the five addresses are bytes 1..12 of the IpAddressSetting and 1..8 of the DnsSetting.
 * [sdk 0x323620] GetCurrentProxySetting(ProxySetting *): GetCurrentNetworkProfile ([sdk 0x322e60], command +0x38),
 *     converted by [sdk 0x324950] (plain copies), then 0xaa bytes at +0xe2 of the converted profile are copied out.
 *     The module's answers are the reference's ([suyu] hle/service/nifm/nifm.cpp) for a host that is not connected:
 *     GetCurrentIpAddress = success, 0.0.0.0; GetCurrentIpConfigInfo = success, all zero; GetCurrentNetworkProfile
 *     = success, a zero profile (so the proxy setting is 0xaa zero bytes: proxy disabled).
 * [sdk 0x322d90] HandleNetworkRequestErrorResult(): false when not initialised; otherwise [sdk 0x324550]
 *     NetworkConnection::HandleNetworkRequestErrorResult -> [sdk 0x31cbc0] RequestClient::PrepareLibraryApplet asks
 *     the request object for the applet to show (IRequest command at +0xc0, GetAppletInfo). [suyu] answers success
 *     with applet id 0 and an empty parameter block; with an empty block the SDK returns 0x1906e (110-200) and the
 *     function gives false. A console shows the system's network-error applet here; that applet is not modelled.
 *
 * ------------------------------------------------------------------ nn::socket (bsd:u) and the resolver (sfdnsres)
 * [sdk] every nn::socket::X is a jump to nn::socket::detail::X and every nnsocketX a jump to nn::socket::X
 *     (0x36ac.. / 0x36b5.., 4 bytes each; Fcntl copies its one variadic int, 0x36afa0 / 0x36b6a0).
 * [sdk 0x36f8f0 / 0x36f5d0] Initialize(pool, poolSize, allocatorPoolSize, concurrency): aborts when already
 *     initialised, pool null or not page aligned, sizes not page aligned, allocator pool < 0x1000, pool <= allocator
 *     pool, concurrency not 1..14; pool < 0xa0000 -> Result 0x21b (module 27, description 1); else registers with
 *     bsd:u (works without a connection), enables the resolver, 0.
 * [sdk 0x36f980] Finalize: abort when not initialised; the session is closed (the module closes the sockets), 0.
 * [sdk 0x36fa60 / 0x36fac0] errno IS the C library's errno (__errno_location); SetLastErrno does nothing and
 *     GetLastErrno gives 22 while the library is not initialised.
 * [sdk 0x374b80 ...] every bsd call aborts when the library is not initialised; the module returns (value, errno)
 *     and the SDK stores errno when the value is negative. SDK-side argument checks, before the module is asked:
 *       Recv / Send / RecvFrom / SendTo  size 0 -> 0; buffer null -> -1 EINVAL(22); size >= 4 GiB -> -1 EFAULT(14)
 *       RecvFrom with no address / null or zero length -> Recv; SendTo with no address / zero length -> Send
 *       Bind / Connect   address null -> -1 EINVAL  (the SockAddr overload of Bind also for length 0)
 *       Accept           address null with *length != 0 -> -1 EINVAL
 *       GetSockName / GetPeerName / GetSockOpt   pointer null or *length 0 -> -1 EINVAL;  SetSockOpt value null -> EINVAL
 *       Poll             fds null with count != 0 -> -1 EINVAL; count >= 2^29 -> -1 EFAULT
 *     The typed overloads (Family, Type, Protocol, MsgFlag, Level, Option, SockAddr) pass the same numbers
 *     ([sdk 0x374a40] masks the family to 8 bits).
 * The module: a real socket layer on Winsock, following [suyu] hle/service/sockets/bsd.cpp + internal_network/
 *     network.cpp (128 descriptors from 0, AF_INET only, O_NONBLOCK 0x800 with F_GETFL 3 / F_SETFL 4, MSG_PEEK 2,
 *     MSG_DONTWAIT 0x80, SOL_SOCKET 0xffff with the BSD option numbers, BSD sockaddr_in {len, family, port, addr},
 *     pollfd {int fd; short events, revents} with POLLIN 1 PRI 2 OUT 4 ERR 8 HUP 0x10 NVAL 0x20 RDNORM 0x40 RDBAND
 *     0x80 WRBAND 0x100). errno numbers are the SDK's own C library's ([sdk] table 0x9f0bb5 + texts 0xaa8204:
 *     9 EBADF, 11 EAGAIN, 22 EINVAL, 24 EMFILE, 32 EPIPE, 88 ENOTSOCK, 90 EMSGSIZE, 92 ENOPROTOOPT, 93, 94, 95,
 *     97 EAFNOSUPPORT, 98 EADDRINUSE, 99 EADDRNOTAVAIL, 100 ENETDOWN, 101 ENETUNREACH, 103, 104, 105, 106 EISCONN,
 *     107 ENOTCONN, 108, 110 ETIMEDOUT, 111 ECONNREFUSED, 113 EHOSTUNREACH, 114 EALREADY, 115 EINPROGRESS).
 *     No connection: only the loopback network 127.0.0.0/8 exists. Connect / SendTo to any other address fail with
 *     ENETUNREACH (what a host without a network gives: Winsock WSAENETUNREACH, [suyu] network.cpp -> NETUNREACH);
 *     Bind to an address other than 0.0.0.0 / 127.x fails with EADDRNOTAVAIL; a socket bound to 0.0.0.0 is bound to
 *     the loopback interface on the PC (nothing is ever opened towards the PC's real network) and GetSockName still
 *     reports 0.0.0.0. NOT VERIFIED on a console: the exact errno of the bsd module for an unreachable network, the
 *     descriptor numbering and limit, sin_len of returned addresses (16 here), Fcntl commands other than 3 / 4
 *     (EINVAL here), options outside SOL_SOCKET / TCP_NODELAY (accepted without effect, as the reference does).
 * Resolver: [sdk 0x37fb40] GetAddrInfo: not initialised -> 11 (EAI_SYSTEM); node and service null -> 8; result
 *     pointer null -> 11 + errno 22; then the service; on any failure the result pointer is null.
 *     [sdk 0x37ef80] GetHostByName(name, options, count): not initialised -> null + errno 22; name null -> null,
 *     h_errno 1, errno 22. [sdk 0x37e9e0] GAIStrError: "Error string is not available" ([sdk] 0x9e3ef4) when not
 *     initialised, else the text comes from the module. [sdk 0x37e760 / 0x37eb30] ResolverGetOption / SetOption:
 *     the keys 0x11171..0x11176 (type 0x67, size 8) live in the SDK's own table .data 0xc0cd28 (read / written in
 *     the mapped SDK image); 0x11170 / 0x11177 -> EINVAL; key 10001 gives a random cancel handle; anything else is
 *     the module's: Get -> -1 ([suyu] sfdnsres.cpp has no such command), Set -> 0 ([suyu] ResolverSetOptionRequest).
 *     [sdk 0x36cc00 / 0x36c790] result layouts: addrinfo {flags, family, socktype, protocol, addrlen, canonname
 *     @0x18, addr @0x20, next @0x28}; nn::socket::AddrInfo {.., family u8 @4, .., addr @0x18, canonname @0x20, next};
 *     [sdk 0x36d890] hostent {name, aliases, addrtype @0x10, length @0x14, addr_list @0x18}.
 *     The module ([suyu] sfdnsres.cpp, "verified on console" tables there): a name that needs DNS cannot be resolved
 *     without a network: EAI_AGAIN (2), h_errno TRY_AGAIN (2), errno 0. A numeric IPv4 address needs no DNS and is
 *     converted (Winsock getaddrinfo with AI_NUMERICHOST, AF_INET). The error texts are the reference's
 *     (sockets_translate.cpp); they are not in sdk.elf. NOT VERIFIED: a console's answer for "localhost".
 *     Results are allocated on the host heap, not in the pool given to Initialize.
 *
 * ------------------------------------------------------------------ nn::ssl
 * [sdk 0x386720] Initialize: use count (.bss 0xcd10a8) is incremented on EVERY call; a call that finds it non-zero
 *     returns 0x1927b (module 123, description 201). [sdk 0x3867e0] Finalize: count 0 -> 0xca7b (123-101); count
 *     still > 0 after the decrement -> 0; reaching 0 closes the session -> 0.
 * [sdk 0x3832f0] Context::Create(version): no session -> 0xca7b; the object is 8 bytes: the address of an 8-byte
 *     chunk that holds the service object, with its four 16-bit pieces in reverse order. [sdk 0x3836d0] GetContextId
 *     = those 8 bytes; null out -> 0xe27b (123-113). [sdk 0x3833f0] Destroy: 0xca7b / 0xdc7b (invalid context,
 *     123-110) / 0xd67b (123-107) while connections exist. [sdk 0x3836f0] ImportServerPki(outId, data, size, format):
 *     0xca7b; null id or data -> 0xe27b; invalid context -> 0xdc7b.
 * [sdk 0x383c70 / 0x383ca0 / 0x383dd0] Connection {chunk @0, context id @8, socket @0x10 (-1), certificate buffer
 *     @0x18 + size @0x20, last Result @0x24}: Create: 0xca7b, null context -> 0xe27b, bad context -> 0xdc7b;
 *     Destroy: 0xca7b, 0xda7b (invalid connection, 123-109), closes the socket @0x10 when it is >= 0.
 *     SetSocketDescriptor / SetHostName (null -> 0xe27b) / SetVerifyOption / SetIoMode / SetSessionCacheMode /
 *     SetOption / DoHandshake / Read / Write (null -> 0xe27b, size 0 -> 0xe07b) / Pending (null -> 0xe27b) /
 *     GetVerifyCertError (null -> 0xe27b) / GetConnectionId: 0xca7b, then 0xda7b, then the module.
 * [sdk 0x385d30 ...] the nnssl* C functions return 0xe27b for a null object and call the C++ function.
 * The module ([suyu] hle/service/ssl/ssl.cpp): CreateContext, CreateConnection, ImportServerPki (id 0),
 *     SetHostName, SetVerifyOption, SetSessionCacheMode, SetOption succeed; SetSocketDescriptor duplicates the
 *     descriptor (unknown descriptor -> 0xd47b, 123-106) and hands back -1, or the duplicate when the option
 *     DoNotCloseSocket (0) is set; SetIoMode without a socket -> 0xce7b (123-103), else it sets the socket's
 *     blocking mode; DoHandshake without a socket -> 0xce7b; Pending -> 0.
 *     OPEN: DoHandshake with a socket, Read, Write and GetVerifyCertError need a TLS implementation on a connected
 *     socket (only a loopback peer can exist here); those paths stop with a message instead of inventing a Result.
 *
 * ------------------------------------------------------------------ nn::nsd
 * [sdk 0x3287a0 -> 0x3290f0] ResolveEx(Fqdn *out, const Fqdn &in): the nsd service command at +0x58; only the
 *     command's own Result is returned. [suyu] hle/service/sockets/nsd.cpp: the name is returned unchanged (0x100
 *     bytes), success. NOT VERIFIED: the real module substitutes the environment identifier in names that contain '%'.
 *
 * ------------------------------------------------------------------ nn::ldn
 * [sdk 0x2bc7c0] Initialize: creates the user service and calls its Initialize (vtable +0x100); only on success the
 *     service is stored in .bss 0xcae718 / 0xcae720. [sdk 0x2bcda0] GetState: 0 while no service is stored. Every
 *     other function loads the service pointer and calls through it without a check ([sdk 0x2bcfb0] etc.).
 * Platform fact modelled: this machine has NO local-wireless radio; that is a console whose wireless communication
 *     is switched off. [suyu] hle/service/ldn/user_local_communication_service.cpp Initialize: no usable interface
 *     -> ResultAirplaneModeEnabled (module 203, description 23) = 0x2ecb. So Initialize fails with 0x2ecb, GetState
 *     stays 0 (None), and (upstream, Smash) the game puts the Result in an ErrorResultVariant and does not go on;
 *     it calls Finalize / SetWirelessControllerRestriction only when GetState() != 0. Any other nn::ldn call after a failed Initialize dereferences a null service pointer on a
 *     console: those handlers stop with that message. (The alternative, "wireless on, nobody around", would need the
 *     real module's NetworkInfo contents for CreateNetwork, which the reference invents: not established.)
 *
 * ------------------------------------------------------------------ nn::ec::ShopServiceAccessor
 * [sdk 0x18e690] InitializeForShopServiceAccessors(buffer, size): builds the proxy and opens the nim:eca session
 *     ([suyu] hle/service/nim/nim.cpp: succeeds) -> 0. [sdk 0x193030] Finalize...: finalizes the accessors still
 *     registered -> 0. [sdk 0x18d740] ShopServiceAccessor(): {this, this} @0, service @0x10 = null, recursive mutex
 *     @0x18, list head {&, &} @0x38. [sdk 0x18d870] Initialize(service): CreateAccessorInterface ([suyu]: succeeds),
 *     service -> @0x10, linked into the proxy's list -> 0. [sdk 0x18d7d0] Finalize, [sdk 0x18d780] destructor.
 * [sdk 0x18da10] Request(): FIRST checks nn::nifm::IsAnyInternetRequestAccepted(GetClientId()); false -> 0x70a4
 *     (module 164, description 56) and nothing else happens. No request is ever accepted here.
 * [sdk 0x18e080 -> 0x18df20] AsyncResponse::GetSize: nothing attached (pointer @0x18 null) -> 0x190a4 (164-200),
 *     *out untouched. [sdk 0x18de10] ~AsyncResponse: Detach ([sdk 0x18de50]: unlink from the accessor @0x10), and
 *     the event @0x20 destroyed when the flag @0x48 is set. */

#define RETI(v) (c->x[0] = (uint64_t)(int64_t)(v))
#define M8(a) (*(uint8_t *)(uintptr_t)(a))
#define M16(a) (*(uint16_t *)(uintptr_t)(a))
#define M32(a) (*(uint32_t *)(uintptr_t)(a))
#define M64(a) (*(uint64_t *)(uintptr_t)(a))
#define A4 (c->x[4])
#define A5 (c->x[5])

static void c4a_call(Ctx *c, const char *name, uint64_t x0) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("nnnet: no handler for %s\n", name); guest_trap("nnnet: missing handler", c->x[30] - 4); }
    c->x[0] = x0;
    f(c);
}

/* ================================================================== nn::nifm */
static int g_nifm_init;
static ImportFn g_nifm_initialize_prev;
static void nifm_Initialize(Ctx *c) {                    /* the existing handler, plus the "initialised" flag of [sdk] 0xcc9910 */
    if (g_nifm_initialize_prev) g_nifm_initialize_prev(c); else RET(0);
    if ((uint32_t)c->x[0] == 0) g_nifm_init = 1;
}
#define NIFM_NOT_INITIALIZED 0xca6eu
static void nifm_GetCurrentProxySetting(Ctx *c) {
    if (!g_nifm_init) { RET(NIFM_NOT_INITIALIZED); return; }
    memset((void *)(uintptr_t)A0, 0, 0xaa);
    RET(0);
}
static void nifm_HandleNetworkRequestErrorResult(Ctx *c) {
    if (g_nifm_init) trace_printf("     nn::nifm::HandleNetworkRequestErrorResult: no error applet to show (not modelled) -> false\n");
    RET(0);
}

/* ================================================================== nn::socket on Winsock */
#define SO_MAX_FD 128
#define SO_O_NONBLOCK 0x800
typedef struct { SOCKET s; LONG refs; int type; int bound_any; } SoObj;
static struct { SoObj *o; int flags; } g_so_fd[SO_MAX_FD];
static SRWLOCK g_so_lock = SRWLOCK_INIT;
static int g_so_init, g_so_wsa;
static _Thread_local int t_so_herrno;

static int *so_errno_ptr(Ctx *c) {
    uint64_t keep = c->x[0];
    c4a_call(c, "__errno_location", 0);
    int *p = (int *)(uintptr_t)c->x[0];
    c->x[0] = keep;
    return p;
}
static void so_set_errno(Ctx *c, int e) { if (g_so_init) *so_errno_ptr(c) = e; }
static void so_fail(Ctx *c, int e) { so_set_errno(c, e); RETI(-1); }
static void so_need_init(Ctx *c, const char *what) {
    if (!g_so_init) {
        printf("\n%s called before nn::socket::Initialize: the SDK aborts here.\n", what);
        guest_trap("nn::socket call before Initialize (SDK abort)", c->x[30] - 4);
    }
}
static int so_wsa_errno(int e, int sending) {
    switch (e) {
    case 0: return 0;
    case WSAEINTR: return 4;
    case WSAEBADF: case WSAENOTSOCK: return e == WSAEBADF ? 9 : 88;
    case WSAEACCES: return 13;
    case WSAEFAULT: return 14;
    case WSAEINVAL: return 22;
    case WSAEMFILE: return 24;
    case WSAEWOULDBLOCK: return 11;
    case WSAEINPROGRESS: return 115;
    case WSAEALREADY: return 114;
    case WSAEDESTADDRREQ: return 89;
    case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;
    case WSAENOPROTOOPT: return 92;
    case WSAEPROTONOSUPPORT: return 93;
    case WSAESOCKTNOSUPPORT: return 94;
    case WSAEOPNOTSUPP: return 95;
    case WSAEPFNOSUPPORT: return 96;
    case WSAEAFNOSUPPORT: return 97;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAENETRESET: return 102;
    case WSAECONNABORTED: return sending ? 32 : 103;      /* [suyu] network.cpp: Winsock reports a broken pipe on send this way */
    case WSAECONNRESET: return 104;
    case WSAENOBUFS: return 105;
    case WSAEISCONN: return 106;
    case WSAENOTCONN: return 107;
    case WSAESHUTDOWN: return sending ? 32 : 108;
    case WSAETIMEDOUT: return 110;
    case WSAECONNREFUSED: return 111;
    case WSAEHOSTDOWN: return 112;
    case WSAEHOSTUNREACH: return 113;
    default: return 5;                                    /* EIO */
    }
}
static SoObj *so_get(int fd, int *flags) {
    SoObj *o = NULL;
    AcquireSRWLockExclusive(&g_so_lock);
    if (fd >= 0 && fd < SO_MAX_FD && g_so_fd[fd].o) { o = g_so_fd[fd].o; o->refs++; if (flags) *flags = g_so_fd[fd].flags; }
    ReleaseSRWLockExclusive(&g_so_lock);
    return o;
}
static void so_put(SoObj *o) {
    int last;
    AcquireSRWLockExclusive(&g_so_lock);
    last = --o->refs == 0;
    ReleaseSRWLockExclusive(&g_so_lock);
    if (last) { closesocket(o->s); free(o); }
}
static int so_install(SoObj *o, int flags) {             /* lowest free descriptor, or -1 */
    int fd = -1;
    AcquireSRWLockExclusive(&g_so_lock);
    for (int i = 0; i < SO_MAX_FD && fd < 0; i++) if (!g_so_fd[i].o) fd = i;
    if (fd >= 0) { g_so_fd[fd].o = o; g_so_fd[fd].flags = flags; o->refs++; }
    ReleaseSRWLockExclusive(&g_so_lock);
    return fd;
}
static int so_remove(int fd) {
    SoObj *o = NULL;
    AcquireSRWLockExclusive(&g_so_lock);
    if (fd >= 0 && fd < SO_MAX_FD) { o = g_so_fd[fd].o; g_so_fd[fd].o = NULL; g_so_fd[fd].flags = 0; }
    ReleaseSRWLockExclusive(&g_so_lock);
    if (!o) return 0;
    so_put(o);
    return 1;
}
static void so_nonblock(SoObj *o, int on) { u_long v = on ? 1 : 0; ioctlsocket(o->s, FIONBIO, &v); }
static int so_is_loopback(uint32_t be_addr) { return (be_addr & 0xff) == 127; }      /* first byte in memory */
static int so_addr_in(uint64_t p, struct sockaddr_in *out) {                          /* guest BSD sockaddr_in -> host; 0 when not AF_INET */
    memset(out, 0, sizeof *out);
    if (M8(p + 1) != 2) return 0;
    out->sin_family = AF_INET;
    out->sin_port = M16(p + 2);
    out->sin_addr.s_addr = M32(p + 4);
    return 1;
}
static uint32_t so_addr_out(uint64_t p, uint32_t cap, const struct sockaddr_in *in) { /* returns the length the module reports */
    uint8_t b[16] = {0};
    b[0] = 16; b[1] = 2;
    memcpy(b + 2, &in->sin_port, 2);
    memcpy(b + 4, &in->sin_addr, 4);
    if (p && cap) memcpy((void *)(uintptr_t)p, b, cap < 16 ? cap : 16);
    return 16;
}

static void socket_InitializeConfig(Ctx *c) {             /* [Recomps#1] finding 133: Initialize(const Config &), the overload B2's online library uses */
    if (g_so_init) guest_trap("nn::socket::Initialize(Config): [sdk] abort (already initialised)", c->x[30] - 4);
    if (!g_so_wsa) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) == 0) g_so_wsa = 1; }
    g_so_init = 1;
    RET(0);
}
static void socket_Finalize(Ctx *c) {
    so_need_init(c, "nn::socket::Finalize");
    for (int i = 0; i < SO_MAX_FD; i++) so_remove(i);
    g_so_init = 0;
    RET(0);
}

static void socket_Socket(Ctx *c) {                       /* (domain, type, protocol) */
    int domain = (int)(A0 & 0xff), type = (int)A1, protocol = (int)A2;
    so_need_init(c, "nn::socket::Socket");
    if (domain != 2) { so_fail(c, 97); return; }
    SOCKET s = socket(AF_INET, type, protocol);
    if (s == INVALID_SOCKET) { so_fail(c, so_wsa_errno(WSAGetLastError(), 0)); return; }
    SoObj *o = calloc(1, sizeof *o);
    o->s = s; o->type = type;
    int fd = so_install(o, 0);
    if (fd < 0) { closesocket(s); free(o); so_fail(c, 24); return; }
    RETI(fd);
}
static void socket_Close(Ctx *c) {
    so_need_init(c, "nn::socket::Close");
    if (!so_remove((int)A0)) { so_fail(c, 9); return; }
    RETI(0);
}
static void so_bind(Ctx *c, int len_check) {              /* (fd, const sockaddr *, len) */
    so_need_init(c, "nn::socket::Bind");
    if (!A1 || (len_check && !(uint32_t)A2)) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    int e = 0;
    if (!so_addr_in(A1, &a)) e = 97;
    else if (a.sin_addr.s_addr != 0 && !so_is_loopback(a.sin_addr.s_addr)) e = 99;
    else {
        int any = a.sin_addr.s_addr == 0;
        if (any) a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(o->s, (struct sockaddr *)&a, sizeof a) != 0) e = so_wsa_errno(WSAGetLastError(), 0);
        else o->bound_any = any;
    }
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_Bind(Ctx *c) { so_bind(c, 0); }
static void socket_BindSockAddr(Ctx *c) { so_bind(c, 1); }
static void socket_Connect(Ctx *c) {                      /* (fd, const sockaddr *, len) */
    so_need_init(c, "nn::socket::Connect");
    if (!A1) { so_fail(c, 22); return; }
    int flags = 0, e = 0;
    SoObj *o = so_get((int)A0, &flags);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    if (!so_addr_in(A1, &a)) e = 97;
    else if (!so_is_loopback(a.sin_addr.s_addr)) e = 101;                        /* no network: nothing but loopback is reachable */
    else if (connect(o->s, (struct sockaddr *)&a, sizeof a) != 0) {
        int w = WSAGetLastError();
        e = w == WSAEWOULDBLOCK ? 115 : w == WSAEISCONN ? 0 : so_wsa_errno(w, 0);     /* [suyu] ConnectImpl: "already connected" is success */
    }
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_Listen(Ctx *c) {
    so_need_init(c, "nn::socket::Listen");
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = listen(o->s, (int)A1) != 0 ? so_wsa_errno(WSAGetLastError(), 0) : 0;
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void so_name(Ctx *c, int peer) {                   /* (fd, sockaddr *, socklen_t *) */
    uint64_t addr = A1, lenp = A2;
    so_need_init(c, peer ? "nn::socket::GetPeerName" : "nn::socket::GetSockName");
    if (!addr || !lenp || !M32(lenp)) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    int alen = sizeof a;
    memset(&a, 0, sizeof a);
    int e = (peer ? getpeername(o->s, (struct sockaddr *)&a, &alen) : getsockname(o->s, (struct sockaddr *)&a, &alen)) != 0 ? so_wsa_errno(WSAGetLastError(), 0) : 0;
    if (!e && !peer && o->bound_any) a.sin_addr.s_addr = 0;
    if (e == 22 && !peer) { e = 0; a.sin_family = AF_INET; }                         /* Winsock refuses an unbound socket; BSD reports 0.0.0.0:0 */
    so_put(o);
    if (e) { so_fail(c, e); return; }
    M32(lenp) = so_addr_out(addr, M32(lenp), &a);
    RETI(0);
}
static void socket_GetSockName(Ctx *c) { so_name(c, 0); }
static void socket_GetPeerName(Ctx *c) { so_name(c, 1); }
static void socket_Fcntl(Ctx *c) {                        /* (fd, cmd, int) */
    int cmd = (int)A1, arg = (int)A2, fd = (int)A0, flags = 0;
    so_need_init(c, "nn::socket::Fcntl");
    SoObj *o = so_get(fd, &flags);
    if (!o) { so_fail(c, 9); return; }
    if (cmd == 3) RETI(flags);
    else if (cmd == 4) {
        so_nonblock(o, (arg & SO_O_NONBLOCK) != 0);
        AcquireSRWLockExclusive(&g_so_lock);
        if (g_so_fd[fd].o == o) g_so_fd[fd].flags = arg;
        ReleaseSRWLockExclusive(&g_so_lock);
        RETI(0);
    } else so_fail(c, 22);
    so_put(o);
}
static void socket_Poll(Ctx *c) {                         /* (pollfd *, nfds, timeout ms) */
    uint64_t fds = A0, n = A1;
    int timeout = (int)A2;
    so_need_init(c, "nn::socket::Poll");
    if (!fds && n) { so_fail(c, 22); return; }
    if (n >> 29) { so_fail(c, 14); return; }
    if ((int64_t)n <= 0) { so_set_errno(c, 0); RETI(-1); return; }                    /* [suyu] PollImpl: no entries -> -1 with errno 0 */
    if (timeout < -1) { so_fail(c, 22); return; }
    WSAPOLLFD *h = calloc((size_t)n, sizeof *h);
    SoObj **objs = calloc((size_t)n, sizeof *objs);
    int ready = 0, hn = 0, r = 0, e = 0;
    for (uint64_t i = 0; i < n; i++) {
        int fd = (int)M32(fds + i * 8);
        uint16_t ev = M16(fds + i * 8 + 4);
        M16(fds + i * 8 + 6) = 0;
        if (fd < 0) continue;                             /* a negative descriptor is ignored */
        objs[i] = so_get(fd, NULL);
        if (!objs[i]) { M16(fds + i * 8 + 6) = 0x20; ready++; continue; }
        h[hn].fd = objs[i]->s;
        h[hn].events = (SHORT)((ev & 0x41 ? POLLRDNORM : 0) | (ev & 0x82 ? POLLRDBAND : 0) | (ev & 0x4 ? POLLWRNORM : 0));
        hn++;
    }
    if (hn) {
        r = WSAPoll(h, (ULONG)hn, ready ? 0 : timeout);
        if (r < 0) e = so_wsa_errno(WSAGetLastError(), 0);
    } else if (!ready && timeout) Sleep(timeout < 0 ? INFINITE : (DWORD)timeout);
    hn = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (!objs[i]) continue;
        if (!e) {
            SHORT rv = h[hn].revents;
            uint16_t ev = M16(fds + i * 8 + 4), out = 0;
            if (rv & POLLRDNORM) out |= (uint16_t)(ev & 0x41 ? ev & 0x41 : 1);
            if (rv & POLLRDBAND) out |= (uint16_t)(ev & 0x82 ? ev & 0x82 : 2);
            if (rv & POLLWRNORM) out |= 4;
            if (rv & POLLERR) out |= 8;
            if (rv & POLLHUP) out |= 0x10;
            if (rv & POLLNVAL) out |= 0x20;
            M16(fds + i * 8 + 6) = out;
            if (out) ready++;
        }
        hn++;
        so_put(objs[i]);
    }
    free(h); free(objs);
    if (e) so_fail(c, e); else RETI(ready);
}
/* The game's option numbers are BSD's (SOL_SOCKET 0xffff, SO_* below); Winsock uses the same numbers, Linux its own. */
#ifdef _WIN32
#define so_host_name(level, name) (name)
#else
static int so_host_name(int level, int name) {
    if (level != 0xffff) return name;          /* IPPROTO_TCP TCP_NODELAY is 1 everywhere */
    switch (name) {
    case 0x4: return SO_REUSEADDR;   case 0x8: return SO_KEEPALIVE;  case 0x10: return SO_DONTROUTE;  case 0x20: return SO_BROADCAST;
    case 0x80: return SO_LINGER;     case 0x100: return SO_OOBINLINE; case 0x200: return SO_REUSEPORT; case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;   case 0x1003: return SO_SNDLOWAT; case 0x1004: return SO_RCVLOWAT; case 0x1005: return SO_SNDTIMEO;
    case 0x1006: return SO_RCVTIMEO; case 0x1007: return SO_ERROR;    case 0x1008: return SO_TYPE;     case 0x2: return SO_ACCEPTCONN;
    default: return -1;                        /* unknown here: getsockopt / setsockopt answer ENOPROTOOPT */
    }
}
#endif
static void socket_GetSockOpt(Ctx *c) {                   /* (fd, level, name, void *, socklen_t *) */
    uint64_t val = A3, lenp = A4;
    int level = (int)A1, name = (int)A2;
    so_need_init(c, "nn::socket::GetSockOpt");
    if (!val || !lenp || !M32(lenp)) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = 0;
    uint32_t cap = M32(lenp);
    if (level == 0xffff || (level == 6 && name == 1)) {
        char tmp[64];
        int tl = sizeof tmp;
        if (name == 0x800 && level == 0xffff) { memset(tmp, 0, 4); tl = 4; }          /* SO_NOSIGPIPE: there are no signals */
        else if (getsockopt(o->s, level == 0xffff ? SOL_SOCKET : IPPROTO_TCP, so_host_name(level, name), tmp, &tl) != 0) e = so_wsa_errno(WSAGetLastError(), 0);
        if (!e) {
            uint8_t out[16] = {0};
            uint32_t ol = 4;
            if (level == 0xffff && name == 0x1007) { int v = so_wsa_errno(*(int *)tmp, 0); memcpy(out, &v, 4); }
            else if (level == 0xffff && name == 0x80) { LINGER *l = (LINGER *)tmp; int v[2] = {l->l_onoff, l->l_linger}; memcpy(out, v, 8); ol = 8; }
#ifdef _WIN32
            else if (level == 0xffff && (name == 0x1005 || name == 0x1006)) { DWORD ms = *(DWORD *)tmp; int64_t tv[2] = {ms / 1000, (ms % 1000) * 1000}; memcpy(out, tv, 16); ol = 16; }
#else
            else if (level == 0xffff && (name == 0x1005 || name == 0x1006)) { struct timeval *t = (struct timeval *)tmp; int64_t tv[2] = {t->tv_sec, t->tv_usec}; memcpy(out, tv, 16); ol = 16; }   /* a timeval here, not milliseconds */
#endif
            else { int v = tl >= 4 ? *(int *)tmp : (tl >= 1 ? tmp[0] != 0 : 0); memcpy(out, &v, 4); }
            if (ol > cap) ol = cap;
            memcpy((void *)(uintptr_t)val, out, ol);
            M32(lenp) = ol;
        }
    }                                                     /* other levels: [suyu] GetSockOptImpl answers success without data */
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_SetSockOpt(Ctx *c) {                   /* (fd, level, name, const void *, len) */
    uint64_t val = A3;
    uint32_t len = (uint32_t)A4;
    int level = (int)A1, name = (int)A2;
    so_need_init(c, "nn::socket::SetSockOpt");
    if (!val) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = 0, r = 0;
    if (level == 0xffff && name == 0x80) {
        if (len < 8) e = 22;
        else { LINGER l; l.l_onoff = (u_short)(M32(val) != 0); l.l_linger = (u_short)M32(val + 4); r = setsockopt(o->s, SOL_SOCKET, SO_LINGER, (char *)&l, sizeof l); }
    } else if (level == 0xffff && (name == 0x1005 || name == 0x1006)) {
        DWORD ms = len >= 16 ? (DWORD)(M64(val) * 1000 + M64(val + 8) / 1000) : len >= 4 ? M32(val) : 0;
#ifdef _WIN32
        if (len < 4) e = 22; else r = setsockopt(o->s, SOL_SOCKET, name, (char *)&ms, sizeof ms);
#else
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        if (len < 4) e = 22; else r = setsockopt(o->s, SOL_SOCKET, so_host_name(level, name), &tv, sizeof tv);
#endif
    } else if (level == 0xffff && name == 0x800) r = 0;   /* SO_NOSIGPIPE */
    else if (level == 0xffff || (level == 6 && name == 1)) {
        if (len < 4) e = 22;
        else { int v = (int)M32(val); r = setsockopt(o->s, level == 0xffff ? SOL_SOCKET : IPPROTO_TCP, so_host_name(level, name), (char *)&v, sizeof v); }
    }                                                     /* other levels: [suyu] SetSockOptImpl accepts and ignores */
    if (!e && r != 0) e = so_wsa_errno(WSAGetLastError(), 0);
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}

/* ---------------------------------------------------------------- resolver */
static const char *const g_gai_text[] = {
    "Success", "Address family for hostname not supported", "Temporary failure in name resolution", "Invalid value for ai_flags",
    "Non-recoverable failure in name resolution", "ai_family not supported", "Memory allocation failure",
    "No address associated with hostname", "hostname nor servname provided, or not known", "servname not supported for ai_socktype",
    "ai_socktype not supported", "System error returned in errno", "Invalid value for hints", "Resolved protocol is unknown",
    "Argument buffer overflow"};

/* ================================================================== nn::ssl */
typedef struct SslCtx { uint32_t magic; int connections; } SslCtx;
typedef struct SslConn { uint32_t magic; SslCtx *ctx; int fd; int keep_socket; } SslConn;      /* fd: the module's duplicate, -1 = none */
static uint64_t g_ssl_count;                              /* [sdk] .bss 0xcd10a8 */
static int g_ssl_session;


/* ================================================================== nn::ec::ShopServiceAccessor */
static uint64_t g_ec_list[2];                             /* the proxy's list of accessors ([sdk] proxy + 0x70) */
static int g_ec_ready;
static uint8_t g_ec_service_token[16];

/* ================================================================== registration */
#define SOK(n) "_ZN2nn6socket" n
static void c4a_register(void) {
    static const ImportDef defs[] = {
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_GetCurrentProxySetting},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_HandleNetworkRequestErrorResult},
         {SOK("10InitializeERKNS0_6ConfigE"), socket_InitializeConfig}, {SOK("8FinalizeEv"), socket_Finalize},
         {SOK("6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE"), socket_Socket}, {"nnsocketSocket", socket_Socket},
        {SOK("5CloseEi"), socket_Close}, {"nnsocketClose", socket_Close},
         {SOK("4BindEiPKNS0_8SockAddrEj"), socket_BindSockAddr}, {"nnsocketBind", socket_Bind},
         {"nnsocketConnect", socket_Connect},
        {SOK("6ListenEii"), socket_Listen},
         {"nnsocketGetSockName", socket_GetSockName}, {"nnsocketGetPeerName", socket_GetPeerName},
         {"nnsocketFcntl", socket_Fcntl},
         {"nnsocketPoll", socket_Poll},
         {"nnsocketGetSockOpt", socket_GetSockOpt},
         {"nnsocketSetSockOpt", socket_SetSockOpt},
    };
    int n = (int)(sizeof defs / sizeof defs[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(defs[i].name)) printf("nnnet: \"%s\" is not an import of the game (typo?)\n", defs[i].name);
    g_nifm_initialize_prev = import_lookup("_ZN2nn4nifm10InitializeEv");
    register_imports(defs, n);
    {
        static const ImportDef wrap = {"_ZN2nn4nifm10InitializeEv", nifm_Initialize};
        if (g_nifm_initialize_prev != nifm_Initialize) register_imports(&wrap, 1);
    }
}
