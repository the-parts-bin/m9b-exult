/*
 *  Imagewin.cc for the m9b SoC: in place of upstream's imagewin.cc (an SDL
 *  window, renderer and texture, and the scalers between them). Here the
 *  window, the game area and the 8-bit draw surface are one 320 x 200 (or
 *  whatever the configuration asks for) SDL_Surface from the port's SDL
 *  (m9b_sdl.cc); show() hands it and its palette to the display
 *  (m9b_video.cc). Only the Point scaler is left (the Black Gate intro uses
 *  its arbitrary scaler to zoom). The fill modes, sizes and coordinate
 *  conversions are upstream's, unchanged.
 *
 *  Copyright (C) 1998 Jeffrey S. Freedman
 *  Copyright (C) 2000-2022 The Exult Team
 *
 *  This library is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Library General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 */

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "imagewin.h"

#include "Configuration.h"
#include "PointScaler.h"
#include "common_types.h"
#include "istring.h"
#include "manip.h"
#include "mouse.h"

#include <cstdlib>
#include <iostream>
#include <string>

#include <SDL.h>

using std::cerr;
using std::cout;
using std::endl;

extern "C" void m9b_video_show(const unsigned char* pixels, int pitch, int w, int h, const SDL_Color* palette);

SDL_PixelFormat*
		  ManipBase::fmt;    // Format of dest. pixels (and src for rgb src).
SDL_Color ManipBase::colors[256];    // Palette for source window.

const Image_window::ScalerType  Image_window::NoScaler(-1);
const Image_window::ScalerConst Image_window::point("Point");
const Image_window::ScalerConst Image_window::interlaced("Interlaced");
const Image_window::ScalerConst Image_window::bilinear("Bilinear");
const Image_window::ScalerConst Image_window::BilinearPlus("BilinearPlus");
const Image_window::ScalerConst Image_window::SaI("2xSaI");
const Image_window::ScalerConst Image_window::SuperEagle("SuperEagle");
const Image_window::ScalerConst Image_window::Super2xSaI("Super2xSaI");
const Image_window::ScalerConst Image_window::Scale2x("Scale2x");
const Image_window::ScalerConst Image_window::Hq2x("Hq2X");
const Image_window::ScalerConst Image_window::Hq3x("Hq3x");
const Image_window::ScalerConst Image_window::Hq4x("Hq4x");
const Image_window::ScalerConst Image_window::_2xBR("2xBR");
const Image_window::ScalerConst Image_window::_3xBR("3xBR");
const Image_window::ScalerConst Image_window::_4xBR("4xBR");
const Image_window::ScalerConst Image_window::NumScalers(nullptr);

Image_window::ScalerVector        Image_window::p_scalers;
const Image_window::ScalerVector& Image_window::Scalers
		= Image_window::p_scalers;

std::map<uint32, Image_window::Resolution>        Image_window::p_resolutions;
const std::map<uint32, Image_window::Resolution>& Image_window::Resolutions
		= Image_window::p_resolutions;
bool        Image_window::any_res_allowed;
const bool& Image_window::AnyResAllowed = Image_window::any_res_allowed;

int Image_window::force_bpp     = 0;
int Image_window::desktop_depth = 0;
int Image_window::windowed      = 0;
// When HighDPI is enabled we will end up with a different native scale factor,
// so we need to define the default
float Image_window::nativescale = 1.0f;

// Only Point (upstream's list less the scalers that aren't built).
Image_window::ScalerVector::ScalerVector() {
	const ScalerInfo point = {"Point", 0xFFFFFFFF, new Pentagram::PointScaler(),
							  nullptr, nullptr,    nullptr,
							  nullptr, nullptr};
	push_back(point);
}

Image_window::ScalerVector::~ScalerVector() {
	for (auto& it : *this) {
		delete it.arb;
	}
}

Image_window::ScalerType Image_window::get_scaler_for_name(const char* scaler) {
	for (int s = 0; s < NumScalers; s++) {
		if (!Pentagram::strcasecmp(scaler, Scalers[s].name)) {
			return s;
		}
	}

	return NoScaler;
}

int Image_window::Get_best_bpp(int w, int h, int bpp, uint32 flags) {
	ignore_unused_variable_warning(w, h, bpp, flags);
	return 8;
}

