/**
 ** Chunkter.cc - Chunk terrain (16x16 flat tiles) on the map.
 **
 ** Written: 7/6/01 - JSF
 **/

/*
Copyright (C) 2001-2022 The Exult Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif
#include "chunkter.h"

#include "gamewin.h"
#include "iwin8.h"

#include <cstring>

Chunk_terrain* Chunk_terrain::render_queue = nullptr;
int            Chunk_terrain::queue_size   = 0;

/*
 *  Insert at start of render queue.  It may already be there, but it's
 *  assumed that it's already tested as not being at the start.
 */

void Chunk_terrain::insert_in_queue() {
	if (render_queue_next) {    // In queue already?
		// !!Assuming it's not at head!!
		render_queue_next->render_queue_prev = render_queue_prev;
		render_queue_prev->render_queue_next = render_queue_next;
	} else {
		queue_size++;    // Adding, so increment count.
	}
	if (!render_queue) {    // First?
		render_queue_next = render_queue_prev = this;
	} else {
		render_queue_next                    = render_queue;
		render_queue_prev                    = render_queue->render_queue_prev;
		render_queue_prev->render_queue_next = this;
		render_queue->render_queue_prev      = this;
	}
	render_queue = this;
}

/*
 *  Remove from render queue.
 */

void Chunk_terrain::remove_from_queue() {
	if (!render_queue_next) {
		return;    // Not in queue.
	}
	queue_size--;
	if (render_queue_next == this) {    // Only element?
		render_queue = nullptr;
	} else {
		if (render_queue == this) {
			render_queue = render_queue_next;
		}
		render_queue_next->render_queue_prev = render_queue_prev;
		render_queue_prev->render_queue_next = render_queue_next;
	}
	render_queue_next = render_queue_prev = nullptr;
}

/*
 *  Paint a flat tile into our cached buffer.
 */

inline void Chunk_terrain::paint_tile(
		int tilex, int tiley    // Tile within chunk.
) {
	Shape_frame* shape = get_shape(tilex, tiley);
	if (shape && !shape->is_rle()) {    // Only do flat tiles.
		rendered_flats->copy8(
				shape->get_data(), c_tilesize, c_tilesize, tilex * c_tilesize,
				tiley * c_tilesize);
	}
}

/*
 *  Create list for a given chunk.
 */

Chunk_terrain::Chunk_terrain(
		const unsigned char* data,        // Chunk data.
		bool                 v2_chunks    // 3 bytes/shape.
		)
		: undo_shapes(nullptr), num_clients(0), modified(false),
		  rendered_flats(nullptr), render_queue_next(nullptr),
		  render_queue_prev(nullptr) {
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			int shnum;
			int frnum;
			if (v2_chunks) {
				shnum = data[0] + 256 * data[1];
				frnum = data[2];
				data += 3;
			} else {
				shnum = data[0] + 256 * (data[1] & 3);
				frnum = (data[1] >> 2) & 0x1f;
				data += 2;
			}
			const ShapeID id(shnum, frnum);
			shapes[16 * tiley + tilex] = id;
		}
	}
}

/*
 *  Copy another.  The 'modified' flag is set to true.
 */

Chunk_terrain::Chunk_terrain(const Chunk_terrain& c2)
		: undo_shapes(nullptr), num_clients(0), modified(true),
		  rendered_flats(nullptr), render_queue_next(nullptr),
		  render_queue_prev(nullptr) {
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			shapes[16 * tiley + tilex] = c2.shapes[16 * tiley + tilex];
		}
	}
}

/*
 *  Clean up.
 */

Chunk_terrain::~Chunk_terrain() {
	delete[] undo_shapes;
	delete rendered_flats;
	remove_from_queue();
}

/*
 *  Set tile's shape.
 *  NOTE:  Set's 'modified' flag.
 */

void Chunk_terrain::set_flat(int tilex, int tiley, const ShapeID& id) {
	if (!undo_shapes) {    // Create backup.
#ifdef M9B
		undo_shapes = new Flat[256];
#else
		undo_shapes = new ShapeID[256];
#endif
		std::memcpy(
				reinterpret_cast<char*>(undo_shapes),
				reinterpret_cast<char*>(&shapes[0]), sizeof(shapes));
	}
	shapes[16 * tiley + tilex] = id;
	modified                   = true;
}

/*
 *  Commit changes.
 *
 *  Output: True if this was edited, else false.
 */

bool Chunk_terrain::commit_edits() {
	if (!undo_shapes) {
		return false;
	}
	delete[] undo_shapes;
	undo_shapes = nullptr;
	render_flats();    // Update with new data.
	return true;
}

/*
 *  Undo changes.   Note:  We don't clear 'modified', since this could
 *  still have been moved to a different position.
 */

void Chunk_terrain::abort_edits() {
	if (undo_shapes) {
		std::memcpy(
				reinterpret_cast<char*>(&shapes[0]),
				reinterpret_cast<char*>(undo_shapes), sizeof(shapes));
		delete[] undo_shapes;
		undo_shapes = nullptr;
	}
}

/*
 *  Figure max. queue size for given game window.
 */
