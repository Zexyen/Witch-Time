/* Included by nnnet.c (round "close5", helper A): nn::account network-service-account functions and AsyncContext,
 * nn::friends requests, nn::bcat DeliveryCacheDirectory / DeliveryCacheFile, nn::ngc::ProfanityFilter object state,
 * nn::pdm::QueryApplicationPlayStatistics, nn::pl::BeginVrMode / EndVrMode.
 * Registered by c5a_register() from nnnet_register_final(), i.e. after every other table: these REPLACE the handlers of
 * the same names in nnfs.c, nnmisc.c, nnsdk.c and nnpl.c (whose table entries are now dead).
 *
 * Sources: [sdk] = binaries\13.0.4\sdk.elf (addresses quoted), [suyu] = reference\suyu\src\core\hle\service (file:line),
 * [game] = main, read with Ghidra. Console state modelled: one local user, no network connection (nn::nifm never accepts
 * an internet request), no Nintendo Account linked to the user.
 *
 * ------------------------------------------------------------------ nn::account
 * UserHandle +0x10 is the IManagerForApplication of the opened user; its commands as the SDK calls them:
 *   +0x20 CheckAvailability, +0x28 GetAccountId, +0x30 EnsureIdTokenCacheAsync, +0x38 LoadIdTokenCache, +0x40 (cmd 130).
 * [sdk 0x4b450] IsNetworkServiceAccountAvailable: CheckAvailability; 0 -> true; a result of module 124 with description
 *     200..269 -> success + false; anything else is returned. So "the user has no usable network service account" is
 *     the range 124-0200..0269. WHICH code of the range the account module gives for a user without a linked Nintendo
 *     Account is NOT in the reference ([suyu] acc/acc.cpp:713 CheckAvailability is a stub that answers success, i.e. it
 *     models a linked account) and no such constant exists in sdk.elf (tools_audit/patches/close5/c5a_scan124.py: none).
 * [sdk 0x4b380] EnsureNetworkServiceAccountAvailable: CheckAvailability; 0 -> 0; a code outside that range is returned;
 *     a code INSIDE it is discarded: GetUserId, then detail::StartPselApplet [sdk 0x3afe0 -> 0x3b010] with UiSettings
 *     {mode 2, uid} ([suyu] am/frontend/applet_profile_select.h:29 mode 2 = EnsureNetworkServiceAccountAvailable):
 *     CreateLibraryApplet(0x10); StartLibraryApplet failing -> 0x27c (124-0001); else the first u32 of the applet's 0x18
 *     byte answer is the result. The applet can only link an account over the network; offline the user can only leave
 *     it, and the applet then answers 124-0001 "cancelled by user" ([suyu] applet_profile_select.cpp:106-107). So the
 *     function gives 0x27c whichever code of the range the module used. The applet is not shown here.
 *     (Upstream, Smash: (result & 0x3fffff) == 0x27c returns 2 without an error message.)
 * [sdk 0x4b500] EnsureNetworkServiceAccountIdTokenCacheAsync: FIRST detail::CheckInternetAvailability [sdk 0x3afb0]:
 *     nn::nifm::IsAnyInternetRequestAccepted(GetClientId()) false -> 0x767c (124-0059), returned before the account
 *     module is asked and before the AsyncContext is touched. [game] 0x7100249cd0 only tests it for 0.
 * AsyncContext [sdk 0x494f0 C1, 0x493b0 move C1, 0x493e0 move =, 0x49510 D1]: {vtable, 8 + 8 bytes, IAsyncContext * @0x18}.
 *     C1(): vtable (_ZTVN2nn7account12AsyncContextE + 0x10), the other three words 0.
 *     C1(&&): vtable; words @8, @0x10, @0x18 copied; the source's three words zeroed.
 *     operator=(&&): takes the source's three words (source zeroed), stores them; if the OLD pointer @0x18 was set: HasDone
 *         on it must succeed and be true (abort otherwise) and it is released. Returns this.
 *     D1: vtable; if the pointer is set: HasDone must succeed, release, pointer = 0, abort when it had not finished.
 *     HasDone [0x49750] / GetResult [0x49780] / Cancel [0x49720] / GetSystemEvent [0x49670] call the IAsyncContext
 *         (+0x30, +0x38, +0x28, +0x20) WITHOUT a null check. In this model no IAsyncContext ever exists (the only
 *         function of the game that makes one fails first, see above), so the pointer is always null and those four
 *         would fault on a console; here they stop the game with a message instead of answering anything.
 * GetNetworkServiceAccountId [0x4b4d0] (+0x28), LoadNetworkServiceAccountIdTokenCache [0x4b5d0] (+0x38),
 *     LoadCachedNintendoAccountInfo [0x4b7c0] (CheckAvailability, then +0x40): the account module's answer for a user
 *     without a network service account, returned unchanged. STILL OPEN: 0x1907c = 124-0200, the first code of the
 *     range above, is used; the exact code is not established (see IsNetworkServiceAccountAvailable).
 *
 * ------------------------------------------------------------------ nn::friends
 * detail::CheckUserStatus [sdk 0x1e0350]: fr_check_user() of nnnet.c (0x479 invalid Uid, 0x679 user not open).
 * AsyncContext [sdk 0x1de230 C1 (lifted)]: {nn::os::MutexType @0, detail::AsyncContextInternal * @0x20 = null}.
 *     GetSystemEvent(ev) [0x1de290]: lock; internal null -> 0xe79 (121-0007); else its completion event's readable handle
 *         is attached to ev ([0x1e0e80], manual clear; abort when ev is already initialised: byte ev+0x28); unlock.
 *     GetResult [0x1de390]: lock; internal null -> 0xe79; else the internal's result word (+0x610); unlock.
 *     D1 [0x1de260]: internal -> DeleteInstance; FinalizeMutex(this).
 * GetProfileList(ctx, profiles, uid, ids, count) [0x1da300]: CheckUserStatus; detail::CallAsync [0x1e0810]: an async
 *     session to the friends module (exists offline: [suyu] friend/friend.cpp CreateFriendService), an
 *     AsyncContextInternal, a thread [0x1e0c80] that sets the result to 0x1079 (121-0008) and runs the request
 *     [0x1da9d0], whose FIRST step is detail::CheckInternetRequestAccepted [0x1e0440]: nifm not accepted -> 0xc79
 *     (121-0006), the module is never asked; the context is attached (SetInternal [0x1de3e0]) and 0 returned.
 *     Here the request is finished before GetProfileList returns (no thread): result 0xc79, event signalled. A console
 *     reports 0x1079 for the few moments the thread needs.
 * GetBlockedUserList(outCount, ids, uid, offset, count) [0x1da1b0]: CheckUserStatus; module command +0x60;
 *     [suyu] friend.cpp:226-233 success with 0 entries; the count is stored only on success.
 * NotificationQueue::Pop(info) [0x1dee90]: lock; service @0x30 null -> 0x279 (121-0001); else the notification service's
 *     Pop: [suyu] friend.cpp:417-425 an empty queue answers Account::ResultNoNotifications = {module 124, 15} = 0x1e7c.
 *     NOT VERIFIED on a console: the reference uses the ACCOUNT module number for this friends result; a console may
 *     answer 121-0015 (0x1e79). Nothing is ever queued offline.
 * NotificationQueue D1 [0x1deb90]: lock; service: release, @0x30 = 0, DestroySystemEvent(this+0x38); unlock; byte @0x60
 *     set -> DestroySystemEvent(this+0x38); FinalizeMutex(this).
 * STILL OPEN (the friends module's answer is not in the reference):
 *     UpdateFriendInfo [0x1da070] (+0x40) and AddPlayHistory [0x1da870] (+0x98; two NormalizeInAppScreenName [0x1e0470]
 *         calls before it are NOT reproduced): [suyu] friend.cpp:31 and :48 have no handler. After the SDK's user check 0
 *         is returned and nothing is written: that is the reference's generic answer for an unhandled command
 *         ([suyu] service.cpp:80-83), not a known answer of the module.
 *     GetPlayHistoryRegistrationKey [0x1da700] (+0x88): [suyu] friend.cpp:260-270 is a stub: success, key not written.
 *         Same here (key untouched); a console writes a 0x40-byte key or fails for a user without an account.
 *     ShowUserDetailInfo [0x1e80c0 -> 0x1e8000]: library applet 0x1a; when it cannot be started the SDK returns 0x3c79
 *         (121-0030) [0x1e8078]. There is no such applet on a PC: 0x3c79. What the applet answers on a console when the
 *         user closes it is not established.
 *
 * ------------------------------------------------------------------ nn::bcat DeliveryCacheDirectory / DeliveryCacheFile
 * Both objects are one pointer to the module's directory / file service ([sdk 0xc3830, 0xc40c0] C1: null; lifted).
 * Directory::Open(name) [0xc3be0 -> 0xc3f80]: DirectoryName::IsValid false -> 0x27a (122-0001); already open -> 0xc7a
 *     (122-0006); ShimLibraryGlobal::CreateDirectoryService [0xc5610]: storage not mounted -> 0xa7a (122-0005); the
 *     service's Open: [suyu] bcat/delivery_cache_directory_service.cpp:55-56 a directory that does not exist is
 *     ResultFailedOpenEntity = 122-0002 = 0x47a; detail::HandleError [0xc51b0] passes it through (it aborts only for
 *     fs results 2-4000..4999); the service is released and the object stays closed.
 *     The delivery cache of a console that never synchronised is empty ([sdk] nnsdk.c: 0 directories), so every valid
 *     name gives 0x47a.
 * Directory::Read [0xc3dd0]: not open -> 0xe7a (122-0007), count not written. Close [0xc3c80] / D1 [0xc3840]: nothing
 *     when not open.
 * File::Open(dir, file) [0xc4520 -> 0xc4960]: DirectoryName or FileName [0xc1b60: 1..31 characters of A-Z a-z 0-9 _ . ,
 *     not ending in '.'] invalid -> 0x27a; already open -> 0xc7a; CreateFileService: not mounted -> 0xa7a; the service's
 *     Open: [suyu] bcat/delivery_cache_file_service.cpp:42-43 -> 0x47a. Close [0xc45d0] / D1 [0xc40d0]: nothing when
 *     not open.
 *
 * ------------------------------------------------------------------ nn::ngc::ProfanityFilter
 * The filter runs inside the SDK ([0x30cc50], [0x30d9b0]) on word lists that ContentsReader::MountContents [0x30b7e0]
 * loads from a system data archive. That archive is firmware content and NOT in the dump.
 *     C1 [0x30b6a0]: vtable, byte @9 (initialised) = 0, u32 @0xc (skip mode) = 0, u64 @0x20 = 0, u32 @0x28 (mask mode) = 0.
 *     SetMaskMode [0x30b9e0]: u32 @0x28 = mode. SkipAtSignCheck [0x30b9d0]: u32 @0xc = mode.
 *     Initialize(buf, size) [0x30b9b0 -> 0x30b700 with flag 1]: buf null -> 0x692 (146-0003); size < 0x20000 -> 0x892
 *         (146-0004); already initialised -> 0x492 (146-0002); modes reset to 0; work buffer set up; contents mounted;
 *         byte @8 = flag, byte @9 = 1.
 *     MaskProfanityWordsInText(outCount, text, patterns) [0x30d5b0]: not initialised -> 0x292 (146-0001); text null ->
 *         0x692; then the masking.
 *     D1 [0x30b840]: vtable; initialised -> contents reference dropped, byte @9 = 0.
 * Here: the object state and every argument check are the SDK's. STILL OPEN: Initialize succeeds without mounting
 * anything and MaskProfanityWordsInText masks nothing (0 words): the faithful answer needs the word lists.
 *
 * ------------------------------------------------------------------ nn::pdm, nn::pl
 * pdm::QueryApplicationPlayStatistics(out, ids, count) [0x341240]: IApplicationFunctions command +0x128; abort on
 *     failure; returns the number of entries. [suyu] am/service/application_functions.cpp:413-420: 0 entries. A console
 *     that has run the queried applications answers with their play counts / times.
 * pl::BeginVrMode [0x341e90]: nn::pctl::CheckStereoVisionPermission(false) false -> 0xd08e (142-0104); else
 *     ICommonStateGetter +0xd0 (BeginVrModeEx; abort on failure) and 0. pl::EndVrMode [0x341f00]: +0xd8 (EndVrModeEx).
 *     [suyu] am/service/common_state_getter.cpp:225-237: the module only keeps a flag. The console also changes the
 *     panel's brightness handling in VR mode; nothing of that exists on a PC. */
