/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include "dosbox.h"
#include "inout.h"
#include "mem.h"
#include "pci_bus.h"
#include "setup.h"
#include "debug.h"
#include "callback.h"
#include "regs.h"
#include <string.h> // for memset

#if defined(PCI_FUNCTIONALITY_ENABLED)

static Bit32u pci_caddress=0;			// current PCI addressing
static Bitu pci_devices_installed=0;	// number of registered PCI devices

static Bit8u pci_cfg_data[PCI_MAX_PCIDEVICES][PCI_MAX_PCIFUNCTIONS][256];		// PCI configuration data
static PCI_Device* pci_devices[PCI_MAX_PCIDEVICES];		// registered PCI devices


// PCI address
// 31    - set for a PCI access
// 30-24 - 0
// 23-16 - bus number			(0x00ff0000)
// 15-11 - device number (slot)	(0x0000f800)
// 10- 8 - subfunction number	(0x00000700)
//  7- 2 - config register #	(0x000000fc)

static void write_pci_addr(Bitu port,Bitu val,Bitu iolen) {
	LOG(LOG_PCI,LOG_NORMAL)("Write PCI address :=%x",val);
	pci_caddress=(Bit32u)val;
}

static void write_pci_register(PCI_Device* dev,Bit8u regnum,Bit8u value) {
	// vendor/device/class IDs/header type/etc. are read-only
	if ((regnum<0x04) || ((regnum>=0x06) && (regnum<0x0c)) || (regnum==0x0e)) return;
	if (dev==NULL) return;
	switch (pci_cfg_data[dev->PCIId()][dev->PCISubfunction()][0x0e]&0x7f) {	// header-type specific handling
		case 0x00:
			if ((regnum>=0x28) && (regnum<0x30)) return;	// subsystem information is read-only
			break;
		case 0x01:
		case 0x02:
		default:
			break;
	}

	// call device routine for special actions and the
	// possibility to discard/replace the value that is to be written
	Bits parsed_register=dev->ParseWriteRegister(regnum,value);
	if (parsed_register>=0)
		pci_cfg_data[dev->PCIId()][dev->PCISubfunction()][regnum]=(Bit8u)(parsed_register&0xff);
}

static void write_pci(Bitu port,Bitu val,Bitu iolen) {
	LOG(LOG_PCI,LOG_NORMAL)("Write PCI data :=%x (len %d)",port,val,iolen);

	// check for enabled/bus 0
	if ((pci_caddress & 0x80ff0000) == 0x80000000) {
		Bit8u devnum = (Bit8u)((pci_caddress >> 11) & 0x1f);
		Bit8u fctnum = (Bit8u)((pci_caddress >> 8) & 0x7);
		Bit8u regnum = (Bit8u)((pci_caddress & 0xfc) + (port & 0x03));
		LOG(LOG_PCI,LOG_NORMAL)("  Write to device %x register %x (function %x) (:=%x)",devnum,regnum,fctnum,val);

		if (devnum>=pci_devices_installed) return;
		PCI_Device* masterdev=pci_devices[devnum];
		if (masterdev==NULL) return;
		if (fctnum>masterdev->NumSubdevices()) return;

		PCI_Device* dev=masterdev->GetSubdevice(fctnum);
		if (dev==NULL) return;

		// write data to PCI device/configuration
		switch (iolen) {
			case 1: write_pci_register(dev,regnum+0,(Bit8u)(val&0xff)); break;
			case 2: write_pci_register(dev,regnum+0,(Bit8u)(val&0xff));
					write_pci_register(dev,regnum+1,(Bit8u)((val>>8)&0xff)); break;
			case 4: write_pci_register(dev,regnum+0,(Bit8u)(val&0xff));
					write_pci_register(dev,regnum+1,(Bit8u)((val>>8)&0xff));
					write_pci_register(dev,regnum+2,(Bit8u)((val>>16)&0xff));
					write_pci_register(dev,regnum+3,(Bit8u)((val>>24)&0xff)); break;
		}
	}
}


static Bitu read_pci_addr(Bitu port,Bitu iolen) {
	LOG(LOG_PCI,LOG_NORMAL)("Read PCI address -> %x",pci_caddress);
	return pci_caddress;
}

