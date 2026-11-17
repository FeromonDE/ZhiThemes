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
