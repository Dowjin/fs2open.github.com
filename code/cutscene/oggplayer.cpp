/*
 * $Logfile: /Freespace2/code/cutscene/oggplayer.cpp $
 * $Revision: 2.1 $
 * $Date: 2007-01-07 12:29:43 $
 * $Author: taylor $
 *
 * movie player code
 * 
 * $Log: not supported by cvs2svn $
 * Revision 1.0  2006/12/25 21:44:11  taylor
 *
 * $NoKeywords: $
 *
 */

// This player code is based largely on the player example in the libtheora package
// and so they get a fair bit of credit for this code, even if in ideas/comments only


#ifdef SCP_UNIX
#include <sys/time.h>
#include <errno.h>
#endif

#include "globalincs/pstypes.h"
#include "cfile/cfile.h"
#include "graphics/gropengl.h"
#include "graphics/gropengltexture.h"
#include "graphics/gropenglextension.h"
#include "graphics/2d.h"
#include "io/key.h"
#include "osapi/osapi.h"
#include "sound/sound.h"
#include "sound/ds.h"
#include "bmpman/bmpman.h"
#include "cutscene/oggplayer.h"
#include "io/timer.h"

#include "theora/theora.h"
#include "vorbis/codec.h"


extern int Cmdline_noscalevid;

static int hp2, wp2;
static int video_inited = 0;
static int scale_video = 0;
static int playing = 1;
static ubyte *pixelbuf = NULL;
static uint g_screenWidth = 0;
static uint g_screenHeight = 0;
static int g_screenX = 0;
static int g_screenY = 0;

static GLuint GLtex = 0;
static GLint gl_screenYH = 0;
static GLint gl_screenXW = 0;
static GLfloat gl_screenU = 0;
static GLfloat gl_screenV = 0;

// video externs from API graphics functions
extern void opengl_tcache_get_adjusted_texture_size(int w_in, int h_in, int *w_out, int *h_out);

static int timer_started = 0;
static longlong base_time = -1;
#ifdef SCP_UNIX
static struct timeval timer_expire = { 0, 0 };
#else
static int timer_expire;
#endif

static int audio_inited = 0;
static int audio_buffer_tail = 0;
static ALuint audio_sid = 0;
#define OGG_AUDIO_BUFFERS	15
static ALuint audio_buffers[OGG_AUDIO_BUFFERS];

// single audio fragment audio buffering
static int audiobuf_fill = 0;
static short *audiobuf = NULL;
static longlong audiobuf_granulepos = 0; // time position of last sample
static int audiofd_fragsize = 0;

// single frame video buffering
static longlong videobuf_granulepos = -1;
static double videobuf_time = 0;


// -----------------------------------------------------------------------------
//  Utility items
//

// helper; just grab some more compressed bitstream and sync it for page extraction
static int OGG_buffer_data(THEORAFILE *movie)
{
	char *buffer = ogg_sync_buffer(&movie->osyncstate, 4096);
	int bytes = cfread(buffer, 1, 4096, movie->cfp);

	ogg_sync_wrote(&movie->osyncstate, bytes);

	return bytes;
}


// helper: push a page into the appropriate steam
// this can be done blindly; a stream won't accept a page that doesn't belong to it
static void OGG_queue_page(THEORAFILE *movie)
{
	Assert( movie->theora_p );

	ogg_stream_pagein(&movie->t_osstate, &movie->opage);

	if (movie->vorbis_p)
		ogg_stream_pagein(&movie->v_osstate, &movie->opage);
}

// get relative time since beginning playback, compensating for A/V drift
static double OGG_get_time()
{
#ifdef SCP_UNIX
	longlong now;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	now = tv.tv_sec * 1000 + tv.tv_usec / 1000;

	if (base_time == -1)
		base_time = now;

	return (now - base_time) * 0.001;
#else
	if (base_time == -1)
		base_time = timer_get_milliseconds();

	return (timer_get_milliseconds() - base_time) * 0.001;
#endif
}

