/*
 *     AAA    M M    OOO    SSSS
 *    A   A  M M M  O   O  S 
 *    AAAAA  M M M  O   O   SSS
 *    A   A  M   M  O   O      S
 *    A   A  M   M   OOO   SSSS 
 *
 *    Author:  Stephen Fewer
 *    License: GNU General Public License (GPL)
 */

#include <kernel/io/dev/floppy.h>
#include <kernel/kernel.h>
#include <kernel/mm/mm.h>
#include <kernel/io/device.h>
#include <kernel/io/io.h>
#include <kernel/io/dma.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/isr.h>

struct FLOPPY_DRIVE * floppy0 = NULL;
struct FLOPPY_DRIVE * floppy1 = NULL;

// we must use the volatile keyword here to force the compiler to
// check the physical location and not optimize or cache this value
static volatile BYTE floppy_donewait = FALSE;

struct FLOPPY_GEOMETRY floppy_geometrys[6] = {
	{  0, 0,  0,   0 },	// no drive
	{ 40, 2,  9, 512 },	// 360KB 5.25"
	{ 80, 2, 15, 512 },	// 1.2MB 5.25"
	{ 80, 2,  9, 512 },	// 720KB 3.5"
	{ 80, 2, 18, 512 },	// 1.44MB 3.5"
	{ 80, 2, 36, 512 }	// 2.88MB 3.5"
};

// Send_Byte Routine as outlined in Figure 8.1 of the Intel 82077AA spec
void floppy_sendbyte( struct FLOPPY_DRIVE * floppy, BYTE byte )
{
    struct MSR msr;
    //initialize timeout counter
    int timeout = FLOPPY_TIMEOUT;
    // loop untill we timeout
    while( timeout-- )
    {
    	// read MSR
		msr.data = inportb( floppy->base + FLOPPY_MSR );
		// loop untill ready status and FIFO direction is inward
		if( msr.bits.mrq && !msr.bits.dio )
		{
			// write byte to FIFO
		    outportb( floppy->base + FLOPPY_DATA, byte );
		    return;
		}
		// delay
		inportb( 0x80 );
    }	
}

// Get_Byte Routine as outlined in Figure 8.2 of the Intel 82077AA spec
BYTE floppy_getbyte( struct FLOPPY_DRIVE * floppy )
{
    struct MSR msr;
    //initialize timeout counter
    int timeout = FLOPPY_TIMEOUT;
    // loop untill we timeout
    while( timeout-- )
    {
    	// read MSR
		msr.data = inportb( floppy->base + FLOPPY_MSR );
		// loop untill ready status and FIFO direction is outward
		if( msr.bits.mrq && msr.bits.dio )
			// return data byte from FIFO
		    return inportb( floppy->base + FLOPPY_DATA );
	    // delay
		inportb( 0x80 );
    }
    return -1;	
}

int floppy_on( struct FLOPPY_DRIVE * floppy )
{
	struct DOR dor;

	dor.data = 0x00;

	dor.bits.dma = TRUE;
	dor.bits.drive = ( floppy->base == FLOPPY_PRIMARY ? 0 : 1 );
	dor.bits.reset = TRUE;
	
	if( floppy->base == FLOPPY_PRIMARY )
		dor.bits.mota = TRUE;
	else
		dor.bits.motb = TRUE;
	
	outportb( floppy->base + FLOPPY_DOR, dor.data );
	
	return TRUE;
}

int floppy_off( struct FLOPPY_DRIVE * floppy )
{
	struct DOR dor;
	// clear it
	dor.data = 0x00;

	dor.bits.dma = TRUE;
	dor.bits.drive = ( floppy->base == FLOPPY_PRIMARY ? 0 : 1 );
	dor.bits.reset = TRUE;
	
	outportb( floppy->base + FLOPPY_DOR, dor.data );
	
	return TRUE;
}

DWORD floppy_handler( struct TASK_STACK * taskstack )
{
	// set the donewait flag to signal a floppy_wait() to finish
	floppy_donewait = TRUE;
	return (DWORD)NULL;
}

int floppy_wait( struct FLOPPY_DRIVE * floppy )
{
	// wait for interrupt to set the donewait flag
    while( TRUE )
    {
    	if( floppy_donewait == TRUE )
    		break;
    	//inportb( 0x80 );
    }
    // reset the donewait flag
    floppy_donewait = FALSE;
    // issue a sence interrupt status command
	floppy_sendbyte( floppy, FLOPPY_SIS );
    // get the result
    floppy->st0.data = floppy_getbyte( floppy );
    // get the current cylinder
    floppy->current_cylinder = floppy_getbyte( floppy );
    return TRUE;
}

