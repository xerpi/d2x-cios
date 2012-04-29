/*-------------------------------------------------------------

usbstorage.c -- Bulk-only USB mass storage support

Copyright (C) 2008
Sven Peter (svpe) <svpe@gmx.net>

quick port to ehci/ios: Kwiirk

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.

-------------------------------------------------------------*/

#define ROUNDDOWN32(v)				(((u32)(v)-0x1f)&~0x1f)

//#define	HEAP_SIZE			(32*1024)
//#define	TAG_START			0x22112211

#define	TAG_START			0x2C0DE001

#define	CBW_SIZE			31
#define	CBW_SIGNATURE			0x43425355
#define	CBW_IN				(1 << 7)
#define	CBW_OUT				0


#define	CSW_SIZE			13
#define	CSW_SIGNATURE			0x53425355

#define	SCSI_TEST_UNIT_READY		0x00
#define	SCSI_INQUIRY			0x12
#define	SCSI_REQUEST_SENSE		0x03
#define	SCSI_START_STOP			0x1b
#define	SCSI_READ_CAPACITY		0x25
#define	SCSI_READ_10			0x28
#define	SCSI_WRITE_10			0x2A

#define	SCSI_SENSE_REPLY_SIZE		18
#define	SCSI_SENSE_NOT_READY		0x02
#define	SCSI_SENSE_MEDIUM_ERROR		0x03
#define	SCSI_SENSE_HARDWARE_ERROR	0x04

#define	USB_CLASS_MASS_STORAGE		0x08
#define USB_CLASS_HUB			0x09

#define	MASS_STORAGE_RBC_COMMANDS	0x01
#define	MASS_STORAGE_ATA_COMMANDS	0x02
#define	MASS_STORAGE_QIC_COMMANDS	0x03
#define	MASS_STORAGE_UFI_COMMANDS	0x04
#define	MASS_STORAGE_SFF8070_COMMANDS	0x05
#define	MASS_STORAGE_SCSI_COMMANDS	0x06

#define	MASS_STORAGE_BULK_ONLY		0x50

#define USBSTORAGE_GET_MAX_LUN		0xFE
#define USBSTORAGE_RESET		0xFF

#define	USB_ENDPOINT_BULK		0x02

#define	USBSTORAGE_CYCLE_RETRIES	10


#define MAX_TRANSFER_SIZE		0x1000

#define DEVLIST_MAXSIZE			8

#define USB_TIMEOUT		(use_alternative_timeout ? 1000*1000 : 200*1000)

#define CYCLE_WRITE			0x1
#define CYCLE_RETRY_ON_BAD_STATUS	0x2


//for testing
static char force_flags = 0;  /* Bits: 1 ->force GetMaxLun, 2-> force SetConfiguration */
static char use_alternative_timeout = 1;
static char use_reset_bulk = 0;

s32 is_dvd[2] = {0,0};

s32 ums_init_done[2] = {0,0};

static usbstorage_handle __usbfd[2];
static u8 __lun[2] = {16,16};
static u8 __mounted[2] = {0,0};
static u16 __vid[2] = {0,0};
static u16 __pid[2] = {0,0};
extern u32 current_port;
static u8 glob_buff[MAX_TRANSFER_SIZE] ATTRIBUTE_ALIGN(32);


void reinit_ehci_headers(void);
void ehci_stop(void);
void ehci_run(void);

//#define MEM_PRINT 1


//#define LOG_STATUS

#ifndef LOG_STATUS
#define log_status(S)    do {} while (0)
#else
void log_status(char *s)
{
	u32 status  = ehci_readl(&ehci->regs->status);
	u32 statusp = ehci_readl(&ehci->regs->port_status[current_port]);

	s_printf("    log_status  (%s)\n",s);

	s_printf("    status: %x %s%s%s%s%s%s%s%s%s%s\n",
		status,
		(status & STS_ASS) ? " Async" : "",
		(status & STS_PSS) ? " Periodic" : "",
		(status & STS_RECL) ? " Recl" : "",
		(status & STS_HALT) ? " Halt" : "",
		(status & STS_IAA) ? " IAA" : "",
		(status & STS_FATAL) ? " FATAL" : "",
		(status & STS_FLR) ? " FLR" : "",
		(status & STS_PCD) ? " PCD" : "",
		(status & STS_ERR) ? " ERR" : "",
		(status & STS_INT) ? " INT" : ""
		);

	s_printf("    status port: %x\n", statusp);
}
#endif



static s32 __usbstorage_reset(usbstorage_handle *dev,int hard_reset);
static s32 __usbstorage_clearerrors(usbstorage_handle *dev, u8 lun);
static s32 __usbstorage_start_stop(usbstorage_handle *dev, u8 lun, u8 start_stop);

// ehci driver has its own timeout.
static s32 __USB_BlkMsgTimeout(usbstorage_handle *dev, u8 bEndpoint, u32 wLength, void *rpData)
{
	return USB_WriteBlkMsg(dev->usb_fd, bEndpoint, wLength, rpData);
}

static s32 __USB_CtrlMsgTimeout(usbstorage_handle *dev, u8 bmRequestType, u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength, void *rpData)
{
	return USB_WriteCtrlMsg(dev->usb_fd, bmRequestType, bmRequest, wValue, wIndex, wLength, rpData);
}



static s32 __send_cbw(usbstorage_handle *dev, u8 lun, u32 len, u8 flags, const u8 *cb, u8 cbLen)
{
	s32 retval = USBSTORAGE_OK;

	if(cbLen == 0 || cbLen > 16 || !dev->buffer)
		return -EINVAL;
	memset(dev->buffer, 0, CBW_SIZE);

	((u32*)dev->buffer)[0] = cpu_to_le32(CBW_SIGNATURE);
	((u32*)dev->buffer)[1] = cpu_to_le32(dev->tag);
	((u32*)dev->buffer)[2] = cpu_to_le32(len);
	dev->buffer[12] = flags;
	dev->buffer[13] = lun;

	// linux usb/storage/protocol.c seems to say only difference is padding
        // and fixed cw size
        /*
        if(dev->ata_protocol)
                dev->buffer[14] = 12; 
        else
                dev->buffer[14] = (cbLen > 6 ? 0x10 : 6);

        dev->buffer[14] = (cb[0] > 0x1f ? 0x0A : 0x06);
        */
        dev->buffer[14] = (cbLen > 6 ? 10 : 6);
        //debug_printf("send cb of size %d\n",dev->buffer[14]);
	memcpy(dev->buffer + 15, cb, cbLen);
        //hexdump(dev->buffer,CBW_SIZE);
	retval = __USB_BlkMsgTimeout(dev, dev->ep_out, CBW_SIZE, (void *)dev->buffer);

	if (retval == CBW_SIZE)
		return USBSTORAGE_OK;
	else if (retval >= 0)
		return USBSTORAGE_ESHORTWRITE;

	return retval;
}


