#pragma once
#include "utils/config_loader.hpp"

namespace pxe {

/*
 * Automatic ISO listing for the boot menus.
 *
 * The client cannot do this itself: GRUB running from a TFTP (pxe) device has
 * no directory listing, so `for iso in /isos/*.iso` never expands and PXELINUX
 * has no globbing at all. The server knows the real directory contents, so it
 * rewrites the generated block between the ISOS markers of every boot menu
 * file under boot_root (grub.cfg and boot.ipxe).
 */
class IsoMenu {
public:
    /*
     * Scans <boot_root>/isos for *.iso images and refreshes the menus.
     * Returns the number of ISO images found, or -1 if boot_root is unusable.
     */
    static int refresh(const Config& cfg);
};

} // namespace pxe
