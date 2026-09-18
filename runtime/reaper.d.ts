interface MediaTrackHandle {
  readonly type: 'MediaTrack';
  readonly id: string;
}
type ReaWebTrackKey = 'D_VOL' | 'D_PAN' | 'B_MUTE' | 'I_SOLO';
interface ReaWebCapabilities {
  version: string;
  protocol: 1;
  methods: string[];
  projectScope: 'current';
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
  | { method: 'CountTracks' | 'CountSelectedTracks'; args: [] | [0 | null] }
  | { method: 'GetTrack' | 'GetSelectedTrack'; args: [0 | null, number] }
  | { method: 'GetTrackName'; args: [MediaTrackHandle] }
  | { method: 'GetMediaTrackInfo_Value'; args: [MediaTrackHandle, ReaWebTrackKey] }
  | { method: 'SetMediaTrackInfo_Value'; args: [MediaTrackHandle, ReaWebTrackKey, number] }
  | { method: 'GetAppVersion'; args: [] };
interface ReaWebAPI {
  /** Resolves after the native protocol handshake. API calls wait for this automatically. */
  readonly ready: Promise<Readonly<ReaWebCapabilities & { windowId: number; projectEpoch: number }>>;
  CountTracks(project?: 0 | null): Promise<number>;
  CountSelectedTracks(project?: 0 | null): Promise<number>;
  GetTrack(project: 0 | null, index: number): Promise<MediaTrackHandle | null>;
  GetSelectedTrack(project?: 0 | null, index?: number): Promise<MediaTrackHandle | null>;
  GetTrackName(track: MediaTrackHandle): Promise<string>;
  GetMediaTrackInfo_Value(track: MediaTrackHandle, key: ReaWebTrackKey): Promise<number>;
  SetMediaTrackInfo_Value(track: MediaTrackHandle, key: ReaWebTrackKey, value: number): Promise<boolean>;
  GetAppVersion(): Promise<string>;
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
  ReaWeb_SetTrackValueLatest(track: MediaTrackHandle, key: ReaWebTrackKey, value: number): Promise<{ applied: boolean; superseded: boolean }>;
}
declare const reaper: Readonly<ReaWebAPI>;
