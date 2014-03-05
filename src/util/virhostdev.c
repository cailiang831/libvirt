/* virhostdev.c: hostdev management
 *
 * Copyright (C) 2006-2007, 2009-2013 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 * Copyright (C) 2014 SUSE LINUX Products GmbH, Nuernberg, Germany.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 * Author: Chunyan Liu <cyliu@suse.com>
 */

#include <config.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "virhostdev.h"
#include "viralloc.h"
#include "virstring.h"
#include "virfile.h"
#include "virerror.h"
#include "virlog.h"
#include "virutil.h"
#include "virnetdev.h"
#include "configmake.h"

#define VIR_FROM_THIS VIR_FROM_NONE
#define HOSTDEV_STATE_DIR LOCALSTATEDIR "/run/libvirt/hostdevmgr"

static virHostdevManagerPtr manager; /* global hostdev manager, never freed */

static virClassPtr virHostdevManagerClass;
static void virHostdevManagerDispose(void *obj);
static virHostdevManagerPtr virHostdevManagerNew(void);

static int virHostdevManagerOnceInit(void)
{
    if (!(virHostdevManagerClass = virClassNew(virClassForObject(),
                                               "virHostdevManager",
                                               sizeof(virHostdevManager),
                                               virHostdevManagerDispose)))
        return -1;

    if (!(manager = virHostdevManagerNew()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virHostdevManager)

static void
virHostdevManagerDispose(void *obj)
{
    virHostdevManagerPtr hostdevMgr = obj;

    if (!hostdevMgr)
        return;

    virObjectUnref(hostdevMgr->activePciHostdevs);
    virObjectUnref(hostdevMgr->inactivePciHostdevs);
    virObjectUnref(hostdevMgr->activeUsbHostdevs);
    virObjectUnref(hostdevMgr->activeScsiHostdevs);
    VIR_FREE(hostdevMgr->stateDir);

    VIR_FREE(hostdevMgr);
}

static virHostdevManagerPtr
virHostdevManagerNew(void)
{
    virHostdevManagerPtr hostdevMgr;

    if (!(hostdevMgr = virObjectNew(virHostdevManagerClass)))
        return NULL;

    if ((hostdevMgr->activePciHostdevs = virPCIDeviceListNew()) == NULL)
        goto error;

    if ((hostdevMgr->activeUsbHostdevs = virUSBDeviceListNew()) == NULL)
        goto error;

    if ((hostdevMgr->inactivePciHostdevs = virPCIDeviceListNew()) == NULL)
        goto error;

    if ((hostdevMgr->activeScsiHostdevs = virSCSIDeviceListNew()) == NULL)
        goto error;

    if (VIR_STRDUP(hostdevMgr->stateDir, HOSTDEV_STATE_DIR) < 0)
        goto error;

    if (virFileMakePath(hostdevMgr->stateDir) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Failed to create state dir '%s'"),
                       hostdevMgr->stateDir);
        goto error;
    }

    return hostdevMgr;

error:
    virObjectUnref(hostdevMgr);
    return NULL;
}

virHostdevManagerPtr
virHostdevManagerGetDefault(void)
{
    if (virHostdevManagerInitialize() < 0)
        return NULL;

    return virObjectRef(manager);
}

static virPCIDeviceListPtr
virHostdevGetPciHostDeviceList(virDomainHostdevDefPtr *hostdevs, int nhostdevs)
{
    virPCIDeviceListPtr list;
    size_t i;

    if (!(list = virPCIDeviceListNew()))
        return NULL;

    for (i = 0; i < nhostdevs; i++) {
        virDomainHostdevDefPtr hostdev = hostdevs[i];
        virPCIDevicePtr dev;

        if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;
        if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            continue;

        dev = virPCIDeviceNew(hostdev->source.subsys.u.pci.addr.domain,
                              hostdev->source.subsys.u.pci.addr.bus,
                              hostdev->source.subsys.u.pci.addr.slot,
                              hostdev->source.subsys.u.pci.addr.function);
        if (!dev) {
            virObjectUnref(list);
            return NULL;
        }

        if (virPCIDeviceListAdd(list, dev) < 0) {
            virPCIDeviceFree(dev);
            virObjectUnref(list);
            return NULL;
        }

        virPCIDeviceSetManaged(dev, hostdev->managed);
        if (hostdev->source.subsys.u.pci.backend
            == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
            if (virPCIDeviceSetStubDriver(dev, "vfio-pci") < 0) {
                virObjectUnref(list);
                return NULL;
            }
        } else {
            if (virPCIDeviceSetStubDriver(dev, "pci-stub") < 0) {
                virObjectUnref(list);
                return NULL;
            }
        }
    }

    return list;
}


/*
 * virHostdevGetActivePciHostDeviceList - make a new list with a *copy* of
 *   every virPCIDevice object that is found on the activePciHostdevs
 *   list *and* is in the hostdev list for this domain.
 *
 * Return the new list, or NULL if there was a failure.
 *
 * Pre-condition: activePciHostdevs is locked
 */
static virPCIDeviceListPtr
virHostdevGetActivePciHostDeviceList(virHostdevManagerPtr mgr,
                                     virDomainHostdevDefPtr *hostdevs,
                                     int nhostdevs)
{
    virPCIDeviceListPtr list;
    size_t i;

    if (!(list = virPCIDeviceListNew()))
        return NULL;

    for (i = 0; i < nhostdevs; i++) {
        virDomainHostdevDefPtr hostdev = hostdevs[i];
        virDevicePCIAddressPtr addr;
        virPCIDevicePtr activeDev;

        if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;
        if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            continue;

        addr = &hostdev->source.subsys.u.pci.addr;
        activeDev = virPCIDeviceListFindByIDs(mgr->activePciHostdevs,
                                              addr->domain, addr->bus,
                                              addr->slot, addr->function);
        if (activeDev && virPCIDeviceListAddCopy(list, activeDev) < 0) {
            virObjectUnref(list);
            return NULL;
        }
    }

    return list;
}

static int
virHostdevPciSysfsPath(virDomainHostdevDefPtr hostdev,
                       char **sysfs_path)
{
    virPCIDeviceAddress config_address;

    config_address.domain = hostdev->source.subsys.u.pci.addr.domain;
    config_address.bus = hostdev->source.subsys.u.pci.addr.bus;
    config_address.slot = hostdev->source.subsys.u.pci.addr.slot;
    config_address.function = hostdev->source.subsys.u.pci.addr.function;

    return virPCIDeviceAddressGetSysfsFile(&config_address, sysfs_path);
}


static int
virHostdevIsVirtualFunction(virDomainHostdevDefPtr hostdev)
{
    char *sysfs_path = NULL;
    int ret = -1;

    if (virHostdevPciSysfsPath(hostdev, &sysfs_path) < 0)
        return ret;

    ret = virPCIIsVirtualFunction(sysfs_path);

    VIR_FREE(sysfs_path);

    return ret;
}


static int
virHostdevNetDevice(virDomainHostdevDefPtr hostdev, char **linkdev,
                    int *vf)
{
    int ret = -1;
    char *sysfs_path = NULL;

    if (virHostdevPciSysfsPath(hostdev, &sysfs_path) < 0)
        return ret;

    if (virPCIIsVirtualFunction(sysfs_path) == 1) {
        if (virPCIGetVirtualFunctionInfo(sysfs_path, linkdev,
                                         vf) < 0)
            goto cleanup;
    } else {
        if (virPCIGetNetName(sysfs_path, linkdev) < 0)
            goto cleanup;
        *vf = -1;
    }

    ret = 0;

cleanup:
    VIR_FREE(sysfs_path);

    return ret;
}


static int
virHostdevNetConfigVirtPortProfile(const char *linkdev, int vf,
                                   virNetDevVPortProfilePtr virtPort,
                                   const virMacAddr *macaddr,
                                   const unsigned char *uuid,
                                   bool associate)
{
    int ret = -1;

    if (!virtPort)
        return ret;

    switch (virtPort->virtPortType) {
    case VIR_NETDEV_VPORT_PROFILE_NONE:
    case VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH:
    case VIR_NETDEV_VPORT_PROFILE_8021QBG:
    case VIR_NETDEV_VPORT_PROFILE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("virtualport type %s is "
                         "currently not supported on interfaces of type "
                         "hostdev"),
                       virNetDevVPortTypeToString(virtPort->virtPortType));
        break;

    case VIR_NETDEV_VPORT_PROFILE_8021QBH:
        if (associate)
            ret = virNetDevVPortProfileAssociate(NULL, virtPort, macaddr,
                                                 linkdev, vf, uuid,
                                                 VIR_NETDEV_VPORT_PROFILE_OP_CREATE, false);
        else
            ret = virNetDevVPortProfileDisassociate(NULL, virtPort,
                                                    macaddr, linkdev, vf,
                                                    VIR_NETDEV_VPORT_PROFILE_OP_DESTROY);
        break;
    }

    return ret;
}


