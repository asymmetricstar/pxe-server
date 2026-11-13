Place bootable ISO images in this directory.

The server lists them automatically: pxesrv scans this folder on startup and
on every DHCP DISCOVER and rewrites the "ISOS" block of the boot menus, so a
newly copied .iso appears at the next client boot without restarting anything.

BIOS (iPXE, boot/boot.ipxe):
  every image becomes a menu item which runs
      sanboot http://<server>/isos/<name>.iso
  iPXE mounts the ISO over HTTP and reads only the sectors it needs (the server
  answers Range requests with 206), so nothing is downloaded up front.
  Windows/PE images (name contains "windows", "winpe", "win10/11/7/8", "hiren",
  or starts with "win") are booted with "sanboot --no-describe --drive 0x80"
  and fallbacks, which maps the ISO as a virtual CD for the Windows loader.
  File names must not contain spaces - an iPXE URL cannot be quoted.

UEFI (GRUB, boot/grub/grub.cfg):
  - Linux-style ISOs (Debian/Ubuntu/Mint layout) become
        ISO: <name>.iso
    which loop-mounts the ISO and boots /boot/vmlinuz with
    iso-scan/filename=/isos/<name>.iso. Other distributions may need a custom
    menuentry added outside the ISOS markers.
  - Windows/PE ISOs become
        Windows ISO: <name>.iso
    which loop-mounts the ISO and chainloads /efi/boot/bootx64.efi (or
    /efi/microsoft/boot/bootmgfw.efi). Booting a large ISO from a net (TFTP)
    device this way can be slow.

The server already serves this directory over TFTP and HTTP.