void Image_window::static_init() {
	static bool done = false;
	if (done) {
		return;
	}
	done                 = true;
	const Resolution res = {320, 200};
	p_resolutions[(res.width << 16) | res.height] = res;
	windowed                                      = 8;
	desktop_depth                                 = 8;
}

Image_window::~Image_window() {
	free_surface();
	delete ibuf;
}

// The window is the draw surface (8 bits, the display's size), no scaling.
void Image_window::create_surface(unsigned int w, unsigned int h) {
	uses_palette = true;
	scale        = 1;
	scaler       = point;
	fill_scaler  = point;
	get_draw_dims(
			w, h, scale, fill_mode, game_width, game_height, inter_width,
			inter_height);
	saved_game_width  = game_width;
	saved_game_height = game_height;
	draw_surface      = SDL_CreateRGBSurface(0, w, h, 8, 0, 0, 0, 0);
	if (!draw_surface) {
		cerr << "Couldn't create the " << w << "x" << h << " surface" << endl;
		exit(-1);
	}
	paletted_surface = inter_surface = display_surface = draw_surface;
	inter_width                                      = w;
	inter_height                                     = h;

	ibuf->width      = draw_surface->w;
	ibuf->height     = draw_surface->h;
	ibuf->line_width = draw_surface->pitch / ibuf->pixel_size;
	ibuf->offset_x   = (get_full_width() - get_game_width()) / 2;
	ibuf->offset_y   = (get_full_height() - get_game_height()) / 2;
	ibuf->bits       = static_cast<unsigned char*>(draw_surface->pixels)
				 - get_start_x() - get_start_y() * ibuf->line_width;
}

bool Image_window::create_scale_surfaces(int w, int h, int bpp) {
	ignore_unused_variable_warning(w, h, bpp);
	return false;
}

bool Image_window::try_scaler(int w, int h) {
	ignore_unused_variable_warning(w, h);
	return false;
}

void Image_window::free_surface() {
	if (draw_surface) {
		SDL_FreeSurface(draw_surface);
	}
	paletted_surface = inter_surface = draw_surface = display_surface = nullptr;
	ibuf->bits                                     = nullptr;
}

std::unique_ptr<Image_buffer> Image_window::create_buffer(int w, int h) {
	return ibuf->create_another(w, h);
}

void Image_window::resized(
		unsigned int neww, unsigned int newh, bool newfs, unsigned int newgw,
		unsigned int newgh, int newsc, int newscaler, FillMode fmode,
		int fillsclr) {
	ignore_unused_variable_warning(newsc, newscaler, fillsclr);
	if (paletted_surface) {
		free_surface();
	}
	fullscreen  = newfs;
	game_width  = newgw;
	game_height = newgh;
	fill_mode   = fmode;
	create_surface(neww, newh);
}

// The whole surface to the display (it takes the frame at once: a copy).
void Image_window::show(int x, int y, int w, int h) {
	ignore_unused_variable_warning(x, y, w, h);
	if (!ready()) {
		return;
	}
	m9b_video_show(
			static_cast<const unsigned char*>(draw_surface->pixels),
			draw_surface->pitch, draw_surface->w, draw_surface->h,
			draw_surface->format->palette->colors);
}

void Image_window::toggle_fullscreen() {}

void Image_window::BeginPaintIntoGuardBand(int* x, int* y, int* w, int* h) {
	ignore_unused_variable_warning(x, y, w, h);
}

void Image_window::EndPaintIntoGuardBand() {}

void Image_window::FillGuardband() {}

bool Image_window::screenshot(SDL_RWops* dst) {
	ignore_unused_variable_warning(dst);
	return false;
}

void Image_window::set_title(const char* title) {
	ignore_unused_variable_warning(title);
}

int Image_window::get_display_width() {
	return display_surface->w;
}

int Image_window::get_display_height() {
	return display_surface->h;
}

void Image_window::screen_to_game(int sx, int sy, bool fast, int& gx, int& gy) {
	if (fast) {
		gx = sx + get_start_x();
		gy = sy + get_start_y();
		if (Mouse::mouse) {
			Mouse::mouse->apply_fast_offset(gx, gy);
		}
	} else {
		gx = (sx * inter_width) / (scale * get_display_width()) + get_start_x();
		gy = (sy * inter_height) / (scale * get_display_height())
			 + get_start_y();
	}
}