static s32 __read_csw(usbstorage_handle *dev, u8 *status, u32 *dataResidue)
{
	s32 retval = USBSTORAGE_OK;
	u32 signature, tag, _dataResidue, _status;

	memset(dev->buffer, 0xff, CSW_SIZE);
	
	retval = __USB_BlkMsgTimeout(dev, dev->ep_in, CSW_SIZE, dev->buffer);
	//print_hex_dump_bytes("csv resp:",DUMP_PREFIX_OFFSET,dev->buffer,CSW_SIZE);

	if (retval >= 0 && retval != CSW_SIZE)
		return USBSTORAGE_ESHORTREAD;
	if (retval < 0)
		return retval;

	signature = le32_to_cpu(((u32*)dev->buffer)[0]);
	tag = le32_to_cpu(((u32*)dev->buffer)[1]);
	_dataResidue = le32_to_cpu(((u32*)dev->buffer)[2]);
	_status = dev->buffer[12];
	//debug_printf("csv status: %d\n",_status);
	if (signature != CSW_SIGNATURE) {
		//BUG();
		return USBSTORAGE_ESIGNATURE;
	}

	if (dataResidue != NULL)
		*dataResidue = _dataResidue;
	if (status != NULL)
		*status = _status;

	if (tag != dev->tag)
		return USBSTORAGE_ETAG;

	dev->tag = ((dev->tag+2) & 0xffff) | (dev->tag & 0xffff0000);
	//dev->tag++;

	return USBSTORAGE_OK;
}

extern u32 usb_timeout;


static s32 __cycle(usbstorage_handle *dev, u8 lun, u8 *buffer, u32 len, u8 *cb, u8 cbLen, u8 flags, u8 *_status, u32 *_dataResidue, s8 retries)
{
	s32 retval = USBSTORAGE_OK;

	u8 status = 0;
	u32 dataResidue = 0;
	u32 thisLen;
	u32 pstatus;
  
	u8 *buffer2 = buffer;
	u32 len2 = len;

	s32 retries2 = 0;
	s32 t;

	u32 retry_on_bad_status = (flags & CYCLE_RETRY_ON_BAD_STATUS) != 0;

	unplug_device[current_port] = 0;

	do {
		if (retval == -ETIMEDOUT || retval == -ENODEV) {
			unplug_device[current_port] = 1;
			break;
		}

		if (retval < 0) {

			retval = __usbstorage_reset(dev,0);

			ehci_msleep(retval >= 0 ? 5 : 60);

			pstatus = ehci_readl(&ehci->regs->port_status[current_port]);
		
			retries2++;
			if (retval < 0) {
				if (retries2 > 2) {
					unplug_device[current_port] = 1;
					break;
				}
				ehci_msleep(10);
				continue;
			} else
				retries2 = 0;
		}

		retries--;

		buffer = buffer2;
		len = len2;
    
		if (flags & CYCLE_WRITE) {
			t = usb_timeout;

			usb_timeout = USB_TIMEOUT;
          
			retval = __send_cbw(dev, lun, len, CBW_OUT, cb, cbLen);
		
			usb_timeout = t;

			if (retval == -ETIMEDOUT || retval == -ENODEV)
				break;

			if (retval < 0)
				continue;//reset+retry
			
			while (len > 0) {
				thisLen = len; 
				
				retval = __USB_BlkMsgTimeout(dev, dev->ep_out, thisLen, buffer);

				if (retval == -ETIMEDOUT || retval == -ENODEV)
					break;

				if (retval < 0) {
					retval = USBSTORAGE_EDATARESIDUE;
					break;
				}

				if (retval != thisLen && len > 0) {
					retval = USBSTORAGE_EDATARESIDUE;	
					break;
				}
				
				len    -= retval;
				buffer += retval;
			}

			if (retval < 0)
				continue;
		}
		else {
			t = usb_timeout;

			usb_timeout = USB_TIMEOUT;

			retval = __send_cbw(dev, lun, len, CBW_IN, cb, cbLen);
           
			usb_timeout = t;

			if (retval < 0) {
				s_printf("__send_cbw ret %i\n", retval);
				log_status("_send_cbw");
			}

			if (retval == -ETIMEDOUT || retval == -ENODEV)
				break;

			if (retval < 0)
				continue; //reset+retry
            
			while (len > 0) {
				thisLen = len; //if(thisLen>8192) thisLen=8192;
				
				retval = __USB_BlkMsgTimeout(dev, dev->ep_in, thisLen, buffer);

				if (retval < 0) {
					s_printf("__USB_BlkMsgTimeout %i\n", retval);
					log_status("__USB_BlkMsgTimeout");
				}

				if (retval == -ETIMEDOUT || retval == -ENODEV)
					break;

				if (retval < 0) {
					retval = USBSTORAGE_EDATARESIDUE;
					break;
				}

				if (retval != thisLen) {
					retval = USBSTORAGE_EDATARESIDUE;
					break;
				}

				len    -= retval;
				buffer += retval;
			}

			if (retval < 0)
				continue;
		}

		t = usb_timeout;

		usb_timeout = USB_TIMEOUT;

		retval = __read_csw(dev, &status, &dataResidue);

		if (retval < 0) {
			s_printf("__read_csw %i\n", retval);
			log_status("__read_csw");
		}

		if (retval == -ENODEV) {
			usb_timeout = t;
			break;
		}

		if (retval < 0) {
			USB_ClearHalt(dev->usb_fd, dev->ep_in);
			set_toggle(dev->usb_fd, dev->ep_in,0);

			retval = __read_csw(dev, &status, &dataResidue);
		}

		usb_timeout = t;

		if (retval == -ETIMEDOUT || retval == -ENODEV)
			break;

		if (dataResidue && retry_on_bad_status)
			status = 1;

		/* Force a retry on bad status (used by read/write) */
		if (retval >= 0 && retry_on_bad_status && status != 0)
			retval = USBSTORAGE_ESTATUS;

		if (retval < 0)
			continue;

		retval = USBSTORAGE_OK;

	} while (retval < 0 && retries > 0);

	// force unplug
	if (retval < 0)
		unplug_device[current_port] = 1;
	else
		unplug_device[current_port] = 0;

	if (_status != NULL)
		*_status = status;
	if (_dataResidue != NULL)
		*_dataResidue = dataResidue;

	return retval;
}

static s32 __usbstorage_start_stop(usbstorage_handle *dev, u8 lun, u8 start_stop)
{
	#if 0
	s32 retval;
	u8 cmd[16];
	
	u8 status = 0;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_START_STOP;
	cmd[1] = (lun << 5) | 1;
	cmd[4] = start_stop & 3;
	cmd[5] = 0;
	//memset(sense, 0, SCSI_SENSE_REPLY_SIZE);
	retval = __cycle(dev, lun, NULL, 0, cmd, 6, 0, &status, NULL, USBSTORAGE_CYCLE_RETRIES);
//	if(retval < 0) goto error;


//error:
	return retval;
	#else
	return 0;
	#endif
}


