#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "Gb_Apu_.h"
#include "Gb_Oscs_.h"
#include "Sound_Buffer.h"
#include "../System.h"

// Blip_Buffer 0.4.1. http://www.slack.net/~ant/

/* Copyright (C) 2003-2007 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"

#ifdef BLARGG_ENABLE_OPTIMIZER
        #include BLARGG_ENABLE_OPTIMIZER
#endif

/* BLIP BUFFER */

#define silent_buf_size 1

Blip_Buffer::Blip_Buffer()
{
        factor_       = INT_MAX;
        buffer_       = 0;
        buffer_size_  = 0;
        sample_rate_  = 0;
        bass_shift_   = 0;
        clock_rate_   = 0;
        bass_freq_    = 16;
        length_       = 0;
#ifndef FASTER_SOUND_HACK_NON_SILENCE
        last_non_silence = 0;
#endif

        clear();
}

#ifndef FASTER_SOUND_HACK_NON_SILENCE
#define blip_buffer_remove_silence(count) \
{ \
   if ( (b.last_non_silence -= count) < 0) \
      b.last_non_silence = 0; \
   \
   b.offset_ -= (uint32_t) count << BLIP_BUFFER_ACCURACY; \
}
#endif

Blip_Buffer::~Blip_Buffer()
{
        if ( buffer_size_ != silent_buf_size )
                free( buffer_ );
}

void Blip_Buffer::clear( int entire_buffer )
{
#ifndef FASTER_SOUND_HACK_NON_SILENCE
        last_non_silence = 0;
#endif
        offset_       = 0;
        reader_accum_ = 0;
        modified_     = 0;
        if ( buffer_ )
        {
                long count = (entire_buffer ? buffer_size_ : samples_avail());
                memset( buffer_, 0, (count + blip_buffer_extra_) * sizeof (buf_t_) );
        }
}

const char * Blip_Buffer::set_sample_rate( long new_rate, int msec )
{
        if ( buffer_size_ == silent_buf_size )
                return "Internal (tried to resize Silent_Blip_Buffer)";

        // start with maximum length that resampled time can represent
        long new_size = (ULONG_MAX >> BLIP_BUFFER_ACCURACY) - blip_buffer_extra_ - 64;
        if ( msec != blip_max_length )
        {
                long s = (new_rate * (msec + 1) + 999) / 1000;
                if ( s < new_size )
                        new_size = s;
        }

        if ( buffer_size_ != new_size )
        {
                void* p = realloc( buffer_, (new_size + blip_buffer_extra_) * sizeof *buffer_ );
                if ( !p )
                        return "Out of memory";
                buffer_ = (buf_t_*) p;
        }

        buffer_size_ = new_size;

        // update things based on the sample rate
        sample_rate_ = new_rate;
        length_ = new_size * 1000 / new_rate - 1;

        // update these since they depend on sample rate
        if ( clock_rate_ )
                clock_rate( clock_rate_ );
        bass_freq( bass_freq_ );

        clear();

        return 0; // success
}

uint32_t Blip_Buffer::clock_rate_factor( long rate ) const
{
        double ratio = (double) sample_rate_ / rate;
        int32_t factor = (int32_t) floor( ratio * (1L << BLIP_BUFFER_ACCURACY) + 0.5 );
        return (uint32_t) factor;
}

void Blip_Buffer::bass_freq( int freq )
{
        bass_freq_ = freq;
        int shift = 31;
        if ( freq > 0 )
        {
                shift = 13;
                long f = (freq << 16) / sample_rate_;
                while ( (f >>= 1) && --shift ) { }
        }
        bass_shift_ = shift;
}

void Blip_Buffer::end_frame( int32_t t )
{
        offset_ += t * factor_;
#ifndef FASTER_SOUND_HACK_NON_SILENCE
        if ( clear_modified() )
                last_non_silence = samples_avail() + blip_buffer_extra_;
#endif
}

void Blip_Buffer::remove_samples( long count )
{
#ifndef FASTER_SOUND_HACK_NON_SILENCE
	if ( (last_non_silence -= count) < 0 )
		last_non_silence = 0;
#endif

	offset_ -= (uint32_t) count << BLIP_BUFFER_ACCURACY;

	// copy remaining samples to beginning and clear old samples
	long remain = samples_avail() + blip_buffer_extra_;
	memmove( buffer_, buffer_ + count, remain * sizeof *buffer_ );
	memset( buffer_ + remain, 0, count * sizeof *buffer_ );
}

// Blip_Synth_

