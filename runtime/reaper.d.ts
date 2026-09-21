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
  storageIsolation: 'app-profile';
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
  | 'AddMediaItemToTrack'
  | 'AddProjectMarker2'
  | 'AddTakeToMediaItem'
  | 'CountAutomationItems'
  | 'CountEnvelopePoints'
  | 'CountEnvelopePointsEx'
  | 'CountMediaItems'
  | 'CountProjectMarkers'
  | 'CountSelectedMediaItems'
  | 'CountSelectedTracks'
  | 'CountTakeEnvelopes'
  | 'CountTakes'
  | 'CountTempoTimeSigMarkers'
  | 'CountTrackEnvelopes'
  | 'CountTrackMediaItems'
  | 'CountTracks'
  | 'CreateNewMIDIItemInProj'
  | 'CreateTrackSend'
  | 'DeleteEnvelopePointRange'
  | 'DeleteEnvelopePointRangeEx'
  | 'DeleteProjectMarker'
  | 'DeleteTempoTimeSigMarker'
  | 'DeleteTrack'
  | 'DeleteTrackMediaItem'
  | 'EnumProjectMarkers3'
  | 'Envelope_SortPoints'
  | 'Envelope_SortPointsEx'
  | 'GetActiveTake'
  | 'GetAppVersion'
  | 'GetCursorPositionEx'
  | 'GetDisplayedMediaItemColor'
  | 'GetDisplayedMediaItemColor2'
  | 'GetEnvelopeInfo_Value'
  | 'GetEnvelopeName'
  | 'GetEnvelopePoint'
  | 'GetEnvelopePointEx'
  | 'GetEnvelopeStateChunk'
  | 'GetFXEnvelope'
  | 'GetItemStateChunk'
  | 'GetMasterTrack'
  | 'GetMediaItem'
  | 'GetMediaItemInfo_Value'
  | 'GetMediaItemTakeInfo_Value'
  | 'GetMediaItemTake_Item'
  | 'GetMediaItemTrack'
  | 'GetMediaTrackInfo_Value'
  | 'GetProjectStateChangeCount'
  | 'GetSelectedMediaItem'
  | 'GetSelectedTrack'
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
  | 'GetTake'
  | 'GetTakeEnvelope'
  | 'GetTakeEnvelopeByName'
  | 'GetTakeName'
  | 'GetTempoTimeSigMarker'
  | 'GetTrack'
  | 'GetTrackColor'
  | 'GetTrackEnvelope'
  | 'GetTrackEnvelopeByName'
  | 'GetTrackGUID'
  | 'GetTrackMediaItem'
  | 'GetTrackName'
  | 'GetTrackNumSends'
  | 'GetTrackSendInfo_Value'
  | 'GetTrackStateChunk'
  | 'InsertAutomationItem'
  | 'InsertEnvelopePoint'
  | 'InsertEnvelopePointEx'
  | 'InsertTrackAtIndex'
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
  | 'MIDI_GetNote'
  | 'MIDI_GetPPQPosFromProjQN'
  | 'MIDI_GetPPQPosFromProjTime'
  | 'MIDI_GetProjQNFromPPQPos'
  | 'MIDI_GetProjTimeFromPPQPos'
  | 'MIDI_GetTextSysexEvt'
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
  | 'MoveMediaItemToTrack'
  | 'RemoveTrackSend'
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
  | 'SplitMediaItem'
  | 'TakeFX_AddByName'
  | 'TakeFX_CopyToTake'
  | 'TakeFX_Delete'
  | 'TakeFX_EndParamEdit'
  | 'TakeFX_GetCount'
  | 'TakeFX_GetEnabled'
  | 'TakeFX_GetFXGUID'
  | 'TakeFX_GetFXName'
  | 'TakeFX_GetNamedConfigParm'
  | 'TakeFX_GetNumParams'
  | 'TakeFX_GetOffline'
  | 'TakeFX_GetParam'
  | 'TakeFX_GetParamNormalized'
  | 'TakeFX_SetEnabled'
  | 'TakeFX_SetNamedConfigParm'
  | 'TakeFX_SetOffline'
  | 'TakeFX_SetParam'
  | 'TakeFX_SetParamNormalized'
  | 'TakeIsMIDI'
  | 'TrackFX_AddByName'
  | 'TrackFX_CopyToTrack'
  | 'TrackFX_Delete'
  | 'TrackFX_EndParamEdit'
  | 'TrackFX_GetCount'
  | 'TrackFX_GetEnabled'
  | 'TrackFX_GetFXGUID'
  | 'TrackFX_GetFXName'
  | 'TrackFX_GetNamedConfigParm'
  | 'TrackFX_GetNumParams'
  | 'TrackFX_GetOffline'
  | 'TrackFX_GetParam'
  | 'TrackFX_GetParamNormalized'
  | 'TrackFX_SetEnabled'
  | 'TrackFX_SetNamedConfigParm'
  | 'TrackFX_SetOffline'
  | 'TrackFX_SetParam'
  | 'TrackFX_SetParamNormalized'
  | 'TrackList_AdjustWindows'
  | 'UpdateArrange'
  | 'UpdateItemInProject';
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
