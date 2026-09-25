/// <reference path="./reaper-api.generated.d.ts" />
/// <reference path="./runtime-api.d.ts" />

interface MediaTrackHandle {
  readonly type: 'MediaTrack';
  readonly id: string;
}
interface MediaItemHandle { readonly type: 'MediaItem'; readonly id: string; }
interface MediaItem_TakeHandle { readonly type: 'MediaItem_Take'; readonly id: string; }
interface TrackEnvelopeHandle { readonly type: 'TrackEnvelope'; readonly id: string; }
interface ReaProjectHandle { readonly type: 'ReaProject'; readonly id: string; }
interface PCM_sourceHandle { readonly type: 'PCM_source'; readonly id: string; }
interface AudioAccessorHandle { readonly type: 'AudioAccessor'; readonly id: string; }
interface HWNDHandle { readonly type: 'HWND'; readonly id: string; }
interface KbdSectionInfoHandle { readonly type: 'KbdSectionInfo'; readonly id: string; }
interface joystick_deviceHandle { readonly type: 'joystick_device'; readonly id: string; }
interface IReaperControlSurfaceHandle { readonly type: 'IReaperControlSurface'; readonly id: string; }
interface ProjectMarkerHandle { readonly type: 'ProjectMarker'; readonly id: string; }
type ReaWebHandle = MediaTrackHandle | MediaItemHandle | MediaItem_TakeHandle | TrackEnvelopeHandle | ReaProjectHandle | PCM_sourceHandle | AudioAccessorHandle | HWNDHandle | KbdSectionInfoHandle | joystick_deviceHandle | IReaperControlSurfaceHandle | ProjectMarkerHandle;