static void OGG_timer_init()
{
#if SCP_UNIX
	int nsec = 0;

	gettimeofday(&timer_expire, NULL);

	timer_expire.tv_usec += (int)((videobuf_time - OGG_get_time()) * 1000000.0);

	if (timer_expire.tv_usec > 1000000) {
		nsec = timer_expire.tv_usec / 1000000;
		timer_expire.tv_sec += nsec;
		timer_expire.tv_usec -= nsec * 1000000;
	}
#else
	timer_expire = timer_get_microseconds();
	timer_expire += (int)((videobuf_time - OGG_get_time()) * 1000000.0);
#endif

	timer_started = 1;
}

static void OGG_timer_do_wait()
{
	if (!timer_started)
		OGG_timer_init();

#if SCP_UNIX
	int nsec = 0;
	struct timespec ts, tsRem;
	struct timeval tv;

	gettimeofday(&tv, NULL);

	if (tv.tv_sec > timer_expire.tv_sec)
		goto end;

	if ( (tv.tv_sec == timer_expire.tv_sec) && (tv.tv_usec >= timer_expire.tv_usec) )
		goto end;

	ts.tv_sec = timer_expire.tv_sec - tv.tv_sec;
	ts.tv_nsec = 1000 * (timer_expire.tv_usec - tv.tv_usec);

	if (ts.tv_nsec < 0) {
		ts.tv_nsec += 1000000000UL;
		--ts.tv_sec;
	}

	if ( (nanosleep(&ts, &tsRem) == -1) && (errno == EINTR) ) {
		// so we got an error that was a signal interupt, try to sleep again with remainder of time
		if ( (nanosleep(&tsRem, NULL) == -1) && (errno == EINTR) ) {
			mprintf(("MVE: Timer error! Aborting movie playback!\n"));
			return;
		}
	}

end:
    timer_expire.tv_usec += (int)((videobuf_time - OGG_get_time()) * 1000000.0);

    if (timer_expire.tv_usec > 1000000) {
        nsec = timer_expire.tv_usec / 1000000;
        timer_expire.tv_sec += nsec;
        timer_expire.tv_usec -= nsec * 1000000;
    }
#else
	int tv, ts, ts2;

	tv = timer_get_microseconds();

	if (tv > timer_expire)
		goto end;

	ts = timer_expire - tv;

	ts2 = ts/1000;

	Sleep(ts2);

end:
	timer_expire += (int)((videobuf_time - OGG_get_time()) * 1000000.0);
#endif
}

//
//  End Utility items
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  That wonderful Audio
//

static void OGG_audio_init(vorbis_info *vinfo)
{
	if (audio_inited)
		return;

	Assert( vinfo != NULL );

	audio_inited = 1;

	OpenAL_ErrorCheck( alGenSources(1, &audio_sid), return );
	OpenAL_ErrorPrint( alSourcef(audio_sid, AL_GAIN, 1.0f) );

	memset( &audio_buffers, 0, sizeof(ALuint) * OGG_AUDIO_BUFFERS );

	audiofd_fragsize = (((vinfo->channels * 16) / 8) * vinfo->rate);

	audiobuf = (short *) vm_malloc(audiofd_fragsize);
}

static void OGG_audio_close()
{
	if ( !audio_inited )
		return;

	audio_inited = 0;

	ALint p = 0;

	OpenAL_ErrorPrint( alSourceStop(audio_sid) );
	OpenAL_ErrorPrint( alGetSourcei(audio_sid, AL_BUFFERS_PROCESSED, &p) );
	OpenAL_ErrorPrint( alSourceUnqueueBuffers(audio_sid, p, audio_buffers) );
	OpenAL_ErrorPrint( alDeleteSources(1, &audio_sid) );

	for (int i = 0; i < OGG_AUDIO_BUFFERS; i++) {
		// make sure that the buffer is real before trying to delete, it could crash for some otherwise
		if ( (audio_buffers[i] != 0) && alIsBuffer(audio_buffers[i]) ) {
			OpenAL_ErrorPrint( alDeleteBuffers(1, &audio_buffers[i]) );
		}
	}

	audio_sid = 0;
	audio_buffer_tail = 0;

	audiobuf_fill = 0;
	audiobuf_granulepos = 0;
	audiofd_fragsize = 0;

	if (audiobuf != NULL) {
		vm_free(audiobuf);
		audiobuf = NULL;
	}
}

