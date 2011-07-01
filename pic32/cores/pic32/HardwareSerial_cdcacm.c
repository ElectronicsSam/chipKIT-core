//************************************************************************
//	pic32_cdcacm.c
//
// this file implements a CDC/ACM transport on top of the usb driver module.
//
// This file originated from the cpustick.com skeleton project from
// http://www.cpustick.com/downloads.htm and was originally written
// by Rich Testardi; please preserve this reference.
//************************************************************************
//*	Edit History
//************************************************************************
//*	Jun 23,	2011	<MLS> Got code from Rich, started on Serial support for Arduino/chipkit
//*	Jun 24,	2011	<MLS> USBSerial working completely
//************************************************************************


#include <plib.h>

//#include "main.h"
#include	"HardwareSerial.h"
//*	make sure the cpu selected has a usb port
#if defined(_USB) && defined(_USE_USB_FOR_SERIAL_)


#include <stdarg.h>
#include <stdint.h>


#include	"HardwareSerial_cdcacm.h"
#include	"HardwareSerial_usb.h"

#define PACKET_SIZE	64

// We have allocated 8 PIDs to you from A660 to A667 (hex).
// The PIDs must be used with VID 0403.
// We use A660; Cale Fallgatter uses A661; Jim Donelson uses A667;
// Avrbootloader group (this project) uses A662.
#define CDCACM_VID	0x0403
#define CDCACM_PID	0xA662
#define CDCACM_RID	0x0180

static const byte cdcacm_device_descriptor[] = {
	18,					// length
	0x01,				// device descriptor
	0x01, 0x01,			// 1.1
	0x02, 0x00, 0x00,	// class (cdc), subclass, protocol
	PACKET_SIZE,		// packet size
	CDCACM_VID%0x100, CDCACM_VID/0x100, CDCACM_PID%0x100, CDCACM_PID/0x100,
	CDCACM_RID%0x100, CDCACM_RID/0x100,
	0x01,				// manufacturer (string)
	0x02,				// product (string)
	0x00,				// sn (string)
	0x01				// num configurations
};

static const byte cdcacm_configuration_descriptor[] = {
	9,	// length
	0x02,	// configuration descriptor
	67, 0,	// total length
	0x02,	// num interfaces
	0x01,	// configuration value
	0x00,	// configuration (string)
	0x80,	// attributes
	250,	// 500 mA

	9,		// length
	0x04,	// interface descriptor
	0x00,	// interface number
	0x00,	// alternate
	0x01,	// num endpoints
	0x02,	// interface class (comm)
	0x02,	// subclass (acm)
	0x01,	// protocol (at)
	0x00,	// interface (string)

	5,		// length
	0x24,	// header functional descriptor
	0x00,
	0x10, 0x01,

	4,		// length
	0x24,	// abstract control model descriptor
	0x02,
	0x00,
	
	5,		// length
	0x24,	// union functional descriptor
	0x06,
	0x00,	// comm
	0x01,	// data
	
	5,		// length
	0x24,	// call management functional descriptor
	0x01,
	0x00,
	0x01,
	
	7,		// length
	0x05,	// endpoint descriptor
	0x81,	// endpoint IN address
	0x03,	// attributes: interrupt
	0x08, 0x00,	// packet size
	0x10,	// interval (ms)
	
	9,		// length
	0x04,	// interface descriptor
	0x01,	// interface number
	0x00,	// alternate
	0x02,	// num endpoints
	0x0a,	// interface class (data)
	0x00,	// subclass
	0x00,	// protocol
	0x00,	// interface (string)

	7,		// length
	0x05,	// endpoint descriptor
	0x82,	// endpoint IN address
	0x02,	// attributes: bulk
	0x40, 0x00,	// packet size
	0x00,	// interval (ms)

	7,			// length
	0x05,		// endpoint descriptor
	0x03,		// endpoint OUT address
	0x02,		// attributes: bulk
	0x40, 0x00,	// packet size
	0x00,		// interval (ms)
};

