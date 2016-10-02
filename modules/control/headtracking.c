/*****************************************************************************
 * headtracking.c: headtracking interface plugin
 *****************************************************************************
 * Copyright (C) 2016 Symonics
 * $Id$
 *
 * Authors: Christian Hoene <christian.hoene@symonics.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

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

/*****************************************************************************
 * intf_sys_t: description and status of FB interface
 *****************************************************************************/
struct intf_sys_t
{
	hid_device *handle;
	vlc_timer_t discoveryTimer;
        libvlc_int_t* libvlc;
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
    set_capability( "interface", 10 )
    set_callbacks( Open, Close )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
vlc_module_end ()

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static void hidDiscovery(void *p)
{
	intf_sys_t *p_sys = (intf_sys_t*)p;
    msg_Info( p_sys->libvlc, "looking for HID devices" );
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

	/* start HID discovery timer */
	res = vlc_timer_create(&p_sys->discoveryTimer, &hidDiscovery, p_sys);
	if(res != 0) 
		return VLC_EGENERIC;

	vlc_timer_schedule(p_sys->discoveryTimer, false, 1000000, 5000000);

	/* create notification object */
    var_Create (p_this->obj.libvlc, "head-rotation", VLC_VAR_ADDRESS);
    var_AddCallback( p_intf->obj.libvlc, "head-rotation", ActionEvent, p_intf );

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

    /* destroy timer */
    vlc_timer_destroy(p_sys->discoveryTimer);

    /* Destroy structure */
    free( p_sys );

// Finalize the hidapi library
    hid_exit();
}

/*****************************************************************************
 * ActionEvent: callback for hotkey actions
 *****************************************************************************/

static int PutAction( intf_thread_t *p_intf, int i_action )
{
    intf_sys_t *p_sys = p_intf->p_sys;
    playlist_t *p_playlist = pl_Get( p_intf );

#if 0
    switch( i_action )
    {
        /* Libvlc / interface actions */
        case ACTIONID_QUIT:
        case ACTIONID_INTF_TOGGLE_FSC:
        case ACTIONID_INTF_HIDE:
            break;
    }
#endif
    return VLC_SUCCESS;
}

static int ActionEvent( vlc_object_t *libvlc, char const *psz_var,
                        vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_data;

    (void)libvlc;
    (void)psz_var;
    (void)oldval;

    return PutAction( p_intf, newval.i_int );
}


