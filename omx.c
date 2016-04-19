/*
 * rpihddevice - VDR HD output device for Raspberry Pi
 * Copyright (C) 2014, 2015, 2016 Thomas Reufer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <queue>

#include "omx.h"
#include "display.h"

#include <vdr/tools.h>
#include <vdr/thread.h>

extern "C" {
#include "ilclient.h"
}

#include "bcm_host.h"

#define OMX_PRE_ROLL 0

// default: 16x 4096 bytes, now 128x 16k (2M)
#define OMX_AUDIO_BUFFERS 128
#define OMX_AUDIO_BUFFERSIZE KILOBYTE(16);

#define OMX_AUDIO_CHANNEL_MAPPING(s, c) \
switch (c) { \
case 4: \
	(s).eChannelMapping[0] = OMX_AUDIO_ChannelLF; \
	(s).eChannelMapping[1] = OMX_AUDIO_ChannelRF; \
	(s).eChannelMapping[2] = OMX_AUDIO_ChannelLR; \
	(s).eChannelMapping[3] = OMX_AUDIO_ChannelRR; \
	break; \
case 1: \
	(s).eChannelMapping[0] = OMX_AUDIO_ChannelCF; \
	break; \
case 8: \
	(s).eChannelMapping[6] = OMX_AUDIO_ChannelLS; \
	(s).eChannelMapping[7] = OMX_AUDIO_ChannelRS; \
case 6: \
	(s).eChannelMapping[2] = OMX_AUDIO_ChannelCF; \
	(s).eChannelMapping[3] = OMX_AUDIO_ChannelLFE; \
	(s).eChannelMapping[4] = OMX_AUDIO_ChannelLR; \
	(s).eChannelMapping[5] = OMX_AUDIO_ChannelRR; \
case 2: \
default: \
	(s).eChannelMapping[0] = OMX_AUDIO_ChannelLF; \
	(s).eChannelMapping[1] = OMX_AUDIO_ChannelRF; \
	break; }

class cOmxEvents
{

public:

	enum eEvent {
		ePortSettingsChanged,
		eConfigChanged,
		eEndOfStream,
		eBufferEmptied
	};

	struct Event
	{
		Event(eEvent _event, int _data)
			: event(_event), data(_data) { };
		eEvent 	event;
		int		data;
	};

	cOmxEvents() :
		m_signal(new cCondWait()),
		m_mutex(new cMutex())
	{ }

	virtual ~cOmxEvents()
	{
		while (!m_events.empty())
		{
			delete m_events.front();
			m_events.pop();
		}
		delete m_signal;
		delete m_mutex;
	}

	Event* Get(void)
	{
		Event* event = 0;
		if (!m_events.empty())
		{
			m_mutex->Lock();
			event = m_events.front();
			m_events.pop();
			m_mutex->Unlock();
		}
		return event;
	}

	void Add(Event* event)
	{
		m_mutex->Lock();
		m_events.push(event);
		m_mutex->Unlock();
		m_signal->Signal();
	}

private:

	cOmxEvents(const cOmxEvents&);
	cOmxEvents& operator= (const cOmxEvents&);

	cCondWait*	m_signal;
	cMutex*		m_mutex;
	std::queue<Event*> m_events;
};

const char* cOmx::errStr(int err)
{
	return 	err == OMX_ErrorNone                               ? "None"                               :
			err == OMX_ErrorInsufficientResources              ? "InsufficientResources"              :
			err == OMX_ErrorUndefined                          ? "Undefined"                          :
			err == OMX_ErrorInvalidComponentName               ? "InvalidComponentName"               :
			err == OMX_ErrorComponentNotFound                  ? "ComponentNotFound"                  :
			err == OMX_ErrorInvalidComponent                   ? "InvalidComponent"                   :
			err == OMX_ErrorBadParameter                       ? "BadParameter"                       :
			err == OMX_ErrorNotImplemented                     ? "NotImplemented"                     :
			err == OMX_ErrorUnderflow                          ? "Underflow"                          :
			err == OMX_ErrorOverflow                           ? "Overflow"                           :
			err == OMX_ErrorHardware                           ? "Hardware"                           :
			err == OMX_ErrorInvalidState                       ? "InvalidState"                       :
			err == OMX_ErrorStreamCorrupt                      ? "StreamCorrupt"                      :
			err == OMX_ErrorPortsNotCompatible                 ? "PortsNotCompatible"                 :
			err == OMX_ErrorResourcesLost                      ? "ResourcesLost"                      :
			err == OMX_ErrorNoMore                             ? "NoMore"                             :
			err == OMX_ErrorVersionMismatch                    ? "VersionMismatch"                    :
			err == OMX_ErrorNotReady                           ? "NotReady"                           :
			err == OMX_ErrorTimeout                            ? "Timeout"                            :
			err == OMX_ErrorSameState                          ? "SameState"                          :
			err == OMX_ErrorResourcesPreempted                 ? "ResourcesPreempted"                 :
			err == OMX_ErrorPortUnresponsiveDuringAllocation   ? "PortUnresponsiveDuringAllocation"   :
			err == OMX_ErrorPortUnresponsiveDuringDeallocation ? "PortUnresponsiveDuringDeallocation" :
			err == OMX_ErrorPortUnresponsiveDuringStop         ? "PortUnresponsiveDuringStop"         :
			err == OMX_ErrorIncorrectStateTransition           ? "IncorrectStateTransition"           :
			err == OMX_ErrorIncorrectStateOperation            ? "IncorrectStateOperation"            :
			err == OMX_ErrorUnsupportedSetting                 ? "UnsupportedSetting"                 :
			err == OMX_ErrorUnsupportedIndex                   ? "UnsupportedIndex"                   :
			err == OMX_ErrorBadPortIndex                       ? "BadPortIndex"                       :
			err == OMX_ErrorPortUnpopulated                    ? "PortUnpopulated"                    :
			err == OMX_ErrorComponentSuspended                 ? "ComponentSuspended"                 :
			err == OMX_ErrorDynamicResourcesUnavailable        ? "DynamicResourcesUnavailable"        :
			err == OMX_ErrorMbErrorsInFrame                    ? "MbErrorsInFrame"                    :
			err == OMX_ErrorFormatNotDetected                  ? "FormatNotDetected"                  :
			err == OMX_ErrorContentPipeOpenFailed              ? "ContentPipeOpenFailed"              :
			err == OMX_ErrorContentPipeCreationFailed          ? "ContentPipeCreationFailed"          :
			err == OMX_ErrorSeperateTablesUsed                 ? "SeperateTablesUsed"                 :
			err == OMX_ErrorTunnelingUnsupported               ? "TunnelingUnsupported"               :
			err == OMX_ErrorKhronosExtensions                  ? "KhronosExtensions"                  :
			err == OMX_ErrorVendorStartUnused                  ? "VendorStartUnused"                  :
			err == OMX_ErrorDiskFull                           ? "DiskFull"                           :
			err == OMX_ErrorMaxFileSize                        ? "MaxFileSize"                        :
			err == OMX_ErrorDrmUnauthorised                    ? "DrmUnauthorised"                    :
			err == OMX_ErrorDrmExpired                         ? "DrmExpired"                         :
			err == OMX_ErrorDrmGeneral                         ? "DrmGeneral"                         :
			"unknown";
}

void cOmx::Action(void)
{
	cTimeMs timer;
	while (Running())
	{
		while (cOmxEvents::Event* event = m_portEvents->Get())
		{
			Lock();
			switch (event->event)
			{
			case cOmxEvents::ePortSettingsChanged:
				HandlePortSettingsChanged(event->data);

				for (cOmxEventHandler *handler = m_eventHandlers->First();
						handler; handler = m_eventHandlers->Next(handler))
					handler->PortSettingsChanged(event->data);
				break;

			case cOmxEvents::eConfigChanged:
				if (event->data == OMX_IndexConfigBufferStall
						&& IsBufferStall() && !IsClockFreezed())
					for (cOmxEventHandler *handler = m_eventHandlers->First();
							handler; handler = m_eventHandlers->Next(handler))
						handler->BufferStalled();
				break;

			case cOmxEvents::eEndOfStream:
				for (cOmxEventHandler *handler = m_eventHandlers->First();
						handler; handler = m_eventHandlers->Next(handler))
					handler->EndOfStreamReceived(event->data);
				break;

			case cOmxEvents::eBufferEmptied:
				HandlePortBufferEmptied((eOmxComponent)event->data);

				for (cOmxEventHandler *handler = m_eventHandlers->First();
						handler; handler = m_eventHandlers->Next(handler))
					handler->BufferEmptied((eOmxComponent)event->data);
				break;

			default:
				break;
			}
			Unlock();
			delete event;
		}
		cCondWait::SleepMs(10);

		if (timer.TimedOut())
		{
			timer.Set(100);
			Lock();
			for (int i = BUFFERSTAT_FILTER_SIZE - 1; i > 0; i--)
			{
				m_usedAudioBuffers[i] = m_usedAudioBuffers[i - 1];
				m_usedVideoBuffers[i] = m_usedVideoBuffers[i - 1];
			}

			for (cOmxEventHandler *handler = m_eventHandlers->First(); handler;
					handler = m_eventHandlers->Next(handler))
				handler->Tick();
			Unlock();
		}
	}
}

void cOmx::GetBufferUsage(int &audio, int &video)
{
	audio = 0;
	for (int i = 0; i < BUFFERSTAT_FILTER_SIZE; i++)
	{
		audio += m_usedAudioBuffers[i];
	}
	audio = audio * 100 / BUFFERSTAT_FILTER_SIZE / OMX_AUDIO_BUFFERS;
}

void cOmx::HandlePortBufferEmptied(eOmxComponent component)
{
	Lock();

	switch (component)
	{
	case eVideoDecoder:
		m_usedVideoBuffers[0]--;
		break;

	case eAudioRender:
		m_usedAudioBuffers[0]--;
		break;

	default:
		ELOG("HandlePortBufferEmptied: invalid component!");
		break;
	}
	Unlock();
}

void cOmx::HandlePortSettingsChanged(unsigned int portId)
{
	Lock();
	DBG("HandlePortSettingsChanged(%d)", portId);

	switch (portId)
	{
	case 191:
		if (!SetupTunnel(eVideoFxToVideoScheduler))
			ELOG("failed to setup up tunnel from video fx to scheduler!");
		if (!ChangeComponentState(eVideoScheduler, OMX_StateExecuting))
			ELOG("failed to enable video scheduler!");
		break;

	case 11:
		if (!SetupTunnel(eVideoSchedulerToVideoRender))
			ELOG("failed to setup up tunnel from scheduler to render!");
		if (!ChangeComponentState(eVideoRender, OMX_StateExecuting))
			ELOG("failed to enable video render!");
		break;
	}

	Unlock();
}

void cOmx::OnBufferEmpty(void *instance, COMPONENT_T *comp)
{
	cOmx* omx = static_cast <cOmx*> (instance);
	omx->m_portEvents->Add(
			new cOmxEvents::Event(cOmxEvents::eBufferEmptied,
					comp == omx->m_comp[eVideoDecoder] ? eVideoDecoder :
					comp == omx->m_comp[eAudioRender] ? eAudioRender :
							eInvalidComponent));
}

void cOmx::OnPortSettingsChanged(void *instance, COMPONENT_T *comp, OMX_U32 data)
{
	cOmx* omx = static_cast <cOmx*> (instance);
	omx->m_portEvents->Add(
			new cOmxEvents::Event(cOmxEvents::ePortSettingsChanged, data));
}

void cOmx::OnConfigChanged(void *instance, COMPONENT_T *comp, OMX_U32 data)
{
	cOmx* omx = static_cast <cOmx*> (instance);
	omx->m_portEvents->Add(
			new cOmxEvents::Event(cOmxEvents::eConfigChanged, data));
}

void cOmx::OnEndOfStream(void *instance, COMPONENT_T *comp, OMX_U32 data)
{
	cOmx* omx = static_cast <cOmx*> (instance);
	omx->m_portEvents->Add(
			new cOmxEvents::Event(cOmxEvents::eEndOfStream, data));
}

void cOmx::OnError(void *instance, COMPONENT_T *comp, OMX_U32 data)
{
	if ((OMX_S32)data != OMX_ErrorSameState)
		ELOG("OmxError(%s)", errStr((int)data));
}

cOmx::cOmx() :
	cThread(),
	m_client(NULL),
	m_setAudioStartTime(false),
	m_spareAudioBuffers(0),
	m_clockReference(eClockRefNone),
	m_clockScale(0),
	m_portEvents(new cOmxEvents()),
	m_eventHandlers(new cList<cOmxEventHandler>)
{
	memset(m_tun, 0, sizeof(m_tun));
	memset(m_comp, 0, sizeof(m_comp));
}

cOmx::~cOmx()
{
	delete m_eventHandlers;
	delete m_portEvents;
}

int cOmx::Init(int display, int layer)
{
	m_client = ilclient_init();
	if (m_client == NULL)
		ELOG("ilclient_init() failed!");

	if (OMX_Init() != OMX_ErrorNone)
		ELOG("OMX_Init() failed!");

	ilclient_set_error_callback(m_client, OnError, this);
	ilclient_set_empty_buffer_done_callback(m_client, OnBufferEmpty, this);
	ilclient_set_port_settings_callback(m_client, OnPortSettingsChanged, this);
	ilclient_set_eos_callback(m_client, OnEndOfStream, this);
	ilclient_set_configchanged_callback(m_client, OnConfigChanged, this);

	// create video_render
	if (!CreateComponent(eVideoRender))
		ELOG("failed creating video render!");

	//create clock
	if (!CreateComponent(eClock))
		ELOG("failed creating clock!");

	// create audio_render
	if (!CreateComponent(eAudioRender, true))
		ELOG("failed creating audio render!");

	//create video_scheduler
	if (!CreateComponent(eVideoScheduler))
		ELOG("failed creating video scheduler!");

	// set tunnels
	SetTunnel(eVideoSchedulerToVideoRender, eVideoScheduler, 11, eVideoRender, 90);
	SetTunnel(eClockToVideoScheduler, eClock, 80, eVideoScheduler, 12);
	SetTunnel(eClockToAudioRender, eClock, 81, eAudioRender, 101);

	// setup clock tunnels first
	if (!SetupTunnel(eClockToVideoScheduler))
		ELOG("failed to setup up tunnel from clock to video scheduler!");

	if (!SetupTunnel(eClockToAudioRender))
		ELOG("failed to setup up tunnel from clock to audio render!");

	ChangeComponentState(eClock, OMX_StateExecuting);
	ChangeComponentState(eAudioRender, OMX_StateIdle);

	SetDisplay(display, layer);
	SetClockLatencyTarget();
	SetClockReference(cOmx::eClockRefVideo);

	FlushAudio();

	Start();

	return 0;
}

int cOmx::DeInit(void)
{
	Cancel(-1);
	m_portEvents->Add(0);

	DisableTunnel(eClockToAudioRender);

	ChangeComponentState(eClock, OMX_StateIdle);
	ChangeComponentState(eAudioRender, OMX_StateIdle);

	CleanupComponent(eClock);
	CleanupComponent(eAudioRender);
	CleanupComponent(eVideoScheduler);
	CleanupComponent(eVideoRender);

	OMX_Deinit();
	ilclient_destroy(m_client);
	return 0;
}

bool cOmx::CreateComponent(eOmxComponent comp, bool enableInputBuffers)
{
	return (comp >= 0 && comp < eNumComponents &&
			!ilclient_create_component(m_client, &m_comp[comp],
					comp == eClock          ? "clock"           :
					comp == eVideoDecoder   ? "video_decode"    :
					comp == eVideoFx        ? "image_fx"        :
					comp == eVideoScheduler ? "video_scheduler" :
					comp == eVideoRender    ? "video_render"    :
					comp == eAudioRender    ? "audio_render"    : "",
					(ILCLIENT_CREATE_FLAGS_T)(ILCLIENT_DISABLE_ALL_PORTS |
					(enableInputBuffers ? ILCLIENT_ENABLE_INPUT_BUFFERS : 0))));
}

void cOmx::CleanupComponent(eOmxComponent comp)
{
	if (comp >= 0 && comp < eNumComponents)
	{
		COMPONENT_T *c[2] = { m_comp[comp], 0 };
		ilclient_cleanup_components(c);
	}
}

bool cOmx::ChangeComponentState(eOmxComponent comp, OMX_STATETYPE state)
{
	return comp >= 0 && comp < eNumComponents &&
			!ilclient_change_component_state(m_comp[comp], state);
}

bool cOmx::FlushComponent(eOmxComponent comp, int port)
{
	if (comp >= 0 && comp < eNumComponents)
	{
		if (OMX_SendCommand(ILC_GET_HANDLE(m_comp[comp]), OMX_CommandFlush,
				port, NULL) == OMX_ErrorNone)
		{
			ilclient_wait_for_event(m_comp[comp], OMX_EventCmdComplete,
				OMX_CommandFlush, 0, port, 0, ILCLIENT_PORT_FLUSH,
				VCOS_EVENT_FLAGS_SUSPEND);

			return true;
		}
	}
	return false;
}

bool cOmx::EnablePortBuffers(eOmxComponent comp, int port)
{
	return comp >= 0 && comp < eNumComponents &&
			!ilclient_enable_port_buffers(m_comp[comp], port, NULL, NULL, NULL);
}

void cOmx::DisablePortBuffers(eOmxComponent comp, int port,
		OMX_BUFFERHEADERTYPE *buffers)
{
	if (comp >= 0 && comp < eNumComponents)
		ilclient_disable_port_buffers(m_comp[comp], port, buffers, NULL, NULL);
}

OMX_BUFFERHEADERTYPE* cOmx::GetBuffer(eOmxComponent comp, int port)
{
	return (comp >= 0 && comp < eNumComponents) ?
			ilclient_get_input_buffer(m_comp[comp], port, 0) : 0;
}

bool cOmx::EmptyBuffer(eOmxComponent comp, OMX_BUFFERHEADERTYPE *buf)
{
	return buf && comp >= 0 && comp < eNumComponents &&
			(OMX_EmptyThisBuffer(ILC_GET_HANDLE(m_comp[comp]), buf)
				== OMX_ErrorNone);
}

bool cOmx::GetParameter(eOmxComponent comp, OMX_INDEXTYPE index, OMX_PTR param)
{
	return param && comp >= 0 && comp < eNumComponents &&
			(OMX_GetParameter(ILC_GET_HANDLE(m_comp[comp]), index, param)
					== OMX_ErrorNone);
}

bool cOmx::SetParameter(eOmxComponent comp, OMX_INDEXTYPE index, OMX_PTR param)
{
	return param && comp >= 0 && comp < eNumComponents &&
			(OMX_SetParameter(ILC_GET_HANDLE(m_comp[comp]), index, param)
					== OMX_ErrorNone);
}

bool cOmx::GetConfig(eOmxComponent comp, OMX_INDEXTYPE index, OMX_PTR config)
{
	return config && comp >= 0 && comp < eNumComponents &&
			(OMX_GetConfig(ILC_GET_HANDLE(m_comp[comp]), index, config)
					== OMX_ErrorNone);
}

bool cOmx::SetConfig(eOmxComponent comp, OMX_INDEXTYPE index, OMX_PTR config)
{
	return config && comp >= 0 && comp < eNumComponents &&
			(OMX_SetConfig(ILC_GET_HANDLE(m_comp[comp]), index, config)
					== OMX_ErrorNone);
}

void cOmx::SetTunnel(eOmxTunnel tunnel, eOmxComponent srcComp, int srcPort,
		eOmxComponent dstComp, int dstPort)
{
	if (tunnel >= 0 && tunnel < eNumTunnels && srcComp >= 0 && srcComp
			< eNumComponents && dstComp >= 0 && dstComp < eNumComponents)
		set_tunnel(&m_tun[tunnel],
			m_comp[srcComp], srcPort, m_comp[dstComp], dstPort);
}

bool cOmx::SetupTunnel(eOmxTunnel tunnel, int timeout)
{
	return tunnel >= 0 && tunnel < eNumTunnels &&
			!ilclient_setup_tunnel(&m_tun[tunnel], 0, timeout);
}

void cOmx::DisableTunnel(eOmxTunnel tunnel)
{
	if (tunnel >= 0 && tunnel < eNumTunnels)
		ilclient_disable_tunnel(&m_tun[tunnel]);
}

void cOmx::TeardownTunnel(eOmxTunnel tunnel)
{
	if (tunnel >= 0 && tunnel < eNumTunnels)
	{
		TUNNEL_T t[2] = { m_tun[tunnel], 0 };
		ilclient_teardown_tunnels(t);
	}
}

void cOmx::FlushTunnel(eOmxTunnel tunnel)
{
	if (tunnel >= 0 && tunnel < eNumTunnels)
		ilclient_flush_tunnels(&m_tun[tunnel], 1);
}

void cOmx::AddEventHandler(cOmxEventHandler *handler)
{
	Lock();
	m_eventHandlers->Add(handler);
	Unlock();
}

void cOmx::RemoveEventHandler(cOmxEventHandler *handler)
{
	Lock();
	m_eventHandlers->Del(handler, false);
	Unlock();
}

OMX_TICKS cOmx::ToOmxTicks(int64_t val)
{
	OMX_TICKS ticks;
	ticks.nLowPart = val;
	ticks.nHighPart = val >> 32;
	return ticks;
}

int64_t cOmx::FromOmxTicks(OMX_TICKS &ticks)
{
	int64_t ret = ticks.nLowPart | ((int64_t)(ticks.nHighPart) << 32);
	return ret;
}

void cOmx::PtsToTicks(int64_t pts, OMX_TICKS &ticks)
{
	// ticks = pts * OMX_TICKS_PER_SECOND / PTSTICKS
	pts = pts * 100 / 9;
	ticks.nLowPart = pts;
	ticks.nHighPart = pts >> 32;
}

int64_t cOmx::TicksToPts(OMX_TICKS &ticks)
{
	// pts = ticks * PTSTICKS / OMX_TICKS_PER_SECOND
	int64_t pts = ticks.nHighPart;
	pts = (pts << 32) + ticks.nLowPart;
	pts = pts * 9 / 100;
	return pts;
}

int64_t cOmx::GetSTC(void)
{
	int64_t stc = OMX_INVALID_PTS;
	OMX_TIME_CONFIG_TIMESTAMPTYPE timestamp;
	OMX_INIT_STRUCT(timestamp);
	timestamp.nPortIndex = OMX_ALL;

	if (OMX_GetConfig(ILC_GET_HANDLE(m_comp[eClock]),
		OMX_IndexConfigTimeCurrentMediaTime, &timestamp) != OMX_ErrorNone)
		ELOG("failed get current clock reference!");
	else
		stc = TicksToPts(timestamp.nTimestamp);

	return stc;
}

bool cOmx::IsClockRunning(void)
{
	OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
	OMX_INIT_STRUCT(cstate);

	if (OMX_GetConfig(ILC_GET_HANDLE(m_comp[eClock]),
			OMX_IndexConfigTimeClockState, &cstate) != OMX_ErrorNone)
		ELOG("failed get clock state!");

	if (cstate.eState == OMX_TIME_ClockStateRunning)
		return true;
	else
		return false;
}

void cOmx::StartClock(bool waitForVideo, bool waitForAudio)
{
	DBG("StartClock(%svideo, %saudio)",
			waitForVideo ? "" : "no ",
			waitForAudio ? "" : "no ");

	OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
	OMX_INIT_STRUCT(cstate);

	cstate.eState = OMX_TIME_ClockStateRunning;
	cstate.nOffset = ToOmxTicks(-1000LL * OMX_PRE_ROLL);

	if (waitForVideo)
	{
		cstate.eState = OMX_TIME_ClockStateWaitingForStartTime;
		cstate.nWaitMask |= OMX_CLOCKPORT0;
	}
	if (waitForAudio)
	{
		cstate.eState = OMX_TIME_ClockStateWaitingForStartTime;
		m_setAudioStartTime = true;
		cstate.nWaitMask |= OMX_CLOCKPORT1;
	}

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
			OMX_IndexConfigTimeClockState, &cstate) != OMX_ErrorNone)
		ELOG("failed to start clock!");
}

void cOmx::StopClock(void)
{
	OMX_TIME_CONFIG_CLOCKSTATETYPE cstate;
	OMX_INIT_STRUCT(cstate);

	cstate.eState = OMX_TIME_ClockStateStopped;
	cstate.nOffset = ToOmxTicks(-1000LL * OMX_PRE_ROLL);

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
			OMX_IndexConfigTimeClockState, &cstate) != OMX_ErrorNone)
		ELOG("failed to stop clock!");
}

void cOmx::SetClockScale(OMX_S32 scale)
{
	if (scale != m_clockScale)
	{
		OMX_TIME_CONFIG_SCALETYPE scaleType;
		OMX_INIT_STRUCT(scaleType);
		scaleType.xScale = scale;

		if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
				OMX_IndexConfigTimeScale, &scaleType) != OMX_ErrorNone)
			ELOG("failed to set clock scale (%d)!", scale);
		else
			m_clockScale = scale;
	}
}

void cOmx::ResetClock(void)
{
	OMX_TIME_CONFIG_TIMESTAMPTYPE timeStamp;
	OMX_INIT_STRUCT(timeStamp);

	if (m_clockReference == eClockRefAudio || m_clockReference == eClockRefNone)
	{
		timeStamp.nPortIndex = 81;
		if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
			OMX_IndexConfigTimeCurrentAudioReference, &timeStamp)
				!= OMX_ErrorNone)
			ELOG("failed to set current audio reference time!");
	}

	if (m_clockReference == eClockRefVideo || m_clockReference == eClockRefNone)
	{
		timeStamp.nPortIndex = 80;
		if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
			OMX_IndexConfigTimeCurrentVideoReference, &timeStamp)
				!= OMX_ErrorNone)
			ELOG("failed to set current video reference time!");
	}
}

unsigned int cOmx::GetAudioLatency(void)
{
	unsigned int ret = 0;

	OMX_PARAM_U32TYPE u32;
	OMX_INIT_STRUCT(u32);
	u32.nPortIndex = 100;

	if (OMX_GetConfig(ILC_GET_HANDLE(m_comp[eAudioRender]),
		OMX_IndexConfigAudioRenderingLatency, &u32) != OMX_ErrorNone)
		ELOG("failed get audio render latency!");
	else
		ret = u32.nU32;

	return ret;
}

void cOmx::SetClockReference(eClockReference clockReference)
{
	if (m_clockReference != clockReference)
	{
		OMX_TIME_CONFIG_ACTIVEREFCLOCKTYPE refClock;
		OMX_INIT_STRUCT(refClock);
		refClock.eClock =
			(clockReference == eClockRefAudio) ? OMX_TIME_RefClockAudio :
			(clockReference == eClockRefVideo) ? OMX_TIME_RefClockVideo :
				OMX_TIME_RefClockNone;

		if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
				OMX_IndexConfigTimeActiveRefClock, &refClock) != OMX_ErrorNone)
			ELOG("failed set active clock reference!");
		else
			DBG("set active clock reference to %s",
					clockReference == eClockRefAudio ? "audio" :
					clockReference == eClockRefVideo ? "video" : "none");

		m_clockReference = clockReference;
	}
}

void cOmx::SetClockLatencyTarget(void)
{
	OMX_CONFIG_LATENCYTARGETTYPE latencyTarget;
	OMX_INIT_STRUCT(latencyTarget);

	// latency target for clock
	// values set according reference implementation in omxplayer
	latencyTarget.nPortIndex = OMX_ALL;
	latencyTarget.bEnabled = OMX_TRUE;
	latencyTarget.nFilter = 10;
	latencyTarget.nTarget = 0;
	latencyTarget.nShift = 3;
	latencyTarget.nSpeedFactor = -60;
	latencyTarget.nInterFactor = 100;
	latencyTarget.nAdjCap = 100;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eClock]),
			OMX_IndexConfigLatencyTarget, &latencyTarget) != OMX_ErrorNone)
		ELOG("failed set clock latency target!");

	// latency target for video render
	// values set according reference implementation in omxplayer
	latencyTarget.nPortIndex = 90;
	latencyTarget.bEnabled = OMX_TRUE;
	latencyTarget.nFilter = 2;
	latencyTarget.nTarget = 4000;
	latencyTarget.nShift = 3;
	latencyTarget.nSpeedFactor = -135;
	latencyTarget.nInterFactor = 500;
	latencyTarget.nAdjCap = 20;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eVideoRender]),
			OMX_IndexConfigLatencyTarget, &latencyTarget) != OMX_ErrorNone)
		ELOG("failed set video render latency target!");
}

bool cOmx::IsBufferStall(void)
{
	OMX_CONFIG_BUFFERSTALLTYPE stallConf;
	OMX_INIT_STRUCT(stallConf);
	stallConf.nPortIndex = 131;
	if (OMX_GetConfig(ILC_GET_HANDLE(m_comp[eVideoDecoder]),
			OMX_IndexConfigBufferStall, &stallConf) != OMX_ErrorNone)
		ELOG("failed to get video decoder stall config!");

	return stallConf.bStalled == OMX_TRUE;
}

void cOmx::SetVolume(int vol)
{
	OMX_AUDIO_CONFIG_VOLUMETYPE volume;
	OMX_INIT_STRUCT(volume);
	volume.nPortIndex = 100;
	volume.bLinear = OMX_TRUE;
	volume.sVolume.nValue = vol * 100 / 255;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexConfigAudioVolume, &volume) != OMX_ErrorNone)
		ELOG("failed to set volume!");
}

void cOmx::SetMute(bool mute)
{
	OMX_AUDIO_CONFIG_MUTETYPE amute;
	OMX_INIT_STRUCT(amute);
	amute.nPortIndex = 100;
	amute.bMute = mute ? OMX_TRUE : OMX_FALSE;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexConfigAudioMute, &amute) != OMX_ErrorNone)
		ELOG("failed to set mute state!");
}

void cOmx::StopAudio(void)
{
	Lock();

	// put audio render onto idle
	ilclient_flush_tunnels(&m_tun[eClockToAudioRender], 1);
	ilclient_disable_tunnel(&m_tun[eClockToAudioRender]);
	ilclient_change_component_state(m_comp[eAudioRender], OMX_StateIdle);
	ilclient_disable_port_buffers(m_comp[eAudioRender], 100,
			m_spareAudioBuffers, NULL, NULL);

	m_spareAudioBuffers = 0;
	Unlock();
}

void cOmx::FlushAudio(void)
{
	Lock();

	if (OMX_SendCommand(ILC_GET_HANDLE(m_comp[eAudioRender]), OMX_CommandFlush, 100, NULL) != OMX_ErrorNone)
		ELOG("failed to flush audio render!");

	ilclient_wait_for_event(m_comp[eAudioRender], OMX_EventCmdComplete,
		OMX_CommandFlush, 0, 100, 0, ILCLIENT_PORT_FLUSH,
		VCOS_EVENT_FLAGS_SUSPEND);

	ilclient_flush_tunnels(&m_tun[eClockToAudioRender], 1);
	Unlock();
}

int cOmx::SetupAudioRender(cAudioCodec::eCodec outputFormat, int channels,
		cRpiAudioPort::ePort audioPort, int samplingRate, int frameSize)
{
	Lock();

	OMX_AUDIO_PARAM_PORTFORMATTYPE format;
	OMX_INIT_STRUCT(format);
	format.nPortIndex = 100;
	if (OMX_GetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexParamAudioPortFormat, &format) != OMX_ErrorNone)
		ELOG("failed to get audio port format parameters!");

	format.eEncoding =
		outputFormat == cAudioCodec::ePCM  ? OMX_AUDIO_CodingPCM :
		outputFormat == cAudioCodec::eMPG  ? OMX_AUDIO_CodingMP3 :
		outputFormat == cAudioCodec::eAC3  ? OMX_AUDIO_CodingDDP :
		outputFormat == cAudioCodec::eEAC3 ? OMX_AUDIO_CodingDDP :
		outputFormat == cAudioCodec::eAAC  ? OMX_AUDIO_CodingAAC :
		outputFormat == cAudioCodec::eDTS  ? OMX_AUDIO_CodingDTS :
				OMX_AUDIO_CodingAutoDetect;

	if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexParamAudioPortFormat, &format) != OMX_ErrorNone)
		ELOG("failed to set audio port format parameters!");

	switch (outputFormat)
	{
	case cAudioCodec::eMPG:
		OMX_AUDIO_PARAM_MP3TYPE mp3;
		OMX_INIT_STRUCT(mp3);
		mp3.nPortIndex = 100;
		mp3.nChannels = channels;
		mp3.nSampleRate = samplingRate;
		mp3.eChannelMode = OMX_AUDIO_ChannelModeStereo; // ?
		mp3.eFormat = OMX_AUDIO_MP3StreamFormatMP1Layer3; // should be MPEG-1 layer 2

		if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
				OMX_IndexParamAudioMp3, &mp3) != OMX_ErrorNone)
			ELOG("failed to set audio render mp3 parameters!");
		break;

	case cAudioCodec::eAC3:
	case cAudioCodec::eEAC3:
		OMX_AUDIO_PARAM_DDPTYPE ddp;
		OMX_INIT_STRUCT(ddp);
		ddp.nPortIndex = 100;
		ddp.nChannels = channels;
		ddp.nSampleRate = samplingRate;
		OMX_AUDIO_CHANNEL_MAPPING(ddp, channels);

		if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
				OMX_IndexParamAudioDdp, &ddp) != OMX_ErrorNone)
			ELOG("failed to set audio render ddp parameters!");
		break;

	case cAudioCodec::eAAC:
		OMX_AUDIO_PARAM_AACPROFILETYPE aac;
		OMX_INIT_STRUCT(aac);
		aac.nPortIndex = 100;
		aac.nChannels = channels;
		aac.nSampleRate = samplingRate;
		aac.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4ADTS;

		if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
				OMX_IndexParamAudioAac, &aac) != OMX_ErrorNone)
			ELOG("failed to set audio render aac parameters!");
		break;

	case cAudioCodec::eDTS:
		OMX_AUDIO_PARAM_DTSTYPE dts;
		OMX_INIT_STRUCT(dts);
		dts.nPortIndex = 100;
		dts.nChannels = channels;
		dts.nSampleRate = samplingRate;
		dts.nDtsType = 1;
		dts.nFormat = 3; /* 16bit, LE */
		dts.nDtsFrameSizeBytes = frameSize;
		OMX_AUDIO_CHANNEL_MAPPING(dts, channels);

		if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
				OMX_IndexParamAudioDts, &dts) != OMX_ErrorNone)
			ELOG("failed to set audio render dts parameters!");
		break;

	case cAudioCodec::ePCM:
		OMX_AUDIO_PARAM_PCMMODETYPE pcm;
		OMX_INIT_STRUCT(pcm);
		pcm.nPortIndex = 100;
		pcm.nChannels = channels;
		pcm.eNumData = OMX_NumericalDataSigned;
		pcm.eEndian = OMX_EndianLittle;
		pcm.bInterleaved = OMX_TRUE;
		pcm.nBitPerSample = 16;
		pcm.nSamplingRate = samplingRate;
		pcm.ePCMMode = OMX_AUDIO_PCMModeLinear;
		OMX_AUDIO_CHANNEL_MAPPING(pcm, channels);

		if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
				OMX_IndexParamAudioPcm, &pcm) != OMX_ErrorNone)
			ELOG("failed to set audio render pcm parameters!");
		break;

	default:
		ELOG("output codec not supported: %s!",
				cAudioCodec::Str(outputFormat));
		break;
	}

	OMX_CONFIG_BRCMAUDIODESTINATIONTYPE audioDest;
	OMX_INIT_STRUCT(audioDest);
	strcpy((char *)audioDest.sName,
			audioPort == cRpiAudioPort::eLocal ? "local" : "hdmi");

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexConfigBrcmAudioDestination, &audioDest) != OMX_ErrorNone)
		ELOG("failed to set audio destination!");

	// set up the number and size of buffers for audio render
	OMX_PARAM_PORTDEFINITIONTYPE param;
	OMX_INIT_STRUCT(param);
	param.nPortIndex = 100;
	if (OMX_GetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexParamPortDefinition, &param) != OMX_ErrorNone)
		ELOG("failed to get audio render port parameters!");

	param.nBufferSize = OMX_AUDIO_BUFFERSIZE;
	param.nBufferCountActual = OMX_AUDIO_BUFFERS;
	for (int i = 0; i < BUFFERSTAT_FILTER_SIZE; i++)
		m_usedAudioBuffers[i] = 0;

	if (OMX_SetParameter(ILC_GET_HANDLE(m_comp[eAudioRender]),
			OMX_IndexParamPortDefinition, &param) != OMX_ErrorNone)
		ELOG("failed to set audio render port parameters!");

	if (ilclient_enable_port_buffers(m_comp[eAudioRender], 100, NULL, NULL, NULL) != 0)
		ELOG("failed to enable port buffer on audio render!");

	ilclient_change_component_state(m_comp[eAudioRender], OMX_StateExecuting);

	if (ilclient_setup_tunnel(&m_tun[eClockToAudioRender], 0, 0) != 0)
		ELOG("failed to setup up tunnel from clock to audio render!");

	Unlock();
	return 0;
}

