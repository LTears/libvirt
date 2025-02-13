/*
 * nwfilter_driver.c: core driver for network filter APIs
 *                    (based on storage_driver.c)
 *
 * Copyright (C) 2006-2011, 2014 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
 * Copyright (C) 2010 IBM Corporation
 * Copyright (C) 2010 Stefan Berger
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
 */

#include <config.h>

#include "virgdbus.h"
#include "virlog.h"

#include "internal.h"

#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "domain_conf.h"
#include "domain_nwfilter.h"
#include "nwfilter_driver.h"
#include "nwfilter_gentech_driver.h"
#include "configmake.h"
#include "virfile.h"
#include "virpidfile.h"
#include "virstring.h"
#include "viraccessapicheck.h"

#include "nwfilter_ipaddrmap.h"
#include "nwfilter_dhcpsnoop.h"
#include "nwfilter_learnipaddr.h"

#define VIR_FROM_THIS VIR_FROM_NWFILTER

VIR_LOG_INIT("nwfilter.nwfilter_driver");


static virNWFilterDriverState *driver;

static int nwfilterStateCleanup(void);

static int nwfilterStateReload(void);

static void nwfilterDriverLock(void)
{
    virMutexLock(&driver->lock);
}
static void nwfilterDriverUnlock(void)
{
    virMutexUnlock(&driver->lock);
}

#ifdef WITH_FIREWALLD

static void
nwfilterFirewalldDBusSignalCallback(GDBusConnection *connection G_GNUC_UNUSED,
                                    const char *senderName G_GNUC_UNUSED,
                                    const char *objectPath G_GNUC_UNUSED,
                                    const char *interfaceName G_GNUC_UNUSED,
                                    const char *signalName G_GNUC_UNUSED,
                                    GVariant *parameters G_GNUC_UNUSED,
                                    gpointer user_data G_GNUC_UNUSED)
{
    nwfilterStateReload();
}

static unsigned int restartID;
static unsigned int reloadID;

static void
nwfilterDriverRemoveDBusMatches(void)
{
    GDBusConnection *sysbus;

    sysbus = virGDBusGetSystemBus();
    if (sysbus) {
        g_dbus_connection_signal_unsubscribe(sysbus, restartID);
        g_dbus_connection_signal_unsubscribe(sysbus, reloadID);
    }
}

/**
 * virNWFilterDriverInstallDBusMatches
 *
 * Startup DBus matches for monitoring the state of firewalld
 */
static void
nwfilterDriverInstallDBusMatches(GDBusConnection *sysbus)
{
    restartID = g_dbus_connection_signal_subscribe(sysbus,
                                                   NULL,
                                                   "org.freedesktop.DBus",
                                                   "NameOwnerChanged",
                                                   NULL,
                                                   "org.fedoraproject.FirewallD1",
                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                   nwfilterFirewalldDBusSignalCallback,
                                                   NULL,
                                                   NULL);
    reloadID = g_dbus_connection_signal_subscribe(sysbus,
                                                  NULL,
                                                  "org.fedoraproject.FirewallD1",
                                                  "Reloaded",
                                                  NULL,
                                                  NULL,
                                                  G_DBUS_SIGNAL_FLAGS_NONE,
                                                  nwfilterFirewalldDBusSignalCallback,
                                                  NULL,
                                                  NULL);
}

#else /* WITH_FIREWALLD */

static void
nwfilterDriverRemoveDBusMatches(void)
{
}

static void
nwfilterDriverInstallDBusMatches(GDBusConnection *sysbus G_GNUC_UNUSED)
{
}

#endif /* WITH_FIREWALLD */

static int
virNWFilterTriggerRebuildImpl(void *opaque)
{
    virNWFilterDriverState *nwdriver = opaque;

    return virNWFilterBuildAll(nwdriver, true);
}


/**
 * nwfilterStateInitialize:
 *
 * Initialization function for the QEMU daemon
 */
