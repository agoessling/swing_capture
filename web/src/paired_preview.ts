import type { CameraRole, StationApi } from "./api.js";

export interface PreviewPairRequest {
  down_the_line: number;
  face_on: number;
}

export interface PreviewPair {
  sequences: PreviewPairRequest;
  urls: Record<CameraRole, string>;
}

export interface ObjectUrlFactory {
  createObjectURL(blob: Blob): string;
  revokeObjectURL(url: string): void;
}

export type PreviewImageDecoder = (url: string) => Promise<void>;

type PairHandler = (pair: PreviewPair) => void;
type ErrorHandler = (error: Error) => void;

function sameRequest(left: PreviewPairRequest | null, right: PreviewPairRequest): boolean {
  return (
    left !== null && left.down_the_line === right.down_the_line && left.face_on === right.face_on
  );
}

function errorMessage(caught: unknown): Error {
  return caught instanceof Error ? caught : new Error("Unknown preview-pair failure");
}

async function decodeImageUrl(url: string): Promise<void> {
  const image = document.createElement("img");
  image.src = url;
  if (typeof image.decode === "function") {
    await image.decode();
  }
}

export class PairedPreviewLoader {
  readonly #api: StationApi;
  readonly #onPair: PairHandler;
  readonly #onError: ErrorHandler;
  readonly #objectUrls: ObjectUrlFactory;
  readonly #decodeImage: PreviewImageDecoder;
  #currentRequest: PreviewPairRequest | null = null;
  #inFlightRequest: PreviewPairRequest | null = null;
  #pendingRequest: PreviewPairRequest | null = null;
  #currentUrls: string[] = [];
  #retiredUrls: string[] = [];
  #decodingUrls: string[] = [];
  #running = false;
  #stopped = false;

  constructor(
    api: StationApi,
    onPair: PairHandler,
    onError: ErrorHandler,
    objectUrls: ObjectUrlFactory = URL,
    decodeImage: PreviewImageDecoder = decodeImageUrl,
  ) {
    this.#api = api;
    this.#onPair = onPair;
    this.#onError = onError;
    this.#objectUrls = objectUrls;
    this.#decodeImage = decodeImage;
  }

  enqueue(request: PreviewPairRequest): void {
    if (
      this.#stopped ||
      request.down_the_line <= 0 ||
      request.face_on <= 0 ||
      sameRequest(this.#currentRequest, request) ||
      sameRequest(this.#inFlightRequest, request) ||
      sameRequest(this.#pendingRequest, request)
    ) {
      return;
    }
    this.#pendingRequest = request;
    void this.#pump();
  }

  stop(): void {
    if (this.#stopped) {
      return;
    }
    this.#stopped = true;
    this.#pendingRequest = null;
    this.#revoke(this.#retiredUrls);
    this.#revoke(this.#currentUrls);
    this.#revoke(this.#decodingUrls);
    this.#retiredUrls = [];
    this.#currentUrls = [];
    this.#decodingUrls = [];
  }

  async #pump(): Promise<void> {
    if (this.#running) {
      return;
    }
    this.#running = true;
    try {
      while (!this.#stopped && this.#pendingRequest !== null) {
        const request = this.#pendingRequest;
        this.#pendingRequest = null;
        this.#inFlightRequest = request;
        try {
          const [downTheLine, faceOn] = await Promise.all([
            this.#api.getPreview("down_the_line", request.down_the_line),
            this.#api.getPreview("face_on", request.face_on),
          ]);
          if (this.#stopped) {
            return;
          }
          const nextUrls = this.#createUrls(downTheLine, faceOn);
          this.#decodingUrls = Object.values(nextUrls);
          try {
            await Promise.all([
              this.#decodeImage(nextUrls.down_the_line),
              this.#decodeImage(nextUrls.face_on),
            ]);
          } catch (caught) {
            this.#revoke(this.#decodingUrls);
            this.#decodingUrls = [];
            throw caught;
          }
          if (this.#stopped) {
            return;
          }
          this.#revoke(this.#retiredUrls);
          this.#decodingUrls = [];
          this.#retiredUrls = this.#currentUrls;
          this.#currentUrls = Object.values(nextUrls);
          this.#currentRequest = request;
          this.#onPair({ sequences: request, urls: nextUrls });
        } catch (caught) {
          if (!this.#stopped) {
            this.#onError(errorMessage(caught));
          }
        } finally {
          this.#inFlightRequest = null;
        }
      }
    } finally {
      this.#running = false;
      if (!this.#stopped && this.#pendingRequest !== null) {
        void this.#pump();
      }
    }
  }

  #createUrls(downTheLine: Blob, faceOn: Blob): Record<CameraRole, string> {
    const downTheLineUrl = this.#objectUrls.createObjectURL(downTheLine);
    try {
      return {
        down_the_line: downTheLineUrl,
        face_on: this.#objectUrls.createObjectURL(faceOn),
      };
    } catch (caught) {
      this.#objectUrls.revokeObjectURL(downTheLineUrl);
      throw caught;
    }
  }

  #revoke(urls: string[]): void {
    for (const url of urls) {
      this.#objectUrls.revokeObjectURL(url);
    }
  }
}
