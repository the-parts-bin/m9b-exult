/*
 *  ucfunction.cc - Usecode function
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

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "ucfunction.h"

#include "endianio.h"

#include <iostream>

using std::istream;

/*
 *  Read in a function.
 */

Usecode_function::Usecode_function(istream& file) : orig(nullptr) {
	id = little_endian::Read2(file);

	// support for our extended usecode format. (32 bit lengths and ids)
	if (id == 0xfffe) {
		id       = little_endian::Read4s(file);
		len      = little_endian::Read4(file);
		extended = true;
		// older extended usecode format. (32 bit lengths)
	} else if (id == 0xffff) {
		id       = little_endian::Read2(file);
		len      = little_endian::Read4(file);
		extended = true;
	} else {
		len      = little_endian::Read2(file);
		extended = false;
	}

#ifdef M9B
	src_path = reading_from;
	offset   = static_cast<long>(file.tellg());
	code     = nullptr;
	file.seekg(len, std::ios::cur);
#else
	code = new unsigned char[len];    // Allocate buffer & read it in.
	file.read(reinterpret_cast<char*>(code), len);
#endif
}

#ifdef M9B
#	include "utils.h"

#	include <algorithm>
#	include <cstring>
#	include <memory>
#	include <vector>

const char* Usecode_function::reading_from = nullptr;

namespace {
	constexpr size_t               code_budget = 192 * 1024;
	size_t                         loaded_bytes;
	unsigned                       use_clock;
	std::vector<Usecode_function*> loaded;

	// The usecode files, kept open (static usecode and its patch).
	struct Open_file {
		const char*                   path = nullptr;
		std::unique_ptr<std::istream> in;
	};

	Open_file open_files[2];

	std::istream* open_source(const char* path) {
		for (auto& f : open_files) {
			if (f.in && !strcmp(f.path, path)) {
				return f.in.get();
			}
		}
		Open_file& f = open_files[open_files[0].in ? 1 : 0];
		f.in         = U7open_in(path);
		f.path       = path;
		return f.in.get();
	}
}    // namespace

void Usecode_function::load() {
	last_use = ++use_clock;
	if (code) {
		return;
	}
	// Room first: the least recently used ones no frame is running.
	while (loaded_bytes + len > code_budget) {
		Usecode_function* lru = nullptr;
		for (auto* f : loaded) {
			if (f->frames == 0 && (!lru || f->last_use < lru->last_use)) {
				lru = f;
			}
		}
		if (!lru) {
			break;    // All running: over budget for now.
		}
		lru->unload();
	}
	code             = new unsigned char[len];
	std::istream* in = open_source(src_path);
	if (in) {
		in->clear();
		in->seekg(offset);
		in->read(reinterpret_cast<char*>(code), len);
	}
	if (!in || !*in) {
		std::cerr << "Usecode function " << std::hex << id << std::dec
				  << ": can't read its code" << std::endl;
	}
	loaded_bytes += len;
	loaded.push_back(this);
}

void Usecode_function::unload() {
	if (!code) {
		return;
	}
	delete[] code;
	code = nullptr;
	loaded_bytes -= len;
	loaded.erase(std::find(loaded.begin(), loaded.end(), this));
}

Usecode_function::~Usecode_function() {
	unload();
}
#endif
