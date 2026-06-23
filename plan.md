# Casio AI Machine Reliability Re-architecture Plan

Date: 2026-06-24
Scope: planning only. This file does not implement firmware or server changes yet.

## 1. Why We Are Re-planning

The previous firmware/server design was improved many times during camera, upload, AI solve, OLED bitmap, and polling tests. Many server-side fixes were added because the physical machine was sealed and firmware could not be changed. Those fixes helped, but they did not remove the root architecture risks.

Now firmware can be changed again, so the goal is to rebuild the Casio AI Machine flow so the device can:

- Keep studying existing answers while new questions are uploading or solving.
- Capture several questions ahead without corrupting earlier answers.
- Recover after WiFi/TLS/API failures without requiring a reboot.
- Recover after ESP reboot by pulling current session state from the server.
- Avoid losing answers because ESP RAM/PSRAM was the only source of truth.
- Keep ESP memory bounded while still allowing a useful study session.
- Reliably show Chinese/math OLED bitmap answers without fallback乱码.

## 2. What We Learned From Previous Tests

### Camera

- The camera must not stay live because OV5640 modules heat quickly.
- The stable pattern is one-shot capture: init camera, warm up a few frames, capture, copy JPEG if needed, return fb, deinit camera, quiet camera pins.
- Upload should not happen inside capture. Capture success should mean the JPEG exists locally; upload is a separate network job.
- Photos must be freed immediately after the server confirms upload.

### Upload / HTTPS

- ESP32 TLS/HTTP is fragile under long or repeated requests.
- `HTTPClient` chunk upload failed sometimes even when Vercel had already received the photo.
- Manual HTTP parsing improved this, but the device still should not run many TLS requests concurrently.
- Uploads must be idempotent so retries do not create duplicate/corrupt question state.

### AI Solve

- Long synchronous `/solve` requests are unsafe for ESP. Even with 300s timeout, the connection can break while the server eventually succeeds.
- The better flow is: start solve quickly, return `PROCESSING`, then poll a stored result endpoint.
- Server answer/render jobs must be stored and recoverable by question ID.

### OLED / Chinese / Math Display

- Device fonts do not support Chinese well. Displaying server text directly causes乱码.
- Server-rendered `display_blocks` as 1-bit XBM is the correct display path.
- Device must validate bitmap blocks before drawing: type, kind, width, height, format, decoded byte length.
- For large answers, device should fetch blocks one by one rather than requiring the whole answer in one JSON body.
- Device must clear buffers before each draw and avoid reusing stale bitmap state after cache eviction or block refetch.

### Main System Stability

Observed failures strongly suggest architecture-level state problems:

- Q1 is uploading/thinking, user captures Q2, then Q1 becomes error or Q2 disappears.
- Two questions in flight can affect each other.
- Answers sometimes become corrupted/乱码 after initially displaying correctly.
- ESP often needs reboot to recover.
- Reboot loses local history because history is kept in RAM.
- The server still identifies questions by `deviceId + questionNo`, while ESP resets `nextQuestionId` to 1 on reboot.

## 3. Current Architecture Problems

### 3.1 ESP Is Acting As Source Of Truth

Current firmware stores these critical objects locally:

- `history` answers and display blocks.
- `draftQuestions` photos waiting to upload.
- `nextQuestionId` local question number.
- active solve jobs and UI state.

This makes answers volatile. Any crash/reboot/memory corruption loses the session view.

### 3.2 Question Identity Is Not Durable

Current server schema uses:

- `CasioAiQuestion @@unique([deviceId, questionNo])`
- `CasioAiPhoto @@unique([deviceId, questionNo, photoIndex])`

Current ESP firmware uses local `nextQuestionId = 1`, then increments in RAM. After reboot, Q1/Q2 can collide with previous Q1/Q2 on the server. Server-side `jobToken` guards reduce stale overwrites, but the identity model remains fragile.

### 3.3 Too Much Network Concurrency On ESP

Current firmware allows multiple `solveTask` tasks, each doing:

- sequential photo uploads,
- solve request,
- result polling/block fetch,
- shared `history` updates.

On an ESP32-S3, multiple simultaneous TLS jobs can cause memory pressure, WiFi contention, heap fragmentation, stale UI updates, and hard-to-debug failures.

### 3.4 UI State And Network State Are Coupled

