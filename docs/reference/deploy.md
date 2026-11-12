# Deploy and restore

> Reference moved out of the [README](../../README.md); the text is unchanged.

## Deploy and restore

```powershell
.\tools\deploy.ps1 -Game cp2077
.\tools\restore.ps1 -Game cp2077
```

Game ids in `tools/games.json`: `cp2077` (Cyberpunk 2077) and `aw2` (Alan Wake 2,
Epic launcher, no `bin\x64` subdirectory).

`deploy.ps1`:

- resolves the game from `tools/games.json` or an explicit `-GamePath`;
- refuses to run while the game's executable is running;
- resolves Steam library folders automatically; installs outside Steam (for
  example Epic) come from `installDirHints` or `-GamePath`;
- never overwrites an existing `.orig` backup (AMD's original DLL, kept only so
  `restore.ps1` can put it back; Sedici itself never loads it);
- refuses to overwrite a DLL that is not Sedici's;
- copies `sedici.toml` only if one is not already there, so a tuned file is kept.

`restore.ps1` puts the original DLL back and removes `sedici.log` and
`sedici.toml`. Restore is an uninstall, not a merge: **copy your `sedici.toml`
out first if you tuned it.** It refuses to touch a DLL that is not Sedici's.

Both scripts only touch `amd_fidelityfx_framegeneration_dx12.dll`, its `.orig`
backup, `sedici.toml` and `sedici.log`. The loader and upscaler DLLs are never
modified.