// read single 8bit value from register file (special register treatment included)
static Bit8u read_pci_register(PCI_Device* dev,Bit8u regnum) {
	switch (regnum) {
		case 0x00:
			return (Bit8u)(dev->VendorID()&0xff);
		case 0x01:
			return (Bit8u)((dev->VendorID()>>8)&0xff);
		case 0x02:
			return (Bit8u)(dev->DeviceID()&0xff);
		case 0x03:
			return (Bit8u)((dev->DeviceID()>>8)&0xff);

		case 0x0e:
			return (pci_cfg_data[dev->PCIId()][dev->PCISubfunction()][regnum]&0x7f) |
						((dev->NumSubdevices()>0)?0x80:0x00);
		default:
			break;
	}

	// call device routine for special actions and possibility to discard/remap register
	Bits parsed_regnum=dev->ParseReadRegister(regnum);
	if ((parsed_regnum>=0) && (parsed_regnum<256))
		return pci_cfg_data[dev->PCIId()][dev->PCISubfunction()][parsed_regnum];

	Bit8u newval, mask;
	if (dev->OverrideReadRegister(regnum, &newval, &mask)) {
		Bit8u oldval=pci_cfg_data[dev->PCIId()][dev->PCISubfunction()][regnum] & (~mask);
		return oldval | (newval & mask);
	}

	return 0xff;
}

static Bitu read_pci(Bitu port,Bitu iolen) {
	LOG(LOG_PCI,LOG_NORMAL)("Read PCI data -> %x",pci_caddress);

	if ((pci_caddress & 0x80ff0000) == 0x80000000) {
		Bit8u devnum = (Bit8u)((pci_caddress >> 11) & 0x1f);
		Bit8u fctnum = (Bit8u)((pci_caddress >> 8) & 0x7);
		Bit8u regnum = (Bit8u)((pci_caddress & 0xfc) + (port & 0x03));
		if (devnum>=pci_devices_installed) return 0xffffffff;
		LOG(LOG_PCI,LOG_NORMAL)("  Read from device %x register %x (function %x); addr %x",
			devnum,regnum,fctnum,pci_caddress);

		PCI_Device* masterdev=pci_devices[devnum];
		if (masterdev==NULL) return 0xffffffff;
		if (fctnum>masterdev->NumSubdevices()) return 0xffffffff;

		PCI_Device* dev=masterdev->GetSubdevice(fctnum);

		if (dev!=NULL) {
			switch (iolen) {
				case 1:
					{
						Bit8u val8=read_pci_register(dev,regnum);
						return val8;
					}
				case 2:
					{
						Bit16u val16=read_pci_register(dev,regnum);
						val16|=(read_pci_register(dev,regnum+1)<<8);
						return val16;
					}
				case 4:
					{
						Bit32u val32=read_pci_register(dev,regnum);
						val32|=(read_pci_register(dev,regnum+1)<<8);
						val32|=(read_pci_register(dev,regnum+2)<<16);
						val32|=(read_pci_register(dev,regnum+3)<<24);
						return val32;
					}
				default:
					break;
			}
		}
	}
	return 0xffffffff;
}


static Bitu PCI_PM_Handler() {
	LOG_MSG("PCI PMode handler, function %x",reg_ax);
	return CBRET_NONE;
}


PCI_Device::PCI_Device(Bit16u vendor, Bit16u device) {
	pci_id=-1;
	pci_subfunction=-1;
	vendor_id=vendor;
	device_id=device;
	num_subdevices=0;
	//DBP: Replaced with memset
	//for (Bitu dct=0;dct<PCI_MAX_PCIFUNCTIONS-1;dct++) subdevices[dct]=0;
	memset(subdevices, 0, sizeof(subdevices));
}

void PCI_Device::SetPCIId(Bitu number, Bits subfct) {
	DBP_ASSERT((number>=0) && (number<PCI_MAX_PCIDEVICES) && (subfct>=0) && (subfct<PCI_MAX_PCIFUNCTIONS-1));
	if ((number>=0) && (number<PCI_MAX_PCIDEVICES)) {
		pci_id=number;
		if ((subfct>=0) && (subfct<PCI_MAX_PCIFUNCTIONS-1))
			pci_subfunction=subfct;
		else
			pci_subfunction=-1;
	}
}

bool PCI_Device::AddSubdevice(PCI_Device* dev) {
	if (num_subdevices<PCI_MAX_PCIFUNCTIONS-1) {
		if (subdevices[num_subdevices]!=NULL) E_Exit("PCI subdevice slot already in use!");
		subdevices[num_subdevices]=dev;
		num_subdevices++;
		return true;
	}
	DBP_ASSERT(false);
	return false;
}