The displayed item in `history` is also the job state. This means a background job can mutate the same structure the UI is reading. The device needs a clearer split:

- Server canonical state.
- Local lightweight list cache.
- Local detail/block cache.
- Network job queue state.
- UI cursor/scroll state.

### 3.5 Server Has Async Pieces But No Session API

Current EBC flow has useful pieces:

- Upload photo.
- Start async solve.
- Poll question.
- Fetch block by index.

But it does not yet provide a true device session/list model. The ESP cannot ask: “what questions are in my current study session, what status are they in, and which one should I display?” without relying on local RAM history.

## 4. Target Architecture

### Core Rule

The server becomes the source of truth. The ESP becomes a bounded cache + input/display terminal.

### Canonical IDs

Use server-created IDs as canonical identifiers:

- `session_id`: one study session, e.g. daily or explicit session.
- `question_uid`: unique UUID/ULID for one question attempt.
- `photo_uid`: unique ID for each uploaded photo.
- `job_id` or `solve_token`: idempotent solve job identity.

The ESP may keep a temporary local draft ID before the server creates the question, but it must not use local `questionNo` as the permanent identity.

### Main State Model

Server owns:

- session list,
- question statuses,
- uploaded photo metadata,
- answer text,
- display block metadata,
- solve/render errors,
- timestamps and retry state.

ESP owns only:

- current `session_id`, persisted in NVS/LittleFS,
- visible list page cache,
- currently selected question ID,
- small detail/block LRU cache,
- pending local draft photos not yet uploaded,
- network job queue.

## 5. Proposed Server Changes

### 5.1 Database

Add or evolve models toward this shape:

```prisma
model CasioAiSession {
  id        String   @id @default(uuid())
  deviceId  String
  title     String?
  status    String   @default("ACTIVE")
  createdAt DateTime @default(now())
  updatedAt DateTime @updatedAt
  questions CasioAiQuestion[]

  @@index([deviceId, updatedAt])
}

model CasioAiQuestion {
  id               String   @id @default(uuid())
  deviceId         String
  sessionId        String?
  session          CasioAiSession? @relation(fields: [sessionId], references: [id])
  displayOrder     Int
  legacyQuestionNo Int?
  status           String   @default("DRAFT")
  mode             String?
  modelHint        String?
  photoCount       Int      @default(0)
  contextTail      String?  @db.Text
  answer           String?  @db.Text
  displayText      String?  @db.Text
  displayBlocks    Json     @default("[]")
  errorMessage     String?  @db.Text
  promptTokens     Int?
  completionTokens Int?
  totalTokens      Int?
  solveToken        String?
  completedAt      DateTime?
  createdAt        DateTime @default(now())
  updatedAt        DateTime @updatedAt
  photos           CasioAiPhoto[]

  @@unique([deviceId, id])
  @@index([deviceId, sessionId, displayOrder])
  @@index([deviceId, status, updatedAt])
}

model CasioAiPhoto {
  id          String   @id @default(uuid())
  questionId  String
  question    CasioAiQuestion @relation(fields: [questionId], references: [id], onDelete: Cascade)
  deviceId    String
  photoIndex  Int
  uploadToken String?
  blobUrl     String   @db.Text
  blobPath    String
  contentType String
  sizeBytes   Int
  sha256      String
  createdAt   DateTime @default(now())
  updatedAt   DateTime @updatedAt

  @@unique([questionId, photoIndex])
  @@unique([deviceId, uploadToken])
  @@index([deviceId, createdAt])
}
```

Important change: uniqueness is based on server `questionId`, not `deviceId + local questionNo`.

### 5.2 New Or Revised APIs

Keep old endpoints temporarily for backward compatibility, but add v2 APIs for the new firmware.

#### Start / Resume Session

`POST /api/casio-ai/v2/session/start`

Request:

```json
{
  "device_id": "CASIO_AI_MACHINE_001",
  "session_id": "optional existing session id",
  "firmware_version": "optional"
}
```

Response:

```json
{
  "ok": true,
  "session_id": "...",
  "active_question_count": 12,
  "server_time": "..."
}
```

#### List Session Questions

`GET /api/casio-ai/v2/session/questions?session_id=...&cursor=...&limit=10`

Returns lightweight rows only:

