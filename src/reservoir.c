/**********************************************************************
 * ISO MPEG Audio Subgroup Software Simulation Group (1996)
 * ISO 13818-3 MPEG-2 Audio Encoder - Lower Sampling Frequency Extension
 *
 * $Id: reservoir.c,v 1.1 1996/02/14 04:04:23 rowlands Exp $
 *
 * $Log: reservoir.c,v $
 * Revision 1.1  1996/02/14 04:04:23  rowlands
 * Initial revision
 *
 * Received from Mike Coleman
 **********************************************************************/
/*
  Revision History:

  Date        Programmer                Comment
  ==========  ========================= ===============================
  1995/09/06  mc@fivebats.com           created

*/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include "l3side.h"
#include "loop.h"
#include "huffman.h"
#include "l3bitstream.h"
#include "reservoir.h"

/*
  Layer3 bit reservoir:
  Described in C.1.5.4.2.2 of the IS
*/

static int ResvSize = 0; /* in bits */
static int ResvMax  = 0; /* in bits */

/*
  ResvFrameBegin:
  Called at the beginning of a frame. Updates the maximum
  size of the reservoir, and checks to make sure main_data_begin
  was set properly by the formatter
*/
void
ResvFrameBegin( frame_params *fr_ps, III_side_info_t *l3_side, int mean_bits, int frameLength )
{
    layer *info;
    int fullFrameBits, mode_gr;
    int expectedResvSize, resvLimit;

    info = fr_ps->header;
    if ( info->version == 1 )
    {
	mode_gr = 2;
	resvLimit = 4088; /* main_data_begin has 9 bits in MPEG 1 */
    }
    else
    {
	mode_gr = 1;
	resvLimit = 2040; /* main_data_begin has 8 bits in MPEG 2 */
    }

    /*
      main_data_begin was set by the formatter to the
      expected value for the next call -- this should
      agree with our reservoir size
    */
    expectedResvSize = l3_side->main_data_begin * 8;
#ifdef DEBUG
    fprintf( stderr, ">>> ResvSize = %d\n", ResvSize );
#endif
    assert( expectedResvSize == ResvSize );

    fullFrameBits = mean_bits * mode_gr;

    /*
      determine maximum size of reservoir:
      ResvMax + frameLength <= 7680;
    */
    if ( frameLength > 7680 )
	ResvMax = 0;
    else
	ResvMax = 7680 - frameLength;

    /*
      limit max size to resvLimit bits because
      main_data_begin cannot indicate a
      larger value
      */
    if ( ResvMax > resvLimit )
	ResvMax = resvLimit;
}

/*
  ResvMaxBits:
  Called at the beginning of each granule to get the max bit
  allowance for the current granule based on reservoir size
  and perceptual entropy.
*/
int
ResvMaxBits( frame_params *fr_ps, III_side_info_t *l3_side, double *pe, int mean_bits )
{
    int more_bits, max_bits, add_bits, over_bits;

    mean_bits /= fr_ps->stereo;
    max_bits = mean_bits;

    if ( max_bits > 4095 )
	max_bits = 4095;

    if ( ResvMax == 0 )
	return max_bits;

    more_bits = *pe * 3.1 - mean_bits;
    add_bits = 0;
    if ( more_bits > 100 )
    {
	int frac = (ResvSize * 6) / 10;

	if ( frac < more_bits )
	    add_bits = frac;
	else
	    add_bits = more_bits;
    }
    over_bits = ResvSize - ((ResvMax * 8) / 10) - add_bits;
    if ( over_bits > 0 )
	add_bits += over_bits;

    max_bits += add_bits;
    if ( max_bits > 4095 )
	max_bits = 4095;
    return max_bits;
}

/*
  ResvAdjust:
  Called after a granule's bit allocation. Readjusts the size of
  the reservoir to reflect the granule's usage.
*/
void
ResvAdjust( frame_params *fr_ps, gr_info *gi, III_side_info_t *l3_side, int mean_bits )
{
    ResvSize += (mean_bits / fr_ps->stereo) - gi->part2_3_length;
}

/*
  ResvFrameEnd:
  Called after all granules in a frame have been allocated. Makes sure
  that the reservoir size is within limits, possibly by adding stuffing
  bits. Note that stuffing bits are added by increasing a granule's
  part2_3_length. The bitstream formatter will detect this and write the
  appropriate stuffing bits to the bitstream.
*/
void
ResvFrameEnd( frame_params *fr_ps, III_side_info_t *l3_side, int mean_bits )
{
    layer *info;
    gr_info *gi;
    int mode_gr, gr, ch, stereo, ancillary_pad, stuffingBits;
    int over_bits;

    info   = fr_ps->header;
    stereo = fr_ps->stereo;
    mode_gr = (info->version == 1) ? 2 : 1;
    ancillary_pad = 0;

#if 1
    /* just in case mean_bits is odd, this is necessary... */
    if ( (stereo == 2) && (mean_bits & 1) )
	ResvSize += 1;
#endif

    over_bits = ResvSize - ResvMax;
    if ( over_bits < 0 )
	over_bits = 0;
    
    ResvSize -= over_bits;
    stuffingBits = over_bits + ancillary_pad;

    /* we must be byte aligned */
    if ( (over_bits = ResvSize % 8) )
    {
	stuffingBits += over_bits;
	ResvSize -= over_bits;
    }

    if ( stuffingBits )
    {
	/*
	  plan a: put all into the first granule
	  This was preferred by someone designing a
	  real-time decoder...
	*/
	gi = (gr_info *) &(l3_side->gr[0].ch[0]);	
	
	if ( gi->part2_3_length + stuffingBits < 4095 )
	    gi->part2_3_length += stuffingBits;
	else
	{
	    /* plan b: distribute throughout the granules */
	    for (gr = 0; gr < mode_gr; gr++ )
		for (ch = 0; ch < stereo; ch++ )
		{
		    int extraBits, bitsThisGr;
		    gr_info *gi = (gr_info *) &(l3_side->gr[gr].ch[ch]);
		    if ( stuffingBits == 0 )
			break;
		    extraBits = 4095 - gi->part2_3_length;
		    bitsThisGr = extraBits < stuffingBits ? extraBits : stuffingBits;
		    gi->part2_3_length += bitsThisGr;
		    stuffingBits -= bitsThisGr;
		}
	    /*
	      If any stuffing bits remain, we elect to spill them
	      into ancillary data. The bitstream formatter will do this if
	      l3side->resvDrain is set
	    */
#ifdef DEBUG
	    if ( stuffingBits )
		fprintf( stderr, "spilling %d stuffing bits into ancillary data\n", stuffingBits );
#endif
	    l3_side->resvDrain = stuffingBits;
	}
    }
}