#if 0 //DBP: Can't just delete things that maybe weren't allocated with new, also PCI_Device is missing virtual destructor
void PCI_Device::RemoveSubdevice(Bits subfct) {
	if ((subfct>0) && (subfct<PCI_MAX_PCIFUNCTIONS)) {
		if (subfct<=this->NumSubdevices()) {
			delete subdevices[subfct-1];
			subdevices[subfct-1]=NULL;
			// should adjust things like num_subdevices as well...
		}
	}
}
#endif

PCI_Device* PCI_Device::GetSubdevice(Bits subfct) {
	if (subfct>=PCI_MAX_PCIFUNCTIONS) return NULL;
	if (subfct>0) {
		if (subfct<=this->NumSubdevices()) return subdevices[subfct-1];
	} else if (subfct==0) {
		return this;
	}
	return NULL;
}


// queued devices (PCI device registering requested before the PCI framework was initialized)
static const Bitu max_rqueued_devices=16;
static Bitu num_rqueued_devices=0;
static PCI_Device* rqueued_devices[max_rqueued_devices];


#include "pci_devices.h"

class PCI:public Module_base{
private:
	bool initialized;

protected:
	IO_WriteHandleObject PCI_WriteHandler[5];
	IO_ReadHandleObject PCI_ReadHandler[5];

	CALLBACK_HandlerObject callback_pci;

public:

	PhysPt GetPModeCallbackPointer(void) {
		return Real2Phys(callback_pci.Get_RealPointer());
	}

	bool IsInitialized(void) {
		return initialized;
	}

	// set up port handlers and configuration data
	void InitializePCI(void) {
		// install PCI-addressing ports
		PCI_WriteHandler[0].Install(0xcf8,write_pci_addr,IO_MD);
		PCI_ReadHandler[0].Install(0xcf8,read_pci_addr,IO_MD);
		// install PCI-register read/write handlers
		for (Bitu ct=0;ct<4;ct++) {
			PCI_WriteHandler[1+ct].Install(0xcfc+ct,write_pci,IO_MB);
			PCI_ReadHandler[1+ct].Install(0xcfc+ct,read_pci,IO_MB);
		}

		//DBP: Replaced with memset
		//for (Bitu dev=0; dev<PCI_MAX_PCIDEVICES; dev++)
		//	for (Bitu fct=0; fct<PCI_MAX_PCIFUNCTIONS-1; fct++)
		//		for (Bitu reg=0; reg<256; reg++)
		//			pci_cfg_data[dev][fct][reg] = 0;
		memset(pci_cfg_data, 0, sizeof(pci_cfg_data));

		//DBP: Allocated fixed callback slot to keep savestate compatibility by not changing the order of callback locations in memory
		//callback_pci.Install(&PCI_PM_Handler,CB_IRETD,"PCI PM");
		callback_pci.InstallFixed(80, &PCI_PM_Handler,CB_IRETD,"PCI PM");
		DBP_ASSERT(callback_pci.Get_callback() == 80);

		initialized=true;
	}

	// register PCI device to bus and setup data
	Bits RegisterPCIDevice(PCI_Device* device, Bits slot=-1) {
		if (device==NULL) return -1;

		if (slot>=0) {
			// specific slot specified, basic check for validity
			if (slot>=PCI_MAX_PCIDEVICES) { DBP_ASSERT(false); return -1; }
		} else {
			// auto-add to new slot, check if one is still free
			if (pci_devices_installed>=PCI_MAX_PCIDEVICES) { DBP_ASSERT(false); return -1; }
		}

		if (!initialized) InitializePCI();

		if (slot<0) slot=pci_devices_installed;	// use next slot
		Bits subfunction=0;	// main device unless specific already-occupied slot is requested
		if (pci_devices[slot]!=NULL) {
			subfunction=pci_devices[slot]->GetNextSubdeviceNumber();
			if (subfunction<0) E_Exit("Too many PCI subdevices!");
		}

		if (device->InitializeRegisters(pci_cfg_data[slot][subfunction])) {
			device->SetPCIId(slot, subfunction);
			if (pci_devices[slot]==NULL) {
				pci_devices[slot]=device;
				pci_devices_installed++;
			} else {
				pci_devices[slot]->AddSubdevice(device);
			}

			return slot;
		}

		DBP_ASSERT(false);
		return -1;
	}