Blip_Synth_Fast_::Blip_Synth_Fast_()
{
        buf          = 0;
        last_amp     = 0;
        delta_factor = 0;
}

void Blip_Synth_Fast_::volume_unit( double new_unit )
{
        delta_factor = int (new_unit * (1L << blip_sample_bits) + 0.5);
}

long Blip_Buffer::read_samples( int16_t * out, long count)
{
	int const bass = BLIP_READER_BASS( *this );
	BLIP_READER_BEGIN( reader, *this );
	BLIP_READER_ADJ_( reader, count );
	int16_t * BLIP_RESTRICT out_tmp = out + count;
	int32_t offset = (int32_t) -count;

	do
	{
		int32_t s = BLIP_READER_READ( reader );
		BLIP_READER_NEXT_IDX_( reader, bass, offset );
		BLIP_CLAMP( s, s );
		out_tmp [offset] = (int16_t) s;
	}
	while ( ++offset );

	BLIP_READER_END( reader, *this );

	remove_samples( count );
	//}
		//end of Blip Buffer read_samples

#ifndef FASTER_SOUND_HACK_NON_SILENCE
		if ( (last_non_silence -= count) < 0 )
			last_non_silence = 0;
#endif

		return count;
}

uint32_t const subsample_mask = (1L << BLIP_BUFFER_ACCURACY) - 1;

void Blip_Buffer::save_state( blip_buffer_state_t* out )
{
        out->offset_       = offset_;
        out->reader_accum_ = reader_accum_;
        memcpy( out->buf, &buffer_ [offset_ >> BLIP_BUFFER_ACCURACY], sizeof out->buf );
}

void Blip_Buffer::load_state( blip_buffer_state_t const& in )
{
        clear( false );

        offset_       = in.offset_;
        reader_accum_ = in.reader_accum_;
        memcpy( buffer_, in.buf, sizeof in.buf );
}


#ifndef FASTER_SOUND_HACK_NON_SILENCE
uint32_t Blip_Buffer::non_silent() const
{
        return last_non_silence | (reader_accum_ >> (blip_sample_bits - 16));
}
#endif

// Stereo_Buffer

int const stereo = 2;

Stereo_Buffer::Stereo_Buffer()
{
	samples_per_frame_      = 2;
	length_                 = 0;
	sample_rate_            = 0;
	channels_changed_count_ = 1;
	channel_types_          = 0;
	channel_count_          = 0;
	immediate_removal_      = true;

        chan.center = mixer_bufs [2] = &bufs_buffer [2];
        chan.left   = mixer_bufs [0] = &bufs_buffer [0];
        chan.right  = mixer_bufs [1] = &bufs_buffer [1];
        mixer_samples_read = 0;
}

Stereo_Buffer::~Stereo_Buffer() { }

const char * Stereo_Buffer::set_sample_rate( long rate, int msec )
{
        mixer_samples_read = 0;
        for ( int i = bufs_size; --i >= 0; )
                RETURN_ERR( bufs_buffer [i].set_sample_rate( rate, msec ) );
	sample_rate_ = bufs_buffer[0].sample_rate();
	length_ = bufs_buffer[0].length();
        return 0; 
}

void Stereo_Buffer::clock_rate( long rate )
{
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].clock_rate( rate );
}

double Stereo_Buffer::real_ratio()
{
   return (double)bufs_buffer[0].clock_rate() * (double)bufs_buffer[0].clock_rate_factor(bufs_buffer[0].clock_rate()) / (1 << BLIP_BUFFER_ACCURACY);
}

void Stereo_Buffer::bass_freq( int bass )
{
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].bass_freq( bass );
}


void Stereo_Buffer::clear()
{
        mixer_samples_read = 0;
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].clear();
}

void Stereo_Buffer::end_frame( int32_t time )
{
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].end_frame( time );
}

long Stereo_Buffer::read_samples( int16_t * out, long out_size )
{
        out_size = min( out_size, samples_avail() );

        int pair_count = int (out_size >> 1);
        if ( pair_count )
        {
                mixer_read_pairs( out, pair_count );

                if ( samples_avail() <= 0 || immediate_removal_ )
                {
                        for ( int i = bufs_size; --i >= 0; )
                        {
                                buf_t& b = bufs_buffer [i];
            #ifndef FASTER_SOUND_HACK_NON_SILENCE
                                // TODO: might miss non-silence setting since it checks END of last read
                                if ( !b.non_silent() )
            {
                                        blip_buffer_remove_silence( mixer_samples_read );
            }
                                else
            #endif
                                        b.remove_samples( mixer_samples_read );
                        }
                        mixer_samples_read = 0;
                }
        }
        return out_size;
}