static int
virHostdevNetConfigReplace(virDomainHostdevDefPtr hostdev,
                           const unsigned char *uuid,
                           char *stateDir)
{
    char *linkdev = NULL;
    virNetDevVlanPtr vlan;
    virNetDevVPortProfilePtr virtPort;
    int ret = -1;
    int vf = -1;
    int vlanid = -1;
    bool port_profile_associate = true;
    int isvf;

    isvf = virHostdevIsVirtualFunction(hostdev);
    if (isvf <= 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Interface type hostdev is currently supported on"
                         " SR-IOV Virtual Functions only"));
        return ret;
    }

    if (virHostdevNetDevice(hostdev, &linkdev, &vf) < 0)
        return ret;

    vlan = virDomainNetGetActualVlan(hostdev->parent.data.net);
    virtPort = virDomainNetGetActualVirtPortProfile(
                                 hostdev->parent.data.net);
    if (virtPort) {
        if (vlan) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("direct setting of the vlan tag is not allowed "
                             "for hostdev devices using %s mode"),
                           virNetDevVPortTypeToString(virtPort->virtPortType));
            goto cleanup;
        }
        ret = virHostdevNetConfigVirtPortProfile(linkdev, vf,
                            virtPort, &hostdev->parent.data.net->mac, uuid,
                            port_profile_associate);
    } else {
        /* Set only mac and vlan */
        if (vlan) {
            if (vlan->nTags != 1 || vlan->trunk) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("vlan trunking is not supported "
                                 "by SR-IOV network devices"));
                goto cleanup;
            }
            if (vf == -1) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("vlan can only be set for SR-IOV VFs, but "
                                 "%s is not a VF"), linkdev);
                goto cleanup;
            }
            vlanid = vlan->tag[0];
        } else  if (vf >= 0) {
            vlanid = 0; /* assure any current vlan tag is reset */
        }

        ret = virNetDevReplaceNetConfig(linkdev, vf,
                                        &hostdev->parent.data.net->mac,
                                        vlanid, stateDir);
    }