static int
nwfilterStateInitialize(bool privileged,
                        const char *root,
                        virStateInhibitCallback callback G_GNUC_UNUSED,
                        void *opaque G_GNUC_UNUSED)
{
    GDBusConnection *sysbus = NULL;

    if (root != NULL) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Driver does not support embedded mode"));
        return -1;
    }

    if (virGDBusHasSystemBus() &&
        !(sysbus = virGDBusGetSystemBus()))
        return VIR_DRV_STATE_INIT_ERROR;

    driver = g_new0(virNWFilterDriverState, 1);

    driver->lockFD = -1;
    if (virMutexInit(&driver->lock) < 0)
        goto err_free_driverstate;

    driver->privileged = privileged;
    if (!(driver->nwfilters = virNWFilterObjListNew()))
        goto error;

    if (!(driver->bindings = virNWFilterBindingObjListNew()))
        goto error;

    if (!privileged)
        return VIR_DRV_STATE_INIT_SKIPPED;

    nwfilterDriverLock();

    driver->stateDir = g_strdup(RUNSTATEDIR "/libvirt/nwfilter");

    if (g_mkdir_with_parents(driver->stateDir, S_IRWXU) < 0) {
        virReportSystemError(errno, _("cannot create state directory '%s'"),
                             driver->stateDir);
        goto error;
    }

    if ((driver->lockFD =
         virPidFileAcquire(driver->stateDir, "driver", false, getpid())) < 0)
        goto error;

    if (virNWFilterIPAddrMapInit() < 0)
        goto err_free_driverstate;
    if (virNWFilterLearnInit() < 0)
        goto err_exit_ipaddrmapshutdown;
    if (virNWFilterDHCPSnoopInit() < 0)
        goto err_exit_learnshutdown;

    if (virNWFilterTechDriversInit(privileged) < 0)
        goto err_dhcpsnoop_shutdown;

    if (virNWFilterConfLayerInit(virNWFilterTriggerRebuildImpl,
                                 driver) < 0)
        goto err_techdrivers_shutdown;

    /*
     * startup the DBus late so we don't get a reload signal while
     * initializing
     */
    if (sysbus)
        nwfilterDriverInstallDBusMatches(sysbus);

    driver->configDir = g_strdup(SYSCONFDIR "/libvirt/nwfilter");

    if (g_mkdir_with_parents(driver->configDir, S_IRWXU) < 0) {
        virReportSystemError(errno, _("cannot create config directory '%s'"),
                             driver->configDir);
        goto error;
    }

    driver->bindingDir = g_strdup(RUNSTATEDIR "/libvirt/nwfilter-binding");

    if (g_mkdir_with_parents(driver->bindingDir, S_IRWXU) < 0) {
        virReportSystemError(errno, _("cannot create config directory '%s'"),
                             driver->bindingDir);
        goto error;
    }

    if (virNWFilterObjListLoadAllConfigs(driver->nwfilters, driver->configDir) < 0)
        goto error;

    if (virNWFilterBindingObjListLoadAllConfigs(driver->bindings, driver->bindingDir) < 0)
        goto error;

    if (virNWFilterBuildAll(driver, false) < 0)
        goto error;

    nwfilterDriverUnlock();

    return VIR_DRV_STATE_INIT_COMPLETE;

 error:
    nwfilterDriverUnlock();
    nwfilterStateCleanup();

    return VIR_DRV_STATE_INIT_ERROR;

 err_techdrivers_shutdown:
    virNWFilterTechDriversShutdown();
 err_dhcpsnoop_shutdown:
    virNWFilterDHCPSnoopShutdown();
 err_exit_learnshutdown:
    virNWFilterLearnShutdown();
 err_exit_ipaddrmapshutdown:
    virNWFilterIPAddrMapShutdown();

 err_free_driverstate:
    virNWFilterObjListFree(driver->nwfilters);
    g_clear_pointer(&driver, g_free);

    return VIR_DRV_STATE_INIT_ERROR;
}

/**
 * nwfilterStateReload:
 *
 * Function to restart the nwfilter driver, it will recheck the configuration
 * files and update its state
 */
static int
nwfilterStateReload(void)
{
    if (!driver)
        return -1;

    if (!driver->privileged)
        return 0;

    virNWFilterDHCPSnoopEnd(NULL);
    /* shut down all threads -- they will be restarted if necessary */
    virNWFilterLearnThreadsTerminate(true);

    nwfilterDriverLock();
    virNWFilterWriteLockFilterUpdates();

    virNWFilterObjListLoadAllConfigs(driver->nwfilters, driver->configDir);

    virNWFilterUnlockFilterUpdates();

    virNWFilterBuildAll(driver, false);

    nwfilterDriverUnlock();

    return 0;
}


