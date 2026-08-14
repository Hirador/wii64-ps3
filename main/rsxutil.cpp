#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"

#define GCM_LABEL_INDEX		255

videoResolution res;
gcmContextData *context = NULL;

u32 curr_fb = 0;
u32 first_fb = 1;

u32 display_width;
u32 display_height;

u32 depth_pitch;
u32 depth_offset;
u32 *depth_buffer;

u32 color_pitch;
u32 color_offset[2];
u32 *color_buffer[2];

static u32 sLabelVal = 1;

#ifdef WII64_FRAME_PROFILE
// Instrumentation for the flip stall. profWaitIters distinguishes the two
// possible causes of a multi-second swap: a large iteration count means the
// RSX genuinely is not completing the flip, while a count of one or two means
// the loop is fine and usleep() itself is sleeping far longer than asked.
extern "C" {
unsigned int profWaitIters = 0;
unsigned int profSysUtilTicks = 0;
unsigned int profLabelTicks = 0;
unsigned int profLabelIters = 0;
unsigned int profVideoInfo[6] = {0,0,0,0,0,0};
// put/get/ref just after flush, get at +200 polls, then put/get at the end.
unsigned int profCtrl[6] = {0,0,0,0,0,0};
}

static inline unsigned int rsxutil_tick(void)
{
	unsigned int r;
	__asm__ __volatile__ ("mftb %0" : "=r" (r));
	return r;
}
#endif

static void waitFinish()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);

	rsxFlushBuffer(context);

	while(*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX)!=sLabelVal)
		usleep(30);

	++sLabelVal;
}

static void waitRSXIdle()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);
	rsxSetWaitLabel(context,GCM_LABEL_INDEX,sLabelVal);

	++sLabelVal;

	waitFinish();
}

void setRenderTarget(u32 index)
{
	gcmSurface sf;

	sf.colorFormat		= GCM_TF_COLOR_X8R8G8B8;
	sf.colorTarget		= GCM_TF_TARGET_0;
	sf.colorLocation[0]	= GCM_LOCATION_RSX;
	sf.colorOffset[0]	= color_offset[index];
	sf.colorPitch[0]	= color_pitch;

	sf.colorLocation[1]	= GCM_LOCATION_RSX;
	sf.colorLocation[2]	= GCM_LOCATION_RSX;
	sf.colorLocation[3]	= GCM_LOCATION_RSX;
	sf.colorOffset[1]	= 0;
	sf.colorOffset[2]	= 0;
	sf.colorOffset[3]	= 0;
	sf.colorPitch[1]	= 64;
	sf.colorPitch[2]	= 64;
	sf.colorPitch[3]	= 64;

	sf.depthFormat		= GCM_TF_ZETA_Z16;
	sf.depthLocation	= GCM_LOCATION_RSX;
	sf.depthOffset		= depth_offset;
	sf.depthPitch		= depth_pitch;

	sf.type				= GCM_TF_TYPE_LINEAR;
	sf.antiAlias		= GCM_TF_CENTER_1;

	sf.width			= display_width;
	sf.height			= display_height;
	sf.x				= 0;
	sf.y				= 0;

	rsxSetSurface(context,&sf);
}