static s32 __usbstorage_clearerrors(usbstorage_handle *dev, u8 lun)
{
	s32 retval;
	u8 cmd[16];
	u8 *sense = USB_Alloc(SCSI_SENSE_REPLY_SIZE);
	u8 status = 0;
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_TEST_UNIT_READY;

	if (!sense)
		return -ENOMEM;

	retval = __cycle(dev, lun, NULL, 0, cmd, 6, CYCLE_WRITE, &status, NULL, USBSTORAGE_CYCLE_RETRIES);

	//if (retval < 0) 
		s_printf("    SCSI_TEST_UNIT_READY ret %i\n", retval);

	if (retval == -ENODEV)
		goto error;

	if (status != 0 || retval < 0) {
		cmd[0] = SCSI_REQUEST_SENSE;
		cmd[1] = lun << 5;
		cmd[4] = SCSI_SENSE_REPLY_SIZE;
		cmd[5] = 0;
		memset(sense, 0, SCSI_SENSE_REPLY_SIZE);

		usb_timeout = (retval == USBSTORAGE_ETIMEDOUT) ? 2500*1000 : 10000*1000;

		retval = __cycle(dev, lun, sense, SCSI_SENSE_REPLY_SIZE, cmd, 6, 0, NULL, NULL, USBSTORAGE_CYCLE_RETRIES);

		s_printf("    SCSI_REQUEST_SENSE ret %i\n", retval);

		if (retval < 0)
			goto error;

		status = sense[2] & 0x0F;

		s_printf("    SCSI_REQUEST_SENSE status %x\n", status);

		if (status == SCSI_SENSE_NOT_READY || status == SCSI_SENSE_MEDIUM_ERROR || status == SCSI_SENSE_HARDWARE_ERROR)
                        retval = USBSTORAGE_ESENSE;
	}

error:
	USB_Free(sense);
	return retval;
}



static s32 __usbstorage_reset(usbstorage_handle *dev,int hard_reset)
{
	u32 status;
	u8 conf;
	s32 retval = -1;
	u32 old_usb_timeout = usb_timeout; 

	usb_timeout = USB_TIMEOUT;

	status = ehci_readl(&ehci->regs->port_status[dev->usb_fd->port]);

	// device unplugged
	if (!(PORT_CONNECT & status))  {
		unplug_device[current_port] = 1;
		usb_timeout = old_usb_timeout;
		return -ENODEV;
	}

	usb_timeout = USB_TIMEOUT;

	if (use_reset_bulk) {
		retval = __USB_CtrlMsgTimeout(dev, 
		(USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE),
		USBSTORAGE_RESET, 0, dev->interface, 0, NULL);
	    
		s_printf("usbstorage reset: BULK RESET %i\n",retval);

		/* gives device enough time to process the reset */
		ehci_msleep(60);
	}
	else {
		USB_ClearHalt(dev->usb_fd, 0);
		ehci_msleep(5);
		retval = 0;
	}

	usb_timeout = USB_TIMEOUT;

	/* FIXME?: some devices return -7004 here which definitely violates the usb ms protocol but they still seem to be working... */
        if (retval < 0) {
		ehci_msleep(50);
		USB_ClearHalt(dev->usb_fd, 0);
		ehci_msleep(5);
		goto end;
	}

	retval = USB_ClearHalt(dev->usb_fd, dev->ep_in);
	ehci_msleep(5);

	s_printf("usbstorage reset: clearhalt in ret %i\n",retval);

	if (retval < 0)
		goto end;
	
	retval = USB_ClearHalt(dev->usb_fd, dev->ep_out);
	ehci_msleep(5);

	s_printf("usbstorage reset: clearhalt out ret %i\n",retval);

	if (retval < 0)
		goto end;
	
	ehci_msleep(10);
	retval = USB_GetConfiguration(dev->usb_fd, &conf); // for test it works

	s_printf("usbstorage reset: USB_GetConfiguration ret %i\n",retval);

	if (retval < 0)
		goto end;

	s_printf("reset ok\n");

	usb_timeout = old_usb_timeout;
	return retval;

end:
	
	usb_timeout = old_usb_timeout;

	unplug_device[current_port] = 1; // force the unplug method

	return retval;
}


static s32 try_status = 0;

struct _device_data
{
	u32 mounted;
	u8  lun;
	u8  use_maxlun;
	u8  bDeviceClass;
	u8  bDeviceSubClass;
	u16 idVendor;
	u16 idProduct;
	u8  iManufacturer;
	u8  iProduct;
	u8  iSerialNumber;
	u8  interface;
	u8  ep_in;
	u8  ep_out;
	u16 pad2;
}
ATTRIBUTE_PACKED _old_device[2];