static const byte cdcacm_string_descriptor[] = {
	4,			// length
	0x03,		// string descriptor
	0x09, 0x04,	// english (usa)

	34,	// length
	0x03,	// string descriptor
	'w', 0, 'w', 0, 'w', 0, '.', 0, 'c', 0, 'p', 0, 'u', 0, 's', 0, 't', 0, 'i', 0, 'c', 0, 'k', 0, '.', 0, 'c', 0, 'o', 0, 'm', 0,

	18,	// length
	0x03,	// string descriptor
	'S', 0, 't', 0, 'k', 0, '5', 0, '0', 0, '0', 0, 'v', 0, '2', 0,
};

boolean cdcacm_active;

//*	cbfn	=	Call Back FuNction
static	cdcacm_reset_cbfn		reset_cbfn;
static	cdcacm_storedata_cbfn	storedata_cbfn;

static byte tx[PACKET_SIZE];	// packet from host

//#define NRX	16
#define NRX	4		//*	make memory foot print smaller

// N.B. -1 forces short packets
static byte rx[NRX][PACKET_SIZE-1]; // packets to host
static int rx_length[NRX];

static byte rx_in;
static byte rx_out;

static boolean discard;	// true when we don't think anyone is listening

unsigned char	gRecevieBuffer[kUSB_RCB_BuffSize];
unsigned short	gRcbBuff_Head;
unsigned short	gRcbBuff_Tail;

//****************************************************************
// this function receives bytes from the CDC/ACM port
// N.B. if this routine returns false, cdcacm will drop the ball and we'll
// call cdcacm_command_ack() later to pick it up again.
//****************************************************************
static boolean	SaveReceivedData(const byte *buffer, int length)
{
unsigned int ii;

	for (ii=0; ii<length; ii++)
	{
		gRecevieBuffer[gRcbBuff_Head]	=	buffer[ii];
		gRcbBuff_Head++;
		if (gRcbBuff_Head >= kUSB_RCB_BuffSize)
		{
			gRcbBuff_Head	=	0;
		}
	}

	return true;
}


//************************************************************************
// this function waits for space to be available in the transport
// buffers and then prints the specified line to the CDCACM transport
// console.
//************************************************************************
void	cdcacm_print(const byte *buffer, int length)
{
	int m;
	static uint32 attached_count;
	
	if (! cdcacm_attached || discard)
	{
		return;
	}

	while (rx_in != rx_out)
	{
	#ifdef _USE_USB_IRQ_
		delayMicroseconds(100);
	#else
		// XXX -- replace with a delay for interrupt use
		usb_isr();
	#endif
	}
	
	if (! length)
	{
		return;
	}
	
	ASSERT(rx_in == rx_out);

	// append to next rx_in(s)
	do
	{
		m	=	MIN(length, sizeof(rx[rx_in])-rx_length[rx_in]);

		assert(rx_length[rx_in]+m <= sizeof(rx[rx_in]));
		memcpy(rx[rx_in]+rx_length[rx_in], buffer, m);
		rx_length[rx_in] += m;

		buffer += m;
		length -= m;

		if (length)
		{
			assert(rx_length[rx_in] == sizeof(rx[rx_in]));
		}
		
		rx_in	=	(rx_in+1)%NRX;
		assert(rx_in != rx_out);
		assert(! rx_length[rx_in]);
	} while (length);

	// start the rx ball rolling
	assert(rx_out != rx_in);
	assert(rx_length[rx_out] > 0 && rx_length[rx_out] < PACKET_SIZE);
	usb_device_enqueue(bulk_in_ep, 1, rx[rx_out], rx_length[rx_out]);
}


/* Mandatory class specific requests. */
#define CDCRQ_SEND_ENCAPSULATED_COMMAND 0x0
#define CDCRQ_GET_ENCAPSULATED_RESPONSE 0x1