type ReaWebTrackKey = 'D_VOL' | 'D_PAN' | 'B_MUTE' | 'I_SOLO' | 'I_CUSTOMCOLOR';
type ReaWebContinuousTrackKey = Exclude<ReaWebTrackKey, 'I_CUSTOMCOLOR'>;
/** Bridge failures reject with this shape. Native false/0/null results remain results. */
interface ReaWebError extends Error {
  readonly code: string;
  readonly details?: unknown;
}
interface ReaWebRuntimeInfo {
  contract: 1;
  mode: 'app-http' | 'dev-http';
  appId: string;
  origin: string;
  storageIsolation: 'origin';
  localResources: boolean;
}
interface ReaWebCapabilities {
  webRuntime: ReaWebRuntimeInfo;
  version: string;
  protocol: 1;
  methods: string[];
  api: {
    schemaVersion: number;
    reaperVersion: string;
    catalogueHash: string;
    official: number;
    implemented: number;
    compatible: number;
    partial: number;
    missing: number;
    available: number;
    unavailable: string[];
    availableMethods: string[];
    bindings: Record<string, { minArgs: number; maxArgs: number; compatibility: 'compatible' | 'partial'; limitations: string[] }>;
  };
  projectScope: 'all';
  events: (keyof ReaWebEvents)[];
  batchMethods: ReaWebBatchMethod[];
  limits: { requestBytes: number; batchCalls: number; pendingCalls: number };
}
interface ReaWebWindowState {
  id: number;
  title: string;
  docked: boolean;
  visible: boolean;
  focused: boolean;
  keyboardCapture: boolean;
  iconVisible: boolean;
}
interface ReaWebEvents {
  projectchange: { projectEpoch: number; changeCount: number };
  selectionchange: { projectEpoch: number; revision: number; count: number };
  itemselectionchange: { projectEpoch: number; revision: number; count: number };
  /** Active takes of selected items, not selected MIDI notes. */
  takeselectionchange: { projectEpoch: number; revision: number; count: number };
  transportchange: { projectEpoch: number; available: boolean; state?: number; position?: number; cursor?: number; tempo?: number };
  fxchange: { projectEpoch: number; changeCount: number; available: boolean; focused: ReaWebFXState | null; touched: ReaWebFXState | null };
  windowstatechange: ReaWebWindowState;
}
interface ReaWebDiagnostics {
  webRuntime: ReaWebRuntimeInfo;
  version: string;
  protocol: number;
  backend: 'WebView2' | 'WKWebView' | 'WebKitGTK';
  browserVersion: string;
  window: ReaWebWindowState;
  stage: 'loading' | 'ready' | 'closing' | 'closed' | 'failed';
  documentGeneration: number;
  projectEpoch: number;
  pendingCalls: number;
  queuedCalls: number;
  processedCalls: number;
  lastError: string;
  /** Soft budget. A native call cannot be preempted. */
  schedulerBudgetMs: number;
}
interface ReaWebFXState {
  trackIndex: number; itemIndex: number; takeIndex: number; fxIndex: number;
  parameter: number | null; focused: boolean | null; value: number | null;
}
/** A top-level argument may refer to a previous batch result, optionally traversing its tuple or object. */
interface ReaWebBatchReference { $ref: number; path?: (string | number)[]; }
type ReaWebBatchMethod =
  | 'APIExists'
  | 'AddMediaItemToTrack'
  | 'AddProjectMarker2'
  | 'AddTakeToMediaItem'
  | 'AnyTrackSolo'
  | 'AudioAccessorStateChanged'
  | 'Audio_IsPreBuffer'
  | 'Audio_IsRunning'
  | 'CSurf_GetTouchState'
  | 'CSurf_NumTracks'
  | 'CSurf_TrackFromID'
  | 'CSurf_TrackToID'
  | 'ColorFromNative'
  | 'ColorToNative'
  | 'CountActionShortcuts'
  | 'CountAutomationItems'
  | 'CountEnvelopePoints'
  | 'CountEnvelopePointsEx'
  | 'CountMediaItems'
  | 'CountProjectMarkers'
  | 'CountSelectedMediaItems'
  | 'CountSelectedTracks'
  | 'CountSelectedTracks2'
  | 'CountTCPFXParms'
  | 'CountTakeEnvelopes'
  | 'CountTakes'
  | 'CountTempoTimeSigMarkers'
  | 'CountTrackEnvelopes'
  | 'CountTrackMediaItems'
  | 'CountTracks'
  | 'CreateNewMIDIItemInProj'
  | 'CreateTrackSend'
  | 'DB2SLIDER'
  | 'DeleteEnvelopePointRange'
  | 'DeleteEnvelopePointRangeEx'
  | 'DeleteProjectMarker'
  | 'DeleteTempoTimeSigMarker'
  | 'DeleteTrack'
  | 'DeleteTrackMediaItem'
  | 'DockGetPosition'
  | 'DockIsChildOfDock'
  | 'EnsureNotCompletelyOffscreen'
  | 'EnumPitchShiftModes'
  | 'EnumPitchShiftSubModes'
  | 'EnumProjExtState'
  | 'EnumProjectMarkers'
  | 'EnumProjectMarkers2'
  | 'EnumProjectMarkers3'
  | 'EnumRegionRenderMatrix'
  | 'EnumTrackMIDIProgramNames'
  | 'EnumTrackMIDIProgramNamesEx'
  | 'Envelope_Evaluate'
  | 'Envelope_FormatValue'
  | 'Envelope_GetParentTake'
  | 'Envelope_GetParentTrack'
  | 'Envelope_SortPoints'
  | 'Envelope_SortPointsEx'
  | 'FindTempoTimeSigMarker'
  | 'GetActionShortcutDesc'
  | 'GetActiveTake'
  | 'GetAppVersion'
  | 'GetArmedCommand'
  | 'GetAudioAccessorEndTime'
  | 'GetAudioAccessorHash'
  | 'GetAudioAccessorStartTime'
  | 'GetAudioDeviceInfo'
  | 'GetConfigWantsDock'
  | 'GetCursorContext'
  | 'GetCursorContext2'
  | 'GetCursorPosition'
  | 'GetCursorPositionEx'
  | 'GetDisplayedMediaItemColor'
  | 'GetDisplayedMediaItemColor2'
  | 'GetEnvelopeInfo_Value'
  | 'GetEnvelopeName'
  | 'GetEnvelopePoint'
  | 'GetEnvelopePointByTime'
  | 'GetEnvelopePointByTimeEx'
  | 'GetEnvelopePointEx'
  | 'GetEnvelopeScalingMode'
  | 'GetEnvelopeStateChunk'
  | 'GetEnvelopeUIState'
  | 'GetExePath'
  | 'GetExtState'
  | 'GetFXEnvelope'
  | 'GetFocusedFX'
  | 'GetFocusedFX2'
  | 'GetGlobalAutomationOverride'
  | 'GetHZoomLevel'
  | 'GetInputActivityLevel'
  | 'GetInputChannelName'
  | 'GetInputOutputLatency'
  | 'GetItemEditingTime2'
  | 'GetItemFromPoint'
  | 'GetItemProjectContext'
  | 'GetItemStateChunk'
  | 'GetLastColorThemeFile'
  | 'GetLastMarkerAndCurRegion'
  | 'GetLastTouchedFX'
  | 'GetLastTouchedTrack'
  | 'GetMIDIInputName'
  | 'GetMIDIInputNameNoAlias'
  | 'GetMIDIOutputName'
  | 'GetMIDIOutputNameNoAlias'
  | 'GetMainHwnd'
  | 'GetMasterMuteSoloFlags'
  | 'GetMasterTrack'
  | 'GetMasterTrackVisibility'
  | 'GetMaxMidiInputs'
  | 'GetMaxMidiOutputs'
  | 'GetMediaItem'
  | 'GetMediaItemInfo_Value'
  | 'GetMediaItemNumTakes'
  | 'GetMediaItemTake'
  | 'GetMediaItemTakeByGUID'
  | 'GetMediaItemTakeInfo_Value'
  | 'GetMediaItemTake_Item'
  | 'GetMediaItemTake_Source'
  | 'GetMediaItemTake_Track'
  | 'GetMediaItemTrack'
  | 'GetMediaItem_Track'
  | 'GetMediaSourceFileName'
  | 'GetMediaSourceLength'
  | 'GetMediaSourceNumChannels'
  | 'GetMediaSourceParent'
  | 'GetMediaSourceSampleRate'
  | 'GetMediaSourceType'
  | 'GetMediaTrackInfo_Value'
  | 'GetMixerScroll'
  | 'GetMouseModifier'
  | 'GetMousePosition'
  | 'GetNumAudioInputs'
  | 'GetNumAudioOutputs'
  | 'GetNumMIDIInputs'
  | 'GetNumMIDIOutputs'
  | 'GetNumRegionsOrMarkers'
  | 'GetNumTakeMarkers'
  | 'GetNumTracks'
  | 'GetOS'
  | 'GetOutputChannelName'
  | 'GetOutputLatency'
  | 'GetParentTrack'
  | 'GetPlayPosition'
  | 'GetPlayPosition2'
  | 'GetPlayPosition2Ex'
  | 'GetPlayPositionEx'
  | 'GetPlayState'
  | 'GetPlayStateEx'
  | 'GetProjExtState'
  | 'GetProjectLength'
  | 'GetProjectName'
  | 'GetProjectPath'
  | 'GetProjectPathEx'
  | 'GetProjectStateChangeCount'
  | 'GetProjectTimeOffset'
  | 'GetProjectTimeSignature'
  | 'GetProjectTimeSignature2'
  | 'GetRegionOrMarker'
  | 'GetRegionOrMarkerInfo_Value'
  | 'GetResourcePath'
  | 'GetSelectedEnvelope'
  | 'GetSelectedMediaItem'
  | 'GetSelectedTrack'
  | 'GetSelectedTrack2'
  | 'GetSelectedTrackEnvelope'
  | 'GetSetAutomationItemInfo'
  | 'GetSetAutomationItemInfo_String'
  | 'GetSetEnvelopeInfo_String'
  | 'GetSetMediaItemInfo_String'
  | 'GetSetMediaItemTakeInfo_String'
  | 'GetSetMediaTrackInfo_String'
  | 'GetSetProjectGrid'
  | 'GetSetProjectInfo'
  | 'GetSetProjectInfo_String'
  | 'GetSetRepeatEx'
  | 'GetSet_LoopTimeRange2'
  | 'GetTCPFXParm'
  | 'GetTake'
  | 'GetTakeEnvelope'
  | 'GetTakeEnvelopeByName'
  | 'GetTakeMarker'
  | 'GetTakeName'
  | 'GetTakeNumStretchMarkers'
  | 'GetTakeStretchMarker'
  | 'GetTakeStretchMarkerSlope'
  | 'GetTempoMatchPlayRate'
  | 'GetTempoTimeSigMarker'
  | 'GetThemeColor'
  | 'GetThingFromPoint'
  | 'GetToggleCommandState'
  | 'GetToggleCommandStateEx'
  | 'GetTooltipWindow'
  | 'GetTouchedOrFocusedFX'
  | 'GetTrack'
  | 'GetTrackAutomationMode'
  | 'GetTrackColor'
  | 'GetTrackDepth'
  | 'GetTrackEnvelope'
  | 'GetTrackEnvelopeByChunkName'
  | 'GetTrackEnvelopeByName'
  | 'GetTrackFromPoint'
  | 'GetTrackGUID'
  | 'GetTrackMIDILyrics'
  | 'GetTrackMIDINoteName'
  | 'GetTrackMIDINoteNameEx'
  | 'GetTrackMIDINoteRange'
  | 'GetTrackMediaItem'
  | 'GetTrackName'
  | 'GetTrackNumMediaItems'
  | 'GetTrackNumSends'
  | 'GetTrackReceiveName'
  | 'GetTrackReceiveUIMute'
  | 'GetTrackReceiveUIVolPan'
  | 'GetTrackSendInfo_Value'
  | 'GetTrackSendName'
  | 'GetTrackSendUIMute'
  | 'GetTrackSendUIVolPan'
  | 'GetTrackState'
  | 'GetTrackStateChunk'
  | 'GetTrackUIMute'
  | 'GetTrackUIPan'
  | 'GetTrackUIVolPan'
  | 'GetUnderrunTime'
  | 'HasExtState'
  | 'HasTrackMIDIPrograms'
  | 'HasTrackMIDIProgramsEx'
  | 'InsertAutomationItem'
  | 'InsertEnvelopePoint'
  | 'InsertEnvelopePointEx'
  | 'InsertTrackAtIndex'
  | 'IsMediaExtension'
  | 'IsMediaItemSelected'
  | 'IsProjectDirty'
  | 'IsTrackSelected'
  | 'IsTrackVisible'
  | 'LICE_ClipLine'
  | 'LocalizeString'
  | 'MIDIEditor_EnumTakes'
  | 'MIDIEditor_GetActive'
  | 'MIDIEditor_GetMode'
  | 'MIDIEditor_GetSetting_int'
  | 'MIDIEditor_GetSetting_str'
  | 'MIDIEditor_GetTake'
  | 'MIDI_CountEvts'
  | 'MIDI_DeleteCC'
  | 'MIDI_DeleteEvt'
  | 'MIDI_DeleteNote'
  | 'MIDI_DeleteTextSysexEvt'
  | 'MIDI_EnumSelCC'
  | 'MIDI_EnumSelEvts'
  | 'MIDI_EnumSelNotes'
  | 'MIDI_EnumSelTextSysexEvts'
  | 'MIDI_GetAllEvts'
  | 'MIDI_GetCC'
  | 'MIDI_GetCCShape'
  | 'MIDI_GetEvt'
  | 'MIDI_GetGrid'
  | 'MIDI_GetHash'
  | 'MIDI_GetNote'
  | 'MIDI_GetPPQPosFromProjQN'
  | 'MIDI_GetPPQPosFromProjTime'
  | 'MIDI_GetPPQPos_EndOfMeasure'
  | 'MIDI_GetPPQPos_StartOfMeasure'
  | 'MIDI_GetProjQNFromPPQPos'
  | 'MIDI_GetProjTimeFromPPQPos'
  | 'MIDI_GetRecentInputEvent'
  | 'MIDI_GetScale'
  | 'MIDI_GetTextSysexEvt'
  | 'MIDI_GetTrackHash'
  | 'MIDI_InsertCC'
  | 'MIDI_InsertEvt'
  | 'MIDI_InsertNote'
  | 'MIDI_InsertTextSysexEvt'
  | 'MIDI_SelectAll'
  | 'MIDI_SetAllEvts'
  | 'MIDI_SetCC'
  | 'MIDI_SetCCShape'
  | 'MIDI_SetEvt'
  | 'MIDI_SetNote'
  | 'MIDI_SetTextSysexEvt'
  | 'MIDI_Sort'
  | 'Master_GetPlayRate'
  | 'Master_GetPlayRateAtTime'
  | 'Master_GetTempo'
  | 'Master_NormalizePlayRate'
  | 'Master_NormalizeTempo'
  | 'MediaExplorerGetLastPlayedFileInfo'
  | 'MediaItemDescendsFromTrack'
  | 'Menu_GetHash'
  | 'MoveMediaItemToTrack'
  | 'NamedCommandLookup'
  | 'PCM_Sink_Enum'
  | 'PCM_Sink_GetExtension'
  | 'PCM_Source_GetSectionInfo'
  | 'RemoveTrackSend'
  | 'Resample_EnumModes'
  | 'ResolveWildcards'
  | 'ReverseNamedCommandLookup'
  | 'SLIDER2DB'
  | 'ScaleFromEnvelopeMode'
  | 'ScaleToEnvelopeMode'
  | 'SectionFromUniqueID'
  | 'SelectAllMediaItems'
  | 'SetActiveTake'
  | 'SetEditCurPos2'
  | 'SetEnvelopePoint'
  | 'SetEnvelopePointEx'
  | 'SetEnvelopeStateChunk'
  | 'SetItemStateChunk'
  | 'SetMediaItemInfo_Value'
  | 'SetMediaItemLength'
  | 'SetMediaItemPosition'
  | 'SetMediaItemSelected'
  | 'SetMediaItemTakeInfo_Value'
  | 'SetMediaTrackInfo_Value'
  | 'SetOnlyTrackSelected'
  | 'SetProjectMarker3'
  | 'SetTempoTimeSigMarker'
  | 'SetTrackColor'
  | 'SetTrackSelected'
  | 'SetTrackSendInfo_Value'
  | 'SetTrackStateChunk'
  | 'SnapToGrid'
  | 'Splash_GetWnd'
  | 'SplitMediaItem'
  | 'TakeFX_AddByName'
  | 'TakeFX_CopyToTake'
  | 'TakeFX_Delete'
  | 'TakeFX_EndParamEdit'
  | 'TakeFX_FormatParamValue'
  | 'TakeFX_FormatParamValueNormalized'
  | 'TakeFX_GetChainVisible'
  | 'TakeFX_GetCount'
  | 'TakeFX_GetEnabled'
  | 'TakeFX_GetFXGUID'
  | 'TakeFX_GetFXName'
  | 'TakeFX_GetFloatingWindow'
  | 'TakeFX_GetFormattedParamValue'
  | 'TakeFX_GetIOSize'
  | 'TakeFX_GetNamedConfigParm'
  | 'TakeFX_GetNumParams'
  | 'TakeFX_GetOffline'
  | 'TakeFX_GetOpen'
  | 'TakeFX_GetParam'
  | 'TakeFX_GetParamEx'
  | 'TakeFX_GetParamFromIdent'
  | 'TakeFX_GetParamIdent'
  | 'TakeFX_GetParamName'
  | 'TakeFX_GetParamNormalized'
  | 'TakeFX_GetParamSectionName'
  | 'TakeFX_GetParameterStepSizes'
  | 'TakeFX_GetPinMappings'
  | 'TakeFX_GetPreset'
  | 'TakeFX_GetPresetIndex'
  | 'TakeFX_GetUserPresetFilename'
  | 'TakeFX_SetEnabled'
  | 'TakeFX_SetNamedConfigParm'
  | 'TakeFX_SetOffline'
  | 'TakeFX_SetParam'
  | 'TakeFX_SetParamNormalized'
  | 'TakeIsMIDI'
  | 'ThemeLayout_GetLayout'
  | 'ThemeLayout_GetParameter'
  | 'TimeMap2_GetDividedBpmAtTime'
  | 'TimeMap2_GetNextChangeTime'
  | 'TimeMap2_QNToTime'
  | 'TimeMap2_beatsToTime'
  | 'TimeMap2_timeToBeats'
  | 'TimeMap2_timeToQN'
  | 'TimeMap_GetDividedBpmAtTime'
  | 'TimeMap_GetMeasureInfo'
  | 'TimeMap_GetMetronomePattern'
  | 'TimeMap_GetTimeSigAtTime'
  | 'TimeMap_QNToMeasures'
  | 'TimeMap_QNToTime'
  | 'TimeMap_QNToTime_abs'
  | 'TimeMap_curFrameRate'
  | 'TimeMap_timeToQN'
  | 'TimeMap_timeToQN_abs'
  | 'TrackFX_AddByName'
  | 'TrackFX_CopyToTrack'
  | 'TrackFX_Delete'
  | 'TrackFX_EndParamEdit'
  | 'TrackFX_FormatParamValue'
  | 'TrackFX_FormatParamValueNormalized'
  | 'TrackFX_GetChainVisible'
  | 'TrackFX_GetCount'
  | 'TrackFX_GetEQBandEnabled'
  | 'TrackFX_GetEQParam'
  | 'TrackFX_GetEnabled'
  | 'TrackFX_GetFXGUID'
  | 'TrackFX_GetFXName'
  | 'TrackFX_GetFloatingWindow'
  | 'TrackFX_GetFormattedParamValue'
  | 'TrackFX_GetIOSize'
  | 'TrackFX_GetInstrument'
  | 'TrackFX_GetNamedConfigParm'
  | 'TrackFX_GetNumParams'
  | 'TrackFX_GetOffline'
  | 'TrackFX_GetOpen'
  | 'TrackFX_GetParam'
  | 'TrackFX_GetParamEx'
  | 'TrackFX_GetParamFromIdent'
  | 'TrackFX_GetParamIdent'
  | 'TrackFX_GetParamName'
  | 'TrackFX_GetParamNormalized'
  | 'TrackFX_GetParamSectionName'
  | 'TrackFX_GetParameterStepSizes'
  | 'TrackFX_GetPinMappings'
  | 'TrackFX_GetPreset'
  | 'TrackFX_GetPresetIndex'
  | 'TrackFX_GetRecChainVisible'
  | 'TrackFX_GetRecCount'
  | 'TrackFX_GetUserPresetFilename'
  | 'TrackFX_SetEnabled'
  | 'TrackFX_SetNamedConfigParm'
  | 'TrackFX_SetOffline'
  | 'TrackFX_SetParam'
  | 'TrackFX_SetParamNormalized'
  | 'TrackList_AdjustWindows'
  | 'Track_GetPeakInfo'
  | 'Undo_CanRedo2'
  | 'Undo_CanUndo2'
  | 'Undo_GetCurEntry'
  | 'Undo_GetEntryDesc'
  | 'Undo_GetEntryTime'
  | 'Undo_GetNumEntries'
  | 'Undo_IsEntryAltTree'
  | 'UpdateArrange'
  | 'UpdateItemInProject'
  | 'ValidatePtr'
  | 'ValidatePtr2'
  | 'format_timestr'
  | 'format_timestr_len'
  | 'format_timestr_pos'
  | 'genGuid'
  | 'get_ini_file'
  | 'guidToString'
  | 'joystick_enum'
  | 'joystick_getaxis'
  | 'joystick_getbuttonmask'
  | 'joystick_getinfo'
  | 'joystick_getpov'
  | 'kbd_enumerateActions'
  | 'kbd_getTextFromCmd'
  | 'mkpanstr'
  | 'mkvolpanstr'
  | 'mkvolstr'
  | 'my_getViewport'
  | 'parse_timestr'
  | 'parse_timestr_len'
  | 'parse_timestr_pos'
  | 'parsepanstr'
  | 'relative_fn'
  | 'stringToGuid'
  | 'time_precise';
