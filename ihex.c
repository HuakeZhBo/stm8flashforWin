/***************************************************************************
 *   Intel hex read and write utility functions                            *
 *                                                                         *
 *   Copyright (c) Valentin Dudouyt, 2004 2012 - 2014                      *
 *   Copyright (c) Philipp Klaus Krause, 2021                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ihex.h"

#define HI(x) (((x) & 0xff00) >> 8)
#define LO(x) ((x) & 0xff)

static unsigned char checksum(unsigned char *buf, unsigned int length, int chunk_len, int chunk_addr, int chunk_type) {
	int sum = chunk_len + (LO(chunk_addr)) + (HI(chunk_addr)) + chunk_type;
	int i;
	for(i = 0; i < length; i++) {
		sum += buf[i];
	}

	int complement = (~sum + 1);
	return(complement & 0xff);
}

int ihex_read(FILE *pFile, unsigned char *buf, unsigned int start, unsigned int end) {
	// a record in intel hex looks like
	// :LLXXXXTTDDDD....DDCC
	// Where LL is the length of the data field and can be as large as 255.  So the maximum size of a record is 9 characters for the 
	// header, 255 * 2 characters for the data, 2 characters for the Checksum, and an additional 2 characters (possibly a Carriage 
	// Return and linefeed.  This is then a total an 9 + 510 + 2 + 2 = 523.
	static char line[523];

	fseek(pFile, 0, SEEK_SET);

	unsigned int chunk_len, chunk_addr, chunk_type, i, byte, line_no = 0, greatest_addr = 0, offset = 0;

	while(fgets(line, sizeof(line), pFile)) {
		line_no++;

		// Strip off Carriage Return at end of line if it exists.
		if (line[strlen(line)] == '\r')
		{
			line[strlen(line)] = 0;
		}

		// Reading chunk header
		if(sscanf(line, ":%02x%04x%02x", &chunk_len, &chunk_addr, &chunk_type) != 3) {
			free(buf);
			fprintf(stderr, "Error while parsing IHEX at line %d\n", line_no);
			return(-1);
		}
		// Reading chunk data
		if(chunk_type == 2) // Extended Segment Address
		{
			unsigned int esa;
			if(sscanf(&line[9],"%04x",&esa) != 1) {
				free(buf);
				fprintf(stderr, "Error while parsing IHEX at line %d\n", line_no);
				return(-1);
			}
			offset = esa * 16;
		}
		if(chunk_type == 4) // Extended Linear Address
		{
			unsigned int ela;
			if(sscanf(&line[9],"%04x",&ela) != 1) {
				free(buf);
				fprintf(stderr, "Error while parsing IHEX at line %d\n", line_no);
				return(-1);
			}
			offset = ela * 65536;
		}
		
		for(i = 9; i < strlen(line) - 1; i +=2) {
			if(sscanf(&(line[i]), "%02x", &byte) != 1) {
				free(buf);
				fprintf(stderr, "Error while parsing IHEX at line %d byte %d\n", line_no, i);
				return(-1);
			}
			if(chunk_type != 0x00) {
				// The only data records have to be processed
                break;
			}
			if((i - 9) / 2 >= chunk_len) {
				// Respect chunk_len and do not capture checksum as data
				break;
			}
			if((chunk_addr + offset) < start) {
				free(buf);
				fprintf(stderr, "Address %04x is out of range at line %d\n", chunk_addr, line_no);
				return(-1);
			}
			if(chunk_addr + offset + chunk_len > end) {
				free(buf);
				fprintf(stderr, "Address %04x + %d is out of range at line %d\n", chunk_addr, chunk_len, line_no);
				return(-1);
			}
			if(chunk_addr + offset + chunk_len > greatest_addr) {
				greatest_addr = chunk_addr + offset + chunk_len;
			}
			buf[chunk_addr + offset - start + (i - 9) / 2] = byte;
		}
	}

	return(greatest_addr - start);
}

int ihex_write(FILE *pFile, unsigned char *buf, unsigned int start, unsigned int end) {
	unsigned int chunk_len, chunk_start, i;
	int cur_ela = 0; 
	// If any address is above the first 64k, cause an initial ELA record to be written
	if (end > 65535)
	{
		cur_ela = -1;
	}
	chunk_start = start;
	while(chunk_start < end)
	{
		// Assume the rest of the bytes will be written in this chunk
		chunk_len = end - chunk_start;
		// Limit the length to 32 bytes per chunk
		if (chunk_len > 32)
		{
			chunk_len = 32;
		}
		// Check if length would go past the end of the current 64k block
		if (((chunk_start & 0xffff) + chunk_len) > 0xffff)
		{
			// If so, reduce the length to go only to the end of the block
			chunk_len = 0x10000 - (chunk_start & 0xffff);
		}

		// See if the last ELA record that was written matches the current block
		if (cur_ela != (chunk_start >> 16))
		{
			// If not, write an ELA record
			unsigned char ela_bytes[2];
			cur_ela = chunk_start >> 16;
			ela_bytes[0] = HI(cur_ela);
			ela_bytes[1] = LO(cur_ela);
			if(fprintf(pFile, ":02000004%04X%02X\n",cur_ela,checksum(ela_bytes,2,2,0,4)) < 0)
			{
				fprintf(stderr, "I/O error during IHEX write\n");
				return(-1);
			}
		}
		// Write the data record
		fprintf(pFile, ":%02X%04X00",chunk_len,chunk_start & 0xffff);
		for(i = chunk_start - start; i < (chunk_start + chunk_len - start); i++)
			if(fprintf(pFile, "%02X",buf[i]) < 0)
			{
				fprintf(stderr, "I/O error during IHEX write\n");
				return(-1);
			}
		if(fprintf(pFile, "%02X\n", checksum( &buf[chunk_start - start], chunk_len, chunk_len, chunk_start, 0)) < 0)
		{
			fprintf(stderr, "I/O error during IHEX write\n");
			return(-1);
		}

		
		chunk_start += chunk_len;
	}
	// Add the END record
	if(fprintf(pFile,":00000001FF\n") < 0)
	{
		fprintf(stderr, "I/O error during IHEX write\n");
		return(-1);
	}
	
	return(0);
}