// Stereo_Mixer

// mixers use a single index value to improve performance on register-challenged processors
// offset goes from negative to zero


void Stereo_Buffer::mixer_read_pairs( int16_t* out, int count )
{
	// TODO: if caller never marks buffers as modified, uses mono
	// except that buffer isn't cleared, so caller can encounter
	// subtle problems and not realize the cause.
	mixer_samples_read += count;
#ifndef FASTER_SOUND_HACK_NON_SILENCE
	if ( mixer_bufs [0]->non_silent() | mixer_bufs [1]->non_silent() )
	{
#endif
		int16_t* BLIP_RESTRICT outtemp = out + count * stereo;

		// do left + center and right + center separately to reduce register load
		Blip_Buffer* const* buf = &mixer_bufs [2];
		{
			--buf;
			--outtemp;

			int const bass = BLIP_READER_BASS( *mixer_bufs [2] );
			BLIP_READER_BEGIN( side,   **buf );
			BLIP_READER_BEGIN( center, *mixer_bufs [2] );

			BLIP_READER_ADJ_( side,   mixer_samples_read );
			BLIP_READER_ADJ_( center, mixer_samples_read );

			int offset = -count;
			do
			{
				blargg_long s = BLIP_READER_READ_RAW( center ) + BLIP_READER_READ_RAW( side );
				s >>= blip_sample_bits - 16;
				BLIP_READER_NEXT_IDX_( side,   bass, offset );
				BLIP_READER_NEXT_IDX_( center, bass, offset );
				BLIP_CLAMP( s, s );

				++offset; // before write since out is decremented to slightly before end
				outtemp [offset * stereo] = (int16_t) s;
			}while ( offset );

			BLIP_READER_END( side,   **buf );
		}
		{
			--buf;
			--outtemp;

			int const bass = BLIP_READER_BASS( *mixer_bufs [2] );
			BLIP_READER_BEGIN( side,   **buf );
			BLIP_READER_BEGIN( center, *mixer_bufs [2] );

			BLIP_READER_ADJ_( side,   mixer_samples_read );
			BLIP_READER_ADJ_( center, mixer_samples_read );

			int offset = -count;
			do
			{
				blargg_long s = BLIP_READER_READ_RAW( center ) + BLIP_READER_READ_RAW( side );
				s >>= blip_sample_bits - 16;
				BLIP_READER_NEXT_IDX_( side,   bass, offset );
				BLIP_READER_NEXT_IDX_( center, bass, offset );
				BLIP_CLAMP( s, s );

				++offset; // before write since out is decremented to slightly before end
				outtemp [offset * stereo] = (int16_t) s;
			}while ( offset );

			BLIP_READER_END( side,   **buf );

			// only end center once
			BLIP_READER_END( center, *mixer_bufs [2] );
		}
#ifndef FASTER_SOUND_HACK_NON_SILENCE
	}
	else
	{
		int const bass = BLIP_READER_BASS( *mixer_bufs [2] );
		BLIP_READER_BEGIN( center, *mixer_bufs [2] );
		BLIP_READER_ADJ_( center, mixer_samples_read );

		typedef int16_t stereo_blip_sample_t [stereo];
		stereo_blip_sample_t* BLIP_RESTRICT outtemp = (stereo_blip_sample_t*) out + count;
		int offset = -count;
		do
		{
			blargg_long s = BLIP_READER_READ( center );
			BLIP_READER_NEXT_IDX_( center, bass, offset );
			BLIP_CLAMP( s, s );

			outtemp [offset] [0] = (int16_t) s;
			outtemp [offset] [1] = (int16_t) s;
		}
		while ( ++offset );
		BLIP_READER_END( center, *mixer_bufs [2] );
	}
#endif
}

int const fixed_shift = 12;
#define TO_FIXED( f )   fixed_t ((f) * ((fixed_t) 1 << fixed_shift))
#define FROM_FIXED( f ) ((f) >> fixed_shift)

int const max_read = 2560; // determines minimum delay

#ifndef USE_GBA_ONLY
void Effects_Buffer::clear()
{
}