void init_screen(void *host_addr,u32 size)
{
	context = rsxInit(CB_SIZE,size,host_addr);

	videoState state;
	videoGetState(0,0,&state);

	videoGetResolution(state.displayMode.resolution,&res);

	videoConfiguration vconfig;
	memset(&vconfig,0,sizeof(videoConfiguration));

	vconfig.resolution = state.displayMode.resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width*sizeof(u32);
	// The struct is memset to zero above and this was never assigned, leaving
	// aspect at 0 where every PSL1GHT sample passes the display's own aspect.
	vconfig.aspect = state.displayMode.aspect;

	waitRSXIdle();

	videoConfigure(0,&vconfig,NULL,0);
	videoGetState(0,0,&state);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	display_width = res.width;
	display_height = res.height;

	color_pitch = display_width*sizeof(u32);
	color_buffer[0] = (u32*)rsxMemalign(64,(display_height*color_pitch));
	color_buffer[1] = (u32*)rsxMemalign(64,(display_height*color_pitch));

	rsxAddressToOffset(color_buffer[0],&color_offset[0]);
	rsxAddressToOffset(color_buffer[1],&color_offset[1]);

	gcmSetDisplayBuffer(0,color_offset[0],color_pitch,display_width,display_height);
	gcmSetDisplayBuffer(1,color_offset[1],color_pitch,display_width,display_height);

	depth_pitch = display_width*sizeof(u32);
	depth_buffer = (u32*)rsxMemalign(64,(display_height*depth_pitch)*2);
	rsxAddressToOffset(depth_buffer,&depth_offset);

	// The reference initScreen() ends here; without it the first waitflip()
	// has no defined status to wait on.
	gcmResetFlipStatus();

#ifdef WII64_FRAME_PROFILE
	profVideoInfo[0] = display_width;
	profVideoInfo[1] = display_height;
	profVideoInfo[2] = state.displayMode.resolution;
	profVideoInfo[3] = state.displayMode.aspect;
	profVideoInfo[4] = state.displayMode.refreshRates;
	profVideoInfo[5] = state.state;
#endif
}

void waitflip()
{
	while(gcmGetFlipStatus()!=0) {
#ifdef WII64_FRAME_PROFILE
		++profWaitIters;
#endif
		usleep(200);
	}
	gcmResetFlipStatus();
}

void flip()
{
	// The sysutil callback queue has to be drained once per frame. main()
	// registers an exit callback but nothing ever pumped the queue, so system
	// events -- above all the XMB's "please quit" request -- were never
	// acknowledged. The system then force-terminates the process, which is why
	// leaving the emulator took down the whole console. Every PSL1GHT sample
	// pairs this call with the flip.
	//
	// flip() is the one choke point shared by all three renderers (the libgui
	// menu, glN64_GX and mupen64_soft_gfx), so it covers every frame path.
#ifdef WII64_FRAME_PROFILE
	profWaitIters = 0;
	unsigned int su0 = rsxutil_tick();
#endif
	sysUtilCheckCallback();
#ifdef WII64_FRAME_PROFILE
	profSysUtilTicks = rsxutil_tick() - su0;

	// Time a plain label round trip. This asks the RSX to write a value to
	// memory and waits for it -- no flip involved. If this is fast while the
	// flip wait is seconds, the RSX is executing commands normally and only
	// the flip is stuck (a display/vsync problem). If this is also seconds,
	// the RSX is not consuming the command buffer at all.
	{
		gcmControlRegister volatile *ctrl = gcmGetControlRegister();
		unsigned int l0 = rsxutil_tick();
		profLabelIters = 0;
		rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);
		rsxFlushBuffer(context);

		// Snapshot the RSX command-buffer pointers immediately after the
		// flush. If get stays pinned while put has moved on, the RSX is
		// blocked on a specific command and the offset tells us which.
		profCtrl[0] = ctrl->put;
		profCtrl[1] = ctrl->get;
		profCtrl[2] = ctrl->ref;

		while(*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX)!=sLabelVal
		      && profLabelIters < 200000){
			++profLabelIters;
			// Record where get has crept to a short way into the stall.
			if(profLabelIters == 200) profCtrl[3] = ctrl->get;
			usleep(30);
		}
		++sLabelVal;

		profCtrl[4] = ctrl->put;
		profCtrl[5] = ctrl->get;
		profLabelTicks = rsxutil_tick() - l0;
	}
#endif

	if(!first_fb) waitflip();
	else gcmResetFlipStatus();

	gcmSetFlip(context,curr_fb);
	rsxFlushBuffer(context);

	gcmSetWaitFlip(context);

	curr_fb ^= 1;
	setRenderTarget(curr_fb);

	first_fb = 0;
}
