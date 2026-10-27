#define _POSIX_C_SOURCE 200809L

#include "scpd.h"

#include <string.h>

/*
 * Every action below is implemented by soap.c and every related state variable
 * is declared. A control point that reads the SCPD should never find an action
 * the renderer will answer with 401, or an output argument with no declaration.
 */

static const char avt_scpd[] =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
"<specVersion><major>1</major><minor>0</minor></specVersion>\n"
"<actionList>\n"

"<action><name>SetAVTransportURI</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
"<argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>Play</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>Pause</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>Stop</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>Seek</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>"
"<argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetTransportInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
"<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
"<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetPositionInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>"
"<argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>"
"<argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>"
"<argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>"
"<argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>"
"<argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>"
"<argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>"
"<argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetMediaInfo</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
"<argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
"<argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
"<argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
"<argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
"<argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
"<argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument>"
"<argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument>"
"<argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetTransportSettings</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>PlayMode</name><direction>out</direction><relatedStateVariable>CurrentPlayMode</relatedStateVariable></argument>"
"<argument><name>RecQualityMode</name><direction>out</direction><relatedStateVariable>RecordQualityModes</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetCurrentTransportActions</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Actions</name><direction>out</direction><relatedStateVariable>CurrentTransportActions</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetDeviceCapabilities</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>PlayMedia</name><direction>out</direction><relatedStateVariable>PossiblePlaybackStorageMedia</relatedStateVariable></argument>"
"<argument><name>RecMedia</name><direction>out</direction><relatedStateVariable>PossibleRecordStorageMedia</relatedStateVariable></argument>"
"<argument><name>RecQualityModes</name><direction>out</direction><relatedStateVariable>PossibleRecordQualityModes</relatedStateVariable></argument>"
"</argumentList></action>\n"

"</actionList>\n"
"<serviceStateTable>\n"

"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>REL_TIME</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>PlaybackStorageMedium</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>NETWORK</allowedValue><allowedValue>NONE</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>RecordStorageMedium</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>NOT_IMPLEMENTED</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>RecordMediumWriteStatus</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>NOT_IMPLEMENTED</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>1</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>TransportState</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>STOPPED</allowedValue><allowedValue>PLAYING</allowedValue>"
"<allowedValue>PAUSED_PLAYBACK</allowedValue><allowedValue>TRANSITIONING</allowedValue>"
"<allowedValue>NO_MEDIA_PRESENT</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>TransportStatus</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>OK</allowedValue><allowedValue>ERROR_OCCURRED</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentTransportActions</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>CurrentPlayMode</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>NORMAL</allowedValue></allowedValueList>"
"<defaultValue>NORMAL</defaultValue></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>RecordQualityModes</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>NOT_IMPLEMENTED</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>PossiblePlaybackStorageMedia</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>PossibleRecordStorageMedia</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>PossibleRecordQualityModes</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"yes\"><name>LastChange</name><dataType>string</dataType></stateVariable>\n"

"</serviceStateTable>\n"
"</scpd>\n";

static const char rcs_scpd[] =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
"<specVersion><major>1</major><minor>0</minor></specVersion>\n"
"<actionList>\n"

"<action><name>GetVolume</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Volume</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>SetVolume</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Volume</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetMute</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Mute</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>SetMute</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Mute</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetVolumeDB</name><argumentList>"
"<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
"<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
"<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_VolumeDB</relatedStateVariable></argument>"
"</argumentList></action>\n"

"</actionList>\n"
"<serviceStateTable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>Master</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Volume</name><dataType>ui2</dataType>"
"<allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Mute</name><dataType>boolean</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_VolumeDB</name><dataType>i2</dataType>"
"<allowedValueRange><minimum>-10240</minimum><maximum>0</maximum><step>1</step></allowedValueRange></stateVariable>\n"
"<stateVariable sendEvents=\"yes\"><name>LastChange</name><dataType>string</dataType></stateVariable>\n"
"</serviceStateTable>\n"
"</scpd>\n";

static const char cm_scpd[] =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
"<specVersion><major>1</major><minor>0</minor></specVersion>\n"
"<actionList>\n"

"<action><name>GetProtocolInfo</name><argumentList>"
"<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
"<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetCurrentConnectionIDs</name><argumentList>"
"<argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
"</argumentList></action>\n"

"<action><name>GetCurrentConnectionInfo</name><argumentList>"
"<argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>"
"<argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>"
"<argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>"
"<argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>"
"<argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>"
"<argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>"
"</argumentList></action>\n"

"</actionList>\n"
"<serviceStateTable>\n"
"<stateVariable sendEvents=\"yes\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"yes\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"yes\"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue>"
"<allowedValue>InsufficientBandwidth</allowedValue><allowedValue>UnreliableChannel</allowedValue>"
"<allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType>"
"<allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>\n"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>\n"
"</serviceStateTable>\n"
"</scpd>\n";

const char *scpd_get(int service)
{
    switch (service) {
    case DLNA_SVC_AVT: return avt_scpd;
    case DLNA_SVC_RCS: return rcs_scpd;
    case DLNA_SVC_CM:  return cm_scpd;
    default:           return NULL;
    }
}

size_t scpd_len(int service)
{
    const char *p = scpd_get(service);

    return p ? strlen(p) : 0;
}