/* Optional class specific requests. Windows usbser.sys depends on these. */
#define CDCRQ_SET_LINE_CODING			0x20
#define CDCRQ_GET_LINE_CODING			0x21
#define CDCRQ_SET_CONTROL_LINE_STATE	0x22
#define CDCRQ_SEND_BREAK				0x23

#define FILL_LINE_CODING(bps, stops, parity, data_bits) \
	(bps) & 0xff, ((bps)>>8) & 0xff, ((bps)>>16) & 0xff, ((bps)>>24) & 0xff, (uint8)(stops), (uint8)(parity), (uint8)(data_bits)

static uint8 line_coding[7]	=	{
	FILL_LINE_CODING(115200, 0, 0, 8) /* Default is 115200 BPS and 8N1 format. */
};

//************************************************************************
// this function implements the CDCACM usb setup control transfer.
//************************************************************************
static int	cdcacm_control_transfer(struct setup *setup, byte *buffer, int length)
{
#if SODEBUG
	if ((setup->requesttype & 0x60) != (SETUP_TYPE_CLASS<<5))
	{
		return 0;
	}
	if ((setup->requesttype & 0x1f) != (SETUP_RECIP_INTERFACE<<0))
	{
		return 0;
	}
	if (setup->index != 0 /*comm*/)
	{
		return 0;
	}
#endif
	switch(setup->request)
	{
		case CDCRQ_SEND_ENCAPSULATED_COMMAND:
			assert(! (setup->requesttype & 0x80));
			length	=	0;
			break;
		case CDCRQ_GET_ENCAPSULATED_RESPONSE:
			assert(setup->requesttype & 0x80);
			assert(length <= 64);
			memset(buffer, 0, length);
			break;
		case CDCRQ_SET_LINE_CODING:
			assert(! (setup->requesttype & 0x80));
			assert(length == sizeof(line_coding));
			memcpy(line_coding, buffer, sizeof(line_coding));
			length	=	0;
			break;
		case CDCRQ_GET_LINE_CODING:
			assert(setup->requesttype & 0x80);
			assert(length == sizeof(line_coding));
			memcpy(buffer, line_coding, sizeof(line_coding));
			break;
		case CDCRQ_SET_CONTROL_LINE_STATE:
			assert(! (setup->requesttype & 0x80));
			length	=	0;
			break;
		case CDCRQ_SEND_BREAK:
			length	=	0;
			break;
		default:
			assert(0);
			length	=	0;
			break;
	}
	
	return length;
}


static boolean waiting;

// this function acknowledges receipt of an CDCACM command from upper
// level code.
//************************************************************************
void	cdcacm_command_ack(void)
{
	if (waiting)
	{
		// start the tx ball rolling
		usb_device_enqueue(bulk_out_ep, 0, tx, sizeof(tx));
		waiting	=	false;
	}
}

// this function implements the CDCACM usb bulk transfer.
//************************************************************************
static int	cdcacm_bulk_transfer(boolean in, byte *buffer, int length)
{
	if (! in)
	{
		discard	=	false;
	
		cdcacm_active	=	true;
		
		// accumulate commands
	//	if (SaveReceivedData(buffer, length))
		if (storedata_cbfn(buffer, length))		//*	call the store data call back function
		{
			// keep the tx ball rolling
			usb_device_enqueue(bulk_out_ep, 0, tx, sizeof(tx));
		}
		else
		{
			// drop the ball
			waiting	=	true;
		}
	}
	else
	{
		rx_length[rx_out]	=	0;
		rx_out	=	(rx_out+1)%NRX;

		// if there is more data to transfer...
		if (rx_length[rx_out] > 0)
		{
			if (rx_in == rx_out)
			{
				rx_in	=	(rx_in+1)%NRX;
				assert(rx_in != rx_out);
				assert(rx_length[rx_in] == 0);
			}

			// keep the rx ball rolling
			assert(rx_out != rx_in);
			assert(rx_length[rx_out] > 0 && rx_length[rx_out] < PACKET_SIZE);
			usb_device_enqueue(bulk_in_ep, 1, rx[rx_out], rx_length[rx_out]);
		}
	}

	return 0;
}

