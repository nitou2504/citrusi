# romm3ds

3DS homebrew: browse your self-hosted [RomM](https://github.com/rommapp/romm) NDS library on-console, download ROMs to SD, and auto-install HOME-menu forwarders — an hShop-like experience for your own DS collection. Fork of [NDSForwarder v1.4.7](https://github.com/volkanturkut/NDSForwarder) (GPL-3); all forwarder generation/install machinery is inherited from it (see `README.upstream.md`).

## Features

- **RomM Library (NDS)** — lists the `nds` platform of your RomM server (HTTP Basic auth, works against a stock authenticated instance — no `DISABLE_DOWNLOAD_ENDPOINT_AUTH` needed). `*` marks games already on SD. Select → downloads to `sd:/roms/nds/` with progress → builds + installs the DSiWare forwarder on-device (in-memory CIA → AM). Multi-part ROMs are skipped.
- **Manage Installed** — lists `sd:/roms/nds/*.nds` with forwarder state (`[+]` = forwarder on NAND, TID derived from the ROM game code), SD free space and DSiWare count in the header. Per game: delete forwarder + ROM, forwarder only, or ROM file only (with confirmation).
- **SD Card Browser** — the original NDSForwarder browser/Install All, unchanged.
- **RomM Server Settings** — re-prompt host/user/password (stored in `sd:/3ds/forwarder/romm.json`, plaintext).

## Requirements (same runtime chain as NDSForwarder)

- Luma3DS CFW (sig patches), Homebrew Launcher.
- [NTR_Forwarder pack](https://github.com/RocketRobz/NTR_Forwarder) `_nds` folder on SD root + TWiLight Menu++ / nds-bootstrap.
- Forwarder template (`sdcard.nds`/`sdcard.fwd`) in romfs (bundled) or `sd:/3ds/forwarder/templates/`.
- RomM ≥ 4.x reachable over plain HTTP on the LAN (HTTPS works with cert verification disabled).
- ~40 DSiWare HOME-menu cap applies (hardware limit; the app warns).

## Build

```sh
docker run --rm -v $PWD:/romm3ds -w /romm3ds devkitpro/devkitarm:latest make
```

Produces `romm3ds.3dsx` (+`.smdh`). Copy to `sd:/3ds/` (e.g. via ftpd on the 3DS: `curl -T romm3ds.3dsx ftp://<3ds-ip>:5000/3ds/`).

## First run

1. Open Homebrew Launcher → romm3ds.
2. "RomM Library (NDS)" → prompts for server (`http://ip[:port]`), username, password.
3. Pick a game → Yes → downloaded + forwarder installed → HOME menu.

## Upstream fixes carried in this fork

- `error.hpp`: parenthesized error macros (`code == ERROR_INSTALL_ALREADY_EXISTS` previously parsed as `(code==ERROR_INSTALL)|0x102` — always true, causing a spurious overwrite prompt after every install).

## Notes

- Forwarder TID = `00048004` + game code (default template, non-random TID). RomM-driven installs always use non-random TIDs so Manage can map ROM ↔ forwarder; the SD browser still honors the random-TID setting, but such installs show as `[ ]` in Manage.
- Deleting a forwarder never touches the `.nds` on SD (and vice versa) — the two are independent, matching System Settings behavior where removing the tiny DSiWare entry keeps the ROM.
