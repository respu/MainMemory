/*
 * arch/generic/spin.h - MainMemory spinning pause.
 *
 * Copyright (C) 2013-2014  Aleksey Demakov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ARCH_GENERIC_SPIN_H
#define ARCH_GENERIC_SPIN_H

static inline void
mm_spin_pause(void)
{
	for (int i = 0; i < 64; i++)
		mm_compiler_barrier();
}

#endif /* ARCH_GENERIC_SPIN_H */
