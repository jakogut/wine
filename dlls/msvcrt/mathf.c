/*
 * msvcrt float functions
 *
 * Copyright 2019 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* this function is part of the import lib to provide floating */
#if 0
#pragma makedep implib
#endif

#include <math.h>

#if defined(__i386__) || (_MSVCR_VER > 0 && _MSVCR_VER < 80)
float sinf(float x) { return sin(x); }
float cosf(float x) { return cos(x); }
float tanf(float x) { return tan(x); }
float atan2f(float x, float y) { return atan2(x, y); }
float expf(float x) { return exp(x); }
float logf(float x) { return log(x); }
float powf(float x, float y) { return pow(x, y); }
float sqrtf(float x) { return sqrt(x); }
float floorf(float x) { return floor(x); }
float ceilf(float x) { return ceil(x); }
#endif