s32 USBStorage_Open(usbstorage_handle *dev, struct ehci_device *fd)
{
	s32 retval = -1;
	u8 conf = 0; //,*max_lun = NULL;
	u32 iConf, iInterface, iEp;
	static usb_devdesc udd;
	usb_configurationdesc *ucd;
	usb_interfacedesc *uid;
	usb_endpointdesc *ued;

	__lun[current_port] = 16; // select bad LUN

	//max_lun = USB_Alloc(1);
	//if (max_lun == NULL) return -ENOMEM;
	
	//memset(dev, 0, sizeof(*dev));

	dev->tag = TAG_START;

	dev->usb_fd = fd;

	udd.configurations = NULL;
	s_printf("USBStorage_Open(): USB_GetDescriptors init\n");

	retval = USB_GetDescriptors(dev->usb_fd, &udd);

	s_printf("USBStorage_Open(): USB_GetDescriptors %i\n",retval);

	log_status("after USB_GetDescriptors");

	if (retval < 0)
		goto free_and_return;

	// test device changed without unmount (prevent device write corruption)
	if (ums_init_done[current_port]) {
		if(_old_device[current_port].bDeviceClass!=udd.bDeviceClass ||
			_old_device[current_port].bDeviceSubClass!=udd.bDeviceSubClass ||
			_old_device[current_port].idVendor!=udd.idVendor ||
			_old_device[current_port].idProduct!=udd.idProduct ||
			_old_device[current_port].iManufacturer!=udd.iManufacturer ||
			_old_device[current_port].iProduct!=udd.iProduct ||
			_old_device[current_port].iSerialNumber!=udd.iSerialNumber) 
		{
			//USB_Free(max_lun);
			USB_FreeDescriptors(&udd);
			#ifdef MEM_PRINT
			s_printf("USBStorage_Open(): device changed!!!\n");
			#endif
			return -ENODEV;
		}
	}

	_old_device[current_port].bDeviceClass    = udd.bDeviceClass;
	_old_device[current_port].bDeviceSubClass = udd.bDeviceSubClass;
	_old_device[current_port].idVendor        = udd.idVendor;
	_old_device[current_port].idProduct       = udd.idProduct;
	_old_device[current_port].iManufacturer   = udd.iManufacturer;
	_old_device[current_port].iProduct        = udd.iProduct;
	_old_device[current_port].iSerialNumber   = udd.iSerialNumber;

	try_status = -128;

	for (iConf = 0; iConf < udd.bNumConfigurations; iConf++) {
		ucd = &udd.configurations[iConf];		
		#ifdef MEM_PRINT
		s_printf("USBStorage_Open(): ucd %i Power %i mA\n",iConf, ((u32) ucd->bMaxPower)*2);
		#endif
		for (iInterface = 0; iInterface < ucd->bNumInterfaces; iInterface++) {
			uid = &ucd->interfaces[iInterface];
			//debug_printf("interface %d, class:%x subclass %x protocol %x\n",iInterface,uid->bInterfaceClass,uid->bInterfaceSubClass, uid->bInterfaceProtocol);
			if (uid->bInterfaceClass    == USB_CLASS_MASS_STORAGE && /*
			   (uid->bInterfaceSubClass == MASS_STORAGE_SCSI_COMMANDS
				|| uid->bInterfaceSubClass == MASS_STORAGE_RBC_COMMANDS
				|| uid->bInterfaceSubClass == MASS_STORAGE_ATA_COMMANDS
				|| uid->bInterfaceSubClass == MASS_STORAGE_QIC_COMMANDS
				|| uid->bInterfaceSubClass == MASS_STORAGE_UFI_COMMANDS
				|| uid->bInterfaceSubClass == MASS_STORAGE_SFF8070_COMMANDS) && */
			   uid->bInterfaceProtocol == MASS_STORAGE_BULK_ONLY && uid->bNumEndpoints>=2)
			{

				dev->ata_protocol = 0;
				if (uid->bInterfaceSubClass != MASS_STORAGE_SCSI_COMMANDS || uid->bInterfaceSubClass != MASS_STORAGE_RBC_COMMANDS)
                                        dev->ata_protocol = 1;

				s_printf("USBStorage_Open(): interface subclass %i ata_prot %i \n",uid->bInterfaceSubClass, dev->ata_protocol);

				dev->ep_in = dev->ep_out = 0;
				for (iEp = 0; iEp < uid->bNumEndpoints; iEp++) {
					ued = &uid->endpoints[iEp];
					if (ued->bmAttributes != USB_ENDPOINT_BULK)
						continue;

					if ((ued->bEndpointAddress & USB_ENDPOINT_IN) && !dev->ep_in && ued->bEndpointAddress != 0) {
						dev->ep_in = ued->bEndpointAddress;
						#ifdef MEM_PRINT
						s_printf("In Point: %i\n", (u32) ued->wMaxPacketSize);
						#endif
					}
					else if (!dev->ep_out && ued->bEndpointAddress != 0) {
						dev->ep_out = ued->bEndpointAddress;

						s_printf("Out Point: %i\n", (u32) ued->wMaxPacketSize);
					}
				}
				if (dev->ep_in != 0 && dev->ep_out != 0) {
					#ifdef MEM_PRINT
					s_printf("ep_in %x ep_out %x\n", (u32) dev->ep_in, (u32) dev->ep_out);
					#endif
					_old_device[current_port].ep_in  = dev->ep_in;
					_old_device[current_port].ep_out = dev->ep_out;

					dev->configuration = ucd->bConfigurationValue;
					dev->interface = uid->bInterfaceNumber;
					_old_device[current_port].interface = dev->interface;
					dev->altInterface = uid->bAlternateSetting;

					goto found;
				}
			}
			else {
				if (uid->endpoints != NULL) {
					USB_Free(uid->endpoints);
					uid->endpoints = NULL;
				}

				if (uid->extra != NULL) {
					USB_Free(uid->extra);
					uid->extra = NULL;
				}

				if (uid->bInterfaceClass == USB_CLASS_HUB) {
					retval = USBSTORAGE_ENOINTERFACE;
					try_status = -20000;

					USB_FreeDescriptors(&udd);

					goto free_and_return;
				}

				if (uid->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
			   		uid->bInterfaceProtocol == MASS_STORAGE_BULK_ONLY && 
					uid->bNumEndpoints >= 2)
				{
					try_status = -(10000 + uid->bInterfaceSubClass);
				}
			}
		}
	}

	s_printf("USBStorage_Open(): cannot find any interface!!!\n");

	USB_FreeDescriptors(&udd);
	retval = USBSTORAGE_ENOINTERFACE;
        //debug_printf("cannot find any interface\n");
	goto free_and_return;

found:
	USB_FreeDescriptors(&udd);

	retval = USBSTORAGE_EINIT;

	try_status = -1201;	

	s_printf("USBStorage_Open(): conf: %x altInterface: %x\n", dev->configuration, dev->altInterface);

	if (USB_GetConfiguration(dev->usb_fd, &conf) < 0) {
		s_printf("USB_GetConfiguration() Error. Continue.\n");
		//goto free_and_return;
	}
	s_printf("Actual conf: %x   next conf: %x\n",conf, dev->configuration);
	try_status = -1202;
	if (USB_SetConfiguration(dev->usb_fd, dev->configuration) < 0)  {
		s_printf("USB_SetConfiguration() Error\n");
		//goto free_and_return;
	}

	try_status = -1203;
	if (/*dev->altInterface != 0 &&*/ USB_SetAlternativeInterface(dev->usb_fd, dev->interface, dev->altInterface) < 0) {
		s_printf("USB_SetAlternativeInterface() Error. Continue\n");
		//goto free_and_return;
	}
	s_printf("USB_SetConfiguration() & USB_SetAlternativeInterface() OK\n");
/*
	if (USB_GetConfiguration(dev->usb_fd, &conf) < 0)
		goto free_and_return;
	try_status = -1202;

	#ifdef MEM_PRINT
	log_status("after USB_GetConfiguration");
	#endif

	#ifdef MEM_PRINT
	if(conf != dev->configuration)
		s_printf("USBStorage_Open(): changing conf from %x\n", conf);
	
	#endif

	usb_timeout = 10000*1000;
	if ((conf != dev->configuration || (force_flags & 2)) && USB_SetConfiguration(dev->usb_fd, dev->configuration) < 0) 
		goto free_and_return;

	try_status = -1203;
	if(dev->altInterface != 0 && USB_SetAlternativeInterface(dev->usb_fd, dev->interface, dev->altInterface) < 0)
		goto free_and_return;

	usb_timeout = 1000*1000;
	try_status = -1204;
	
	#ifdef MEM_PRINT
	log_status("Before USBStorage_Reset");
	#endif
	retval = USBStorage_Reset(dev);
	//retval=0;
	//retval =__usbstorage_reset(dev, 1);
	#ifdef MEM_PRINT
	log_status("After USBStorage_Reset");
	#endif
	if (retval < 0)
		goto free_and_return;
*/
	u8 max_lun = 0;
	retval = __USB_CtrlMsgTimeout(dev, (USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), USBSTORAGE_GET_MAX_LUN, 0, dev->interface, sizeof(max_lun), &max_lun);
	if (retval < 0 ) {
		s_printf("Get_Max_Lun(): err, default max_lun=8\n");
		dev->max_lun = 8;
	}
	else {
		dev->max_lun = (max_lun + 1);
		s_printf("Get_Max_Lun(): OK: %i\n",dev->max_lun);
	}

/*	if(retval == USBSTORAGE_ETIMEDOUT)*/

	/* NOTE: from usbmassbulk_10.pdf "Devices that do not support multiple LUNs may STALL this command." */
//		dev->max_lun = 8; // max_lun can be from 1 to 16, but some devices do not support lun 
	
	retval = USBSTORAGE_OK;

	/*if(dev->max_lun == 0)
		dev->max_lun++;*/

	/* taken from linux usbstorage module (drivers/usb/storage/transport.c) */
	/*
	 * Some devices (i.e. Iomega Zip100) need this -- apparently
	 * the bulk pipes get STALLed when the GetMaxLUN request is
	 * processed.   This is, in theory, harmless to all other devices
	 * (regardless of if they stall or not).
	 */
	//USB_ClearHalt(dev->usb_fd, dev->ep_in);
	//USB_ClearHalt(dev->usb_fd, dev->ep_out);

	if (dev->buffer == NULL)
		dev->buffer = glob_buff;
	
	if (dev->buffer == NULL) {
		retval = -ENOMEM;
		try_status = -1205;
	}
	else
		retval = USBSTORAGE_OK;

free_and_return:

	usb_timeout = 1000*1000;

	if (retval < 0) {
		/*
		// Never free dev->buffer or destroy the dev info
		if(dev->buffer != NULL)
			USB_Free(dev->buffer);
		memset(dev, 0, sizeof(*dev));
		*/

		s_printf("USBStorage_Open(): try_status %i\n",try_status);

		return retval;
	}

	s_printf("USBStorage_Open(): OK, return 0\n");

	return 0;
}


