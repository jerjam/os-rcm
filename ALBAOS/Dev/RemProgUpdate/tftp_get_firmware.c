#ifndef TFTP_SERVER
	#define TFTP_SERVER  "84.89.255.184"
	#define TFTP_FILE "firmware.bin"
#endif

//	#define BU_TEMP_USE_FAT				// use file on FAT filesystem
//	#define BU_TEMP_USE_SBF				// use unused portion of serial boot flash
	#define BU_TEMP_USE_SFLASH			// use serial data flash (without FAT)
//	#define BU_TEMP_USE_DIRECT_WRITE	// write directly to boot firmware image

//	#define BU_TEMP_PAGE_OFFSET 0

#define TCPCONFIG 5

// Make sure an option has been enabled.
#if ! defined BU_TEMP_USE_FAT && \
	 ! defined BU_TEMP_USE_SBF && \
	 ! defined BU_TEMP_USE_SFLASH && \
	 ! defined BU_TEMP_USE_DIRECT_WRITE && \
	 ! defined BU_ENABLE_SECONDARY
#fatal "You must uncomment a BU_TEMP_USE_xxx macro at the top of this sample."
#endif

// need a UDP buffer for the TFTP connection
#define MAX_UDP_SOCKET_BUFFERS 2

#use "dcrtcp.lib"
#use "tftp.lib"

#ifdef BU_TEMP_USE_SFLASH
	#use "sflash.lib"
#endif

#use "board_update.lib"

int install_firmware()
{
	firmware_info_t fi;
	int			i;
	int 			result;
	int			progress;

   printf( "verifying and installing new firmware\n");

   result = buOpenFirmwareTemp( BU_FLAG_NONE);

   if (!result)
   {
	   // buGetInfo is a non-blocking call, and may take multiple attempts
	   // before the file is completely open.
	   i = 0;
	   do {
	      result = buGetInfo( &fi);
	   } while ( (result == -EBUSY) && (++i < 20) );
   }

   if (result)
   {
      printf( "Error %d reading new firmware\n", result);
   }
   else
   {
      printf( "Found %s v%u.%02x...\n", fi.program_name,
         fi.version >> 8, fi.version & 0xFF);

      printf( "Attempting to install new version...\n");
      progress = 0;
      do
      {
         printf( "\r verify %u.%02u%%\r", progress / 100, progress % 100);
         result = buVerifyFirmware( &progress);
      } while (result == -EAGAIN);
      if (result)
      {
         printf( "\nError %d verifying firmware\n", result);
         printf( "firmware image bad, installation aborted\n");
      }
      else
      {
         printf( "verify complete, installing new firmware\n");
         result = buInstallFirmware();
         if (result)
         {
            printf( "!!! Error %d installing firmware !!!\n", result);
         }
         else
         {
            printf( "Install successful: rebooting.\n");
            exit( 0);
         }
      }
   }

   // make sure firmware file is closed if there were any errors
	while (buCloseFirmware() == -EBUSY);

   return result;
}

// It's safer to keep sockets as globals, especially when using uC/OS-II.  If
// your socket is on the stack, and another task (with its own stack, instead
// of your task's stack) calls tcp_tick, tcp_tick won't find your socket
// structure in the other task's stack.
// Even though this sample doesn't use uC/OS-II, using globals for sockets is
// a good habit to be in.
udp_Socket demosock;

/*	Return codes:
		Most return codes come from install_firmware (see above).

		Additional return codes for tftp_and_install:
         -EPERM: buTempCreate not supported on this hardware.
         -ENODEV: Couldn't read from serial flash.
         -EBUSY: Timeout waiting for FAT filesystem.
         <0: Error opening FAT file, see fat_Open for full list of
             error codes and their meanings.

		TFTP Errors:
         -1: Error from remote side, transfer terminated.  In this
             case, the ts_addr->file field will be overwritten with a
             null-terminated error message from the server.
         -2: Error, could not contact remote host or lost contact.
         -3: Timed out, transfer terminated.
         -4: (not used)
         -5: Transfer complete, but truncated because buffer too
             small to receive the complete file.
			-7: Server address zero. (Failed to resolve TFTP_SERVER)
*/