void os_signal_event(uint64_t key);                       /* os_threads.c */

/* ---------------------------------------------------------------- nn::account */
#define ACC_CANCELLED 0x27cu                               /* 124-0001 */
#define ACC_NO_INTERNET 0x767cu                            /* 124-0059 */
#define ACC_NSA_UNAVAILABLE_UNVERIFIED 0x1907cu            /* 124-0200: first code of the SDK's range; exact code open */
static uint64_t c5a_acc_vtable(void) { uint64_t v = sdk_symbol("_ZTVN2nn7account12AsyncContextE"); return v ? v + 0x10 : 0; }
static void c5a_acc_no_context(Ctx *c, const char *what) {
    printf("\nnn::account::AsyncContext::%s on a context that holds no request ([sdk] null pointer at +0x18: a console faults here).\n", what);
    guest_trap("nn::account::AsyncContext used without a request", c->x[30] - 4);
}
static void c5a_acc_release_old(Ctx *c, uint64_t old) {
    if (old) c5a_acc_no_context(c, "destructor / move assignment (a request pointer exists, which this model never creates)");
}
static void c5a_acc_Async_ctor(Ctx *c) { uint64_t s = A0; wr64(s, c5a_acc_vtable()); wr64(s + 8, 0); wr64(s + 0x10, 0); wr64(s + 0x18, 0); }
static void c5a_acc_Async_dtor(Ctx *c) {
    uint64_t s = A0;
    wr64(s, c5a_acc_vtable());
    c5a_acc_release_old(c, rd64(s + 0x18));
    RET(s);
}
static void c5a_acc_Async_GetResult(Ctx *c) { c5a_acc_no_context(c, "GetResult"); }
static void c5a_acc_Async_GetSystemEvent(Ctx *c) { c5a_acc_no_context(c, "GetSystemEvent"); }
static void c5a_acc_EnsureAvailable(Ctx *c) { RET(ACC_CANCELLED); }
static void c5a_acc_EnsureIdTokenCacheAsync(Ctx *c) { RET(ACC_NO_INTERNET); }
static void c5a_acc_nsa_unavailable(Ctx *c) { RET(ACC_NSA_UNAVAILABLE_UNVERIFIED); }

