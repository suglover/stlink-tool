/*
  Copyright (c) 2018 Jean THOMAS.
  
  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the Software
  is furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
  OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <libusb.h>

#include "crypto.h"
#include "stlink.h"

#define EP_IN 1 | LIBUSB_ENDPOINT_IN
#define EP_OUT 1 | LIBUSB_ENDPOINT_OUT

#define USB_TIMEOUT 5000

#define DFU_DETACH 0x00
#define DFU_DNLOAD 0x01
#define DFU_UPLOAD 0x02
#define DFU_GETSTATUS 0x03
#define DFU_CLRSTATUS 0x04
#define DFU_GETSTATE 0x05
#define DFU_ABORT 0x06
#define DFU_EXIT  0x07

#define GET_COMMAND 0x00
#define SET_ADDRESS_POINTER_COMMAND 0x21
#define ERASE_PAGES_COMMAND 0x41
#define ERASE_SECTORS_COMMAND 0x42
#define READ_UNPROTECT_COMMAND 0x92

int stlink_read_infos(libusb_device_handle *dev_handle,
		      struct STLinkInfos *infos) {
  unsigned char data[20];
  int res, rw_bytes;

  memset(data, 0, sizeof(data));

  data[0] = 0xF1;
  data[1] = 0x80;

  /* Write */
  res = libusb_bulk_transfer(dev_handle,
			     EP_OUT,
			     data,
			     16,
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  /* Read */
  res = libusb_bulk_transfer(dev_handle,
		       EP_IN,
		       data,
		       6,
		       &rw_bytes,
		       USB_TIMEOUT);
  if (res) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  infos->stlink_version = data[0] >> 4;

  if (infos->stlink_version < 3) {
    infos->jtag_version = (data[0] & 0x0F) << 2 | (data[1] & 0xC0) >> 6;
    infos->swim_version = data[1] & 0x3F;
    infos->loader_version = data[5] << 8 | data[4];
    infos->product_id = 0;
  } else {
    infos->product_id = data[3] << 8 | data[2];

    memset(data, 0, sizeof(data));

    data[0] = 0xFB;
    data[1] = 0x80;

    /* Write */
    res = libusb_bulk_transfer(dev_handle,
            EP_OUT,
            data,
            16,
            &rw_bytes,
            USB_TIMEOUT);
    if (res) {
      fprintf(stderr, "USB transfer failure\n");
      return -1;
    }

    /* Read */
    res = libusb_bulk_transfer(dev_handle,
            EP_IN,
            data,
            12,
            &rw_bytes,
            USB_TIMEOUT);
    if (res) {
      fprintf(stderr, "USB transfer failure\n");
      return -1;
    }

    infos->jtag_version = data[2];
    infos->swim_version = data[1];
    infos->loader_version = data[11] << 8 | data[10];
  }

  memset(data, 0, sizeof(data));

  data[0] = 0xF3;
  data[1] = 0x08;

  /* Write */
  res = libusb_bulk_transfer(dev_handle,
			     EP_OUT,
			     data,
			     16,
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  /* Read */
  libusb_bulk_transfer(dev_handle,
		       EP_IN,
		       data,
		       20,
		       &rw_bytes,
		       USB_TIMEOUT);
  if (res) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  memcpy(infos->id, data+8, 12);

  /* Firmware encryption key generation */
  memcpy(infos->firmware_key, data, 4);
  memcpy(infos->firmware_key+4, data+8, 12);

  if (infos->stlink_version < 3) {
    my_encrypt((unsigned char*)"I am key, wawawa", infos->firmware_key, 16);
  } else {
    my_encrypt((unsigned char*)" found...STlink ", infos->firmware_key, 16);
  }

  return 0;
}

int stlink_current_mode(libusb_device_handle *dev_handle) {
  unsigned char data[16];
  int rw_bytes, res;
  
  memset(data, 0, sizeof(data));

  data[0] = 0xF5;

  /* Write */
  res = libusb_bulk_transfer(dev_handle,
			     EP_OUT,
			     data,
			     sizeof(data),
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  /* Read */
  libusb_bulk_transfer(dev_handle,
		       EP_IN,
		       data,
		       2,
		       &rw_bytes,
		       USB_TIMEOUT);
  if (res) {
    fprintf(stderr, "stlink_read_infos() failure\n");
    return -1;
  }

  return data[0] << 8 | data[1];
}

uint16_t stlink_checksum(const unsigned char *firmware,
			 size_t len) {
  unsigned int i;
  int ret = 0;

  for (i = 0; i < len; i++) {
    ret += firmware[i];
  }

  return (uint16_t)ret & 0xFFFF;
}

int stlink_dfu_download(libusb_device_handle *dev_handle,
			unsigned char *data,
			size_t data_len,
			uint16_t wBlockNum,
			struct STLinkInfos *stlink_infos) {
  unsigned char download_request[16];
  struct DFUStatus dfu_status;
  int rw_bytes, res;

  memset(download_request, 0, sizeof(download_request));

  if (wBlockNum >= 2 && stlink_infos->stlink_version == 3) {
    my_encrypt((uint8_t*)" .ST-Link.ver.3.", data, data_len);
  }

  download_request[0] = 0xF3;
  download_request[1] = DFU_DNLOAD;
  *(uint16_t*)(download_request+2) = wBlockNum; /* wValue */
  *(uint16_t*)(download_request+4) = stlink_checksum(data, data_len); /* wIndex */
  *(uint16_t*)(download_request+6) = data_len; /* wLength */

  if (wBlockNum >= 2) {
    my_encrypt(stlink_infos->firmware_key, data, data_len);
  }

  res = libusb_bulk_transfer(dev_handle,
			       EP_OUT,
			       download_request,
			       sizeof(download_request),
			       &rw_bytes,
			       USB_TIMEOUT);
  if (res || rw_bytes != sizeof(download_request)) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }
  res = libusb_bulk_transfer(dev_handle,
			     EP_OUT,
			     data,
			     data_len,
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res || rw_bytes != (int)data_len) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  if (stlink_dfu_status(dev_handle, &dfu_status)) {
    return -1;
  }

  if (dfu_status.bState != dfuDNBUSY) {
    fprintf(stderr, "Unexpected DFU state : %d\n", dfu_status.bState);
    return -2;
  }

  if (dfu_status.bStatus != OK) {
    fprintf(stderr, "Unexpected DFU status : %d\n", dfu_status.bStatus);
    return -3;
  }

  usleep(dfu_status.bwPollTimeout * 1000);

  if (stlink_dfu_status(dev_handle, &dfu_status)) {
    return -1;
  }

  if (dfu_status.bState != dfuDNLOAD_IDLE) {
    if (dfu_status.bStatus == errVENDOR) {
      fprintf(stderr, "Read-only protection active\n");
      return -3;
    } else if (dfu_status.bStatus == errTARGET) {
      fprintf(stderr, "Invalid address error\n");
      return -3;
    } else {
      fprintf(stderr, "Unknown error : %d\n", dfu_status.bStatus);
      return -3;
    }
  }

  return 0;
}

int stlink_dfu_status(libusb_device_handle *dev_handle,
		       struct DFUStatus *status) {
  unsigned char data[16];
  int rw_bytes, res;

  memset(data, 0, sizeof(data));

  data[0] = 0xF3;
  data[1] = DFU_GETSTATUS;
  data[6] = 0x06; /* wLength */

  res = libusb_bulk_transfer(dev_handle,
			     EP_OUT,
			     data,
			     16,
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res || rw_bytes != 16) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }
  res = libusb_bulk_transfer(dev_handle,
			     EP_IN,
			     data,
			     6,
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res || rw_bytes != 6) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }

  status->bStatus = data[0];
  status->bwPollTimeout = data[1] | data[2] << 8 | data[3] << 16;
  status->bState = data[4];
  status->iString = data[5];
  
  return 0;
}