```json
{
  "ok": true,
  "items": [
    {
      "question_id": "uuid",
      "display_order": 7,
      "status": "SUCCEEDED",
      "photo_count": 2,
      "summary": "二项分布...",
      "updated_at": "..."
    }
  ],
  "next_cursor": "..."
}
```

#### Create Question

`POST /api/casio-ai/v2/questions`

Creates a durable server question before upload.

Response:

```json
{
  "ok": true,
  "question_id": "uuid",
  "display_order": 8,
  "status": "DRAFT"
}
```

#### Upload Photo

`POST /api/casio-ai/v2/questions/:question_id/photos?index=0`

Headers include `X-Upload-Token` for retry idempotency.

Response:

```json
{
  "ok": true,
  "question_id": "uuid",
  "photo_id": "uuid",
  "index": 0,
  "photo_count": 1
}
```

#### Start Solve

`POST /api/casio-ai/v2/questions/:question_id/solve`

This must return quickly, usually 202.

```json
{
  "ok": true,
  "status": "PROCESSING",
  "question_id": "uuid",
  "poll_after_ms": 3000
}
```

If already running or already solved, return the current state idempotently.

#### Poll Question

`GET /api/casio-ai/v2/questions/:question_id`

Returns status and either inline display blocks or block count.

```json
{
  "ok": true,
  "question_id": "uuid",
  "status": "SUCCEEDED",
  "answer": "...",
  "display_text": "...",
  "display_blocks": [],
  "display_block_count": 8,
  "display_blocks_inline": false
}
```

#### Fetch Block

`GET /api/casio-ai/v2/questions/:question_id/blocks/:index`

Same bitmap contract as current endpoint, but keyed by `question_id` UUID.

#### Device Events / Debug Logs

Optional but recommended:

`POST /api/casio-ai/v2/device-events`

Used for ESP logs such as upload retry, memory low, reboot reason, selected question, and render failure. This makes field debugging much easier without needing serial logs every time.

## 6. Proposed Firmware Changes

### 6.1 Replace Local History As Source Of Truth

Current `history` should become a cache, not canonical storage.

New local structures:

```cpp
struct QuestionListItem {
  String questionId;
  int displayOrder;
  String status;
  String summary;
  int photoCount;
  unsigned long updatedAt;
};

struct QuestionDetailCache {
  String questionId;
  String status;
  String answer;
  int blockCount;
  std::vector<DisplayBlock> blocks;
  int blockIndex;
  int xOffset;
  int yOffset;
  unsigned long lastAccessMs;
};

struct DraftQuestion {
  String localDraftId;
  String serverQuestionId;
  std::vector<PendingPhoto> photos;
  String status;
};

struct NetworkJob {
  String jobId;
  String type; // create_question, upload_photo, start_solve, poll_question, fetch_block
  String questionId;
  int photoIndex;
  int attempt;
  unsigned long nextRunMs;
};
```

### 6.2 One Network Worker

Use one background network worker task for HTTPS operations.

Rules:

- Only one TLS request at a time.
- UI never performs network calls directly.
- Button/camera actions enqueue jobs and update local status.
- Worker updates state through a mutex-protected message/state function.
- Retries use exponential backoff and idempotency tokens.
- Polling is scheduled, not blocking.

This is the main device-side fix for “Q1 fails when Q2 starts”.

### 6.3 Capture Flow

User flow:

1. Press CAM from any page -> enter capture/draft page.
2. Press CAM again -> one-shot camera capture.
3. Store JPEG in PSRAM only as pending local draft.
4. Display `[1/3] captured` etc.
5. Press ENTER -> enqueue upload/solve pipeline.
6. Return user to list or last viewed answer immediately.
7. Network worker uploads and solves in background.

Do not upload inside the camera capture function.

### 6.4 Upload/Solve Flow

For each draft:

1. Create server question if no `serverQuestionId` yet.
2. Upload photos one by one with upload tokens.
3. After each successful upload, free that JPEG immediately.
4. Start solve once all photos are uploaded.
5. Mark local list item as `PROCESSING`.
6. Poll question until `SUCCEEDED` or `FAILED`.
7. If succeeded, fetch blocks only when needed.

### 6.5 Polling Strategy

Poll only what matters:

- Selected question if not completed.
- Recently submitted in-flight questions.
- List page statuses every few seconds.