	void Deinitialize(void) {
		initialized=false;
		pci_devices_installed=0;
		num_rqueued_devices=0;
		pci_caddress=0;

		//DBP: Replaced with memset
		//for (Bitu dev=0; dev<PCI_MAX_PCIDEVICES; dev++)
		//	for (Bitu fct=0; fct<PCI_MAX_PCIFUNCTIONS-1; fct++)
		//		for (Bitu reg=0; reg<256; reg++)
		//			pci_cfg_data[dev][fct][reg] = 0;
		memset(pci_cfg_data, 0, sizeof(pci_cfg_data));

		// install PCI-addressing ports
		PCI_WriteHandler[0].Uninstall();
		PCI_ReadHandler[0].Uninstall();
		// install PCI-register read/write handlers
		for (Bitu ct=0;ct<4;ct++) {
			PCI_WriteHandler[1+ct].Uninstall();
			PCI_ReadHandler[1+ct].Uninstall();
		}

		callback_pci.Uninstall();
	}

#if 0 //DBP: Can't just delete things that maybe weren't allocated with new, also PCI_Device is missing virtual destructor
	void RemoveDevice(Bit16u vendor_id, Bit16u device_id) {
		for (Bitu dct=0;dct<pci_devices_installed;dct++) {
			if (pci_devices[dct]!=NULL) {
				if (pci_devices[dct]->NumSubdevices()>0) {
					for (Bitu sct=1;sct<PCI_MAX_PCIFUNCTIONS;sct++) {
						PCI_Device* sdev=pci_devices[dct]->GetSubdevice(sct);
						if (sdev!=NULL) {
							if ((sdev->VendorID()==vendor_id) && (sdev->DeviceID()==device_id)) {
								pci_devices[dct]->RemoveSubdevice(sct);
							}
						}
					}
				}

				if ((pci_devices[dct]->VendorID()==vendor_id) && (pci_devices[dct]->DeviceID()==device_id)) {
					delete pci_devices[dct];
					pci_devices[dct]=NULL;
				}
			}
		}

		// check if all devices have been removed
		bool any_device_left=false;
		for (Bitu dct=0;dct<PCI_MAX_PCIDEVICES;dct++) {
			if (dct>=pci_devices_installed) break;
			if (pci_devices[dct]!=NULL) {
				any_device_left=true;
				break;
			}
		}
		if (!any_device_left) Deinitialize();

		Bitu last_active_device=PCI_MAX_PCIDEVICES;
		for (Bitu dct=0;dct<PCI_MAX_PCIDEVICES;dct++) {
			if (pci_devices[dct]!=NULL) last_active_device=dct;
		}
		if (last_active_device<pci_devices_installed)
			pci_devices_installed=last_active_device+1;
	}
#endif

	PCI(Section* configuration):Module_base(configuration) {
		initialized=false;
		pci_devices_installed=0;

		for (Bitu devct=0;devct<PCI_MAX_PCIDEVICES;devct++)
			pci_devices[devct]=NULL;

		if (num_rqueued_devices>0) {
			// register all devices that have been added before the PCI bus was instantiated
			for (Bitu dct=0;dct<num_rqueued_devices;dct++) {
				this->RegisterPCIDevice(rqueued_devices[dct]);
			}
			num_rqueued_devices=0;
		}
	}

	~PCI(){
		initialized=false;
		pci_devices_installed=0;
		num_rqueued_devices=0;
	}

};

static PCI* pci_interface=NULL;


PhysPt PCI_GetPModeInterface(void) {
	if (pci_interface) {
		return pci_interface->GetPModeCallbackPointer();
	}
	return 0;
}

bool PCI_IsInitialized() {
	if (pci_interface) return pci_interface->IsInitialized();
	return false;
}


void PCI_ShutDown(Section* sec){
	delete pci_interface;
	pci_interface=NULL;
}

void PCI_Init(Section* sec) {
	//DBP: Only enable PCI on SVGA machines
	if (machine != MCH_VGA || svgaCard == SVGA_None) return;
	pci_interface = new PCI(sec);
	sec->AddDestroyFunction(&PCI_ShutDown,false);
}

