#include <ananas/types.h>
#include <ananas/device.h>
#include <ananas/driver.h>
#include <ananas/error.h>
#include <ananas/endian.h>
#include <ananas/lib.h>
#include <ananas/lock.h>
#include <ananas/thread.h>
#include <ananas/pcpu.h>
#include <ananas/schedule.h>
#include <ananas/trace.h>
#include <ananas/mm.h>
#include "../core/usb-core.h"
#include "../core/usb-device.h"
#include "../core/usb-transfer.h"
#include "../core/config.h"
#include "../../scsi/scsi.h" // XXX kludge

TRACE_SETUP;

#if 0
#define DPRINTF Printf
#else
#define DPRINTF(...)
#endif

namespace {

struct USBSTORAGE_CBW {
	/* 00-03 */ uint32_t d_cbw_signature;
#define USBSTORAGE_CBW_SIGNATURE 0x43425355
	/* 04-07 */ uint32_t d_cbw_tag;
	/* 08-0b */ uint32_t d_cbw_data_transferlength;
	/* 0c */    uint8_t  d_bm_cbwflags;
#define USBSTORAGE_CBW_FLAG_DATA_OUT (0 << 7)
#define USBSTORAGE_CBW_FLAG_DATA_IN  (1 << 7)
	/* 0d */    uint8_t  d_cbw_lun;
	/* 0e */    uint8_t  d_cbw_cblength;
	/* 0f-1e */ uint8_t  d_cbw_cb[16];
} __attribute__((packed));

struct USBSTORAGE_CSW {
	/* 00-03 */ uint32_t d_csw_signature;
#define USBSTORAGE_CSW_SIGNATURE 0x53425355
	/* 04-07 */ uint32_t d_csw_tag;
	/* 08-0b */ uint32_t d_csw_data_residue;
	/* 0c */    uint8_t  d_csw_status;
#define USBSTORAGE_CSW_STATUS_GOOD 0x00
#define USBSTORAGE_CSW_STATUS_FAIL 0x01
#define USBSTORAGE_CSW_STATUS_PHASE_ERROR 0x02
} __attribute__((packed));

#if 0
void
DumpCBW(USBSTORAGE_CBW& cbw)
{
	kprintf("signature %x tag %x data_transferlen %x\n",
	 cbw.d_cbw_signature, cbw.d_cbw_tag, cbw.d_cbw_data_transferlength);
	kprintf("cbwflags %x lun %d cblength %d cb",
	 cbw.d_bm_cbwflags, cbw.d_cbw_lun, cbw.d_cbw_cblength);
	for(unsigned int n = 0; n < 16; n++)
		kprintf(" %x", cbw.d_cbw_cb[n]);
	kprintf("\n");
}
#endif

class USBStorage;

struct StorageDevice_PipeInCallbackWrapper : public Ananas::USB::IPipeCallback
{
	StorageDevice_PipeInCallbackWrapper(USBStorage& device)
	 : pi_Device(device)
	{
	}

	void OnPipeCallback(Ananas::USB::Pipe& pipe) override;

	USBStorage& pi_Device;
};

struct StorageDevice_PipeOutCallbackWrapper : public Ananas::USB::IPipeCallback
{
	StorageDevice_PipeOutCallbackWrapper(USBStorage& device)
	 : pi_Device(device)
	{
	}

	void OnPipeCallback(Ananas::USB::Pipe& pipe) override;

	USBStorage& pi_Device;
};

class USBStorage : public Ananas::Device, private Ananas::IDeviceOperations, private Ananas::ISCSIDeviceOperations
{
public:
	USBStorage(const Ananas::CreateDeviceProperties& cdp);
	virtual ~USBStorage() = default;

	IDeviceOperations& GetDeviceOperations() override
	{
		return *this;
	}

	ISCSIDeviceOperations* GetSCSIDeviceOperations() override
	{
		return this;
	}

	void OnPipeInCallback();
	void OnPipeOutCallback();

	errorcode_t PerformSCSIRequest(int lun, ISCSIDeviceOperations::Direction dir, const void* cb, size_t cb_len, void* result, size_t* result_len) override;

protected:
	errorcode_t Attach() override;
	errorcode_t Detach() override;

private:
	void Lock()
	{
		mutex_lock(&us_mutex);
	}

	void Unlock()
	{
		mutex_unlock(&us_mutex);
	}

	Ananas::USB::USBDevice* us_Device = nullptr;
	Ananas::USB::Pipe* us_BulkIn = nullptr;
	Ananas::USB::Pipe* us_BulkOut = nullptr;

	StorageDevice_PipeInCallbackWrapper us_PipeInCallback;
	StorageDevice_PipeOutCallbackWrapper us_PipeOutCallback;

	mutex_t us_mutex;
	unsigned int us_max_lun = 0;
	/* Output buffer */
	void* us_output_buffer = nullptr;
	size_t us_output_filled = 0;
	size_t us_output_len = 0;
	/* Most recent CSW received */
	errorcode_t* us_result_ptr = nullptr;
	struct USBSTORAGE_CSW* us_csw_ptr = nullptr;

