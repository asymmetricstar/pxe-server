# PXE Server (C++)

A small PXE boot server that runs **DHCP + TFTP + HTTP** in a single executable, built with MinGW on Windows. It serves network boot to BIOS (iPXE) and UEFI (GRUB2) clients.

---

<img width="712" height="392" alt="screenshot-2026-11-13_16-26-10" src="https://github.com/user-attachments/assets/ee62f23a-e952-46a3-9215-7182ceeca975" />

---

## 4. Running

Open a terminal **as Administrator** (cmd / PowerShell ), then:

```bat
pxesrv.exe
```

- Default config file: `config/pxe.cfg` (looked up next to the exe, then `../config/pxe.cfg`).
- Custom config file: `pxesrv.exe C:\path\to\pxe.cfg`
- The console title is set to `PXE Service`.
- Stop with **Ctrl+C** (SIGINT/SIGTERM trigger a clean shutdown: DHCP/TFTP/HTTP sockets are closed).
- Logs: `build/bin/logs/pxe.log` (verbosity controlled by `[logging] level`).

On startup the console prints the loaded config path, server root, and server IP.
With `[logging] level = debug` every DHCP/TFTP/HTTP request is logged in detail.

---

## 5. Configuration — `config/pxe.cfg`

`${SERVER_ROOT}` resolves relative to the config file: if the config file lives in a
directory named `config`, it expands to that directory's **parent**
(e.g. `build/bin/config/pxe.cfg` → `build/bin`); otherwise it expands to the config
directory itself (e.g. `<project>/test/test_pxe.cfg` → `<project>/test`).

### `[server]`

| Key | Default | Description |
| --- | --- | --- |
| `host` | `0.0.0.0` | Interface to bind. Usually keep `0.0.0.0` so replies go out the NIC that received the DHCP broadcast |
| `port` | `67` | DHCP port |
| `server_ip` | *(empty)* | Empty = auto-detect local IPv4. Set explicitly on multi-homed hosts |
| `boot_root` | `${SERVER_ROOT}/boot` | TFTP/HTTP root directory |
| `boot_file` | `undionly.kpxe` | NBP served to **BIOS** clients. Set to an `http://...` URL to use iPXE HTTP boot mode |
| `boot_file_efi` | `grub/grubx64.efi` | NBP served to **UEFI** clients (if empty, UEFI clients receive the BIOS file) |

### `[dhcp]`

| Key | Default | Description |
| --- | --- | --- |
| `range_start` / `range_end` | empty → `.100` / `.199` of the server subnet | Lease pool |
| `subnet_mask` | `255.255.255.0` | Subnet mask |
| `router` | empty → `.1` of the server subnet | Default gateway |
| `dns` | empty → same as `router` | DNS server |
| `domain` | `local.home` | Domain name |
| `lease_time` | `21600` | Lease time (seconds) |

A warning is printed at load time if the pool is not on the same subnet as `server_ip`.

### `[tftp]`

| Key | Default | Description |
| --- | --- | --- |
| `port` | `69` | TFTP port |
| `timeout` | `3` | Per-block retransmit timeout (seconds) |
| `max_clients` | `50` | Concurrent client limit |
| `blksize` | `1456` | Maximum block size offered during negotiation (fits a 1500-byte MTU) |

### `[http]`

| Key | Default | Description |
| --- | --- | --- |
| `port` | `80` | HTTP port — set to **`0` to disable the HTTP service** |
| `root` | `${SERVER_ROOT}/boot` | Directory to serve |

### `[logging]`

| Key | Default | Description |
| --- | --- | --- |
| `level` | `info` | `debug` \| `info` \| `warning` \| `error` |
| `file` | `${SERVER_ROOT}/logs/pxe.log` | Log file (leave empty for console only) |

---

## 6. Boot payload (`boot/`) and menus

`boot/` is the single tree served over both TFTP and HTTP — everything a PXE client sees.

