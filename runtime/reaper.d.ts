/// <reference path="./runtime-api.d.ts" />

type ReaWebTrackKey = 'D_VOL' | 'D_PAN' | 'B_MUTE' | 'I_SOLO' | 'I_CUSTOMCOLOR';
type ReaWebContinuousTrackKey = Exclude<ReaWebTrackKey, 'I_CUSTOMCOLOR'>;
/** Bridge failures reject with this shape. Native false/0/null results remain results. */
interface ReaWebError extends Error {
  readonly code: string;
  readonly details?: unknown;
}
interface ReaWebRuntimeInfo {
  contract: 1;
  mode: 'app-virtual' | 'app-loopback' | 'dev-http';
  appId: string;
  origin: string;
  storageIsolation: 'origin';
  localResources: boolean;
  crossOriginIsolation: {
    requested: 'off' | 'required';
    /** null means the backend has not been verified. */
    supported: boolean | null;
    /** null until the current document reports its state. */
    active: boolean | null;
    reason: 'NOT_TESTED' | 'CROSS_ORIGIN_ISOLATION_UNSUPPORTED' | 'CROSS_ORIGIN_ISOLATION_FAILED' | null;
    stage: 'pending' | 'document' | 'failed';
    failureStage: 'document-policy' | 'handshake' | 'navigation' | null;
    documentGeneration: number;
  };
  browser: ReaWebBrowserCapabilities;
}
interface ReaWebBrowserCapabilities {
  secureContext: boolean | null;
  crossOriginIsolated: boolean | null;
  sharedArrayBuffer: boolean | null;
  wasmSharedMemory: boolean | null;
  worker: boolean | null;
  blobWorker: boolean | null;
  webgl2: boolean | null;
  errors?: Partial<Record<'worker' | 'blobWorker' | 'webgl2', string>>;
  workers?: Partial<Record<'worker' | 'blobWorker', {
    crossOriginIsolated: boolean; sharedArrayBuffer: boolean; wasmSharedMemory: boolean;
    sharedRoundTrip: boolean | null;
  }>>;
}
interface ReaWebManifestRuntime { crossOriginIsolation?: 'off' | 'required'; }
interface ReaWebCapabilities {
  webRuntime: ReaWebRuntimeInfo;
  version: string;
  protocol: 1;
  methods: string[];
  projectScope: 'all';
  events: (keyof ReaWebEvents)[];
  limits: { requestBytes: number; pendingCalls: number };
}
interface ReaWebWindowState {
  id: number;
  title: string;
  docked: boolean;
  visible: boolean;
  focused: boolean;
  keyboardCapture: boolean;
  shortcutRouting: boolean;
  /** Current Space routing state. JSON defaults: ordinary Apps false, JSFX Web UI true. */
  dawKeyboard: boolean;
  iconVisible: boolean;
  frameless: boolean;
  resizable: boolean;
  framelessActive: boolean;
  /** Requested radius in logical desktop units, 0..64. Zero clears custom clipping. */
  cornerRadius: number;
  cornerRadiusActive: boolean;
  /** Creation-time request. Transparent windows cannot be docked. */
  transparent: boolean;
  /** Native desktop Alpha has been established, independently of page CSS. */
  transparentActive: boolean;
  layer: ReaWebWindowLayer;
  layerActive: boolean;
  mouseMode: ReaWebMouseMode;
  mouseModeActive: boolean;
}
interface ReaWebEvents {
  'project-changed': { projectEpoch: number; changeCount: number };
  /** @deprecated Use `project-changed`. */
  projectchange: ReaWebEvents['project-changed'];
  'track-selection-changed': { projectEpoch: number; revision: number; count: number };
  /** @deprecated Use `track-selection-changed`. */
  selectionchange: ReaWebEvents['track-selection-changed'];
  'item-selection-changed': { projectEpoch: number; revision: number; count: number };
  /** @deprecated Use `item-selection-changed`. */
  itemselectionchange: ReaWebEvents['item-selection-changed'];
  'take-selection-changed': { projectEpoch: number; revision: number; count: number };
  /** @deprecated Use `take-selection-changed`. */
  takeselectionchange: ReaWebEvents['take-selection-changed'];
  'transport-changed': { projectEpoch: number; available: boolean; state?: number; position?: number; cursor?: number; tempo?: number };
  /** @deprecated Use `transport-changed`. */
  transportchange: ReaWebEvents['transport-changed'];
  'fx-focus-touch-state-changed': { projectEpoch: number; changeCount: number; available: boolean; focused: ReaWebFXState | null; touched: ReaWebFXState | null };
  /** @deprecated Use `fx-focus-touch-state-changed`. */
  fxchange: ReaWebEvents['fx-focus-touch-state-changed'];
  'window-state-changed': ReaWebWindowState;
  /** @deprecated Use `window-state-changed`. */
  windowstatechange: ReaWebEvents['window-state-changed'];
}
interface ReaWebDiagnostics {
  video: ReaWebVideoDiagnostics;
    /** Last accepted page report. Collection requires matching diagnostic settings and resets on enable/navigation. */
  bridgeHealth: {
      enabled: boolean; probePending: boolean; unresponsive: boolean;
    pagePending?: number; oldestAgeMs?: number; oldestStarted?: number; method?: string; timeoutMethod?: string;
    durationMs?: number; activeStreams?: number; streamPackets?: number;
    timeouts?: number; queueRejected?: number; sendFailures?: number;
    scriptErrors?: number; scriptLine?: number; scriptColumn?: number;
    promiseErrors?: number; callbackErrors?: number; streamErrors?: number; streamCloses?: number;
  };
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
interface ReaWebFileInfo { path: string; exists: boolean; type: 'file' | 'directory' | 'other'; size: number | null; }
interface ReaWebDirectoryEntry extends ReaWebFileInfo { name: string; }
declare const reaper: Readonly<ReaWebAPI>;
interface Window { readonly reaper: Readonly<ReaWebAPI>; }
