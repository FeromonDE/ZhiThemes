# Trophy provider verified from the user's 13.20 dumps

The implementation uses the decrypted game-hub bundle supplied on 2026-11-17.
Hermes version: 89. Source hash: `6281c2fc2085b197285788df14feb56ecdcccdfb`.
Container SHA-256: `537b80e64bc64022a9cffc62a5a0cc999b9041970426ee75ff6b75b2e37a94e0`.
The Home bundle has source hash `80837fe66ccb00ba923f279c90b78c7c4813e543`.

The module table and call chain were inspected using P1sec/hermes-dec:

| Module | Bytecode functions | Role |
| --- | --- | --- |
| 2576 | 26124–26126 | Summary hook; passes `{npTitleId}` to the progress fetcher |
| 2577 | 26127–26130 | Anonymous default progress wrapper; invokes `fetchTitleData`, then maps its reply |
| 2578 | 26131–26140 | Named `fetchTitleData({npTitleId, userId?, accountId?})` service |
| 2579 | 26141 onward | `runWithAccessor` controls native accessor lifetime |

The 2578 service uses `User.launchedUser` when userId is omitted, defaults
accountId to `me`, obtains local trophy IDs with
`TrophyAccessor.getPlayedTitleIdListByNpTitleId`, then calls `getTitleConf`
and `getTitleRecord`. The returned object already contains
`definedTrophies`, `earnedTrophies`, and numeric `progress`, which the Home
widget consumes directly. Its mapper is not needed for the widget.

An exported JavaScript function name of `fetchTrophyProgressData` is absent.
That text is a measurement label at the caller. Requiring 2559 or waiting
for a function with that name cannot resolve this bundle's anonymous wrapper.

The implementation checks the named `fetchTitleData` export in the verified
service module before using it. It does not invoke an arbitrary default
export in that module. Legacy 9.60 compatibility remains as a fallback.

Regression tests cover the real export layout with an anonymous function,
parameter object and default user selection, two different focused games,
legacy fallback, and requests arriving before the local service loads.
Native service calls are simulated. Hardware testing remains necessary.

## Offline reading in build 381

The build 380 console log resolves the correct service but reports
`TrophyHelperError` for PPSA02530 (with and without `_00`); other titles
return definitions and zero earned counts. No timeout occurs for this title.
The error string alone does not identify the original native failure.

Inspection of module 2579, function 26144, shows that the stock helper calls
`TrophyAccessor.isOnlineModeAvailable` and chooses `ONLINE` or `OFFLINE`.
Module 932 defines these values as `online` and `offline`. Thus using the
stock helper does not guarantee a local-only request. Whether it actually
chose online mode on this console was not logged by build 380.

Build 381 uses the verified module 210 TrophyAccessor and module 8 User APIs
to create an explicitly offline accessor private to the widget request. It
queries title mappings, reads configuration and the current user's record,
and releases the accessor in both success and failure paths. It does not
modify native API exports or the stock trophy application's behavior.
Each read is bounded by a timeout and overall deadline; late create replies
are deleted, and a failing first mapping does not hide a later valid record.
Nested error fields are retained so a native failure remains diagnosable.
A missing record is not substituted with zero earned counts.

`theme/test_trophies_offline.js` verifies nonzero unlocked counts, offline
mode, launched-user selection, cleanup, error codes, late handle creation,
and stalled operations with controlled native-service replies. Hardware
verification remains necessary.

## Native Trophies catalog fallback in build 382

The build 381 log reports `local trophy title not found` for PPSA02530
with and without `_00`, while the user's native Trophies screen displays a
PS5 set with 8%, one silver and four bronze. Forcing offline mode did not
resolve the missing NP-title mapping. The exact reason that index has no
entry is not established by the available dumps/logs.

The supplied NPXS40025 Trophies app is Hermes v89, source hash
`64cea6a0515f212c1e75066dbffb6ab6c126b621`. Its actual catalog path is:

