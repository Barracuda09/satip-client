/*
 * satip: logging
 *
 * Copyright (C) 2014 mc.fishdish@gmail.com
 * [copy of vtuner by Honza Petrous]
 * Copyright (C) 2010-11 Honza Petrous <jpetrous@smartimp.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __LOG_H__
#define __LOG_H__

#include <string>

#include <sys/types.h>
#include <unistd.h>

extern int dbg_level;
extern unsigned int dbg_mask; // MSG_DATA | MSG_MAIN | MSG_NET | MSG_HW | MSG_SRV
extern int use_syslog;


#define MSG_MAIN   1
#define MSG_NET    2
#define MSG_HW     4
#define MSG_SRV    8
#define MSG_DATA  16
#define MSG_ALL   (MSG_DATA | MSG_MAIN | MSG_NET | MSG_HW | MSG_SRV )

#define MSG_ERROR  1
#define MSG_WARN   2
#define MSG_INFO   3
#define MSG_DEBUG  4

#define ERROR(mtype, msg, ...) write_message(mtype, MSG_ERROR, "[%d %-15s:%4u][%-27s]\t error: " msg, getpid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define  WARN(mtype, msg, ...) write_message(mtype, MSG_WARN,  "[%d %-15s:%4u][%-27s]\t  warn: " msg, getpid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define  INFO(mtype, msg, ...) write_message(mtype, MSG_INFO,  "[%d %-15s:%4u][%-27s]\t  info: " msg, getpid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define DEBUG(mtype, msg, ...) write_message(mtype, MSG_DEBUG, "[%d %-15s:%4u][%-27s]\t debug: " msg, getpid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)

void write_message(const unsigned int, const int, const char*, ...);

std::string convertToHexASCIITable(
		const unsigned char* p,
		const std::size_t length,
		const std::size_t blockSize);

#endif // __LOG_H__
