/*
 *  * pcie_edu.c
 *  *
 *  * Copyright (c) 2017 Amith Abraham <aabraham@cray.com>
 *  *                          Cray Inc
 *  *
 *  * This program is free software; you can redistribute it and/or modify
 *  * it under the terms of the GNU General Public License as published by
 *  * the Free Software Foundation; either version 2 of the License, or
 *  * (at your option) any later version.
 *  *
 *  * This program is distributed in the hope that it will be useful,
 *  * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  * GNU General Public License for more details.
 *  *
 *  * You should have received a copy of the GNU General Public License along
 *  * with this program; if not, see <http://www.gnu.org/licenses/>.
 *  */

/*
 *  *  Example PCIe Device w/ SR-IOV
 *  *
 *  */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "include/hw/pci/pci.h"
#include "include/hw/pci/pcie.h"
#include "qemu/event_notifier.h"
#include <time.h>
#include <inttypes.h>

/* Edit the following IDs and capability offsets as necessary */
#define PCIE_EDU_DEV_ID 0x0001    /* Arbitrary Device IDs */
#define PCIE_EDU_VF_DEV_ID 0x0002
#define PCIE_EDU_VF_OFFSET 0x80
#define PCIE_EDU_VF_STRIDE 2
#define PCIE_EDU_TOTAL_VFS 16
#define CAP_OFFSET 0xa0          /* Offset for PCIe extended Capabilities */
#define MSIX_OFFSET 0x70         /* Offst for MSIX Capability */
#define AER_OFFSET 0x100         /* Offset for Advanced Error Reporting Capability */
#define ATS_OFFSET 0x140         /* Offset for Address Translation Services Capability */
#define ARI_OFFSET 0x150         /* Offset for Alternative Routing ID Offset *
                                  * likely needed with SR-IOV, refer to Spec */
#define SRIOV_OFFSET 0x160       /* Single Root IO Virtualization Offset */

#define TYPE_PCIE_EDU "pcie_edu"
#define PCIE_EDU(obj) \
    OBJECT_CHECK(PCIE_EDUState, (obj), TYPE_PCIE_EDU)
#define TYPE_PCIE_EDU_VF "pcie_edu_vf"
#define PCIE_EDU_VF(obj) \
    OBJECT_CHECK(PCIE_EDUState, (obj), TYPE_PCIE_EDU_VF) //TODO Change PCIE_EDUState to VF State

/* Edit sizes below as necessary */
#define PCIE_EDU_MMIO_SIZE        0x80000000  /* 2GB Bar (Physical Function) */
#define PCIE_EDU_VF_MMIO_SIZE     (4  * 1024) /* 4KB Bar (Virtual Functions) */
#define PCIE_EDU_MSIX_SIZE        (16 * 1024)
#define PCIE_EDU_MSIX_VECTORS_PF  (4)
#define PCIE_EDU_MSIX_VECTORS_VF  (2)
#define PCIE_EDU_MSIX_TABLE       (0x000)
#define PCIE_EDU_MSIX_PBA         (0x800)     /*TODO Choose more appropriate value? */
#define PCIE_EDU_MSIX_BAR         2           /* Bar ID/Number for MSIX */


#define DEBUG( x ) printf ( ( x ) )

/* Temporary MSIX Workaround */
PCIDevice *physfn;

typedef struct PCIE_EDUState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    MemoryRegion msix;

} PCIE_EDUState;

//TODO Determine if we want to use this
#if 0
typedef struct PCIE_EDU_VFState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    MemoryRegion msix;

} PCIE_EDUState;
#endif 

static struct {
    int dev_id;
    int reply_ready;
    int64_t rtc;
    QEMUTimer *timer;
} ns;

#if 0
static uint32_t pcie_edu_read_config(PCIDevice *d,
                                      uint32_t address, int len)
{
    uint32_t val;
    val = pci_default_read_config(d, address, len);
    return val;
}

static void pcie_edu_write_config(PCIDevice *d, uint32_t address, uint32_t val, int len)
{
    pci_default_write_config(d, address, val, len);
}
#endif

static uint64_t pcie_edu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
//    PCIE_EDUState *s = opaque; // Will help in future to differentiate PF/VFs
//    uint64_t val = -1;
    printf("Doing mmio read.\n");
    //sim_io(1, addr, val, size);
    return 0;
}

static void pcie_edu_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
//    PCIE_EDUState *s = opaque;
    printf("Doing mmio write.\n");
    printf("addr=%" PRIx64 " ", addr);
    printf("val=%" PRIx64 " ", val);
    printf("size=%d\n", size);
    //sim_io(0, addr, val, size);
    return;
}