int stlink_erase_pages(libusb_device_handle *dev_handle, uint32_t address) {
  unsigned char command[5];
  int res;

  command[0] = ERASE_PAGES_COMMAND;
  command[1] = address & 0xFF;
  command[2] = (address >> 8) & 0xFF;
  command[3] = (address >> 16) & 0xFF;
  command[4] = (address >> 24) & 0xFF;

  res = stlink_dfu_download(dev_handle, command, sizeof(command), 0, NULL);

  return res;
}

int address2sector(struct STLinkInfos *stlink_infos, uint32_t address) {
  uint32_t sector_size, offset;

  sector_size = (stlink_infos->product_id != 0x449) ? 0x4000 : 0x8000;
  if (address < 0x08000000 || address >= (0x08000000 + 32*sector_size)) {
    fprintf(stderr, "Invalid sector address\n");
    return -1;
  }

  offset = address - 0x08000000;
  if (offset < 4 * sector_size) {
    // sectors 0..3
    return offset / sector_size;

  } else if (offset < 8 * sector_size) {
    // sector 4
    return 4;

  } else {
    // sectors 5+
    return (offset / (8 * sector_size)) + 4;
  }
}

int stlink_erase_sectors(libusb_device_handle *dev_handle, struct STLinkInfos *stlink_infos, uint32_t address, uint32_t size) {
  unsigned char command[5];
  int res;
  int sector, sector_start, sector_end;

  sector_start = address2sector(stlink_infos, address);
  sector_end = address2sector(stlink_infos, address + size - 1);
  if (sector_start < 0 || sector_end < 0 || sector_end < sector_start) {
    return -1;
  }

  for (sector = sector_start; sector <= sector_end; sector++) {
    command[0] = ERASE_SECTORS_COMMAND;
    command[1] = sector & 0xFF;
    command[2] = 0;
    command[3] = 0;
    command[4] = 0;

    res = stlink_dfu_download(dev_handle, command, sizeof(command), 0, NULL);
    if (res != 0) return res;
  }

  return 0;
}