/**
 * nwfilterStateCleanup:
 *
 * Shutdown the nwfilter driver, it will stop all active nwfilters
 */
static int
nwfilterStateCleanup(void)
{
    if (!driver)
        return -1;

    if (driver->privileged) {
        virNWFilterConfLayerShutdown();
        virNWFilterDHCPSnoopShutdown();
        virNWFilterLearnShutdown();
        virNWFilterIPAddrMapShutdown();
        virNWFilterTechDriversShutdown();

        nwfilterDriverLock();

        nwfilterDriverRemoveDBusMatches();

        if (driver->lockFD != -1)
            virPidFileRelease(driver->stateDir, "driver", driver->lockFD);

        g_free(driver->stateDir);
        g_free(driver->configDir);
        g_free(driver->bindingDir);
        nwfilterDriverUnlock();
    }

    virObjectUnref(driver->bindings);

    /* free inactive nwfilters */
    virNWFilterObjListFree(driver->nwfilters);

    virMutexDestroy(&driver->lock);
    g_clear_pointer(&driver, g_free);

    return 0;
}


static virDrvOpenStatus
nwfilterConnectOpen(virConnectPtr conn,
                    virConnectAuthPtr auth G_GNUC_UNUSED,
                    virConf *conf G_GNUC_UNUSED,
                    unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (driver == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("nwfilter state driver is not active"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (STRNEQ(conn->uri->path, "/system")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected nwfilter URI path '%s', try nwfilter:///system"),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }

    if (virConnectOpenEnsureACL(conn) < 0)
        return VIR_DRV_OPEN_ERROR;

    return VIR_DRV_OPEN_SUCCESS;
}

static int nwfilterConnectClose(virConnectPtr conn G_GNUC_UNUSED)
{
    return 0;
}


static int nwfilterConnectIsSecure(virConnectPtr conn G_GNUC_UNUSED)
{
    /* Trivially secure, since always inside the daemon */
    return 1;
}


static int nwfilterConnectIsEncrypted(virConnectPtr conn G_GNUC_UNUSED)
{
    /* Not encrypted, but remote driver takes care of that */
    return 0;
}


static int nwfilterConnectIsAlive(virConnectPtr conn G_GNUC_UNUSED)
{
    return 1;
}


static virNWFilterObj *
nwfilterObjFromNWFilter(const unsigned char *uuid)
{
    virNWFilterObj *obj;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!(obj = virNWFilterObjListFindByUUID(driver->nwfilters, uuid))) {
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_NWFILTER,
                       _("no nwfilter with matching uuid '%s'"), uuidstr);
    }
    return obj;
}


static virNWFilterPtr
nwfilterLookupByUUID(virConnectPtr conn,
                     const unsigned char *uuid)
{
    virNWFilterObj *obj;
    virNWFilterDef *def;
    virNWFilterPtr nwfilter = NULL;

    nwfilterDriverLock();
    obj = nwfilterObjFromNWFilter(uuid);
    nwfilterDriverUnlock();

    if (!obj)
        return NULL;
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterLookupByUUIDEnsureACL(conn, def) < 0)
        goto cleanup;

    nwfilter = virGetNWFilter(conn, def->name, def->uuid);

 cleanup:
    virNWFilterObjUnlock(obj);
    return nwfilter;
}


static virNWFilterPtr
nwfilterLookupByName(virConnectPtr conn,
                     const char *name)
{
    virNWFilterObj *obj;
    virNWFilterDef *def;
    virNWFilterPtr nwfilter = NULL;

    nwfilterDriverLock();
    obj = virNWFilterObjListFindByName(driver->nwfilters, name);
    nwfilterDriverUnlock();

    if (!obj) {
        virReportError(VIR_ERR_NO_NWFILTER,
                       _("no nwfilter with matching name '%s'"), name);
        return NULL;
    }
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterLookupByNameEnsureACL(conn, def) < 0)
        goto cleanup;

    nwfilter = virGetNWFilter(conn, def->name, def->uuid);

 cleanup:
    virNWFilterObjUnlock(obj);
    return nwfilter;
}