/* Specifies the device Memory Region callbacks. */
static const MemoryRegionOps pcie_edu_mmio_ops = {
        .read = pcie_edu_mmio_read,
        .write = pcie_edu_mmio_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
        .valid = {
            .min_access_size = 4,
            .max_access_size = 8,
        },
        .impl = {
            .min_access_size = 4,
            .max_access_size = 8,
        },
};

static void pcie_edu_pci_uninit(PCIDevice *pci_dev)
{
    PCIE_EDUState *s = PCIE_EDU(pci_dev);

    pcie_sriov_pf_exit(pci_dev);
    pcie_cap_exit(pci_dev);
    msix_unuse_all_vectors(pci_dev);
    msix_uninit(pci_dev, &s->msix, &s->msix);
    printf("Example PCIE Device unloaded\n");
}

/* Physical Function Realize. Main setup for device is done here. */
static void pcie_edu_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    int ret, v;
    PCIE_EDUState *s = PCIE_EDU(pci_dev);
    physfn = pci_dev;


    /* Define memory regions */
    memory_region_init_io(&s->bar0, OBJECT(s), &pcie_edu_mmio_ops, s,
                              "pcie_edu_mmio0", PCIE_EDU_MMIO_SIZE);
    memory_region_init(&s->msix, OBJECT(s), "pcie_edu-msix",
                       PCIE_EDU_MSIX_SIZE);

    /* Register the defined memory regions */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);
    pci_register_bar(pci_dev, PCIE_EDU_MSIX_BAR, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->msix);

    /* Enable MSIX */
    ret = msix_init(pci_dev, PCIE_EDU_MSIX_VECTORS_PF,
                    &s->msix,
                    PCIE_EDU_MSIX_BAR, PCIE_EDU_MSIX_TABLE,
                    &s->msix,
                    PCIE_EDU_MSIX_BAR, PCIE_EDU_MSIX_PBA,
                    MSIX_OFFSET, NULL);
    if (ret) {
        goto err_msix;
    }

    /* Enable each MSIX vector */
    for (v = 0; v < PCIE_EDU_MSIX_VECTORS_PF; v++) {
        ret = msix_vector_use(pci_dev, v);
        if (ret) {
            goto err_pcie_cap;
        }
    }

    /* Enable PCIe extended capabilities. This must be done before enabling AER
     * ARI, SR-IOV etc. */
    ret = pcie_endpoint_cap_init(pci_dev, CAP_OFFSET);
    if (ret < 0) {
        printf("End Point Cap init error\n");
        goto err_pcie_cap;
    }

    /* Enable Advanced Error Reporting capability */
    ret = pcie_aer_init(pci_dev, PCI_ERR_VER, AER_OFFSET, PCI_ERR_SIZEOF, NULL);
    if (ret < 0) {
        printf("AER init error\n");
        goto err_aer;
    }

    /* Enable Alternative Routing-ID Interpretation. Needed for large num of VFs */
    pcie_ari_init(pci_dev, ARI_OFFSET, 1);

    /* Enable Address Translation Services */
    pcie_ats_init(pci_dev, ATS_OFFSET);

    /* Enable SR_IOV */
    pcie_sriov_pf_init(pci_dev, SRIOV_OFFSET, "pcie_edu_vf",
                       PCIE_EDU_VF_DEV_ID, PCIE_EDU_TOTAL_VFS, PCIE_EDU_TOTAL_VFS,
                       PCIE_EDU_VF_OFFSET, PCIE_EDU_VF_STRIDE);

    /* Initialize Virtual Function Bars */
    pcie_sriov_pf_init_vf_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_64,
                              PCIE_EDU_VF_MMIO_SIZE);

    ns.rtc = 1;

    printf("Template Device loaded\n");
    return;

    err_aer:
        pcie_cap_exit(pci_dev);
    err_pcie_cap:
        msix_unuse_all_vectors(pci_dev);
        msix_uninit(pci_dev, &s->msix, &s->msix);
    err_msix:
        pcie_edu_pci_uninit(pci_dev);
}

static void pcie_edu_vf_pci_uninit(PCIDevice *pci_dev)
{
    PCIE_EDUState *s = PCIE_EDU_VF(pci_dev);

    pcie_cap_exit(pci_dev);
    msix_unuse_all_vectors(pci_dev);
    msix_uninit(pci_dev, &s->msix, &s->msix);
    printf("Template Device unloaded\n");
}

