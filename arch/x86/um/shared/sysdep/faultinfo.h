/*
 * arch/x86/um/shared/sysdep/faultinfo.h
 *
 * Copyright (c) 2016 Allwinnertech Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */
#ifdef __i386__
#include "faultinfo_32.h"
#else
#include "faultinfo_64.h"
#endif