type ReaWebBatchArgs<T extends unknown[]> = { [I in keyof T]: T[I] | ReaWebBatchReference };
type ReaWebBatchCall = { [M in ReaWebBatchMethod]: { method: M; args: ReaWebBatchArgs<Parameters<ReaWebAPI[M]>> } }[ReaWebBatchMethod];
declare const reawebBatchValue: unique symbol;
/** Opaque deferred result, owned by one synchronous batch callback. */
interface ReaWebBatchDeferred<T> { readonly [reawebBatchValue]: T; }
type ReaWebBatchValue<T> = ReaWebBatchDeferred<T> &
  (T extends readonly unknown[] ? { readonly [I in keyof T]: ReaWebBatchValue<T[I]> } : unknown);
type ReaWebBatchBuilderArgs<T extends unknown[]> = { [I in keyof T]: T[I] | ReaWebBatchDeferred<T[I] | null> };
type ReaWebBatchNativeResult<T> = [T] extends [void] ? null : T;
/** Mirror signatures restricted to the native batch whitelist. Nullable deferred handles are validated by the host. */
type ReaWebBatchBuilder = {
  readonly [M in ReaWebBatchMethod]: (...args: ReaWebBatchBuilderArgs<Parameters<ReaWebAPI[M]>>) =>
    ReaWebBatchValue<ReaWebBatchNativeResult<Awaited<ReturnType<ReaWebAPI[M]>>>>;
};
type ReaWebBatchResolved<T> = T extends ReaWebBatchDeferred<infer R> ? R
  : T extends Uint8Array ? T : T extends object ? { [K in keyof T]: ReaWebBatchResolved<T[K]> } : T;
type ReaWebBatchResult<T> = T extends void ? unknown[] : ReaWebBatchResolved<T>;
interface ReaWebFileInfo { path: string; exists: boolean; type: 'file' | 'directory' | 'other'; size: number | null; }
interface ReaWebDirectoryEntry extends ReaWebFileInfo { name: string; }
declare const reaper: Readonly<ReaWebAPI>;
interface Window { readonly reaper: Readonly<ReaWebAPI>; }
