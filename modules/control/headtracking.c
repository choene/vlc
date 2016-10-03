/*****************************************************************************
 * headtracking.c: headtracking interface plugin
 *****************************************************************************
 * Copyright (C) 2016 Christian Hoene, Symonics GmbH
 * $Id$
 *
 * Authors: Christian Hoene <christian.hoene@symonics.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License version 2.1 as published by the Free Software Foundation.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation Inc. 59 Temple Place, Suite 330, Boston MA 02111-1307 USA
 */

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define VLC_MODULE_LICENSE VLC_LICENSE_GPL_2_PLUS
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <hidapi/hidapi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/joystick.h>
#include <string.h>


/*****************************************************************************
 * intf_sys_t: description and status of FB interface
 *****************************************************************************/
struct intf_sys_t
{
        libvlc_int_t* libvlc;

	vlc_timer_t discoveryTimer;
	vlc_thread_t queryThread;

	hid_device *hidHandle;
	struct hid_device_info *hidList;

	int jsDevice;

	int running;
};

#if 0
static int vlc_key_to_action (vlc_object_t *obj, const char *varname,
                              vlc_value_t prevkey, vlc_value_t curkey, void *d)
{
    void *const *map = d;
    const struct mapping **pent;
    uint32_t keycode = curkey.i_int;

    pent = tfind (&keycode, map, keycmp);
    if (pent == NULL)
        return VLC_SUCCESS;

    (void) varname;
    (void) prevkey;
    return var_SetInteger (obj, "key-action", (*pent)->action);
}
#endif 

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Open    ( vlc_object_t * );
static void Close   ( vlc_object_t * );
static int  ActionEvent( vlc_object_t *, char const *,
                         vlc_value_t, vlc_value_t, void * );


/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

vlc_module_begin ()
    set_shortname( N_("Headtracking") )
    set_description( N_("Headtracking interface") )
    set_capability( "interface", 0 )
    set_callbacks( Open, Close )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
vlc_module_end ()

/*****************************************************************************
 * Look for proper HID devices
 *****************************************************************************/

/* TODO, on Ubuntu 14.04, hid_get_feature_report does not work proper */

static void hidVerify(intf_sys_t *p_sys, struct hid_device_info *hid)
{
	unsigned char buf[65];
	int res,i;

	// Open the device using the VID, PID,
	// and optionally the Serial number.
	hid_device *handle = hid_open(hid->vendor_id, hid->product_id, hid->serial_number);
	if(handle == NULL)
		return;

	// Read a Feature Report from the device
	buf[0] = 2;
	res = hid_get_feature_report(handle, buf, sizeof(buf));
	if(res<0) 
		printf("Feature Error %ls\n",hid_error(handle));

	// Print out the returned buffer.
	printf("Feature Report\n   ");
	for (i = 0; i < res; i++)
		printf("%02hhx ", buf[i]);
	printf("\n");

	/* TODO analyse feature report */
	hid_close(handle);
}

static int hidDiscovery(intf_sys_t *p_sys)
{
	struct hid_device_info *list, *i, *j;

	list = hid_enumerate(0,0);
	if(list == NULL)
		return 0;

	for(i = list;i != NULL; i = i->next) {
		int found = 0;
		for(j = p_sys->hidList; j!=NULL; j = j->next) {
			if(j->vendor_id == i->vendor_id &&
			   j->product_id == i->product_id &&
			   (j->serial_number==NULL || i->serial_number==NULL || !wcscmp(j->serial_number,i->serial_number))) {
				found = 1;
				break;
			}
		}
		if(!found) {
			msg_Info( p_sys->libvlc, "HID device %04X:%04X %ls %ls %ls",
				i->vendor_id, i->product_id, 
				i->manufacturer_string, i->product_string,
				i->serial_number);
//			hidVerify(p_sys, i);
			
		}

	}
	
	hid_free_enumeration(p_sys->hidList);
	p_sys->hidList = list;

	return 0; // do not find any device yet
}

static int jsDiscovery(intf_sys_t *p_sys)
{
	char filename[20];
	int file,i;

	for(i=0;i<32;i++) {
		sprintf(filename,"/dev/input/js%d",i);
		file = open(filename, O_RDONLY);
		if(file >= 0)
			break;
	}
	if(file == -1)
		return 0;

	p_sys->jsDevice = file;
	msg_Info( p_sys->libvlc, "found joystick at %s", filename);

	return 1;
}