cleanup:
    VIR_FREE(linkdev);
    return ret;
}

/* @oldStateDir:
 * For upgrade purpose:
 * To an existing VM on QEMU, the hostdev netconfig file is originally stored
 * in cfg->stateDir (/var/run/libvirt/qemu). Switch to new version, it uses new
 * location (hostdev_mgr->stateDir) but certainly will not find it. In this
 * case, try to find in the old state dir.
 */
static int
virHostdevNetConfigRestore(virDomainHostdevDefPtr hostdev,
                           char *stateDir,
                           char *oldStateDir)
{
    char *linkdev = NULL;
    virNetDevVPortProfilePtr virtPort;
    int ret = -1;
    int vf = -1;
    bool port_profile_associate = false;
    int isvf;

    /* This is only needed for PCI devices that have been defined
     * using <interface type='hostdev'>. For all others, it is a NOP.
     */
    if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS ||
        hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI ||
        hostdev->parent.type != VIR_DOMAIN_DEVICE_NET ||
        !hostdev->parent.data.net)
       return 0;

    isvf = virHostdevIsVirtualFunction(hostdev);
    if (isvf <= 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Interface type hostdev is currently supported on"
                         " SR-IOV Virtual Functions only"));
        return ret;
    }

    if (virHostdevNetDevice(hostdev, &linkdev, &vf) < 0)
        return ret;

    virtPort = virDomainNetGetActualVirtPortProfile(
                                 hostdev->parent.data.net);
    if (virtPort) {
        ret = virHostdevNetConfigVirtPortProfile(linkdev, vf, virtPort,
                                                 &hostdev->parent.data.net->mac,
                                                 NULL,
                                                 port_profile_associate);
    } else {
        ret = virNetDevRestoreNetConfig(linkdev, vf, stateDir);
        if (ret < 0 && oldStateDir != NULL)
            ret = virNetDevRestoreNetConfig(linkdev, vf, oldStateDir);
    }

    VIR_FREE(linkdev);

    return ret;
}