static int
nwfilterConnectNumOfNWFilters(virConnectPtr conn)
{
    if (virConnectNumOfNWFiltersEnsureACL(conn) < 0)
        return -1;

    return virNWFilterObjListNumOfNWFilters(driver->nwfilters, conn,
                                        virConnectNumOfNWFiltersCheckACL);
}


static int
nwfilterConnectListNWFilters(virConnectPtr conn,
                             char **const names,
                             int maxnames)
{
    int nnames;

    if (virConnectListNWFiltersEnsureACL(conn) < 0)
        return -1;

    nwfilterDriverLock();
    nnames = virNWFilterObjListGetNames(driver->nwfilters, conn,
                                    virConnectListNWFiltersCheckACL,
                                    names, maxnames);
    nwfilterDriverUnlock();
    return nnames;
}


static int
nwfilterConnectListAllNWFilters(virConnectPtr conn,
                                virNWFilterPtr **nwfilters,
                                unsigned int flags)
{
    int ret;

    virCheckFlags(0, -1);

    if (virConnectListAllNWFiltersEnsureACL(conn) < 0)
        return -1;

    nwfilterDriverLock();
    ret = virNWFilterObjListExport(conn, driver->nwfilters, nwfilters,
                                   virConnectListAllNWFiltersCheckACL);
    nwfilterDriverUnlock();

    return ret;
}


static virNWFilterPtr
nwfilterDefineXMLFlags(virConnectPtr conn,
                       const char *xml,
                       unsigned int flags)
{
    virNWFilterDef *def;
    virNWFilterObj *obj = NULL;
    virNWFilterDef *objdef;
    virNWFilterPtr nwfilter = NULL;

    virCheckFlags(VIR_NWFILTER_DEFINE_VALIDATE, NULL);


    if (!driver->privileged) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Can't define NWFilters in session mode"));
        return NULL;
    }

    nwfilterDriverLock();
    virNWFilterWriteLockFilterUpdates();

    if (!(def = virNWFilterDefParseString(xml, flags)))
        goto cleanup;

    if (virNWFilterDefineXMLFlagsEnsureACL(conn, def) < 0)
        goto cleanup;

    if (!(obj = virNWFilterObjListAssignDef(driver->nwfilters, def)))
        goto cleanup;
    def = NULL;
    objdef = virNWFilterObjGetDef(obj);

    if (virNWFilterSaveConfig(driver->configDir, objdef) < 0) {
        virNWFilterObjListRemove(driver->nwfilters, obj);
        goto cleanup;
    }

    nwfilter = virGetNWFilter(conn, objdef->name, objdef->uuid);

 cleanup:
    virNWFilterDefFree(def);
    if (obj)
        virNWFilterObjUnlock(obj);

    virNWFilterUnlockFilterUpdates();
    nwfilterDriverUnlock();
    return nwfilter;
}


static virNWFilterPtr
nwfilterDefineXML(virConnectPtr conn,
                  const char *xml)
{
    return nwfilterDefineXMLFlags(conn, xml, 0);
}


static int
nwfilterUndefine(virNWFilterPtr nwfilter)
{
    virNWFilterObj *obj;
    virNWFilterDef *def;
    int ret = -1;

    nwfilterDriverLock();
    virNWFilterWriteLockFilterUpdates();

    if (!(obj = nwfilterObjFromNWFilter(nwfilter->uuid)))
        goto cleanup;
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterUndefineEnsureACL(nwfilter->conn, def) < 0)
        goto cleanup;

    if (virNWFilterObjTestUnassignDef(obj) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s",
                       _("nwfilter is in use"));
        goto cleanup;
    }

    if (virNWFilterDeleteDef(driver->configDir, def) < 0)
        goto cleanup;

    virNWFilterObjListRemove(driver->nwfilters, obj);
    obj = NULL;
    ret = 0;

 cleanup:
    if (obj)
        virNWFilterObjUnlock(obj);

    virNWFilterUnlockFilterUpdates();
    nwfilterDriverUnlock();
    return ret;
}