static int jsQuery(intf_sys_t *p_sys)
{
	double axes[3] = { 0,0,0 };

	if(p_sys->jsDevice >= 0) {

		/* at start up, all events are fired */
		struct js_event event[10];
		int n;
		int fire=0;

		/* blocking read, TODO: check for thread cancel */
		n = read(p_sys->jsDevice, (void*)event, sizeof(event));
		if(n < 0) {
			msg_Info(p_sys->libvlc, "joystick error %d %s",errno,strerror(errno));
			close(p_sys->jsDevice);
			p_sys->jsDevice = -1;
			return -1;
		}
		
		for(struct js_event *e = event;n > 0; e++,n-=sizeof(*e)) {
			if((e->type & ~JS_EVENT_INIT) == JS_EVENT_AXIS && e->number>=0 && e->number<3) {
				axes[e->number] = e->value * (180. / 32767.);
				fire++;
			}
#if DEBUG
			else
				msg_Info(p_sys->libvlc, "unknown js event %9d %04X %02X %d %p\n", e->time, e->value, e->type, e->number, axes);
#endif
		}
		if(fire) {
			var_SetAddress(p_sys->libvlc, "head-rotation", axes);
		}
	}
	return 1;	
}

static void* query(void *p)
{
	volatile intf_sys_t *p_sys = (intf_sys_t*)p;
#ifdef DEBUG
	msg_Info( p_sys->libvlc, "query thread started");		
#endif

	while(p_sys->running) {
		vlc_testcancel();

		
		if(jsQuery(p_sys)<0)
			break;

	}

	p_sys->running = 0;
}

static void discovery(void *p)
{
	intf_sys_t *p_sys = (intf_sys_t*)p;

	if(p_sys->running)
		return;

	if(!hidDiscovery(p_sys) && !jsDiscovery(p_sys))
		return;

	p_sys->running = 1;
	vlc_clone(&p_sys->queryThread, &query, p_sys, VLC_THREAD_PRIORITY_INPUT);
}


/*****************************************************************************
 * Open: initialize headtracking interface
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    intf_sys_t *p_sys;
    int res;

    msg_Info( p_intf, "using the headtracking interface module..." );

	/* Initialize the hidapi library */
    res = hid_init();
    if(res != 0)
	return VLC_EGENERIC;

	/* get memory for structure */
    p_sys = malloc( sizeof( intf_sys_t ) );
    if( !p_sys )
        return VLC_ENOMEM;
    p_intf->p_sys = p_sys;

    p_sys->libvlc = p_this->obj.libvlc;
    p_sys->hidList = NULL;
    p_sys->hidHandle = NULL;
    p_sys->jsDevice = -1;
    p_sys->running = 0;

	/* start HID discovery timer */
	res = vlc_timer_create(&p_sys->discoveryTimer, &discovery, p_sys);
	if(res != 0) 
		return VLC_EGENERIC;

	vlc_timer_schedule(p_sys->discoveryTimer, false, 1000000, 5000000);

	/* create notification object */
    var_Create (p_sys->libvlc, "head-rotation", VLC_VAR_ADDRESS);
    var_AddCallback(p_sys->libvlc, "head-rotation", ActionEvent, p_sys );

    msg_Info( p_intf, "done" );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: destroy interface
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    intf_sys_t *p_sys = p_intf->p_sys;

    var_DelCallback( p_intf->obj.libvlc, "head-rotation", ActionEvent, p_intf );

    /* destroy thread */
    if(p_sys->running) {
	vlc_cancel(p_sys->queryThread);
	vlc_join(p_sys->queryThread, NULL);
    }

    /* destroy timer */
    vlc_timer_destroy(p_sys->discoveryTimer);

    if(p_sys->hidList)
	hid_free_enumeration(p_sys->hidList);
    /* Destroy structure */
    free( p_sys );

// Finalize the hidapi library
    hid_exit();
}

/*****************************************************************************
 * ActionEvent: callback for hotkey actions
 *****************************************************************************/

static int ActionEvent( vlc_object_t *libvlc, char const *psz_var,
                        vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
	intf_sys_t *p_sys = (intf_sys_t*)p_data;
	double *axes = (double*) newval.p_address;
	 msg_Info(  libvlc, "%s yaw %4.0f pitch %4.0f roll %4.0f", psz_var, axes[0], axes[1], axes[2]);
	return VLC_SUCCESS;
}


