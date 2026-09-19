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
interface ReaWebCapabilities {
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
  windowstatechange: ReaWebWindowState;
}
interface ReaWebDiagnostics {
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
type ReaWebBatchCall =
  | { method: 'CountTracks' | 'CountSelectedTracks'; args: [0 | null] }
  | { method: 'GetTrack' | 'GetSelectedTrack'; args: [0 | null, number] }
  | { method: 'GetTrackName'; args: [MediaTrackHandle] }
  | { method: 'GetMediaTrackInfo_Value'; args: [MediaTrackHandle, ReaWebTrackKey] }
  | { method: 'SetMediaTrackInfo_Value'; args: [MediaTrackHandle, ReaWebTrackKey, number] }
  | { method: 'SetTrackColor'; args: [MediaTrackHandle, number] }
  | { method: 'GetAppVersion'; args: [] };
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
  /** 1–32 prevalidated calls. One synchronous Undo block when undoLabel is supplied.
   * BATCH_FAILED includes details.completed/results. Completed writes are not rolled back.
   * Calls cannot refer to earlier results in the same batch.
   */
  ReaWeb_Batch(calls: ReaWebBatchCall[], options?: { undoLabel?: string }): Promise<unknown[]>;
  /** Explicit coalescing: one in-flight write and the latest waiting value per track/key.
   * Superseded values settle without being sent. Does not create an Undo gesture.
   */
  ReaWeb_SetTrackValueLatest(track: MediaTrackHandle, key: ReaWebContinuousTrackKey, value: number): Promise<{ applied: boolean; superseded: boolean }>;
}
declare const reaper: Readonly<ReaWebAPI>;
interface Window { readonly reaper: Readonly<ReaWebAPI>; }