void Effects_Buffer::mixer_read_pairs( int16_t * out, int count )
{
	// TODO: if caller never marks buffers as modified, uses mono
	// except that buffer isn't cleared, so caller can encounter
	// subtle problems and not realize the cause.
	mixer_samples_read += count;
#ifndef FASTER_SOUND_HACK_NON_SILENCE
	if ( mixer_bufs [0]->non_silent() | mixer_bufs [1]->non_silent() )
	{
#endif
		int16_t * BLIP_RESTRICT outtemp = out + count * stereo;

		// do left + center and right + center separately to reduce register load
		Blip_Buffer* const* buf = &mixer_bufs [2];
		{
			--buf;
			--outtemp;

			int const bass = BLIP_READER_BASS( *mixer_bufs [2] );
			BLIP_READER_BEGIN( side,   **buf );
			BLIP_READER_BEGIN( center, *mixer_bufs [2] );

			BLIP_READER_ADJ_( side,   mixer_samples_read );
			BLIP_READER_ADJ_( center, mixer_samples_read );

			int offset = -count;
			do
			{
				blargg_long s = BLIP_READER_READ_RAW( center ) + BLIP_READER_READ_RAW( side );
				s >>= blip_sample_bits - 16;
				BLIP_READER_NEXT_IDX_( side,   bass, offset );
				BLIP_READER_NEXT_IDX_( center, bass, offset );
				BLIP_CLAMP( s, s );

				++offset; // before write since out is decremented to slightly before end
				outtemp [offset * stereo] = (int16_t) s;
			}while ( offset );

			BLIP_READER_END( side,   **buf );
		}
		{
			--buf;
			--outtemp;

			int const bass = BLIP_READER_BASS( *mixer_bufs [2] );
			BLIP_READER_BEGIN( side,   **buf );
			BLIP_READER_BEGIN( center, *mixer_bufs [2] );

			BLIP_READER_ADJ_( side,   mixer_samples_read );
			BLIP_READER_ADJ_( center, mixer_samples_read );

			int offset = -count;
			do
			{
				blargg_long s = BLIP_READER_READ_RAW( center ) + BLIP_READER_READ_RAW( side );
				s >>= blip_sample_bits - 16;
				BLIP_READER_NEXT_IDX_( side,   bass, offset );
				BLIP_READER_NEXT_IDX_( center, bass, offset );
				BLIP_CLAMP( s, s );

				++offset; // before write since out is decremented to slightly before end
				outtemp [offset * stereo] = (int16_t) s;
			}while ( offset );

			BLIP_READER_END( side,   **buf );

			// only end center once
			BLIP_READER_END( center, *mixer_bufs [2] );
		}
#ifndef FASTER_SOUND_HACK_NON_SILENCE
	}
	else
	{
		int const bass = BLIP_READER_BASS( *mixer_bufs [2] );
		BLIP_READER_BEGIN( center, *mixer_bufs [2] );
		BLIP_READER_ADJ_( center, mixer_samples_read );

		typedef int16_t stereo_blip_sample_t [stereo];
		stereo_blip_sample_t* BLIP_RESTRICT outtemp = (stereo_blip_sample_t*) out + count;
		int offset = -count;
		do
		{
			blargg_long s = BLIP_READER_READ( center );
			BLIP_READER_NEXT_IDX_( center, bass, offset );
			BLIP_CLAMP( s, s );

			outtemp [offset] [0] = (int16_t) s;
			outtemp [offset] [1] = (int16_t) s;
		}
		while ( ++offset );
		BLIP_READER_END( center, *mixer_bufs [2] );
	}
#endif
}

Effects_Buffer::Effects_Buffer( int max_bufs, long echo_size_ )
{
	//from Multi_Buffer
	samples_per_frame_      = stereo;
	length_                 = 0;
	sample_rate_            = 0;
	channels_changed_count_ = 1;
	channel_types_          = 0;
	channel_count_          = 0;
	immediate_removal_      = true;

        echo_size   = max( max_read * (long) stereo, echo_size_ & ~1 );
        clock_rate_ = 0;
        bass_freq_  = 90;
        bufs_buffer       = 0;
        bufs_size   = 0;
        bufs_max    = max( max_bufs, (int) extra_chans );
        no_echo     = true;
        no_effects  = true;

        // defaults
        config_.enabled   = false;
        config_.delay [0] = 120;
        config_.delay [1] = 122;
        config_.feedback  = 0.2f;
        config_.treble    = 0.4f;

        static float const sep = 0.8f;
        config_.side_chans [0].pan = -sep;
        config_.side_chans [1].pan = +sep;
        config_.side_chans [0].vol = 1.0f;
        config_.side_chans [1].vol = 1.0f;

        memset( &s, 0, sizeof s );
        echo_pos       = 0;
        s.low_pass [0] = 0;
        s.low_pass [1] = 0;
        mixer_samples_read = 0;

        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].clear();
        if ( echo.size() )
                memset( echo.begin(), 0, echo.size() * sizeof echo [0] );
}