Do not poll every historical question. Do not fetch every display block for every answer.

### 6.6 Cache And Memory Strategy

Suggested ESP limits:

- Local question list cache: 20 lightweight items.
- Detail/block cache: 2 or 3 questions only.
- Pending local drafts: max 3 questions.
- Pending photos per draft: max 3.
- Uploading HTTPS job: 1 at a time.
- In-flight server solves tracked locally: up to 6 metadata items, but only one ESP network operation at a time.

Eviction:

- When detail cache is full, evict least-recently-used completed answer blocks.
- Never evict pending photo bytes before upload succeeds unless user cancels draft.
- Once photo upload succeeds, always free JPEG bytes.
- Keep only `session_id`, selected question ID, and maybe list cursor in NVS/LittleFS. Do not persist large answer blocks locally.

### 6.7 Reboot Recovery

On boot:

1. Connect WiFi.
2. Load last `session_id` from NVS/LittleFS.
3. Call session start/resume.
4. Fetch first page of current session list.
5. Resume polling unfinished questions.
6. If a local draft existed but was not uploaded, show “local draft lost” or, if persisted photo storage is added later, resume upload.

### 6.8 Display Strategy

- For Chinese/math answers, render only server bitmap blocks.
- Do not show raw `display_text` with ASCII font except for English status lines.
- Validate bitmap block byte size before drawing.
- Clear OLED buffer before every block draw.
- If a block fails validation, mark that block stale and enqueue refetch.
- For large answers, fetch only current/nearby block, not all blocks at once.
- Keep scroll state per question detail cache, not in global history only.

### 6.9 Error Handling

Errors should become recoverable states, not dead-end screens.

Recommended statuses:

- `LOCAL_DRAFT`
- `CREATING`
- `UPLOADING`
- `UPLOADED`
- `PROCESSING`
- `ANSWER_READY`
- `RENDERING`
- `SUCCEEDED`
- `FAILED_RETRYABLE`
- `FAILED_FINAL`

Device UI should show retryable failures as list items with status, e.g. `Q3 NET RETRY`, not force the whole app into global `MODE_ERROR`.

## 7. Proposed Server State Machine

```text
DRAFT
  -> UPLOADING
  -> UPLOADED
  -> PROCESSING
  -> ANSWER_READY
  -> RENDERING
  -> SUCCEEDED
```

Failure paths:

```text
UPLOADING/PROCESSING/RENDERING -> FAILED_RETRYABLE or FAILED_FINAL
```

Important behavior:

- Starting solve twice should be idempotent.
- Uploading the same photo with the same upload token should return the same photo result.
- Polling should never trigger duplicate render jobs for the same answer.
- Stale jobs must not overwrite newer question attempts.
- Server should store the full answer and display block metadata before telling device `SUCCEEDED`.

## 8. User Experience Flow After Re-architecture

### Boot

```text
OLED: Casio AI Machine
OLED: Connecting WiFi
-> fetch session list
-> show sidebar/list
```

### Sidebar/List

- Shows two lightweight question rows at a time.
- Rows come from server session list cache.
- UP/DOWN moves selection.
- ENTER opens selected question.
- PAGE toggles list/current answer.
- CAM enters capture draft page.

### Viewing Answer

- Shows server-rendered bitmap blocks.
- UP/DOWN/LEFT/RIGHT scroll blocks.
- ENTER toggles blank style mode.
- PAGE goes back to list.
- CAM enters capture draft page without losing the current answer.

### Capturing Ahead

- User can view Q1 while Q2/Q3 are processing.
- Capturing Q4 does not mutate Q1/Q2 detail state.
- Background network worker serializes upload/solve/poll operations.
- The server keeps Q2/Q3 status even if the ESP reboots.

## 9. Migration Plan

### Phase 1: Server V2 APIs, Backward Compatible

- Add session/question UID schema migration.
- Add v2 session/question/photo/solve/poll/block endpoints.
- Keep old endpoints working for existing firmware.
- Make upload and solve idempotent by server ID/token.
- Add structured device event logs.

### Phase 2: Firmware Network Queue

- Add network worker and job queue.
- Stop spawning multiple solve tasks with direct TLS calls.
- Convert old local `history` into list/detail cache.
- Keep existing camera capture behavior.
- Keep old API fallback only during migration.

