#pragma once
#include "core/core.hpp"
#include <set>

namespace reaweb {
// Reviewed synchronous project operations. Dialogs, arbitrary actions, project
// lifetime changes and manual Undo/UI-refresh scopes are deliberately excluded.
inline const std::set<std::string>& batch_methods() {
  static const std::set<std::string> names = {
    "GetAppVersion", "CountTracks", "CountSelectedTracks", "GetTrack", "GetMasterTrack", "GetSelectedTrack",
    "GetTrackName", "GetMediaTrackInfo_Value", "SetMediaTrackInfo_Value", "GetSetMediaTrackInfo_String",
    "GetTrackColor", "SetTrackColor", "SetTrackSelected", "SetOnlyTrackSelected", "GetTrackGUID",
    "InsertTrackAtIndex", "DeleteTrack", "TrackList_AdjustWindows", "UpdateArrange",
    "CountMediaItems", "CountSelectedMediaItems", "GetMediaItem", "GetSelectedMediaItem",
    "CountTrackMediaItems", "GetTrackMediaItem", "AddMediaItemToTrack", "DeleteTrackMediaItem",
    "GetMediaItemTrack", "GetMediaItemInfo_Value", "SetMediaItemInfo_Value", "GetSetMediaItemInfo_String",
    "SetMediaItemPosition", "SetMediaItemLength", "SetMediaItemSelected", "SelectAllMediaItems",
    "SplitMediaItem", "MoveMediaItemToTrack", "UpdateItemInProject", "SetMediaItemTakeInfo_Value",
    "GetMediaItemTakeInfo_Value", "GetSetMediaItemTakeInfo_String", "GetMediaItemTake_Item",
    "CountTakes", "GetTake", "GetActiveTake", "SetActiveTake", "AddTakeToMediaItem", "TakeIsMIDI",
    "CreateNewMIDIItemInProj", "GetTakeName", "GetDisplayedMediaItemColor", "GetDisplayedMediaItemColor2",
    "MIDI_CountEvts", "MIDI_GetAllEvts", "MIDI_SetAllEvts", "MIDI_GetEvt", "MIDI_SetEvt",
    "MIDI_InsertEvt", "MIDI_DeleteEvt", "MIDI_GetNote", "MIDI_SetNote", "MIDI_InsertNote", "MIDI_DeleteNote",
    "MIDI_GetCC", "MIDI_SetCC", "MIDI_InsertCC", "MIDI_DeleteCC", "MIDI_GetCCShape", "MIDI_SetCCShape",
    "MIDI_GetTextSysexEvt", "MIDI_SetTextSysexEvt", "MIDI_InsertTextSysexEvt", "MIDI_DeleteTextSysexEvt",
    "MIDI_EnumSelEvts", "MIDI_EnumSelNotes", "MIDI_EnumSelCC", "MIDI_EnumSelTextSysexEvts",
    "MIDI_SelectAll", "MIDI_Sort", "MIDI_GetPPQPosFromProjTime", "MIDI_GetProjTimeFromPPQPos",
    "MIDI_GetPPQPosFromProjQN", "MIDI_GetProjQNFromPPQPos",
    "TrackFX_GetCount", "TrackFX_GetFXName", "TrackFX_GetNumParams", "TrackFX_GetParam",
    "TrackFX_GetParamNormalized", "TrackFX_SetParam", "TrackFX_SetParamNormalized",
    "TrackFX_GetEnabled", "TrackFX_SetEnabled", "TrackFX_GetOffline", "TrackFX_SetOffline",
    "TrackFX_GetFXGUID", "TrackFX_GetNamedConfigParm", "TrackFX_SetNamedConfigParm",
    "TrackFX_AddByName", "TrackFX_Delete", "TrackFX_CopyToTrack", "TrackFX_EndParamEdit",
    "TakeFX_GetCount", "TakeFX_GetFXName", "TakeFX_GetNumParams", "TakeFX_GetParam",
    "TakeFX_GetParamNormalized", "TakeFX_SetParam", "TakeFX_SetParamNormalized",
    "TakeFX_GetEnabled", "TakeFX_SetEnabled", "TakeFX_GetOffline", "TakeFX_SetOffline",
    "TakeFX_GetFXGUID", "TakeFX_GetNamedConfigParm", "TakeFX_SetNamedConfigParm",
    "TakeFX_AddByName", "TakeFX_Delete", "TakeFX_CopyToTake", "TakeFX_EndParamEdit",
    "CountTrackEnvelopes", "GetTrackEnvelope", "GetTrackEnvelopeByName", "GetFXEnvelope",
    "CountTakeEnvelopes", "GetTakeEnvelope", "GetTakeEnvelopeByName", "GetEnvelopeName",
    "GetEnvelopeInfo_Value", "GetSetEnvelopeInfo_String",
    "CountEnvelopePoints", "CountEnvelopePointsEx", "GetEnvelopePoint", "GetEnvelopePointEx",
    "SetEnvelopePoint", "SetEnvelopePointEx", "InsertEnvelopePoint", "InsertEnvelopePointEx",
    "DeleteEnvelopePointRange", "DeleteEnvelopePointRangeEx", "Envelope_SortPoints", "Envelope_SortPointsEx",
    "CountAutomationItems", "InsertAutomationItem", "GetSetAutomationItemInfo", "GetSetAutomationItemInfo_String",
    "GetTrackNumSends", "CreateTrackSend", "RemoveTrackSend", "GetTrackSendInfo_Value", "SetTrackSendInfo_Value",
    "CountProjectMarkers", "EnumProjectMarkers3", "AddProjectMarker2", "SetProjectMarker3", "DeleteProjectMarker",
    "CountTempoTimeSigMarkers", "GetTempoTimeSigMarker", "SetTempoTimeSigMarker", "DeleteTempoTimeSigMarker",
    "GetSet_LoopTimeRange2", "GetSetProjectGrid", "GetSetProjectInfo", "GetSetProjectInfo_String",
    "GetProjectStateChangeCount", "GetCursorPositionEx", "SetEditCurPos2", "GetSetRepeatEx",
    "GetTrackStateChunk", "SetTrackStateChunk",
    "GetItemStateChunk", "SetItemStateChunk", "GetEnvelopeStateChunk", "SetEnvelopeStateChunk"
  };
  return names;
}
}