Effects_Buffer::~Effects_Buffer()
{
        if ( bufs_buffer )
        {
                for ( int i = bufs_size; --i >= 0; )
                        bufs_buffer [i].~buf_t();
                free( bufs_buffer );
                bufs_buffer = 0;
        }
        bufs_size = 0;
}

// avoid using new []
const char * Effects_Buffer::new_bufs( int size )
{
        bufs_buffer = (buf_t*) malloc( size * sizeof *bufs_buffer );
        CHECK_ALLOC( bufs_buffer );
        for ( int i = 0; i < size; i++ )
                new (bufs_buffer + i) buf_t;
        bufs_size = size;
        return 0;
}

const char * Effects_Buffer::set_sample_rate( long rate, int msec )
{
        // extra to allow farther past-the-end pointers
        mixer_samples_read = 0;
        RETURN_ERR( echo.resize( echo_size + stereo ) );
	sample_rate_ = rate;
	length_ = msec;
	return 0;
}

void Effects_Buffer::clock_rate( long rate )
{
        clock_rate_ = rate;
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].clock_rate( clock_rate_ );
}

void Effects_Buffer::bass_freq( int freq )
{
        bass_freq_ = freq;
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].bass_freq( bass_freq_ );
}

const char * Effects_Buffer::set_channel_count( int count, int const* types )
{
	channel_count_ = count;
	channel_types_ = types;

        if ( bufs_buffer )
        {
                for ( int i = bufs_size; --i >= 0; )
                        bufs_buffer [i].~buf_t();
                free( bufs_buffer );
                bufs_buffer = 0;
        }
        bufs_size = 0;

        mixer_samples_read = 0;

        RETURN_ERR( chans.resize( count + extra_chans ) );

        RETURN_ERR( new_bufs( min( bufs_max, count + extra_chans ) ) );

        for ( int i = bufs_size; --i >= 0; )
                RETURN_ERR( bufs_buffer [i].set_sample_rate( sample_rate_, length_ ));

        for ( int i = chans.size(); --i >= 0; )
        {
                chan_t& ch = chans [i];
                ch.cfg.vol      = 1.0f;
                ch.cfg.pan      = 0.0f;
                ch.cfg.surround = false;
                ch.cfg.echo     = false;
        }
        // side channels with echo
        chans [2].cfg.echo = true;
        chans [3].cfg.echo = true;

        clock_rate( clock_rate_ );
        bass_freq( bass_freq_ );
        apply_config();
        echo_pos       = 0;
        s.low_pass [0] = 0;
        s.low_pass [1] = 0;
        mixer_samples_read = 0;

        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].clear();
        if ( echo.size() )
                memset( echo.begin(), 0, echo.size() * sizeof echo [0] );

        return 0;
}

channel_t Effects_Buffer::channel( int i )
{
        i += extra_chans;
        return chans [i].channel;
}


// Configuration

// 3 wave positions with/without surround, 2 multi (one with same config as wave)
int const simple_bufs = 3 * 2 + 2 - 1;

Simple_Effects_Buffer::Simple_Effects_Buffer() :
        Effects_Buffer( extra_chans + simple_bufs, 18 * 1024L )
{
        config_.echo     = 0.20f;
        config_.stereo   = 0.20f;
        config_.surround = true;
        config_.enabled  = false;
}

void Simple_Effects_Buffer::apply_config()
{
        Effects_Buffer::config_t& c = Effects_Buffer::config();

        c.enabled = config_.enabled;
        if ( c.enabled )
        {
                c.delay [0] = 120;
                c.delay [1] = 122;
                c.feedback  = config_.echo * 0.7f;
                c.treble    = 0.6f - 0.3f * config_.echo;

                float sep = config_.stereo + 0.80f;
                if ( sep > 1.0f )
                        sep = 1.0f;

                c.side_chans [0].pan = -sep;
                c.side_chans [1].pan = +sep;

                for ( int i = channel_count_; --i >= 0; )
                {
                        chan_config_t& ch = Effects_Buffer::chan_config( i );

                        ch.pan      = 0.0f;
                        ch.surround = config_.surround;
                        ch.echo     = false;

                        int const type = (channel_types_ ? channel_types_ [i] : 0);
                        if ( !(type & noise_type) )
                        {
                                int index = (type & type_index_mask) % 6 - 3;
                                if ( index < 0 )
                                {
                                        index += 3;
                                        ch.surround = false;
                                        ch.echo     = true;
                                }
                                if ( index >= 1 )
                                {
                                        ch.pan = config_.stereo;
                                        if ( index == 1 )
                                                ch.pan = -ch.pan;
                                }
                        }
                        else if ( type & 1 )
                        {
                                ch.surround = false;
                        }
                }
        }

        Effects_Buffer::apply_config();
}