void Image_window::game_to_screen(int gx, int gy, bool fast, int& sx, int& sy) {
	if (fast) {
		if (Mouse::mouse) {
			Mouse::mouse->unapply_fast_offset(gx, gy);
		}
		sx = gx - get_start_x();
		sy = gy - get_start_y();
	} else {
		sx = ((gx - get_start_x()) * scale * get_display_width()) / inter_width;
		sy = ((gy - get_start_y()) * scale * get_display_height())
			 / inter_height;
	}
}

bool Image_window::get_draw_dims(
		int sw, int sh, int scale, FillMode fillmode, int& gw, int& gh, int& iw,
		int& ih) {
	// Handle each type separately

	if (fillmode == Fill) {
		if (gw == 0 || gh == 0) {
			gw = sw / scale;
			gh = sh / scale;
		}

		iw = gw * scale;

		ih = gh * scale;
	} else if (fillmode == Fit) {
		if (gw == 0 || gh == 0) {
			gw = sw / scale;
			gh = sh / scale;
		}

		// Height determines the scaling factor
		if (sw * gh >= sh * gw) {
			ih = gh * scale;

			iw = (sw * ih) / (sh);
		}
		// Width determines the scaling factor
		else {
			iw = gw * scale;

			ih = (sh * iw) / (sw);
		}
	} else if (fillmode == AspectCorrectFit) {
		if (gw == 0 || gh == 0) {
			gw = sw / scale;
			gh = (sh * 5) / (scale * 6);
		}

		// Height determines the scaling factor
		if (sw * gh * 6 >= sh * gw * 5) {
			if (gh == (sh * 5) / (scale * 6)) {
				ih = (sh * 5) / 6;
			} else {
				ih = gh * scale;
			}

			iw = (sw * ih * 6) / (sh * 5);
		}
		// Width determines the scaling factor
		else {
			if (gw == sw / scale) {
				iw = sw;
			} else {
				iw = gw * scale;
			}

			ih = (sh * iw * 5) / (sw * 6);
		}
	} else if (fillmode >= Centre && fillmode < (1 << 16)) {
		const int factor        = 2 + ((fillmode - Centre) / 2);
		const int aspect_factor = (fillmode & 1) ? 5 : 6;

		if (gw == 0 || gh == 0) {
			gw = (sw * 2) / (factor * scale);
			gh = (sh * aspect_factor) / (3 * factor * scale);
		}

		iw = (sw * 2) / factor;
		ih = (sh * aspect_factor) / (3 * factor);

		if (gw * scale > iw) {
			gw = iw / scale;
		}

		if (gh * scale > ih) {
			gh = ih / scale;
		}
	} else {
		const int fw = fillmode & 0xFFFF;
		const int fh = (fillmode >> 16) & 0xFFFF;

		if (!fw || !fh) {
			return false;
		}

		if (gw == 0 || gh == 0) {
			gw = fw / scale;
			gh = fh / scale;
		}

		if (fw > sw) {
			gw = (gw * sw) / fw;
			iw = gw * scale;
		} else {
			iw = (sw * gw * scale) / fw;
		}

		if (fh > sh) {
			gh = (gh * sh) / fh;
			ih = gh * scale;
		} else {
			ih = (sh * gh * scale) / fh;
		}
	}

	// If there is a rounding error don't scale twice, just centre
	if (iw == (sw / scale) * scale) {
		iw = sw;
	}
	if (ih == (sh / scale) * scale) {
		ih = sh;
	}

	return true;
}

