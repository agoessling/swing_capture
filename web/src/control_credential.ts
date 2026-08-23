export interface ControlCredentialSource {
  current(): string;
}

export class MutableControlCredential implements ControlCredentialSource {
  #value: string;
  readonly #onReplace: ((value: string) => void) | undefined;

  constructor(value: string, onReplace?: (value: string) => void) {
    this.#value = value;
    this.#onReplace = onReplace;
  }

  current(): string {
    return this.#value;
  }

  replace(value: string): void {
    this.#value = value;
    this.#onReplace?.(value);
  }
}

export type ControlCredential = string | ControlCredentialSource;

export function controlCredentialValue(credential: ControlCredential): string {
  return typeof credential === "string" ? credential : credential.current();
}
