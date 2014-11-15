/*****************************************************************************
 * PLItem.m: MacOS X interface module
 *****************************************************************************
 * Copyright (C) 2014 VLC authors and VideoLAN
 * $Id$
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

#import "PLItem.h"

#include <vlc_playlist.h>
#include <vlc_input_item.h>

#pragma mark -
;
@implementation PLItem

@synthesize children=_children;
@synthesize plItemId=_playlistId;
@synthesize input=p_input;
@synthesize parent=_parent;

- (id)initWithPlaylistItem:(playlist_item_t *)p_item parent:(PLItem *)parent;
{
    self = [super init];
    if(self) {
        _playlistId = p_item->i_id;

        p_input = p_item->p_input;
        input_item_Hold(p_input);
        _children = [[NSMutableArray alloc] init];
        [parent retain];
        _parent = parent;
    }

    return self;
}

- (void)dealloc
{
    input_item_Release(p_input);
    [_children release];
    [_parent release];

    [super dealloc];
}


- (BOOL)isLeaf
{
    return [_children count] == 0;
}

- (void)clear
{
    [_children removeAllObjects];
}

- (void)addChild:(PLItem *)item atPos:(int)pos
{
//    if ([o_children count] > pos) {
//        NSLog(@"invalid position %d", pos);
//    }
    [_children insertObject:item atIndex:pos];

}

- (void)deleteChild:(PLItem *)child
{
    [_children removeObject:child];
}

@end