int
virHostdevPreparePCIDevices(virHostdevManagerPtr hostdev_mgr,
                            const char *drv_name,
                            const char *name,
                            const unsigned char *uuid,
                            virDomainHostdevDefPtr *hostdevs,
                            int nhostdevs,
                            unsigned int flags)
{
    virPCIDeviceListPtr pcidevs = NULL;
    int last_processed_hostdev_vf = -1;
    size_t i;
    int ret = -1;

    virObjectLock(hostdev_mgr->activePciHostdevs);
    virObjectLock(hostdev_mgr->inactivePciHostdevs);

    if (!(pcidevs = virHostdevGetPciHostDeviceList(hostdevs, nhostdevs)))
        goto cleanup;

    /* We have to use 9 loops here. *All* devices must
     * be detached before we reset any of them, because
     * in some cases you have to reset the whole PCI,
     * which impacts all devices on it. Also, all devices
     * must be reset before being marked as active.
     */

    /* Loop 1: validate that non-managed device isn't in use, eg
     * by checking that device is either un-bound, or bound
     * to pci-stub.ko
     */

    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);
        virPCIDevicePtr other;
        bool strict_acs_check = !!(flags & VIR_HOSTDEV_STRICT_ACS_CHECK);

        if (!virPCIDeviceIsAssignable(dev, strict_acs_check)) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("PCI device %s is not assignable"),
                           virPCIDeviceGetName(dev));
            goto cleanup;
        }
        /* The device is in use by other active domain if
         * the dev is in list activePciHostdevs.
         */
        if ((other = virPCIDeviceListFind(hostdev_mgr->activePciHostdevs, dev))) {
            const char *other_drvname;
            const char *other_domname;

            virPCIDeviceGetUsedBy(other, &other_drvname, &other_domname);
            if (other_drvname && other_domname)
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("PCI device %s is in use by "
                                 "driver %s, domain %s"),
                               virPCIDeviceGetName(dev),
                               other_drvname, other_domname);
            else
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("PCI device %s is already in use"),
                               virPCIDeviceGetName(dev));
            goto cleanup;
        }
    }

    /* Loop 2: detach managed devices (i.e. bind to appropriate stub driver) */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);
        if (virPCIDeviceGetManaged(dev) &&
            virPCIDeviceDetach(dev, hostdev_mgr->activePciHostdevs, NULL) < 0)
            goto reattachdevs;
    }

    /* Loop 3: Now that all the PCI hostdevs have been detached, we
     * can safely reset them */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);

        if (virPCIDeviceReset(dev, hostdev_mgr->activePciHostdevs,
                              hostdev_mgr->inactivePciHostdevs) < 0)
            goto reattachdevs;
    }

    /* Loop 4: For SRIOV network devices, Now that we have detached the
     * the network device, set the netdev config */
    for (i = 0; i < nhostdevs; i++) {
         virDomainHostdevDefPtr hostdev = hostdevs[i];
         if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
             continue;
         if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
             continue;
         if (hostdev->parent.type == VIR_DOMAIN_DEVICE_NET &&
             hostdev->parent.data.net) {
             if (virHostdevNetConfigReplace(hostdev, uuid,
                                            hostdev_mgr->stateDir) < 0) {
                 goto resetvfnetconfig;
             }
         }
         last_processed_hostdev_vf = i;
    }

    /* Loop 5: Now mark all the devices as active */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);
        if (virPCIDeviceListAdd(hostdev_mgr->activePciHostdevs, dev) < 0)
            goto inactivedevs;
    }

    /* Loop 6: Now remove the devices from inactive list. */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
         virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);
         virPCIDeviceListDel(hostdev_mgr->inactivePciHostdevs, dev);
    }

    /* Loop 7: Now set the used_by_domain of the device in
     * activePciHostdevs as domain name.
     */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev, activeDev;

        dev = virPCIDeviceListGet(pcidevs, i);
        activeDev = virPCIDeviceListFind(hostdev_mgr->activePciHostdevs, dev);

        if (activeDev)
            virPCIDeviceSetUsedBy(activeDev, drv_name, name);
    }

    /* Loop 8: Now set the original states for hostdev def */
    for (i = 0; i < nhostdevs; i++) {
        virPCIDevicePtr dev;
        virPCIDevicePtr pcidev;
        virDomainHostdevDefPtr hostdev = hostdevs[i];

        if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
            continue;
        if (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            continue;

        dev = virPCIDeviceNew(hostdev->source.subsys.u.pci.addr.domain,
                              hostdev->source.subsys.u.pci.addr.bus,
                              hostdev->source.subsys.u.pci.addr.slot,
                              hostdev->source.subsys.u.pci.addr.function);

        /* original states "unbind_from_stub", "remove_slot",
         * "reprobe" were already set by pciDettachDevice in
         * loop 2.
         */
        if ((pcidev = virPCIDeviceListFind(pcidevs, dev))) {
            hostdev->origstates.states.pci.unbind_from_stub =
                virPCIDeviceGetUnbindFromStub(pcidev);
            hostdev->origstates.states.pci.remove_slot =
                virPCIDeviceGetRemoveSlot(pcidev);
            hostdev->origstates.states.pci.reprobe =
                virPCIDeviceGetReprobe(pcidev);
        }

        virPCIDeviceFree(dev);
    }

    /* Loop 9: Now steal all the devices from pcidevs */
    while (virPCIDeviceListCount(pcidevs) > 0)
        virPCIDeviceListStealIndex(pcidevs, 0);

    ret = 0;
    goto cleanup;

inactivedevs:
    /* Only steal all the devices from activePciHostdevs. We will
     * free them in virObjectUnref().
     */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);
        virPCIDeviceListSteal(hostdev_mgr->activePciHostdevs, dev);
    }