/* ---------------------------------------------------------------- nn::friends */
typedef struct { uint32_t magic, result; } C5aFriendsAsync;   /* stands for detail::AsyncContextInternal: a finished request */

/* ---------------------------------------------------------------- nn::bcat */
extern volatile LONG g_bcat_mounted;                      /* nnsdk.c */
int bcat_dirname_valid(const char *s);                    /* nnsdk.c: [sdk 0xc1be0] */

static const ImportDef c5a_defs[] = {
    {"_ZN2nn7account12AsyncContextC1Ev", c5a_acc_Async_ctor},
    {"_ZN2nn7account12AsyncContextD1Ev", c5a_acc_Async_dtor},
    {"_ZN2nn7account12AsyncContext9GetResultEv", c5a_acc_Async_GetResult},
    {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", c5a_acc_Async_GetSystemEvent},
    {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", c5a_acc_EnsureAvailable},
    {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", c5a_acc_EnsureIdTokenCacheAsync},
    {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", c5a_acc_nsa_unavailable},
    {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", c5a_acc_nsa_unavailable},
};
static void c5a_register(void) {
    int n = (int)(sizeof c5a_defs / sizeof c5a_defs[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(c5a_defs[i].name)) printf("nnsvc_c5a: \"%s\" is not an import of the game (typo?)\n", c5a_defs[i].name);
    register_imports(c5a_defs, n);
}
