# UI160 / Core22 legacy database recovery

This narrow companion handles the reproduced epoch3 Core20-to-Core22 startup
failure: `Native PGN database recheck held: native reconnect proof/history rejected`.
It supports only the fingerprinted public UI160 and Core22 binaries.

Keep one Horizon window open on its stopped-Core error, then run PGN-Recover.exe.
It checks the actual last startup log before doing any repair. A different
failure stops the tool without closing Horizon or modifying the database.

For the matching failure it closes Horizon normally, reserves its lifecycle
lock and runs the unchanged Core22 with `-reindex-chainstate` and wallets disabled.
If the stored canonical block4246 was rejected by Core20, standard Core22
`reconsiderblock` validation runs again. No validation rule is disabled.
The tool checks native verifychain(4,50), verifies wallet files and configuration
are unchanged, stops Core normally, then restarts the original Horizon command.
It does not upload diagnostics, delete wallets, reset the network, or modify
Defender settings. Interrupted process identity is retained in a local journal.

Build on Windows using the installed .NET Framework C# compiler:
`csc /nologo /optimize+ /platform:x64 /target:exe /r:System.Web.Extensions.dll /r:System.Management.dll /out:PGN-Recover.exe CoreRecovery272.cs`

Qualification: actual public UI160/Core22 with a Core20-replayed state, showing
the stopped-Core error; actual companion repair; wallet-file identity; local
wallet reopen and real network peer; refusal on a healthy Core; complete UTXO
MuHash identity with the original Core22 node at the same canonical tip.
The generated test wallet has zero funds; the UTXO comparison covers the entire
chain state. These tests do not claim execution on Robert's PC.
