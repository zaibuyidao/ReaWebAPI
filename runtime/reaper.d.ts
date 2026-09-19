/// <reference path="./reaper-api.generated.d.ts" />

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
interface ReaWebFileInfo { path: string; exists: boolean; type: 'file' | 'directory' | 'other'; size: number | null; }
interface ReaWebDirectoryEntry extends ReaWebFileInfo { name: string; }
interface ReaWebAPI {
  /** Capacity for fixed native output buffers in this document (4096..16777216 bytes). Default 65536.
   * NeedBig buffers grow through REAPER's allocator automatically. */
  ReaWeb_SetBufferSize(bytes: number): Promise<number>;
  /** Resolves after the native protocol handshake. API calls wait for this automatically. */
  readonly ready: Promise<Readonly<ReaWebCapabilities & { windowId: number; projectEpoch: number }>>;
  ReaWebOpen(path: string): Promise<number>;
  ReaWeb_Close(): Promise<boolean>;
  ReaWeb_DevTools(): Promise<boolean>;
  ReaWeb_SetDocked(docked: boolean): Promise<boolean>;
  ReaWeb_IsDocked(): Promise<boolean>;
  ReaWeb_Focus(): Promise<boolean>;
  ReaWeb_SetTitle(title: string): Promise<boolean>;
  /** Default true. False allows REAPER's normal global shortcut policy for this window. */
  ReaWeb_SetKeyboardCapture(capture: boolean): Promise<boolean>;
  ReaWeb_GetWindowState(): Promise<ReaWebWindowState>;
  ReaWeb_GetDiagnostics(): Promise<ReaWebDiagnostics>;
  ReaWeb_GetCapabilities(): Promise<ReaWebCapabilities>;
  /** Receives the initial snapshot and coalesced changes. Dispose is idempotent. */
  ReaWeb_On<K extends keyof ReaWebEvents>(name: K, callback: (state: ReaWebEvents[K]) => void): Promise<() => Promise<void>>;
  /** 1–128 synchronous calls on the current project. Literal arguments are validated up front;
   * references are resolved and validated before their call. See capabilities.batchMethods.
   * BATCH_FAILED includes completed/results/cause; completed writes are not rolled back.
   */
  ReaWeb_Batch(calls: ReaWebBatchCall[], options?: { undoLabel?: string }): Promise<unknown[]>;
  /** Explicitly trust a loopback HTTP server; regular ReaWebOpen stays local-file only. */
  ReaWeb_OpenDev(url: string): Promise<number>;
  /** One managed gesture at a time. Ends on close, reload, project change or after 30 seconds.
   * Does not hold PreventUIRefresh across browser events. Uses the reviewed batch API set. End before batching. */
  ReaWeb_BeginUndo(label: string): Promise<string>;
  ReaWeb_EndUndo(token: string): Promise<boolean>;
  ReaWeb_WithUndo<T>(label: string, callback: () => T | Promise<T>): Promise<T>;
  /** Files run on the worker, relative to the entry directory. Absolute local paths are allowed. */
  ReaWeb_ReadFile(path: string, options?: { encoding?: 'utf8' }): Promise<string>;
  ReaWeb_ReadFile(path: string, options: { encoding: 'binary' }): Promise<Uint8Array>;
  /** Atomic replacement; overwrite defaults to false. Parent directory must exist. Limit 16 MiB. */
  ReaWeb_WriteFile(path: string, data: string, options?: { encoding?: 'utf8'; overwrite?: boolean }): Promise<{ path: string; bytes: number }>;
  ReaWeb_WriteFile(path: string, data: Uint8Array, options: { encoding: 'binary'; overwrite?: boolean }): Promise<{ path: string; bytes: number }>;
  ReaWeb_Stat(path: string): Promise<ReaWebFileInfo>;
  ReaWeb_ReadDirectory(path: string): Promise<ReaWebDirectoryEntry[]>;
  ReaWeb_MakeDirectory(path: string, options?: { recursive?: boolean }): Promise<boolean>;
  ReaWeb_ClipboardReadText(): Promise<string>;
  ReaWeb_ClipboardWriteText(text: string): Promise<boolean>;
  /** Opens http/https/mailto using the operating system; never navigates the privileged page. */
  ReaWeb_OpenExternal(url: string): Promise<boolean>;
  /** Explicit coalescing: one in-flight write and the latest waiting value per track/key.
   * Superseded values settle without being sent. Does not create an Undo gesture.
   */
  ReaWeb_SetTrackValueLatest(track: MediaTrackHandle, key: ReaWebContinuousTrackKey, value: number): Promise<{ applied: boolean; superseded: boolean }>;
}
declare const reaper: Readonly<ReaWebAPI>;
interface Window { readonly reaper: Readonly<ReaWebAPI>; }