s32 USBStorage_Close(usbstorage_handle *dev)
{

	// Never free dev->buffer or destroy the dev info

        /*if(dev->buffer != NULL)
                USB_Free(dev->buffer);
		dev->buffer=NULL;
		*/
	//memset(dev, 0, sizeof(*dev));

	return 0;
}

s32 USBStorage_Reset(usbstorage_handle *dev)
{
	s32 retval;

	retval = __usbstorage_reset(dev,0);

	return retval;
}

s32 USBStorage_GetMaxLUN(usbstorage_handle *dev)
{
	return dev->max_lun;
}


s32 USBStorage_MountLUN(usbstorage_handle *dev, u8 lun)
{
	s32 retval;

	if (lun >= dev->max_lun)
		return -EINVAL;

	usb_timeout = 1000*1000;

	retval = __usbstorage_start_stop(dev, lun, 1);

	#ifdef MEM_PRINT
	   s_printf("    start_stop cmd ret %i\n",retval);
	#endif
	if (retval < 0)
		goto ret;
	usb_timeout = 20000*1000;
	retval = __usbstorage_clearerrors(dev, lun);
	if (retval < 0)
		goto ret;
	usb_timeout = 1000*1000;

	retval = USBStorage_Inquiry(dev, lun);
	
	s_printf("    Inquiry ret %i\n",retval);

/*
	if (retval >= 0) {
		s_printf("    Device Type: %x\n",  *(((u8 *)dev->buffer)+2048) & 31);
	}
*/	
	if (retval < 0)
		goto ret;
	retval = USBStorage_ReadCapacity(dev, lun, &dev->sector_size[lun], &dev->n_sector[lun]);
	
	s_printf("    ReadCapacity ret %i  sector_size: %u  sectors: %u\n",retval,dev->sector_size[lun],dev->n_sector[lun]);
	
	if (dev->sector_size[lun] < 512 || dev->n_sector[lun] < 10)
		retval = -33;
 
ret:
	usb_timeout = 1000*1000;

	return retval;
}

s32 USBStorage_Inquiry(usbstorage_handle *dev, u8 lun)
{
	int n;
	s32 retval;
	u8 cmd[] = {SCSI_INQUIRY, lun << 5, 0, 0, 36, 0};
	u8 *response = USB_Alloc(36);

	if (!response)
		return -ENOMEM;

	for (n = 0; n < 2; n++) {
		memset(response, 0, 36);

		retval = __cycle(dev, lun, response, 36, cmd, 6, 0, NULL, NULL, USBSTORAGE_CYCLE_RETRIES);
		if (retval >= 0)
			break;
	}

	if (retval >= 0) {
		switch(*response & 31) {
			// info from http://en.wikipedia.org/wiki/SCSI_Peripheral_Device_Type
			case 5: // CDROM
			case 7: // optical memory device (e.g., some optical disks)
				is_dvd[current_port] = 1;
				break;
			default:
				is_dvd[current_port] = 0;
				break;
		}
	}

	USB_Free(response);

	return retval;
}
s32 USBStorage_ReadCapacity(usbstorage_handle *dev, u8 lun, u32 *sector_size, u32 *n_sectors)
{
	s32 retval;

	// FIX by digicroxx
	// Specifications state that SCSI_READ_CAPACITY has a 10 byte long Command Descriptor Block.
	// See page 32 at http://www.usb.org/developers/devclass_docs/usbmass-ufi10.pdf 
	// and http://en.wikipedia.org/wiki/SCSI_Read_Capacity_Command
	// Certain devices strictly require this command lenght.
	u8 cmd[] = {SCSI_READ_CAPACITY, lun << 5, 0, 0, 0, 0, 0, 0, 0, 0}; 
	u8 *response = USB_Alloc(8);
	u32 val;

	if (!response)
		return -ENOMEM;

	retval = __cycle(dev, lun, response, 8, cmd, sizeof(cmd), 0, NULL, NULL, USBSTORAGE_CYCLE_RETRIES); 

	if (retval >= 0) {
	        memcpy(&val, response, 4);
		if (n_sectors != NULL)
			*n_sectors = be32_to_cpu(val);
		memcpy(&val, response + 4, 4);
		if (sector_size != NULL)
			*sector_size = be32_to_cpu(val);
		//if (be32_to_cpu(val) == 2048) is_dvd[current_port] = 1;
		retval = USBSTORAGE_OK;
	}

	USB_Free(response);

	return retval;
}