int floppy_recalibrate( struct FLOPPY_DRIVE * floppy )
{
	// turn on the floppy motor
    floppy_on( floppy );
    // recalibrate the drive
    floppy_sendbyte( floppy, FLOPPY_RECALIBRATE );
    // specify which drive
    floppy_sendbyte( floppy, ( floppy->base == FLOPPY_PRIMARY ? 0 : 1 ) );
    // wait
    floppy_wait( floppy );
    // turn off the motor
    floppy_off( floppy );
    return TRUE;
}

int floppy_seekcylinder( struct FLOPPY_DRIVE * floppy, BYTE cylinder )
{
	// check if we actually need to perform this operation
	if( floppy->current_cylinder == cylinder )
		return TRUE;
	// issue a seek command
	floppy_sendbyte( floppy, FLOPPY_SEEK );
	// specify drive
	floppy_sendbyte( floppy, ( floppy->base == FLOPPY_PRIMARY ? 0 : 1 ) );
	// specify cylinder
	floppy_sendbyte( floppy, cylinder );
	// wait for the controller to send an interrupt back
	floppy_wait( floppy );
	// test if the seek operation performed correctly
	if( floppy->current_cylinder != cylinder )
		return FALSE;
	return TRUE;
}

int floppy_reset( struct FLOPPY_DRIVE * floppy )
{
	struct DOR dor;
	dor.data = 0x00;
	// disable the controller and irq and dma
	outportb( floppy->base + FLOPPY_DOR, dor.data );
	// enable the controller
	dor.bits.dma = TRUE;
	dor.bits.reset = TRUE;
	outportb( floppy->base + FLOPPY_DOR, dor.data );
	// wait for the controller to send an interrupt back
	floppy_wait( floppy );
	// write 0x00 to the config controll register
	outportb( floppy->base + FLOPPY_CCR, 0x00 );
	// recalibrate the drive
	floppy_recalibrate( floppy );
	return TRUE;
}

void floppy_blockGeometry( struct FLOPPY_DRIVE * floppy, int block, struct FLOPPY_GEOMETRY * geometry )
{
	// locate the correct cylinder
	geometry->cylinders = block / ( floppy->geometry->sectors * floppy->geometry->heads );
	// locate the correct head
	geometry->heads = ( block % ( floppy->geometry->sectors * floppy->geometry->heads ) ) / floppy->geometry->sectors;
	// locate the correct sector
	geometry->sectors = block % floppy->geometry->sectors + 1;
}

// based on Figure 8.5 (read/write) of the Intel 82077AA spec
int floppy_readBlock( struct FLOPPY_DRIVE * floppy, int block, void * buffer )
{
	void * dma_address = (void *)0x00080000;
	int tries = FLOPPY_RWTRIES;
	struct FLOPPY_GEOMETRY blockGeometry;
	// retrieve the block geometry
	floppy_blockGeometry( floppy, block, &blockGeometry );
	// we try this 3 times as laid out int the Intel documentation
    while( tries-- )
    {		
    	// turn on the floppy motor
    	floppy_on( floppy );
    	// seek to the correct location
    	if( !floppy_seekcylinder( floppy, blockGeometry.cylinders ) )
    	{
    		// turn off the floppy motor
    		floppy_off( floppy );
    		return FALSE;	
    	}
    	// setup DMA
    	dma_write( FLOPPY_DMA_CHANNEL, dma_address, floppy->geometry->blocksize );
    	// issue a read command
    	floppy_sendbyte( floppy, FLOPPY_READ );
    	// followed by the read data
    	floppy_sendbyte( floppy, (blockGeometry.heads << 2) | ( floppy->base == FLOPPY_PRIMARY ? 0 : 1 ) );
    	floppy_sendbyte( floppy, blockGeometry.cylinders );
		floppy_sendbyte( floppy, blockGeometry.heads );
		floppy_sendbyte( floppy, blockGeometry.sectors );
		floppy_sendbyte( floppy, 2 );
		floppy_sendbyte( floppy, floppy->geometry->sectors );
		floppy_sendbyte( floppy, 0x1B );
		floppy_sendbyte( floppy, 0xFF );
		// wait for the floppy drive to send back an interrupt
		if( !floppy_wait( floppy ) )
		{
			// if this fails reset the drive
			floppy_reset( floppy );
			return FALSE;
		}
		// read in the result phase of the read command
		// we dont need the bytes we just need to clear the FIFO queue
		floppy_getbyte( floppy ); // st0
		floppy_getbyte( floppy ); // st1
		floppy_getbyte( floppy ); // st2
		floppy_getbyte( floppy ); // cylinder
		floppy_getbyte( floppy ); // head
		floppy_getbyte( floppy ); // sector number
		floppy_getbyte( floppy ); // sector size
		// test if operation was successfull, st0 was set by the previous floppy_wait() call
		if( floppy->st0.bits.int_code == 0x00 )
		{
			// turn off the floppy motor
    		floppy_off( floppy );
			// copy block back to buffer
			memcpy( buffer, dma_address, floppy->geometry->blocksize );
			return TRUE;
		}
		// recalibrate the drive
		floppy_recalibrate( floppy );
    }
	// we fail if we cant read in three tries
	return FALSE;
}