resetvfnetconfig:
    for (i = 0;
         last_processed_hostdev_vf != -1 && i < last_processed_hostdev_vf; i++)
        virHostdevNetConfigRestore(hostdevs[i], hostdev_mgr->stateDir, NULL);

reattachdevs:
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);

        /* NB: This doesn't actually re-bind to original driver, just
         * unbinds from the stub driver
         */
        ignore_value(virPCIDeviceReattach(dev, hostdev_mgr->activePciHostdevs,
                                          NULL));
    }

cleanup:
    virObjectUnlock(hostdev_mgr->activePciHostdevs);
    virObjectUnlock(hostdev_mgr->inactivePciHostdevs);
    virObjectUnref(pcidevs);
    return ret;
}

/*
 * Pre-condition: inactivePciHostdevs & activePciHostdevs
 * are locked
 */
static void
virHostdevReattachPciDevice(virPCIDevicePtr dev, virHostdevManagerPtr mgr)
{
    /* If the device is not managed and was attached to guest
     * successfully, it must have been inactive.
     */
    if (!virPCIDeviceGetManaged(dev)) {
        if (virPCIDeviceListAdd(mgr->inactivePciHostdevs, dev) < 0)
            virPCIDeviceFree(dev);
        return;
    }

    /* Wait for device cleanup if it is qemu/kvm */
    if (STREQ(virPCIDeviceGetStubDriver(dev), "pci-stub")) {
        int retries = 100;
        while (virPCIDeviceWaitForCleanup(dev, "kvm_assigned_device")
               && retries) {
            usleep(100*1000);
            retries--;
        }
    }

    if (virPCIDeviceReattach(dev, mgr->activePciHostdevs,
                             mgr->inactivePciHostdevs) < 0) {
        virErrorPtr err = virGetLastError();
        VIR_ERROR(_("Failed to re-attach PCI device: %s"),
                  err ? err->message : _("unknown error"));
        virResetError(err);
    }
    virPCIDeviceFree(dev);
}