//************************************************************************
// this function is called by the usb driver when the USB device
// is reset.
//************************************************************************
static void	cdcacm_reset(void)
{
	int i;

	for (i = 0; i < NRX; i++)
	{
		rx_length[i]	=	0;
	}

	// prepare for rx
	usb_device_enqueue(bulk_in_ep, -1, NULL, 0);
	usb_device_enqueue(int_ep, -1, NULL, 0);
	
	// start the tx ball rolling
	usb_device_enqueue(bulk_out_ep, 0, tx, sizeof(tx));
	
	assert(reset_cbfn);
	reset_cbfn();
}

//************************************************************************
static int check(const byte *descriptor, int length)
{
	int i;
	int j;

	i	=	0;
	j	=	0;
	while (i < length)
	{
		i += descriptor[i];
		j++;
	}
	assert(i == length);
	return j;
}

//************************************************************************
// this function is called by upper level code to register callback
// functions.
//************************************************************************
void	cdcacm_register(cdcacm_reset_cbfn reset, cdcacm_storedata_cbfn storeData)
{
	int i;

	for (i = 0; i < NRX; i++)
	{
		rx_length[i]	=	0;
	}

	reset_cbfn		=	reset;
	storedata_cbfn	=	storeData;
	
	usb_register(cdcacm_reset, cdcacm_control_transfer, cdcacm_bulk_transfer);

	assert(check(cdcacm_device_descriptor, sizeof(cdcacm_device_descriptor)) == 1);
	usb_device_descriptor(cdcacm_device_descriptor, sizeof(cdcacm_device_descriptor));

	assert(check(cdcacm_configuration_descriptor, sizeof(cdcacm_configuration_descriptor)) == 10);
	usb_configuration_descriptor(cdcacm_configuration_descriptor, sizeof(cdcacm_configuration_descriptor));

	assert(check(cdcacm_string_descriptor, sizeof(cdcacm_string_descriptor)) == 3);
	usb_string_descriptor(cdcacm_string_descriptor, sizeof(cdcacm_string_descriptor));
}

/*
From: CDCACM Support
To: 'Rich Testardi at Home' 
Sent: Monday, March 24, 2008 4:46 AM
Subject: RE: Custom PID?

Hello,

We only give out the PIDs in blocks.

We have allocated 8 PIDs to you from A660 to A667 (hex).

The PIDs must be used with VID 0403.

To reprogram the EEPROM use MPROG.

The help appendix of this utility will show you how to edit the driver for
your new identity.

http://www.cdcacmchip.com/Resources/Utilities/MProg3.0_Setup.exe

NOTE 1: Editing the driver for your new identity will invalidate any current
driver certification.

NOTE 2: It will be necessary for you to maintain your own edited driver
release for distribution to your customers.

Regards,
 
Gordon Lunn
Support Engineer

CDCACM Ltd
373 Scotland Street
Glasgow
Scotland
UK
G5 8QB

Tel:	 +44 (0) 141 429 2777
Fax:	+44 (0) 141 429 2758
Skype: cdcacm.support2
Web:	www.cdcacmchip.com

-----Original Message-----
From: Rich Testardi at Home [mailto:rich@testardi.com] 
Sent: 24 March 2008 06:45
To: Support1
Subject: Custom PID?

Hi,

I was wondering if I could get a custom PID to use with your VID?
(I just need one, not a block of 8, if that matters...  I just want
to be able to control the version of the driver for my hardware
independent of the version of the driver for two other CDCACM chips
I already have connected to my development PC!)

Name: Rich Testardi
Company: Incipient, Inc.
Country: USA
E-Mail address: rich@testardi.com

Thank you!

-- Rich
*/


#endif	//	defined(_USB) && defined(_USE_USB_FOR_SERIAL_)