struct IO_HANDLE * floppy_open( struct IO_HANDLE * handle, char * filename )
{
	// see if were trying to open floppy0
	if( strcmp( filename, floppy0->name ) == 0 )
	{
		// if its allready locked we cant open it
		if( floppy0->locked == TRUE )
			return NULL;
		// lock the drive
		floppy0->locked = TRUE;
		// associate flopp0 with the handle
		handle->data = floppy0;	
		// reset the drive
		floppy_reset( floppy0 );
	}
	else if( strcmp( filename, floppy1->name ) == 0 )
	{
		if( floppy1->locked == TRUE )
			return NULL;
		handle->data = floppy1;
		floppy1->locked = TRUE;
		// reset the drive
		floppy_reset( floppy1 );
	}
	else
	{
		// cant open the device
		return NULL;
	}
	// return the floppy handle
	return handle;	
}

int floppy_close( struct IO_HANDLE * handle)
{
	struct FLOPPY_DRIVE * floppy = (struct FLOPPY_DRIVE *)handle->data;
	// turn off the drive if its on
	floppy_off( floppy );
	// set the current block to zero for future access
	floppy->current_block = 0;
	// unlock the drive
	floppy->locked = FALSE;
	return 0;	
}

int floppy_read( struct IO_HANDLE * handle, BYTE * buffer, DWORD size  )
{
	struct FLOPPY_DRIVE * floppy = (struct FLOPPY_DRIVE *)handle->data;
	// make sure the buffer is big enough	
	if( size < floppy->geometry->blocksize )
		return -1;
	// try to read in the next block into the buffer
	if( floppy_readBlock( floppy, floppy->current_block, buffer ) )
	{
		// if we succeed increment the current block for the next read
		floppy->current_block++;
		// return th block size
		return floppy->geometry->blocksize;
	}
	// return fail
	return -1;
}

int floppy_write( struct IO_HANDLE * handle, BYTE * buffer, DWORD size  )
{
	return -1;	
}

int floppy_seek( struct IO_HANDLE * handle, DWORD offset, BYTE origin )
{
	struct FLOPPY_DRIVE * floppy = (struct FLOPPY_DRIVE *)handle->data;
	// calculate the current block from the offset
	floppy->current_block = ( offset / floppy->geometry->blocksize );
	// returnt he current block to the caller
	return floppy->current_block;	
}

void floppy_init()
{
	BYTE i, floppy_type;
	struct IO_CALLTABLE * calltable;
	// create and initilise the call table
	calltable = (struct IO_CALLTABLE *)mm_malloc( sizeof(struct IO_CALLTABLE) );
	calltable->open = floppy_open;
	calltable->close = floppy_close;
	calltable->read = floppy_read;
	calltable->write = floppy_write;
    calltable->seek = floppy_seek;
    // setup the floppy handler
	isr_setHandler( IRQ6, floppy_handler );
    // ask the CMOS if we have any floppy drives
    // location 0x10 has the floppy info
	outportb( 0x70, 0x10 );
	i = inportb( 0x71 );
	// detect the first floppy drive
	floppy_type = i >> 4;
	if( floppy_type != 0 )
	{
		floppy0 = (struct FLOPPY_DRIVE *)mm_malloc( sizeof(struct FLOPPY_DRIVE) );
		// set the device name
		floppy0->name = "/device/floppy0";
		// set the floppy command base address
		floppy0->base = FLOPPY_PRIMARY;
		// set the correct floppy geometry
		floppy0->geometry = &floppy_geometrys[floppy_type];
		// unlock the drive
		floppy0->locked = FALSE;
		// add it to the device manager
		device_add( floppy0->name, calltable );
	}
	// detect the second floppy drive
    floppy_type = i & 0x0F;
 	if( floppy_type != 0 )
	{
		floppy1 = (struct FLOPPY_DRIVE *)mm_malloc( sizeof(struct FLOPPY_DRIVE) );
		// set the device name
		floppy1->name = "/device/floppy1";
		// set the floppy command base address
		floppy1->base = FLOPPY_SECONDARY;
		// set the correct floppy geometry
		floppy1->geometry = &floppy_geometrys[floppy_type];
		// unlock the drive
		floppy1->locked = FALSE;
		// add it to the device manager
		device_add( floppy1->name, calltable );
	}
}