void cOmx::SetDisplayMode(bool fill, bool noaspect)
{
	OMX_CONFIG_DISPLAYREGIONTYPE region;
	OMX_INIT_STRUCT(region);
	region.nPortIndex = 90;
	region.set = (OMX_DISPLAYSETTYPE)
			(OMX_DISPLAY_SET_MODE | OMX_DISPLAY_SET_NOASPECT);

	region.noaspect = noaspect ? OMX_TRUE : OMX_FALSE;
	region.mode = fill ? OMX_DISPLAY_MODE_FILL : OMX_DISPLAY_MODE_LETTERBOX;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eVideoRender]),
			OMX_IndexConfigDisplayRegion, &region) != OMX_ErrorNone)
		ELOG("failed to set display region!");
}

void cOmx::SetPixelAspectRatio(int width, int height)
{
	OMX_CONFIG_DISPLAYREGIONTYPE region;
	OMX_INIT_STRUCT(region);
	region.nPortIndex = 90;
	region.set = (OMX_DISPLAYSETTYPE)(OMX_DISPLAY_SET_PIXEL);

	region.pixel_x = width;
	region.pixel_y = height;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eVideoRender]),
			OMX_IndexConfigDisplayRegion, &region) != OMX_ErrorNone)
		ELOG("failed to set pixel apect ratio!");
}

