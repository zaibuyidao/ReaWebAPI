/// <reference path="./runtime-api.d.ts" />
/** Legacy whole-value export limit. Streaming writers advertise their own limits. */
export const MAX_DOWNLOAD_BYTES: number;
interface FileHost {
  dialog: { saveFile(options: { title: string; initialPath: string }): Promise<string | null> };
  fs: {
    writeBinary(path: string, bytes: Uint8Array, options: { overwrite: boolean }): Promise<unknown>;
    openWrite?(path: string, options?: { overwrite?: boolean; expectedBytes?: number }): Promise<ReaWebFileWriter>;
  };
  system?: { getCapabilities(): Promise<{ runtime?: { fs?: { streamingWrite?: unknown } } }> };
}
export interface ExportProgress {
  bytes: number;
  total: number | null;
  state: 'writing' | 'committing' | 'committed';
}
export interface ExportOptions {
  signal?: AbortSignal;
  onProgress?: (progress: ExportProgress) => void;
}
/** Returns the selected path, or null when the save dialog is cancelled. */
export function saveBlob(blob: Blob, filename: string, host?: FileHost, options?: ExportOptions): Promise<string | null>;
export function installDownloadHandler(options?: {
  onError?: (error: unknown) => void;
  onProgress?: (progress: ExportProgress) => void;
  host?: FileHost;
  document?: Document;
}): () => void;