static s32 __USBStorage_Read_Write(u8 cycle_flags, usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer)
{
	u8 status = 0;
	s32 retval;
	u8 cmd[] = {
		cycle_flags & CYCLE_WRITE ?
			SCSI_WRITE_10 :
			SCSI_READ_10,
		cycle_flags & CYCLE_WRITE ? 
			(lun << 5) | 8 :  // LUN & FUA (Force Unit Access to avoid the cache),
			(lun << 5),
		sector >> 24,
		sector >> 16,
		sector >>  8,
		sector,
		0,
		n_sectors >> 8,
		n_sectors,
		0
	};

	if (!dev || lun >= dev->max_lun || dev->sector_size[lun] == 0)
		return -EINVAL;

	retval = __cycle(dev, lun, buffer, ((u32) n_sectors) * dev->sector_size[lun], cmd, sizeof(cmd), cycle_flags, &status, NULL, 6);
	if (retval > 0 && status != 0)
		retval = USBSTORAGE_ESTATUS;

	return retval;
}

static s32 USBStorage_Read_Write(u8 cycle_flags, usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer)
{
	u32 max_sectors = n_sectors;
	u32 sectors;
	s32 ret = -1;

	/* This should fix problems with certain devices */
	if (((u32) n_sectors) * dev->sector_size[lun] > 64*1024)
		max_sectors = (64*1024) / dev->sector_size[lun];

	while (n_sectors > 0) {
		sectors = n_sectors > max_sectors ? max_sectors: n_sectors;

		ret = __USBStorage_Read_Write(cycle_flags, dev, lun, sector, sectors, buffer);
		if (ret < 0)
			break;

		n_sectors -= sectors;
		sector    += sectors;
		buffer    += sectors * dev->sector_size[lun];
	}

	return ret;
}

inline s32 USBStorage_Read(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer)
{
	return USBStorage_Read_Write(CYCLE_RETRY_ON_BAD_STATUS, dev, lun, sector, n_sectors, buffer);
}

inline s32 USBStorage_Write(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, const u8 *buffer)
{
	return USBStorage_Read_Write(CYCLE_WRITE | CYCLE_RETRY_ON_BAD_STATUS, dev, lun, sector, n_sectors, (void *)buffer);
}

/*
The following is for implementing the ioctl interface inpired by the disc_io.h
as used by libfat

This opens the first lun of the first usbstorage device found.
*/



// temp function before libfat is available */
s32 USBStorage_Try_Device(struct ehci_device *fd)
{
	int maxLun, j, retval;
	int test_max_lun = 1;

	__vid[current_port]     = 0;
	__pid[current_port]     = 0;
	__mounted[current_port] = 0;
	__lun[current_port]     = 0;

	try_status = -120;

	s_printf("USBStorage_Open\n");
	if (USBStorage_Open(&__usbfd[current_port], fd) < 0)
		return -EINVAL;
	s_printf("USBStorage_Open end\n");	

	maxLun = 1;
	__usbfd[current_port].max_lun = 1;  

	j = 0;

	// fast re-mount
	if (_old_device[current_port].mounted) {
		if (_old_device[current_port].use_maxlun || (force_flags & 1)) {
			__usbfd[current_port].max_lun = 0;
			usb_timeout = 10000*1000;
			retval = __USB_CtrlMsgTimeout(&__usbfd[current_port], (USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), 
						USBSTORAGE_GET_MAX_LUN, 0, __usbfd[current_port].interface, 1, &__usbfd[current_port].max_lun);
			usb_timeout = 1000*1000;
			if (retval < 0) {
				__usbfd[current_port].max_lun = 1;
				goto bad_mount;
			} 
			else {
				__usbfd[current_port].max_lun++;
			}
		}

		j = _old_device[current_port].lun;

		#ifdef MEM_PRINT
		s_printf("Fast USBStorage_MountLUN %i#\n", j);
		#endif
		retval = USBStorage_MountLUN(&__usbfd[current_port], j);
		#ifdef MEM_PRINT
		s_printf("USBStorage_MountLUN: ret %i\n", retval);
		#endif

		if (retval == USBSTORAGE_ETIMEDOUT || retval==-ENODEV /*&& test_max_lun==0*/) {
			USBStorage_Reset(&__usbfd[current_port]);
			try_status = -121;
			__mounted[current_port] = 0;
			USBStorage_Close(&__usbfd[current_port]); 
			return -EINVAL;
		}

		if (retval < 0)
			goto bad_mount;

		__mounted[current_port] = 1;
		__lun[current_port] = j;
		usb_timeout = 1000*1000;
		try_status = 0;

		return 0;
	}

	_old_device[current_port].use_maxlun = 0;
	//for(j = 0; j < maxLun; j++)
	while (1) {
		if (!(force_flags & 1) || j!=0 || !test_max_lun) {
			#ifdef MEM_PRINT
			s_printf("USBStorage_MountLUN %i#\n", j);
			#endif
			retval = USBStorage_MountLUN(&__usbfd[current_port], j);
			#ifdef MEM_PRINT
			s_printf("USBStorage_MountLUN: ret %i\n", retval);
			#endif

			if ((retval == USBSTORAGE_ETIMEDOUT || retval==-ENODEV) && j != 0) { 
				usb_timeout = 1000*1000;
				USBStorage_Reset(&__usbfd[current_port]);
				try_status = -121;
				__mounted[current_port] = 0;
				USBStorage_Close(&__usbfd[current_port]); 
				return -EINVAL;
				//  break;
			}
		}
		else {
			retval = -1;  // force get max lun before Mount LUN
		}

		if (retval < 0) {
			if (test_max_lun) {
				unplug_device[current_port] = 0;
				__usbfd[current_port].max_lun = 0;

				usb_timeout = 10000*1000;
				retval = __USB_CtrlMsgTimeout(&__usbfd[current_port], 
						(USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), 
						USBSTORAGE_GET_MAX_LUN, 0, __usbfd[current_port].interface, 1, &__usbfd[current_port].max_lun);
				usb_timeout = 1000*1000;
				if (retval < 0 ) {
					__usbfd[current_port].max_lun = 1;
					unplug_device[current_port] = 0;
				}
				else {
					__usbfd[current_port].max_lun++;
					_old_device[current_port].use_maxlun = 1;
				}
				maxLun = __usbfd[current_port].max_lun;

				#ifdef MEM_PRINT
				s_printf("USBSTORAGE_GET_MAX_LUN ret %i maxlun %i\n", retval,maxLun);
				#endif
				if (!(force_flags & 1))
					test_max_lun = 0;
				else if (retval >= 0) 
					test_max_lun = 0;
			}
			else
				j++;

			if (j >= maxLun)
				break;

			continue;
		}

		_old_device[current_port].mounted = 1;
		_old_device[current_port].lun = j;

		__vid[current_port] = fd->desc.idVendor;
		__pid[current_port] = fd->desc.idProduct;
		__mounted[current_port] = 1;
		__lun[current_port] = j;
		usb_timeout = 1000*1000;
		try_status = 0;

		return 0;
	}
bad_mount:
	try_status = -122;
	  
	USBStorage_Reset(&__usbfd[current_port]);
	__mounted[current_port] = 0;
	USBStorage_Close(&__usbfd[current_port]);

	#ifdef MEM_PRINT
	s_printf("USBStorage_MountLUN fail!!!\n");
	#endif

	return -EINVAL;
}