/* @oldStateDir:
 * For upgrade purpose: see virHostdevNetConfigRestore
 */
void
virHostdevReAttachPCIDevices(virHostdevManagerPtr hostdev_mgr,
                             const char *drv_name,
                             const char *name,
                             virDomainHostdevDefPtr *hostdevs,
                             int nhostdevs,
                             char *oldStateDir)
{
    virPCIDeviceListPtr pcidevs;
    size_t i;

    virObjectLock(hostdev_mgr->activePciHostdevs);
    virObjectLock(hostdev_mgr->inactivePciHostdevs);

    if (!(pcidevs = virHostdevGetActivePciHostDeviceList(hostdev_mgr,
                                                         hostdevs,
                                                         nhostdevs))) {
        virErrorPtr err = virGetLastError();
        VIR_ERROR(_("Failed to allocate PCI device list: %s"),
                  err ? err->message : _("unknown error"));
        virResetError(err);
        goto cleanup;
    }

    /* Again 4 loops; mark all devices as inactive before reset
     * them and reset all the devices before re-attach.
     * Attach mac and port profile parameters to devices
     */
    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);
        virPCIDevicePtr activeDev = NULL;

        /* delete the copy of the dev from pcidevs if it's used by
         * other domain. Or delete it from activePciHostDevs if it had
         * been used by this domain.
         */
        activeDev = virPCIDeviceListFind(hostdev_mgr->activePciHostdevs, dev);
        if (activeDev) {
            const char *usedby_drvname;
            const char *usedby_domname;
            virPCIDeviceGetUsedBy(activeDev, &usedby_drvname, &usedby_domname);
            if (STRNEQ_NULLABLE(drv_name, usedby_drvname) ||
                STRNEQ_NULLABLE(name, usedby_domname)) {
                    virPCIDeviceListDel(pcidevs, dev);
                    continue;
                }
        }

        virPCIDeviceListDel(hostdev_mgr->activePciHostdevs, dev);
    }

    /* At this point, any device that had been used by the guest is in
     * pcidevs, but has been removed from activePciHostdevs.
     */

    /*
     * For SRIOV net host devices, unset mac and port profile before
     * reset and reattach device
     */
    for (i = 0; i < nhostdevs; i++)
        virHostdevNetConfigRestore(hostdevs[i], hostdev_mgr->stateDir,
                                   oldStateDir);

    for (i = 0; i < virPCIDeviceListCount(pcidevs); i++) {
        virPCIDevicePtr dev = virPCIDeviceListGet(pcidevs, i);

        if (virPCIDeviceReset(dev, hostdev_mgr->activePciHostdevs,
                              hostdev_mgr->inactivePciHostdevs) < 0) {
            virErrorPtr err = virGetLastError();
            VIR_ERROR(_("Failed to reset PCI device: %s"),
                      err ? err->message : _("unknown error"));
            virResetError(err);
        }
    }

    while (virPCIDeviceListCount(pcidevs) > 0) {
        virPCIDevicePtr dev = virPCIDeviceListStealIndex(pcidevs, 0);
        virHostdevReattachPciDevice(dev, hostdev_mgr);
    }

    virObjectUnref(pcidevs);
cleanup:
    virObjectUnlock(hostdev_mgr->activePciHostdevs);
    virObjectUnlock(hostdev_mgr->inactivePciHostdevs);
}