void Effects_Buffer::apply_config()
{
        int i;

        if ( !bufs_size )
                return;

        s.treble = TO_FIXED( config_.treble );

        bool echo_dirty = false;

        fixed_t old_feedback = s.feedback;
        s.feedback = TO_FIXED( config_.feedback );
        if ( !old_feedback && s.feedback )
                echo_dirty = true;

        // delays
        for ( i = stereo; --i >= 0; )
        {
                long delay = config_.delay [i] * sample_rate_ / 1000 * stereo;
                delay = max( delay, long (max_read * stereo) );
                delay = min( delay, long (echo_size - max_read * stereo) );
                if ( s.delay [i] != delay )
                {
                        s.delay [i] = delay;
                        echo_dirty = true;
                }
        }

        // side channels
        for ( i = 2; --i >= 0; )
        {
                chans [i+2].cfg.vol = chans [i].cfg.vol = config_.side_chans [i].vol * 0.5f;
                chans [i+2].cfg.pan = chans [i].cfg.pan = config_.side_chans [i].pan;
        }

        // convert volumes
        for ( i = chans.size(); --i >= 0; )
        {
                chan_t& ch = chans [i];
                ch.vol [0] = TO_FIXED( ch.cfg.vol - ch.cfg.vol * ch.cfg.pan );
                ch.vol [1] = TO_FIXED( ch.cfg.vol + ch.cfg.vol * ch.cfg.pan );
                if ( ch.cfg.surround )
                        ch.vol [0] = -ch.vol [0];
        }

   //Begin of assign buffers
        // assign channels to buffers
        int buf_count = 0;
        for ( int i = 0; i < (int) chans.size(); i++ )
        {
                // put second two side channels at end to give priority to main channels
                // in case closest matching is necessary
                int x = i;
                if ( i > 1 )
                        x += 2;
                if ( x >= (int) chans.size() )
                        x -= (chans.size() - 2);
                chan_t& ch = chans [x];

                int b = 0;
                for ( ; b < buf_count; b++ )
                {
                        if (    ch.vol [0] == bufs_buffer [b].vol [0] &&
                                        ch.vol [1] == bufs_buffer [b].vol [1] &&
                                        (ch.cfg.echo == bufs_buffer [b].echo || !s.feedback) )
                                break;
                }

                if ( b >= buf_count )
                {
                        if ( buf_count < bufs_max )
                        {
                                bufs_buffer [b].vol [0] = ch.vol [0];
                                bufs_buffer [b].vol [1] = ch.vol [1];
                                bufs_buffer [b].echo    = ch.cfg.echo;
                                buf_count++;
                        }
                        else
                        {
                                // TODO: this is a mess, needs refinement
                                b = 0;
                                fixed_t best_dist = TO_FIXED( 8 );
                                for ( int h = buf_count; --h >= 0; )
                                {
                                        #define CALC_LEVELS( vols, sum, diff, surround ) \
                                        fixed_t sum, diff;\
                                        bool surround = false;\
                                        {\
                                                fixed_t vol_0 = vols [0];\
                                                if ( vol_0 < 0 ) vol_0 = -vol_0, surround = true;\
                                                fixed_t vol_1 = vols [1];\
                                                if ( vol_1 < 0 ) vol_1 = -vol_1, surround = true;\
                                                sum  = vol_0 + vol_1;\
                                                diff = vol_0 - vol_1;\
                                        }
                                        CALC_LEVELS( ch.vol,       ch_sum,  ch_diff,  ch_surround );
                                        CALC_LEVELS( bufs_buffer [h].vol, buf_sum, buf_diff, buf_surround );

                                        fixed_t dist = abs( ch_sum - buf_sum ) + abs( ch_diff - buf_diff );

                                        if ( ch_surround != buf_surround )
                                                dist += TO_FIXED( 1 ) / 2;

                                        if ( s.feedback && ch.cfg.echo != bufs_buffer [h].echo )
                                                dist += TO_FIXED( 1 ) / 2;

                                        if ( best_dist > dist )
                                        {
                                                best_dist = dist;
                                                b = h;
                                        }
                                }
                        }
                }

                ch.channel.center = &bufs_buffer [b];
        }
   //End of assign buffers

        // set side channels
        for ( i = chans.size(); --i >= 0; )
        {
                chan_t& ch = chans [i];
                ch.channel.left  = chans [ch.cfg.echo*2  ].channel.center;
                ch.channel.right = chans [ch.cfg.echo*2+1].channel.center;
        }

        bool old_echo = !no_echo && !no_effects;

        // determine whether effects and echo are needed at all
        no_effects = true;
        no_echo    = true;
        for ( i = chans.size(); --i >= extra_chans; )
        {
                chan_t& ch = chans [i];
                if ( ch.cfg.echo && s.feedback )
                        no_echo = false;

                if ( ch.vol [0] != TO_FIXED( 1 ) || ch.vol [1] != TO_FIXED( 1 ) )
                        no_effects = false;
        }
        if ( !no_echo )
                no_effects = false;

        if (    chans [0].vol [0] != TO_FIXED( 1 ) ||
                        chans [0].vol [1] != TO_FIXED( 0 ) ||
                        chans [1].vol [0] != TO_FIXED( 0 ) ||
                        chans [1].vol [1] != TO_FIXED( 1 ) )
                no_effects = false;

        if ( !config_.enabled )
                no_effects = true;

        if ( no_effects )
        {
                for ( i = chans.size(); --i >= 0; )
                {
                        chan_t& ch = chans [i];
                        ch.channel.center = &bufs_buffer [2];
                        ch.channel.left   = &bufs_buffer [0];
                        ch.channel.right  = &bufs_buffer [1];
                }
        }

        mixer_bufs [0] = &bufs_buffer [0];
        mixer_bufs [1] = &bufs_buffer [1];
        mixer_bufs [2] = &bufs_buffer [2];

        if ( echo_dirty || (!old_echo && (!no_echo && !no_effects)) )
   {
      if ( echo.size() )
         memset( echo.begin(), 0, echo.size() * sizeof echo [0] );
   }

        channels_changed();
}