void USBStorage_Umount(void)
{
	/*
	if(__mounted && !unplug_device) {
		if(__usbstorage_start_stop(&__usbfd, __lun, 0x0) == 0) // stop
			ehci_msleep(1000);
	}
*/
/*
	if(__mounted[current_port]) {
		USBStorage_Close(&__usbfd[current_port]);__lun[current_port] = 16;
		__mounted[current_port] = 0;
		unplug_device[current_port] = ;
		memset(&_old_device[current_port], 0, sizeof(_old_device[current_port]));
	}
	
	ums_init_done[current_port] = 0;
*/	
}

static void __USBStorage_DisablePortChangesDetection(void)
{
	ehci_writel(0, &ehci->regs->intr_enable);
	ehci_irq_passive_callback(NULL);
}

static void __USBStorage_EnablePortChangesDetection(void)
{
	ehci_irq_passive_callback(passive_callback_hand);
	ehci_writel(STS_PCD, &ehci->regs->intr_enable);
}

s32 USBStorage_Init()
{
	u32 status;
	s32 ret;

	if (ums_init_done[current_port] == 1)
		return 0;

	__USBStorage_DisablePortChangesDetection();

	try_status = -100; 

	#ifdef MEM_PRINT
	s_printf("EHCI: USBStorage_Init(%i)\n",current_port);
	#endif		

	ret = -1;

	struct ehci_device *dev = &ehci->devices[current_port];

	ums_init_done[current_port] = 0;
	unplug_device[current_port] = 1;
	__mounted[current_port] = 0;
	_old_device[current_port].mounted = 0;                

	dev->port = current_port;

	if (dev->id == 0) {
		status = ehci_readl(&ehci->regs->port_status[current_port]);
		if (!(status & 1)) 
			ehci_acquire_port(current_port);					

		status = ehci_readl(&ehci->regs->port_status[current_port]);

		if (status & 1) {
			ret = ehci_reset_port(current_port);
			if (ret == -1119 || ret == -1120) {
				try_status = -100;
				dev->id = 0;
			}
			else {
				ret = ehci_reset_port2(current_port);
				ehci_msleep(20);
				status = ehci_readl(&ehci->regs->port_status[current_port]);

				if (ret < 0 || (status & 0x3905) != 0x1005)
					ret = ehci_reset_port(current_port);
			}
		}

	}

	if (dev->id != 0) {
		unplug_device[current_port] = 1;

		if (USBStorage_Try_Device(dev) == 0)  {
			u8 *sector_buffer;
			u32 sector_size;

			ums_init_done[current_port] = 1;
			unplug_device[current_port] = 0;

			if (is_dvd[current_port])
				s_printf("Device is a dvd\n");
			else
				s_printf("Device is a usbhd\n");
			s_printf("USBStorage_Init() Ok\n");
			ehci_msleep(100);
			usb_timeout = 10000*1000;
			sector_size = __usbfd[current_port].sector_size[__lun[current_port]];
			s_printf("Sector size = %d\n", sector_size);
			sector_buffer = USB_GetSectorBuffer(sector_size);
			s_printf("Reading sector 0\n");
			ret = USBStorage_Read(&__usbfd[current_port], __lun[current_port], 0, 1, sector_buffer);								
			usb_timeout = 500*1000;
			if (ret < 0)
				s_printf("Error Reading sector 0\n");
			else
				s_printf("OK Reading sector 0\n");
			if (ret < 0) {								
				ehci_release_port(current_port);
				ret = -1118;
			}
			else
				ret = 1;
			//if(is_dvd) return i+2; // is DVD medium
			//return i;
					
			// 0 -> port0    1 -> port1     2 -> dvdport0    3 -> dvdport1
		}
		else
			ehci_release_port(current_port);
	}

	__USBStorage_EnablePortChangesDetection();

	if (ret == 1)
		try_status = 0; 

	return try_status;
}

s32 USBStorage_Get_Capacity(u32*sector_size)
{
	u32 __iomem *status_reg = &ehci->regs->port_status[current_port];
	u32 status = ehci_readl(status_reg);

	if (sector_size)
		*sector_size = 0;

	if ((PORT_OWNER&status) || !(PORT_CONNECT&status) || !(status & PORT_PE) || PORT_USB11(status)) {
		if (__mounted[current_port]) {
			__lun[current_port] = 16;
			memset(&__usbfd[current_port], 0, sizeof(__usbfd[current_port]));

			__mounted[current_port] = 0;
			unplug_device[current_port] = 0;
			memset(&_old_device[current_port], 0, sizeof(_old_device[current_port]));
		}

		ums_init_done[current_port] = 0;
		return 0;
	}

	if (__mounted[current_port] == 1 && __lun[current_port] != 16) {
		if(sector_size)
			*sector_size = __usbfd[current_port].sector_size[__lun[current_port]];
			
		return __usbfd[current_port].n_sector[__lun[current_port]];
	}

	return 0;
}

static s32 fast_remount(void)
{
	s32 retval;
	usb_devdesc udd;

	if (!ums_init_done[current_port])
		return -1009;

	retval = 0;

	__lun[current_port] = 16;

	usb_timeout = USB_TIMEOUT;

	udd.configurations = NULL;
	retval = USB_GetDescriptors(__usbfd[current_port].usb_fd, &udd);

	if (_old_device[current_port].bDeviceClass != udd.bDeviceClass ||
		_old_device[current_port].bDeviceSubClass != udd.bDeviceSubClass ||
		_old_device[current_port].idVendor != udd.idVendor ||
		_old_device[current_port].idProduct != udd.idProduct ||
		_old_device[current_port].iManufacturer != udd.iManufacturer ||
		_old_device[current_port].iProduct != udd.iProduct ||
		_old_device[current_port].iSerialNumber != udd.iSerialNumber) 
	{

		USB_FreeDescriptors(&udd);
		#ifdef MEM_PRINT
		s_printf("fast_remount(): device changed!!!\n");

		#endif
		return -1009;
	}

	USB_FreeDescriptors(&udd);

	//if(USB_GetConfiguration(__usbfd.usb_fd, &conf) <0) return -1000;

	usb_timeout = 800*1000;

	if (USB_SetConfiguration(__usbfd[current_port].usb_fd, __usbfd[current_port].configuration) < 0)
		return -1001;
	
	if (__usbfd[current_port].altInterface != 0 && USB_SetAlternativeInterface(__usbfd[current_port].usb_fd, __usbfd[current_port].interface, __usbfd[current_port].altInterface) < 0)
		return -1002;

	/*
	if((conf != __usbfd.configuration || (force_flags & 2)) && USB_SetConfiguration(__usbfd.usb_fd, __usbfd.configuration) < 0) return -1001;
		
	if(__usbfd.altInterface != 0 && USB_SetAlternativeInterface(__usbfd.usb_fd, __usbfd.interface, __usbfd.altInterface) < 0) return -1002;
	*/

	usb_timeout = USB_TIMEOUT;

	//if( __usbstorage_reset(&__usbfd,0)<0) return -1003;
			
	//usb_timeout=1000*1000;

	//if(_old_device.use_maxlun || (force_flags & 1))
	{
		__usbfd[current_port].max_lun = 0;

		//usb_timeout = 10000*1000;
		retval = __USB_CtrlMsgTimeout(&__usbfd[current_port], (USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), 
							USBSTORAGE_GET_MAX_LUN, 0, __usbfd[current_port].interface, 1, &__usbfd[current_port].max_lun);
		//usb_timeout = 1000*1000;
		if (retval < 0) {
			__usbfd[current_port].max_lun = 1;
			__usbstorage_reset(&__usbfd[current_port],0);
			return -1004;
		} 
		else {
			__usbfd[current_port].max_lun++;
		}
	}

	//retval = USBStorage_MountLUN(&__usbfd, _old_device.lun);

	//usb_timeout=20000*1000;
	retval = __usbstorage_clearerrors(&__usbfd[current_port], _old_device[current_port].lun);
	#ifdef MEM_PRINT
	  // s_printf(" Clear error ret %i\n",retval);
	#endif
	//usb_timeout=1000*1000;
	if (retval < 0)
		return -1005;

	retval = USBStorage_Inquiry(&__usbfd[current_port], _old_device[current_port].lun);
	#ifdef MEM_PRINT
	  // s_printf("    Inquiry ret %i\n",retval);
	#endif

	if (retval < 0)
		return -1006;

	__lun[current_port] = _old_device[current_port].lun;
	__mounted[current_port] = 1;

	return retval;
}