static void OGG_audio_write(vorbis_info *vorbis, bool *ready)
{
	ALint status, queued, processed = 0;
	ALuint bid = 0;

	if ( !audio_inited || !(*ready) )
		return;

	OpenAL_ErrorCheck( alGetSourcei(audio_sid, AL_BUFFERS_PROCESSED, &processed), return );

	while (processed-- > 2)
		OpenAL_ErrorPrint( alSourceUnqueueBuffers(audio_sid, 1, &bid) );

	if ( !(*ready) )
		return;

	OpenAL_ErrorCheck( alGetSourcei(audio_sid, AL_BUFFERS_QUEUED, &queued), return );

	if ( audiobuf_fill && (queued < OGG_AUDIO_BUFFERS) ) {
		if ( !audio_buffers[audio_buffer_tail] )
			OpenAL_ErrorCheck( alGenBuffers(1, &audio_buffers[audio_buffer_tail]), return );

		ALenum format = (vorbis->channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;

		OpenAL_ErrorCheck( alBufferData(audio_buffers[audio_buffer_tail], format, audiobuf, audiobuf_fill, vorbis->rate), return );
	    
		OpenAL_ErrorCheck( alSourceQueueBuffers(audio_sid, 1, &audio_buffers[audio_buffer_tail]), return );

		if (++audio_buffer_tail == OGG_AUDIO_BUFFERS)
			audio_buffer_tail = 0;

		audiobuf_fill = 0;
		*ready = false;
	} else {
	//	mprintf(("Theora WARN: Buffer overrun: Queue full\n"));
	}

	OpenAL_ErrorCheck( alGetSourcei(audio_sid, AL_SOURCE_STATE, &status), return );
	
	OpenAL_ErrorCheck( alGetSourcei(audio_sid, AL_BUFFERS_QUEUED, &queued), return );

	if ( (status != AL_PLAYING) && (queued > 0) )
		OpenAL_ErrorPrint( alSourcePlay(audio_sid) );
}

//
//  End Audio stuff
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  Video related stuff
//

static void OGG_video_init(theora_info *tinfo)
{
	float scale_by = 0.0f;

	if (video_inited)
		return;

	Assert( tinfo != NULL );

	g_screenWidth = tinfo->frame_width;
	g_screenHeight = tinfo->frame_height;


	pixelbuf = (ubyte *) vm_malloc_q(g_screenWidth * g_screenHeight * 3);

	if (pixelbuf == NULL) {
		nprintf(("MOVIE", "ERROR: Can't allocate memory for pixelbuf"));
		video_inited = 1;
		return;
	}

	memset( pixelbuf, 0, g_screenWidth * g_screenHeight * 3 );

	if (gr_screen.mode == GR_OPENGL) {
		// set the appropriate texture target
		if ( Is_Extension_Enabled(OGL_ARB_TEXTURE_RECTANGLE) && !Is_Extension_Enabled(OGL_ARB_TEXTURE_NON_POWER_OF_TWO) )
			opengl_set_texture_target( GL_TEXTURE_RECTANGLE_ARB );

		opengl_tcache_get_adjusted_texture_size(g_screenWidth, g_screenHeight, &wp2, &hp2);

		glGenTextures(1, &GLtex);

		Assert( GLtex != 0 );

		if ( GLtex == 0 ) {
			nprintf(("MOVIE", "ERROR: Can't create a GL texture"));
			video_inited = 1;
			return;
		}

		// disable everything but what we need
		opengl_switch_arb(-1, 0);
		opengl_switch_arb(0, 1);

		gr_set_lighting(false, false);
	
		glBindTexture(GL_texture_target, GLtex);
	
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthFunc(GL_ALWAYS);
		glDepthMask(GL_FALSE);
		glDisable(GL_DEPTH_TEST);
		glTexParameteri(GL_texture_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_texture_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_texture_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_texture_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		float screen_ratio = (float)gr_screen.max_w / (float)gr_screen.max_h;
		float movie_ratio = (float)g_screenWidth / (float)g_screenHeight;

		if (screen_ratio > movie_ratio)
			scale_by = (float)gr_screen.max_h / (float)g_screenHeight;
		else
			scale_by = (float)gr_screen.max_w / (float)g_screenWidth;

		// don't bother setting anything if we aren't going to need it
		if (!Cmdline_noscalevid && (scale_by != 1.0f)) {
			glMatrixMode(GL_MODELVIEW);
			glPushMatrix();
			glLoadIdentity();

			glScalef( scale_by, scale_by, -1.0f );
			scale_video = 1;
		}

		// NOTE: using NULL instead of pixelbuf crashes some drivers, but then so does pixelbuf
		glTexImage2D(GL_texture_target, 0, GL_RGB8, wp2, hp2, 0, GL_BGR, GL_UNSIGNED_BYTE, NULL);
	}
#ifndef NO_DIRECT3D
	else if (gr_screen.mode == GR_DIRECT3D) {
		// TODO: is there a fast way to do frames in D3D too?
	}
#endif

	if (scale_video) {
		g_screenX = ((fl2i(gr_screen.max_w / scale_by + 0.5f) - g_screenWidth) / 2);
		g_screenY = ((fl2i(gr_screen.max_h / scale_by + 0.5f) - g_screenHeight) / 2);
	} else {
		// centers on 1024x768, fills on 640x480
		g_screenX = ((gr_screen.max_w - g_screenWidth) / 2);
		g_screenY = ((gr_screen.max_h - g_screenHeight) / 2);
	}

	// set additional values for screen width/height and UV coords
	if (gr_screen.mode == GR_OPENGL) {
		gl_screenYH = g_screenY + g_screenHeight;
		gl_screenXW = g_screenX + g_screenWidth;

		if (GL_texture_target == GL_TEXTURE_RECTANGLE_ARB) {
			gl_screenU = (GLfloat)wp2;
			gl_screenV = (GLfloat)hp2;
		} else {
			gl_screenU = i2fl(g_screenWidth) / i2fl(wp2);
			gl_screenV = i2fl(g_screenHeight) / i2fl(hp2);
		}
	}

	video_inited = 1;
}

static void OGG_video_close()
{
	if (!video_inited)
		return;

	if (gr_screen.mode == GR_OPENGL) {
		if (scale_video) {
			glMatrixMode(GL_MODELVIEW);
			glPopMatrix();
		}

		opengl_switch_arb(-1, 0);

		glBindTexture(GL_texture_target, 0);
		glDeleteTextures(1, &GLtex);
		GLtex = 0;

		opengl_set_texture_target();

		glEnable(GL_DEPTH_TEST);
	}

	if (pixelbuf != NULL) {
		vm_free(pixelbuf);
		pixelbuf = NULL;
	}

	video_inited = 0;
	scale_video = 0;
	pixelbuf = NULL;
	g_screenWidth = 0;
	g_screenHeight = 0;
	g_screenX = 0;
	g_screenY = 0;

	GLtex = 0;
	gl_screenYH = 0;
	gl_screenXW = 0;
	gl_screenU = 0;
	gl_screenV = 0;
}

static void convert_YUV_to_RGB(yuv_buffer *yuv)
{
	int Y1, Y2, U, V;
	int R = 0, G = 0, B = 0;
	int C, D, E;
	uint x, y;
	uint width_2 = g_screenWidth/2;

	ubyte *pix = &pixelbuf[0];

	ubyte *y_ptr = (ubyte *)yuv->y;
	ubyte *u_ptr = (ubyte *)yuv->u;
	ubyte *v_ptr = (ubyte *)yuv->v;

	for (y = 0; y < g_screenHeight; y++) {
		for (x = 0; x < width_2; x++) {
			// we need two pixels of Y
			Y1 = *y_ptr; y_ptr++;
			Y2 = *y_ptr; y_ptr++;

			// only one pixel of U and V (half the size of Y)
			U = u_ptr[x];
			V = v_ptr[x];

			D = (U - 128);
			E = (V - 128);

			// first pixel
			C = (Y1 - 16) * 298;

			R = ((C           + 409 * E + 128) >> 8);
			G = ((C - 100 * D - 208 * E + 128) >> 8);
			B = ((C + 516 * D           + 128) >> 8);

			CLAMP(R, 0, 255);
			CLAMP(G, 0, 255);
			CLAMP(B, 0, 255);

			*pix++ = (ubyte)B;
			*pix++ = (ubyte)G;
			*pix++ = (ubyte)R;

			// second pixel (U and V values are resused)
			C = (Y2 - 16) * 298;

			R = ((C           + 409 * E + 128) >> 8);
			G = ((C - 100 * D - 208 * E + 128) >> 8);
			B = ((C + 516 * D           + 128) >> 8);

			CLAMP(R, 0, 255);
			CLAMP(G, 0, 255);
			CLAMP(B, 0, 255);

			*pix++ = (ubyte)B;
			*pix++ = (ubyte)G;
			*pix++ = (ubyte)R;
		}

		y_ptr += (yuv->y_stride - yuv->y_width);

		// u and v have to be done every other row (it's a 2x2 block)
		if (y % 2) {
			u_ptr += yuv->uv_stride;
			v_ptr += yuv->uv_stride;
		}
	}
}

static void OGG_video_draw(theora_state *tstate)
{
	yuv_buffer yuv;

	theora_decode_YUVout(tstate, &yuv);
	convert_YUV_to_RGB(&yuv);

	if (gr_screen.mode == GR_OPENGL) {
		glBindTexture(GL_texture_target, GLtex);

		glTexSubImage2D(GL_texture_target, 0, 0, 0, g_screenWidth, g_screenHeight, GL_BGR, GL_UNSIGNED_BYTE, pixelbuf);

		glBegin(GL_QUADS);
			glTexCoord2f(0, 0);
				glVertex2i(g_screenX, g_screenY);

			glTexCoord2f(0, gl_screenV);
				glVertex2i(g_screenX, gl_screenYH);

			glTexCoord2f(gl_screenU, gl_screenV);
				glVertex2i(gl_screenXW, gl_screenYH);

			glTexCoord2f(gl_screenU, 0);
				glVertex2i(gl_screenXW, g_screenY);
		glEnd();
	} else {
		// TODO:  FIXME!!!!
	}

	gr_flip();
	os_poll();

	int k = key_inkey();
	switch (k) {
		case KEY_ESC:
		case KEY_ENTER:
		case KEY_SPACEBAR:
			playing = 0;
	}
}

//
//  End Video related stuff
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
//  The global stuff...
//

// close out a theora movie and free it's memory
void theora_close(THEORAFILE *movie)
{
	Assert( movie != NULL );

	timer_started = 0;
	base_time = -1;

	videobuf_granulepos = -1;
	videobuf_time = 0;

	if (movie->vorbis_p) {
		ogg_stream_clear(&movie->v_osstate);

		vorbis_block_clear(&movie->vblock);
		vorbis_dsp_clear(&movie->vstate);
		vorbis_comment_clear(&movie->vcomment);
		vorbis_info_clear(&movie->vinfo);
	}

	if (movie->theora_p) {
		ogg_stream_clear(&movie->t_osstate);

		theora_clear(&movie->tstate);
		theora_comment_clear(&movie->tcomment);
		theora_info_clear(&movie->tinfo);
	}
 
	ogg_sync_clear(&movie->osyncstate);

	// free the stream
	if (movie->cfp)
		cfclose(movie->cfp);

	// free the struct
	vm_free(movie);

	playing = 1;
}

// opens a theora file, verifies we can use it, initializes everything needed
// to decode and use the file
THEORAFILE *theora_open(char *filename)
{
	char lower_name[MAX_FILENAME_LEN];
	THEORAFILE *movie;
	int stateflag = 0;

	
	// create the file
	movie = (THEORAFILE *) vm_malloc_q( sizeof(THEORAFILE) );

	if (movie == NULL)
		return NULL;

	memset( movie, 0, sizeof(THEORAFILE) );

	// lower case filename for checking
	memset( lower_name, 0, sizeof(lower_name) );
	strncpy( lower_name, filename, strlen(filename) );
	strlwr(lower_name);
	
	char *p = strchr( lower_name, '.' );
	if ( p ) *p = 0;

	SAFE_STRCAT( lower_name, ".ogg", sizeof(lower_name) );

	// NOTE: Because the .ogg extension is used for both movies and sounds, we have to
	//       be a bit more specific about our search locations, so we look only in the
	//       two places that Theora movies might exist.
	movie->cfp = cfopen(lower_name, "rb", CFILE_NORMAL, CF_TYPE_ROOT);

	if (movie->cfp == NULL)
		movie->cfp = cfopen(lower_name, "rb", CFILE_NORMAL, CF_TYPE_MOVIES);

	if (movie->cfp == NULL) {
		mprintf(("Theora ERROR:  Unable to find and open movie file named '%s'\n", lower_name));
		goto Error;
	}

	// start up Ogg stream synchronization layer
	ogg_sync_init(&movie->osyncstate);

	// init supporting Vorbis structures needed in header parsing
	vorbis_info_init(&movie->vinfo);
	vorbis_comment_init(&movie->vcomment);

	// init supporting Theora structures needed in header parsing
	theora_comment_init(&movie->tcomment);
	theora_info_init(&movie->tinfo);

	// ogg file open so parse the headers
	// we are only interested in Vorbis/Theora streams
	while ( !stateflag ) {
		int ret = OGG_buffer_data(movie);

		if (ret == 0)
			break;

		while (ogg_sync_pageout(&movie->osyncstate, &movie->opage) > 0) {
			ogg_stream_state test;

			// is this a mandated initial header? If not, stop parsing
			if ( !ogg_page_bos(&movie->opage) ) {
  				// don't leak the page; get it into the appropriate stream
				OGG_queue_page(movie);
				stateflag = 1;
				break;
			}

			ogg_stream_init(&test, ogg_page_serialno(&movie->opage));
			ogg_stream_pagein(&test, &movie->opage);
			ogg_stream_packetout(&test, &movie->opacket);

			// identify the codec: try theora
			if ( !movie->theora_p && (theora_decode_header(&movie->tinfo, &movie->tcomment, &movie->opacket) >= 0) ) {
				// it is theora
				memcpy( &movie->t_osstate, &test, sizeof(test) );
				movie->theora_p = 1;
			} else if ( !movie->vorbis_p && (vorbis_synthesis_headerin(&movie->vinfo, &movie->vcomment, &movie->opacket) >= 0) ) {
				// it is vorbis
				memcpy( &movie->v_osstate, &test, sizeof(test) );
				movie->vorbis_p = 1;
			} else {
				// whatever it is, we don't care about it
				ogg_stream_clear(&test);
			}
		}
		// fall through to non-bos page parsing
	}

	// if we don't have usable video then bail out now
	if ( !movie->theora_p ) {
		mprintf(("Theora ERROR: Unable to find video data in '%s'\n", lower_name));
		goto Error;
	}

	// go ahead and do some audio stream error checking...
	if ( (movie->vinfo.channels < 0) || (movie->vinfo.channels > 2) ) {
		mprintf(("Theora ERROR:  Unsupported number of audio channels!\n"));
		movie->vorbis_p = 0;
	}

	// if we don't have usable audio then close out it's partial setup and just use the video
	if ( !Sound_enabled || !movie->vorbis_p ) {
		vorbis_info_clear(&movie->vinfo);
		vorbis_comment_clear(&movie->vcomment);
		movie->vorbis_p = 0;
	}

	// we're expecting more header packets.
	while ( (movie->theora_p < 3) || (movie->vorbis_p && (movie->vorbis_p < 3)) ) {
		int ret;

		// look for further theora headers
		while ( (movie->theora_p < 3) && (ret = ogg_stream_packetout(&movie->t_osstate, &movie->opacket)) ){ 
			if ( (ret < 0) || theora_decode_header(&movie->tinfo, &movie->tcomment, &movie->opacket) ) {
				mprintf(("Theora ERROR:  Error parsing Theora stream headers on '%s'!  Corrupt stream?\n", lower_name));
				goto Error;
			}

			if (++movie->theora_p == 3)
				break;
		}

		// look for more vorbis header packets
		while ( movie->vorbis_p && (movie->vorbis_p < 3) && (ret = ogg_stream_packetout(&movie->v_osstate, &movie->opacket)) ) {
			if ( (ret < 0) || vorbis_synthesis_headerin(&movie->vinfo, &movie->vcomment, &movie->opacket) ) {
				mprintf(("Theora ERROR:  Error parsing Vorbis stream headers on '%s'!  Corrupt stream?\n", lower_name));
				goto Error;
			}

			if (++movie->vorbis_p == 3)
				break;
		}

		// The header pages/packets will arrive before anything else we care about, or the stream is not obeying spec

		if ( ogg_sync_pageout(&movie->osyncstate, &movie->opage) > 0 ) {
			OGG_queue_page(movie); // demux into the appropriate stream
		} else {
			int ret = OGG_buffer_data(movie); // someone needs more data

			if ( ret == 0 ) {
				mprintf(("Theora ERROR:  End of file while searching for codec headers in '%s'!\n", lower_name));
				goto Error;
			}
		}
	}

	// initialize video decoder
	theora_decode_init(&movie->tstate, &movie->tinfo);

	// and now for video stream error checking...
	if (movie->tinfo.pixelformat != OC_PF_420) {
		mprintf(("Theora ERROR:  Only the yuv420p chroma is supported!\n"));
		goto Error;
	}

	if (movie->tinfo.offset_x || movie->tinfo.offset_y) {
		mprintf(("Theora ERROR:  Player does not support frame offsets!\n"));
		goto Error;
	}

	if ( (movie->tinfo.frame_width > 640) || (movie->tinfo.frame_height > 480) ) {
		mprintf(("Theora ERROR:  Movies must be no larger than 640x480!\n"));
		goto Error;
	}

	// initialize audio decoder, if there is audio
	if (movie->vorbis_p) {
		vorbis_synthesis_init(&movie->vstate, &movie->vinfo);
		vorbis_block_init(&movie->vstate, &movie->vblock);
	}

	return movie;


Error:
	theora_close(movie);

	return NULL;
}

// play that funky music... err, movie!
void theora_play(THEORAFILE *movie)
{
	int i, j;
	bool stateflag = false;
	bool videobuf_ready = false;
	bool audiobuf_ready = false;
	double last_time = 0.0;	// for frame skipper

	if ( (movie == NULL) || !movie->theora_p )
		return;

	// open audio
	if (movie->vorbis_p)
		OGG_audio_init(&movie->vinfo);

	// open video
	OGG_video_init(&movie->tinfo);

	// on to the main decode loop.	We assume in this example that audio
	// and video start roughly together, and don't begin playback until
	// we have a start frame for both.	This is not necessarily a valid
	// assumption in Ogg A/V streams! It will always be true of the
	// example_encoder (and most streams) though.

	while (playing) {
		// we want a video and audio frame ready to go at all times.	If
		// we have to buffer incoming, buffer the compressed data (ie, let
		// ogg do the buffering)
		while ( movie->vorbis_p && !audiobuf_ready ) {
			int ret;
			float **pcm;

			// if there's pending, decoded audio, grab it
			if ( (ret = vorbis_synthesis_pcmout(&movie->vstate, &pcm)) > 0 ) {
				int count = (audiobuf_fill / 2);
				int maxsamples = (audiofd_fragsize - audiobuf_fill) / 2 / movie->vinfo.channels;

				for (i = 0; (i < ret) && (i < maxsamples); i++) {
					for (j = 0; j < movie->vinfo.channels; j++) {
						int val = fl2i(pcm[j][i] * 32767.0f + 0.5f);

						if (val > 32767)
							val = 32767;

						if (val < -32768)
							val = -32768;

						audiobuf[count++] = val;
					}
				}

				vorbis_synthesis_read(&movie->vstate, i);

				audiobuf_fill += (i * movie->vinfo.channels * 2);

				if (audiobuf_fill == audiofd_fragsize)
					audiobuf_ready = true;

				if (movie->vstate.granulepos >= 0)
					audiobuf_granulepos = movie->vstate.granulepos - ret + i;
				else
					audiobuf_granulepos += i;
			} else {
				// no pending audio; is there a pending packet to decode?
				if ( ogg_stream_packetout(&movie->v_osstate, &movie->opacket) > 0 ) {
					if ( vorbis_synthesis(&movie->vblock, &movie->opacket) == 0 )
						vorbis_synthesis_blockin(&movie->vstate, &movie->vblock);
				}
				// we need more data; break out to suck in another page
				else {
					break;
				}
			}
		}

		while ( !videobuf_ready ) {
			// theora is one in, one out...
			if ( ogg_stream_packetout(&movie->t_osstate, &movie->opacket) <= 0 )
				break;

			theora_decode_packetin(&movie->tstate, &movie->opacket);
				
			videobuf_time = theora_granule_time(&movie->tstate, movie->tstate.granulepos);

			double now_time = OGG_get_time();
			double delay = videobuf_time - OGG_get_time();

			// if we are running slow then skip the frame, otherwise to go into ready mode
			if ( (delay >= 0.0) || (now_time - last_time >= 1.0) )
				videobuf_ready = true;
		}

		if ( !videobuf_ready && (movie->vorbis_p && !audiobuf_ready) && cfeof(movie->cfp) )
			break;

		if ( !videobuf_ready || (movie->vorbis_p && !audiobuf_ready) ) {
			// no data yet for somebody.	Grab another page
			OGG_buffer_data(movie);

			while (ogg_sync_pageout(&movie->osyncstate, &movie->opage) > 0)
				OGG_queue_page(movie);
		}

		// If playback has begun, top audio buffer off immediately.
		if ( stateflag )
			OGG_audio_write(&movie->vinfo, &audiobuf_ready);

		// are we at or past time for this video frame?
		if ( stateflag && videobuf_ready && (videobuf_time <= OGG_get_time()) ) {
			OGG_video_draw(&movie->tstate);
			last_time = OGG_get_time();
			videobuf_ready = false;
		}

		// if our buffers either don't exist or are ready to go, we can begin playback
		if ( videobuf_ready && (audiobuf_ready || !movie->vorbis_p) )
			stateflag = true;

		// bail if we run into eof
	//	if ( cfeof(movie->cfp) )
	//		break;

		if (videobuf_ready)
			OGG_timer_do_wait();
	}

	// tear it all down
	OGG_audio_close();
	OGG_video_close();
}