static int Figure_queue_size() {
	// Game_window *gwin = Game_window::get_instance();
	// int w = gwin->get_width(), h = gwin->get_height();
	//  Figure # chunks, rounding up.
	// int cw = (w + c_chunksize - 1)/c_chunksize,
	//     ch = (h + c_chunksize - 1)/c_chunksize;
	//  Add extra in each dir.
#ifdef M9B
	return 24;    // M9B: 16 KB each; 320 x 200 shows about 12 chunks
#else
	return 100;    //(cw + 3)*(ch + 3);
#endif
}

/*
 *  Create rendered_flats buffer.
 */

Image_buffer8* Chunk_terrain::render_flats() {
	if (!rendered_flats) {
		if (queue_size > Figure_queue_size()) {
			// Grown too big.  Remove last.
			Chunk_terrain* last = render_queue->render_queue_prev;
			last->free_rendered_flats();
			render_queue->render_queue_prev = last->render_queue_prev;
			last->render_queue_prev->render_queue_next = render_queue;
			last->render_queue_next = last->render_queue_prev = nullptr;
			queue_size--;
		}
		rendered_flats = new Image_buffer8(c_chunksize, c_chunksize);
	}
	// Go through array of tiles.
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			paint_tile(tilex, tiley);
		}
	}
	return rendered_flats;
}

#ifdef M9B
// Tiles land on 8-pixel boundaries (but with smooth scrolling), so most lie
// wholly inside the clip rectangle with their rows word-aligned: those are
// two 32-bit words a row, the rest go through copy8's clipping.
void Chunk_terrain::paint_flats(Image_window8* win, int xoff, int yoff) {
	Image_buffer8* ib = win->get_ib8();
	int            clipx;
	int            clipy;
	int            clipw;
	int            cliph;
	ib->get_clip(clipx, clipy, clipw, cliph);
	unsigned char* bits   = ib->get_bits();
	const int      stride = ib->get_line_width();
	const bool     words  = ((reinterpret_cast<uintptr_t>(bits) | stride) & 3) == 0;
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		const int y = yoff + tiley * c_tilesize;
		if (y + c_tilesize <= clipy || y >= clipy + cliph) {
			continue;
		}
		const bool  rows_in = y >= clipy && y + c_tilesize <= clipy + cliph;
		const Flat* row     = &shapes[16 * tiley];
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			const int x = xoff + tilex * c_tilesize;
			if (x + c_tilesize <= clipx || x >= clipx + clipw) {
				continue;
			}
			Shape_frame* f = sman->get_flat(row[tilex].shape, row[tilex].frame);
			if (!f) {
				continue;
			}
			const unsigned char* src = f->get_data();
			if (words && rows_in && x >= clipx && x + c_tilesize <= clipx + clipw
				&& ((x | reinterpret_cast<uintptr_t>(src)) & 3) == 0) {
				auto*       from = reinterpret_cast<const uint32*>(src);
				auto*       to   = reinterpret_cast<uint32*>(bits + y * stride + x);
				for (int i = 0; i < c_tilesize; i++) {
					to[0] = from[0];
					to[1] = from[1];
					from += c_tilesize / 4;
					to += stride / 4;
				}
			} else {
				win->copy8(src, c_tilesize, c_tilesize, x, y);
			}
		}
	}
}
#endif

/*
 *  Free pre-rendered landscape.
 */

void Chunk_terrain::free_rendered_flats() {
	delete rendered_flats;
	rendered_flats = nullptr;
}

/*
 *  This method is only used in 'terrain-editor' mode, NOT in normal
 *  gameplay.
 */

void Chunk_terrain::render_all(
		int cx, int cy, int pass    // Chunk rendering too.
) {
	Image_window8* iwin     = gwin->get_win();
	const int      ctx      = cx * c_tiles_per_chunk;
	const int      cty      = cy * c_tiles_per_chunk;
	const int      scrolltx = gwin->get_scrolltx();
	const int      scrollty = gwin->get_scrollty();
	// Go through array of tiles.
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			Shape_frame* shape = get_shape(tilex, tiley);
			if (!shape) {
				continue;
			}
			if (!shape->is_rle() && pass == 1) {
				iwin->copy8(
						shape->get_data(), c_tilesize, c_tilesize,
						(ctx + tilex - scrolltx) * c_tilesize,
						(cty + tiley - scrollty) * c_tilesize);
			} else if (shape->is_rle() && pass == 2) {    // RLE.
				int              x;
				int              y;
				const Tile_coord tile(ctx + tilex, cty + tiley, 0);
				gwin->get_shape_location(tile, x, y);
				sman->paint_shape(x, y, shape);
			}
		}
	}
}

/*
 *  Write out to a chunk.
 *
 *  Output: Length of data stored.
 */

int Chunk_terrain::write_flats(
		unsigned char* chunk_data,
		bool           v2_chunks    // 3 bytes/entry.
) {
	unsigned char* start = chunk_data;
	for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
		for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
			const ShapeID id       = get_flat(tx, ty);
			const int     shapenum = id.get_shapenum();
			const int     framenum = id.get_framenum();
			if (v2_chunks) {
				Write1(chunk_data, shapenum & 0xff);
				Write1(chunk_data, (shapenum >> 8) & 0xff);
				Write1(chunk_data, framenum);
			} else {
				Write1(chunk_data, shapenum & 0xff);
				Write1(chunk_data, ((shapenum >> 8) & 3) | (framenum << 2));
			}
		}
	}
	return chunk_data - start;
}