s32 unplug_procedure(void)
{
	s32 retval = 1;
	u32 status;

	if (unplug_device[current_port] != 0) {
		s32 ret;

		// unplug detection method

		status = ehci_readl(&ehci->regs->port_status[current_port]);
		if (!(status & 1))
			return 1;

		__USBStorage_DisablePortChangesDetection();

		ret = ehci_reset_port2(current_port);
		//if(ret<0) ehci_reset_port(current_port);

		if (ret < 0)
			ret = ehci_reset_port2(current_port);

		ehci_msleep(60);
		status = ehci_readl(&ehci->regs->port_status[current_port]);

		#ifdef MEM_PRINT
		s_printf("Unplug: reset %i status %x\n", ret, status);
		#endif

		unplug_device[current_port] = 1;

		if (ret >= 0 && (status & 0x3105) == 0x1005) {
			__mounted[current_port] = 0;
			unplug_device[current_port] = 0;
			USBStorage_Close(&__usbfd[current_port]);

			retval = fast_remount();
			//usb_timeout=1000*1000;
			if (retval >= 0) {
				retval = 0;
				unplug_device[current_port] = 0;
				__mounted[current_port] = 1;
			}
			else {
				unplug_device[current_port] = 1;
				#ifdef MEM_PRINT
				s_printf("fast_remount KO ret %i\n", ret);
				#endif
				__mounted[current_port] = 0;
				retval = 1; 
				ehci_msleep(100);
			}

			__USBStorage_EnablePortChangesDetection();

			//if(USBStorage_Try_Device(&ehci->devices[0])==0) {retval=0;unplug_device=0;} else unplug_device=1;

		}

		ehci_msleep(100);
	}

	return retval;
}

s32 USBStorage_DVD_Test(void)
{
	s32 retval; 

	if (!ums_init_done[current_port])
		return 0;

	unplug_procedure();

	if (unplug_device[current_port] != 0 || __mounted[current_port] == 0)
		return 0;

	//usb_timeout = 1000*1000;
	//retval = __usbstorage_clearerrors(&__usbfd, _old_device.lun);
	//if (retval < 0) return 0;
	//usb_timeout = 1000*1000;
	retval = USBStorage_Inquiry(&__usbfd[current_port], _old_device[current_port].lun);
	if (retval < 0)
		return 0;
	//usb_timeout = 1000*1000;
	retval = USBStorage_ReadCapacity(&__usbfd[current_port], _old_device[current_port].lun, 
			&__usbfd[current_port].sector_size[_old_device[current_port].lun], &__usbfd[current_port].n_sector[_old_device[current_port].lun]);
	if (retval < 0)
		return 0;

	// sector size check for USB DVD mode (must be 2048 bytes)
	if (retval >= 0 && is_dvd[current_port]) {
		if(__usbfd[current_port].sector_size[_old_device[current_port].lun] != 2048)
			return 0;
	}

	return is_dvd[current_port];
}

//s32 is_watchdog_read_sector = 0;


s32 USBStorage_Read_Sectors(u32 sector, u32 numSectors, void *buffer)
{
	s32 retval = 0;
	int retry;

	//  if(!is_watchdog_read_sector && is_dvd[current_port]) last_sector=(int) (sector & 0x7fffffff);//return false;
	//  if(!is_watchdog_read_sector && is_dvd[current_port]) last_sector=sector;//return false;

	for (retry = 0; retry < 4; retry++) {
		//if(!is_watchdog_read_sector && !is_dvd) retry = 0; // infinite loop except for watchdog

		if (!unplug_procedure())
			retval = 0;

		if (retval < 0 || __mounted[current_port] != 1) {
			unplug_device[current_port] = 1;
			retval = -1;
		}

		if (unplug_device[current_port] != 0)
			continue;

		usb_timeout = is_dvd[current_port] ? 10000*1000 : 800*1000;

		if (retval >= 0) {

			__USBStorage_DisablePortChangesDetection();

			retval = USBStorage_Read(&__usbfd[current_port], __lun[current_port], sector, numSectors, buffer);

			__USBStorage_EnablePortChangesDetection();
		}

		usb_timeout = 500*1000;

		if (retval < 0)
			unplug_device[current_port] = 1;

		if (unplug_device[current_port] !=0 || __mounted[current_port] != 1)
			continue;

		if (retval >= 0)
			break;
		
	}

	return (retval >= 0);
}


s32 USBStorage_Write_Sectors(u32 sector, u32 numSectors, const void *buffer)
{
	s32 retval = 0;

	if (is_dvd[current_port])
		return false;

	while (1) {

		if (!unplug_procedure())
			retval = 0;

		if (retval < 0 || __mounted[current_port] != 1) {
			unplug_device[current_port] = 1;
			retval = -1;
		}

		if (unplug_device[current_port] != 0)
			continue;

		usb_timeout = 1000*1000;

		if (retval >= 0) {
			__USBStorage_DisablePortChangesDetection();

			retval = USBStorage_Write(&__usbfd[current_port], __lun[current_port], sector, numSectors, buffer);

			__USBStorage_EnablePortChangesDetection();
		}

		usb_timeout = 500*1000;
		if (retval < 0)
			unplug_device[current_port] = 1;

		if (unplug_device[current_port] != 0)
			continue;

		if (retval >= 0)
			break;
	}

	return (retval >= 0);
}