int stlink_set_address(libusb_device_handle *dev_handle, uint32_t address) {
  unsigned char command[5];
  int res;

  command[0] = SET_ADDRESS_POINTER_COMMAND;
  command[1] = address & 0xFF;
  command[2] = (address >> 8) & 0xFF;
  command[3] = (address >> 16) & 0xFF;
  command[4] = (address >> 24) & 0xFF;

  res = stlink_dfu_download(dev_handle, command, sizeof(command), 0, NULL);
  return res;
}

int stlink_flash(libusb_device_handle *dev_handle,
		 const char *filename,
		 unsigned int base_offset,
		 unsigned int chunk_size,
		 struct STLinkInfos *stlink_infos) {
  unsigned char *firmware, firmware_chunk[chunk_size];
  unsigned int cur_chunk_size, flashed_bytes, file_size;
  int fd, res;
  struct stat firmware_stat;

  fd = open(filename, O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "File opening failed\n");
    return -1;
  }
  
  fstat(fd, &firmware_stat);

  file_size = firmware_stat.st_size;

  firmware = mmap(NULL, file_size, PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (firmware == MAP_FAILED) {
    fprintf(stderr, "mmap failure\n");
    return -1;
  }

  printf("Loaded firmware : %s, size : %d bytes\n", filename, (int)file_size);

  flashed_bytes = 0;

  if (stlink_infos->stlink_version == 3) {
    printf("Erasing...\n");
    res = stlink_erase_sectors(dev_handle, stlink_infos, base_offset, file_size);
    if (res) {
      fprintf(stderr, "Erase error\n");
      return res;
    }
  }

  while (flashed_bytes < file_size) {
    if ((flashed_bytes+chunk_size) > file_size) {
      cur_chunk_size = file_size - flashed_bytes;
    } else {
      cur_chunk_size = chunk_size;
    }

    if (stlink_infos->stlink_version < 3) {
      res = stlink_erase_pages(dev_handle, base_offset+flashed_bytes);
      if (res) {
        fprintf(stderr, "Erase error\n");
        return res;
      }
    }
    
    res = stlink_set_address(dev_handle, base_offset+flashed_bytes);
    if (res) {
      fprintf(stderr, "SetAddress error\n");
      return res;
    }

    memcpy(firmware_chunk, firmware+flashed_bytes, cur_chunk_size);
    memset(firmware_chunk+cur_chunk_size, 0xff, chunk_size-cur_chunk_size);
    res = stlink_dfu_download(dev_handle, firmware_chunk, chunk_size, 2, stlink_infos);
    if (res) {
      fprintf(stderr, "Download error\n");
      return res;
    }

    printf(".");
    fflush(stdout); /* Flush stdout buffer */

    flashed_bytes += cur_chunk_size;
  }

  munmap(firmware, file_size);
  close(fd);

  printf("\n");

  return 0;
}

int stlink_exit_dfu(libusb_device_handle *dev_handle) {
  unsigned char data[16];
  int rw_bytes, res;
  
  memset(data, 0, sizeof(data));
  
  data[0] = 0xF3;
  data[1] = DFU_EXIT;
  
  res = libusb_bulk_transfer(dev_handle,
			     EP_OUT,
			     data,
			     16,
			     &rw_bytes,
			     USB_TIMEOUT);
  if (res || rw_bytes != 16) {
    fprintf(stderr, "USB transfer failure\n");
    return -1;
  }
  return 0;
}