// Mixing

void Effects_Buffer::end_frame( int32_t time )
{
        for ( int i = bufs_size; --i >= 0; )
                bufs_buffer [i].end_frame( time );
}

long Effects_Buffer::read_samples( int16_t * out, long out_size )
{
        out_size = min( out_size, samples_avail() );

        int pair_count = int (out_size >> 1);
        if ( pair_count )
        {
                if ( no_effects )
                {
                        mixer_read_pairs( out, pair_count );
                }
                else
                {
                        int pairs_remain = pair_count;
                        do
                        {
                                // mix at most max_read pairs at a time
                                int count = max_read;
                                if ( count > pairs_remain )
                                        count = pairs_remain;

                                if ( no_echo )
                                {
                                        // optimization: clear echo here to keep mix_effects() a leaf function
                                        echo_pos = 0;
                                        memset( echo.begin(), 0, count * stereo * sizeof echo [0] );
                                }

                                mix_effects( out, count );

                                blargg_long new_echo_pos = echo_pos + count * stereo;
                                if ( new_echo_pos >= echo_size )
                                        new_echo_pos -= echo_size;
                                echo_pos = new_echo_pos;

                                out += count * stereo;
                                mixer_samples_read += count;
                                pairs_remain -= count;
                        }
                        while ( pairs_remain );
                }

                if ( samples_avail() <= 0 || immediate_removal_ )
                {
                        for ( int i = bufs_size; --i >= 0; )
                        {
                                buf_t& b = bufs_buffer [i];
                                // TODO: might miss non-silence settling since it checks END of last read
            #ifdef FASTER_SOUND_HACK_NON_SILENCE
                                b.remove_samples( mixer_samples_read );
            #else
                                if ( b.non_silent() )
                                        b.remove_samples( mixer_samples_read );
                                else
            {
                                        blip_buffer_remove_silence( mixer_samples_read );
            }
            #endif
                        }
                        mixer_samples_read = 0;
                }
        }
        return out_size;
}

