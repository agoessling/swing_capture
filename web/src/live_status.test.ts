import assert from "node:assert/strict";
import {
  LiveStatusCursor,
  parseLiveStatusVersion,
  reconnectDelayMs,
  subscribeToReconnectableStatus,
  type LiveStatusVersion,
  type StatusSubscriptionScheduler,
} from "./live_status.js";

async function main() {
  parsesAndRejectsStaleStatusVersions();
  await reconnectsWithBoundedBackoff();
}

function parsesAndRejectsStaleStatusVersions() {
  const cursor = new LiveStatusCursor();
  assert.equal(cursor.accept(version("stream-one", 4, 400)), true);
  assert.equal(cursor.accept(version("stream-one", 4, 401)), false);
  assert.equal(cursor.accept(version("stream-one", 3, 402)), false);
  assert.equal(cursor.accept(version("stream-one", 5, 399)), false);
  assert.equal(cursor.accept(version("stream-two", 1, 10)), true);
  assert.equal(cursor.accept(undefined), true, "legacy servers remain pollable");
  assert.deepEqual(
    parseLiveStatusVersion(version("stream-one", 6, 500)),
    version("stream-one", 6, 500),
  );
  assert.throws(
    () => parseLiveStatusVersion({ ...version("stream-one", 6, 500), revision: "-1" }),
    /unsigned decimal/,
  );
  assert.equal(reconnectDelayMs(100, 800, 1), 100);
  assert.equal(reconnectDelayMs(100, 800, 2), 200);
  assert.equal(reconnectDelayMs(100, 800, 8), 800);
}

async function reconnectsWithBoundedBackoff() {
  const scheduler = new FakeScheduler();
  const outcomes: Array<LiveStatusVersion | Error> = [
    version("stream-one", 1, 100),
    new Error("phone disconnected"),
    new Error("still disconnected"),
    version("stream-one", 2, 200),
    version("stream-one", 2, 201),
    version("stream-two", 1, 10),
  ];
  let changes = 0;
  const unsubscribe = subscribeToReconnectableStatus(
    () => {
      const outcome = outcomes.shift();
      assert.ok(outcome !== undefined);
      return outcome instanceof Error ? Promise.reject(outcome) : Promise.resolve(outcome);
    },
    () => ++changes,
    { intervalMs: 100, maximumBackoffMs: 800, scheduler },
  );

  assert.equal(scheduler.nextDelay(), 100);
  await scheduler.runNext();
  assert.equal(changes, 1);
  assert.equal(scheduler.nextDelay(), 100);
  await scheduler.runNext();
  assert.equal(changes, 2, "the first disconnect invalidates live UI data");
  assert.equal(scheduler.nextDelay(), 100);
  await scheduler.runNext();
  assert.equal(changes, 2, "repeated disconnects do not spam refreshes");
  assert.equal(scheduler.nextDelay(), 200);
  await scheduler.runNext();
  assert.equal(changes, 3, "the first fresh status reconnects the UI");
  assert.equal(scheduler.nextDelay(), 100);
  await scheduler.runNext();
  assert.equal(changes, 3, "a stale status revision is ignored");
  await scheduler.runNext();
  assert.equal(changes, 4, "a restarted server stream may begin at a lower revision");
  unsubscribe();
  assert.equal(scheduler.pending(), 0);
}

function version(streamId: string, revision: number, generated: number): LiveStatusVersion {
  return {
    schema_version: 1,
    stream_id: streamId,
    revision: String(revision),
    generated_elapsed_realtime_ns: String(generated),
  };
}

class FakeScheduler implements StatusSubscriptionScheduler {
  readonly #scheduled: Array<{ callback: () => void; delayMs: number; cancelled: boolean }> = [];

  setTimeout(callback: () => void, delayMs: number): unknown {
    const task = { callback, delayMs, cancelled: false };
    this.#scheduled.push(task);
    return task;
  }

  clearTimeout(handle: unknown): void {
    (handle as { cancelled: boolean }).cancelled = true;
  }

  nextDelay(): number {
    const task = this.#scheduled.find((candidate) => !candidate.cancelled);
    assert.ok(task);
    return task.delayMs;
  }

  async runNext(): Promise<void> {
    const index = this.#scheduled.findIndex((candidate) => !candidate.cancelled);
    assert.notEqual(index, -1);
    const [task] = this.#scheduled.splice(index, 1);
    assert.ok(task);
    task.callback();
    await Promise.resolve();
    await Promise.resolve();
  }

  pending(): number {
    return this.#scheduled.filter((task) => !task.cancelled).length;
  }
}

void main();