	/* Signalled when the CSW is received */
	semaphore_t us_signal_sem;
};

void StorageDevice_PipeInCallbackWrapper::OnPipeCallback(Ananas::USB::Pipe& pipe)
{
	pi_Device.OnPipeInCallback();
}

void StorageDevice_PipeOutCallbackWrapper::OnPipeCallback(Ananas::USB::Pipe& pipe)
{
	pi_Device.OnPipeOutCallback();
}

USBStorage::USBStorage(const Ananas::CreateDeviceProperties& cdp)
	: Device(cdp), us_PipeInCallback(*this), us_PipeOutCallback(*this)
{
}

errorcode_t
USBStorage::PerformSCSIRequest(int lun, Direction dir, const void* cb, size_t cb_len, void* result, size_t* result_len)
{
	KASSERT(result_len == nullptr || result != nullptr, "result_len without result?");
	DPRINTF("dir %d len %d cb_len %d result_len %d", dir, lun, cb_len, result_len ? *result_len : -1);

	struct USBSTORAGE_CSW csw;
	errorcode_t err = ANANAS_ERROR(UNKNOWN);

	/* Create the command-block-wrapper */
	struct USBSTORAGE_CBW cbw;
	memset(&cbw, 0, sizeof cbw);
	cbw.d_cbw_signature = USBSTORAGE_CBW_SIGNATURE;
	cbw.d_cbw_data_transferlength = (result_len != NULL) ? *result_len : 0;
	cbw.d_bm_cbwflags = (dir == Direction::D_In) ? USBSTORAGE_CBW_FLAG_DATA_IN : USBSTORAGE_CBW_FLAG_DATA_OUT;
	cbw.d_cbw_lun = lun;
	cbw.d_cbw_cblength = cb_len;
	memcpy(&cbw.d_cbw_cb[0], cb, cb_len);

	/* All SCSI CDB's follow a standard format; fill out those fields here XXX This is not the correct place */
	switch(cb_len) {
		case 6: {
			struct SCSI_CDB_6* cdb = (struct SCSI_CDB_6*)&cbw.d_cbw_cb[0];
			cdb->c_alloc_len = (result_len != nullptr) ? *result_len : 0;
			break;
		}
		case 10: {
			struct SCSI_CDB_10* cdb = (struct SCSI_CDB_10*)&cbw.d_cbw_cb[0];
			uint16_t v = (result_len != nullptr) ? *result_len : 0;
			if (cdb->c_code != SCSI_CMD_READ_10) /* XXX */
				cdb->c_alloc_len = htobe16(v);
			break;
		}
		default:
			panic("invalid cb_len %d", cb_len);
	}

	Lock();
	/* Ensure our output is at a sensible location */
	us_output_buffer = result;
	us_output_filled = 0;
	us_output_len = (result_len != NULL) ? *result_len : 0;
	us_result_ptr = &err;
	us_csw_ptr = &csw;
	/* Now, submit the request */
	us_BulkOut->p_xfer.t_length = sizeof(cbw);
	memcpy(&us_BulkOut->p_xfer.t_data[0], &cbw, us_BulkOut->p_xfer.t_length);

	us_BulkOut->Start();
	Unlock();

	/* Now we wait for the signal ... */
	sem_wait_and_drain(&us_signal_sem);
	ANANAS_ERROR_RETURN(err);

	/* See if the CSW makes sense */
	if (csw.d_csw_signature != USBSTORAGE_CSW_SIGNATURE)
		return ANANAS_ERROR(IO);
	if (csw.d_csw_tag != cbw.d_cbw_tag)
		return ANANAS_ERROR(IO);
	if (csw.d_csw_status != USBSTORAGE_CSW_STATUS_GOOD) {
		DPRINTF("device rejected request: %d", csw.d_csw_status);
		return ANANAS_ERROR(IO);
	}

	return ananas_success();
}

/* Called when data flows from the device -> us */
void
USBStorage::OnPipeInCallback()
{
	Ananas::USB::Transfer& xfer = us_BulkIn->p_xfer;

	DPRINTF("usbstorage_in_callback! -> flags %x len %d", xfer.t_flags, xfer.t_result_length);

	/*
	 * We'll have one or two responses now: the first will be the resulting data,
	 * and the second will be the CSW.
	 */
	bool need_schedule = false;
	Lock();
	int len = xfer.t_result_length;
	if (us_output_buffer != nullptr) {
		size_t left = us_output_len - us_output_filled;
		if (len > left)
			len = left;

		memcpy((char*)us_output_buffer + us_output_filled, &xfer.t_data[0], len);
		us_output_filled += len;
		left -= len;
		if (left == 0) {
			us_output_len = len;
			us_output_buffer = nullptr;
		}
		need_schedule = true; /* as more data will arrive */
	} else if (us_csw_ptr != nullptr && us_result_ptr != nullptr) {
		if (len != sizeof(struct USBSTORAGE_CSW)) {
			Printf("invalid csw length (expected %d got %d)", sizeof(struct USBSTORAGE_CSW), len);
			*us_result_ptr = ANANAS_ERROR(BAD_LENGTH);
		} else {
			memcpy(us_csw_ptr, &xfer.t_data[0], len);
			*us_result_ptr = ananas_success();
		}
		us_result_ptr = nullptr;
		us_csw_ptr = nullptr;
		sem_signal(&us_signal_sem);
	} else {
		Printf("received %d bytes but no sink?", len);
	}
	Unlock();

	if (need_schedule)
		us_BulkIn->Start();
}

void
USBStorage::OnPipeOutCallback()
{
	Ananas::USB::Transfer& xfer = us_BulkOut->p_xfer;
	(void)xfer;

	DPRINTF("usbstorage_out_callback! -> len %d", xfer.t_result_length);

	us_BulkIn->Start();
}

errorcode_t
USBStorage::Attach()
{
	us_Device = static_cast<Ananas::USB::USBDevice*>(d_ResourceSet.AllocateResource(Ananas::Resource::RT_USB_Device, 0));

	mutex_init(&us_mutex, "usbstorage");
	sem_init(&us_signal_sem, 0);

	/*
	 * Determine the max LUN of the device - note that devices do not have to support this,
	 * so we use 0 in case they do not give this information.
	 */
	uint8_t max_lun;
	size_t len = sizeof(max_lun);
	errorcode_t err = us_Device->PerformControlTransfer(USB_CONTROL_REQUEST_GET_MAX_LUN, USB_CONTROL_RECIPIENT_INTERFACE, USB_CONTROL_TYPE_CLASS, USB_REQUEST_MAKE(0, 0), 0, &max_lun, &len, false);
	if (ananas_is_failure(err) || len != sizeof(max_lun))
		max_lun = 0;
	us_max_lun = max_lun;

	/*
	 * There must be a BULK/IN and BULK/OUT endpoint - however, the spec doesn't
	 * say in which order they are. To cope, we'll just try both.
	 */
	int outep_index = 1;
	err = us_Device->AllocatePipe(0, TRANSFER_TYPE_BULK, EP_DIR_IN, 0, us_PipeInCallback, us_BulkIn);
	if (ananas_is_failure(err)) {
		err = us_Device->AllocatePipe(1, TRANSFER_TYPE_BULK, EP_DIR_IN, 0, us_PipeInCallback, us_BulkIn);
		outep_index = 0;
	}
	if (ananas_is_failure(err)) {
		Printf("no bulk/in endpoint present");
		return ANANAS_ERROR(NO_RESOURCE);
	}
	err = us_Device->AllocatePipe(outep_index, TRANSFER_TYPE_BULK, EP_DIR_OUT, 0, us_PipeOutCallback, us_BulkOut);
	if (ananas_is_failure(err)) {
		Printf("no bulk/out endpoint present");
		return ANANAS_ERROR(NO_RESOURCE);
	}

	// Now create SCSI disks for all LUN's here
	for (unsigned int lun = 0; lun <= us_max_lun; lun++)
	{
		Ananas::ResourceSet sub_resourceSet;
		sub_resourceSet.AddResource(Ananas::Resource(Ananas::Resource::RT_ChildNum, lun, 0));

		Ananas::Device* sub_device = Ananas::DeviceManager::CreateDevice("scsidisk", Ananas::CreateDeviceProperties(*this, sub_resourceSet));
		if (sub_device == nullptr)
			continue;
		Ananas::DeviceManager::AttachSingle(*sub_device);
	}

	return ananas_success();
}

errorcode_t
USBStorage::Detach()
{
	if (us_Device == nullptr)
		return ananas_success();

	if (us_BulkIn != nullptr)
		us_Device->FreePipe(*us_BulkIn);
	us_BulkIn = nullptr;

	if (us_BulkOut != nullptr)
		us_Device->FreePipe(*us_BulkOut);
	us_BulkOut = nullptr;
	return ananas_success();
}

struct USBStorage_Driver : public Ananas::Driver
{
	USBStorage_Driver()
	 : Driver("usbstorage")
	{
	}

	const char* GetBussesToProbeOn() const override
	{
		return "usbbus";
	}

	Ananas::Device* CreateDevice(const Ananas::CreateDeviceProperties& cdp) override
	{
		auto res = cdp.cdp_ResourceSet.GetResource(Ananas::Resource::RT_USB_Device, 0);
		if (res == nullptr)
			return nullptr;

		auto usb_dev = static_cast<Ananas::USB::USBDevice*>(reinterpret_cast<void*>(res->r_Base));

		Ananas::USB::Interface& iface = usb_dev->ud_interface[usb_dev->ud_cur_interface];
		if (iface.if_class == USB_IF_CLASS_STORAGE && iface.if_protocol == USB_IF_PROTOCOL_BULKONLY)
			return new USBStorage(cdp);
		return nullptr;
	}
};

} // unnamed namespace

REGISTER_DRIVER(USBStorage_Driver)

/* vim:set ts=2 sw=2: */