#ifdef C_DBP_ENABLE_VOODOO
void PCI_AddDevice(PCI_Device* dev) {
	if (pci_interface!=NULL) {
		pci_interface->RegisterPCIDevice(dev);
	} else {
		if (num_rqueued_devices<max_rqueued_devices)
			rqueued_devices[num_rqueued_devices++]=dev;
	}
}

Bit8u PCI_GetCFGData(Bits pci_id, Bits pci_subfunction, Bit8u regnum) {
	return pci_cfg_data[pci_id][pci_subfunction][regnum];
}

#include "../ints/int10.h"

struct PCI_VGADevice : public PCI_Device {
	enum { vendor = 0x5333 }; // S3
	enum { device = 0x8811 }; // trio64
	//enum { device = 0x8810 }; // trio32

	PCI_VGADevice():PCI_Device(vendor,device) { }

	Bits ParseReadRegister(Bit8u regnum) { return regnum; }

	bool OverrideReadRegister(Bit8u regnum, Bit8u* rval, Bit8u* rval_mask) { return false; }

	Bits ParseWriteRegister(Bit8u regnum,Bit8u value) {
		if ((regnum>=0x18) && (regnum<0x28)) return -1;	// base addresses are read-only
		if ((regnum>=0x30) && (regnum<0x34)) return -1;	// expansion rom addresses are read-only
		switch (regnum) {
			case 0x10:
				return (pci_cfg_data[this->PCIId()][this->PCISubfunction()][0x10]&0x0f);
			case 0x11:
				return 0x00;
			case 0x12:
				//return (value&0xc0);	// -> 4mb addressable
				return (value&0x00);	// -> 16mb addressable
			case 0x13:
				return value;
			case 0x14:
				return (pci_cfg_data[this->PCIId()][this->PCISubfunction()][0x10]&0x0f);
			case 0x15:
				return 0x00;
			case 0x16:
				return value;	// -> 64kb addressable
			case 0x17:
				return value;
			default:
				break;
		}
		return value;
	}

	bool InitializeRegisters(Bit8u registers[256]) {
		// init (S3 graphics card)
		//registers[0x08] = 0x44;	// revision ID (s3 trio64v+)
		registers[0x08] = 0x00;	// revision ID
		registers[0x09] = 0x00;	// interface
		registers[0x0a] = 0x00;	// subclass type (vga compatible)
		//registers[0x0a] = 0x01;	// subclass type (xga device)
		registers[0x0b] = 0x03;	// class type (display controller)
		registers[0x0c] = 0x00;	// cache line size
		registers[0x0d] = 0x00;	// latency timer
		registers[0x0e] = 0x00;	// header type (other)

		// reset
		registers[0x04] = 0x23;	// command register (vga palette snoop, ports enabled, memory space enabled)
		registers[0x05] = 0x00;
		registers[0x06] = 0x80;	// status register (medium timing, fast back-to-back)
		registers[0x07] = 0x02;

		//registers[0x3c] = 0x0b;	// irq line
		//registers[0x3d] = 0x01;	// irq pin

		//Bit32u gfx_address_space=(((Bit32u)S3_LFB_BASE)&0xfffffff0) | 0x08;	// memory space, within first 4GB, prefetchable
		Bit32u gfx_address_space=(((Bit32u)S3_LFB_BASE)&0xfffffff0);	// memory space, within first 4GB
		registers[0x10] = (Bit8u)(gfx_address_space&0xff);		// base addres 0
		registers[0x11] = (Bit8u)((gfx_address_space>>8)&0xff);
		registers[0x12] = (Bit8u)((gfx_address_space>>16)&0xff);
		registers[0x13] = (Bit8u)((gfx_address_space>>24)&0xff);

		Bit32u gfx_address_space_mmio=(((Bit32u)S3_LFB_BASE+0x1000000)&0xfffffff0);	// memory space, within first 4GB
		registers[0x14] = (Bit8u)(gfx_address_space_mmio&0xff);		// base addres 0
		registers[0x15] = (Bit8u)((gfx_address_space_mmio>>8)&0xff);
		registers[0x16] = (Bit8u)((gfx_address_space_mmio>>16)&0xff);
		registers[0x17] = (Bit8u)((gfx_address_space_mmio>>24)&0xff);

		return true;
	}
};

void PCI_AddSVGAS3_Device(void) {
	// Keep it static because no one is able to delete it anywhere if allocated
	static PCI_VGADevice vgadevice;
	PCI_AddDevice(&vgadevice);
}

#endif //C_DBP_ENABLE_VOODOO
#endif