int tftp_and_install( char *server, char *file)
{
	int 			result;
	int			offset;
	char			ipbuf[16];
	unsigned long	copied;
	struct tftp_state ts;
	char			buffer[512];

  	ts.state = 0;								// 0 = read
  	ts.buf_len = 512;							// download a block at a time
  	ts.buf_addr = paddr(buffer);			// address of buffer
  	ts.my_tid = 0;								// zero to use default TFTP UDP port number
  	ts.sock = &demosock;						// point to socket to use
  	ts.rem_ip = resolve(TFTP_SERVER);	// resolve server IP address
  	ts.mode = TFTP_MODE_OCTET;				// send/receive binary data
  	strcpy(ts.file, TFTP_FILE);			// set file name on server

  	printf("Downloading %s from %s via TFTP...\n",
  		ts.file, inet_ntoa (ipbuf, ts.rem_ip));

  	if ( (result = tftp_init(&ts)) )
  	{
		printf("Couldn't resolve TFTP server address.\n");
		return result;
  	}

	while ( (result = buTempCreate()) == -EBUSY);
	if (result)
	{
		printf( "Error %d calling buTempCreate\n", result);
		return result;
	}

	copied = 0;
  	// This uses the non-blocking TFTP functions, but in a blocking
  	// manner.  It would be easier to use tftp_exec(), but this
  	// doesn't return the server error message.
	while ((result = tftp_tick(&ts)) >= 0)
	{
		if (ts.buf_used)
		{
			// buTempWrite is non-blocking, so it may take multiple calls to
			// complete the write.
			offset = 0;
			while (offset < ts.buf_used)
			{
				result = buTempWrite( &buffer[offset], ts.buf_used - offset);
				if (result == -EBUSY)
				{
					// resources busy, try again without any changes
				}
				else if (result < 0)
				{
					printf( "Error %d writing firmware to temp location.\n", result);
	            sock_close( &demosock);
	            return result;
				}
				else
				{
					offset += result;
				}
			}

			copied += ts.buf_used;
			printf( " %lu bytes received\r", copied);
			ts.buf_used = 0;
		}
		if (!result)
		{
			// this was the last block of data, exit
			break;
		}
	}
	printf( "\n");

	// close firmware, if it's still open
   while (buTempClose() == -EBUSY);

	switch (result)
	{
	   case 0:
	      printf( "Download completed, attempting to install.\n");
	      result = install_firmware();
	      break;

	   case -5:
	      printf( "ERROR: Download completed, but truncated.\n");
	      break;

		case -2:
			printf( "ERROR: Could not contact remote host or lost contact.\n");
			break;

	   case -3:
	      printf( "ERROR: Download timed out.\n");
	      break;

	   case -1:
         // tftp_tick() puts error messages in ts.file
         printf( "  Message from server: %s\n", ts.file);
         // fall-through to default case, that download failed

	   default:
	      printf( "ERROR: Download failed (code %d)\n", result);
	}

   #ifdef BU_TEMP_USE_DIRECT_WRITE
      if (result)
      {
         printf( "attempting to restore boot firmware from RAM\n");
         // There was an error downloading or installing the firmware,
         // so we need to restore the boot firmware image from the copy
         // of the program running in RAM.
         result = buRestoreFirmware( 0);
         if (result)
         {
            printf( "error %d attempting to restore firmware\n", result);
            // At this point, the firmware stored on the boot flash is
            // corrupted and cannot be restored.
         }
         else
         {
            printf( "restore complete\n");
         }
      }
   #endif

   return result;
}

int main()
{
	int result;
	char url[128];
	char localfile[128];

	printf( "Firmware Download Sample (TFTP)\n\n");

	printf( "Initializing TCP/IP stack...\n");

	// Start network and wait for interface to come up (or error exit).
	sock_init_or_exit(1);

	printf( "Press any key to download\n\t%s from %s\n", TFTP_FILE, TFTP_SERVER);
	getchar();
	printf( "Connecting...\n");

	result = tftp_and_install( TFTP_SERVER, TFTP_FILE);

	if (result)
	{
		exit( result);
	}

	return 0;
}