static char *
nwfilterGetXMLDesc(virNWFilterPtr nwfilter,
                   unsigned int flags)
{
    virNWFilterObj *obj;
    virNWFilterDef *def;
    char *ret = NULL;

    virCheckFlags(0, NULL);

    nwfilterDriverLock();
    obj = nwfilterObjFromNWFilter(nwfilter->uuid);
    nwfilterDriverUnlock();

    if (!obj)
        return NULL;
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterGetXMLDescEnsureACL(nwfilter->conn, def) < 0)
        goto cleanup;

    ret = virNWFilterDefFormat(def);

 cleanup:
    virNWFilterObjUnlock(obj);
    return ret;
}


static virNWFilterBindingPtr
nwfilterBindingLookupByPortDev(virConnectPtr conn,
                               const char *portdev)
{
    virNWFilterBindingPtr ret = NULL;
    virNWFilterBindingObj *obj;
    virNWFilterBindingDef *def;

    obj = virNWFilterBindingObjListFindByPortDev(driver->bindings,
                                                 portdev);
    if (!obj) {
        virReportError(VIR_ERR_NO_NWFILTER_BINDING,
                       _("no nwfilter binding for port dev '%s'"), portdev);
        goto cleanup;
    }

    def = virNWFilterBindingObjGetDef(obj);
    if (virNWFilterBindingLookupByPortDevEnsureACL(conn, def) < 0)
        goto cleanup;

    ret = virGetNWFilterBinding(conn, def->portdevname, def->filter);

 cleanup:
    virNWFilterBindingObjEndAPI(&obj);
    return ret;
}


static int
nwfilterConnectListAllNWFilterBindings(virConnectPtr conn,
                                       virNWFilterBindingPtr **bindings,
                                       unsigned int flags)
{
    virCheckFlags(0, -1);

    if (virConnectListAllNWFilterBindingsEnsureACL(conn) < 0)
        return -1;

    return virNWFilterBindingObjListExport(driver->bindings, conn, bindings,
                                           virConnectListAllNWFilterBindingsCheckACL);
}


static char *
nwfilterBindingGetXMLDesc(virNWFilterBindingPtr binding,
                          unsigned int flags)
{
    virNWFilterBindingObj *obj;
    virNWFilterBindingDef *def;
    char *ret = NULL;

    virCheckFlags(0, NULL);

    obj = virNWFilterBindingObjListFindByPortDev(driver->bindings,
                                                 binding->portdev);
    if (!obj) {
        virReportError(VIR_ERR_NO_NWFILTER_BINDING,
                       _("no nwfilter binding for port dev '%s'"), binding->portdev);
        goto cleanup;
    }

    def = virNWFilterBindingObjGetDef(obj);
    if (virNWFilterBindingGetXMLDescEnsureACL(binding->conn, def) < 0)
        goto cleanup;

    ret = virNWFilterBindingDefFormat(def);

 cleanup:
    virNWFilterBindingObjEndAPI(&obj);
    return ret;
}


static virNWFilterBindingPtr
nwfilterBindingCreateXML(virConnectPtr conn,
                         const char *xml,
                         unsigned int flags)
{
    virNWFilterBindingDef *def;
    virNWFilterBindingObj *obj = NULL;
    virNWFilterBindingPtr ret = NULL;

    virCheckFlags(0, NULL);

    if (!driver->privileged) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Can't define NWFilter bindings in session mode"));
        return NULL;
    }

    def = virNWFilterBindingDefParseString(xml);
    if (!def)
        return NULL;

    if (virNWFilterBindingCreateXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    obj = virNWFilterBindingObjListAdd(driver->bindings,
                                       def);
    if (!obj)
        goto cleanup;

    if (!(ret = virGetNWFilterBinding(conn, def->portdevname, def->filter)))
        goto cleanup;

    if (virNWFilterInstantiateFilter(driver, def) < 0) {
        virNWFilterBindingObjListRemove(driver->bindings, obj);
        virObjectUnref(ret);
        ret = NULL;
        goto cleanup;
    }
    virNWFilterBindingObjSave(obj, driver->bindingDir);

 cleanup:
    if (!obj)
        virNWFilterBindingDefFree(def);
    virNWFilterBindingObjEndAPI(&obj);

    return ret;
}