void cOmx::SetDisplayRegion(int x, int y, int width, int height)
{
	OMX_CONFIG_DISPLAYREGIONTYPE region;
	OMX_INIT_STRUCT(region);
	region.nPortIndex = 90;
	region.set = (OMX_DISPLAYSETTYPE)
			(OMX_DISPLAY_SET_FULLSCREEN | OMX_DISPLAY_SET_DEST_RECT);

	region.fullscreen = (!x && !y && !width && !height) ? OMX_TRUE : OMX_FALSE;
	region.dest_rect.x_offset = x;
	region.dest_rect.y_offset = y;
	region.dest_rect.width = width;
	region.dest_rect.height = height;

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eVideoRender]),
			OMX_IndexConfigDisplayRegion, &region) != OMX_ErrorNone)
		ELOG("failed to set display region!");
}

void cOmx::SetDisplay(int display, int layer)
{
	OMX_CONFIG_DISPLAYREGIONTYPE region;
	OMX_INIT_STRUCT(region);
	region.nPortIndex = 90;
	region.layer = layer;
	region.num = display;
	region.set = (OMX_DISPLAYSETTYPE)
			(OMX_DISPLAY_SET_LAYER | OMX_DISPLAY_SET_NUM);

	if (OMX_SetConfig(ILC_GET_HANDLE(m_comp[eVideoRender]),
			OMX_IndexConfigDisplayRegion, &region) != OMX_ErrorNone)
		ELOG("failed to set display number and layer!");
}