/* Virtual Function Realize */
static void pcie_edu_vf_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    int ret, v;
    PCIE_EDUState *s = PCIE_EDU_VF(pci_dev);

    /* Register VF Memory Regions */
    memory_region_init_io(&s->bar0, OBJECT(s), &pcie_edu_mmio_ops, s,
                             "pcie_edu_vf_mmio0", PCIE_EDU_VF_MMIO_SIZE);
    memory_region_init(&s->msix, OBJECT(pci_dev), "pcie_edu_vf-msix", PCIE_EDU_MSIX_SIZE);

    /* Register VF BARs - Note this uses a different method than normal BARs */
    pcie_sriov_vf_register_bar(pci_dev, 0, &s->bar0);
    pcie_sriov_vf_register_bar(pci_dev, PCIE_EDU_MSIX_BAR, &s->msix);


    /* Enable MSIX */
    ret = msix_init(pci_dev, PCIE_EDU_MSIX_VECTORS_VF,
                    &s->msix,
                    PCIE_EDU_MSIX_BAR, PCIE_EDU_MSIX_TABLE, 
                    &s->msix,
                    PCIE_EDU_MSIX_BAR, PCIE_EDU_MSIX_PBA,
                    MSIX_OFFSET, NULL);
    if (ret) {
        goto err_msix;
    }

    /* Enable each VF MSIX Vector */
    for (v = 0; v < PCIE_EDU_MSIX_VECTORS_VF; v++) {
        ret = msix_vector_use(pci_dev, v);
        if (ret) {
            goto err_pcie_cap;
        }
    }
                               
    /* Enable PCIe extended capabilities */
    ret = pcie_endpoint_cap_init(pci_dev, CAP_OFFSET);
    if (ret < 0) {
        printf("VF: End Point Cap init error\n");
        goto err_pcie_cap;
    }

    /* Enable Advanced Error Reporting capability */
    ret = pcie_aer_init(pci_dev, PCI_ERR_VER, AER_OFFSET, PCI_ERR_SIZEOF, NULL);
    if (ret < 0) {
        printf("VF: AER init error\n");
        goto err_aer;
    }

    /* Enable Alternative Routing-ID Interpretation. Needed for large num of VFs */
    pcie_ari_init(pci_dev, ARI_OFFSET, 1);

    printf("Template VF Device loaded\n");
    return;

    err_aer:
        pcie_cap_exit(pci_dev);
    err_pcie_cap:
        msix_unuse_all_vectors(pci_dev);
        msix_uninit(pci_dev, &s->msix, &s->msix);
    err_msix:
        pcie_edu_vf_pci_uninit(pci_dev);
}

static void pcie_edu_qdev_reset(DeviceState *dev)
{
    printf("Reset Template Device\n");
}

static Property pcie_edu_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void pcie_edu_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *c = PCI_DEVICE_CLASS(class);

    c->realize = pcie_edu_pci_realize;
    c->exit = pcie_edu_pci_uninit;
    c->vendor_id = 0x000017db;   /* TODO Update based on community guidelines*/
    c->device_id = PCIE_EDU_DEV_ID;
    c->revision  = 0x00;
    c->class_id = PCI_CLASS_OTHERS;
    c->is_express = 1;

    dc->desc = "Example PCIE Device";
    dc->props = pcie_edu_properties;
    dc->reset = pcie_edu_qdev_reset;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pcie_edu_vf_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *c = PCI_DEVICE_CLASS(class);

    c->realize = pcie_edu_vf_pci_realize;
    c->exit = pcie_edu_vf_pci_uninit;
    c->vendor_id = 0x000017db;   /* TODO Update based on community guidelines */
    c->device_id = PCIE_EDU_VF_DEV_ID;
    c->revision  = 0x00;
    c->class_id = PCI_CLASS_OTHERS;
    c->is_express = 1;

    dc->desc = "Example PCIE Device";
    dc->props = pcie_edu_properties;
    dc->reset = pcie_edu_qdev_reset;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pcie_edu_info = {
    .name           = TYPE_PCIE_EDU,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(PCIE_EDUState),
    .class_init     = pcie_edu_class_init,
};

static const TypeInfo pcie_edu_vf_info = {
    .name           = TYPE_PCIE_EDU_VF,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(PCIE_EDUState),
    .class_init     = pcie_edu_vf_class_init,
};

static void pcie_edu_register_types(void)
{
    type_register_static(&pcie_edu_info);
    type_register_static(&pcie_edu_vf_info);
}

/* Register device with QEMU. */
type_init(pcie_edu_register_types);
