/*  src/ygl.h (Vita stub)
    Copyright 2005 Guillaume Duhamel
    Copyright 2005-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA

    ---
    VITA PORT NOTE: the real ygl.h wraps its ENTIRE contents in
    "#ifdef HAVE_LIBGL". We never define HAVE_LIBGL (no OpenGL backend
    in this milestone), so the real file would preprocess down to
    nothing anyway. This stub is behaviorally identical for our build,
    without needing GLUT/SDL headers that don't exist on Vita.
*/

#ifndef YGL_H
#define YGL_H

/* intentionally empty: see note above */

#endif
