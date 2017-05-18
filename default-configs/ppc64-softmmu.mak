# Default configuration for ppc64-softmmu

# PCI configuration - cut down from the defaults in pci.mak
CONFIG_PCI=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO=y
CONFIG_USB_EHCI=y
CONFIG_USB_XHCI=y
CONFIG_WDT_IB6300ESB=y
CONFIG_PCI_TESTDEV=y

include sound.mak
include usb.mak
CONFIG_VIRTIO_VGA=y
CONFIG_VGA=y
CONFIG_VGA_PCI=y
CONFIG_SERIAL=y
CONFIG_PSERIES=y
CONFIG_LIBDECNUMBER=y
CONFIG_USB_OHCI=y
# For pSeries
CONFIG_XICS=$(CONFIG_PSERIES)
CONFIG_XICS_KVM=$(and $(CONFIG_PSERIES),$(CONFIG_KVM))
CONFIG_MEM_HOTPLUG=y

# Enable vhost-user-scsi
CONFIG_VHOST_USER_SCSI=y