/*
 * Note that this is primarily intended for usage by the hypervisor
 * drivers. it is exposed to the admin, however, and nothing stops
 * an admin from deleting filter bindings created by the hypervisor
 * drivers. IOW, it is the admin's responsibility not to shoot
 * themself in the foot
 */
static int
nwfilterBindingDelete(virNWFilterBindingPtr binding)
{
    virNWFilterBindingObj *obj;
    virNWFilterBindingDef *def;
    int ret = -1;

    obj = virNWFilterBindingObjListFindByPortDev(driver->bindings, binding->portdev);
    if (!obj) {
        virReportError(VIR_ERR_NO_NWFILTER_BINDING,
                       _("no nwfilter binding for port dev '%s'"), binding->portdev);
        return -1;
    }

    def = virNWFilterBindingObjGetDef(obj);
    if (virNWFilterBindingDeleteEnsureACL(binding->conn, def) < 0)
        goto cleanup;

    virNWFilterTeardownFilter(def);
    virNWFilterBindingObjDelete(obj, driver->bindingDir);
    virNWFilterBindingObjListRemove(driver->bindings, obj);

    ret = 0;

 cleanup:
    virNWFilterBindingObjEndAPI(&obj);
    return ret;
}


static virNWFilterDriver nwfilterDriver = {
    .name = "nwfilter",
    .connectNumOfNWFilters = nwfilterConnectNumOfNWFilters, /* 0.8.0 */
    .connectListNWFilters = nwfilterConnectListNWFilters, /* 0.8.0 */
    .connectListAllNWFilters = nwfilterConnectListAllNWFilters, /* 0.10.2 */
    .nwfilterLookupByName = nwfilterLookupByName, /* 0.8.0 */
    .nwfilterLookupByUUID = nwfilterLookupByUUID, /* 0.8.0 */
    .nwfilterDefineXML = nwfilterDefineXML, /* 0.8.0 */
    .nwfilterDefineXMLFlags = nwfilterDefineXMLFlags, /* 7.7.0 */
    .nwfilterUndefine = nwfilterUndefine, /* 0.8.0 */
    .nwfilterGetXMLDesc = nwfilterGetXMLDesc, /* 0.8.0 */
    .nwfilterBindingLookupByPortDev = nwfilterBindingLookupByPortDev, /* 4.5.0 */
    .connectListAllNWFilterBindings = nwfilterConnectListAllNWFilterBindings, /* 4.5.0 */
    .nwfilterBindingGetXMLDesc = nwfilterBindingGetXMLDesc, /* 4.5.0 */
    .nwfilterBindingCreateXML = nwfilterBindingCreateXML, /* 4.5.0 */
    .nwfilterBindingDelete = nwfilterBindingDelete, /* 4.5.0 */
};


static virHypervisorDriver nwfilterHypervisorDriver = {
    .name = "nwfilter",
    .connectOpen = nwfilterConnectOpen, /* 4.1.0 */
    .connectClose = nwfilterConnectClose, /* 4.1.0 */
    .connectIsEncrypted = nwfilterConnectIsEncrypted, /* 4.1.0 */
    .connectIsSecure = nwfilterConnectIsSecure, /* 4.1.0 */
    .connectIsAlive = nwfilterConnectIsAlive, /* 4.1.0 */
};


static virConnectDriver nwfilterConnectDriver = {
    .localOnly = true,
    .uriSchemes = (const char *[]){ "nwfilter", NULL },
    .hypervisorDriver = &nwfilterHypervisorDriver,
    .nwfilterDriver = &nwfilterDriver,
};


static virStateDriver stateDriver = {
    .name = "NWFilter",
    .stateInitialize = nwfilterStateInitialize,
    .stateCleanup = nwfilterStateCleanup,
    .stateReload = nwfilterStateReload,
};

int nwfilterRegister(void)
{
    if (virRegisterConnectDriver(&nwfilterConnectDriver, false) < 0)
        return -1;
    if (virSetSharedNWFilterDriver(&nwfilterDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&stateDriver) < 0)
        return -1;
    return 0;
}