```
boot/
├── undionly.kpxe        # BIOS NBP (iPXE)  -> DHCP option 67
├── boot.ipxe            # iPXE stage-2 menu (fetched over HTTP)
├── grub/grubx64.efi     # UEFI NBP (GRUB2) -> DHCP option 67
├── grub/grub.cfg        # UEFI menu
├── grub.cfg             # copy of the GRUB menu at the TFTP root
├── vmlinuz-lts          # Linux LTS kernel
├── initramfs-lts        # initramfs
└── isos/                # drop your .iso images here (README.txt)
```

Menu entries:

- **BIOS, iPXE (`boot.ipxe`):** lists only the ISO images found in `boot/isos/` plus
  `iPXE shell`, `Reboot`, `Exit to BIOS`. The selected ISO is booted with `sanboot`
  over HTTP **without downloading it up front** — the HTTP server answers range
  requests (`206 Partial Content`).
- **UEFI, GRUB (`grub/grub.cfg`):** Linux LTS (default/verbose), Reboot, Power off,
  plus the ISO images found in `boot/isos/`.

To add a custom (non-ISO) menu entry, add a `menuentry { ... }` block to
`boot/grub/grub.cfg` **outside** the generated ISO markers (see below).

### Automatic ISO listing (`boot/isos/`)

Drop `.iso` files into `boot/isos/` and **the server generates the menu entries for you.**
Clients cannot do this themselves (GRUB cannot list a TFTP directory and has no glob on a
`pxe` device), so `pxesrv` scans the directory and rewrites the generated block between
the `>>> ISOS ... <<< ISOS` markers in the menu files:

- **Scanned on startup and on every DHCP DISCOVER** — no server restart is needed for a
  newly copied ISO; rebooting the client is enough.
- **Rewritten files:** `grub.cfg` (3 copies) and `boot.ipxe` (2 blocks: menu items and
  boot actions). Lines outside the markers are never touched. With no ISOs present the
  menu shows `(No ISO images in /isos)`.

How each ISO is booted depends on its name:

- **BIOS (iPXE, `boot.ipxe`):** every ISO becomes an item that runs
  `sanboot http://<server>/isos/<file>.iso` (read on demand over HTTP). File names must
  not contain spaces (iPXE cannot quote a URL).
  - Images whose name contains `windows`, `winpe`, `win10/11/7/8`, `hiren`, or starts
    with `win` are treated as **Windows/PE** and booted with
    `sanboot --no-describe --drive 0x80` (with fallbacks) to map the ISO as a virtual CD.
  - Other images use `sanboot --no-describe` with a plain `sanboot` fallback.
- **UEFI (GRUB):**
  - **Linux (Debian/Ubuntu/Mint layout):** loopback-mounts the ISO and boots
    `/boot/vmlinuz` + `/boot/initrd.img` with `iso-scan/filename=/isos/<file>.iso`.
    Other distributions may need a custom `menuentry` added outside the markers.
  - **Alpine Linux** (name contains `alpine`): boots `(loop)/boot/vmlinuz-lts` +
    `initramfs-lts`, falling back to `vmlinuz-virt` + `initramfs-virt`.
  - **Windows/PE:** loopback-mounts the ISO and chainloads
    `/efi/boot/bootx64.efi` (or `/efi/microsoft/boot/bootmgfw.efi`). Loopbacking a large
    ISO over a net device can be slow.

ISO files must live in the running server's `boot/isos` directory (`boot_root/isos`).

> **Note:** Booting an ISO over the network depends on the image and firmware. BIOS
> `sanboot` maps the ISO as a virtual drive, but some Linux initramfs images (e.g. Alpine)
> may fail to locate their boot media after the kernel loads (`Mounting boot media failed`);
> in that case try UEFI/GRUB, or boot the distribution's netboot kernel/initramfs directly
> with a custom menu entry instead of `sanboot`-ing the ISO.

### Adding a kernel or a custom menu entry manually

1. Copy the file(s) under `boot/`.
2. Add a `menuentry` block to `boot/grub/grub.cfg` **outside** the ISO markers.
3. No server restart is needed (files are read from disk on each request).

---
