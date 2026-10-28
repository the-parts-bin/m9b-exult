/*
 *  ucfunction.h - Usecode function
 *
 *  Copyright (C) 2002-2022  The Exult Team
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef UCFUNCTION_H
#define UCFUNCTION_H

#include "useval.h"

#include <iosfwd>
#include <vector>

class Usecode_function {
public:
	int id;    // The function #.  (Appears to be the
	//   game item # the function gets
	//   called for.)
	Usecode_function* orig;    // If this was from 'patch', orig will
	//   be the function this replaced (if
	//   it existed).
	int len;    // Length.

	bool extended;    // is this an 'extented' function? (aka 32 bit function)
	unsigned char*             code;       // The code.
	std::vector<Usecode_value> statics;    // Local statics.
	// Create from file.
	Usecode_function(std::istream& file);

#ifdef M9B
	// M9B: the code stays in the file (1.5 MB of it in all) until a stack
	// frame needs it (load()), and is dropped again, least recently used
	// first and never while a frame runs it, to keep what is loaded under a
	// budget (ucfunction.cc). read_usecode's callers say which file it is.
	static const char* reading_from;
	const char*        src_path;        // The file (a usecode path macro).
	long               offset;          // Of the code in it.
	int                frames   = 0;    // Stack frames running it.
	unsigned           last_use = 0;
	void               load();
	void               unload();
	~Usecode_function();
#else
	inline ~Usecode_function() {
		delete[] code;
	}
#endif
};

#endif
