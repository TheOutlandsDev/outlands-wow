# Living Azeroth Gate 5 Windows launcher

This standard-library Python launcher has no administrator or server controls.
It verifies the local Tailscale backend, the exact server peer, TSMP, and TCP
3724/8085 before changing or launching anything.

Double-click `Start-Living-Azeroth-Windows.cmd` while the Linux Gate 5 observer
is waiting. If WoW is not in a common location, run this in PowerShell:

```powershell
& ".\Start-Living-Azeroth-Windows.cmd" --wow-dir "D:\Games\World of Warcraft 3.3.5a"
```

The launcher backs up `Data\<locale>\realmlist.wtf`, temporarily writes
`set realmlist 100.64.0.10`, launches `Wow.exe`, waits for it to close, and
restores the original file. If Windows or Python is interrupted, recover with:

```powershell
& ".\Start-Living-Azeroth-Windows.cmd" --restore-realmlist --wow-dir "D:\Games\World of Warcraft 3.3.5a"
```

For a network-only check, add `--check`. Sanitized results are written beneath
`%LOCALAPPDATA%\LivingAzerothLauncher`; no account name, password, character
name, server credential, or client file content is recorded.