Image_window::FillMode Image_window::string_to_fillmode(const char* str) {
	// If only C++ had reflection capabilities...

	if (!Pentagram::strcasecmp(str, "Fill")) {
		return Fill;
	} else if (!Pentagram::strcasecmp(str, "Fit")) {
		return Fit;
	} else if (!Pentagram::strcasecmp(str, "Aspect Correct Fit")) {
		return AspectCorrectFit;
	} else if (
			!Pentagram::strcasecmp(str, "Centre")
			|| !Pentagram::strcasecmp(str, "Center")) {
		return Centre;
	} else if (
			!Pentagram::strcasecmp(str, "Aspect Correct Centre")
			|| !Pentagram::strcasecmp(str, "Aspect Correct Center")
			|| !Pentagram::strcasecmp(str, "Centre Aspect Correct")
			|| !Pentagram::strcasecmp(str, "Center Aspect Correct")) {
		return AspectCorrectCentre;
	} else if (
			!Pentagram::strncasecmp(str, "Centre ", 7)
			|| !Pentagram::strncasecmp(str, "Center ", 7)) {
		str += 7;
		if (*str != 'X' && *str != 'x') {
			return static_cast<FillMode>(0);
		}

		++str;
		if (!std::isdigit(static_cast<unsigned char>(*str))) {
			return static_cast<FillMode>(0);
		}

		char*               end;
		const unsigned long f = std::strtoul(str, &end, 10);
		str += (end - str);

		if (f >= (65536 - Centre) / 2 || *str) {
			return static_cast<FillMode>(0);
		}

		return static_cast<FillMode>(Centre + f * 2);
	} else if (
			!Pentagram::strncasecmp(str, "Aspect Correct Centre ", 22)
			|| !Pentagram::strncasecmp(str, "Aspect Correct Center ", 22)
			|| !Pentagram::strncasecmp(str, "Centre Aspect Correct ", 22)
			|| !Pentagram::strncasecmp(str, "Center Aspect Correct ", 22)) {
		str += 22;
		if (*str != 'X' && *str != 'x') {
			return static_cast<FillMode>(0);
		}

		++str;
		if (!std::isdigit(static_cast<unsigned char>(*str))) {
			return static_cast<FillMode>(0);
		}

		char*               end;
		const unsigned long f = std::strtoul(str, &end, 10);
		str += (end - str);

		if (f >= (65536 - AspectCorrectCentre) / 2 || *str) {
			return static_cast<FillMode>(0);
		}

		return static_cast<FillMode>(AspectCorrectCentre + f * 2);
	} else {
		if (!std::isdigit(static_cast<unsigned char>(*str))) {
			return static_cast<FillMode>(0);
		}

		char*               end;
		const unsigned long fx = std::strtoul(str, &end, 10);
		str += (end - str);

		if (fx > 65535 || (*str != 'X' && *str != 'x')) {
			return static_cast<FillMode>(0);
		}

		++str;
		if (!std::isdigit(static_cast<unsigned char>(*str))) {
			return static_cast<FillMode>(0);
		}

		const unsigned long fy = std::strtoul(str, &end, 10);
		str += (end - str);

		if (fy > 65535 || *str) {
			return static_cast<FillMode>(0);
		}

		return static_cast<FillMode>(fx | (fy << 16));
	}
}

bool Image_window::fillmode_to_string(FillMode fmode, std::string& str) {
	// If only C++ had reflection capabilities...

	if (fmode == Fill) {
		str = "Fill";
		return true;
	} else if (fmode == Fit) {
		str = "Fit";
		return true;
	} else if (fmode == AspectCorrectFit) {
		str = "Aspect Correct Fit";
		return true;
	} else if (fmode >= Centre && fmode < (1 << 16)) {
		const int factor = 2 + ((fmode - Centre) / 2);
		char      factor_str[16];

		if (factor == 2) {
			factor_str[0] = 0;
		} else {
			snprintf(
					factor_str, sizeof(factor_str),
					(factor & 1) ? " x%d.5" : " x%d", factor / 2);
		}

		if (fmode & 1) {
			str = std::string("Aspect Correct Centre")
				  + std::string(factor_str);
		} else {
			str = std::string("Centre") + std::string(factor_str);
		}
		return true;
	} else {
		const int fw = fmode & 0xFFFF;
		const int fh = (fmode >> 16) & 0xFFFF;

		if (!fw || !fh) {
			return false;
		}

		char factor_str[16];
		snprintf(factor_str, sizeof(factor_str), "%dx%d", fw, fh);
		str = std::string(factor_str);

		return true;
	}

	return false;
}

void Image_window::UpdateRect(SDL_Surface* surf, int x, int y, int w, int h) {
	ignore_unused_variable_warning(surf, x, y, w, h);
}

int Image_window::VideoModeOK(int width, int height) {
	return width == 320 && height == 200 ? 8 : 0;
}

SDL_DisplayMode Image_window::desktop_displaymode;