void Effects_Buffer::mix_effects( int16_t * out_, int pair_count )
{
        typedef fixed_t stereo_fixed_t [stereo];

        // add channels with echo, do echo, add channels without echo, then convert to 16-bit and output
        int echo_phase = 1;
        do
        {
                // mix any modified buffers
                {
                        buf_t* buf = bufs_buffer;
                        int bufs_remain = bufs_size;
                        do
                        {
            #ifdef FASTER_SOUND_HACK_NON_SILENCE
                                if ( ( buf->echo == !!echo_phase ) )
            #else
                                if ( buf->non_silent() && ( buf->echo == !!echo_phase ) )
            #endif
                                {
                                        stereo_fixed_t* BLIP_RESTRICT out = (stereo_fixed_t*) &echo [echo_pos];
                                        int const bass = BLIP_READER_BASS( *buf );
                                        BLIP_READER_BEGIN( in, *buf );
                                        BLIP_READER_ADJ_( in, mixer_samples_read );
                                        fixed_t const vol_0 = buf->vol [0];
                                        fixed_t const vol_1 = buf->vol [1];

                                        int count = unsigned (echo_size - echo_pos) / stereo;
                                        int remain = pair_count;
                                        if ( count > remain )
                                                count = remain;
                                        do
                                        {
                                                remain -= count;
                                                BLIP_READER_ADJ_( in, count );

                                                out += count;
                                                int offset = -count;
                                                do
                                                {
                                                        fixed_t s = BLIP_READER_READ( in );
                                                        BLIP_READER_NEXT_IDX_( in, bass, offset );

                                                        out [offset] [0] += s * vol_0;
                                                        out [offset] [1] += s * vol_1;
                                                }
                                                while ( ++offset );

                                                out = (stereo_fixed_t*) echo.begin();
                                                count = remain;
                                        }
                                        while ( remain );

                                        BLIP_READER_END( in, *buf );
                                }
                                buf++;
                        }
                        while ( --bufs_remain );
                }

                // add echo
                if ( echo_phase && !no_echo )
                {
                        fixed_t const feedback = s.feedback;
                        fixed_t const treble   = s.treble;

                        int i = 1;
                        do
                        {
                                fixed_t low_pass = s.low_pass [i];

                                fixed_t* echo_end = &echo [echo_size + i];
                                fixed_t const* BLIP_RESTRICT in_pos = &echo [echo_pos + i];
                                blargg_long out_offset = echo_pos + i + s.delay [i];
                                if ( out_offset >= echo_size )
                                        out_offset -= echo_size;
                                fixed_t* BLIP_RESTRICT out_pos = &echo [out_offset];

                                // break into up to three chunks to avoid having to handle wrap-around
                                // in middle of core loop
                                int remain = pair_count;
                                do
                                {
                                        fixed_t const* pos = in_pos;
                                        if ( pos < out_pos )
                                                pos = out_pos;
                                        int count = blargg_ulong ((char*) echo_end - (char const*) pos) /
                                                        unsigned (stereo * sizeof (fixed_t));
                                        if ( count > remain )
                                                count = remain;
                                        remain -= count;

                                        in_pos  += count * stereo;
                                        out_pos += count * stereo;
                                        int offset = -count;
                                        do
                                        {
                                                low_pass += FROM_FIXED( in_pos [offset * stereo] - low_pass ) * treble;
                                                out_pos [offset * stereo] = FROM_FIXED( low_pass ) * feedback;
                                        }
                                        while ( ++offset );

                                        if (  in_pos >= echo_end )  in_pos -= echo_size;
                                        if ( out_pos >= echo_end ) out_pos -= echo_size;
                                }
                                while ( remain );

                                s.low_pass [i] = low_pass;
                        }
                        while ( --i >= 0 );
                }
        }
        while ( --echo_phase >= 0 );

        // clamp to 16 bits
        {
                stereo_fixed_t const* BLIP_RESTRICT in = (stereo_fixed_t*) &echo [echo_pos];
                typedef int16_t stereo_blip_sample_t [stereo];
                stereo_blip_sample_t* BLIP_RESTRICT out = (stereo_blip_sample_t*) out_;
                int count = unsigned (echo_size - echo_pos) / (unsigned) stereo;
                int remain = pair_count;
                if ( count > remain )
                        count = remain;
                do
                {
                        remain -= count;
                        in  += count;
                        out += count;
                        int offset = -count;
                        do
                        {
                                fixed_t in_0 = FROM_FIXED( in [offset] [0] );
                                fixed_t in_1 = FROM_FIXED( in [offset] [1] );

                                BLIP_CLAMP( in_0, in_0 );
                                out [offset] [0] = (int16_t) in_0;

                                BLIP_CLAMP( in_1, in_1 );
                                out [offset] [1] = (int16_t) in_1;
                        }
                        while ( ++offset );

                        in = (stereo_fixed_t*) echo.begin();
                        count = remain;
                }
                while ( remain );
        }
}
#endif