### Phase 3: Server-Backed UI

- Boot/resume session from server.
- Fetch list pages from server.
- Fetch detail/block only on demand.
- Cache 2-3 active answer details locally.
- Add retry/recover UI states.

### Phase 4: Remove Fragile Legacy Paths

- Stop using `question_id` integer as canonical identity.
- Stop using `deviceId + questionNo` as primary API lookup.
- Keep `displayOrder` only as user-facing Q number.
- Retire synchronous long `/solve` behavior from firmware.

## 10. Test Matrix Before Declaring Stable

### Single Question

- Capture 1 photo, upload, solve, render, display.
- Capture 3 photos for one question; verify all 3 photo URLs reach AI in order.
- WiFi reconnect during upload; verify retry does not duplicate/corrupt photo rows.

### Multiple Questions

- Q1 processing while capturing Q2.
- Q1 processing while Q2 uploads.
- Q1 and Q2 processing while user views an old completed Q0.
- Submit Q1, Q2, Q3 quickly; verify each keeps independent status and answer.
- Q2 fails render; Q1/Q3 remain viewable.

### Reboot Recovery

- Reboot during Q1 processing; device fetches session and resumes polling.
- Reboot after answer stored but before blocks fetched; device can fetch blocks later.
- Reboot after upload but before solve; server state remains visible and recoverable.

### Display

- Chinese answer.
- Mixed English/math/Chinese answer.
- Long multi-block answer.
- Large block response requiring `/blocks/:index` fetch.
- Corrupt/missing block -> refetch or show block error without crashing UI.

### Memory/Soak

- 20+ questions in one session; ESP cache stays bounded.
- Repeated scroll between answers; no accumulating OLED artifacts.
- Repeated capture/upload cycles; PSRAM returns close to baseline after each upload.
- Serial/event logs include question ID, job ID, retry count, free heap/PSRAM.

## 11. Key Design Decisions To Confirm

1. Session lifetime: daily automatic session, manual new session, or both?
2. Max local detail cache: 2 or 3 answers?
3. Max local drafts: keep 3 pending questions or allow 6 metadata with only one photo-heavy draft active?
4. Should ESP persist unfinished local photo drafts to flash, or accept that only already-uploaded data survives reboot?
5. Should server keep all historical sessions, or only current/recent sessions for device list?
6. Should the device support cancel/retry per question from the UI?

## 12. Recommended Defaults

- Server source of truth: yes.
- Daily/current session by default.
- ESP list cache: 20 rows.
- ESP detail cache: 3 questions.
- ESP pending draft questions: 3.
- Photos per question: 3.
- HTTPS concurrency on ESP: 1.
- Server in-flight solves: many allowed, but device only polls bounded active set.
- Use `question_uid` everywhere in v2 protocol.
- Keep old integer `questionNo` only as display order / legacy compatibility.

## 13. What Not To Do Again

- Do not make ESP RAM `history` the only source of truth.
- Do not use local incremental `question_id` as canonical server identity.
- Do not launch many independent TLS solve tasks on ESP.
- Do not keep JPEG bytes after successful upload.
- Do not rely on one 300s solve HTTP request to deliver the final answer.
- Do not display Chinese answer using U8g2 ASCII font fallback.
- Do not let one question's network failure put the entire app into unrecoverable global error mode.

## 14. First Implementation Checklist

When implementation starts, do this order:

1. Add server v2 DB fields/models and migration plan.
2. Add server v2 session/question/list endpoints.
3. Add server v2 idempotent upload/solve/poll/block endpoints.
4. Add firmware config constants for v2 paths.
5. Add firmware network worker and job queue.
6. Convert capture submit flow to create/upload/solve jobs.
7. Replace local `history` source-of-truth with server-backed list/detail cache.
8. Add block-on-demand fetch and LRU cache.
9. Add reboot resume from `session_id`.
10. Run the full test matrix above.

## 15. Expected Outcome

After this re-architecture, the machine should feel like a reliable study device:

- You can submit a question and keep reading previous answers.
- You can capture future questions while older ones are processing.
- A failed upload or render affects only that question, not the whole device.
- Reboot does not erase server-stored answers.
- ESP memory stays bounded because it caches only what is currently useful.
- Server logs can explain what happened even without serial monitor access.