OMX_BUFFERHEADERTYPE* cOmx::GetAudioBuffer(int64_t pts)
{
	Lock();
	OMX_BUFFERHEADERTYPE* buf = 0;
	if (m_spareAudioBuffers)
	{
		buf = m_spareAudioBuffers;
		m_spareAudioBuffers =
				static_cast <OMX_BUFFERHEADERTYPE*>(buf->pAppPrivate);
		buf->pAppPrivate = 0;
	}
	else
	{
		buf = ilclient_get_input_buffer(m_comp[eAudioRender], 100, 0);
		if (buf)
			m_usedAudioBuffers[0]++;
	}

	if (buf)
	{
		buf->nFilledLen = 0;
		buf->nOffset = 0;
		buf->nFlags = 0;

		if (pts == OMX_INVALID_PTS)
			buf->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;
		else if (m_setAudioStartTime)
		{
			buf->nFlags |= OMX_BUFFERFLAG_STARTTIME;
			m_setAudioStartTime = false;
		}
		cOmx::PtsToTicks(pts, buf->nTimeStamp);
	}
	Unlock();
	return buf;
}

#ifdef DEBUG_BUFFERS
void cOmx::DumpBuffer(OMX_BUFFERHEADERTYPE *buf, const char *prefix)
{
	DLOG("%s: TS=%8x%08x, LEN=%5d/%5d: %02x %02x %02x %02x... "
			"FLAGS: %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
		prefix,
		buf->nTimeStamp.nHighPart, buf->nTimeStamp.nLowPart,
		buf->nFilledLen, buf->nAllocLen,
		buf->pBuffer[0], buf->pBuffer[1], buf->pBuffer[2], buf->pBuffer[3],
		buf->nFlags & OMX_BUFFERFLAG_EOS             ? "EOS "           : "",
		buf->nFlags & OMX_BUFFERFLAG_STARTTIME       ? "STARTTIME "     : "",
		buf->nFlags & OMX_BUFFERFLAG_DECODEONLY      ? "DECODEONLY "    : "",
		buf->nFlags & OMX_BUFFERFLAG_DATACORRUPT     ? "DATACORRUPT "   : "",
		buf->nFlags & OMX_BUFFERFLAG_ENDOFFRAME      ? "ENDOFFRAME "    : "",
		buf->nFlags & OMX_BUFFERFLAG_SYNCFRAME       ? "SYNCFRAME "     : "",
		buf->nFlags & OMX_BUFFERFLAG_EXTRADATA       ? "EXTRADATA "     : "",
		buf->nFlags & OMX_BUFFERFLAG_CODECCONFIG     ? "CODECCONFIG "   : "",
		buf->nFlags & OMX_BUFFERFLAG_TIME_UNKNOWN    ? "TIME_UNKNOWN "  : "",
		buf->nFlags & OMX_BUFFERFLAG_CAPTURE_PREVIEW ? "CAPTURE_PREV "  : "",
		buf->nFlags & OMX_BUFFERFLAG_ENDOFNAL        ? "ENDOFNAL "      : "",
		buf->nFlags & OMX_BUFFERFLAG_FRAGMENTLIST    ? "FRAGMENTLIST "  : "",
		buf->nFlags & OMX_BUFFERFLAG_DISCONTINUITY   ? "DISCONTINUITY " : "",
		buf->nFlags & OMX_BUFFERFLAG_CODECSIDEINFO   ? "CODECSIDEINFO " : ""
	);
}
#endif

bool cOmx::EmptyAudioBuffer(OMX_BUFFERHEADERTYPE *buf)
{
	if (!buf)
		return false;

	Lock();
	bool ret = true;
#ifdef DEBUG_BUFFERS
	DumpBuffer(buf, "A");
#endif

	if (OMX_EmptyThisBuffer(ILC_GET_HANDLE(m_comp[eAudioRender]), buf)
			!= OMX_ErrorNone)
	{
		ELOG("failed to empty OMX audio buffer");

		if (buf->nFlags & OMX_BUFFERFLAG_STARTTIME)
			m_setAudioStartTime = true;

		buf->nFilledLen = 0;
		buf->pAppPrivate = m_spareAudioBuffers;
		m_spareAudioBuffers = buf;
		ret = false;
	}
	Unlock();
	return ret;
}
