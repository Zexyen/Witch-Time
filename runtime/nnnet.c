/* Network-facing leftovers of an OFFLINE console, plus the run-time shader compiler entry points.
 *
 * LIFTED from the SDK (sdklift_gen.c), not here:
 *   nn::socket::InetAton / InetNtoa / InetNtop / InetPton (pure conversions),
 *   nn::bcat::DeliveryCacheProgress getters (GetStatus, GetResult, GetWholeTotal, GetWholeDownloaded),
 *   nn::friends::Friend / FriendPresence / Profile getters (plain field reads),
 *   nn::account::CachedNintendoAccountInfo getters, nn::ldn::MakeBroadcastAddress, nn::ngc::CountNumbers,
 *   nn::hid::SixAxisSensorState::GetQuaternion.
 * and, REPLACING do-nothing placeholders of nnsdk.c / nnmisc.c with the SDK's real constructors so that
 * the lifted getters read initialised objects: DeliveryCacheProgress C1 / D1 / Update ([sdk] 0xc4ab0: mutex
 * @0, service pointer @0x20 = null, 0x200 zero bytes @0x58, i.e. status 0 = "none"), DeliveryCacheFile C1,
 * DeliveryCacheDirectory C1, ec::ShopServiceAccessor::AsyncResponse C1 ([sdk] 0x18de00).
 *
 * Answered here:
 *  - nn::nifm leftovers, nn::socket / nnsocket*, nn::ssl / nnssl*, nn::nsd, nn::ldn, nn::ec::ShopServiceAccessor: see nnnet_c4a.inc.
 *  - nn::bcat::DeliveryCacheFile::Read: [sdk] 0xc47e0 a file that is not open gives 0xe7a (module 122,
 *    description 7); no file can be open because DeliveryCacheFile::Open fails (nnsdk.c).
 *  - nn::ec::ShopServiceAccessor::AsyncResponse: [sdk] 0x18e390 TryWait is true when no request is attached
 *    (pointer @0x18 null); [sdk] 0x18e1f8 Get then returns 0x190a4 (module 164, description 200). A
 *    request can never be attached: ShopServiceAccessor::Request fails first (nnnet_c4a.inc).
 *    nn::ec::ShopServiceAccessor::DefaultTimeout (a data import) gets its real 8 bytes from the SDK image.
 *  - glslc* (8 entry points of the run-time GLSL compiler): WEAK imports of the game that no module of this
 *    dump provides (sdk.elf has no such symbols), so on a console their GOT entries are null.
 *    [game] 0x710003a630 collects the eight pointers and 0x71000382f8 uses them only for a shader whose
 *    code type is 2 (GLSL source); every shader of the retail game is precompiled (code type 0), so the
 *    path is dead. If it were ever taken the handlers below stop with a clear message instead of
 *    returning garbage. A real implementation would have to compile GLSL to what the NVN backend takes
 *    (or hand the source straight to the OpenGL backend); nothing in the game needs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include "sdklift.h"

#pragma comment(lib, "ws2_32.lib")

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))


/* ---------------------------------------------------------------- nn::friends: the client side as the SDK has it
 * Re-implemented from sdk.elf, instruction by instruction, with the friends system module (not in the dump) answering as
 * a console without friends and without a network (reference: suyu friend/friend.cpp + friend_interface.cpp: the
 * notification service exists and hands out an event; nothing ever signals it offline).
 *   detail::CheckUserStatus(uid) [sdk 0x1e0350]: an invalid Uid {0,0} is 0x479 (friends 121-0002); with the SDK's check flag
 *       set (.data 0xbfba40 = 1) the Uid must be one of the OPEN users (nn::account::ListOpenUsers), else 0x679 (121-0003).
 *   UserPresence::Initialize(uid) [sdk 0x1deff0]: CheckUserStatus; on success the Uid is stored at this+0.
 *   NotificationQueue::Initialize(uid) [sdk 0x1dec40]: CheckUserStatus; lock this (mutex at +0); create the notification
 *       service; its event handle is attached to the SystemEvent at this+0x38 (manual clear); Uid -> this+0x20;
 *       service -> this+0x30; unlock; success.
 *   NotificationQueue::Finalize() [sdk 0x1debf0]: lock; Uid at +0x20 cleared; if a service is held: release it,
 *       this+0x30 = 0, DestroySystemEvent(this+0x38); unlock.
 *   NotificationQueue::GetSystemEvent() [sdk 0x1dedf0]: lock; this+0x30 ? this+0x38 : NULL; unlock.
 * The service object is a token (the game never sees it; the SDK only keeps the pointer). */
void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
uint32_t fr_check_user(Ctx *c, uint64_t uid) {       /* also used by nnsdk.c (GetFriendList) */
    uint64_t lo = rd64(uid), hi = rd64(uid + 8);
    if (!lo && !hi) return 0x479;
    uint64_t flag = sdk_symbol("__nnDetailNintendoSdkRuntimeObjectFileRefer") ? SDK_BASE + 0xbfba40 : 0;      /* the SDK image's own flag byte */
    if (flag && rd8(flag)) {
        int account_user_is_open(uint64_t lo, uint64_t hi);       /* nnfs.c: the host's account model (ListOpenUsers is not an import of the game) */
        (void)c;
        return account_user_is_open(lo, hi) ? 0 : 0x679;
    }
    return 0;
}
static void friends_UserPresence_Initialize(Ctx *c) {     /* (this, const Uid &) -> Result */
    uint64_t self = A0, uid = A1;
    uint32_t r = fr_check_user(c, uid);
    if (!r) { wr64(self, rd64(uid)); wr64(self + 8, rd64(uid + 8)); }
    RET(r);
}
static void c5a_register(void);                           /* nnsvc_c5a.inc: account, friends, bcat directory / file, ngc, pdm, pl VR */
static void c4a_register(void);                           /* nnnet_c4a.inc: nifm, sockets, ssl, nsd, ldn, ec accessor */
void nnnet_register_final(void) {                          /* after every other table (nnsdk2.c): replaces the do-nothing versions */
    static const ImportDef defs[] = {
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", friends_UserPresence_Initialize},
    };
    register_imports(defs, (int)(sizeof defs / sizeof defs[0]));
    c4a_register();
    c5a_register();
}

#include "nnnet_c4a.inc"
#include "nnsvc_c5a.inc"