| Bytecode function | Verified behavior |
| --- | --- |
| 803 | `getTitleConfList({accessorId,accountId,cancelToken})` returns a cursor; calls `read(10000)`, then `close()` |
| 814/815 | `getTitleRecordList` has the same cursor contract; records are indexed by `titleId` |
| 3417 onward | Main list reads definition and record cursors, joins by `titleId`, closes both |
| 14189 | Renders definition `name`, `iconUri`, `platforms` and record `progress`, `numUnlocked*` |
| 1778 | Platform values are `ps3`, `psp2`, `ps4`, `prospero` (PS5), `pspc` |

The theme uses the already verified hub TrophyAccessor, with no new module
number guess. Only when the NP-title mappings return empty arrays does it
read the catalog using the verified cursor signature. It selects exactly
one distinct trophy `titleId` whose full name matches a current installed
game name and whose platforms include the requested game's platform. Name
normalization folds case/whitespace and trademark symbols; no fuzzy title,
substring, progress, ID-prefix or "first earned set" selection is performed.
Localized Home names and file metadata names are relayed by ID, rather than
borrowing the name of the current focus. Two matching PS5 sets are rejected.
Names that differ beyond the limited normalization remain unmatched.

Once matched, configuration and the current user's record are read by the
catalog's actual trophy `titleId`. Only definitions are cached for 20 seconds,
scoped to user/account; earned counts are read again. Cursors are closed
before accessor deletion, including failed reads, malformed cursor replies
with a close method, and late cursor creation. Calls plus normal cleanup
fit inside the outer queue watchdog.

Tests reproduce empty mappings plus the screenshot's nonzero counts, reject
wrong-platform/similar/duplicate names, check current per-ID metadata and
user isolation, and exercise stalled/late cursor replies. These are simulated
native replies; matching on the user's console is still unverified.

## Native account identity and production row capture in build 383

The user's build 382 system-screen log shows the Trophies app in offline
mode with a real decimal-string `myAccountId`, the same local user ID as the
theme, and aggregate counts of one silver/four bronze. The earlier provider
always queried `accountId: 'me'`; this differs from the real screen's calls.
The log's generic map reaches its 220-component cap after the first rows,
so the affected title's full definition is not present. Its actual NPWR
identifier is not inferred from an unrelated row.

The verified native call chain in NPXS40025 is:

| Function | Behavior |
| --- | --- |
| 13614 | Resolves `getAccountId({userId})`, stores `myAccountId`, falls back to `me` |
| 14116 (module 1203) | Reads `Account.getInfo({userId}).accountId` via module 16's Account API |
| 5979 | Passes navigation `myAccountId` into offline `requestGet` as `accountId` |

In the hub, factory 95's Account getter (function 96) resolves module 1009,
whose factory 12717 wraps the native `VshNMPsnAccountModule`. Build 383 checks
its Account export and calls the verified `getInfo({userId})` method before
creating the private offline accessor. Decimal strings are passed unchanged
to mapping, configuration-list and record queries; numbers are rejected to
avoid rounding 64-bit IDs. Explicit caller accounts are preserved. Failed,
missing or timed-out identity lookup retains the stock `me` fallback. No
account ID is hardcoded or written into the theme configuration/logs.

The supplied production Home, hub and Trophies HBC payloads contain none of
the React DevTools callback identifiers used by the earlier tree dumper.
Native bundle copies can therefore exist without any `trophy-*.txt` tree.
Build 383 captures the real `data`/`rowData`/`dataTrophyInfo` passed through
the existing JSX hook instead, writing bounded, deduplicated JSON lines to
`trophy-records.txt` only when Screen dump is enabled. These lines include
only the trophy ID, game name, platforms, progress and earned counts;
unknown counts remain null. They are independent of the generic map cap.

Tests reproduce a `me` lookup with no earned record and a native account
lookup returning 8%, one silver/four bronze, preserve IDs above 2^53, check
separate users/explicit accounts and late identity replies, and replay the
production row prop shapes without a React DevTools hook. Console testing
of this change remains necessary; the account difference is verified, but
its effect on PPSA02530 has not yet been measured on the user's console.
